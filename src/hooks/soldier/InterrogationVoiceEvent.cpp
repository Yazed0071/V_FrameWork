#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "AddressSet.h"
#include "HookUtils.h"
#include "InterrogationVoiceEvent.h"
#include "MissionStateReset.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kDdVoxEne      = 0x95ea16b0u;
    constexpr std::uint32_t kDdVoxEneState = 0x68b311e1u;

    using UpdateInterrogation_t = void (__fastcall*)(void* soldier2, std::uint32_t index,
                                                     void* knowledge, std::uint32_t hung);
    using CallVoice_t = char (__fastcall*)(void* self, std::uint32_t slot,
                                           std::uint32_t voiceType, std::uint32_t event,
                                           std::uint64_t a5, std::uint64_t a6, std::uint64_t a7);

    std::mutex g_mtx;
    std::unordered_map<std::uint32_t, std::uint32_t> g_eventByCp;

    thread_local std::uint32_t t_event = 0;

    std::atomic<std::uint32_t> g_pendingEvent{ 0 };
    std::atomic<std::uint64_t> g_pendingUntilTick{ 0 };

    UpdateInterrogation_t g_OrigUpdate       = nullptr;
    UpdateInterrogation_t g_OrigUpdateMarker = nullptr;
    CallVoice_t           g_OrigCallVoice    = nullptr;

    void* g_UpdateTarget    = nullptr;
    void* g_MarkerTarget    = nullptr;
    void* g_CallVoiceTarget = nullptr;

    bool TryReadCpId(void* soldier2, std::uint32_t& out)
    {
        __try
        {
            const auto base = reinterpret_cast<const std::uint8_t*>(soldier2);
            void* p = *reinterpret_cast<void* const*>(base + 0xD0);
            if (!p)
                return false;
            out = *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(p) + 0x30);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    std::uint32_t LookupEvent(std::uint32_t cpId)
    {
        MissionStateReset::PollMissionChange();

        std::lock_guard<std::mutex> lk(g_mtx);
        const auto it = g_eventByCp.find(cpId);
        return it != g_eventByCp.end() ? it->second : 0u;
    }

    void RunUpdateGuarded(UpdateInterrogation_t orig, void* soldier2, std::uint32_t index,
                          void* knowledge, std::uint32_t hung, std::uint32_t ev)
    {
        if (ev)
        {
            g_pendingEvent.store(ev, std::memory_order_relaxed);
            g_pendingUntilTick.store(GetTickCount64() + 2000, std::memory_order_relaxed);
        }

        const std::uint32_t prev = t_event;
        t_event = ev;
        __try   { orig(soldier2, index, knowledge, hung); }
        __finally { t_event = prev; }
    }

    void __fastcall hk_UpdateInterrogation(void* soldier2, std::uint32_t index,
                                           void* knowledge, std::uint32_t hung)
    {
        if (!g_OrigUpdate)
            return;
        std::uint32_t cp = 0, ev = 0;
        if (TryReadCpId(soldier2, cp))
            ev = LookupEvent(cp);
        RunUpdateGuarded(g_OrigUpdate, soldier2, index, knowledge, hung, ev);
    }

    void __fastcall hk_UpdateInterrogationMarker(void* soldier2, std::uint32_t index,
                                                 void* knowledge, std::uint32_t hung)
    {
        if (!g_OrigUpdateMarker)
            return;
        std::uint32_t cp = 0, ev = 0;
        if (TryReadCpId(soldier2, cp))
            ev = LookupEvent(cp);
        RunUpdateGuarded(g_OrigUpdateMarker, soldier2, index, knowledge, hung, ev);
    }

    std::uint32_t TakePendingEvent()
    {
        if (t_event != 0)
            return t_event;
        if (GetTickCount64() >= g_pendingUntilTick.load(std::memory_order_relaxed))
            return 0;
        return g_pendingEvent.exchange(0, std::memory_order_relaxed);
    }

    char __fastcall hk_CallVoice(void* self, std::uint32_t slot, std::uint32_t voiceType,
                                 std::uint32_t event, std::uint64_t a5,
                                 std::uint64_t a6, std::uint64_t a7)
    {
        std::uint32_t ev = event;
        if (event == kDdVoxEne || event == kDdVoxEneState)
        {
            const std::uint32_t pending = TakePendingEvent();
            if (pending != 0)
                ev = pending;
        }
        return g_OrigCallVoice ? g_OrigCallVoice(self, slot, voiceType, ev, a5, a6, a7) : 0;
    }
}

void Register_InterrogationVoiceEvent(std::uint32_t cpIndex, std::uint32_t eventId)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (eventId)
        g_eventByCp[cpIndex] = eventId;
    else
        g_eventByCp.erase(cpIndex);
}

void Clear_InterrogationVoiceEvents()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_eventByCp.clear();
    g_pendingEvent.store(0, std::memory_order_relaxed);
    g_pendingUntilTick.store(0, std::memory_order_relaxed);
}

bool Install_InterrogationVoiceEvent_Hook()
{
    void* cv = ResolveGameAddress(gAddr.SoundControllerImpl_CallVoice);
    if (!cv)
    {
        Log("[InterrogationVoice] ERROR: CallVoice address 0 (unsupported build) - custom "
            "interrogation voice disabled.\n");
        return true;
    }
    if (!CreateAndEnableHook(cv, reinterpret_cast<void*>(&hk_CallVoice),
                             reinterpret_cast<void**>(&g_OrigCallVoice)))
    {
        Log("[InterrogationVoice] ERROR: CallVoice hook failed - custom interrogation voice "
            "will not apply.\n");
        return false;
    }
    g_CallVoiceTarget = cv;

    void* up = ResolveGameAddress(gAddr.Soldier2InterrogateUtil_UpdateInterrogation);
    if (up && CreateAndEnableHook(up, reinterpret_cast<void*>(&hk_UpdateInterrogation),
                                  reinterpret_cast<void**>(&g_OrigUpdate)))
        g_UpdateTarget = up;
    else
        Log("[InterrogationVoice] WARN: UpdateInterrogation hook failed - main interrogation "
            "line will keep the vanilla voice.\n");

    void* mk = ResolveGameAddress(gAddr.Soldier2InterrogateUtil_UpdateInterrogationMarker);
    if (mk && CreateAndEnableHook(mk, reinterpret_cast<void*>(&hk_UpdateInterrogationMarker),
                                  reinterpret_cast<void**>(&g_OrigUpdateMarker)))
        g_MarkerTarget = mk;
    else
        Log("[InterrogationVoice] WARN: UpdateInterrogationMarker hook failed - marker "
            "interrogation lines will keep the vanilla voice.\n");

    return true;
}

bool Uninstall_InterrogationVoiceEvent_Hook()
{
    if (g_CallVoiceTarget) DisableAndRemoveHook(g_CallVoiceTarget);
    if (g_UpdateTarget)    DisableAndRemoveHook(g_UpdateTarget);
    if (g_MarkerTarget)    DisableAndRemoveHook(g_MarkerTarget);

    g_OrigCallVoice = nullptr;
    g_OrigUpdate = nullptr;
    g_OrigUpdateMarker = nullptr;
    g_CallVoiceTarget = g_UpdateTarget = g_MarkerTarget = nullptr;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_eventByCp.clear();
    }
    g_pendingEvent.store(0, std::memory_order_relaxed);
    g_pendingUntilTick.store(0, std::memory_order_relaxed);
    return true;
}
