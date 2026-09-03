#include "pch.h"

#include "UiEquipPreviewControllerImpl_ScrollNext.h"

#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    using PreviewScroll_t = void (__fastcall*)(void* ctl);
    using StartEquipPreviewImpl_t =
        void (__fastcall*)(void* panel, void* info, float arg3);
    using GetQuarkSystemTable_t = void* (__fastcall*)();

    static PreviewScroll_t         g_OrigScrollNext = nullptr;
    static PreviewScroll_t         g_OrigScrollPrev = nullptr;
    static StartEquipPreviewImpl_t g_OrigStart      = nullptr;

    static bool g_ScrollInstalled = false;
    static bool g_StartInstalled  = false;

    static void* LiveDevelopController()
    {
        __try
        {
            const auto get = reinterpret_cast<GetQuarkSystemTable_t>(
                ResolveGameAddress(gAddr.Fox_GetQuarkSystemTable));
            if (!get) return nullptr;
            std::uint8_t* t = reinterpret_cast<std::uint8_t*>(get());
            if (!t) return nullptr;
            std::uint8_t* a = *reinterpret_cast<std::uint8_t**>(t + 0x98);
            if (!a) return nullptr;
            std::uint8_t* b = *reinterpret_cast<std::uint8_t**>(a + 0x110);
            if (!b) return nullptr;
            return *reinterpret_cast<void**>(b + 0xAC8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void RefreshDevelopControllerCache(void* ctl)
    {
        if (!ctl) return;
        void* live = LiveDevelopController();
        if (!live) return;

        __try
        {
            void** cached = reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(ctl) + 0x88);
            if (*cached == live) return;

            void* stale = *cached;
            *cached = live;

            static std::atomic<int> s_staleLogged{ 0 };
            if (s_staleLogged.fetch_add(1) < 8)
                LogDebug("[EquipPreview] the preview controller held a stale "
                         "develop-controller pointer (%p -> %p) - the record block "
                         "was relocated after it cached one, so every cursor-move "
                         "preview resolved to equip id 0; repointed to the live "
                         "block\n",
                    stale, live);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void __fastcall hkScrollNext(void* ctl)
    {
        RefreshDevelopControllerCache(ctl);
        g_OrigScrollNext(ctl);
    }

    static void __fastcall hkScrollPrev(void* ctl)
    {
        RefreshDevelopControllerCache(ctl);
        g_OrigScrollPrev(ctl);
    }

    static void __fastcall hkStartEquipPreviewImpl(void* panel, void* info,
                                                   float arg3)
    {
        __try
        {
            RefreshDevelopControllerCache(*reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(panel) + 0x90));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        g_OrigStart(panel, info, arg3);
    }
}

namespace equip
{
    bool Install_UiEquipPreviewControllerScroll()
    {
        if (g_ScrollInstalled) return true;

        void* nextTarget =
            ResolveGameAddress(gAddr.UiEquipPreviewController_ScrollNext);
        void* prevTarget =
            ResolveGameAddress(gAddr.UiEquipPreviewController_ScrollPrev);
        if (!nextTarget || !prevTarget)
        {
            LogDebug("[EquipPreview] ScrollNext/ScrollPrev unresolved on this build "
                     "- the preview controller keeps its pre-migration "
                     "develop-controller pointer, so cursor-move previews resolve "
                     "to equip id 0 and only menu-open rows render\n");
            return false;
        }

        const bool a = CreateAndEnableHook(
            nextTarget, reinterpret_cast<void*>(&hkScrollNext),
            reinterpret_cast<void**>(&g_OrigScrollNext));
        const bool b = CreateAndEnableHook(
            prevTarget, reinterpret_cast<void*>(&hkScrollPrev),
            reinterpret_cast<void**>(&g_OrigScrollPrev));

        if (!a || !b)
            Log("[EquipPreview] scroll hook install FAILED (next=%p ok=%d, "
                "prev=%p ok=%d); cursor-move previews will render nothing\n",
                nextTarget, a ? 1 : 0, prevTarget, b ? 1 : 0);

        void* startTarget =
            ResolveGameAddress(gAddr.ItemSelector_StartEquipPreviewImpl);
        if (startTarget)
        {
            g_StartInstalled = CreateAndEnableHook(
                startTarget, reinterpret_cast<void*>(&hkStartEquipPreviewImpl),
                reinterpret_cast<void**>(&g_OrigStart));
            if (!g_StartInstalled)
                Log("[EquipPreview] StartEquipPreviewImpl hook FAILED (target=%p) - "
                    "the stale develop-controller pointer only heals on the first "
                    "cursor move, so a tab rebuild before that reloads nothing\n", startTarget);
        }

        g_ScrollInstalled = a && b;
        return g_ScrollInstalled;
    }

    void Uninstall_UiEquipPreviewControllerScroll()
    {
        if (g_StartInstalled)
        {
            if (void* t =
                    ResolveGameAddress(gAddr.ItemSelector_StartEquipPreviewImpl))
                DisableAndRemoveHook(t);
            g_OrigStart      = nullptr;
            g_StartInstalled = false;
        }
        if (!g_ScrollInstalled) return;
        if (void* t =
                ResolveGameAddress(gAddr.UiEquipPreviewController_ScrollPrev))
            DisableAndRemoveHook(t);
        if (void* t =
                ResolveGameAddress(gAddr.UiEquipPreviewController_ScrollNext))
            DisableAndRemoveHook(t);
        g_OrigScrollNext  = nullptr;
        g_OrigScrollPrev  = nullptr;
        g_ScrollInstalled = false;
    }
}
