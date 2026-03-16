#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "HookUtils.h"
#include "log.h"
#include "VIPRadioHook.h"

namespace
{
    // Address of tpp::gm::corpse::impl::CorpseManagerImpl::RequestCorpse.
    // Params: self, animControl, ragdoll, facial, facialParam, location, originalGameObjectId, inheritanceInfo, fromScript
    static constexpr uintptr_t ABS_RequestCorpse = 0x140A69070ull;

    // Address of tpp::gm::impl::cp::ActionControllerImpl::StateRadio.
    // Params: self, slot, proc
    static constexpr uintptr_t ABS_StateRadio = 0x140D69140ull;

    // Address of tpp::gm::impl::cp::anonymous_namespace::RadioSpeechHandlerImpl::CallWithRadioType.
    // Params: self, outHandle, ownerIndex, radioType, arg5
    static constexpr uintptr_t ABS_CallWithRadioType = 0x1473CFF10ull;

    // Address of tpp::gm::impl::cp::anonymous_namespace::RadioSpeechHandlerImpl::CallImpl.
    // Params: selfMinus20, outHandle, ownerIndex, speechLabel, arg5
    static constexpr uintptr_t ABS_CallImpl = 0x1473CFCD0ull;

    // CPR0040: body found.
    static constexpr std::uint8_t kRadioTypeBodyFound = 0x0E;

    // CPR0042: VIP body found.
    static constexpr std::uint8_t kRadioTypeVipBodyFound = 0x0F;

    // Officer-specific direct speech label.
    static constexpr std::uint32_t kOfficerBodyFoundSpeechLabel = 0xDD6EA61Bu;

    struct ImportantTargetInfo
    {
        std::uint32_t gameObjectId = 0;
        std::uint16_t soldierIndex = 0;
        bool isOfficer = false;
    };

    struct PendingBodyFoundOverride
    {
        bool active = false;
        ImportantTargetInfo target{};
    };

    // Function pointer for CorpseManagerImpl::RequestCorpse.
    // Params: self, animControl, ragdollPlugin, facialPlugin, facialParam, location, originalGameObjectId, inheritanceInfo, fromScript
    using RequestCorpse_t = void(__fastcall*)(
        void* self,
        void* animControl,
        void* ragdollPlugin,
        void* facialPlugin,
        std::uint32_t* facialParam,
        const void* location,
        std::uint16_t originalGameObjectId,
        const void* inheritanceInfo,
        bool fromScript);

    // Function pointer for ActionControllerImpl::StateRadio.
    // Params: self, slot, proc
    using StateRadio_t = void(__fastcall*)(void* self, std::uint32_t slot, int proc);

    // Function pointer for RadioSpeechHandlerImpl::CallWithRadioType.
    // Params: self, outHandle, ownerIndex, radioType, arg5
    using CallWithRadioType_t = short* (__fastcall*)(
        void* self,
        short* outHandle,
        std::uint32_t ownerIndex,
        std::uint8_t radioType,
        std::uint16_t arg5);

    // Function pointer for RadioSpeechHandlerImpl::CallImpl.
    // Params: selfMinus20, outHandle, ownerIndex, speechLabel, arg5
    using CallImpl_t = short* (__fastcall*)(
        long long selfMinus20,
        short* outHandle,
        int ownerIndex,
        std::uint32_t speechLabel,
        std::uint16_t arg5);

    static RequestCorpse_t g_OrigRequestCorpse = nullptr;
    static StateRadio_t g_OrigStateRadio = nullptr;
    static CallWithRadioType_t g_OrigCallWithRadioType = nullptr;
    static CallImpl_t g_CallImpl = nullptr;

    static std::mutex g_StateMutex;

    // Registered important targets.
    static std::unordered_map<std::uint32_t, ImportantTargetInfo> g_ImportantByGameObjectId;
    static std::unordered_map<std::uint16_t, ImportantTargetInfo> g_ImportantBySoldierIndex;

    // Exact important bodies that were actually discovered and should override the next body-found radio(s).
    static std::deque<ImportantTargetInfo> g_DiscoveredImportantBodyQueue;

    // Prevent repeated queueing of the same discovered important body.
    static std::unordered_set<std::uint64_t> g_SeenDiscoveredImportantBodies;

    // The currently armed override for the next body-found radio call.
    static PendingBodyFoundOverride g_PendingBodyFoundOverride;

    // Converts a game object id to the soldier index used by the gameplay systems.
    // For your soldiers: 0x00000408 -> 0x0008, 0x00000411 -> 0x0011.
    // Params: gameObjectId (32-bit game object id)
    static std::uint16_t GameObjectIdToSoldierIndex(std::uint32_t gameObjectId)
    {
        const std::uint16_t low8 = static_cast<std::uint16_t>(gameObjectId & 0x00FFu);
        if (low8 != 0)
            return low8;

        return static_cast<std::uint16_t>(gameObjectId & 0xFFFFu);
    }

    // Returns a readable YES/NO string for logging.
    // Params: value (boolean flag)
    static const char* YesNo(bool value)
    {
        return value ? "YES" : "NO";
    }

    // Normalizes a soldier index so broken values like 0x0408 do not poison the lookup tables.
    // Params: gameObjectId (full soldier game object id), soldierIndex (candidate soldier index)
    static std::uint16_t NormalizeSoldierIndex(std::uint32_t gameObjectId, std::uint16_t soldierIndex)
    {
        if (soldierIndex != 0 && soldierIndex <= 0x00FFu)
            return soldierIndex;

        return GameObjectIdToSoldierIndex(gameObjectId);
    }

    // Builds a stable key for de-duplicating discovered important bodies.
    // Params: gameObjectId, soldierIndex
    static std::uint64_t MakeBodyKey(std::uint32_t gameObjectId, std::uint16_t soldierIndex)
    {
        return (static_cast<std::uint64_t>(gameObjectId) << 32) | static_cast<std::uint64_t>(soldierIndex);
    }

    // Looks up an important target by gameObjectId first, then by normalized soldierIndex.
    // Params: gameObjectId, soldierIndex, outInfo (receives match on success)
    static bool FindImportantTarget(
        std::uint32_t gameObjectId,
        std::uint16_t soldierIndex,
        ImportantTargetInfo& outInfo)
    {
        if (gameObjectId != 0)
        {
            const auto byGameObject = g_ImportantByGameObjectId.find(gameObjectId);
            if (byGameObject != g_ImportantByGameObjectId.end())
            {
                outInfo = byGameObject->second;
                return true;
            }
        }

        const std::uint16_t normalizedSoldierIndex = NormalizeSoldierIndex(gameObjectId, soldierIndex);
        if (normalizedSoldierIndex != 0)
        {
            const auto bySoldierIndex = g_ImportantBySoldierIndex.find(normalizedSoldierIndex);
            if (bySoldierIndex != g_ImportantBySoldierIndex.end())
            {
                outInfo = bySoldierIndex->second;
                return true;
            }
        }

        return false;
    }

    // Convenience overload when only a gameObjectId is available.
    // Params: gameObjectId, outInfo (receives match on success)
    static bool FindImportantTarget(std::uint32_t gameObjectId, ImportantTargetInfo& outInfo)
    {
        return FindImportantTarget(
            gameObjectId,
            static_cast<std::uint16_t>(gameObjectId & 0xFFFFu),
            outInfo);
    }

    // Reads the current radio type from ActionControllerImpl state.
    // Params: self (ActionControllerImpl*), slot (radio slot)
    static std::uint8_t ReadRadioType(void* self, std::uint32_t slot)
    {
        const auto selfBytes = reinterpret_cast<std::uint8_t*>(self);
        const auto entry88Base = *reinterpret_cast<std::uint8_t**>(selfBytes + 0x88);
        if (!entry88Base)
            return 0;

        return *(entry88Base + 8 + static_cast<std::size_t>(slot) * 0x0C);
    }

    // Clears runtime radio override state without taking the mutex.
    // Params: none
    static void ClearRuntimeRadioState_NoLock()
    {
        g_DiscoveredImportantBodyQueue.clear();
        g_SeenDiscoveredImportantBodies.clear();
        g_PendingBodyFoundOverride = {};
    }

    // Queues an actually discovered important body for the next body-found radio override.
    // Params: foundGameObjectId (0 if unknown), foundSoldierIndex (must be valid when gameObjectId is unknown)
    static bool QueueDiscoveredImportantBody(std::uint32_t foundGameObjectId, std::uint16_t foundSoldierIndex)
    {
        ImportantTargetInfo info{};
        if (!FindImportantTarget(foundGameObjectId, foundSoldierIndex, info))
        {
            Log(
                "[Radio] The found body wasn't the VIP's: gameObjectId=0x%08X soldierIndex=0x%04X\n",
                static_cast<unsigned int>(foundGameObjectId),
                static_cast<unsigned int>(foundSoldierIndex));
            return false;
        }

        const std::uint64_t bodyKey = MakeBodyKey(info.gameObjectId, info.soldierIndex);
        if (g_SeenDiscoveredImportantBodies.find(bodyKey) != g_SeenDiscoveredImportantBodies.end())
        {
            Log(
                "[Radio] Duplicate important body discovery ignored: gameObjectId=0x%08X soldierIndex=0x%04X officer=%s\n",
                static_cast<unsigned int>(info.gameObjectId),
                static_cast<unsigned int>(info.soldierIndex),
                YesNo(info.isOfficer));
            return false;
        }

        g_SeenDiscoveredImportantBodies.insert(bodyKey);
        g_DiscoveredImportantBodyQueue.push_back(info);

        Log(
            "[Radio] Queued discovered important body: gameObjectId=0x%08X soldierIndex=0x%04X officer=%s queueSize=%zu\n",
            static_cast<unsigned int>(info.gameObjectId),
            static_cast<unsigned int>(info.soldierIndex),
            YesNo(info.isOfficer),
            g_DiscoveredImportantBodyQueue.size());

        return true;
    }

    // Hook for CorpseManagerImpl::RequestCorpse.
    // This only logs corpse creation. It must NOT decide radio overrides.
    // Params: original function params from RequestCorpse
    static void __fastcall hkRequestCorpse(
        void* self,
        void* animControl,
        void* ragdollPlugin,
        void* facialPlugin,
        std::uint32_t* facialParam,
        const void* location,
        std::uint16_t originalGameObjectId,
        const void* inheritanceInfo,
        bool fromScript)
    {
        if (g_OrigRequestCorpse)
        {
            g_OrigRequestCorpse(
                self,
                animControl,
                ragdollPlugin,
                facialPlugin,
                facialParam,
                location,
                originalGameObjectId,
                inheritanceInfo,
                fromScript);
        }

        ImportantTargetInfo info{};
        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            if (FindImportantTarget(static_cast<std::uint32_t>(originalGameObjectId), info))
            {
                Log(
                    "[Radio] RequestCorpse originalGameObjectId=0x%04X soldierIndex=%u important=YES officer=%s fromScript=%s\n",
                    static_cast<unsigned int>(originalGameObjectId),
                    static_cast<unsigned int>(info.soldierIndex),
                    YesNo(info.isOfficer),
                    YesNo(fromScript));
                return;
            }
        }

        Log(
            "[Radio] RequestCorpse originalGameObjectId=0x%04X soldierIndex=%u important=NO officer=NO fromScript=%s\n",
            static_cast<unsigned int>(originalGameObjectId),
            static_cast<unsigned int>(GameObjectIdToSoldierIndex(static_cast<std::uint32_t>(originalGameObjectId))),
            YesNo(fromScript));
    }

    // Hook for ActionControllerImpl::StateRadio.
    // This only arms from already-verified discovered important bodies.
    // Params: self (ActionControllerImpl*), slot (radio slot), proc (state proc)
    static void __fastcall hkStateRadio(void* self, std::uint32_t slot, int proc)
    {
        if (proc == 0)
        {
            const std::uint8_t radioType = ReadRadioType(self, slot);

            std::lock_guard<std::mutex> lock(g_StateMutex);

            if (radioType == kRadioTypeBodyFound)
            {
                if (!g_PendingBodyFoundOverride.active && !g_DiscoveredImportantBodyQueue.empty())
                {
                    g_PendingBodyFoundOverride.active = true;
                    g_PendingBodyFoundOverride.target = g_DiscoveredImportantBodyQueue.front();

                    Log(
                        "[Radio] Armed body-found override: slot=%u gameObjectId=0x%08X soldierIndex=%u officer=%s queueSize=%zu\n",
                        static_cast<unsigned int>(slot),
                        static_cast<unsigned int>(g_PendingBodyFoundOverride.target.gameObjectId),
                        static_cast<unsigned int>(g_PendingBodyFoundOverride.target.soldierIndex),
                        YesNo(g_PendingBodyFoundOverride.target.isOfficer),
                        g_DiscoveredImportantBodyQueue.size());
                }
            }
            else
            {
                g_PendingBodyFoundOverride = {};
            }
        }

        if (g_OrigStateRadio)
        {
            g_OrigStateRadio(self, slot, proc);
        }
    }

    // Hook for RadioSpeechHandlerImpl::CallWithRadioType.
    // If the next body-found radio belongs to a discovered important body, this swaps the line.
    // Params: self (RadioSpeechHandlerImpl*), outHandle, ownerIndex, radioType, arg5
    static short* __fastcall hkCallWithRadioType(
        void* self,
        short* outHandle,
        std::uint32_t ownerIndex,
        std::uint8_t radioType,
        std::uint16_t arg5)
    {
        PendingBodyFoundOverride pending{};
        bool shouldConsumeQueueFront = false;

        {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            if (radioType == kRadioTypeBodyFound && g_PendingBodyFoundOverride.active)
            {
                pending = g_PendingBodyFoundOverride;
                g_PendingBodyFoundOverride = {};
                shouldConsumeQueueFront = !g_DiscoveredImportantBodyQueue.empty();
            }
        }

        if (radioType == kRadioTypeBodyFound && pending.active)
        {
            if (pending.target.isOfficer)
            {
                Log(
                    "[Radio] Officer body-found override: owner=%u arg5=0x%04X gameObjectId=0x%08X soldierIndex=%u speechLabel=0x%08X\n",
                    static_cast<unsigned int>(ownerIndex),
                    static_cast<unsigned int>(arg5),
                    static_cast<unsigned int>(pending.target.gameObjectId),
                    static_cast<unsigned int>(pending.target.soldierIndex),
                    static_cast<unsigned int>(kOfficerBodyFoundSpeechLabel));

                if (shouldConsumeQueueFront)
                {
                    std::lock_guard<std::mutex> lock(g_StateMutex);
                    if (!g_DiscoveredImportantBodyQueue.empty())
                        g_DiscoveredImportantBodyQueue.pop_front();
                }

                if (g_CallImpl)
                {
                    return g_CallImpl(
                        reinterpret_cast<long long>(self) - 0x20ll,
                        outHandle,
                        static_cast<int>(ownerIndex),
                        kOfficerBodyFoundSpeechLabel,
                        arg5);
                }
            }
            else
            {
                Log(
                    "[Radio] VIP body-found override: owner=%u arg5=0x%04X gameObjectId=0x%08X soldierIndex=%u radioType=0x%02X\n",
                    static_cast<unsigned int>(ownerIndex),
                    static_cast<unsigned int>(arg5),
                    static_cast<unsigned int>(pending.target.gameObjectId),
                    static_cast<unsigned int>(pending.target.soldierIndex),
                    static_cast<unsigned int>(kRadioTypeVipBodyFound));

                if (shouldConsumeQueueFront)
                {
                    std::lock_guard<std::mutex> lock(g_StateMutex);
                    if (!g_DiscoveredImportantBodyQueue.empty())
                        g_DiscoveredImportantBodyQueue.pop_front();
                }

                if (g_OrigCallWithRadioType)
                {
                    return g_OrigCallWithRadioType(
                        self,
                        outHandle,
                        ownerIndex,
                        kRadioTypeVipBodyFound,
                        arg5);
                }
            }
        }

        if (g_OrigCallWithRadioType)
        {
            return g_OrigCallWithRadioType(self, outHandle, ownerIndex, radioType, arg5);
        }

        return outHandle;
    }
}

// Adds one important target for the radio module using both gameObjectId and soldierIndex.
// Params: gameObjectId (soldier game object id), soldierIndex (real soldier index), isOfficer
void Add_VIPRadioImportantTarget(std::uint32_t gameObjectId, std::uint16_t soldierIndex, bool isOfficer)
{
    const ImportantTargetInfo info{
        gameObjectId,
        NormalizeSoldierIndex(gameObjectId, soldierIndex),
        isOfficer
    };

    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_ImportantByGameObjectId[info.gameObjectId] = info;
    g_ImportantBySoldierIndex[info.soldierIndex] = info;

    Log(
        "[Radio] Added important target: gameObjectId=0x%08X soldierIndex=0x%04X officer=%s\n",
        static_cast<unsigned int>(info.gameObjectId),
        static_cast<unsigned int>(info.soldierIndex),
        YesNo(info.isOfficer));
}

// Adds one important target for the radio module using only the gameObjectId.
// Params: gameObjectId (soldier game object id), isOfficer
void Add_VIPRadioImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer)
{
    Add_VIPRadioImportantTarget(
        gameObjectId,
        GameObjectIdToSoldierIndex(gameObjectId),
        isOfficer);
}

// Removes one important target from the radio module.
// Params: gameObjectId (soldier game object id)
void Remove_VIPRadioImportantGameObjectId(std::uint32_t gameObjectId)
{
    std::lock_guard<std::mutex> lock(g_StateMutex);

    const std::uint16_t soldierIndex = GameObjectIdToSoldierIndex(gameObjectId);
    g_ImportantByGameObjectId.erase(gameObjectId);
    g_ImportantBySoldierIndex.erase(soldierIndex);

    Log(
        "[Radio] Removed important target: gameObjectId=0x%08X soldierIndex=%u\n",
        static_cast<unsigned int>(gameObjectId),
        static_cast<unsigned int>(soldierIndex));
}

// Call this from the actual "body discovered / body reported" hook when you know the discovered body's gameObjectId.
// Params: foundGameObjectId (discovered body's original soldier game object id if available)
bool Notify_VIPRadioBodyDiscovered(std::uint32_t foundGameObjectId)
{
    std::lock_guard<std::mutex> lock(g_StateMutex);
    return QueueDiscoveredImportantBody(
        foundGameObjectId,
        GameObjectIdToSoldierIndex(foundGameObjectId));
}

// Call this from the actual "body discovered / body reported" hook when you know both ids.
// Params: foundGameObjectId, foundSoldierIndex
bool Notify_VIPRadioBodyDiscoveredTarget(std::uint32_t foundGameObjectId, std::uint16_t foundSoldierIndex)
{
    std::lock_guard<std::mutex> lock(g_StateMutex);
    return QueueDiscoveredImportantBody(foundGameObjectId, foundSoldierIndex);
}

// Clears all important targets and radio runtime state.
// Params: none
void Clear_VIPRadioImportantGameObjectIds()
{
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_ImportantByGameObjectId.clear();
    g_ImportantBySoldierIndex.clear();
    ClearRuntimeRadioState_NoLock();

    Log("[Radio] Cleared important targets, discovered-body queue, duplicate cache, and pending radio state\n");
}

// Installs the VIP radio hooks.
// Params: none
bool Install_VIPRadio_Hook()
{
    g_CallImpl = reinterpret_cast<CallImpl_t>(ResolveGameAddress(ABS_CallImpl));

    void* requestCorpseTarget = ResolveGameAddress(ABS_RequestCorpse);
    void* stateRadioTarget = ResolveGameAddress(ABS_StateRadio);
    void* callWithRadioTypeTarget = ResolveGameAddress(ABS_CallWithRadioType);

    if (!g_CallImpl || !requestCorpseTarget || !stateRadioTarget || !callWithRadioTypeTarget)
    {
        Log("[Hook] VIPRadio: address resolve failed\n");
        return false;
    }

    bool ok = true;

    ok = ok && CreateAndEnableHook(
        requestCorpseTarget,
        reinterpret_cast<void*>(&hkRequestCorpse),
        reinterpret_cast<void**>(&g_OrigRequestCorpse));

    ok = ok && CreateAndEnableHook(
        stateRadioTarget,
        reinterpret_cast<void*>(&hkStateRadio),
        reinterpret_cast<void**>(&g_OrigStateRadio));

    ok = ok && CreateAndEnableHook(
        callWithRadioTypeTarget,
        reinterpret_cast<void*>(&hkCallWithRadioType),
        reinterpret_cast<void**>(&g_OrigCallWithRadioType));

    Log("[Hook] VIPRadio: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Uninstalls the VIP radio hooks and clears runtime state.
// Params: none
bool Uninstall_VIPRadio_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_RequestCorpse));
    DisableAndRemoveHook(ResolveGameAddress(ABS_StateRadio));
    DisableAndRemoveHook(ResolveGameAddress(ABS_CallWithRadioType));

    g_OrigRequestCorpse = nullptr;
    g_OrigStateRadio = nullptr;
    g_OrigCallWithRadioType = nullptr;
    g_CallImpl = nullptr;

    Clear_VIPRadioImportantGameObjectIds();
    return true;
}