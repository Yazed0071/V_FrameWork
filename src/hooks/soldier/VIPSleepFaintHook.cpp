#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "VIPSleepFaintHook.h"
#include "VIPRadioHook.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"

namespace
{


    using State_ComradeAction_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);


    using State_RecoveryTouch_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);


    using State_RecoveryKick_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);


    static constexpr std::uint32_t HASH_EVENT_VOICE_NOTICE = 0x1077DB8Du;


    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;


    static constexpr std::uint32_t HASH_SLEEP_WAKE_OFFICER = 0x9CD0A89Cu;


    static State_ComradeAction_t g_OrigState_ComradeAction = nullptr;
    static State_RecoveryTouch_t g_OrigState_RecoveryTouch = nullptr;
    static State_RecoveryKick_t  g_OrigState_RecoveryKick  = nullptr;


    struct ImportantTargetInfo
    {
        bool important = false;
        bool isOfficer = false;
    };


    struct PendingWakeInfo
    {
        std::uint16_t sleeperIndex = 0xFFFFu;
        std::uint16_t sleeperGameObjectId = 0xFFFFu;
        ULONGLONG tickMs = 0;
    };


    static std::unordered_map<std::uint16_t, ImportantTargetInfo> g_ImportantTargetsBySoldierIndex;


    static std::unordered_map<std::uint32_t, PendingWakeInfo> g_PendingWakeByActor;


    static std::mutex g_SleepFaintMutex;
}


static bool SafeReadByte(std::uintptr_t addr, std::uint8_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint8_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool SafeReadWord(std::uintptr_t addr, std::uint16_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint16_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool IsFreshTick(ULONGLONG tickMs, ULONGLONG maxAgeMs = 10000ull)
{
    if (tickMs == 0)
        return false;

    const ULONGLONG now = GetTickCount64();
    return (now - tickMs) <= maxAgeMs;
}


static std::uint16_t NormalizeSoldierIndexFromGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t raw = static_cast<std::uint16_t>(gameObjectId);

    if (raw == 0xFFFFu)
        return 0xFFFFu;

    if ((raw & 0xFE00u) != 0x0400u)
        return 0xFFFFu;

    return static_cast<std::uint16_t>(raw & 0x01FFu);
}


static std::uintptr_t GetNoticeActionEntry(void* self, std::uint32_t actorId)
{
    if (!self)
        return 0;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    __try
    {
        const std::uint32_t baseIndex =
            *reinterpret_cast<const std::uint32_t*>(selfAddr + 0x98ull);

        const std::uint64_t tableBase =
            *reinterpret_cast<const std::uint64_t*>(selfAddr + 0x90ull);

        const std::uint32_t slot = actorId - baseIndex;
        return static_cast<std::uintptr_t>(
            tableBase + (static_cast<std::uint64_t>(slot) * 0x68ull));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}


static std::uint32_t GetEventHash(void* evt)
{
    if (!evt)
        return 0;

    __try
    {
        const auto objectAddr = reinterpret_cast<std::uintptr_t>(evt);
        const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(objectAddr);
        if (!vtbl)
            return 0;

        const auto fnAddr = *reinterpret_cast<const std::uintptr_t*>(vtbl + 0x0ull);
        if (!fnAddr)
            return 0;

        using GetHashFn_t = std::uint32_t(__fastcall*)(void*);
        auto fn = reinterpret_cast<GetHashFn_t>(fnAddr);
        return fn(evt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}


static void DispatchNoticeReaction(void* noticeSelf, std::uint32_t actorId, std::uint32_t reactionHash)
{
    if (!noticeSelf)
        return;

    __try
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(noticeSelf);

        const std::uint64_t soldierActionRoot =
            *reinterpret_cast<const std::uint64_t*>(selfAddr + 0x78ull);
        if (!soldierActionRoot)
            return;

        const std::uint64_t reactionMgr =
            *reinterpret_cast<const std::uint64_t*>(
                static_cast<std::uintptr_t>(soldierActionRoot) + 0xA8ull);
        if (!reactionMgr)
            return;

        const std::uint64_t vtbl =
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(reactionMgr));
        if (!vtbl)
            return;

        const std::uint64_t fnAddr =
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(vtbl) + 0x20ull);
        if (!fnAddr)
            return;

        using DispatchFn_t =
            void(__fastcall*)(void* mgr,
                std::uint32_t actorId,
                std::uint32_t categoryHash,
                int arg4,
                std::uint32_t reactionHash,
                float delaySeconds);

        auto fn = reinterpret_cast<DispatchFn_t>(fnAddr);
        fn(
            reinterpret_cast<void*>(reactionMgr),
            actorId,
            HASH_REACTION_CATEGORY_NOTICE,
            1,
            reactionHash,
            1.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogDebug("[SleepFaint] DispatchNoticeReaction exception\n");
    }
}


static bool TryGetImportantTargetInfo(std::uint16_t soldierIndex, ImportantTargetInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_SleepFaintMutex);

    const auto it = g_ImportantTargetsBySoldierIndex.find(soldierIndex);
    if (it == g_ImportantTargetsBySoldierIndex.end())
        return false;

    outInfo = it->second;
    return outInfo.important;
}


static void SetPendingWake(
    std::uint32_t actorId,
    std::uint16_t sleeperIndex,
    std::uint16_t sleeperGameObjectId)
{
    PendingWakeInfo info{};
    info.sleeperIndex = sleeperIndex;
    info.sleeperGameObjectId = sleeperGameObjectId;
    info.tickMs = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_PendingWakeByActor[actorId] = info;
    }
}


static bool TryGetPendingWake(std::uint32_t actorId, PendingWakeInfo& outInfo)
{
    outInfo = {};

    std::lock_guard<std::mutex> lock(g_SleepFaintMutex);

    const auto it = g_PendingWakeByActor.find(actorId);
    if (it == g_PendingWakeByActor.end())
    {
        return false;
    }

    if (!IsFreshTick(it->second.tickMs))
    {
        g_PendingWakeByActor.erase(it);
        return false;
    }

    outInfo = it->second;

    return true;
}


static void ErasePendingWake(std::uint32_t actorId, const char* reason)
{
    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_PendingWakeByActor.erase(actorId);
    }
}


static bool TryExtractSleepFaintCandidatesFromEntry(
    void* self,
    std::uint32_t actorId,
    std::uint16_t& outSleeperIndexFrom5D,
    std::uint16_t& outSleeperGameObjectIdFrom52,
    std::uint16_t& outSleeperIndexFrom52,
    bool emitRawLog)
{
    outSleeperIndexFrom5D = 0xFFFFu;
    outSleeperGameObjectIdFrom52 = 0xFFFFu;
    outSleeperIndexFrom52 = 0xFFFFu;

    const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
    if (!entry)
    {
        return false;
    }

    std::uint8_t b5D = 0xFFu;
    std::uint16_t w52 = 0xFFFFu;

    SafeReadByte(entry + 0x5Dull, b5D);
    SafeReadWord(entry + 0x52ull, w52);

    if (b5D != 0xFFu)
    {
        outSleeperIndexFrom5D = static_cast<std::uint16_t>(b5D);
    }

    if (w52 != 0xFFFFu)
    {
        outSleeperGameObjectIdFrom52 = w52;
        outSleeperIndexFrom52 = NormalizeSoldierIndexFromGameObjectId(w52);
    }

    return true;
}


static void __fastcall hkState_ComradeAction(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_ComradeAction, self, actorId, proc, evt);

    UNREFERENCED_PARAMETER(evt);

    if (proc == 1)
    {
        std::uint16_t sleeperIndexFrom5D = 0xFFFFu;
        std::uint16_t sleeperGameObjectIdFrom52 = 0xFFFFu;
        std::uint16_t sleeperIndexFrom52 = 0xFFFFu;

        if (TryExtractSleepFaintCandidatesFromEntry(
            self,
            actorId,
            sleeperIndexFrom5D,
            sleeperGameObjectIdFrom52,
            sleeperIndexFrom52,
            true))
        {
            std::uint16_t chosenIndex = 0xFFFFu;
            ImportantTargetInfo info{};
            bool isImportant = false;


            if (sleeperIndexFrom5D != 0xFFFFu && sleeperIndexFrom5D != 0)
            {
                if (TryGetImportantTargetInfo(sleeperIndexFrom5D, info))
                {
                    chosenIndex = sleeperIndexFrom5D;
                    isImportant = true;
                }
            }

            if (!isImportant && sleeperIndexFrom52 != 0xFFFFu)
            {
                if (TryGetImportantTargetInfo(sleeperIndexFrom52, info))
                {
                    chosenIndex = sleeperIndexFrom52;
                    isImportant = true;
                }
            }


            if (!isImportant)
            {
                std::uint16_t fallbackIndex = 0xFFFFu;
                bool fallbackOfficer = false;

                if (Try_GetSingleRecentImportantCorpseIndex(fallbackIndex, fallbackOfficer))
                {
                    chosenIndex = fallbackIndex;
                    isImportant = TryGetImportantTargetInfo(chosenIndex, info);
                }
            }


            if (chosenIndex == 0xFFFFu)
            {
                if (sleeperIndexFrom5D != 0xFFFFu && sleeperIndexFrom5D != 0)
                    chosenIndex = sleeperIndexFrom5D;
                else if (sleeperIndexFrom52 != 0xFFFFu)
                    chosenIndex = sleeperIndexFrom52;
            }

            if (chosenIndex != 0xFFFFu)
            {
                SetPendingWake(actorId, chosenIndex, sleeperGameObjectIdFrom52);
            }
        }
    }

    g_OrigState_ComradeAction(self, actorId, proc, evt);
}


static void __fastcall hkState_RecoveryTouch(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_RecoveryTouch, self, actorId, proc, evt);

    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);

        if (eventHash == HASH_EVENT_VOICE_NOTICE)
        {
            PendingWakeInfo cached{};
            std::uint16_t sleeperIndex = 0xFFFFu;
            std::uint16_t sleeperGameObjectId = 0xFFFFu;

            if (TryGetPendingWake(actorId, cached))
            {
                sleeperIndex = cached.sleeperIndex;
                sleeperGameObjectId = cached.sleeperGameObjectId;
            }
            else
            {
                std::uint16_t sleeperIndexFrom5D = 0xFFFFu;
                std::uint16_t sleeperGameObjectIdFrom52 = 0xFFFFu;
                std::uint16_t sleeperIndexFrom52 = 0xFFFFu;

                if (TryExtractSleepFaintCandidatesFromEntry(
                    self,
                    actorId,
                    sleeperIndexFrom5D,
                    sleeperGameObjectIdFrom52,
                    sleeperIndexFrom52,
                    true))
                {

                    if (sleeperIndexFrom52 != 0xFFFFu)
                    {
                        sleeperIndex = sleeperIndexFrom52;
                        sleeperGameObjectId = sleeperGameObjectIdFrom52;
                    }
                    else if (sleeperIndexFrom5D != 0xFFFFu && sleeperIndexFrom5D != 0)
                    {
                        sleeperIndex = sleeperIndexFrom5D;
                    }
                }
            }

            ImportantTargetInfo info{};
            const bool isImportant = TryGetImportantTargetInfo(sleeperIndex, info);

            if (isImportant)
            {
                ErasePendingWake(actorId, "dispatch");
                DispatchNoticeReaction(self, actorId, HASH_SLEEP_WAKE_OFFICER);
                return;
            }
        }
    }

    g_OrigState_RecoveryTouch(self, actorId, proc, evt);
}


static bool TryInterceptRecoveryWake(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt,
    const char* tag)
{
    if (proc != 6 || evt == nullptr)
        return false;

    const std::uint32_t eventHash = GetEventHash(evt);

    if (eventHash != HASH_EVENT_VOICE_NOTICE)
        return false;

    PendingWakeInfo cached{};
    std::uint16_t sleeperIndex = 0xFFFFu;
    std::uint16_t sleeperGameObjectId = 0xFFFFu;

    if (TryGetPendingWake(actorId, cached))
    {
        sleeperIndex = cached.sleeperIndex;
        sleeperGameObjectId = cached.sleeperGameObjectId;
    }
    else
    {
        std::uint16_t sleeperIndexFrom5D = 0xFFFFu;
        std::uint16_t sleeperGameObjectIdFrom52 = 0xFFFFu;
        std::uint16_t sleeperIndexFrom52 = 0xFFFFu;

        if (TryExtractSleepFaintCandidatesFromEntry(
            self,
            actorId,
            sleeperIndexFrom5D,
            sleeperGameObjectIdFrom52,
            sleeperIndexFrom52,
            true))
        {
            if (sleeperIndexFrom52 != 0xFFFFu)
            {
                sleeperIndex = sleeperIndexFrom52;
                sleeperGameObjectId = sleeperGameObjectIdFrom52;
            }
            else if (sleeperIndexFrom5D != 0xFFFFu && sleeperIndexFrom5D != 0)
            {
                sleeperIndex = sleeperIndexFrom5D;
            }
        }
    }

    ImportantTargetInfo info{};
    const bool isImportant = TryGetImportantTargetInfo(sleeperIndex, info);

    if (!isImportant)
        return false;

    ErasePendingWake(actorId, "dispatch");
    DispatchNoticeReaction(self, actorId, HASH_SLEEP_WAKE_OFFICER);
    return true;
}


static void __fastcall hkState_RecoveryKick(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_RecoveryKick, self, actorId, proc, evt);

    if (TryInterceptRecoveryWake(self, actorId, proc, evt, "KICK"))
        return;

    g_OrigState_RecoveryKick(self, actorId, proc, evt);
}


void Add_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer)
{
    const std::uint16_t soldierIndex = NormalizeSoldierIndexFromGameObjectId(gameObjectId);
    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[SleepFaint] Add ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    ImportantTargetInfo info{};
    info.important = true;
    info.isOfficer = isOfficer;

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex[soldierIndex] = info;
    }
}


void Remove_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex = NormalizeSoldierIndexFromGameObjectId(gameObjectId);
    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[SleepFaint] Remove ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex.erase(soldierIndex);
    }
}


void Clear_VIPSleepFaintImportantGameObjectIds()
{
    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_PendingWakeByActor.clear();
    }
}


bool Install_VIPSleepFaint_Hook()
{
    const bool okComrade = CreateAndEnableHook(
        ResolveGameAddress(gAddr.State_ComradeAction),
        reinterpret_cast<void*>(&hkState_ComradeAction),
        reinterpret_cast<void**>(&g_OrigState_ComradeAction));

    const bool okTouch = CreateAndEnableHook(
        ResolveGameAddress(gAddr.State_RecoveryTouch),
        reinterpret_cast<void*>(&hkState_RecoveryTouch),
        reinterpret_cast<void**>(&g_OrigState_RecoveryTouch));

    const bool okKick = CreateAndEnableHook(
        ResolveGameAddress(gAddr.State_RecoveryKick),
        reinterpret_cast<void*>(&hkState_RecoveryKick),
        reinterpret_cast<void**>(&g_OrigState_RecoveryKick));

#ifdef _DEBUG
    Log("[SleepFaint] Install State_ComradeAction: %s\n", okComrade ? "OK" : "FAIL");
    Log("[SleepFaint] Install State_RecoveryTouch: %s\n", okTouch ? "OK" : "FAIL");
    Log("[SleepFaint] Install State_RecoveryKick:  %s\n", okKick ? "OK" : "FAIL");
#else
    if (!okComrade)
        Log("[SleepFaint] Install State_ComradeAction: %s\n", okComrade ? "OK" : "FAIL");
    if (!okTouch)
        Log("[SleepFaint] Install State_RecoveryTouch: %s\n", okTouch ? "OK" : "FAIL");
    if (!okKick)
        Log("[SleepFaint] Install State_RecoveryKick:  %s\n", okKick ? "OK" : "FAIL");
#endif

    return okComrade && okTouch && okKick;
}


bool Uninstall_VIPSleepFaint_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_ComradeAction));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_RecoveryTouch));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_RecoveryKick));

    g_OrigState_ComradeAction = nullptr;
    g_OrigState_RecoveryTouch = nullptr;
    g_OrigState_RecoveryKick  = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_PendingWakeByActor.clear();
    }

#ifdef _DEBUG
    LogDebug("[SleepFaint] Hooks removed and state cleared\n");
#endif
    return true;
}