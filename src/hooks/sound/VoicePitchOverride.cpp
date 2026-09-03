#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <set>
#include <mutex>
#include <unordered_map>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "VoicePitchOverride.h"
#include "MissionCodeGuard.h"
#include "../soldier/SoldierAkObjIdMap.h"


namespace
{
    using SetPitch_t = void(__fastcall*)(void* self, float pitchCents);

    constexpr std::uintptr_t kResampler_BackToPitchNode  = 0x10;
    constexpr std::uintptr_t kPitchNode_PBI              = 0xD8;
    constexpr std::uintptr_t kPBI_RegisteredObj          = 0xA8;
    constexpr std::uintptr_t kRegisteredObj_AkObjId      = 0x70;

    static SetPitch_t        g_OrigSetPitch       = nullptr;
    static void*             g_HookTarget         = nullptr;
    static std::atomic<float> g_PitchBiasCents   { 0.0f };

    static std::unordered_map<std::uint64_t, float> g_BiasByAkObjId;
    static std::mutex g_BiasMapMutex;
    static std::atomic<bool> g_HavePerAkObjIdBias{ false };

    static void* SafeReadPtr(const void* base, std::uintptr_t off)
    {
        if (!base) return nullptr;
        __try
        {
            return *reinterpret_cast<void* const*>(
                reinterpret_cast<std::uintptr_t>(base) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static std::uint64_t SafeReadU64(const void* base, std::uintptr_t off)
    {
        if (!base) return 0;
        __try
        {
            return *reinterpret_cast<const std::uint64_t*>(
                reinterpret_cast<std::uintptr_t>(base) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }


    static std::uint64_t ResolveAkObjIdFromResampler(void* resampler)
    {
        if (!resampler) return 0;

        const auto pitchNode = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(resampler) - kResampler_BackToPitchNode);

        const void* pbi = SafeReadPtr(pitchNode, kPitchNode_PBI);
        if (!pbi) return 0;

        const void* regObj = SafeReadPtr(pbi, kPBI_RegisteredObj);
        if (!regObj) return 0;

        return SafeReadU64(regObj, kRegisteredObj_AkObjId);
    }


    static float LookupBiasForAkObjId(std::uint64_t akObjId)
    {
        if (!akObjId) return 0.0f;
        if (!g_HavePerAkObjIdBias.load(std::memory_order_relaxed)) return 0.0f;

        std::lock_guard<std::mutex> lock(g_BiasMapMutex);
        const auto it = g_BiasByAkObjId.find(akObjId);
        if (it == g_BiasByAkObjId.end()) return 0.0f;
        return it->second;
    }


    static std::atomic<bool>          g_ReportSpeakers{ true };
    static std::atomic<std::uint32_t> g_ReportSpeakerCalls{ 0 };

    static constexpr std::size_t   kSpeakerNameCap    = 48;
    static constexpr std::size_t   kUnnamedSpeakerCap = 16;
    static constexpr std::uint32_t kSpeakerProbeCalls = 4000000u;

    static void ReportSpeakingAkObjId(std::uint64_t akObjId)
    {
        if (g_ReportSpeakerCalls.fetch_add(1, std::memory_order_relaxed)
            >= kSpeakerProbeCalls)
        {
            g_ReportSpeakers.store(false, std::memory_order_relaxed);
            return;
        }

        const std::string name = SoldierAkObjIdMap::GetEmitterNameForAkObjId(
            static_cast<std::uint32_t>(akObjId));

        static std::mutex              s_mutex;
        static std::set<std::string>   s_namesHeard;
        static std::set<std::uint64_t> s_unnamedHeard;

        std::lock_guard<std::mutex> lock(s_mutex);

        if (name.empty())
        {
            if (s_unnamedHeard.size() >= kUnnamedSpeakerCap
                || !s_unnamedHeard.insert(akObjId).second)
                return;

            LogDebug("[VoicePitch] akObjId %llu is producing sound but never passed "
                     "through the emitter registration hook, so it has no name to key "
                     "a pitch override on\n",
                static_cast<unsigned long long>(akObjId));
            return;
        }

        if (!s_namesHeard.insert(name).second)
            return;
        if (s_namesHeard.size() >= kSpeakerNameCap)
            g_ReportSpeakers.store(false, std::memory_order_relaxed);

        LogDebug("[VoicePitch] emitter '%s' is producing sound on akObjId %llu - a "
                 "pitch bias keyed to that id reaches this voice\n",
            name.c_str(), static_cast<unsigned long long>(akObjId));
    }


    static void __fastcall hk_SetPitch(void* self, float pitchCents)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigSetPitch, self, pitchCents);

        const float globalBias = g_PitchBiasCents.load(std::memory_order_relaxed);
        const bool  haveAnyPerObj =
            g_HavePerAkObjIdBias.load(std::memory_order_relaxed);

        std::uint64_t akObjId = 0;
        const bool probing = g_ReportSpeakers.load(std::memory_order_relaxed);
        if ((haveAnyPerObj || probing) && self)
            akObjId = ResolveAkObjIdFromResampler(self);
        if (probing && akObjId)
            ReportSpeakingAkObjId(akObjId);

        float bias = globalBias;
        if (haveAnyPerObj && akObjId)
        {
            const float perObjBias = LookupBiasForAkObjId(akObjId);
            if (perObjBias != 0.0f)
                bias = perObjBias;
        }

        if (g_OrigSetPitch)
            g_OrigSetPitch(self, pitchCents + bias);
    }
}


void Set_GlobalVoicePitchBiasCents(float centsBias)
{
    g_PitchBiasCents.store(centsBias, std::memory_order_relaxed);
}


float Get_GlobalVoicePitchBiasCents()
{
    return g_PitchBiasCents.load(std::memory_order_relaxed);
}


void Set_PitchBiasForAkObjId(std::uint64_t akObjId, float centsBias)
{
    if (!akObjId) return;

    std::lock_guard<std::mutex> lock(g_BiasMapMutex);
    if (centsBias == 0.0f)
        g_BiasByAkObjId.erase(akObjId);
    else
        g_BiasByAkObjId[akObjId] = centsBias;
    g_HavePerAkObjIdBias.store(!g_BiasByAkObjId.empty(), std::memory_order_relaxed);
}


void Clear_PitchBiasForAkObjId(std::uint64_t akObjId)
{
    std::lock_guard<std::mutex> lock(g_BiasMapMutex);
    g_BiasByAkObjId.erase(akObjId);
    g_HavePerAkObjIdBias.store(!g_BiasByAkObjId.empty(), std::memory_order_relaxed);
}


void Clear_AllPerAkObjIdPitchBiases()
{
    std::lock_guard<std::mutex> lock(g_BiasMapMutex);
    g_BiasByAkObjId.clear();
    g_HavePerAkObjIdBias.store(false, std::memory_order_relaxed);
}


bool Install_VoicePitchOverride_Hook()
{
    const auto addr = gAddr.CAkResampler_SetPitch;
    if (!addr) return false;

    void* target = ResolveGameAddress(addr);
    if (!target) return false;

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hk_SetPitch),
        reinterpret_cast<void**>(&g_OrigSetPitch));
    if (ok)
        g_HookTarget = target;
    return ok;
}


bool Uninstall_VoicePitchOverride_Hook()
{
    if (g_HookTarget)
    {
        DisableAndRemoveHook(g_HookTarget);
        g_HookTarget = nullptr;
        g_OrigSetPitch = nullptr;
    }
    g_PitchBiasCents.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_BiasMapMutex);
        g_BiasByAkObjId.clear();
    }
    g_HavePerAkObjIdBias.store(false, std::memory_order_relaxed);
    return true;
}
