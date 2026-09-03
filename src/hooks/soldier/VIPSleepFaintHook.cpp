#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "VIPSleepFaintHook.h"
#include "VIPHoldupHook.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"

namespace
{


    using State_RecoveryTouch_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);


    using State_RecoveryKick_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);


    static constexpr std::uint32_t HASH_EVENT_VOICE_NOTICE = 0x1077DB8Du;


    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;


    static constexpr std::uint32_t HASH_SLEEP_WAKE_OFFICER = 0x9CD0A89Cu;


    static State_RecoveryTouch_t g_OrigState_RecoveryTouch = nullptr;
    static State_RecoveryKick_t  g_OrigState_RecoveryKick  = nullptr;


    struct ImportantTargetInfo
    {
        bool important = false;
        bool isOfficer = false;
    };


    static std::unordered_map<std::uint16_t, ImportantTargetInfo> g_ImportantTargetsBySoldierIndex;


    static std::mutex g_SleepFaintMutex;


    static std::atomic<unsigned> g_UnresolvedTargetLogs{0};
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


static bool TryResolveDownedSoldierIndex(
    void* self,
    std::uint32_t actorId,
    std::uint16_t& outSoldierIndex)
{
    outSoldierIndex = 0xFFFFu;

    const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
    if (!entry)
        return false;

    std::uint16_t downedGameObjectId = 0xFFFFu;
    if (!SafeReadWord(entry + 0x52ull, downedGameObjectId))
        return false;

    const std::uint16_t index = NormalizeSoldierIndexFromGameObjectId(downedGameObjectId);
    if (index == 0xFFFFu)
        return false;

    outSoldierIndex = index;
    return true;
}


static bool IsRegisteredImportantSoldier(std::uint16_t soldierIndex)
{
    if (soldierIndex == 0xFFFFu)
        return false;

    ImportantTargetInfo info{};
    if (TryGetImportantTargetInfo(soldierIndex, info))
        return true;

    bool isOfficer = false;
    return IsVIPHoldupImportantSoldierIndex(soldierIndex, &isOfficer);
}


static bool TryInterceptRecoveryWake(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    if (proc != 6 || evt == nullptr)
        return false;

    if (GetEventHash(evt) != HASH_EVENT_VOICE_NOTICE)
        return false;

    std::uint16_t sleeperIndex = 0xFFFFu;
    if (!TryResolveDownedSoldierIndex(self, actorId, sleeperIndex))
    {
        if (g_UnresolvedTargetLogs.fetch_add(1u) < 8u)
            Log("[SleepFaint] actor=%u: the downed soldier could not be identified from the notice "
                "record, so no important-comrade line is substituted and the vanilla line plays\n",
                actorId);

        return false;
    }

    if (!IsRegisteredImportantSoldier(sleeperIndex))
        return false;

    DispatchNoticeReaction(self, actorId, HASH_SLEEP_WAKE_OFFICER);
    return true;
}


static void __fastcall hkState_RecoveryTouch(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_RecoveryTouch, self, actorId, proc, evt);

    if (TryInterceptRecoveryWake(self, actorId, proc, evt))
        return;

    g_OrigState_RecoveryTouch(self, actorId, proc, evt);
}


static void __fastcall hkState_RecoveryKick(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_RecoveryKick, self, actorId, proc, evt);

    if (TryInterceptRecoveryWake(self, actorId, proc, evt))
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
    }
}


bool IsVIPSleepFaintImportantSoldierIndex(std::uint16_t soldierIndex, bool* outIsOfficer)
{
    ImportantTargetInfo info{};
    if (!TryGetImportantTargetInfo(soldierIndex, info))
        return false;

    if (outIsOfficer)
        *outIsOfficer = info.isOfficer;

    return true;
}


bool Install_VIPSleepFaint_Hook()
{
    const bool okTouch = CreateAndEnableHook(
        ResolveGameAddress(gAddr.State_RecoveryTouch),
        reinterpret_cast<void*>(&hkState_RecoveryTouch),
        reinterpret_cast<void**>(&g_OrigState_RecoveryTouch));

    const bool okKick = CreateAndEnableHook(
        ResolveGameAddress(gAddr.State_RecoveryKick),
        reinterpret_cast<void*>(&hkState_RecoveryKick),
        reinterpret_cast<void**>(&g_OrigState_RecoveryKick));

#ifdef _DEBUG
    Log("[SleepFaint] Install State_RecoveryTouch: %s\n", okTouch ? "OK" : "FAIL");
    Log("[SleepFaint] Install State_RecoveryKick:  %s\n", okKick ? "OK" : "FAIL");
#else
    if (!okTouch)
        Log("[SleepFaint] Install State_RecoveryTouch: %s\n", okTouch ? "OK" : "FAIL");
    if (!okKick)
        Log("[SleepFaint] Install State_RecoveryKick:  %s\n", okKick ? "OK" : "FAIL");
#endif

    return okTouch && okKick;
}


bool Uninstall_VIPSleepFaint_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_RecoveryTouch));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_RecoveryKick));

    g_OrigState_RecoveryTouch = nullptr;
    g_OrigState_RecoveryKick  = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex.clear();
    }

#ifdef _DEBUG
    LogDebug("[SleepFaint] Hooks removed and state cleared\n");
#endif
    return true;
}