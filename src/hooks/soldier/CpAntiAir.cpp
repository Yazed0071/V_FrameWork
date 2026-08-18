#include "pch.h"

#include "CpAntiAir.h"

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaBroadcaster.h"
#include "MissionCodeGuard.h"

using UpdateAntiAirHook_t = bool(__thiscall*)(long long* self, unsigned long long param_1);
using ClearAntiAirHook_t = void(__thiscall*)(long long* self, unsigned long long param_1);

// ----------------------------------------------------
// Original pointers
// ----------------------------------------------------

static UpdateAntiAirHook_t g_OrigUpdateAntiAirHook = nullptr;
static ClearAntiAirHook_t g_OrigClearAntiAirHook = nullptr;

// ----------------------------------------------------
// Message
// ----------------------------------------------------

void SendAntiAirMessage(unsigned long long cpIndex, bool isEnable)
{
    V_FrameWork::EmitMessage("GameObject", "AntiAir", cpIndex + 512, isEnable ? 1 : 0);
}

// ----------------------------------------------------
// Hook
// ----------------------------------------------------

bool __thiscall hkUpdateAntiAir(long long* self, unsigned long long cpIndex)
{
    const bool ret = g_OrigUpdateAntiAirHook(self, cpIndex);
    if (MissionCodeGuard::ShouldBypassHooks())
        return ret;

    if (ret == true)
        SendAntiAirMessage(cpIndex, true);

    return ret;
}

void __thiscall hkClearAntiAir(long long* self, unsigned long long cpIndex)
{
    g_OrigClearAntiAirHook(self, cpIndex);
    if (MissionCodeGuard::ShouldBypassHooks())
        return;
    SendAntiAirMessage(cpIndex, false);
}

bool Install_CpAntiAir_Hook()
{
    void* target = ResolveGameAddress(gAddr.UpdateAntiAir);

    const bool okTarget = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkUpdateAntiAir),
        reinterpret_cast<void**>(&g_OrigUpdateAntiAirHook));

#ifdef _DEBUG
    LogDebug("[Hook] UpdateAntiAir %d installed at %p\n", okTarget, target);
#endif

    void* target2 = ResolveGameAddress(gAddr.ClearAntiAir);

    const bool okTarget2 = CreateAndEnableHook(
        target2,
        reinterpret_cast<void*>(&hkClearAntiAir),
        reinterpret_cast<void**>(&g_OrigClearAntiAirHook));

#ifdef _DEBUG
    LogDebug("[Hook] ClearAntiAir %d installed at %p\n", okTarget2, target2);
#endif

    return okTarget && okTarget2;
}

bool Uninstall_CpAntiAir_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.UpdateAntiAir));
    g_OrigUpdateAntiAirHook = nullptr;
    DisableAndRemoveHook(ResolveGameAddress(gAddr.ClearAntiAir));
    g_OrigClearAntiAirHook = nullptr;
    return true;
}
