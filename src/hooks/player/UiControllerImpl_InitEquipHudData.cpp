#include "pch.h"
#include "UiControllerImpl_InitEquipHudData.h"

#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "TimeCigaretteUiHook.h"
#include "MissionCodeGuard.h"

namespace
{
    using InitEquipHudData_t = void(__fastcall*)(void* this_);
    static InitEquipHudData_t g_Orig = nullptr;

    static constexpr std::size_t kInnerPtrOffset  = 0x48;
    static constexpr std::size_t kNoUseListOffset = 0x1a4;
    static constexpr std::size_t kNoUseListCount  = 32;
}

static void __fastcall hkInitEquipHudData(void* this_)
{
    if (g_Orig)
        g_Orig(this_);

    TimeCigaretteUi_SetUiController(this_);

    if (MissionCodeGuard::ShouldBypassHooks())
        return;

    auto* inner = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(this_) + kInnerPtrOffset);
    if (inner)
        std::memset(inner + kNoUseListOffset, 0, kNoUseListCount * sizeof(std::uint32_t));
}

bool Install_InitEquipHudData()
{
    if (!gAddr.UiControllerImpl_InitEquipHudData)
    {
        LogDebug("[InitEquipHudData] address not set for this build\n");
        return false;
    }
    void* target = ResolveGameAddress(gAddr.UiControllerImpl_InitEquipHudData);
    const bool ok = CreateAndEnableHook(target, reinterpret_cast<void*>(&hkInitEquipHudData),
                                        reinterpret_cast<void**>(&g_Orig));
#ifdef _DEBUG
    Log("[InitEquipHudData] hook: %s (target=%p)\n", ok ? "OK" : "FAIL", target);
#else
    if (!ok)
        Log("[InitEquipHudData] hook: %s (target=%p)\n", ok ? "OK" : "FAIL", target);
#endif
    return ok;
}

bool Uninstall_InitEquipHudData()
{
    if (gAddr.UiControllerImpl_InitEquipHudData)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.UiControllerImpl_InitEquipHudData));
    g_Orig = nullptr;
    return true;
}
