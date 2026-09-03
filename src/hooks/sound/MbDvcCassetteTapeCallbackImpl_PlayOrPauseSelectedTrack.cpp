#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "MbDvcCassetteTapeCallbackImpl_PlayOrPauseSelectedTrack.h"
#include "CustomTapeOwnership.h"
#include "SoundSystemImpl_BeginSoundSystem.h"
#include "CassetteWalkmanEvents.h"
#include "AddressSet.h"
#include "CassetteTrackPaging.h"

static int ReadU32FieldSEH(void* base, std::uintptr_t off, std::uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<std::uint32_t*>(
            static_cast<std::uint8_t*>(base) + off);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
#include <LuaBroadcaster.h>

namespace
{
    using CassetteStart_t = void(__fastcall*)(void* cassetteCallbackBase);

    using PlayOrPauseSelectedTrack_t = bool(__fastcall*)(void* cassetteCallbackBase);

    using MusicPlayerPlay_t = void(__fastcall*)(
        void* player,
        void* outHandle,
        std::uint32_t playMode,
        void* tapeIdTable,
        std::uint32_t albumIndex,
        std::uint32_t selectedTrackIndex,
        std::uint32_t reservedZero,
        std::uint8_t loopPlayFlag,
        std::uint8_t allOrOneFlag);

    using CassettePlayerGetState_t = int(__fastcall*)(void* player);

    using CassettePlayerSetSpeakerMode_t =
        std::uint32_t* (__fastcall*)(void* player, void* outResult, std::uint32_t targetMode);

    using CassettePlayerGetSpeakerMode_t = int(__fastcall*)(void* player);

    static constexpr std::size_t kMusicPlayerPlayVtableOffset = 0xF0ull;
    static constexpr std::size_t kCassettePlayerGetStateVtableOffset = 0xD8ull;
    static constexpr std::size_t kCassettePlayerSetSpeakerModeVtableOffset = 0x1A0ull;
    static constexpr std::size_t kCassettePlayerGetSpeakerModeVtableOffset = 0x1A8ull;

    static constexpr std::uint32_t kCassetteSpeakerModeDisabled = 0u;
    static constexpr std::uint32_t kCassetteSpeakerModeEnabled = 1u;

    static constexpr std::size_t kTapeIdTableSize = 0x200ull;

    static CassetteStart_t g_OrigCassetteStart = nullptr;
    static PlayOrPauseSelectedTrack_t g_OrigPlayOrPauseSelectedTrack = nullptr;
    static MusicPlayerPlay_t g_OrigMusicPlayerPlay = nullptr;

    static std::mutex g_CassettePlayHookMutex;

    static void* g_MusicPlayerPlayTarget = nullptr;
    static void* g_LastCassetteCallbackBase = nullptr;
    static void* g_LastMusicPlayer = nullptr;

    static std::uint8_t g_CachedTapeIdTable[kTapeIdTableSize] = {};
    static bool g_HasCopiedTapeIdTable = false;


    static std::uint32_t g_DirectTrackTable[0x80] = {};
}


static void __fastcall hkMusicPlayerPlay(
    void* player,
    void* outHandle,
    std::uint32_t playMode,
    void* tapeIdTable,
    std::uint32_t albumIndex,
    std::uint32_t selectedTrackIndex,
    std::uint32_t reservedZero,
    std::uint8_t flag1,
    std::uint8_t flag2);


static std::uint32_t ReadTapeIdTableEntry(const void* tapeIdTable, std::uint32_t index)
{
    if (!tapeIdTable || index >= 0x1000u)
        return 0;

    __try
    {
        return reinterpret_cast<const std::uint32_t*>(tapeIdTable)[index];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}


static void* ResolveMusicPlayerFromCassetteCallback(void* cassetteCallbackBase)
{
    if (!cassetteCallbackBase)
        return nullptr;

    __try
    {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(cassetteCallbackBase);
        const std::uintptr_t context = *reinterpret_cast<const std::uintptr_t*>(base + 0x30ull);
        if (!context)
            return nullptr;

        const std::uintptr_t player = *reinterpret_cast<const std::uintptr_t*>(context + 0xD8ull);
        if (!player)
            return nullptr;

        return reinterpret_cast<void*>(player);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}


static bool IsModuleImagePtr(const void* p, bool requireExec)
{
    if (!p)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    if (requireExec)
    {
        const DWORD exec =
            PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & exec) == 0)
            return false;
    }
    return true;
}


static void* ResolveMusicPlayerPlayTarget(void* musicPlayer)
{
    if (!musicPlayer)
        return nullptr;

    __try
    {
        const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(musicPlayer);
        if (!vtable)
            return nullptr;

        if (!IsModuleImagePtr(reinterpret_cast<const void*>(vtable), false))
            return nullptr;

        const std::uintptr_t target =
            *reinterpret_cast<const std::uintptr_t*>(vtable + kMusicPlayerPlayVtableOffset);
        if (!target)
            return nullptr;

        if (!IsModuleImagePtr(reinterpret_cast<const void*>(target), true))
            return nullptr;

        return reinterpret_cast<void*>(target);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}


static bool TryCopyTapeIdTableFromCassetteCallback(void* cassetteCallbackBase, std::uint8_t* outBuffer)
{
    if (!cassetteCallbackBase || !outBuffer)
        return false;

    __try
    {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(cassetteCallbackBase);
        const void* source = reinterpret_cast<const void*>(base + 0xD80ull);

        std::memcpy(outBuffer, source, kTapeIdTableSize);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static void CacheTapeIdTableFromCassetteCallback(void* cassetteCallbackBase)
{
    if (!cassetteCallbackBase)
        return;

    std::uint8_t tempBuffer[kTapeIdTableSize] = {};
    const bool ok = TryCopyTapeIdTableFromCassetteCallback(cassetteCallbackBase, tempBuffer);

    if (!ok)
    {
        Log("[CassettePlay] WARN: could not read the cassette tape-id table - cassette track selection may be wrong this session.\n");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);

        std::memcpy(g_CachedTapeIdTable, tempBuffer, kTapeIdTableSize);
        g_HasCopiedTapeIdTable = true;
    }
}


static void InstallMusicPlayerPlayHookOnTarget(void* targetPtr)
{
    if (!targetPtr)
        return;

    std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);

    if (g_MusicPlayerPlayTarget != nullptr)
        return;

    const bool ok = CreateAndEnableHook(
        targetPtr,
        reinterpret_cast<void*>(&hkMusicPlayerPlay),
        reinterpret_cast<void**>(&g_OrigMusicPlayerPlay));

    if (!ok)
    {
        Log("[CassettePlay] ERROR: failed to hook the music player's play function (target=%p) - direct cassette playback will not work.\n", targetPtr);
        return;
    }

    g_MusicPlayerPlayTarget = targetPtr;
}


static void TryInstallMusicPlayerPlayHook(void* musicPlayer)
{
    if (!musicPlayer)
        return;

    void* targetPtr = ResolveMusicPlayerPlayTarget(musicPlayer);
    if (!targetPtr)
    {
        Log("[CassettePlay] WARN: could not resolve the music player's play function from its vtable - direct cassette playback unavailable until the Music Player screen is opened.\n");
        return;
    }

    InstallMusicPlayerPlayHookOnTarget(targetPtr);
}


static void __fastcall hkMusicPlayerPlay(
    void* player,
    void* outHandle,
    std::uint32_t playMode,
    void* tapeIdTable,
    std::uint32_t albumIndex,
    std::uint32_t selectedTrackIndex,
    std::uint32_t reservedZero,
    std::uint8_t flag1,
    std::uint8_t flag2)
{
    {
        std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);
        g_LastMusicPlayer = player;
    }

    if (g_OrigMusicPlayerPlay)
    {
        g_OrigMusicPlayerPlay(
            player,
            outHandle,
            playMode,
            tapeIdTable,
            albumIndex,
            selectedTrackIndex,
            reservedZero,
            flag1,
            flag2);
    }

    if (!MissionCodeGuard::ShouldBypassHooks())
    {
        const std::uint32_t playedTrackId = ReadTapeIdTableEntry(tapeIdTable, selectedTrackIndex);
        if (playedTrackId != 0 && !IsPagerSentinelId(playedTrackId))
            OnCassetteTrackPlayedByTrackId(playedTrackId);

        if (!IsPagerSentinelId(playedTrackId))
            Emit_CassetteWalkmanStart(playedTrackId);
    }
}

static void __fastcall hkCassetteStart(void* cassetteCallbackBase)
{
    if (!g_OrigCassetteStart)
    {
        Log("[CassettePlay] ERROR: Cassette Start trampoline is null - the hook failed to install; cassette playback control is broken.\n");
        return;
    }

    g_OrigCassetteStart(cassetteCallbackBase);

    const bool bypass = MissionCodeGuard::ShouldBypassHooks();

    if (bypass)
        return;

    {
        std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);
        g_LastCassetteCallbackBase = cassetteCallbackBase;
    }

    CacheTapeIdTableFromCassetteCallback(cassetteCallbackBase);
}

static bool __fastcall hkPlayOrPauseSelectedTrack(void* cassetteCallbackBase)
{
    if (!g_OrigPlayOrPauseSelectedTrack)
    {
        Log("[CassettePlay] ERROR: PlayOrPauseSelectedTrack trampoline is null - the hook failed to install; cassette playback control is broken.\n");
        return false;
    }

    const bool bypass = MissionCodeGuard::ShouldBypassHooks();

    if (bypass)
        return g_OrigPlayOrPauseSelectedTrack(cassetteCallbackBase);

    {
        std::uint32_t selIdx = 0;
        if (ReadU32FieldSEH(cassetteCallbackBase, 0xF84ull, &selIdx))
        {
            const int dir = GetPagerActionForSlot(cassetteCallbackBase, selIdx);
            if (dir != 0)
            {
                FlipCassettePage(cassetteCallbackBase, dir);
                return true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);
        g_LastCassetteCallbackBase = cassetteCallbackBase;
    }

    CacheTapeIdTableFromCassetteCallback(cassetteCallbackBase);

    void* musicPlayer = ResolveMusicPlayerFromCassetteCallback(cassetteCallbackBase);
    if (musicPlayer)
    {
        TryInstallMusicPlayerPlayHook(musicPlayer);
    }
    else
    {
        Log("[CassettePlay] WARN: could not resolve the music player from the cassette callback (callback=%p) - direct cassette playback may be unavailable.\n", cassetteCallbackBase);
    }

    return g_OrigPlayOrPauseSelectedTrack(cassetteCallbackBase);
}


static void* ResolveCassettePlayerVtableFunction(void* player, std::size_t vtableOffset)
{
    if (!player)
        return nullptr;

    __try
    {
        const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(player);
        if (!vtable)
            return nullptr;

        const std::uintptr_t target =
            *reinterpret_cast<const std::uintptr_t*>(vtable + vtableOffset);
        if (!target)
            return nullptr;

        return reinterpret_cast<void*>(target);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}


static bool ResolveCachedCassetteCallbackAndPlayer(void*& outCallbackBase, void*& outPlayer)
{
    outCallbackBase = nullptr;
    outPlayer = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);
        outCallbackBase = g_LastCassetteCallbackBase;
    }

    if (!outCallbackBase)
        return false;

    outPlayer = ResolveMusicPlayerFromCassetteCallback(outCallbackBase);
    return outPlayer != nullptr;
}


static bool CanSwitchCassetteSpeakerMode(void* player)
{
    if (!player)
        return false;

    void* fnAddr = ResolveCassettePlayerVtableFunction(player, kCassettePlayerGetStateVtableOffset);
    if (!fnAddr)
        return false;

    CassettePlayerGetState_t getState =
        reinterpret_cast<CassettePlayerGetState_t>(fnAddr);

    __try
    {
        const int state = getState(player);
        return static_cast<unsigned int>(state - 4) >= 2u;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


bool IsCassetteSpeakerEnabled(bool& outEnabled)
{
    outEnabled = false;

    if (MissionCodeGuard::ShouldBypassHooks())
        return false;

    void* callbackBase = nullptr;
    void* player = nullptr;
    if (!ResolveCachedCassetteCallbackAndPlayer(callbackBase, player))
    {
        Log("[CassetteSpeaker] WARN: cassette player not captured yet - open the in-game Music Player once before reading speaker mode.\n");
        return false;
    }

    if (!CanSwitchCassetteSpeakerMode(player))
    {
        Log("[CassetteSpeaker] WARN: cassette player state does not allow speaker switching right now (callback=%p player=%p) - try while a tape is playing.\n", callbackBase, player);
        return false;
    }

    void* fnAddr = ResolveCassettePlayerVtableFunction(player, kCassettePlayerGetSpeakerModeVtableOffset);
    if (!fnAddr)
    {
        Log("[CassetteSpeaker] ERROR: could not resolve the speaker-mode getter from the player vtable - speaker toggle will not work on this build.\n");
        return false;
    }

    CassettePlayerGetSpeakerMode_t getSpeakerMode =
        reinterpret_cast<CassettePlayerGetSpeakerMode_t>(fnAddr);

    __try
    {
        const int currentMode = getSpeakerMode(player);
        if (currentMode != static_cast<int>(kCassetteSpeakerModeDisabled) &&
            currentMode != static_cast<int>(kCassetteSpeakerModeEnabled))
        {
            Log("[CassetteSpeaker] WARN: speaker getter returned unexpected mode=%d (callback=%p player=%p) - cannot report speaker state.\n", currentMode, callbackBase, player);
            return false;
        }

        outEnabled = currentMode == static_cast<int>(kCassetteSpeakerModeEnabled);

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[CassetteSpeaker] ERROR: exception while reading speaker mode - speaker toggle may be unsupported on this build.\n");
        return false;
    }
}


bool SetCassetteSpeakerEnabled(bool enabled)
{
    if (MissionCodeGuard::ShouldBypassHooks())
        return false;

    void* callbackBase = nullptr;
    void* player = nullptr;
    if (!ResolveCachedCassetteCallbackAndPlayer(callbackBase, player))
    {
        Log("[CassetteSpeaker] WARN: cassette player not captured yet - open the in-game Music Player once before changing speaker mode.\n");
        return false;
    }

    if (!CanSwitchCassetteSpeakerMode(player))
    {
        Log("[CassetteSpeaker] WARN: cassette player state does not allow speaker switching right now (callback=%p player=%p) - try while a tape is playing.\n", callbackBase, player);
        return false;
    }

    bool currentEnabled = false;
    if (!IsCassetteSpeakerEnabled(currentEnabled))
        return false;

    if (currentEnabled == enabled)
    {
        return true;
    }

    void* fnAddr = ResolveCassettePlayerVtableFunction(player, kCassettePlayerSetSpeakerModeVtableOffset);
    if (!fnAddr)
    {
        Log("[CassetteSpeaker] ERROR: could not resolve the speaker-mode setter from the player vtable - speaker toggle will not work on this build.\n");
        return false;
    }

    CassettePlayerSetSpeakerMode_t setSpeakerMode =
        reinterpret_cast<CassettePlayerSetSpeakerMode_t>(fnAddr);

    std::uint8_t resultStorage[0x20] = {};

    __try
    {
        const std::uint32_t targetMode = enabled ? kCassetteSpeakerModeEnabled : kCassetteSpeakerModeDisabled;
        Set_CassetteWalkmanProgrammatic(true);
        std::uint32_t* result = setSpeakerMode(player, resultStorage, targetMode);
        Set_CassetteWalkmanProgrammatic(false);
        if (!result)
        {
            Log("[CassetteSpeaker] WARN: speaker-mode setter returned null (enabled=%d) - speaker mode was not changed.\n", enabled ? 1 : 0);
            return false;
        }

        const bool ok = ((*result >> 31) & 1u) == 0;

        return ok;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Set_CassetteWalkmanProgrammatic(false);
        Log("[CassetteSpeaker] ERROR: exception while setting speaker mode (enabled=%d) - speaker toggle may be unsupported on this build.\n", enabled ? 1 : 0);
        return false;
    }
}


static void* ResolveSoundMusicPlayerFromMusicManager()
{
    void* mmGlobal = ResolveGameAddress(gAddr.MusicManager_s_instance);
    if (!mmGlobal)
        return nullptr;

    __try
    {
        void* mm = *reinterpret_cast<void**>(mmGlobal);
        if (!mm)
            return nullptr;

        return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(mm) + 0xA8ull);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static void* ResolveStaticPlayWrapperFromVtable()
{
    void* vtbl = ResolveGameAddress(gAddr.CassettePlayerVtable);
    if (!vtbl || !IsModuleImagePtr(vtbl, false))
        return nullptr;

    __try
    {
        void* target = *reinterpret_cast<void**>(
            reinterpret_cast<std::uintptr_t>(vtbl) + kMusicPlayerPlayVtableOffset);
        if (target && IsModuleImagePtr(target, true))
            return target;
        return nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}


static bool ResolveDirectPlayState(MusicPlayerPlay_t& outPlayFn, void*& outPlayer)
{
    outPlayFn = nullptr;
    outPlayer = nullptr;

    void* callbackBase = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);
        outPlayFn = g_OrigMusicPlayerPlay;
        outPlayer = g_LastMusicPlayer;
        callbackBase = g_LastCassetteCallbackBase;
    }

    void* callbackPlayer = callbackBase ? ResolveMusicPlayerFromCassetteCallback(callbackBase) : nullptr;
    void* smp = ResolveSoundMusicPlayerFromMusicManager();

    if (!outPlayer)
        outPlayer = callbackPlayer;
    if (!outPlayer)
        outPlayer = smp;

    if (!outPlayFn && outPlayer)
    {
        void* target = ResolveMusicPlayerPlayTarget(outPlayer);
        if (target)
            outPlayFn = reinterpret_cast<MusicPlayerPlay_t>(target);
    }

    if (!outPlayFn)
    {
        void* staticWrapper = ResolveStaticPlayWrapperFromVtable();
        if (staticWrapper)
        {
            outPlayFn = reinterpret_cast<MusicPlayerPlay_t>(staticWrapper);
            if (!outPlayer)
                outPlayer = smp;
        }
    }

    if (outPlayFn == nullptr || outPlayer == nullptr)
    {
        Log("[CassettePlay] WARN: no play-capable cassette player resolved (player=%p playFn=%p) - open the in-game Music Player once so it gets captured.\n",
            outPlayer, reinterpret_cast<void*>(outPlayFn));
    }

    return (outPlayFn != nullptr && outPlayer != nullptr);
}


bool Install_MbDvcCassetteTapeCallbackImpl_PlayOrPauseSelectedTrack_Hook()
{
    void* startTarget = ResolveGameAddress(gAddr.CassetteStart);
    if (!startTarget)
    {
        Log("[CassettePlay] ERROR: Cassette Start address unavailable for this build - cassette playback control is disabled.\n");
        return false;
    }

    void* playTarget = ResolveGameAddress(gAddr.PlayOrPauseSelectedTrack);
    if (!playTarget)
    {
        Log("[CassettePlay] ERROR: PlayOrPauseSelectedTrack address unavailable for this build - cassette playback control is disabled.\n");
        return false;
    }

    const bool okStart = CreateAndEnableHook(
        startTarget,
        reinterpret_cast<void*>(&hkCassetteStart),
        reinterpret_cast<void**>(&g_OrigCassetteStart));

    const bool okPlay = CreateAndEnableHook(
        playTarget,
        reinterpret_cast<void*>(&hkPlayOrPauseSelectedTrack),
        reinterpret_cast<void**>(&g_OrigPlayOrPauseSelectedTrack));

    if (!okStart)
        Log("[CassettePlay] ERROR: failed to hook Cassette Start - cassette playback control will not work.\n");
    if (!okPlay)
        Log("[CassettePlay] ERROR: failed to hook PlayOrPauseSelectedTrack - cassette playback control will not work.\n");

    void* playWrapper = ResolveGameAddress(gAddr.MusicPlayerPlayWrapper);
    if (playWrapper)
        InstallMusicPlayerPlayHookOnTarget(playWrapper);

    return okStart && okPlay;
}


bool Uninstall_MbDvcCassetteTapeCallbackImpl_PlayOrPauseSelectedTrack_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.CassetteStart));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.PlayOrPauseSelectedTrack));

    g_OrigCassetteStart = nullptr;
    g_OrigPlayOrPauseSelectedTrack = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_CassettePlayHookMutex);

        if (g_MusicPlayerPlayTarget != nullptr)
        {
            DisableAndRemoveHook(g_MusicPlayerPlayTarget);
            g_MusicPlayerPlayTarget = nullptr;
        }

        g_OrigMusicPlayerPlay = nullptr;
        g_LastCassetteCallbackBase = nullptr;
        g_LastMusicPlayer = nullptr;

        std::memset(g_CachedTapeIdTable, 0, sizeof(g_CachedTapeIdTable));
        g_HasCopiedTapeIdTable = false;
        std::memset(g_DirectTrackTable, 0, sizeof(g_DirectTrackTable));
    }

    return true;
}


bool PlayCassetteByTrackId(
    std::uint32_t albumIndex,
    std::uint32_t trackId,
    bool loopPlay,
    bool playAll)
{
    if (MissionCodeGuard::ShouldBypassHooks())
        return false;

    MusicPlayerPlay_t playFn = nullptr;
    void* player = nullptr;

    if (!ResolveDirectPlayState(playFn, player))
    {
        Log("[CassettePlay] WARN: no play-capable cassette player - open the "
            "in-game Music Player once so it gets captured (the cold *(MM+0xA8) "
            "object is track-info-only, not a vtable play target)\n");
        return false;
    }

    std::uint64_t outHandle = 0;

    std::memset(g_DirectTrackTable, 0, sizeof(g_DirectTrackTable));
    g_DirectTrackTable[0] = trackId;

    const std::uint32_t playMode = 0;

    const std::uint32_t trackCount = 1;
    const std::uint32_t selectedTrackIndex = 0;
    const std::uint32_t reservedZero = 0;
    const std::uint8_t flag1 = loopPlay ? 1 : 0;
    const std::uint8_t flag2 = playAll ? 1 : 0;

    __try
    {
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    Set_CassetteWalkmanProgrammatic(true);
    playFn(
        player,
        &outHandle,
        playMode,
        g_DirectTrackTable,
        trackCount,
        selectedTrackIndex,
        reservedZero,
        flag1,
        flag2);
    Emit_CassetteWalkmanStart(trackId);
    Set_CassetteWalkmanProgrammatic(false);

    const std::int32_t playResult =
        static_cast<std::int32_t>(static_cast<std::uint32_t>(outHandle));
    if (playResult < 0)
        Log("[CassettePlay] WARN: the play function rejected tape trackId=%u (handle=%d) - music subsystem not ready or blocked by game state.\n", trackId, playResult);

    return true;
}