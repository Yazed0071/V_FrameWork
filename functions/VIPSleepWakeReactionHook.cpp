#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "VIPSleepWakeReactionHook.h"

namespace
{
    // Hook type for NoticeActionImpl::State_ComradeAction.
    // Params: self, actorId, proc, evt
    using State_ComradeAction_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);

    // Hook type for NoticeActionImpl::State_StandToSquatRecoverySleepFaintComradeByTouch.
    // Params: self, actorId, proc, evt
    using State_StandToSquatRecoverySleepFaintComradeByTouch_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);

    // Hook type for NoticeActionImpl::State_StandRecoveryHoldup.
    // Params: self, actorId, proc, evt
    using State_StandRecoveryHoldup_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, int proc, void* evt);

    // Hook type for ActionControllerImpl::StateRadio.
    // Params: self, slot, proc
    using StateRadio_t =
        void(__fastcall*)(void* self, std::uint32_t slot, int proc);

    // Hook type for CpRadioService::ConvertRadioTypeToSpeechLabel.
    // Params: radioType
    using ConvertRadioTypeToSpeechLabel_t =
        std::uint32_t(__fastcall*)(std::uint8_t radioType);

    // Hook type for CorpseManagerImpl::RequestCorpse.
    // Params: self, animControl, ragdollPlugin, facialPlugin, param4, location, originalTargetGameObjectId, inheritanceInfo, isPlayerSide
    using RequestCorpse_t =
        void(__fastcall*)(
            void* self,
            void* animControl,
            void* ragdollPlugin,
            void* facialPlugin,
            std::uint32_t* param4,
            std::uint32_t* location,
            std::uint16_t originalTargetGameObjectId,
            void* inheritanceInfo,
            char isPlayerSide);

    // Hook type for DownActionImpl::State_DownIdle.
    // Params: self, actorId, proc, evt
    using State_DownIdle_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, int proc, void* evt);

    // Absolute address of NoticeActionImpl::State_ComradeAction.
    static constexpr std::uintptr_t ABS_State_ComradeAction = 0x1414B8D20ull;

    // Absolute address of NoticeActionImpl::State_StandToSquatRecoverySleepFaintComradeByTouch.
    static constexpr std::uintptr_t ABS_State_StandToSquatRecoverySleepFaintComradeByTouch = 0x1414BCEF0ull;

    // Absolute address of NoticeActionImpl::State_StandRecoveryHoldup.
    static constexpr std::uintptr_t ABS_State_StandRecoveryHoldup = 0x1414BCA10ull;

    // Absolute address of ActionControllerImpl::StateRadio.
    static constexpr std::uintptr_t ABS_StateRadio = 0x140D69140ull;

    // Absolute address of CpRadioService::ConvertRadioTypeToSpeechLabel.
    static constexpr std::uintptr_t ABS_ConvertRadioTypeToSpeechLabel = 0x140D685C0ull;

    // Absolute address of CorpseManagerImpl::RequestCorpse.
    static constexpr std::uintptr_t ABS_RequestCorpse = 0x140A69070ull;

    // Absolute address of DownActionImpl::State_DownIdle.
    static constexpr std::uintptr_t ABS_State_DownIdle = 0x14146C540ull;

    // Event hash used by the game for notice voice dispatch.
    static constexpr std::uint32_t HASH_EVENT_VOICE_NOTICE = 0x1077DB8Du;

    // Reaction category hash used by the game's notice reaction manager.
    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;

    // Custom wake/faint reaction hash for important VIPs.
    static constexpr std::uint32_t HASH_SLEEP_WAKE_VIP = 0x9CD0A89Cu;

    // Custom wake/faint reaction hash for important Russian VIPs.
    static constexpr std::uint32_t HASH_SLEEP_WAKE_VIP_RUS = 0x9CD0A89Fu;

    // Custom holdup recovery reaction hash for important VIPs.
    static constexpr std::uint32_t HASH_HOLDUP_RECOVERY_VIP = 0x92D098DEu;

    // Custom holdup recovery reaction hash for important Russian VIPs.
    static constexpr std::uint32_t HASH_HOLDUP_RECOVERY_VIP_RUS = 0x92D098DDu;

    // Vanilla CPR0040 label used for normal comrade body found.
    static constexpr std::uint32_t HASH_CP_BODY_FOUND_NORMAL = 0xCFA83D85u;

    // CPR0042 label used for VIP body found.
    static constexpr std::uint32_t HASH_CP_BODY_FOUND_VIP = 0xB357F224u;

    // Custom officer override label.
    static constexpr std::uint32_t HASH_CP_BODY_FOUND_OFFICER = 0xDD6EA61Bu;

    // Original NoticeActionImpl::State_ComradeAction.
    static State_ComradeAction_t g_OrigState_ComradeAction = nullptr;

    // Original NoticeActionImpl::State_StandToSquatRecoverySleepFaintComradeByTouch.
    static State_StandToSquatRecoverySleepFaintComradeByTouch_t g_OrigState_RecoveryTouch = nullptr;

    // Original NoticeActionImpl::State_StandRecoveryHoldup.
    static State_StandRecoveryHoldup_t g_OrigState_StandRecoveryHoldup = nullptr;

    // Original ActionControllerImpl::StateRadio.
    static StateRadio_t g_OrigStateRadio = nullptr;

    // Original CpRadioService::ConvertRadioTypeToSpeechLabel.
    static ConvertRadioTypeToSpeechLabel_t g_OrigConvertRadioTypeToSpeechLabel = nullptr;

    // Original CorpseManagerImpl::RequestCorpse.
    static RequestCorpse_t g_OrigRequestCorpse = nullptr;

    // Original DownActionImpl::State_DownIdle.
    static State_DownIdle_t g_OrigState_DownIdle = nullptr;

    // Describes one important target.
    struct ImportantTargetInfo
    {
        bool important = false;
        bool isRussian = false;
        bool isOfficer = false;
    };

    // Caches corpse-slot -> original important target relation.
    struct CorpseCacheEntry
    {
        std::uint16_t originalTargetGameObjectId = 0xFFFFu;
        ImportantTargetInfo info{};
    };

    // Caches actor -> sleeper target for wake/faint recovery.
    struct PendingComradeWake
    {
        std::uint16_t sleeperIndex = 0xFFFFu;
    };

    // Caches caller -> target relation for upcoming body-found CP radio.
    struct PendingBodyReport
    {
        std::uint16_t targetSoldierIndex = 0xFFFFu;
        ImportantTargetInfo info{};
        ULONGLONG tickMs = 0;
    };

    // Thread-local context for the current radio conversion.
    struct RadioContext
    {
        bool active = false;
        std::uint32_t slot = 0;
        std::uint32_t reporterGameObjectId = 0;
        std::uint16_t callerIndex = 0xFFFFu;
        std::uint16_t targetSoldierIndex = 0xFFFFu;
        std::uint8_t radioType = 0;
        ImportantTargetInfo info{};
        bool fromPendingBodyReport = false;
        bool fromCorpseCache = false;
        bool consumeCorpseCacheAfterReturn = false;
    };

    // Important targets stored by normalized soldier index.
    static std::unordered_map<std::uint16_t, ImportantTargetInfo> g_ImportantTargetsBySoldierIndex;

    // Corpse cache stored by corpse index.
    static std::unordered_map<std::uint16_t, CorpseCacheEntry> g_CorpseCacheByCorpseIndex;

    // Pending wake target stored by actor id.
    static std::unordered_map<std::uint32_t, PendingComradeWake> g_PendingComradeWakeByActor;

    // Pending body report stored by caller index.
    static std::unordered_map<std::uint16_t, PendingBodyReport> g_PendingBodyReportByCaller;

    // Mutex protecting all runtime VIP state.
    static std::mutex g_VipWakeMutex;

    // Thread-local radio context.
    thread_local RadioContext g_RadioContext;
}

// Safely reads a qword from memory.
// Params: addr, outValue
static bool SafeReadQword(std::uintptr_t addr, std::uint64_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint64_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Safely reads a dword from memory.
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

// Safely reads a word from memory.
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

// Safely reads a byte from memory.
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

// Looks up important-target info using a normalized soldier index.
// Params: soldierIndex, outInfo
static bool TryGetImportantTargetInfoBySoldierIndex(std::uint16_t soldierIndex, ImportantTargetInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_VipWakeMutex);

    const auto it = g_ImportantTargetsBySoldierIndex.find(soldierIndex);
    if (it == g_ImportantTargetsBySoldierIndex.end())
        return false;

    outInfo = it->second;
    return outInfo.important;
}

// Resolves important-target info from a subject id.
// It first checks corpse cache, then direct soldier-index lookup, then GameObjectId normalization.
// Params: subjectId, outInfo, fromCorpseCache
static bool TryResolveImportantTargetInfoFromSubject(
    std::uint16_t subjectId,
    ImportantTargetInfo& outInfo,
    bool& fromCorpseCache)
{
    fromCorpseCache = false;

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);

        const auto corpseIt = g_CorpseCacheByCorpseIndex.find(subjectId);
        if (corpseIt != g_CorpseCacheByCorpseIndex.end())
        {
            outInfo = corpseIt->second.info;
            fromCorpseCache = true;
            return outInfo.important;
        }
    }

    if (TryGetImportantTargetInfoBySoldierIndex(subjectId, outInfo))
        return true;

    const std::uint16_t normalized = NormalizeSoldierIndexFromGameObjectId(subjectId);
    if (normalized != 0xFFFFu && TryGetImportantTargetInfoBySoldierIndex(normalized, outInfo))
        return true;

    return false;
}

// Removes one corpse-cache entry after the radio uses it.
// Params: corpseIndex
static void EraseCorpseCacheEntry(std::uint16_t corpseIndex)
{
    std::lock_guard<std::mutex> lock(g_VipWakeMutex);
    g_CorpseCacheByCorpseIndex.erase(corpseIndex);
}

// Stores one pending wake target for an actor.
// Params: actorId, sleeperIndex
static void SetPendingComradeWake(std::uint32_t actorId, std::uint16_t sleeperIndex)
{
    std::lock_guard<std::mutex> lock(g_VipWakeMutex);
    g_PendingComradeWakeByActor[actorId].sleeperIndex = sleeperIndex;
}

// Consumes one pending wake target for an actor.
// Params: actorId
static std::uint16_t TakePendingComradeWake(std::uint32_t actorId)
{
    std::lock_guard<std::mutex> lock(g_VipWakeMutex);

    const auto it = g_PendingComradeWakeByActor.find(actorId);
    if (it == g_PendingComradeWakeByActor.end())
        return 0xFFFFu;

    const std::uint16_t sleeperIndex = it->second.sleeperIndex;
    g_PendingComradeWakeByActor.erase(it);
    return sleeperIndex;
}

// Stores one pending body report for the upcoming CP body-found radio.
// Params: callerIndex, targetSoldierIndex, info
static void SetPendingBodyReport(
    std::uint16_t callerIndex,
    std::uint16_t targetSoldierIndex,
    const ImportantTargetInfo& info)
{
    if (callerIndex == 0xFFFFu)
        return;

    PendingBodyReport entry{};
    entry.targetSoldierIndex = targetSoldierIndex;
    entry.info = info;
    entry.tickMs = GetTickCount64();

    std::lock_guard<std::mutex> lock(g_VipWakeMutex);
    g_PendingBodyReportByCaller[callerIndex] = entry;
}

// Removes one pending body report without consuming it.
// Params: callerIndex
static void ErasePendingBodyReport(std::uint16_t callerIndex)
{
    if (callerIndex == 0xFFFFu)
        return;

    std::lock_guard<std::mutex> lock(g_VipWakeMutex);
    g_PendingBodyReportByCaller.erase(callerIndex);
}

// Consumes one pending body report for the reporter/caller that is about to talk on radio.
// Params: callerIndex, outEntry
static bool TakePendingBodyReport(std::uint16_t callerIndex, PendingBodyReport& outEntry)
{
    outEntry = {};

    if (callerIndex == 0xFFFFu)
        return false;

    std::lock_guard<std::mutex> lock(g_VipWakeMutex);

    const auto it = g_PendingBodyReportByCaller.find(callerIndex);
    if (it == g_PendingBodyReportByCaller.end())
        return false;

    const ULONGLONG now = GetTickCount64();
    if ((now - it->second.tickMs) > 10000ull)
    {
        g_PendingBodyReportByCaller.erase(it);
        return false;
    }

    outEntry = it->second;
    g_PendingBodyReportByCaller.erase(it);
    return true;
}

// Adds or updates one important target from Lua.
// Params: gameObjectId, isRussian, isOfficer
void Add_VIPImportantGameObjectId(std::uint32_t gameObjectId, bool isRussian, bool isOfficer)
{
    const std::uint16_t soldierIndex = NormalizeSoldierIndexFromGameObjectId(gameObjectId);
    if (soldierIndex == 0xFFFFu)
    {
        Log("[VIPWake] Add ignored: invalid/non-soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    ImportantTargetInfo info{};
    info.important = true;
    info.isRussian = isRussian;
    info.isOfficer = isOfficer;

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_ImportantTargetsBySoldierIndex[soldierIndex] = info;
    }

    Log("[VIPWake] Added important target: gameObjectId=0x%08X russian=%s officer=%s\n",
        gameObjectId,
        isRussian ? "YES" : "NO",
        isOfficer ? "YES" : "NO");
}

// Removes one important target using the original soldier GameObjectId.
// Params: gameObjectId
void Remove_VIPImportantGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex = NormalizeSoldierIndexFromGameObjectId(gameObjectId);
    if (soldierIndex == 0xFFFFu)
    {
        Log("[VIPWake] Remove ignored: invalid/non-soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_ImportantTargetsBySoldierIndex.erase(soldierIndex);
    }

    Log("[VIPWake] Removed important target: gameObjectId=0x%08X\n", gameObjectId);
}

// Clears all important targets and all runtime caches.
// Params: none
void Clear_VIPImportantGameObjectIds()
{
    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_CorpseCacheByCorpseIndex.clear();
        g_PendingComradeWakeByActor.clear();
        g_PendingBodyReportByCaller.clear();
    }

    Log("[VIPWake] Cleared important targets\n");
}

// Calls the first virtual method on an event object to get its event hash.
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
        return reinterpret_cast<GetHashFn_t>(fnAddr)(evt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Dispatches a custom notice reaction through the same reaction manager the game uses.
// Params: noticeSelf, actorId, reactionHash
static void DispatchNoticeReaction(void* noticeSelf, std::uint32_t actorId, std::uint32_t reactionHash)
{
    if (!noticeSelf)
        return;

    __try
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(noticeSelf);

        std::uint64_t managerRoot = 0;
        if (!SafeReadQword(selfAddr + 0x78ull, managerRoot) || !managerRoot)
            return;

        std::uint64_t reactionMgr = 0;
        if (!SafeReadQword(static_cast<std::uintptr_t>(managerRoot) + 0xA8ull, reactionMgr) || !reactionMgr)
            return;

        std::uint64_t vtbl = 0;
        if (!SafeReadQword(static_cast<std::uintptr_t>(reactionMgr), vtbl) || !vtbl)
            return;

        std::uint64_t fnAddr = 0;
        if (!SafeReadQword(static_cast<std::uintptr_t>(vtbl) + 0x20ull, fnAddr) || !fnAddr)
            return;

        using DispatchFn_t =
            void(__fastcall*)(void* mgr,
                std::uint32_t actorId,
                std::uint32_t categoryHash,
                int arg4,
                std::uint32_t reactionHash,
                float delay);

        reinterpret_cast<DispatchFn_t>(fnAddr)(
            reinterpret_cast<void*>(reactionMgr),
            actorId,
            HASH_REACTION_CATEGORY_NOTICE,
            1,
            reactionHash,
            1.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[VIPWake] DispatchNoticeReaction exception\n");
    }
}

// Resolves a NoticeAction state entry used by ComradeAction / RecoveryTouch / StandRecoveryHoldup.
// Params: self, actorId
static std::uintptr_t GetNoticeActionEntry(void* self, std::uint32_t actorId)
{
    if (!self)
        return 0;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint32_t baseIndex = 0;
    std::uint64_t tableBase = 0;

    if (!SafeReadDword(selfAddr + 0x98ull, baseIndex))
        return 0;

    if (!SafeReadQword(selfAddr + 0x90ull, tableBase))
        return 0;

    const std::uint32_t slot = actorId - baseIndex;
    return static_cast<std::uintptr_t>(tableBase + (static_cast<std::uint64_t>(slot) * 0x68ull));
}

// Resolves a DownActionImpl::State_DownIdle entry.
// Params: self, actorId
static std::uintptr_t GetDownIdleEntry(void* self, std::uint32_t actorId)
{
    if (!self)
        return 0;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint32_t baseIndex = 0;
    std::uint64_t tableBase = 0;

    if (!SafeReadDword(selfAddr + 0x98ull, baseIndex))
        return 0;

    if (!SafeReadQword(selfAddr + 0x90ull, tableBase))
        return 0;

    const std::uint32_t slot = actorId - baseIndex;
    return static_cast<std::uintptr_t>(tableBase + (static_cast<std::uint64_t>(slot) * 0x30ull));
}

// Tries to resolve the sleeper index directly from the recovery-touch state entry.
// Params: self, actorId, outSleeperIndex
static bool TryResolveRecoveryTouchSleeperIndex(
    void* self,
    std::uint32_t actorId,
    std::uint16_t& outSleeperIndex)
{
    outSleeperIndex = 0xFFFFu;

    const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
    if (!entry)
        return false;

    std::uint8_t sleeperIndex8 = 0xFFu;
    if (!SafeReadByte(entry + 0x5Dull, sleeperIndex8))
        return false;

    if (sleeperIndex8 == 0xFFu)
        return false;

    outSleeperIndex = static_cast<std::uint16_t>(sleeperIndex8);
    return true;
}

// Tries several plausible fields in the holdup recovery entry to resolve the actual soldier index.
// Params: self, actorId, outRecoveredSoldierIndex, outInfo
static bool TryResolveHoldupImportantTargetInfo(
    void* self,
    std::uint32_t actorId,
    std::uint16_t& outRecoveredSoldierIndex,
    ImportantTargetInfo& outInfo)
{
    outRecoveredSoldierIndex = 0xFFFFu;
    outInfo = {};

    // First try direct actor id as soldier index.
    if (TryGetImportantTargetInfoBySoldierIndex(static_cast<std::uint16_t>(actorId), outInfo))
    {
        outRecoveredSoldierIndex = static_cast<std::uint16_t>(actorId);
        return true;
    }

    const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
    if (!entry)
        return false;

    std::uint8_t candidate8 = 0xFFu;

    // Candidate 1: byte at +0x57.
    if (SafeReadByte(entry + 0x57ull, candidate8) && candidate8 != 0xFFu)
    {
        if (TryGetImportantTargetInfoBySoldierIndex(static_cast<std::uint16_t>(candidate8), outInfo))
        {
            outRecoveredSoldierIndex = static_cast<std::uint16_t>(candidate8);
            return true;
        }
    }

    // Candidate 2: byte at +0x5D.
    if (SafeReadByte(entry + 0x5Dull, candidate8) && candidate8 != 0xFFu)
    {
        if (TryGetImportantTargetInfoBySoldierIndex(static_cast<std::uint16_t>(candidate8), outInfo))
        {
            outRecoveredSoldierIndex = static_cast<std::uint16_t>(candidate8);
            return true;
        }
    }

    // Candidate 3: dword at +0x24.
    std::uint32_t candidate32 = 0;
    if (SafeReadDword(entry + 0x24ull, candidate32))
    {
        const std::uint16_t idx = static_cast<std::uint16_t>(candidate32 & 0xFFFFu);
        if (TryGetImportantTargetInfoBySoldierIndex(idx, outInfo))
        {
            outRecoveredSoldierIndex = idx;
            return true;
        }
    }

    return false;
}

// Caches the current sleeper relation during the comrade wake-up prepare phase.
// Params: self, actorId, proc, evt
static void __fastcall hkState_ComradeAction(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    UNREFERENCED_PARAMETER(evt);

    if (proc == 1)
    {
        const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
        if (entry)
        {
            std::uint8_t sleeperIndex8 = 0xFFu;
            if (SafeReadByte(entry + 0x5Dull, sleeperIndex8))
            {
                const std::uint16_t sleeperIndex = static_cast<std::uint16_t>(sleeperIndex8);
                SetPendingComradeWake(actorId, sleeperIndex);

                ImportantTargetInfo info{};
                if (TryGetImportantTargetInfoBySoldierIndex(sleeperIndex, info))
                {
                    Log("[VIPWake] COMRADE_PREP actor=%u sleeperIndex=%u important=YES russian=%s officer=%s\n",
                        actorId,
                        static_cast<unsigned>(sleeperIndex),
                        info.isRussian ? "YES" : "NO",
                        info.isOfficer ? "YES" : "NO");
                }
            }
        }
    }
    else if (proc == 2)
    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_PendingComradeWakeByActor.erase(actorId);
    }

    g_OrigState_ComradeAction(self, actorId, proc, evt);
}

// Replaces the normal wake/faint reaction when the sleeping target is important.
// Params: self, actorId, proc, evt
static void __fastcall hkState_RecoveryTouch(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);
        if (eventHash == HASH_EVENT_VOICE_NOTICE)
        {
            std::uint16_t sleeperIndex = TakePendingComradeWake(actorId);

            // Fallback read so wake/faint still works even if the cache was lost.
            if (sleeperIndex == 0xFFFFu)
            {
                std::uint16_t fallbackSleeper = 0xFFFFu;
                if (TryResolveRecoveryTouchSleeperIndex(self, actorId, fallbackSleeper))
                {
                    sleeperIndex = fallbackSleeper;
                }
            }

            ImportantTargetInfo info{};
            const bool isImportant = TryGetImportantTargetInfoBySoldierIndex(sleeperIndex, info);

            Log("[VIPWake] TOUCH actor=%u sleeper=%u important=%s russian=%s officer=%s\n",
                actorId,
                static_cast<unsigned>(sleeperIndex),
                isImportant ? "YES" : "NO",
                (isImportant && info.isRussian) ? "YES" : "NO",
                (isImportant && info.isOfficer) ? "YES" : "NO");

            if (isImportant)
            {
                const std::uint32_t reactionHash =
                    info.isRussian ? HASH_SLEEP_WAKE_VIP_RUS : HASH_SLEEP_WAKE_VIP;

                DispatchNoticeReaction(self, actorId, reactionHash);
                return;
            }
        }
    }

    g_OrigState_RecoveryTouch(self, actorId, proc, evt);
}

// Replaces the holdup recovery reaction when the recovered soldier is important.
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
        if (eventHash == HASH_EVENT_VOICE_NOTICE)
        {
            std::uint16_t recoveredSoldierIndex = 0xFFFFu;
            ImportantTargetInfo info{};
            const bool isImportant =
                TryResolveHoldupImportantTargetInfo(self, actorId, recoveredSoldierIndex, info);

            Log("[VIPWake] HOLDUP_RECOVERY actor=%u recoveredSoldierIndex=%u important=%s russian=%s officer=%s\n",
                actorId,
                static_cast<unsigned>(recoveredSoldierIndex),
                isImportant ? "YES" : "NO",
                (isImportant && info.isRussian) ? "YES" : "NO",
                (isImportant && info.isOfficer) ? "YES" : "NO");

            if (isImportant)
            {
                const std::uint32_t reactionHash =
                    info.isRussian ? HASH_HOLDUP_RECOVERY_VIP_RUS : HASH_HOLDUP_RECOVERY_VIP;

                DispatchNoticeReaction(self, actorId, reactionHash);
                return;
            }
        }
    }

    g_OrigState_StandRecoveryHoldup(self, actorId, proc, evt);
}

// Captures the body-report preparation in DownActionImpl::State_DownIdle.
// Params: self, actorId, proc, evt
static void __fastcall hkState_DownIdle(
    void* self,
    std::uint32_t actorId,
    int proc,
    void* evt)
{
    if (proc == 5)
    {
        const std::uintptr_t entry = GetDownIdleEntry(self, actorId);
        if (entry)
        {
            std::uint32_t targetIndex32 = 0;
            if (SafeReadDword(entry + 0x24ull, targetIndex32))
            {
                const std::uint16_t targetSoldierIndex =
                    static_cast<std::uint16_t>(targetIndex32 & 0xFFFFu);

                ImportantTargetInfo info{};
                const bool isImportant =
                    TryGetImportantTargetInfoBySoldierIndex(targetSoldierIndex, info);

                const std::uint16_t callerIndex = static_cast<std::uint16_t>(actorId);

                if (isImportant)
                {
                    SetPendingBodyReport(callerIndex, targetSoldierIndex, info);

                    Log("[VIPWake] BODY_REPORT_PREP callerIndex=0x%04X targetSoldierIndex=0x%04X proc=%d important=YES russian=%s officer=%s\n",
                        static_cast<unsigned>(callerIndex),
                        static_cast<unsigned>(targetSoldierIndex),
                        proc,
                        info.isRussian ? "YES" : "NO",
                        info.isOfficer ? "YES" : "NO");
                }
                else
                {
                    ErasePendingBodyReport(callerIndex);
                }
            }
        }
    }

    g_OrigState_DownIdle(self, actorId, proc, evt);
}

// Builds the current radio context so the label conversion hook knows who is being reported.
// Params: self, slot, proc, outCtx
static bool BuildRadioContext(void* self, std::uint32_t slot, int proc, RadioContext& outCtx)
{
    outCtx = {};

    if (!self || proc != 0)
        return false;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint64_t stateBase = 0;
    std::uint64_t radioBase = 0;

    if (!SafeReadQword(selfAddr + 0x90ull, stateBase))
        return false;

    if (!SafeReadQword(selfAddr + 0x88ull, radioBase))
        return false;

    std::uint16_t rawTargetId = 0xFFFFu;
    std::uint32_t reporterGameObjectId = 0;
    std::uint8_t radioType = 0;

    if (!SafeReadWord(static_cast<std::uintptr_t>(stateBase) + 4ull + (static_cast<std::uint64_t>(slot) * 8ull), rawTargetId))
        return false;

    if (!SafeReadDword(static_cast<std::uintptr_t>(radioBase) + (static_cast<std::uint64_t>(slot) * 0xCull), reporterGameObjectId))
        return false;

    if (!SafeReadByte(static_cast<std::uintptr_t>(radioBase) + 8ull + (static_cast<std::uint64_t>(slot) * 0xCull), radioType))
        return false;

    const std::uint16_t callerIndex =
        NormalizeSoldierIndexFromGameObjectId(reporterGameObjectId);

    outCtx.active = true;
    outCtx.slot = slot;
    outCtx.reporterGameObjectId = reporterGameObjectId;
    outCtx.callerIndex = callerIndex;
    outCtx.targetSoldierIndex = rawTargetId;
    outCtx.radioType = radioType;
    outCtx.info = {};
    outCtx.fromPendingBodyReport = false;
    outCtx.fromCorpseCache = false;
    outCtx.consumeCorpseCacheAfterReturn = false;

    // First choice for body-found radio: pending body-report cache keyed by the real caller.
    if ((radioType == 0x0Eu || radioType == 0x0Fu) && callerIndex != 0xFFFFu)
    {
        PendingBodyReport pending{};
        if (TakePendingBodyReport(callerIndex, pending))
        {
            outCtx.targetSoldierIndex = pending.targetSoldierIndex;
            outCtx.info = pending.info;
            outCtx.fromPendingBodyReport = true;
            return true;
        }
    }

    // Fallback: corpse cache or direct target lookup.
    ImportantTargetInfo info{};
    bool fromCorpseCache = false;
    const bool isImportant =
        TryResolveImportantTargetInfoFromSubject(rawTargetId, info, fromCorpseCache);

    if (isImportant)
        outCtx.info = info;

    outCtx.fromCorpseCache = fromCorpseCache;
    return true;
}

// Wraps StateRadio so label conversion can use the correct per-call context.
// Params: self, slot, proc
static void __fastcall hkStateRadio(void* self, std::uint32_t slot, int proc)
{
    const RadioContext previousCtx = g_RadioContext;

    RadioContext newCtx{};
    if (BuildRadioContext(self, slot, proc, newCtx))
        g_RadioContext = newCtx;

    g_OrigStateRadio(self, slot, proc);

    if (g_RadioContext.active &&
        g_RadioContext.consumeCorpseCacheAfterReturn &&
        g_RadioContext.fromCorpseCache)
    {
        Log("[VIPWake] CorpseCache CLEAR corpseIndex=0x%04X\n",
            static_cast<unsigned>(g_RadioContext.targetSoldierIndex));

        EraseCorpseCacheEntry(g_RadioContext.targetSoldierIndex);
    }

    g_RadioContext = previousCtx;
}

// Overrides CPR0040 with VIP/officer label when the reported body belongs to an important target.
// Params: radioType
static std::uint32_t __fastcall hkConvertRadioTypeToSpeechLabel(std::uint8_t radioType)
{
    const std::uint32_t originalLabel = g_OrigConvertRadioTypeToSpeechLabel(radioType);

    if (!g_RadioContext.active || g_RadioContext.radioType != radioType)
        return originalLabel;

    std::uint32_t finalLabel = originalLabel;

    if ((radioType == 0x0Eu || radioType == 0x0Fu) && g_RadioContext.info.important)
    {
        if (g_RadioContext.info.isOfficer)
            finalLabel = HASH_CP_BODY_FOUND_OFFICER;
        else
            finalLabel = HASH_CP_BODY_FOUND_VIP;

        if (g_RadioContext.fromCorpseCache)
            g_RadioContext.consumeCorpseCacheAfterReturn = true;
    }

    if (radioType == 0x0Cu || radioType == 0x0Eu || radioType == 0x0Fu || g_RadioContext.info.important)
    {
        Log("[VIPWake] CPRadio radioType=0x%02X (%u) slot=%u callerIndex=0x%04X targetSoldierIndex=0x%04X originalLabel=0x%08X finalLabel=0x%08X important=%s russian=%s officer=%s fromPendingBodyReport=%s fromCorpseCache=%s\n",
            static_cast<unsigned>(radioType),
            static_cast<unsigned>(radioType),
            g_RadioContext.slot,
            static_cast<unsigned>(g_RadioContext.callerIndex),
            static_cast<unsigned>(g_RadioContext.targetSoldierIndex),
            originalLabel,
            finalLabel,
            g_RadioContext.info.important ? "YES" : "NO",
            g_RadioContext.info.isRussian ? "YES" : "NO",
            g_RadioContext.info.isOfficer ? "YES" : "NO",
            g_RadioContext.fromPendingBodyReport ? "YES" : "NO",
            g_RadioContext.fromCorpseCache ? "YES" : "NO");
    }

    return finalLabel;
}

// Caches the original important target when the corpse instance is created.
// Params: self, animControl, ragdollPlugin, facialPlugin, param4, location, originalTargetGameObjectId, inheritanceInfo, isPlayerSide
static void __fastcall hkRequestCorpse(
    void* self,
    void* animControl,
    void* ragdollPlugin,
    void* facialPlugin,
    std::uint32_t* param4,
    std::uint32_t* location,
    std::uint16_t originalTargetGameObjectId,
    void* inheritanceInfo,
    char isPlayerSide)
{
    std::uint32_t corpseIndex32 = 0xFFFFFFFFu;
    if (self)
        SafeReadDword(reinterpret_cast<std::uintptr_t>(self) + 0x64ull, corpseIndex32);

    const std::uint16_t corpseIndex = static_cast<std::uint16_t>(corpseIndex32 & 0xFFFFu);
    const std::uint16_t soldierIndex = NormalizeSoldierIndexFromGameObjectId(originalTargetGameObjectId);

    ImportantTargetInfo info{};
    const bool isImportant =
        (soldierIndex != 0xFFFFu) && TryGetImportantTargetInfoBySoldierIndex(soldierIndex, info);

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);

        if (corpseIndex != 0xFFFFu)
        {
            g_CorpseCacheByCorpseIndex.erase(corpseIndex);

            if (isImportant)
            {
                CorpseCacheEntry entry{};
                entry.originalTargetGameObjectId = originalTargetGameObjectId;
                entry.info = info;
                g_CorpseCacheByCorpseIndex[corpseIndex] = entry;

                Log("[VIPWake] CorpseCache SET corpseIndex=0x%04X originalTargetId=0x%04X russian=%s officer=%s\n",
                    static_cast<unsigned>(corpseIndex),
                    static_cast<unsigned>(originalTargetGameObjectId),
                    info.isRussian ? "YES" : "NO",
                    info.isOfficer ? "YES" : "NO");
            }
        }
    }

    Log("[VIPWake] RequestCorpse originalTargetId=0x%04X corpseIndex=0x%04X russian=%s officer=%s\n",
        static_cast<unsigned>(originalTargetGameObjectId),
        static_cast<unsigned>(corpseIndex),
        isImportant && info.isRussian ? "YES" : "NO",
        isImportant && info.isOfficer ? "YES" : "NO");

    g_OrigRequestCorpse(
        self,
        animControl,
        ragdollPlugin,
        facialPlugin,
        param4,
        location,
        originalTargetGameObjectId,
        inheritanceInfo,
        isPlayerSide);
}

// Installs all VIP wake / holdup / body-report / corpse / radio hooks.
// Params: none
bool Install_VIPSleepWakeReaction_Hook()
{
    const bool okComrade = CreateAndEnableHook(
        ResolveGameAddress(ABS_State_ComradeAction),
        reinterpret_cast<void*>(&hkState_ComradeAction),
        reinterpret_cast<void**>(&g_OrigState_ComradeAction));

    const bool okTouch = CreateAndEnableHook(
        ResolveGameAddress(ABS_State_StandToSquatRecoverySleepFaintComradeByTouch),
        reinterpret_cast<void*>(&hkState_RecoveryTouch),
        reinterpret_cast<void**>(&g_OrigState_RecoveryTouch));

    const bool okHoldup = CreateAndEnableHook(
        ResolveGameAddress(ABS_State_StandRecoveryHoldup),
        reinterpret_cast<void*>(&hkState_StandRecoveryHoldup),
        reinterpret_cast<void**>(&g_OrigState_StandRecoveryHoldup));

    const bool okDownIdle = CreateAndEnableHook(
        ResolveGameAddress(ABS_State_DownIdle),
        reinterpret_cast<void*>(&hkState_DownIdle),
        reinterpret_cast<void**>(&g_OrigState_DownIdle));

    const bool okStateRadio = CreateAndEnableHook(
        ResolveGameAddress(ABS_StateRadio),
        reinterpret_cast<void*>(&hkStateRadio),
        reinterpret_cast<void**>(&g_OrigStateRadio));

    const bool okConvert = CreateAndEnableHook(
        ResolveGameAddress(ABS_ConvertRadioTypeToSpeechLabel),
        reinterpret_cast<void*>(&hkConvertRadioTypeToSpeechLabel),
        reinterpret_cast<void**>(&g_OrigConvertRadioTypeToSpeechLabel));

    const bool okRequestCorpse = CreateAndEnableHook(
        ResolveGameAddress(ABS_RequestCorpse),
        reinterpret_cast<void*>(&hkRequestCorpse),
        reinterpret_cast<void**>(&g_OrigRequestCorpse));

    Log("[VIPWake] Install State_ComradeAction: %s\n", okComrade ? "OK" : "FAIL");
    Log("[VIPWake] Install RecoveryTouch: %s\n", okTouch ? "OK" : "FAIL");
    Log("[VIPWake] Install State_StandRecoveryHoldup: %s\n", okHoldup ? "OK" : "FAIL");
    Log("[VIPWake] Install State_DownIdle: %s\n", okDownIdle ? "OK" : "FAIL");
    Log("[VIPWake] Install StateRadio: %s\n", okStateRadio ? "OK" : "FAIL");
    Log("[VIPWake] Install ConvertRadioTypeToSpeechLabel: %s\n", okConvert ? "OK" : "FAIL");
    Log("[VIPWake] Install RequestCorpse: %s\n", okRequestCorpse ? "OK" : "FAIL");

    return okComrade && okTouch && okHoldup && okDownIdle && okStateRadio && okConvert && okRequestCorpse;
}

// Removes all VIP wake / holdup / body-report / corpse / radio hooks and clears state.
// Params: none
bool Uninstall_VIPSleepWakeReaction_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_ComradeAction));
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_StandToSquatRecoverySleepFaintComradeByTouch));
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_StandRecoveryHoldup));
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_DownIdle));
    DisableAndRemoveHook(ResolveGameAddress(ABS_StateRadio));
    DisableAndRemoveHook(ResolveGameAddress(ABS_ConvertRadioTypeToSpeechLabel));
    DisableAndRemoveHook(ResolveGameAddress(ABS_RequestCorpse));

    g_OrigState_ComradeAction = nullptr;
    g_OrigState_RecoveryTouch = nullptr;
    g_OrigState_StandRecoveryHoldup = nullptr;
    g_OrigState_DownIdle = nullptr;
    g_OrigStateRadio = nullptr;
    g_OrigConvertRadioTypeToSpeechLabel = nullptr;
    g_OrigRequestCorpse = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_CorpseCacheByCorpseIndex.clear();
        g_PendingComradeWakeByActor.clear();
        g_PendingBodyReportByCaller.clear();
    }

    g_RadioContext = {};

    Log("[VIPWake] Hooks removed and state cleared\n");
    return true;
}