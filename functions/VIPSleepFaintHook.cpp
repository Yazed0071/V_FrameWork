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

namespace
{
    // Hook type for NoticeActionImpl::State_ComradeAction.
    // Params: self, actorId, proc, evt
    using State_ComradeAction_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);

    // Hook type for NoticeActionImpl::State_StandToSquatRecoverySleepFaintComradeByTouch.
    // Params: self, actorId, proc, evt
    using State_RecoveryTouch_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);

    // Absolute address of NoticeActionImpl::State_ComradeAction.
    static constexpr std::uintptr_t ABS_State_ComradeAction = 0x1414B8D20ull;

    // Absolute address of NoticeActionImpl::State_StandToSquatRecoverySleepFaintComradeByTouch.
    static constexpr std::uintptr_t ABS_State_RecoveryTouch = 0x1414BCEF0ull;

    // Event hash used by the game for voice notice.
    static constexpr std::uint32_t HASH_EVENT_VOICE_NOTICE = 0x1077DB8Du;

    // Reaction category used by the game's notice reaction manager.
    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;

    // Custom reaction hash for important officer wake/faint.
    static constexpr std::uint32_t HASH_SLEEP_WAKE_OFFICER = 0x9CD0A89Cu;

    // Original function pointers.
    static State_ComradeAction_t g_OrigState_ComradeAction = nullptr;
    static State_RecoveryTouch_t g_OrigState_RecoveryTouch = nullptr;

    // Important target info stored by normalized soldier index.
    struct ImportantTargetInfo
    {
        bool important = false;
        bool isOfficer = false;
    };

    // Pending wake relation stored by actor id.
    struct PendingWakeInfo
    {
        std::uint16_t sleeperIndex = 0xFFFFu;
        std::uint16_t sleeperGameObjectId = 0xFFFFu;
        ULONGLONG tickMs = 0;
    };

    // Important targets by normalized soldier index.
    static std::unordered_map<std::uint16_t, ImportantTargetInfo> g_ImportantTargetsBySoldierIndex;

    // Cached actor -> sleeper relation prepared by State_ComradeAction(proc=1).
    static std::unordered_map<std::uint32_t, PendingWakeInfo> g_PendingWakeByActor;

    // Mutex protecting both maps.
    static std::mutex g_SleepFaintMutex;
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

// Returns true if the cached timestamp is still fresh.
// Params: tickMs, maxAgeMs
static bool IsFreshTick(ULONGLONG tickMs, ULONGLONG maxAgeMs = 10000ull)
{
    if (tickMs == 0)
        return false;

    const ULONGLONG now = GetTickCount64();
    return (now - tickMs) <= maxAgeMs;
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
        Log("[SleepFaint] DispatchNoticeReaction exception\n");
    }
}

// Looks up important-target info by normalized soldier index.
// Params: soldierIndex, outInfo
static bool TryGetImportantTargetInfo(std::uint16_t soldierIndex, ImportantTargetInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_SleepFaintMutex);

    const auto it = g_ImportantTargetsBySoldierIndex.find(soldierIndex);
    if (it == g_ImportantTargetsBySoldierIndex.end())
        return false;

    outInfo = it->second;
    return outInfo.important;
}

// Dumps a byte range from the sleep/faint state entry.
// Params: entry, startOffset, endOffset
static void DumpSleepFaintEntryRange(std::uintptr_t entry, std::size_t startOffset, std::size_t endOffset)
{
    if (!entry || endOffset < startOffset)
        return;

    char line[1024]{};
    int pos = 0;

    pos += sprintf_s(
        line + pos,
        sizeof(line) - pos,
        "[SleepFaint] entry=%p dump:",
        reinterpret_cast<void*>(entry));

    for (std::size_t off = startOffset; off <= endOffset; ++off)
    {
        std::uint8_t value = 0xFFu;
        if (SafeReadByte(entry + off, value))
        {
            pos += sprintf_s(line + pos, sizeof(line) - pos, " +%02zX=%02X", off, value);
        }
        else
        {
            pos += sprintf_s(line + pos, sizeof(line) - pos, " +%02zX=??", off);
        }

        if (pos >= static_cast<int>(sizeof(line) - 20))
            break;
    }

    pos += sprintf_s(line + pos, sizeof(line) - pos, "\n");
    Log("%s", line);
}

// Stores one pending sleeper relation for the actor currently preparing the wake action.
// Params: actorId, sleeperIndex, sleeperGameObjectId
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

    Log("[SleepFaint] CACHE_SET actor=%u sleeperIndex=%u sleeperGameObjectId=0x%04X\n",
        actorId,
        static_cast<unsigned>(sleeperIndex),
        static_cast<unsigned>(sleeperGameObjectId));
}

// Tries to read one pending wake relation from the cache without consuming it.
// Params: actorId, outInfo
static bool TryGetPendingWake(std::uint32_t actorId, PendingWakeInfo& outInfo)
{
    outInfo = {};

    std::lock_guard<std::mutex> lock(g_SleepFaintMutex);

    const auto it = g_PendingWakeByActor.find(actorId);
    if (it == g_PendingWakeByActor.end())
    {
        Log("[SleepFaint] CACHE_MISS actor=%u\n", actorId);
        return false;
    }

    if (!IsFreshTick(it->second.tickMs))
    {
        Log("[SleepFaint] CACHE_EXPIRED actor=%u sleeperIndex=%u sleeperGameObjectId=0x%04X\n",
            actorId,
            static_cast<unsigned>(it->second.sleeperIndex),
            static_cast<unsigned>(it->second.sleeperGameObjectId));

        g_PendingWakeByActor.erase(it);
        return false;
    }

    outInfo = it->second;

    Log("[SleepFaint] CACHE_HIT actor=%u sleeperIndex=%u sleeperGameObjectId=0x%04X\n",
        actorId,
        static_cast<unsigned>(outInfo.sleeperIndex),
        static_cast<unsigned>(outInfo.sleeperGameObjectId));

    return true;
}

// Removes one pending sleeper relation for the actor.
// Params: actorId, reason
static void ErasePendingWake(std::uint32_t actorId, const char* reason)
{
    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_PendingWakeByActor.erase(actorId);
    }

    Log("[SleepFaint] CACHE_ERASE actor=%u reason=%s\n",
        actorId,
        reason ? reason : "unknown");
}

// Extracts candidate sleeper values directly from the state entry.
// Params: self, actorId, outSleeperIndexFrom5D, outSleeperGameObjectIdFrom52, outSleeperIndexFrom52, emitRawLog
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
        if (emitRawLog)
        {
            Log("[SleepFaint] no state entry for actor=%u\n", actorId);
        }
        return false;
    }

    std::uint8_t b50 = 0xFFu;
    std::uint8_t b51 = 0xFFu;
    std::uint8_t b52 = 0xFFu;
    std::uint8_t b53 = 0xFFu;
    std::uint8_t b57 = 0xFFu;
    std::uint8_t b5C = 0xFFu;
    std::uint8_t b5D = 0xFFu;
    std::uint8_t b5E = 0xFFu;
    std::uint8_t b5F = 0xFFu;
    std::uint16_t w52 = 0xFFFFu;

    SafeReadByte(entry + 0x50ull, b50);
    SafeReadByte(entry + 0x51ull, b51);
    SafeReadByte(entry + 0x52ull, b52);
    SafeReadByte(entry + 0x53ull, b53);
    SafeReadByte(entry + 0x57ull, b57);
    SafeReadByte(entry + 0x5Cull, b5C);
    SafeReadByte(entry + 0x5Dull, b5D);
    SafeReadByte(entry + 0x5Eull, b5E);
    SafeReadByte(entry + 0x5Full, b5F);
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

    if (emitRawLog)
    {
        Log("[SleepFaint] actor=%u entry=%p raw: "
            "+50=0x%02X +51=0x%02X +52=0x%02X +53=0x%02X "
            "word52=0x%04X idx52=0x%04X +57=0x%02X +5C=0x%02X +5D=0x%02X +5E=0x%02X +5F=0x%02X\n",
            actorId,
            reinterpret_cast<void*>(entry),
            static_cast<unsigned>(b50),
            static_cast<unsigned>(b51),
            static_cast<unsigned>(b52),
            static_cast<unsigned>(b53),
            static_cast<unsigned>(w52),
            static_cast<unsigned>(outSleeperIndexFrom52),
            static_cast<unsigned>(b57),
            static_cast<unsigned>(b5C),
            static_cast<unsigned>(b5D),
            static_cast<unsigned>(b5E),
            static_cast<unsigned>(b5F));

        DumpSleepFaintEntryRange(entry, 0x50, 0x60);
    }

    return true;
}

// Hooks ComradeAction and caches actor -> sleeperIndex during proc=1.
// Params: self, actorId, proc, evt
static void __fastcall hkState_ComradeAction(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    UNREFERENCED_PARAMETER(evt);

    if (proc == 1 || proc == 2 || proc == 6)
    {
        std::uint16_t sleeperIndexFrom5D = 0xFFFFu;
        std::uint16_t sleeperGameObjectIdFrom52 = 0xFFFFu;
        std::uint16_t sleeperIndexFrom52 = 0xFFFFu;

        TryExtractSleepFaintCandidatesFromEntry(
            self,
            actorId,
            sleeperIndexFrom5D,
            sleeperGameObjectIdFrom52,
            sleeperIndexFrom52,
            false);

        Log("[SleepFaint] COMRADE_PROC=%u actor=%u idxFrom5D=%u gameObjectIdFrom52=0x%04X idxFrom52=%u\n",
            proc,
            actorId,
            static_cast<unsigned>(sleeperIndexFrom5D),
            static_cast<unsigned>(sleeperGameObjectIdFrom52),
            static_cast<unsigned>(sleeperIndexFrom52));
    }

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
            false))
        {
            std::uint16_t chosenIndex = 0xFFFFu;

            // For ComradeAction proc=1, prefer +0x5D because the decomp showed it
            // feeding CallVoiceNotice. Keep +0x52 only as additional debug context.
            if (sleeperIndexFrom5D != 0xFFFFu && sleeperIndexFrom5D != 0)
            {
                chosenIndex = sleeperIndexFrom5D;
            }
            else if (sleeperIndexFrom52 != 0xFFFFu)
            {
                chosenIndex = sleeperIndexFrom52;
            }

            if (chosenIndex != 0xFFFFu)
            {
                SetPendingWake(actorId, chosenIndex, sleeperGameObjectIdFrom52);

                ImportantTargetInfo info{};
                const bool isImportant = TryGetImportantTargetInfo(chosenIndex, info);

                Log("[SleepFaint] COMRADE_PREP actor=%u chosenSleeperIndex=%u important=%s officer=%s\n",
                    actorId,
                    static_cast<unsigned>(chosenIndex),
                    isImportant ? "YES" : "NO",
                    (isImportant && info.isOfficer) ? "YES" : "NO");

                if (isImportant)
                {
                    const bool queuedForRadio =
                        Notify_VIPRadioBodyDiscoveredTarget(0, chosenIndex);

                    Log("[SleepFaint] RADIO_QUEUE actor=%u chosenSleeperIndex=%u queued=%s officer=%s\n",
                        actorId,
                        static_cast<unsigned>(chosenIndex),
                        queuedForRadio ? "YES" : "NO",
                        info.isOfficer ? "YES" : "NO");
                }
            }
        }
    }

    g_OrigState_ComradeAction(self, actorId, proc, evt);
}

// Hooks RecoveryTouch and replaces the normal wake/faint notice reaction when the sleeper is important.
// Params: self, actorId, proc, evt
static void __fastcall hkState_RecoveryTouch(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    if (proc == 1 || proc == 2 || proc == 6)
    {
        std::uint16_t sleeperIndexFrom5D = 0xFFFFu;
        std::uint16_t sleeperGameObjectIdFrom52 = 0xFFFFu;
        std::uint16_t sleeperIndexFrom52 = 0xFFFFu;

        TryExtractSleepFaintCandidatesFromEntry(
            self,
            actorId,
            sleeperIndexFrom5D,
            sleeperGameObjectIdFrom52,
            sleeperIndexFrom52,
            false);

        Log("[SleepFaint] TOUCH_PROC=%u actor=%u idxFrom5D=%u gameObjectIdFrom52=0x%04X idxFrom52=%u\n",
            proc,
            actorId,
            static_cast<unsigned>(sleeperIndexFrom5D),
            static_cast<unsigned>(sleeperGameObjectIdFrom52),
            static_cast<unsigned>(sleeperIndexFrom52));
    }

    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);
        Log("[SleepFaint] TOUCH_PROC=6 actor=%u eventHash=0x%08X\n", actorId, eventHash);

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
                    false))
                {
                    // At touch time, try both and prefer the full GameObjectId-derived value if valid.
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

            Log("[SleepFaint] TOUCH actor=%u sleeperGameObjectId=0x%04X sleeperIndex=%u important=%s officer=%s\n",
                actorId,
                static_cast<unsigned>(sleeperGameObjectId),
                static_cast<unsigned>(sleeperIndex),
                isImportant ? "YES" : "NO",
                (isImportant && info.isOfficer) ? "YES" : "NO");

            if (isImportant && info.isOfficer)
            {
                Log("[SleepFaint] DISPATCH actor=%u sleeperGameObjectId=0x%04X sleeperIndex=%u officer=YES\n",
                    actorId,
                    static_cast<unsigned>(sleeperGameObjectId),
                    static_cast<unsigned>(sleeperIndex));

                ErasePendingWake(actorId, "dispatch");
                DispatchNoticeReaction(self, actorId, HASH_SLEEP_WAKE_OFFICER);
                return;
            }
        }
    }

    g_OrigState_RecoveryTouch(self, actorId, proc, evt);
}

// Adds one important sleep/faint target by original GameObjectId.
// Params: gameObjectId, isOfficer
void Add_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer)
{
    const std::uint16_t soldierIndex = NormalizeSoldierIndexFromGameObjectId(gameObjectId);
    if (soldierIndex == 0xFFFFu)
    {
        Log("[SleepFaint] Add ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    ImportantTargetInfo info{};
    info.important = true;
    info.isOfficer = isOfficer;

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex[soldierIndex] = info;
    }

    Log("[SleepFaint] Added important target: gameObjectId=0x%08X soldierIndex=0x%04X officer=%s\n",
        gameObjectId,
        static_cast<unsigned>(soldierIndex),
        isOfficer ? "YES" : "NO");
}

// Removes one important sleep/faint target by original GameObjectId.
// Params: gameObjectId
void Remove_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex = NormalizeSoldierIndexFromGameObjectId(gameObjectId);
    if (soldierIndex == 0xFFFFu)
    {
        Log("[SleepFaint] Remove ignored: invalid soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex.erase(soldierIndex);
    }

    Log("[SleepFaint] Removed important target: gameObjectId=0x%08X soldierIndex=0x%04X\n",
        gameObjectId,
        static_cast<unsigned>(soldierIndex));
}

// Clears all registered important targets and pending wake cache.
// Params: none
void Clear_VIPSleepFaintImportantGameObjectIds()
{
    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_PendingWakeByActor.clear();
    }

    Log("[SleepFaint] Cleared all important targets and pending wake cache\n");
}

// Installs the sleep/faint-only hooks.
// Params: none
bool Install_VIPSleepFaint_Hook()
{
    const bool okComrade = CreateAndEnableHook(
        ResolveGameAddress(ABS_State_ComradeAction),
        reinterpret_cast<void*>(&hkState_ComradeAction),
        reinterpret_cast<void**>(&g_OrigState_ComradeAction));

    const bool okTouch = CreateAndEnableHook(
        ResolveGameAddress(ABS_State_RecoveryTouch),
        reinterpret_cast<void*>(&hkState_RecoveryTouch),
        reinterpret_cast<void**>(&g_OrigState_RecoveryTouch));

    Log("[SleepFaint] Install State_ComradeAction: %s\n", okComrade ? "OK" : "FAIL");
    Log("[SleepFaint] Install State_RecoveryTouch: %s\n", okTouch ? "OK" : "FAIL");

    return okComrade && okTouch;
}

// Removes the sleep/faint-only hooks and clears runtime state.
// Params: none
bool Uninstall_VIPSleepFaint_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_ComradeAction));
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_RecoveryTouch));

    g_OrigState_ComradeAction = nullptr;
    g_OrigState_RecoveryTouch = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_SleepFaintMutex);
        g_ImportantTargetsBySoldierIndex.clear();
        g_PendingWakeByActor.clear();
    }

    Log("[SleepFaint] Hooks removed and state cleared\n");
    return true;
}