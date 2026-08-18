#include "pch.h"

#include "ItemSelectorCallbackImpl_AddDevelopWeaponList.h"

#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"

#include "../equip/DevelopArrayGrow.h"
#include "../equip/EquipDevelop_SetEquipUndeveloped.h"
#include "EquipDevelopControllerImpl_GetSuitDevelopInfoIndex.h"

namespace
{
    using AddDevelopWeaponList_t =
        std::uint32_t (__fastcall*)(void* panel, std::uint32_t tab,
                                    std::uint32_t startRow, void* curItems);
    using RootCount_t =
        std::uint16_t (__fastcall*)(void* edc, std::uint32_t tab);
    using RowGate_t =
        std::uint8_t (__fastcall*)(void* edc, std::uint32_t flow);
    using VarCount_t =
        std::uint16_t (__fastcall*)(void* edc, std::uint32_t flow,
                                    std::uint8_t b);
    using VarFill_t =
        void (__fastcall*)(void* edc, std::uint32_t flow,
                           std::uint32_t count, std::uint16_t* out);
    using VarColumn_t =
        std::uint8_t (__fastcall*)(void* edc, std::uint32_t flow,
                                   std::uint8_t b);
    using AddRecord_t =
        void (__fastcall*)(void* panel, std::uint32_t flow, void* curItems,
                           std::uint32_t row, std::uint32_t col,
                           std::uint32_t a6, std::uint32_t a7,
                           std::uint32_t a8);
    using SupplyIdx_t =
        std::uint16_t (__fastcall*)(void* panel, std::uint32_t flow);

    static AddDevelopWeaponList_t g_Orig     = nullptr;
    static bool                   g_Installed = false;

    constexpr std::uint32_t kNativeRootCap = 0x14;
    constexpr std::uint32_t kRootCap       = 0x200;
    constexpr std::uint32_t kVarCap        = 0x100;
    constexpr std::uint32_t kRowCap        = 0x400;

    constexpr std::uint32_t kFallback        = 0xFFFFFFFFu;
    constexpr std::uint32_t kFirstCustomFlow = 922;

    static std::uint32_t CollectTabRoots(void* edc, std::uint32_t tab,
                                         std::uint16_t* out)
    {
        const std::uintptr_t vtbl = *reinterpret_cast<std::uintptr_t*>(edc);
        const auto isVisible =
            *reinterpret_cast<const RowGate_t*>(vtbl + 0x100);
        if (!isVisible)
            return 0;
        const auto* base = reinterpret_cast<const std::uint8_t*>(edc);
        std::uint32_t count = 0;
        for (std::uint32_t row = 0; row < equip::NativeFlowBound(); ++row)
        {
            const std::uint8_t* rec = base
                + static_cast<std::size_t>(row) * 0x68;
            if (*reinterpret_cast<const std::uint16_t*>(rec + 0x8) == 0)
            {
                if (equip::IsReservedFlowRow(row))
                    continue;
                break;
            }
            if (equip::IsReservedFlowRow(row))
                continue;
            if (row >= kFirstCustomFlow)
            {
                static std::atomic<int> s_tabProbe[64]{};
                if (s_tabProbe[tab & 63].fetch_add(1) < 48)
                    LogDebug("[DevelopTabProbe] flow=%u rec[0x3E]=%u while building tab=%u "
                        "-> %s. rec[0x3E] is the ONLY thing this list filters on; if it "
                        "does not equal the equipDevelopTypeID the Lua declared for this "
                        "row, the row is filed under that other category and renders "
                        "there instead\n",
                        row, static_cast<unsigned>(rec[0x3E]), tab,
                        rec[0x3E] == static_cast<std::uint8_t>(tab) ? "KEPT" : "skipped");
            }
            if (rec[0x3E] != static_cast<std::uint8_t>(tab))
                continue;
            if (*reinterpret_cast<const std::uint16_t*>(rec + 0xA) != 0)
                continue;
            if (row >= kFirstCustomFlow)
            {
                if (outfit::IsDevelopHidden(static_cast<unsigned short>(row)))
                    continue;
            }
            else if (!isVisible(edc, row))
                continue;
            if (count >= kRootCap)
            {
                static std::atomic<int> s_capLogged{ 0 };
                if (s_capLogged.fetch_add(1) < 4)
                    LogDebug("[WeaponListRoots] tab %u has more than %u base "
                        "develop rows - the extended list path holds %u, "
                        "rows past that will not list\n",
                        tab, kRootCap, kRootCap);
                break;
            }
            out[count++] = static_cast<std::uint16_t>(row);
        }
        return count;
    }

    static std::uint32_t BuildOverCapList(void* panel, std::uint32_t tab,
                                          std::uint32_t startRow,
                                          void* curItems,
                                          const std::uint16_t* roots,
                                          std::uint32_t rootCount)
    {
        void* edc = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(panel) + 0x58);
        const std::uintptr_t vtbl = *reinterpret_cast<std::uintptr_t*>(edc);
        const auto rowGate  = *reinterpret_cast<const RowGate_t*>(vtbl + 0x1C8);
        const auto varCount = *reinterpret_cast<const VarCount_t*>(vtbl + 0x160);
        const auto varFill  = *reinterpret_cast<const VarFill_t*>(vtbl + 0x168);
        const auto varCol   = *reinterpret_cast<const VarColumn_t*>(vtbl + 0x188);
        const auto addRec   = reinterpret_cast<AddRecord_t>(
            ResolveGameAddress(gAddr.ItemSelectorCallbackImpl_AddRecord));
        const auto supplyIdx = reinterpret_cast<SupplyIdx_t>(
            ResolveGameAddress(
                gAddr.ItemSelectorCallbackImpl_DecideActMotherBaseDeviceSupportDropMode));
        if (!rowGate || !varCount || !varFill || !varCol
            || !addRec || !supplyIdx)
            return kFallback;

        static std::atomic<std::uint64_t> s_diagTabs{ 0 };
        const std::uint64_t tabBit = 1ull << (tab & 63);
        const bool diag = (s_diagTabs.fetch_or(tabBit) & tabBit) == 0;
        const auto* recBase = reinterpret_cast<const std::uint8_t*>(edc);
        const auto recEq = [recBase](std::uint32_t f) -> unsigned {
            return *reinterpret_cast<const std::uint16_t*>(
                recBase + static_cast<std::size_t>(f) * 0x68 + 0x8);
        };
        const auto recParent = [recBase](std::uint32_t f) -> unsigned {
            return *reinterpret_cast<const std::uint16_t*>(
                recBase + static_cast<std::size_t>(f) * 0x68 + 0xA);
        };

        std::uint16_t vars[kVarCap];
        std::uint32_t nextRow = startRow;
        for (std::uint32_t i = 0; i < rootCount; ++i)
        {
            const std::uint32_t flow = roots[i];
            if (!rowGate(edc, flow))
            {
                if (diag && flow >= kFirstCustomFlow)
                    LogDebug("[WeaponListPick] tab %u ROOT flow=%u eq=%u SKIPPED "
                        "rowGate=0 - this base row is dropped from the list "
                        "entirely\n",
                        tab, flow, recEq(flow));
                continue;
            }
            addRec(panel, flow, curItems, nextRow, 0, 0, 0xFF, 1);
            if (diag && flow >= kFirstCustomFlow)
                LogDebug("[WeaponListPick] tab %u ROOT flow=%u eq=%u LISTED row=%u "
                    "- this base row DOES render in this category\n",
                    tab, flow, recEq(flow), nextRow);
            std::uint16_t vc = varCount(edc, flow, 1);
            if (vc > kVarCap)
            {
                static std::atomic<int> s_varLogged{ 0 };
                if (s_varLogged.fetch_add(1) < 4)
                    LogDebug("[WeaponListRoots] root flow %u has %u variant rows "
                        "- the extended list path holds %u, variants past "
                        "that will not list\n",
                        flow, static_cast<unsigned>(vc), kVarCap);
                vc = kVarCap;
            }
            varFill(edc, flow, vc, vars);
            std::uint32_t maxRow = nextRow;
            for (std::uint32_t j = 0; j < vc; ++j)
            {
                const std::uint32_t vflow = vars[j];
                const bool dv = diag && vflow >= kFirstCustomFlow;
                if (!rowGate(edc, vflow))
                {
                    if (dv)
                        LogDebug("[WeaponListPick] tab %u root=%u VAR flow=%u eq=%u "
                            "parent=%u SKIPPED rowGate=0 - the variant is not "
                            "listed, so picking its R&D row resolves to no "
                            "equip\n",
                            tab, flow, vflow, recEq(vflow), recParent(vflow));
                    continue;
                }
                const std::uint32_t row = nextRow + varCol(edc, vflow, 1);
                if (row >= kRowCap)
                {
                    if (dv)
                        LogDebug("[WeaponListPick] tab %u root=%u VAR flow=%u eq=%u "
                            "SKIPPED row=%u past the %u-row list cap\n",
                            tab, flow, vflow, recEq(vflow), row, kRowCap);
                    continue;
                }
                if (maxRow < row)
                    maxRow = row;
                const std::uint16_t vi = supplyIdx(panel, vflow);
                if (vi >= 0xF)
                {
                    if (dv)
                        LogDebug("[WeaponListPick] tab %u root=%u VAR flow=%u eq=%u "
                            "parent=%u row=%u SKIPPED supplyIdx=%u >= 15 - the "
                            "row renders in R&D but is never added to the "
                            "selector, so picking it equips nothing\n",
                            tab, flow, vflow, recEq(vflow), recParent(vflow),
                            row, static_cast<unsigned>(vi));
                    continue;
                }
                if (dv)
                    LogDebug("[WeaponListPick] tab %u root=%u VAR flow=%u eq=%u "
                        "parent=%u -> row=%u col=%u varCol=%u\n",
                        tab, flow, vflow, recEq(vflow), recParent(vflow), row,
                        static_cast<unsigned>(vi),
                        static_cast<unsigned>(varCol(edc, vflow, 1)));
                addRec(panel, vflow, curItems, row, vi, 0, 0xFF, 1);
            }
            nextRow = maxRow + 1;
            if (nextRow >= kRowCap)
                return kRowCap;
        }
        return nextRow;
    }

    static std::uint32_t __fastcall hkAddDevelopWeaponList(
        void* panel, std::uint32_t tab, std::uint32_t startRow, void* curItems)
    {
        MISSION_GUARD_ORIGINAL_RET(g_Orig, panel, tab, startRow, curItems);

        EquipDevelop_DrainIfRestorePending();

        std::uint32_t r = kFallback;
        std::uint16_t roots[kRootCap];
        __try
        {
            void* edc = panel
                ? *reinterpret_cast<void**>(
                      reinterpret_cast<std::uint8_t*>(panel) + 0x58)
                : nullptr;
            if (edc)
            {
                const std::uintptr_t vtbl =
                    *reinterpret_cast<std::uintptr_t*>(edc);
                const auto rootCount =
                    *reinterpret_cast<const RootCount_t*>(vtbl + 0x140);
                const std::uint16_t n =
                    rootCount ? rootCount(edc, tab) : 0;
                const std::uint32_t ours = CollectTabRoots(edc, tab, roots);
                if (ours > kNativeRootCap || ours > n)
                {
                    static std::atomic<int> s_engageLogged{ 0 };
                    if (s_engageLogged.fetch_add(1) < 12)
                    {
                        if (n > kNativeRootCap)
                            LogDebug("[WeaponListRoots] tab %u rebuilt by the "
                                "extended path (handled): its %u root rows "
                                "overflow the native 20-entry base-row buffer, "
                                "which would otherwise render the whole "
                                "category empty in the prep/supply list\n",
                                tab, static_cast<unsigned>(ours));
                        else
                            LogDebug("[WeaponListRoots] tab %u rebuilt by the "
                                "extended path (handled): the native root scan "
                                "sees only %u of %u base rows, because "
                                "IsEquipVisile refuses custom rows whose "
                                "equipId lies outside the vanilla item range\n",
                                tab, static_cast<unsigned>(n), ours);
                    }
                    r = BuildOverCapList(panel, tab, startRow, curItems,
                                         roots, ours);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[WeaponListRoots] exception rebuilding tab %u - falling "
                "back to the native build (the tab lists empty when its "
                "base-row count exceeds 20)\n", tab);
            r = kFallback;
        }
        if (r != kFallback)
            return r;
        return g_Orig(panel, tab, startRow, curItems);
    }
}

namespace outfit
{
    bool Install_DevelopWeaponList_Hook()
    {
        if (g_Installed) return true;

        void* target = ResolveGameAddress(gAddr.ItemSelector_AddDevelopWeaponList);
        if (!target)
        {
            LogDebug("[WeaponListRoots] target unresolved; module disabled (a "
                "prep/supply weapon tab with more than 20 base develop rows "
                "lists EMPTY on this build)\n");
            return false;
        }

        g_Installed = CreateAndEnableHook(
            target,
            reinterpret_cast<void*>(&hkAddDevelopWeaponList),
            reinterpret_cast<void**>(&g_Orig));

        if (!g_Installed)
            Log("[WeaponListRoots] hook install FAILED (target=%p); a "
                "prep/supply weapon tab with more than 20 base develop rows "
                "lists EMPTY\n", target);

        return g_Installed;
    }

    void Uninstall_DevelopWeaponList_Hook()
    {
        if (!g_Installed) return;
        if (void* t = ResolveGameAddress(gAddr.ItemSelector_AddDevelopWeaponList))
            DisableAndRemoveHook(t);
        g_Orig      = nullptr;
        g_Installed = false;
    }
}
