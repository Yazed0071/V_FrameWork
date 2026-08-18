#include "pch.h"
#include "TimeCigaretteUiHook.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "LuaBroadcaster.h"
#include "MissionCodeGuard.h"
#include "log.h"

namespace
{
    constexpr const char* kClass = "UI";
    constexpr const char* kMessage = "TimeCigaretteUi";

    using ShowHideTimeCigaretteUi_t = void(__fastcall*)(void* this_, std::uint32_t index);

    static ShowHideTimeCigaretteUi_t g_OrigShow = nullptr;
    static ShowHideTimeCigaretteUi_t g_OrigHide = nullptr;
    static void*                     g_ShowTarget = nullptr;
    static void*                     g_HideTarget = nullptr;
    static bool                      g_Installed = false;

    static std::atomic<void*>         g_UiController{ nullptr };
    static std::atomic<std::uint32_t> g_LastIndex{ 0 };

    static constexpr std::size_t kOwnerField        = 0x8;
    static constexpr std::size_t kManagerField      = 0x138;
    static constexpr std::size_t kUiControllerField = 0xD8;
    static constexpr std::size_t kActionInfoField   = 0x38;
    static constexpr std::size_t kSlotOriginField   = 0x24;
    static constexpr std::size_t kSlotArrayField    = 0x78;
    static constexpr std::size_t kSlotStride        = 0x1A0;
    static constexpr std::size_t kSlotFlagsField    = 0x17C;
    static constexpr std::uint32_t kShownBit        = 0x400;

    static constexpr std::size_t kShowUiVtblByteOffset = 0x210;
    static constexpr std::size_t kHideUiVtblByteOffset = 0x218;

    void ReportShown(std::uint32_t index)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return;
        V_FrameWork::EmitMessage(kClass, kMessage, index, 1u);
    }

    void ReportHidden(std::uint32_t index)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return;
        V_FrameWork::EmitMessage(kClass, kMessage, index, 0u);
    }

    static bool TryReadSlot(void* this_, std::uint32_t index, void** outUiController, void** outElement)
    {
        *outUiController = nullptr;
        *outElement      = nullptr;

        if (!this_)
            return false;

        __try
        {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(this_);

            void* owner = *reinterpret_cast<void**>(base + kOwnerField);
            if (owner)
            {
                void* manager = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(owner) + kManagerField);
                if (manager)
                    *outUiController = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(manager) + kUiControllerField);
            }

            void* actionInfo = *reinterpret_cast<void**>(base + kActionInfoField);
            const std::uintptr_t slotArray = *reinterpret_cast<std::uintptr_t*>(base + kSlotArrayField);
            if (!actionInfo || !slotArray)
                return *outUiController != nullptr;

            const std::uint32_t origin =
                *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(actionInfo) + kSlotOriginField);
            if (index < origin)
                return *outUiController != nullptr;

            *outElement = reinterpret_cast<void*>(
                slotArray + static_cast<std::uintptr_t>(index - origin) * kSlotStride);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *outUiController = nullptr;
            *outElement      = nullptr;
            return false;
        }
    }

    static bool TryIsShown(void* element)
    {
        if (!element)
            return false;

        __try
        {
            const std::uint32_t flags =
                *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(element) + kSlotFlagsField);
            return (flags & kShownBit) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool TryCallUiVtblSlot(void* uiController, std::size_t vtblByteOffset)
    {
        if (!uiController)
            return false;

        __try
        {
            void** vtbl = *reinterpret_cast<void***>(uiController);
            if (!vtbl)
                return false;

            auto fn = reinterpret_cast<void(__fastcall*)(void*)>(vtbl[vtblByteOffset / sizeof(void*)]);
            if (!fn)
                return false;

            fn(uiController);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void __fastcall hkShowTimeCigaretteUi(void* this_, std::uint32_t index)
    {
        void* uiController = nullptr;
        void* element      = nullptr;
        const bool read    = TryReadSlot(this_, index, &uiController, &element);

        if (uiController)
            g_UiController.store(uiController, std::memory_order_relaxed);
        g_LastIndex.store(index, std::memory_order_relaxed);

        const bool wasShown = read && TryIsShown(element);

        if (g_OrigShow)
            g_OrigShow(this_, index);

        const bool nowShown = read && TryIsShown(element);

        {
            static unsigned long long s_lastMs = 0;
            const bool transition = (wasShown != nowShown);
            const unsigned long long now = GetTickCount64();
            if (transition || now - s_lastMs >= 2000)
            {
                s_lastMs = now;
                LogDebug("[TimeCigDiag] SHOW read=%d was=%d now=%d idx=%u%s\n",
                    static_cast<int>(read), static_cast<int>(wasShown),
                    static_cast<int>(nowShown), index,
                    transition ? " <TRANSITION>" : "");
            }
        }

        if (read && !wasShown && nowShown)
            ReportShown(index);
    }

    static void __fastcall hkHideTimeCigaretteUi(void* this_, std::uint32_t index)
    {
        void* uiController = nullptr;
        void* element      = nullptr;
        const bool read    = TryReadSlot(this_, index, &uiController, &element);

        if (uiController)
            g_UiController.store(uiController, std::memory_order_relaxed);
        g_LastIndex.store(index, std::memory_order_relaxed);

        const bool wasShown = read && TryIsShown(element);

        if (g_OrigHide)
            g_OrigHide(this_, index);

        const bool nowShown = read && TryIsShown(element);

        {
            static unsigned long long s_lastMs = 0;
            const bool transition = (wasShown != nowShown);
            const unsigned long long now = GetTickCount64();
            if (transition || now - s_lastMs >= 2000)
            {
                s_lastMs = now;
                LogDebug("[TimeCigDiag] HIDE read=%d was=%d now=%d idx=%u%s\n",
                    static_cast<int>(read), static_cast<int>(wasShown),
                    static_cast<int>(nowShown), index,
                    transition ? " <TRANSITION>" : "");
            }
        }

        if (read && wasShown && !nowShown)
            ReportHidden(index);
    }
}

void TimeCigaretteUi_SetUiController(void* uiController)
{
    if (uiController)
        g_UiController.store(uiController, std::memory_order_relaxed);
}

bool Show_TimeCigaretteUi()
{
    const bool ok = TryCallUiVtblSlot(g_UiController.load(std::memory_order_relaxed), kShowUiVtblByteOffset);
    if (ok)
        ReportShown(g_LastIndex.load(std::memory_order_relaxed));
    return ok;
}

bool Hide_TimeCigaretteUi()
{
    const bool ok = TryCallUiVtblSlot(g_UiController.load(std::memory_order_relaxed), kHideUiVtblByteOffset);
    if (ok)
        ReportHidden(g_LastIndex.load(std::memory_order_relaxed));
    return ok;
}

bool Install_TimeCigaretteUi_Hook()
{
    if (g_Installed)
        return true;

    if (!gAddr.TimeCigaretteActionPluginImpl_ShowTimeCigaretteUi)
    {
        LogDebug("[TimeCigaretteUi] address not set for this build\n");
        return false;
    }

    void* showTarget = ResolveGameAddress(gAddr.TimeCigaretteActionPluginImpl_ShowTimeCigaretteUi);
    if (!showTarget)
    {
        Log("[TimeCigaretteUi] resolve failed\n");
        return false;
    }

    const bool showOk = CreateAndEnableHook(
        showTarget,
        reinterpret_cast<void*>(&hkShowTimeCigaretteUi),
        reinterpret_cast<void**>(&g_OrigShow));

    if (showOk)
    {
        g_ShowTarget = showTarget;
        g_Installed = true;
    }

    void* hideTarget = ResolveGameAddress(gAddr.TimeCigaretteActionPluginImpl_HideTimeCigaretteUi);
    if (hideTarget && CreateAndEnableHook(
            hideTarget,
            reinterpret_cast<void*>(&hkHideTimeCigaretteUi),
            reinterpret_cast<void**>(&g_OrigHide)))
        g_HideTarget = hideTarget;
    else
        Log("[TimeCigaretteUi] WARN: Hide hook failed - TimeCigaretteUi will not fire on hide.\n");

    Log("[TimeCigDiag] install show=%s (target=%p) hide=%s (target=%p)\n",
        showOk ? "OK" : "FAIL", showTarget, g_HideTarget ? "OK" : "off", hideTarget);
    return showOk;
}

bool Uninstall_TimeCigaretteUi_Hook()
{
    if (!g_Installed)
        return true;

    if (g_ShowTarget) DisableAndRemoveHook(g_ShowTarget);
    if (g_HideTarget) DisableAndRemoveHook(g_HideTarget);

    g_OrigShow   = nullptr;
    g_OrigHide   = nullptr;
    g_ShowTarget = nullptr;
    g_HideTarget = nullptr;
    g_Installed  = false;
    g_UiController.store(nullptr, std::memory_order_relaxed);
    return true;
}
