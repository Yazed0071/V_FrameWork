#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "VIPHoldupHook.h"

namespace
{
    // Hook type for NoticeActionImpl::State_StandRecoveryHoldup.
    // Params: self, actorId, proc, evt
    using State_StandRecoveryHoldup_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, int proc, void* evt);

    // Absolute address of NoticeActionImpl::State_StandRecoveryHoldup.
    static constexpr std::uintptr_t ABS_State_StandRecoveryHoldup = 0x1414BCA10ull;

    // Event hash for the game's normal voice-notice path.
    static constexpr std::uint32_t HASH_EVENT_VOICE_NOTICE = 0x1077DB8Du;

    // Event hash observed for holdup recovery in your logs.
    static constexpr std::uint32_t HASH_EVENT_HOLDUP_RECOVERY = 0x67926792u;

    // Reaction category used by the game's notice reaction manager.
    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;

    // Custom reaction hash for officer holdup recovery.
    static constexpr std::uint32_t HASH_HOLDUP_RECOVERY_OFFICER = 0x92D098DEu;

    // Re-dispatch cooldown for the same actor/target.
    static constexpr ULONGLONG HOLdup_RECOVERY_DISPATCH_COOLDOWN_MS = 3000ull;

    // Window where the follow-up vanilla voice notice should be swallowed.
    static constexpr ULONGLONG HOLdup_VANILLA_SUPPRESS_MS = 2500ull;

    // Original function pointer.
    static State_StandRecoveryHoldup_t g_OrigState_StandRecoveryHoldup = nullptr;

    // Important target info stored by normalized soldier index.
    struct ImportantTargetInfo
    {
        bool important = false;
        bool isOfficer = false;
    };

    // Recent dispatch/suppression state per actor.
    struct RecentRecoveryDispatch
    {
        std::uint16_t recoveredGameObjectId = 0xFFFFu;
        ULONGLONG lastDispatchTickMs = 0;
        ULONGLONG suppressVanillaUntilMs = 0;
    };

    // Important targets by normalized soldier index.
    static std::unordered_map<std::uint16_t, ImportantTargetInfo> g_ImportantTargetsBySoldierIndex;

    // Recent custom dispatch state by actor id.
    static std::unordered_map<std::uint32_t, RecentRecoveryDispatch> g_RecentDispatchByActor;

    // Mutex protecting all holdup state.
    static std::mutex g_HoldupMutex;
}

// Safely reads one byte from memory.
// Params: addr, outValue
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

// Safely reads one word from memory.
// Params: addr, outValue
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

// Safely reads one dword from memory.
// Params: addr, outValue
static bool SafeReadDword(std::uintptr_t addr, std::uint32_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint32_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Converts a soldier GameObjectId like 0x0408 into soldier index 0x0008.
// Params: gameObjectId
static std::uint16_t NormalizeSoldierIndexFromGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t raw = static_cast<std::uint16_t>(gameObjectId);

    if (raw == 0xFFFFu)
        return 0xFFFFu;

    if ((raw & 0xFE00u) != 0x0400u)
        return 0xFFFFu;

    return static_cast<std::uint16_t>(raw & 0x01FFu);
}

// Resolves one NoticeAction entry using the game's table math.
// Params: self, actorId
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

// Reads the event hash by calling the first virtual function on the event object.
// Params: evt
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

// Dispatches one custom notice reaction through the game's reaction manager.
// Params: noticeSelf, actorId, reactionHash
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
        Log("[Holdup] DispatchNoticeReaction exception\n");
    }
}

// Looks up important-target info by normalized soldier index.
// Params: soldierIndex, outInfo
static bool TryGetImportantTargetInfo(std::uint16_t soldierIndex, ImportantTargetInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_HoldupMutex);

    const auto it = g_ImportantTargetsBySoldierIndex.find(soldierIndex);
    if (it == g_ImportantTargetsBySoldierIndex.end())
        return false;

    outInfo = it->second;
    return outInfo.important;
}

// Resolves the recovered target from the holdup entry.
// Verified from your dump: +0x52..+0x53 held 0x0408.
// Params: self, actorId, outRecoveredGameObjectId, outRecoveredSoldierIndex, outInfo
static bool TryResolveHoldupImportantTargetInfo(
    void* self,
    std::uint32_t actorId,
    std::uint16_t& outRecoveredGameObjectId,
    std::uint16_t& outRecoveredSoldierIndex,
    ImportantTargetInfo& outInfo)
{
    outRecoveredGameObjectId = 0xFFFFu;
    outRecoveredSoldierIndex = 0xFFFFu;
    outInfo = {};

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

    Log("[Holdup] actor=%u entry=%p recoveredGameObjectId=0x%04X recoveredSoldierIndex=0x%04X\n",
        actorId,
        reinterpret_cast<void*>(entry),
        static_cast<unsigned>(recoveredGameObjectId),
        static_cast<unsigned>(recoveredSoldierIndex));

    if (recoveredSoldierIndex == 0xFFFFu)
        return false;

    if (!TryGetImportantTargetInfo(recoveredSoldierIndex, outInfo))
        return false;

    return true;
}

// Returns true only if this actor/target should dispatch the custom holdup voice now.
// Params: actorId, recoveredGameObjectId
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
        if (elapsed <= HOLdup_RECOVERY_DISPATCH_COOLDOWN_MS)
            return false;
    }

    entry.recoveredGameObjectId = recoveredGameObjectId;
    entry.lastDispatchTickMs = now;
    entry.suppressVanillaUntilMs = now + HOLdup_VANILLA_SUPPRESS_MS;
    return true;
}

// Returns true if the follow-up vanilla voice notice should be swallowed.
// Params: actorId, recoveredGameObjectId
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

    if (now <= it->second.suppressVanillaUntilMs)
        return true;

    return false;
}

// Hooks holdup recovery and suppresses the vanilla follow-up line after custom officer dispatch.
// Params: self, actorId, proc, evt
static void __fastcall hkState_StandRecoveryHoldup(
    void* self,
    std::uint32_t actorId,
    int proc,
    void* evt)
{
    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);
        Log("[Holdup] PROC=6 actor=%u eventHash=0x%08X\n", actorId, eventHash);

        // First branch: the actual holdup-recovery event.
        if (eventHash == HASH_EVENT_HOLDUP_RECOVERY)
        {
            std::uint16_t recoveredGameObjectId = 0xFFFFu;
            std::uint16_t recoveredSoldierIndex = 0xFFFFu;
            ImportantTargetInfo info{};

            const bool isImportant =
                TryResolveHoldupImportantTargetInfo(
                    self,
                    actorId,
                    recoveredGameObjectId,
                    recoveredSoldierIndex,
                    info);

            Log("[Holdup] RECOVERY actor=%u recoveredSoldierIndex=%u important=%s officer=%s\n",
                actorId,
                static_cast<unsigned>(recoveredSoldierIndex),
                isImportant ? "YES" : "NO",
                (isImportant && info.isOfficer) ? "YES" : "NO");

            if (isImportant && info.isOfficer)
            {
                if (ShouldDispatchHoldupRecovery(actorId, recoveredGameObjectId))
                {
                    Log("[Holdup] DISPATCH actor=%u recoveredGameObjectId=0x%04X recoveredSoldierIndex=%u officer=YES\n",
                        actorId,
                        static_cast<unsigned>(recoveredGameObjectId),
                        static_cast<unsigned>(recoveredSoldierIndex));

                    DispatchNoticeReaction(self, actorId, HASH_HOLDUP_RECOVERY_OFFICER);
                }

                // Always swallow the original holdup-recovery branch for officer targets.
                return;
            }
        }

        // Second branch: the normal voice-notice event that follows the custom dispatch.
        if (eventHash == HASH_EVENT_VOICE_NOTICE)
        {
            std::uint16_t recoveredGameObjectId = 0xFFFFu;
            std::uint16_t recoveredSoldierIndex = 0xFFFFu;
            ImportantTargetInfo info{};

            const bool isImportant =
                TryResolveHoldupImportantTargetInfo(
                    self,
                    actorId,
                    recoveredGameObjectId,
                    recoveredSoldierIndex,
                    info);

            if (isImportant && info.isOfficer)
            {
                if (ShouldSuppressVanillaVoiceNotice(actorId, recoveredGameObjectId))
                {
                    Log("[Holdup] SUPPRESS vanilla voice notice actor=%u recoveredGameObjectId=0x%04X recoveredSoldierIndex=%u\n",
                        actorId,
                        static_cast<unsigned>(recoveredGameObjectId),
                        static_cast<unsigned>(recoveredSoldierIndex));
                    return;
                }
            }
        }
    }

    g_OrigState_StandRecoveryHoldup(self, actorId, proc, evt);
}

// Adds one important holdup target by original GameObjectId.
// Params: gameObjectId, isOfficer
void Add_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        Log("[Holdup] Add ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    ImportantTargetInfo info{};
    info.important = true;
    info.isOfficer = isOfficer;

    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex[soldierIndex] = info;
    }

    Log("[Holdup] Added important target: gameObjectId=0x%08X soldierIndex=0x%04X officer=%s\n",
        gameObjectId,
        static_cast<unsigned>(soldierIndex),
        isOfficer ? "YES" : "NO");
}

// Removes one important holdup target by original GameObjectId.
// Params: gameObjectId
void Remove_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        Log("[Holdup] Remove ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex.erase(soldierIndex);
        g_RecentDispatchByActor.clear();
    }

    Log("[Holdup] Removed important target: gameObjectId=0x%08X soldierIndex=0x%04X\n",
        gameObjectId,
        static_cast<unsigned>(soldierIndex));
}

// Clears all registered important holdup targets.
// Params: none
void Clear_VIPHoldupImportantGameObjectIds()
{
    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_RecentDispatchByActor.clear();
    }

    Log("[Holdup] Cleared all important targets\n");
}

// Installs the holdup-only hook.
// Params: none
bool Install_VIPHoldup_Hook()
{
    const bool ok = CreateAndEnableHook(
        ResolveGameAddress(ABS_State_StandRecoveryHoldup),
        reinterpret_cast<void*>(&hkState_StandRecoveryHoldup),
        reinterpret_cast<void**>(&g_OrigState_StandRecoveryHoldup));

    Log("[Holdup] Install State_StandRecoveryHoldup: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes the holdup-only hook and clears runtime state.
// Params: none
bool Uninstall_VIPHoldup_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_StandRecoveryHoldup));
    g_OrigState_StandRecoveryHoldup = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_HoldupMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_RecentDispatchByActor.clear();
    }

    Log("[Holdup] Hooks removed and state cleared\n");
    return true;
}