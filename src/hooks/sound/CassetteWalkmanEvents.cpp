#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#ifdef _DEBUG
#include <intrin.h>
#include <mutex>
#include <set>
#endif

#include "AddressSet.h"
#include "HookUtils.h"
#include "LuaBroadcaster.h"
#include "SoundSystemImpl_BeginSoundSystem.h"
#include "CassetteWalkmanEvents.h"
#include "log.h"

namespace
{
    constexpr const char*   kClass                 = "UI";
    constexpr std::uintptr_t kSpeakerModeVtblOffset = 0x1A0;

    using StopMusicPlayer_t = std::uint32_t* (__fastcall*)(void*, std::uint32_t*, std::uint32_t, std::uint8_t);
    using PauseResume_t     = int* (__fastcall*)(void*, int*, std::uint32_t);
    using SetSpeakerMode_t  = std::uint32_t* (__fastcall*)(void* player, std::uint32_t* outResult, std::uint32_t targetMode);

    std::atomic<std::uint32_t> g_currentTrackId{ 0 };
    thread_local bool          t_programmatic = false;

    enum class WalkmanPlayState : std::uint32_t { Stopped = 0, Playing = 1, Paused = 2 };
    std::atomic<WalkmanPlayState> g_playState{ WalkmanPlayState::Stopped };

    StopMusicPlayer_t g_OrigStop    = nullptr;
    PauseResume_t     g_OrigPause   = nullptr;
    PauseResume_t     g_OrigResume  = nullptr;
    SetSpeakerMode_t  g_OrigSpeaker = nullptr;

    void* g_StopTarget    = nullptr;
    void* g_PauseTarget   = nullptr;
    void* g_ResumeTarget  = nullptr;
    void* g_SpeakerTarget = nullptr;

    std::uint32_t ByUser() { return t_programmatic ? 0u : 1u; }

#ifdef _DEBUG
    int ScanStopCallerChainSEH(std::uint64_t* chain4)
    {
        int n = 0;
        __try
        {
            auto** frame = reinterpret_cast<void**>(_AddressOfReturnAddress());
            for (int i = 0; i < 64 && n < 4; ++i)
            {
                const auto v = reinterpret_cast<std::uint64_t>(frame[i]);
                if (v >= 0x140000000ull && v < 0x142E00000ull)
                    chain4[n++] = v;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    void LogStopCallerOnce()
    {
        std::uint64_t chain[4] = {};
        const int n = ScanStopCallerChainSEH(chain);

        static std::mutex             s_m;
        static std::set<std::uint64_t> s_seen;
        std::lock_guard<std::mutex> lk(s_m);
        if (n == 0 || s_seen.size() >= 16 || !s_seen.insert(chain[0]).second)
            return;
        LogDebug("[CassetteWalkman] StopMusicPlayer caller (game-code, no ASLR): "
            "0x%llX <- 0x%llX <- 0x%llX <- 0x%llX (tid=%lu prog=%d state=%u) - map "
            "the first addr to the EN15.4 dump to see which game path polls the "
            "walkman stop\n",
            static_cast<unsigned long long>(chain[0]),
            static_cast<unsigned long long>(chain[1]),
            static_cast<unsigned long long>(chain[2]),
            static_cast<unsigned long long>(chain[3]),
            GetCurrentThreadId(),
            t_programmatic ? 1 : 0,
            static_cast<unsigned>(g_playState.load(std::memory_order_relaxed)));
    }
#endif

    std::uint32_t* __fastcall hk_Stop(void* player, std::uint32_t* outErr,
                                      std::uint32_t fadeMs, std::uint8_t stopByUser)
    {
#ifdef _DEBUG
        LogStopCallerOnce();
#endif
        if (g_playState.exchange(WalkmanPlayState::Stopped, std::memory_order_relaxed)
                != WalkmanPlayState::Stopped)
            V_FrameWork::EmitMessage(kClass, "StopWalkMan",
                g_currentTrackId.load(std::memory_order_relaxed), ByUser());
        return g_OrigStop ? g_OrigStop(player, outErr, fadeMs, stopByUser) : nullptr;
    }

    int* __fastcall hk_Pause(void* player, int* outErr, std::uint32_t fadeMs)
    {
        WalkmanPlayState expected = WalkmanPlayState::Playing;
        if (g_playState.compare_exchange_strong(expected, WalkmanPlayState::Paused,
                                                std::memory_order_relaxed))
            V_FrameWork::EmitMessage(kClass, "PauseWalkMan",
                g_currentTrackId.load(std::memory_order_relaxed), ByUser());
        return g_OrigPause ? g_OrigPause(player, outErr, fadeMs) : nullptr;
    }

    int* __fastcall hk_Resume(void* player, int* outErr, std::uint32_t fadeMs)
    {
        if (g_playState.exchange(WalkmanPlayState::Playing, std::memory_order_relaxed)
                != WalkmanPlayState::Playing)
            V_FrameWork::EmitMessage(kClass, "StartWalkMan",
                g_currentTrackId.load(std::memory_order_relaxed), ByUser());
        return g_OrigResume ? g_OrigResume(player, outErr, fadeMs) : nullptr;
    }

    std::uint32_t* __fastcall hk_Speaker(void* player, std::uint32_t* outResult, std::uint32_t targetMode)
    {
        if (g_playState.load(std::memory_order_relaxed) != WalkmanPlayState::Stopped)
            V_FrameWork::EmitMessage(kClass, "SpeakerWalkMan",
                g_currentTrackId.load(std::memory_order_relaxed), targetMode, ByUser());
        return g_OrigSpeaker ? g_OrigSpeaker(player, outResult, targetMode) : nullptr;
    }

    void* ResolveSpeakerModeConcrete()
    {
        void* vtbl = ResolveGameAddress(gAddr.CassettePlayerVtable);
        if (!vtbl)
            return nullptr;
        __try
        {
            return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(vtbl) + kSpeakerModeVtblOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
}

void Set_CassetteWalkmanProgrammatic(bool programmatic)
{
    t_programmatic = programmatic;
}

void Emit_CassetteWalkmanStart(std::uint32_t trackId)
{
    g_currentTrackId.store(trackId, std::memory_order_relaxed);
    g_playState.store(WalkmanPlayState::Playing, std::memory_order_relaxed);
    V_FrameWork::EmitMessage(kClass, "StartWalkMan", trackId, ByUser());
}

bool Install_CassetteWalkmanEvents_Hook()
{
    void* stop = ResolveGameAddress(gAddr.StopMusicPlayer);
    if (stop && CreateAndEnableHook(stop, reinterpret_cast<void*>(&hk_Stop),
                                    reinterpret_cast<void**>(&g_OrigStop)))
        g_StopTarget = stop;
    else
        Log("[CassetteWalkman] WARN: StopMusicPlayer hook failed - StopWalkMan will not fire.\n");

    void* pause = ResolveGameAddress(gAddr.PauseMusicPlayer);
    if (pause && CreateAndEnableHook(pause, reinterpret_cast<void*>(&hk_Pause),
                                     reinterpret_cast<void**>(&g_OrigPause)))
        g_PauseTarget = pause;
    else
        Log("[CassetteWalkman] WARN: PauseMusicPlayer hook failed - PauseWalkMan will not fire.\n");

    void* resume = ResolveGameAddress(gAddr.ResumeMusicPlayer);
    if (resume && CreateAndEnableHook(resume, reinterpret_cast<void*>(&hk_Resume),
                                      reinterpret_cast<void**>(&g_OrigResume)))
        g_ResumeTarget = resume;
    else
        Log("[CassetteWalkman] WARN: ResumeMusicPlayer hook failed - resume StartWalkMan will not fire.\n");

    void* speaker = ResolveSpeakerModeConcrete();
    if (speaker && CreateAndEnableHook(speaker, reinterpret_cast<void*>(&hk_Speaker),
                                       reinterpret_cast<void**>(&g_OrigSpeaker)))
        g_SpeakerTarget = speaker;
    else
        Log("[CassetteWalkman] WARN: SetSpeakerMode hook failed - SpeakerWalkMan will not fire.\n");

    return true;
}

bool Uninstall_CassetteWalkmanEvents_Hook()
{
    if (g_StopTarget)    DisableAndRemoveHook(g_StopTarget);
    if (g_PauseTarget)   DisableAndRemoveHook(g_PauseTarget);
    if (g_ResumeTarget)  DisableAndRemoveHook(g_ResumeTarget);
    if (g_SpeakerTarget) DisableAndRemoveHook(g_SpeakerTarget);

    g_OrigStop = nullptr;
    g_OrigPause = nullptr;
    g_OrigResume = nullptr;
    g_OrigSpeaker = nullptr;
    g_StopTarget = g_PauseTarget = g_ResumeTarget = g_SpeakerTarget = nullptr;
    return true;
}
