#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "VIPHoldupHook.h"
#include "MissionCodeGuard.h"
#include "PlayerAlertPhase.h"
#include "AddressSet.h"

namespace
{


    using State_StandRecoveryHoldup_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, int proc, void* evt);


    static constexpr std::uint32_t HASH_EVENT_VOICE_NOTICE = 0x1077DB8Du;


    static constexpr std::uint32_t HASH_EVENT_HOLDUP_RECOVERY = 0x67926792u;


    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;


    static constexpr std::uint32_t HASH_HOLDUP_RECOVERY_VIP = 0x92D098DEu;


    static constexpr std::uint32_t HASH_HOLDUP_RECOVERY_NONVIP_CUSTOM = 0x97D0A0FEu;


    static constexpr ULONGLONG HOLDUP_RECOVERY_DISPATCH_COOLDOWN_MS = 6000ull;


    static constexpr ULONGLONG HOLDUP_VANILLA_SUPPRESS_MS = 4000ull;


    static State_StandRecoveryHoldup_t g_OrigState_StandRecoveryHoldup = nullptr;


    struct ImportantTargetInfo
    {
        bool important = false;
        bool isOfficer = false;
    };


    static bool UseCustomNonVipRecoveryNow()
    {
        return PlayerAlertPhase::IsAtOrAbove(PlayerAlertPhase::kCaution);
    }


    struct RecentRecoveryDispatch
    {
        std::uint16_t recoveredGameObjectId = 0xFFFFu;
        ULONGLONG lastDispatchTickMs = 0;
        ULONGLONG suppressVanillaUntilMs = 0;
    };


    struct RecentNonVipRecoverySuppress
    {
        ULONGLONG suppressUntilMs = 0;
    };


    static std::unordered_map<std::uint16_t, ImportantTargetInfo> g_ImportantTargetsBySoldierIndex;


    static std::unordered_map<std::uint32_t, RecentRecoveryDispatch> g_RecentDispatchByActor;


    static std::unordered_set<std::uint32_t> g_CustomNonVipRecoveryPendingTargets;


    static std::unordered_map<std::uint32_t, RecentNonVipRecoverySuppress> g_RecentNonVipSuppressByTarget;


    static std::mutex g_HoldupMutex;
}


static std::uint32_t MakeCustomNonVipRecoveryKey(
    std::uint16_t recoveredGameObjectId,
    std::uint16_t recoveredSoldierIndex)
{
    if (recoveredGameObjectId != 0xFFFFu && recoveredGameObjectId != 0u)
        return static_cast<std::uint32_t>(recoveredGameObjectId);

    if (recoveredSoldierIndex != 0xFFFFu && recoveredSoldierIndex != 0u)
        return 0x80000000u | static_cast<std::uint32_t>(recoveredSoldierIndex);

    return 0u;
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
        LogDebug("[Holdup] DispatchNoticeReaction exception\n");
    }
}


static bool TryGetImportantTargetInfo(std::uint16_t soldierIndex, ImportantTargetInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_HoldupMutex);

    const auto it = g_ImportantTargetsBySoldierIndex.find(soldierIndex);
    if (it == g_ImportantTargetsBySoldierIndex.end())
        return false;

    outInfo = it->second;
    return outInfo.important;
}


static bool TryResolveHoldupRecoveredTargetIds(
    void* self,
    std::uint32_t actorId,
    std::uint16_t& outRecoveredGameObjectId,
    std::uint16_t& outRecoveredSoldierIndex)
{
    outRecoveredGameObjectId = 0xFFFFu;
    outRecoveredSoldierIndex = 0xFFFFu;

    const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
    if (!entry)
        return false;

    std::uint16_t recoveredGameObjectId = 0xFFFFu;
    if (!SafeReadWord(entry + 0x52ull, recoveredGameObjectId))
        return false;

    const std::uint16_t recoveredSoldierIndex =
        NormalizeSoldierIndexFromGameObjectId(recoveredGameObjectId);

    outRecoveredGameObjectId = recoveredGameObjectId;
    outRecoveredSoldierIndex = recoveredSoldierIndex;

    return recoveredSoldierIndex != 0xFFFFu;
}


static bool ShouldDispatchHoldupRecovery(
    std::uint32_t actorId,
    std::uint16_t recoveredGameObjectId)
{
    if (recoveredGameObjectId == 0xFFFFu)
        return false;

    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_HoldupMutex);

    auto& entry = g_RecentDispatchByActor[actorId];

    if (entry.recoveredGameObjectId == recoveredGameObjectId)
    {
        const ULONGLONG elapsed = now - entry.lastDispatchTickMs;
        if (elapsed <= HOLDUP_RECOVERY_DISPATCH_COOLDOWN_MS)
            return false;
    }

    entry.recoveredGameObjectId = recoveredGameObjectId;
    entry.lastDispatchTickMs = now;
    entry.suppressVanillaUntilMs = now + HOLDUP_VANILLA_SUPPRESS_MS;
    return true;
}


static bool ShouldSuppressVanillaVoiceNotice(
    std::uint32_t actorId,
    std::uint16_t recoveredGameObjectId)
{
    if (recoveredGameObjectId == 0xFFFFu)
        return false;

    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_HoldupMutex);

    const auto it = g_RecentDispatchByActor.find(actorId);
    if (it == g_RecentDispatchByActor.end())
        return false;

    if (it->second.recoveredGameObjectId != recoveredGameObjectId)
        return false;

    return now <= it->second.suppressVanillaUntilMs;
}


static bool ShouldDispatchCustomNonVipRecovery(
    std::uint16_t recoveredGameObjectId,
    std::uint16_t recoveredSoldierIndex)
{
    const std::uint32_t key =
        MakeCustomNonVipRecoveryKey(recoveredGameObjectId, recoveredSoldierIndex);

    if (key == 0u)
        return false;

    std::lock_guard<std::mutex> lock(g_HoldupMutex);
    return g_CustomNonVipRecoveryPendingTargets.insert(key).second;
}


static void BeginCustomNonVipRecoverySuppressWindow(
    std::uint16_t recoveredGameObjectId,
    std::uint16_t recoveredSoldierIndex)
{
    const std::uint32_t key =
        MakeCustomNonVipRecoveryKey(recoveredGameObjectId, recoveredSoldierIndex);

    if (key == 0u)
        return;

    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_HoldupMutex);
    g_RecentNonVipSuppressByTarget[key].suppressUntilMs = now + HOLDUP_VANILLA_SUPPRESS_MS;
}


static bool ShouldSuppressCustomNonVipRecoveryNow(
    std::uint16_t recoveredGameObjectId,
    std::uint16_t recoveredSoldierIndex)
{
    const std::uint32_t key =
        MakeCustomNonVipRecoveryKey(recoveredGameObjectId, recoveredSoldierIndex);

    if (key == 0u)
        return false;

    const ULONGLONG now = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_HoldupMutex);

    const auto it = g_RecentNonVipSuppressByTarget.find(key);
    if (it == g_RecentNonVipSuppressByTarget.end())
        return false;

    return now <= it->second.suppressUntilMs;
}


static void ClearAllCustomNonVipRecovery()
{
    std::lock_guard<std::mutex> lock(g_HoldupMutex);
    g_CustomNonVipRecoveryPendingTargets.clear();
}


static void ClearAllCustomNonVipRecoverySuppressWindows()
{
    std::lock_guard<std::mutex> lock(g_HoldupMutex);
    g_RecentNonVipSuppressByTarget.clear();
}


static void __fastcall hkState_StandRecoveryHoldup(
    void* self,
    std::uint32_t actorId,
    int proc,
    void* evt)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_StandRecoveryHoldup, self, actorId, proc, evt);

    if (proc == 6 && evt)
    {
        const bool useCustomNonVipRecovery = UseCustomNonVipRecoveryNow();

        const std::uint32_t eventHash = GetEventHash(evt);

        std::uint16_t recoveredGameObjectId = 0xFFFFu;
        std::uint16_t recoveredSoldierIndex = 0xFFFFu;
        const bool resolvedTarget =
            TryResolveHoldupRecoveredTargetIds(
                self,
                actorId,
                recoveredGameObjectId,
                recoveredSoldierIndex);

        ImportantTargetInfo info{};
        const bool isImportant =
            resolvedTarget &&
            TryGetImportantTargetInfo(recoveredSoldierIndex, info);


        if (useCustomNonVipRecovery &&
            resolvedTarget &&
            !isImportant &&
            (eventHash == HASH_EVENT_HOLDUP_RECOVERY || eventHash == HASH_EVENT_VOICE_NOTICE) &&
            ShouldSuppressCustomNonVipRecoveryNow(recoveredGameObjectId, recoveredSoldierIndex))
        {
            return;
        }

        if (eventHash == HASH_EVENT_HOLDUP_RECOVERY)
        {
            if (isImportant)
            {
                if (ShouldDispatchHoldupRecovery(actorId, recoveredGameObjectId))
                {
                    DispatchNoticeReaction(self, actorId, HASH_HOLDUP_RECOVERY_VIP);
                }


                return;
            }


            if (useCustomNonVipRecovery && resolvedTarget)
            {
                if (ShouldDispatchCustomNonVipRecovery(recoveredGameObjectId, recoveredSoldierIndex))
                {
                    DispatchNoticeReaction(self, actorId, HASH_HOLDUP_RECOVERY_NONVIP_CUSTOM);
                    BeginCustomNonVipRecoverySuppressWindow(recoveredGameObjectId, recoveredSoldierIndex);
                }
                else
                {
                    BeginCustomNonVipRecoverySuppressWindow(recoveredGameObjectId, recoveredSoldierIndex);
                }


                return;
            }
        }


        if (eventHash == HASH_EVENT_VOICE_NOTICE)
        {
            if (isImportant)
            {
                if (ShouldSuppressVanillaVoiceNotice(actorId, recoveredGameObjectId))
                {
                    return;
                }
            }

            if (useCustomNonVipRecovery &&
                resolvedTarget &&
                !isImportant &&
                ShouldSuppressCustomNonVipRecoveryNow(recoveredGameObjectId, recoveredSoldierIndex))
            {
                return;
            }
        }
    }

    g_OrigState_StandRecoveryHoldup(self, actorId, proc, evt);
}


void Add_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[Holdup] Add ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    ImportantTargetInfo info{};
    info.important = true;
    info.isOfficer = isOfficer;

    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex[soldierIndex] = info;
    }
}


void Remove_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[Holdup] Remove ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex.erase(soldierIndex);
        g_RecentDispatchByActor.clear();
        g_CustomNonVipRecoveryPendingTargets.clear();
        g_RecentNonVipSuppressByTarget.clear();
    }
}


bool IsVIPHoldupImportantSoldierIndex(std::uint16_t soldierIndex, bool* outIsOfficer)
{
    ImportantTargetInfo info{};
    if (!TryGetImportantTargetInfo(soldierIndex, info))
        return false;

    if (outIsOfficer)
        *outIsOfficer = info.isOfficer;

    return true;
}


void Clear_VIPHoldupImportantGameObjectIds()
{
    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_RecentDispatchByActor.clear();
        g_CustomNonVipRecoveryPendingTargets.clear();
        g_RecentNonVipSuppressByTarget.clear();
    }
}


void Reset_CustomNonVipRecoveryTracking()
{
    ClearAllCustomNonVipRecovery();
    ClearAllCustomNonVipRecoverySuppressWindows();
}


bool Install_VIPHoldup_Hook()
{
    const bool ok = CreateAndEnableHook(
        ResolveGameAddress(gAddr.State_StandRecoveryHoldup),
        reinterpret_cast<void*>(&hkState_StandRecoveryHoldup),
        reinterpret_cast<void**>(&g_OrigState_StandRecoveryHoldup));

#ifdef _DEBUG
    Log("[Holdup] Install State_StandRecoveryHoldup: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[Holdup] Install State_StandRecoveryHoldup: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_VIPHoldup_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_StandRecoveryHoldup));
    g_OrigState_StandRecoveryHoldup = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_RecentDispatchByActor.clear();
        g_CustomNonVipRecoveryPendingTargets.clear();
        g_RecentNonVipSuppressByTarget.clear();
    }

#ifdef _DEBUG
    LogDebug("[Holdup] Hooks removed and state cleared\n");
#endif
    return true;
}