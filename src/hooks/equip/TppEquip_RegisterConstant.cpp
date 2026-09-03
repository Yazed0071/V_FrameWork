#include "pch.h"
#include "TppEquip_RegisterConstant.h"

#include <Windows.h>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "TppEquipConstRegistry.h"
#include "CustomBluePrint.h"

extern "C" {
#include "lua.h"
}

namespace
{
    using RegisterConstant_t = void(__fastcall*)(lua_State* L);
    static RegisterConstant_t g_OrigRegisterConstant = nullptr;

    static void __fastcall hkRegisterConstant(lua_State* L)
    {
        if (g_OrigRegisterConstant)
            g_OrigRegisterConstant(L);

        if (L)
        {
            TppEquipConst_InjectAll(L);
            TppDamageConst_InjectAll(L);
            bluePrint::InjectConstants(L);
        }
    }
}

bool Install_TppEquip_RegisterConstant_Hook()
{
    void* target = ResolveGameAddress(gAddr.TppEquip_RegisterConstant);
    if (!target)
    {
        LogDebug("[TppEquipConst] address not set for this build - skipped\n");
        return true;
    }

    const bool ok = CreateAndEnableHook(
        target, &hkRegisterConstant,
        reinterpret_cast<void**>(&g_OrigRegisterConstant));
    if (!ok)
    {
        Log("[TppEquipConst] Install -> FAIL (target=%p)\n", target);
    }
#ifdef _DEBUG
    else
    {
        LogDebug("[TppEquipConst] Install -> OK (target=%p)\n", target);
    }
#endif
    return ok;
}

bool Uninstall_TppEquip_RegisterConstant_Hook()
{
    if (gAddr.TppEquip_RegisterConstant)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.TppEquip_RegisterConstant));
    g_OrigRegisterConstant = nullptr;

    TppEquipConst_ResetRuntimeState();
    return true;
}
