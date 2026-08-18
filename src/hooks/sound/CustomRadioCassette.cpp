#include "pch.h"
#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "HookUtils.h"
#include "log.h"
#include "AddressSet.h"
#include "CustomRadioCassette.h"
#include "CustomTapeOwnership.h"

namespace
{
    using SearchCasseteInfo_t    = int          (__fastcall*)(void* thisPtr, std::uint64_t key);
    using GetCassetteMusic_t     = std::uint32_t (__fastcall*)(void* thisPtr, int slot);
    using IsGotCassette_t        = bool          (__fastcall*)(void* thisPtr, int slot, std::int16_t* outSaveIndex);
    using GetCassetteSaveIndex_t = bool          (__fastcall*)(void* thisPtr, int slot, std::int16_t* outSaveIndex);

    using GetTrackInfoByName_t   = void* (__fastcall*)(void* soundMusicPlayer, std::int32_t trackNameStrCode);

    using SdPostEvent_t = void* (__fastcall*)(void* thisPtr, void* errOut, std::uint32_t* controlPtr, std::uint32_t eventId, void* pos);

    using RadioUpdate_t = void (__fastcall*)(void* thisPtr);

    using RadioActivateUnit_t = void (__fastcall*)(void* unit);

    using IsSameSaveIndexFromName_t = bool (__fastcall*)(void* thisPtr, std::uint64_t key, std::int16_t saveIndex);

    static SearchCasseteInfo_t    g_OrigSearchCasseteInfo    = nullptr;
    static GetCassetteMusic_t     g_OrigGetCassetteMusic     = nullptr;
    static IsGotCassette_t        g_OrigIsGotCassette        = nullptr;
    static GetCassetteSaveIndex_t g_OrigGetCassetteSaveIndex = nullptr;
    static SdPostEvent_t          g_OrigSdPostEvent          = nullptr;
    static RadioUpdate_t          g_OrigRadioUpdate          = nullptr;
    static RadioActivateUnit_t    g_ActivateRadioUnit        = nullptr;
    static IsSameSaveIndexFromName_t g_OrigIsSameSaveIndexFromName = nullptr;

    constexpr int         kSyntheticSlotBase     = 0x100;
    constexpr std::size_t kMaxCustomRadioCassette = 30000;

    constexpr std::int16_t kSaveIndexUnresolved = -2;

    struct CustomRadioCassetteEntry
    {
        std::uint32_t nameHash     = 0;
        std::uint32_t fox2PathHash = 0;
        std::uint32_t wwiseEventId = 0;
        std::uint32_t trackNameId  = 0;
        std::string   fileName;
        std::int16_t  resolvedSaveIndex = kSaveIndexUnresolved;
    };

    static std::mutex                            g_Mutex;
    static std::vector<CustomRadioCassetteEntry> g_Entries;

    static std::unordered_map<int, bool>     g_LastOwnedBySlot;
    static std::unordered_set<void*>         g_ActivatedUnits;
}

static std::int16_t ResolveSaveIndexFromTrackName(std::uint32_t trackNameId)
{
    if (trackNameId == 0)
        return -1;

    void* fnAddr   = ResolveGameAddress(gAddr.GetTrackInfoByName);
    void* mmGlobal = ResolveGameAddress(gAddr.MusicManager_s_instance);
    if (!fnAddr || !mmGlobal)
        return -1;

    GetTrackInfoByName_t GetTrackInfoByName = reinterpret_cast<GetTrackInfoByName_t>(fnAddr);

    __try
    {
        void* musicManager = *reinterpret_cast<void**>(mmGlobal);
        if (!musicManager)
            return -1;

        void* soundMusicPlayer =
            *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(musicManager) + 0xA8ull);
        if (!soundMusicPlayer)
            return -1;

        void* trackInfo = GetTrackInfoByName(soundMusicPlayer, static_cast<std::int32_t>(trackNameId));
        if (!trackInfo)
            return -1;

        return *reinterpret_cast<std::int16_t*>(reinterpret_cast<std::uintptr_t>(trackInfo) + 0x1Cull);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}


static std::int16_t ResolveSlotSaveIndex(int slot)
{
    const std::size_t idx = static_cast<std::size_t>(slot - kSyntheticSlotBase);

    std::uint32_t trackNameId = 0;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (idx >= g_Entries.size())
            return -1;
        if (g_Entries[idx].resolvedSaveIndex >= 0)
            return g_Entries[idx].resolvedSaveIndex;
        trackNameId = g_Entries[idx].trackNameId;
    }

    const std::int16_t saveIndex = ResolveSaveIndexFromTrackName(trackNameId);

    if (saveIndex >= 0)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (idx < g_Entries.size())
            g_Entries[idx].resolvedSaveIndex = saveIndex;
    }

    return saveIndex;
}


bool Register_CustomRadioCassette(
    std::uint32_t gimmickNameHash,
    std::uint32_t fox2PathHash,
    std::uint32_t wwiseEventId,
    std::uint32_t trackNameId,
    const char* fileName)
{
    if (gimmickNameHash == 0 || wwiseEventId == 0)
        return false;

    std::lock_guard<std::mutex> lock(g_Mutex);

    for (auto& existing : g_Entries)
    {
        if (existing.nameHash == gimmickNameHash && existing.fox2PathHash == fox2PathHash)
        {
            existing.wwiseEventId      = wwiseEventId;
            existing.trackNameId       = trackNameId;
            existing.fileName          = fileName ? fileName : "";
            existing.resolvedSaveIndex = kSaveIndexUnresolved;
            return true;
        }
    }

    if (g_Entries.size() >= kMaxCustomRadioCassette)
    {
        LogDebug("[RadioCassette] register refused: %zu cassettes reached the "
            "int16 save-index ceiling (%zu)\n",
            g_Entries.size(), kMaxCustomRadioCassette);
        return false;
    }

    CustomRadioCassetteEntry entry;
    entry.nameHash     = gimmickNameHash;
    entry.fox2PathHash = fox2PathHash;
    entry.wwiseEventId = wwiseEventId;
    entry.trackNameId  = trackNameId;
    entry.fileName     = fileName ? fileName : "";
    g_Entries.push_back(entry);
    return true;
}

static bool EntryMatchesKey(const CustomRadioCassetteEntry& e, std::uint64_t key)
{
    if (e.fox2PathHash != 0)
        return ((static_cast<std::uint64_t>(e.fox2PathHash) << 32) | e.nameHash) == key;
    return e.nameHash == static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
}

static int __fastcall hkSearchCasseteInfo(void* thisPtr, std::uint64_t key)
{
    if (g_OrigSearchCasseteInfo)
    {
        const int original = g_OrigSearchCasseteInfo(thisPtr, key);
        if (original >= 0)
            return original;
    }

    const std::uint32_t incomingNameHash = static_cast<std::uint32_t>(key & 0xFFFFFFFFu);

    std::lock_guard<std::mutex> lock(g_Mutex);
    for (std::size_t i = 0; i < g_Entries.size(); ++i)
    {
        if (EntryMatchesKey(g_Entries[i], key))
        {
            const int slot = kSyntheticSlotBase + static_cast<int>(i);
            return slot;
        }
    }

    return -1;
}

static std::uint32_t __fastcall hkGetCassetteMusic(void* thisPtr, int slot)
{
    if (slot >= kSyntheticSlotBase)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const std::size_t idx = static_cast<std::size_t>(slot - kSyntheticSlotBase);
        if (idx < g_Entries.size())
        {
            return g_Entries[idx].wwiseEventId;
        }
        return 0;
    }

    return g_OrigGetCassetteMusic ? g_OrigGetCassetteMusic(thisPtr, slot) : 0;
}

static bool __fastcall hkGetCassetteSaveIndex(void* thisPtr, int slot, std::int16_t* outSaveIndex)
{
    if (slot >= kSyntheticSlotBase)
    {
        const std::int16_t saveIndex = ResolveSlotSaveIndex(slot);
        if (outSaveIndex)
            *outSaveIndex = saveIndex;

        return saveIndex >= 0;
    }

    return g_OrigGetCassetteSaveIndex ? g_OrigGetCassetteSaveIndex(thisPtr, slot, outSaveIndex) : false;
}

static bool __fastcall hkIsGotCassette(void* thisPtr, int slot, std::int16_t* outSaveIndex)
{
    if (slot >= kSyntheticSlotBase)
    {
        const std::int16_t saveIndex = ResolveSlotSaveIndex(slot);
        if (outSaveIndex)
            *outSaveIndex = saveIndex;

        return (saveIndex >= 0) && IsCustomTapeOwnedSaveIndex(saveIndex);
    }

    return g_OrigIsGotCassette ? g_OrigIsGotCassette(thisPtr, slot, outSaveIndex) : false;
}

static void* __fastcall hkSdPostEvent(
    void* thisPtr, void* errOut, std::uint32_t* controlPtr, std::uint32_t eventId, void* pos)
{
    return g_OrigSdPostEvent
        ? g_OrigSdPostEvent(thisPtr, errOut, controlPtr, eventId, pos)
        : nullptr;
}

static bool __fastcall hkIsSameSaveIndexFromName(void* thisPtr, std::uint64_t key, std::int16_t saveIndex)
{
    int matchedSlot = -1;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (std::size_t i = 0; i < g_Entries.size(); ++i)
        {
            if (EntryMatchesKey(g_Entries[i], key))
            {
                matchedSlot = kSyntheticSlotBase + static_cast<int>(i);
                break;
            }
        }
    }

    if (matchedSlot < 0)
        return g_OrigIsSameSaveIndexFromName ? g_OrigIsSameSaveIndexFromName(thisPtr, key, saveIndex) : false;

    const std::int16_t ourSaveIndex = ResolveSlotSaveIndex(matchedSlot);
    const bool same = (ourSaveIndex >= 0) && (ourSaveIndex == saveIndex);

    return same;
}

static std::uint64_t ReadGimmickKey(void* thisPtr)
{
    __try { return *reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(thisPtr) + 0x60ull); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool ForceRadioStopState(void* thisPtr)
{
    __try
    {
        std::uint32_t* state = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(thisPtr) + 0x68ull);
        if ((*state & 0xFu) == 0u)
        {
            *state = (*state & 0xFFFFFFF0u) | 2u;
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static bool IsTakeEffectInProgress(void* thisPtr)
{
    __try
    {
        return *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(thisPtr) + 0x150ull) >= 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static std::uint32_t ReadMusicHandle(void* thisPtr)
{
    __try { return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(thisPtr) + 0x158ull); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void WriteMusicHandle(void* thisPtr, std::uint32_t handle)
{
    __try { *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(thisPtr) + 0x158ull) = handle; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static std::uint32_t ReadRadioNibble(void* thisPtr)
{
    __try { return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(thisPtr) + 0x68ull) & 0xFu; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

static int FindSlotForKey(std::uint64_t key)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    for (std::size_t i = 0; i < g_Entries.size(); ++i)
        if (EntryMatchesKey(g_Entries[i], key))
            return kSyntheticSlotBase + static_cast<int>(i);
    return -1;
}

static bool RadioUnitRealized(void* thisPtr)
{
    __try
    {
        return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(thisPtr) + 0xB8ull) != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void __fastcall hkRadioUpdate(void* thisPtr)
{
    bool          ourRadio     = false;
    int           ourSlot      = -1;
    std::int16_t  ourSaveIndex = -1;
    std::uint32_t handleBefore = 0;
    std::uint32_t nibbleBefore = 0xFFFFFFFFu;

    if (thisPtr)
    {
        const std::uint64_t key  = ReadGimmickKey(thisPtr);
        const int           slot = (key != 0) ? FindSlotForKey(key) : -1;
        if (slot >= 0)
        {
            const std::int16_t saveIndex = ResolveSlotSaveIndex(slot);

            const bool ownedPersist = (saveIndex >= 0) && IsCustomTapeOwnedSaveIndex(saveIndex);
            const bool ownedLive    = (saveIndex >= 0) && IsCustomTapeOwnedInLiveTable(saveIndex);
            const bool owned        = ownedPersist;

            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                auto it = g_LastOwnedBySlot.find(slot);
                if (it == g_LastOwnedBySlot.end() || it->second != owned)
                {
                    g_LastOwnedBySlot[slot] = owned;
                }
            }

             ourRadio     = true;
            ourSlot      = slot;
            ourSaveIndex = saveIndex;
            handleBefore = ReadMusicHandle(thisPtr);
            nibbleBefore = ReadRadioNibble(thisPtr);

            if (owned)
            {
                if (!IsTakeEffectInProgress(thisPtr) && ForceRadioStopState(thisPtr))
                {
                }
            }
            else if (g_ActivateRadioUnit)
            {
                if (!RadioUnitRealized(thisPtr))
                {
                    std::lock_guard<std::mutex> lock(g_Mutex);
                    g_ActivatedUnits.erase(thisPtr);
                }
                else
                {
                    bool firstTime = false;
                    {
                        std::lock_guard<std::mutex> lock(g_Mutex);
                        firstTime = g_ActivatedUnits.insert(thisPtr).second;
                    }

                    if (firstTime)
                    {
                        g_ActivateRadioUnit(thisPtr);
                    }
                }
            }
        }
    }

    if (g_OrigRadioUpdate)
        g_OrigRadioUpdate(thisPtr);

    if (ourRadio)
    {
        const std::uint32_t nibbleAfter = ReadRadioNibble(thisPtr);
        if (nibbleBefore == 0 && (nibbleAfter == 1 || nibbleAfter == 2)
            && handleBefore != 0 && ReadMusicHandle(thisPtr) == 0)
        {
            WriteMusicHandle(thisPtr, handleBefore);
        }
    }
}


bool Install_CustomRadioCassette_Hooks()
{
    void* searchTarget  = ResolveGameAddress(gAddr.RadioCassette_SearchCasseteInfo);
    void* musicTarget   = ResolveGameAddress(gAddr.RadioCassette_GetCassetteMusic);
    void* saveIdxTarget = ResolveGameAddress(gAddr.RadioCassette_GetCassetteSaveIndex);
    void* isGotTarget   = ResolveGameAddress(gAddr.RadioCassette_IsGotCassette);

    if (!searchTarget || !musicTarget)
    {
        Log("[RadioCassette] ERROR: core addresses unavailable for this build (search=%p music=%p) - custom radio cassettes are disabled.\n",
            searchTarget, musicTarget);
        return false;
    }

    bool ok = CreateAndEnableHook(
        searchTarget,
        reinterpret_cast<void*>(&hkSearchCasseteInfo),
        reinterpret_cast<void**>(&g_OrigSearchCasseteInfo));

    ok = CreateAndEnableHook(
        musicTarget,
        reinterpret_cast<void*>(&hkGetCassetteMusic),
        reinterpret_cast<void**>(&g_OrigGetCassetteMusic)) && ok;

    if (saveIdxTarget)
    {
        if (!CreateAndEnableHook(
                saveIdxTarget,
                reinterpret_cast<void*>(&hkGetCassetteSaveIndex),
                reinterpret_cast<void**>(&g_OrigGetCassetteSaveIndex)))
            Log("[RadioCassette] ERROR: failed to hook GetCassetteSaveIndex - custom radio cassettes may stay silent.\n");
    }
    else
    {
        Log("[RadioCassette] ERROR: GetCassetteSaveIndex address unavailable for this build - custom radio cassettes may stay silent.\n");
    }

    if (isGotTarget)
    {
        if (!CreateAndEnableHook(
                isGotTarget,
                reinterpret_cast<void*>(&hkIsGotCassette),
                reinterpret_cast<void**>(&g_OrigIsGotCassette)))
            Log("[RadioCassette] ERROR: failed to hook IsGotCassette - custom radio cassettes may stay silent.\n");
    }

    void* postTarget = ResolveGameAddress(gAddr.RadioCassette_SdPostEvent);
    if (postTarget)
    {
        if (!CreateAndEnableHook(
                postTarget,
                reinterpret_cast<void*>(&hkSdPostEvent),
                reinterpret_cast<void**>(&g_OrigSdPostEvent)))
            Log("[RadioCassette] WARN: failed to hook SdPostEvent (diagnostic only) - no functional impact on custom radio cassettes.\n");
    }

    void* radioUpdateTarget = ResolveGameAddress(gAddr.RadioCassette_RadioUpdate);
    if (radioUpdateTarget)
    {
        if (!CreateAndEnableHook(
                radioUpdateTarget,
                reinterpret_cast<void*>(&hkRadioUpdate),
                reinterpret_cast<void**>(&g_OrigRadioUpdate)))
            Log("[RadioCassette] ERROR: failed to hook radio Update - custom radio cassettes will not be ownership-gated.\n");
    }
    else
    {
        Log("[RadioCassette] ERROR: radio Update address unavailable for this build - custom radio cassettes will not be ownership-gated.\n");
    }

    void* sameSaveIdxTarget = ResolveGameAddress(gAddr.RadioCassette_IsSameSaveIndexFromName);
    if (sameSaveIdxTarget)
    {
        if (!CreateAndEnableHook(
                sameSaveIdxTarget,
                reinterpret_cast<void*>(&hkIsSameSaveIndexFromName),
                reinterpret_cast<void**>(&g_OrigIsSameSaveIndexFromName)))
            Log("[RadioCassette] ERROR: failed to hook IsSameSaveIndexFromName - taking a custom radio cassette will not work.\n");
    }
    else
    {
        Log("[RadioCassette] ERROR: IsSameSaveIndexFromName address unavailable for this build - taking a custom radio cassette will not work.\n");
    }

    g_ActivateRadioUnit = reinterpret_cast<RadioActivateUnit_t>(
        ResolveGameAddress(gAddr.RadioCassette_ActivateUnit));
    if (!g_ActivateRadioUnit)
        Log("[RadioCassette] WARN: activate-unit address unavailable for this build - custom radio cassettes will not auto-play.\n");

    if (!ok)
        Log("[RadioCassette] ERROR: one or more core radio-cassette hooks failed - custom radio cassettes are disabled.\n");
    return ok;
}


bool Uninstall_CustomRadioCassette_Hooks()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RadioCassette_SearchCasseteInfo));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RadioCassette_GetCassetteMusic));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RadioCassette_GetCassetteSaveIndex));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RadioCassette_IsGotCassette));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RadioCassette_SdPostEvent));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RadioCassette_RadioUpdate));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RadioCassette_IsSameSaveIndexFromName));

    g_OrigSearchCasseteInfo    = nullptr;
    g_OrigGetCassetteMusic     = nullptr;
    g_OrigGetCassetteSaveIndex = nullptr;
    g_OrigIsGotCassette        = nullptr;
    g_OrigSdPostEvent          = nullptr;
    g_OrigRadioUpdate          = nullptr;
    g_ActivateRadioUnit        = nullptr;
    g_OrigIsSameSaveIndexFromName = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_ActivatedUnits.clear();
    }
    return true;
}
