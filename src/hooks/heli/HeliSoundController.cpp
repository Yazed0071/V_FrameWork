#include "pch.h"
#include "HeliSoundController.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "LuaBroadcaster.h"
#include "log.h"
#include "MissionCodeGuard.h"

namespace
{
    using Update_t       = void(__fastcall*)(void* this_);
    using CallVoice_t    = void(__fastcall*)(void* this_, std::uint32_t slot, std::uint32_t voiceId, std::uint32_t voiceType, std::uint32_t param4);
    using FlightUpdate_t = void(__fastcall*)(void* this_);

    static Update_t           g_OrigUpdate       = nullptr;
    static CallVoice_t        g_CallVoice        = nullptr;
    static CallVoice_t        g_OrigCallVoice    = nullptr;
    static FlightUpdate_t     g_OrigFlightUpdate = nullptr;
    static bool               g_Installed     = false;
    static std::atomic<void*> g_Controller{ nullptr };

    static const int kMaxSlots = 16;

    struct VoiceSnap   { std::uint32_t voiceId; std::uint32_t voiceType; std::uint32_t param4; };
    struct VoiceFinish { std::uint32_t voiceId; std::uint32_t slot; std::uint32_t voiceType; std::uint32_t param4; };

    static VoiceSnap g_Snap[kMaxSlots] = {};

    static VoiceFinish g_PendingFinish[kMaxSlots] = {};
    static int         g_PendingFinishCount = 0;

    static int CollectVoiceFinishes(void* this_, VoiceFinish* out, int maxOut)
    {
        int n = 0;
        __try
        {
            char* self = reinterpret_cast<char*>(this_);
            char* base = *reinterpret_cast<char**>(self + 0x58);
            char* p48  = *reinterpret_cast<char**>(self + 0x48);
            if (!base || !p48)
                return 0;

            char* cfg = *reinterpret_cast<char**>(p48 + 0x28);
            if (!cfg)
                return 0;

            std::uint32_t count = *reinterpret_cast<std::uint32_t*>(cfg + 0x40);
            if (count > static_cast<std::uint32_t>(kMaxSlots))
                count = kMaxSlots;

            for (std::uint32_t s = 0; s < count; ++s)
            {
                char* work = base + static_cast<size_t>(s) * 0x260;
                std::uint32_t cur = *reinterpret_cast<std::uint32_t*>(work + 0x250);

                if (cur != 0)
                {
                    if (g_Snap[s].voiceId == 0)
                    {
                        g_Snap[s].voiceId   = cur;
                        g_Snap[s].voiceType = (static_cast<std::uint8_t>(work[0x258]) >> 2) & 3;
                        g_Snap[s].param4    = *reinterpret_cast<std::uint32_t*>(work + 0x254);
                    }
                }
                else if (g_Snap[s].voiceId != 0)
                {
                    if (n < maxOut)
                    {
                        out[n].voiceId   = g_Snap[s].voiceId;
                        out[n].slot      = s;
                        out[n].voiceType = g_Snap[s].voiceType;
                        out[n].param4    = g_Snap[s].param4;
                        ++n;
                    }
                    g_Snap[s].voiceId = 0;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }

    static void FlushPendingFinishes()
    {
        const int n = g_PendingFinishCount;
        g_PendingFinishCount = 0;
        for (int i = 0; i < n; ++i)
            V_FrameWork::EmitMessage("Radio", "HeliFinish", g_PendingFinish[i].voiceId, g_PendingFinish[i].param4, g_PendingFinish[i].voiceType);
    }

    static void __fastcall hkUpdate(void* this_)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigUpdate, this_);

        if (this_)
            g_Controller.store(this_, std::memory_order_relaxed);

        if (g_OrigUpdate)
            g_OrigUpdate(this_);

        if (this_)
        {
            VoiceFinish finishes[kMaxSlots];
            const int n = CollectVoiceFinishes(this_, finishes, kMaxSlots);
            for (int i = 0; i < n && g_PendingFinishCount < kMaxSlots; ++i)
                g_PendingFinish[g_PendingFinishCount++] = finishes[i];
        }
    }

    static void __fastcall hkFlightUpdate(void* this_)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigFlightUpdate, this_);

        if (g_OrigFlightUpdate)
            g_OrigFlightUpdate(this_);

        FlushPendingFinishes();
    }

    static void __fastcall hkCallVoice(void* this_, std::uint32_t slot, std::uint32_t voiceId, std::uint32_t voiceType, std::uint32_t param4)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigCallVoice, this_, slot, voiceId, voiceType, param4);

        if (this_)
            g_Controller.store(this_, std::memory_order_relaxed);

        FlushPendingFinishes();
        V_FrameWork::EmitMessage("Radio", "HeliStart", voiceId, param4, voiceType);

        if (g_OrigCallVoice)
            g_OrigCallVoice(this_, slot, voiceId, voiceType, param4);
    }

    static bool TryCallVoice(CallVoice_t fn, void* controller, std::uint32_t slot, std::uint32_t voiceId, std::uint32_t voiceType, std::uint32_t param4)
    {
        __try
        {
            fn(controller, slot, voiceId, voiceType, param4);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

bool Play_PilotCallVoice(std::uint32_t voiceId, std::uint32_t slot, std::uint32_t voiceType, std::uint32_t param4)
{
    MISSION_GUARD_RETURN_FALSE();

    CallVoice_t fn = g_OrigCallVoice ? g_OrigCallVoice : g_CallVoice;
    if (!fn)
        return false;

    void* controller = g_Controller.load(std::memory_order_relaxed);
    if (!controller)
        return false;

    FlushPendingFinishes();
    V_FrameWork::EmitMessage("Radio", "HeliStart", voiceId, param4, voiceType);
    return TryCallVoice(fn, controller, slot, voiceId, voiceType, param4);
}

bool Install_HeliSoundController_Hook()
{
    if (g_Installed)
        return true;

    if (!gAddr.HeliSoundControllerImpl_Update)
    {
        LogDebug("[HeliSoundController] Update address not set for this build\n");
        return false;
    }

    if (gAddr.HeliSoundControllerImpl_CallVoice)
    {
        g_CallVoice = reinterpret_cast<CallVoice_t>(ResolveGameAddress(gAddr.HeliSoundControllerImpl_CallVoice));
        if (g_CallVoice)
        {
            if (!CreateAndEnableHook(reinterpret_cast<void*>(g_CallVoice),
                                     reinterpret_cast<void*>(&hkCallVoice),
                                     reinterpret_cast<void**>(&g_OrigCallVoice)))
            {
                g_OrigCallVoice = nullptr;
                Log("[HeliSoundController] CallVoice hook failed; HeliStart broadcast disabled\n");
            }
        }
    }

    if (gAddr.HeliFlightControllerImpl_Update)
    {
        void* flightTarget = ResolveGameAddress(gAddr.HeliFlightControllerImpl_Update);
        if (flightTarget)
        {
            if (!CreateAndEnableHook(flightTarget,
                                     reinterpret_cast<void*>(&hkFlightUpdate),
                                     reinterpret_cast<void**>(&g_OrigFlightUpdate)))
            {
                g_OrigFlightUpdate = nullptr;
                Log("[HeliSoundController] FlightController::Update hook failed; HeliFinish drain disabled\n");
            }
        }
    }

    void* target = ResolveGameAddress(gAddr.HeliSoundControllerImpl_Update);
    if (!target)
    {
        Log("[HeliSoundController] Update resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkUpdate),
        reinterpret_cast<void**>(&g_OrigUpdate));

    if (ok)
        g_Installed = true;

    if (!ok)
    {
        Log("[HeliSoundController] hook: FAIL (Update=%p, CallVoice=%p, trampoline=%p)\n",
            target, reinterpret_cast<void*>(g_CallVoice), reinterpret_cast<void*>(g_OrigCallVoice));
    }
#ifdef _DEBUG
    else
    {
        LogDebug("[HeliSoundController] hook: OK (Update=%p, CallVoice=%p, trampoline=%p)\n",
            target, reinterpret_cast<void*>(g_CallVoice), reinterpret_cast<void*>(g_OrigCallVoice));
    }
#endif
    return ok;
}

bool Uninstall_HeliSoundController_Hook()
{
    if (!g_Installed)
        return true;

    if (gAddr.HeliSoundControllerImpl_Update)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.HeliSoundControllerImpl_Update));

    if (g_CallVoice)
        DisableAndRemoveHook(reinterpret_cast<void*>(g_CallVoice));

    if (gAddr.HeliFlightControllerImpl_Update && g_OrigFlightUpdate)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.HeliFlightControllerImpl_Update));

    g_OrigUpdate       = nullptr;
    g_CallVoice        = nullptr;
    g_OrigCallVoice    = nullptr;
    g_OrigFlightUpdate = nullptr;
    g_Installed     = false;
    g_Controller.store(nullptr, std::memory_order_relaxed);
    for (int i = 0; i < kMaxSlots; ++i)
        g_Snap[i].voiceId = 0;
    g_PendingFinishCount = 0;
    return true;
}
