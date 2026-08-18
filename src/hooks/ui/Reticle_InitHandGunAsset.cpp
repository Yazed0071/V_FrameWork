#include "pch.h"
#include "Reticle_InitHandGunAsset.h"

#include <Windows.h>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    using InitHandGunAsset_t = bool (__fastcall*)(void* self, std::uint8_t withSecondary);

    static InitHandGunAsset_t g_OrigInitHandGunAsset = nullptr;
    static void* g_HookAddr = nullptr;

    static int AvFilter(unsigned int code)
    {
        return code == EXCEPTION_ACCESS_VIOLATION
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    static void DropDanglingReticleAsset(std::uint8_t* self)
    {
        if (!self)
            return;
        void** slot = reinterpret_cast<void**>(self + 0x140);
        __try
        {
            void* asset = *slot;
            if (!asset)
                return;
            if (*reinterpret_cast<void**>(asset) == nullptr)
                *slot = nullptr;
        }
        __except (AvFilter(GetExceptionCode()))
        {
            *slot = nullptr;
        }
    }

    static bool __fastcall hk_InitHandGunAsset(std::uint8_t* self,
                                               std::uint8_t withSecondary)
    {
        DropDanglingReticleAsset(self);
        return g_OrigInitHandGunAsset
            ? g_OrigInitHandGunAsset(self, withSecondary) : false;
    }
}

bool Install_Reticle_InitHandGunAsset_Hook()
{
    void* target = ResolveGameAddress(gAddr.Reticle_InitHandGunAsset);
    if (!target)
    {
        LogDebug("[ReticleAssetGuard] no InitHandGunAsset address for %s; handgun "
            "reticle use-after-free guard disabled.\n",
            GetGameBuildName(gGameBuild));
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target, reinterpret_cast<void*>(&hk_InitHandGunAsset),
        reinterpret_cast<void**>(&g_OrigInitHandGunAsset));
    if (!ok)
    {
        Log("[ReticleAssetGuard] ERROR: failed to hook InitHandGunAsset @ %p; "
            "handgun reticle use-after-free guard disabled.\n", target);
        return false;
    }

    g_HookAddr = target;
    return true;
}

void Uninstall_Reticle_InitHandGunAsset_Hook()
{
    if (g_HookAddr)
        DisableAndRemoveHook(g_HookAddr);
    g_HookAddr = nullptr;
    g_OrigInitHandGunAsset = nullptr;
}
