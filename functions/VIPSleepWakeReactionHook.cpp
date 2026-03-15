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
    // Stores which alternate line set an important target should use.
    enum class ImportantTargetMode : std::uint8_t
    {
        Normal = 0,
        Russian = 1,
    };

    // Hooks NoticeActionImpl::State_ComradeAction.
    // Params:
    //   self    = NoticeActionImpl*
    //   actorId = acting soldier id
    //   proc    = state proc
    //   evt     = state event
    using State_ComradeAction_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);

    // Hooks NoticeActionImpl::State_StandToSquatRecoverySleepFaintComradeByTouch.
    // Params:
    //   self    = NoticeActionImpl*
    //   actorId = acting soldier id
    //   proc    = state proc
    //   evt     = state event
    using State_StandToSquatRecoverySleepFaintComradeByTouch_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);

    // Hooks NoticeActionImpl::State_StandRecoveryHoldup.
    // Params:
    //   self    = NoticeActionImpl*
    //   actorId = acting soldier id
    //   proc    = state proc
    //   evt     = state event
    using State_StandRecoveryHoldup_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc, void* evt);

    // Hooks RadioSpeechHandlerImpl::CallWithRadioType.
    // Params:
    //   handlerSelf          = radio speech handler
    //   outSpeechId          = out speech id
    //   slot                 = radio slot
    //   radioType            = radio type enum
    //   targetGameObjectId   = reported target / corpse locator id
    using CallWithRadioType_t =
        std::uint16_t* (__fastcall*)(void* handlerSelf,
            std::uint16_t* outSpeechId,
            std::uint32_t slot,
            std::uint8_t radioType,
            std::uint32_t targetGameObjectId);

    // Hooks CorpseManagerImpl::RequestCorpse.
    // Params:
    //   self                 = corpse manager
    //   animControl          = anim control
    //   ragdollPlugin        = ragdoll plugin
    //   facialPlugin         = facial plugin
    //   param4               = extra payload
    //   locationPtr          = corpse location
    //   originalGameObjectId = original dead soldier GameObjectId
    //   inheritanceInfo      = inheritance info
    //   forceFlag            = bool flag
    using RequestCorpse_t =
        void(__fastcall*)(void* self,
            void* animControl,
            void* ragdollPlugin,
            void* facialPlugin,
            void* param4,
            void* locationPtr,
            std::uint32_t originalGameObjectId,
            void* inheritanceInfo,
            bool forceFlag);

    // Absolute addresses recovered from your dumps.
    static constexpr std::uintptr_t ABS_State_ComradeAction =
        0x1414B8D20ull;

    static constexpr std::uintptr_t ABS_State_StandToSquatRecoverySleepFaintComradeByTouch =
        0x1414BCEF0ull;

    static constexpr std::uintptr_t ABS_State_StandRecoveryHoldup =
        0x1414BCA10ull;

    static constexpr std::uintptr_t ABS_CallWithRadioType =
        0x1473CFF10ull;

    static constexpr std::uintptr_t ABS_RequestCorpse =
        0x140A69070ull;

    // Corpse locator ids observed in your logs start from 0x000B.
    static constexpr std::uint16_t CORPSE_LOCATOR_BASE_GAMEOBJECT_ID = 0x000Bu;

    // Event hash used by the recovery voice dispatch in both wake and holdup flows.
    static constexpr std::uint32_t EVT_RECOVERY_VOICE = 0x1077DB8Du;

    // Default body-found radio type and VIP body-found radio type.
    static constexpr std::uint8_t RADIO_TYPE_BODY_FOUND_NORMAL = 0x0Eu;
    static constexpr std::uint8_t RADIO_TYPE_BODY_FOUND_VIP = 0x0Fu;

    // Reaction hashes for sleeping comrade wakeup.
    static constexpr std::uint32_t REACTION_WAKE_NORMAL = 0x9CD0A89Cu;
    static constexpr std::uint32_t REACTION_WAKE_RUSSIAN = 0x9CD0A89Fu;

    // Reaction hashes for holdup recovery.
    static constexpr std::uint32_t REACTION_HOLDUP_NORMAL = 0x92D098DEu;
    static constexpr std::uint32_t REACTION_HOLDUP_RUSSIAN = 0x92D098DDu;

    static State_ComradeAction_t g_OrigState_ComradeAction = nullptr;
    static State_StandToSquatRecoverySleepFaintComradeByTouch_t
        g_OrigState_StandToSquatRecoverySleepFaintComradeByTouch = nullptr;
    static State_StandRecoveryHoldup_t g_OrigState_StandRecoveryHoldup = nullptr;
    static CallWithRadioType_t g_OrigCallWithRadioType = nullptr;
    static RequestCorpse_t g_OrigRequestCorpse = nullptr;

    // Important original soldier targets from Lua:
    //   0x0400 | index -> mode
    static std::unordered_map<std::uint16_t, ImportantTargetMode> g_ImportantTargets;

    // Maps corpse locator object id -> original important dead soldier id.
    struct CorpseOriginCacheEntry
    {
        std::uint16_t originalTargetId = 0xFFFFu;
        ImportantTargetMode mode = ImportantTargetMode::Normal;
    };

    static std::unordered_map<std::uint16_t, CorpseOriginCacheEntry> g_CorpseLocatorOriginCache;

    static std::mutex g_VipWakeMutex;
}

// Safely reads a qword from memory.
// Params: addr (uintptr_t), outValue (uint64_t&)
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
// Params: addr (uintptr_t), outValue (uint32_t&)
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
// Params: addr (uintptr_t), outValue (uint16_t&)
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
// Params: addr (uintptr_t), outValue (uint8_t&)
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

// Checks whether a GameObjectId is a soldier-style id.
// Params: targetId (uint16_t)
static bool IsValidSoldierTargetId(std::uint16_t targetId)
{
    if (targetId == 0xFFFFu)
        return false;

    return (targetId & 0xFE00u) == 0x0400u;
}

// Normalizes a Lua/GameObject id into the soldier-target domain.
// Params: gameObjectId (uint32_t)
static std::uint16_t NormalizeSoldierTargetId(std::uint32_t gameObjectId)
{
    const std::uint16_t targetId = static_cast<std::uint16_t>(gameObjectId);
    return IsValidSoldierTargetId(targetId) ? targetId : 0xFFFFu;
}

// Converts a corpse slot to its corpse locator GameObjectId.
// Params: corpseSlot (uint32_t)
static std::uint16_t CorpseLocatorIdFromSlot(std::uint32_t corpseSlot)
{
    return static_cast<std::uint16_t>(CORPSE_LOCATOR_BASE_GAMEOBJECT_ID + corpseSlot);
}

// Returns the wake reaction hash for a mode.
// Params: mode (ImportantTargetMode)
static std::uint32_t GetWakeReactionHash(ImportantTargetMode mode)
{
    return mode == ImportantTargetMode::Russian
        ? REACTION_WAKE_RUSSIAN
        : REACTION_WAKE_NORMAL;
}

// Returns the holdup reaction hash for a mode.
// Params: mode (ImportantTargetMode)
static std::uint32_t GetHoldupReactionHash(ImportantTargetMode mode)
{
    return mode == ImportantTargetMode::Russian
        ? REACTION_HOLDUP_RUSSIAN
        : REACTION_HOLDUP_NORMAL;
}

// Looks up whether a target id is important and returns its mode.
// Params: targetId (uint16_t), outMode (ImportantTargetMode&)
static bool TryGetImportantTargetMode(std::uint16_t targetId, ImportantTargetMode& outMode)
{
    std::lock_guard<std::mutex> lock(g_VipWakeMutex);

    const auto it = g_ImportantTargets.find(targetId);
    if (it == g_ImportantTargets.end())
        return false;

    outMode = it->second;
    return true;
}

// Updates the corpse-origin cache for a corpse locator.
// Only important original targets are cached.
// Params: corpseLocatorId (uint16_t), originalTargetId (uint16_t)
static void UpdateCorpseOriginCache(std::uint16_t corpseLocatorId, std::uint16_t originalTargetId)
{
    ImportantTargetMode mode = ImportantTargetMode::Normal;
    const bool isImportant = TryGetImportantTargetMode(originalTargetId, mode);

    std::lock_guard<std::mutex> lock(g_VipWakeMutex);

    if (isImportant)
    {
        g_CorpseLocatorOriginCache[corpseLocatorId] = { originalTargetId, mode };

        Log("[VIPWake] CorpseCache SET corpseLocatorId=0x%04X originalTargetId=0x%04X mode=%s\n",
            static_cast<unsigned>(corpseLocatorId),
            static_cast<unsigned>(originalTargetId),
            mode == ImportantTargetMode::Russian ? "Russian" : "Normal");
    }
    else
    {
        g_CorpseLocatorOriginCache.erase(corpseLocatorId);
    }
}

// Takes and clears one corpse-origin cache entry.
// Params:
//   corpseLocatorId     = corpse locator object id
//   outOriginalTargetId = original dead soldier id
//   outMode             = important target mode
static bool TryTakeCorpseOriginCache(std::uint16_t corpseLocatorId,
    std::uint16_t& outOriginalTargetId,
    ImportantTargetMode& outMode)
{
    std::lock_guard<std::mutex> lock(g_VipWakeMutex);

    const auto it = g_CorpseLocatorOriginCache.find(corpseLocatorId);
    if (it == g_CorpseLocatorOriginCache.end())
        return false;

    outOriginalTargetId = it->second.originalTargetId;
    outMode = it->second.mode;
    g_CorpseLocatorOriginCache.erase(it);
    return true;
}

// Calls the first virtual on an event object and returns the event hash.
// Params: evt (void*)
static std::uint32_t GetEventHash(void* evt)
{
    if (!evt)
        return 0;

    __try
    {
        const std::uintptr_t evtAddr = reinterpret_cast<std::uintptr_t>(evt);
        const std::uintptr_t vtbl = *reinterpret_cast<const std::uintptr_t*>(evtAddr);
        if (!vtbl)
            return 0;

        const std::uintptr_t fnAddr = *reinterpret_cast<const std::uintptr_t*>(vtbl + 0x0ull);
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

// Resolves a NoticeActionImpl action entry.
// Formula recovered from the game:
//   entry = ((actorId - *(int*)(self + 0x98)) * 0x68) + *(qword*)(self + 0x90)
// Params: self (void*), actorId (uint32_t)
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
    return static_cast<std::uintptr_t>(tableBase + static_cast<std::uint64_t>(slot) * 0x68ull);
}

// Reads the target id from entry + 0x50.
// In the working paths you showed, the upper 16 bits hold the relevant target GameObjectId.
// Params: entry (uintptr_t), outTargetId (uint16_t&)
static bool TryGetTargetIdFromEntryPacked50(std::uintptr_t entry, std::uint16_t& outTargetId)
{
    std::uint32_t packed50 = 0;
    if (!SafeReadDword(entry + 0x50ull, packed50))
        return false;

    const std::uint16_t targetId = static_cast<std::uint16_t>(packed50 >> 16);
    if (!IsValidSoldierTargetId(targetId))
        return false;

    outTargetId = targetId;
    return true;
}

// Reads the preparation target from ComradeAction entry + 0x5D.
// This is useful only for a light debug log.
// Params: entry (uintptr_t), outTargetId (uint16_t&)
static bool TryGetPreparedComradeTargetId(std::uintptr_t entry, std::uint16_t& outTargetId)
{
    std::uint8_t index8 = 0xFFu;
    if (!SafeReadByte(entry + 0x5Dull, index8))
        return false;

    if (index8 == 0xFFu)
        return false;

    const std::uint16_t targetId = static_cast<std::uint16_t>(0x0400u | index8);
    if (!IsValidSoldierTargetId(targetId))
        return false;

    outTargetId = targetId;
    return true;
}

// Dispatches a reaction through the same downstream manager used by the vanilla code.
// Params: self (void*), actorId (uint32_t), reactionHash (uint32_t)
static void DispatchRecoveryReaction(void* self, std::uint32_t actorId, std::uint32_t reactionHash)
{
    if (!self)
        return;

    __try
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

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

        auto fn = reinterpret_cast<DispatchFn_t>(fnAddr);
        fn(reinterpret_cast<void*>(reactionMgr), actorId, 0x95EA16B0u, 1, reactionHash, 1.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[VIPWake] DispatchRecoveryReaction exception\n");
    }
}

// Adds or updates one important target from Lua.
// Params: gameObjectId (uint32_t), isRussian (bool)
void Add_VIPImportantGameObjectId(std::uint32_t gameObjectId, bool isRussian)
{
    const std::uint16_t targetId = NormalizeSoldierTargetId(gameObjectId);
    if (targetId == 0xFFFFu)
    {
        Log("[VIPWake] Add ignored: invalid/non-soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    const ImportantTargetMode mode =
        isRussian ? ImportantTargetMode::Russian : ImportantTargetMode::Normal;

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_ImportantTargets[targetId] = mode;
    }

    Log("[VIPWake] Added important target: gameObjectId=0x%08X mode=%s\n",
        gameObjectId,
        mode == ImportantTargetMode::Russian ? "Russian" : "Normal");
}

// Removes one important target.
// Params: gameObjectId (uint32_t)
void Remove_VIPImportantGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t targetId = NormalizeSoldierTargetId(gameObjectId);
    if (targetId == 0xFFFFu)
    {
        Log("[VIPWake] Remove ignored: invalid/non-soldier GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_ImportantTargets.erase(targetId);
    }

    Log("[VIPWake] Removed important target: gameObjectId=0x%08X\n", gameObjectId);
}

// Clears all important targets and corpse-origin cache.
// Params: none
void Clear_VIPImportantGameObjectIds()
{
    std::lock_guard<std::mutex> lock(g_VipWakeMutex);
    g_ImportantTargets.clear();
    g_CorpseLocatorOriginCache.clear();

    Log("[VIPWake] Cleared important targets\n");
}

// Hooked State_ComradeAction.
// This only logs the prepared target on proc 1 when it is important.
// Params: self (void*), actorId (uint32_t), proc (uint32_t), evt (void*)
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
            std::uint16_t targetId = 0xFFFFu;
            if (TryGetPreparedComradeTargetId(entry, targetId))
            {
                ImportantTargetMode mode = ImportantTargetMode::Normal;
                if (TryGetImportantTargetMode(targetId, mode))
                {
                    Log("[VIPWake] COMRADE_PREP actor=%u targetId=0x%04X mode=%s\n",
                        actorId,
                        static_cast<unsigned>(targetId),
                        mode == ImportantTargetMode::Russian ? "Russian" : "Normal");
                }
            }
        }
    }

    g_OrigState_ComradeAction(self, actorId, proc, evt);
}

// Hooked sleeping-comrade recovery touch.
// If the touched target is an important VIP, it forces the VIP recovery line.
// Params: self (void*), actorId (uint32_t), proc (uint32_t), evt (void*)
static void __fastcall hkState_StandToSquatRecoverySleepFaintComradeByTouch(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);
        if (eventHash == EVT_RECOVERY_VOICE)
        {
            const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
            std::uint16_t targetId = 0xFFFFu;

            if (entry && TryGetTargetIdFromEntryPacked50(entry, targetId))
            {
                ImportantTargetMode mode = ImportantTargetMode::Normal;
                if (TryGetImportantTargetMode(targetId, mode))
                {
                    const std::uint32_t reactionHash = GetWakeReactionHash(mode);

                    Log("[VIPWake] SLEEP_MATCH actor=%u targetId=0x%04X mode=%s reaction=0x%08X\n",
                        actorId,
                        static_cast<unsigned>(targetId),
                        mode == ImportantTargetMode::Russian ? "Russian" : "Normal",
                        reactionHash);

                    DispatchRecoveryReaction(self, actorId, reactionHash);
                    return;
                }
            }
        }
    }

    g_OrigState_StandToSquatRecoverySleepFaintComradeByTouch(self, actorId, proc, evt);
}

// Hooked holdup recovery.
// If the recovered target is an important VIP, it forces the VIP holdup line.
// Params: self (void*), actorId (uint32_t), proc (uint32_t), evt (void*)
static void __fastcall hkState_StandRecoveryHoldup(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc,
    void* evt)
{
    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);
        if (eventHash == EVT_RECOVERY_VOICE)
        {
            const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);
            std::uint16_t targetId = 0xFFFFu;

            if (entry && TryGetTargetIdFromEntryPacked50(entry, targetId))
            {
                ImportantTargetMode mode = ImportantTargetMode::Normal;
                if (TryGetImportantTargetMode(targetId, mode))
                {
                    const std::uint32_t reactionHash = GetHoldupReactionHash(mode);

                    Log("[VIPWake] HOLDUP_MATCH actor=%u targetId=0x%04X mode=%s reaction=0x%08X\n",
                        actorId,
                        static_cast<unsigned>(targetId),
                        mode == ImportantTargetMode::Russian ? "Russian" : "Normal",
                        reactionHash);

                    DispatchRecoveryReaction(self, actorId, reactionHash);
                    return;
                }
            }
        }
    }

    g_OrigState_StandRecoveryHoldup(self, actorId, proc, evt);
}

// Hooked RequestCorpse.
// Captures corpseLocatorId -> original dead soldier GameObjectId.
// Params:
//   self                 = corpse manager
//   animControl          = anim control
//   ragdollPlugin        = ragdoll plugin
//   facialPlugin         = facial plugin
//   param4               = extra payload
//   locationPtr          = corpse location
//   originalGameObjectId = original dead soldier GameObjectId
//   inheritanceInfo      = inheritance info
//   forceFlag            = bool flag
static void __fastcall hkRequestCorpse(void* self,
    void* animControl,
    void* ragdollPlugin,
    void* facialPlugin,
    void* param4,
    void* locationPtr,
    std::uint32_t originalGameObjectId,
    void* inheritanceInfo,
    bool forceFlag)
{
    std::uint32_t corpseSlotBefore = 0;
    std::uint64_t recordBaseBefore = 0;

    if (self)
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);
        SafeReadDword(selfAddr + 0x64ull, corpseSlotBefore); // this + 100
        SafeReadQword(selfAddr + 0x38ull, recordBaseBefore); // corpse record array
    }

    g_OrigRequestCorpse(self,
        animControl,
        ragdollPlugin,
        facialPlugin,
        param4,
        locationPtr,
        originalGameObjectId,
        inheritanceInfo,
        forceFlag);

    if (!self || !recordBaseBefore)
        return;

    const std::uint16_t originalTargetId =
        static_cast<std::uint16_t>(originalGameObjectId);

    if (!IsValidSoldierTargetId(originalTargetId))
        return;

    const std::uintptr_t recordAddr =
        static_cast<std::uintptr_t>(recordBaseBefore +
            static_cast<std::uint64_t>(corpseSlotBefore) * 0x48ull);

    // The original GameObjectId is stored at record + 0x44.
    std::uint16_t storedOriginalTargetId = 0xFFFFu;
    if (!SafeReadWord(recordAddr + 0x44ull, storedOriginalTargetId))
        return;

    if (storedOriginalTargetId != originalTargetId)
    {
        Log("[VIPWake] RequestCorpse VERIFY FAIL slot=%u inputOriginal=0x%04X storedOriginal=0x%04X\n",
            corpseSlotBefore,
            static_cast<unsigned>(originalTargetId),
            static_cast<unsigned>(storedOriginalTargetId));
        return;
    }

    const std::uint16_t corpseLocatorId = CorpseLocatorIdFromSlot(corpseSlotBefore);

    Log("[VIPWake] RequestCorpse slot=%u corpseLocatorId=0x%04X originalTargetId=0x%04X\n",
        corpseSlotBefore,
        static_cast<unsigned>(corpseLocatorId),
        static_cast<unsigned>(originalTargetId));

    UpdateCorpseOriginCache(corpseLocatorId, originalTargetId);
}

// Hooked CallWithRadioType.
// For body-found reports, if the target is an important dead VIP, it swaps:
//   0x0E (CPR0040 body found) -> 0x0F (CPR0042 VIP body found)
// It also consumes the corpse-origin cache after use.
// Params:
//   handlerSelf        = radio speech handler
//   outSpeechId        = out speech id
//   slot               = radio slot
//   radioType          = radio type enum
//   targetGameObjectId = reported target / corpse locator id
static std::uint16_t* __fastcall hkCallWithRadioType(
    void* handlerSelf,
    std::uint16_t* outSpeechId,
    std::uint32_t slot,
    std::uint8_t radioType,
    std::uint32_t targetGameObjectId)
{
    const std::uint16_t targetId16 =
        static_cast<std::uint16_t>(targetGameObjectId);

    if (radioType == RADIO_TYPE_BODY_FOUND_NORMAL)
    {
        ImportantTargetMode mode = ImportantTargetMode::Normal;
        bool important = false;
        std::uint16_t resolvedOriginalTargetId = targetId16;

        // First handle the direct case, just in case the engine gives the original soldier id.
        if (TryGetImportantTargetMode(targetId16, mode))
        {
            important = true;
        }
        else
        {
            // Then handle the corpse locator case and consume that cache entry.
            if (TryTakeCorpseOriginCache(targetId16, resolvedOriginalTargetId, mode))
            {
                important = true;
            }
        }

        Log("[VIPWake] CPRadio radioType=0x%02X slot=%u targetGameObjectId=0x%04X resolvedOriginal=0x%04X important=%s\n",
            static_cast<unsigned>(radioType),
            slot,
            static_cast<unsigned>(targetId16),
            static_cast<unsigned>(resolvedOriginalTargetId),
            important ? "YES" : "NO");

        if (important)
        {
            Log("[VIPWake] CPRadio OVERRIDE 0x0E -> 0x0F for originalTargetId=0x%04X mode=%s\n",
                static_cast<unsigned>(resolvedOriginalTargetId),
                mode == ImportantTargetMode::Russian ? "Russian" : "Normal");

            return g_OrigCallWithRadioType(
                handlerSelf,
                outSpeechId,
                slot,
                RADIO_TYPE_BODY_FOUND_VIP,
                targetGameObjectId);
        }
    }

    return g_OrigCallWithRadioType(
        handlerSelf,
        outSpeechId,
        slot,
        radioType,
        targetGameObjectId);
}

// Installs all VIP hooks in this module.
// Params: none
bool Install_VIPSleepWakeReaction_Hook()
{
    void* comradeTarget = ResolveGameAddress(ABS_State_ComradeAction);
    void* sleepTarget = ResolveGameAddress(ABS_State_StandToSquatRecoverySleepFaintComradeByTouch);
    void* holdupTarget = ResolveGameAddress(ABS_State_StandRecoveryHoldup);
    void* radioTarget = ResolveGameAddress(ABS_CallWithRadioType);
    void* requestCorpseTarget = ResolveGameAddress(ABS_RequestCorpse);

    if (!comradeTarget)
    {
        Log("[VIPWake] Resolve failed: State_ComradeAction\n");
        return false;
    }

    if (!sleepTarget)
    {
        Log("[VIPWake] Resolve failed: State_StandToSquatRecoverySleepFaintComradeByTouch\n");
        return false;
    }

    if (!holdupTarget)
    {
        Log("[VIPWake] Resolve failed: State_StandRecoveryHoldup\n");
        return false;
    }

    if (!radioTarget)
    {
        Log("[VIPWake] Resolve failed: CallWithRadioType\n");
        return false;
    }

    if (!requestCorpseTarget)
    {
        Log("[VIPWake] Resolve failed: RequestCorpse\n");
        return false;
    }

    const bool okComrade = CreateAndEnableHook(
        comradeTarget,
        reinterpret_cast<void*>(&hkState_ComradeAction),
        reinterpret_cast<void**>(&g_OrigState_ComradeAction));

    const bool okSleep = CreateAndEnableHook(
        sleepTarget,
        reinterpret_cast<void*>(&hkState_StandToSquatRecoverySleepFaintComradeByTouch),
        reinterpret_cast<void**>(&g_OrigState_StandToSquatRecoverySleepFaintComradeByTouch));

    const bool okHoldup = CreateAndEnableHook(
        holdupTarget,
        reinterpret_cast<void*>(&hkState_StandRecoveryHoldup),
        reinterpret_cast<void**>(&g_OrigState_StandRecoveryHoldup));

    const bool okRadio = CreateAndEnableHook(
        radioTarget,
        reinterpret_cast<void*>(&hkCallWithRadioType),
        reinterpret_cast<void**>(&g_OrigCallWithRadioType));

    const bool okRequestCorpse = CreateAndEnableHook(
        requestCorpseTarget,
        reinterpret_cast<void*>(&hkRequestCorpse),
        reinterpret_cast<void**>(&g_OrigRequestCorpse));

    Log("[VIPWake] Install State_ComradeAction: %s\n", okComrade ? "OK" : "FAIL");
    Log("[VIPWake] Install SleepRecoveryTouch: %s\n", okSleep ? "OK" : "FAIL");
    Log("[VIPWake] Install StandRecoveryHoldup: %s\n", okHoldup ? "OK" : "FAIL");
    Log("[VIPWake] Install CallWithRadioType: %s\n", okRadio ? "OK" : "FAIL");
    Log("[VIPWake] Install RequestCorpse: %s\n", okRequestCorpse ? "OK" : "FAIL");

    return okComrade && okSleep && okHoldup && okRadio && okRequestCorpse;
}

// Uninstalls all VIP hooks and clears runtime state.
// Params: none
bool Uninstall_VIPSleepWakeReaction_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_ComradeAction));
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_StandToSquatRecoverySleepFaintComradeByTouch));
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_StandRecoveryHoldup));
    DisableAndRemoveHook(ResolveGameAddress(ABS_CallWithRadioType));
    DisableAndRemoveHook(ResolveGameAddress(ABS_RequestCorpse));

    g_OrigState_ComradeAction = nullptr;
    g_OrigState_StandToSquatRecoverySleepFaintComradeByTouch = nullptr;
    g_OrigState_StandRecoveryHoldup = nullptr;
    g_OrigCallWithRadioType = nullptr;
    g_OrigRequestCorpse = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_VipWakeMutex);
        g_ImportantTargets.clear();
        g_CorpseLocatorOriginCache.clear();
    }

    Log("[VIPWake] Hooks removed and state cleared\n");
    return true;
}