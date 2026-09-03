#include "pch.h"

#include "ItemSelectorCallbackImpl_DecideActMissionPreparationSetEquipMode.h"
#include "OutfitRegistry.h"
#include "ItemSelectorCallbackImpl_AddListSuit.h"
#include "Player2UtilityImpl_SetSuitAndHandConditionWithLoadoutInfo.h"
#include "CustomHeadRegistry.h"

#include <cstdint>
#include <unordered_set>

#include "../equip/DevelopArrayGrow.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"
#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"

namespace
{


    using DecideActMissionPrep_t = void* (__fastcall*)(
        void* self, void* out, void* p3, void* p4);

    static DecideActMissionPrep_t g_OrigDecideActMissionPrep = nullptr;
    static DecideActMissionPrep_t g_OrigDecideActSupplyDrop  = nullptr;
    static DecideActMissionPrep_t g_OrigDecideActCustomize   = nullptr;
    static bool                    g_Installed                = false;
    static bool                    g_InstalledSupplyDrop      = false;
    static bool                    g_InstalledCustomize       = false;


    using SetSupplyCBoxInfo_t = void (__fastcall*)(
        void* self, std::uint16_t flowIndex);

    static SetSupplyCBoxInfo_t g_OrigSetSupplyCBoxInfo = nullptr;
    static bool                g_InstalledSetSupplyCBox = false;


    static std::unordered_set<std::uint32_t> g_UnresolvedSuitClickLogged;

    constexpr std::uint32_t kEquipKindMissionPrepSuit = 0x80;


    constexpr std::uint32_t kEquipKindSupplyDropSuit = 0x100;
    constexpr std::uint32_t kEquipKindArm            = 0x40;

    struct SelectionSample
    {
        bool          haveSample   = false;
        std::uint32_t equipKind    = 0;
        std::uint16_t selectedId   = 0xFFFF;
        std::uint8_t  selectorCode = 0xFF;
        std::size_t   cellIndex    = 0;
    };

    static bool TryReadSelection(void* self, SelectionSample& out)
    {
        out = {};
        if (!self) return false;

        auto* base = reinterpret_cast<std::uint8_t*>(self);

        __try
        {
            const std::uint32_t equipKind = *reinterpret_cast<std::uint32_t*>(base + 0x4434);


            const std::uint32_t count    = *reinterpret_cast<std::uint32_t*>(base + 0x104);
            const std::int32_t baseRow   = *reinterpret_cast<std::int32_t*>(base + 0x10C);
            const std::int32_t cursorRow = *reinterpret_cast<std::int32_t*>(base + 0x110);

            const std::int32_t row = (count != 0)
                ? static_cast<std::int32_t>(
                      static_cast<std::uint32_t>(baseRow + cursorRow) % count)
                : 0;

            if (row < 0 || row > static_cast<std::int32_t>(outfit::kPanelRowMax)) return false;

            const std::uint8_t variant = *(base + 0xC040 + row);
            if (variant > 14) return false;

            const std::size_t cellIndex = static_cast<std::size_t>(row) * 15 + variant;
            if (cellIndex >= 0x3C00) return false;

            const std::uint16_t selectedId =
                *reinterpret_cast<std::uint16_t*>(base + 0x4440 + cellIndex * 2);

            const std::uint32_t selectorCodeU32 =
                *reinterpret_cast<std::uint32_t*>(base + 0xCC40 + cellIndex * 12);

            out.haveSample    = true;
            out.equipKind     = equipKind;
            out.selectedId    = selectedId;
            out.selectorCode  = static_cast<std::uint8_t>(selectorCodeU32 & 0xFF);
            out.cellIndex     = cellIndex;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }


    static std::uint16_t MatchSelectionToOutfit(const SelectionSample& s,
                                                bool activateVariant)
    {


        if (s.selectorCode >= outfit::kCustomSelectorStart
            && s.selectorCode <= outfit::kCustomSelectorEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            std::uint8_t variantIdx = 0;
            if (outfit::TryGetOutfitByVariantSelector(
                    s.selectorCode, &entry, &variantIdx) && entry)
            {
                const outfit::OutfitEntry* rowEntry = nullptr;
                if (outfit::TryGetOutfitByFlowIndex(s.selectedId, &rowEntry)
                    && rowEntry && rowEntry != entry)
                {
                    Log("[OutfitItemSelector] WARN stale variant cell: selector "
                        "0x%02X resolves to developId=%u but the clicked row is "
                        "developId=%u - trusting the row (the byte was rotated to "
                        "another outfit)\n",
                        static_cast<unsigned>(s.selectorCode),
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(rowEntry->developId));
                    entry = nullptr;
                }
            }
            if (entry)
            {
                if (activateVariant)
                {
                    outfit::ClearCrateDeliveredVariant();
                    outfit::ConsumePendingSupplyDropVariantIdx();
                    outfit::ConsumePendingSupplyDropDevelopId();
                    outfit::SetActiveVariant(entry->partsType, variantIdx);
                }
                return entry->developId;
            }
        }


        const outfit::OutfitEntry* byFlow = nullptr;
        if (outfit::TryGetOutfitByFlowIndex(s.selectedId, &byFlow) && byFlow)
        {
            const std::uint16_t flowDevId = byFlow->developId;
            if (activateVariant)
            {
                outfit::ClearCrateDeliveredVariant();
                outfit::ConsumePendingSupplyDropVariantIdx();
                outfit::ConsumePendingSupplyDropDevelopId();
                if (!byFlow->bound
                    && outfit::BindOutfit(flowDevId, true, "equip-click"))
                    outfit::TryGetOutfitByFlowIndex(s.selectedId, &byFlow);
                if (byFlow && byFlow->bound)
                    outfit::SetActiveVariant(byFlow->partsType, 0);
            }
            return flowDevId;
        }


        const outfit::OutfitEntry* byDev = nullptr;
        if (outfit::TryGetOutfitByDevelopId(s.selectedId, &byDev) && byDev)
        {
            const std::uint16_t devDevId = byDev->developId;
            if (activateVariant)
            {
                outfit::ClearCrateDeliveredVariant();
                outfit::ConsumePendingSupplyDropVariantIdx();
                outfit::ConsumePendingSupplyDropDevelopId();
                if (!byDev->bound
                    && outfit::BindOutfit(devDevId, true, "equip-click"))
                    outfit::TryGetOutfitByDevelopId(s.selectedId, &byDev);
                if (byDev && byDev->bound)
                    outfit::SetActiveVariant(byDev->partsType, 0);
            }
            return devDevId;
        }

        return 0;
    }


    constexpr std::uint32_t kEquipKindHeadOption = 0x201;


    static SelectionSample ProcessSelectionAndPublish(void* self, const char* tag,
                                                      bool activateVariant)
    {
        SelectionSample s{};
        const bool haveSample = TryReadSelection(self, s) && s.haveSample;

        if (haveSample)
        {
            const bool isSuitClick = (s.equipKind == kEquipKindMissionPrepSuit
                                   || s.equipKind == kEquipKindSupplyDropSuit);
            const bool isHeadOption = (s.equipKind == kEquipKindHeadOption);
            const bool isPrepEquip =
                activateVariant && s.equipKind == kEquipKindMissionPrepSuit;
            const std::uint16_t devId =
                isSuitClick ? MatchSelectionToOutfit(s, isPrepEquip) : 0;

            bool published = false;
            if (devId != 0)
            {
                bool bound = true;
                if (isPrepEquip && !outfit::IsOutfitBound(devId))
                    bound = outfit::BindOutfit(devId, true, "equip-click");
                if (bound)
                {
                    if (isPrepEquip)
                        outfit::NoteOutfitApplied(devId);
                    outfit::SetPendingOutfitDevelopId(devId);
                    published = true;
                }
                else
                {
                    LogDebug("[OutfitItemSelector:%s] equip refused: live-byte "
                             "pools full and every bound outfit is pinned - "
                             "developId=%u stays vanilla; unequip something to free "
                             "a slot\n", tag, static_cast<unsigned>(devId));
                }
            }
            if (devId == 0 && isSuitClick)
            {
                const std::uint32_t sig =
                    (static_cast<std::uint32_t>(s.selectedId) << 8)
                    | static_cast<std::uint32_t>(s.selectorCode);
                if (g_UnresolvedSuitClickLogged.insert(sig).second)
                {
                    const outfit::OutfitEntry* byFlow = nullptr;
                    const outfit::OutfitEntry* byDev  = nullptr;
                    const bool haveFlow =
                        outfit::TryGetOutfitByFlowIndex(s.selectedId, &byFlow)
                        && byFlow;
                    const bool haveDev =
                        outfit::TryGetOutfitByDevelopId(s.selectedId, &byDev)
                        && byDev;
                    LogDebug("[OutfitItemSelector:%s] suit click matched no "
                             "registered outfit: flowIndex=%u selector=0x%02X "
                             "equipKind=0x%X byFlow=%s byDevelop=%s - no partsType "
                             "activated and no pending developId, so the engine "
                             "falls back to the vanilla suit\n",
                        tag,
                        static_cast<unsigned>(s.selectedId),
                        static_cast<unsigned>(s.selectorCode),
                        static_cast<unsigned>(s.equipKind),
                        haveFlow ? "hit" : "miss",
                        haveDev ? "hit" : "miss");
                }
            }

            if (!published && isSuitClick)
                outfit::ClearPendingOutfitDevelopId();


            if (isHeadOption && s.selectedId != 0xFFFF)
            {
                outfit::SetPendingHeadOptionEquipId(s.selectedId);
#ifdef _DEBUG
                LogDebug("[OutfitItemSelector:%s] head-option stash: "
                    "equipId=0x%X (will be re-injected into faceEquipId "
                    "during apply if orig drops it)\n",
                    tag,
                    static_cast<unsigned>(s.selectedId));
#endif
            }
        }
        else
        {
#ifdef _DEBUG
            Log("[OutfitItemSelector:%s] fire: selection-state read failed\n", tag);
#endif
        }
        return s;
    }

    struct VextCellSwap
    {
        std::uint32_t* cellWord = nullptr;
        std::uint32_t  saved    = 0;
    };

    static VextCellSwap SwapInVextSelector(void* self,
                                           const SelectionSample& s,
                                           const char* tag)
    {
        static_cast<void>(tag);
        VextCellSwap sw{};
        if (!self || !s.haveSample) return sw;
        if (s.selectorCode >= outfit::kCustomSelectorStart) return sw;

        const std::uint8_t sel = outfit::VextLookupCellSelector(
            s.selectedId, static_cast<std::uint8_t>(s.cellIndex % 15));
        if (sel < outfit::kCustomSelectorStart
            || sel > outfit::kCustomSelectorEnd)
            return sw;

        __try
        {
            auto* cell = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(self)
                + 0xCC40 + s.cellIndex * 12);
            sw.saved    = *cell;
            *cell       = (sw.saved & 0xFFFFFF00u)
                        | static_cast<std::uint32_t>(sel);
            sw.cellWord = cell;
#ifdef _DEBUG
            LogDebug("[OutfitItemSelector:%s] vext selector swap-in: flowIndex=%u "
                "cell=%zu 0x%08X -> 0x%08X (restored after orig)\n",
                tag, static_cast<unsigned>(s.selectedId), s.cellIndex,
                sw.saved, *cell);
#endif
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            sw.cellWord = nullptr;
        }
        return sw;
    }

    static void RestoreVextSelector(const VextCellSwap& sw)
    {
        if (!sw.cellWord) return;
        __try
        {
            *sw.cellWord = sw.saved;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void FixupSupplyCellSelector(void* self, const SelectionSample& s,
                                        const char* tag)
    {
        if (!self || !s.haveSample) return;
        if (s.selectorCode != 0xFF) return;
        if (s.equipKind != 0xFF) return;

        const outfit::OutfitEntry* entry = nullptr;
        bool resolves =
            outfit::TryGetOutfitByFlowIndex(s.selectedId, &entry) && entry;
        const bool knownAsOutfit = resolves;
        if (resolves && !entry->bound)
        {
            if (outfit::BindOutfit(entry->developId, true, "supply-order"))
                resolves = outfit::TryGetOutfitByFlowIndex(s.selectedId, &entry)
                           && entry;
            else
                resolves = false;
        }
        if (resolves && entry)
            outfit::NoteOutfitOrdered(entry->developId);

        constexpr std::uint16_t kCustomFlowFirst = 922;
        constexpr std::uint16_t kCustomFlowLast  =
            static_cast<std::uint16_t>(equip::kMaxFlowSlots - 1);
        std::uint8_t newSelector = 0;
        const char*  why         = nullptr;
        if (resolves
            && entry->defaultVariant < outfit::kMaxVariantsPerOutfit)
        {
            newSelector = entry->variantSelectorCodes[entry->defaultVariant];
            why = "single custom outfit";
        }
        else if (s.selectedId >= kCustomFlowFirst && s.selectedId <= kCustomFlowLast)
        {
            if (!knownAsOutfit
                && EquipDevelopAdd::IsManagedFlowIndex(
                       static_cast<std::uint16_t>(s.selectedId)))
            {
#ifdef _DEBUG
                LogDebug("[OutfitItemSelector:%s] supply-cell left alone: "
                         "flowIndex=%u cell=%zu is a registered NON-OUTFIT custom "
                         "row - the vanilla-suit degrade would stamp a suit "
                         "selector into a weapon/item crate\n",
                    tag, static_cast<unsigned>(s.selectedId), s.cellIndex);
#endif
                return;
            }
            newSelector = 0x4F;
            why = "UNREGISTERED custom row - degraded to coherent VANILLA crate";
        }
        else
        {
            return;
        }

        __try
        {
            auto* cell = reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(self)
                + 0xCC40 + s.cellIndex * 12);
            const std::uint32_t prev = *cell;
            *cell = (prev & 0xFFFFFF00u)
                  | static_cast<std::uint32_t>(newSelector);
#ifdef _DEBUG
            LogDebug("[OutfitItemSelector:%s] supply-cell selector fixup: "
                     "flowIndex=%u cell=%zu 0x%08X -> 0x%08X (%s: the crate payload "
                     "now carries selector 0x%02X so the pickup resolver produces a "
                     "coherent suit)\n",
                tag,
                static_cast<unsigned>(s.selectedId),
                s.cellIndex, prev, *cell,
                why,
                static_cast<unsigned>(newSelector));
#endif
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[OutfitItemSelector:%s] SEH writing supply-cell selector "
                "(cell=%zu) - skipped\n", tag, s.cellIndex);
        }
    }

    static void* __fastcall hkDecideActMissionPrep(
        void* self, void* out, void* p3, void* p4)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigDecideActMissionPrep(self, out, p3, p4);

        const SelectionSample sPrep = ProcessSelectionAndPublish(self, "prep", true);
        FixupSupplyCellSelector(self, sPrep, "prep");
        VextCellSwap vswPrep = SwapInVextSelector(self, sPrep, "prep");
        if (!vswPrep.cellWord && sPrep.haveSample
            && sPrep.equipKind == kEquipKindMissionPrepSuit)
        {
            outfit::ResetAllVanillaExtVariants();
#ifdef _DEBUG
            LogDebug("[OutfitItemSelector:prep] non-vext suit pick "
                "(flowIndex=%u selector=0x%02X) - vext variants reset "
                "(prep applies bypass SetSuit)\n",
                static_cast<unsigned>(sPrep.selectedId),
                static_cast<unsigned>(sPrep.selectorCode));
#endif
        }
        if (sPrep.haveSample
            && (sPrep.equipKind == kEquipKindSupplyDropSuit
                || sPrep.equipKind == 0xFF))
        {
            const outfit::OutfitEntry* entry = nullptr;
            std::uint8_t variantIdx = 0;
            bool matched = false;
            if (sPrep.selectorCode >= outfit::kCustomSelectorStart
                && sPrep.selectorCode <= outfit::kCustomSelectorEnd
                && outfit::TryGetOutfitByVariantSelector(
                       sPrep.selectorCode, &entry, &variantIdx)
                && entry)
            {
                matched = true;
            }
            else if (outfit::TryGetOutfitByFlowIndex(sPrep.selectedId, &entry)
                     && entry)
            {
                matched = true;
                variantIdx = entry->defaultVariant;
            }
            else if (outfit::TryGetOutfitByDevelopId(sPrep.selectedId, &entry)
                     && entry)
            {
                matched = true;
                variantIdx = entry->defaultVariant;
            }
            if (matched && entry
                && !outfit::IsOutfitBound(entry->developId)
                && !outfit::BindOutfit(entry->developId, true, "order-click"))
            {
                LogDebug("[OutfitItemSelector:prep] order refused: live byte pools "
                    "full for developId=%u - unequip an outfit to free a "
                    "slot\n", static_cast<unsigned>(entry->developId));
                matched = false;
            }
            if (matched && entry)
            {
                outfit::NoteOutfitOrdered(entry->developId);
                outfit::SetPendingSupplyDropDevelopId(entry->developId);
                outfit::SetPendingSupplyDropVariantIdx(variantIdx);
                outfit::SetPendingOutfitDevelopId(entry->developId);
#ifdef _DEBUG
                LogDebug("[OutfitItemSelector:prep] supply ORDER of custom outfit "
                         "developId=%u variantIdx=%u - stashed for crate pickup "
                         "(the crate delivers this variant)\n",
                    static_cast<unsigned>(entry->developId),
                    static_cast<unsigned>(variantIdx));
#endif
            }
        }
        outfit::SetHeadEquipDecideActive(true);
        void* r = g_OrigDecideActMissionPrep(self, out, p3, p4);
        outfit::SetHeadEquipDecideActive(false);
        RestoreVextSelector(vswPrep);
        return r;
    }

    static void* __fastcall hkDecideActCustomize(
        void* self, void* out, void* p3, void* p4)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigDecideActCustomize(self, out, p3, p4);

        const SelectionSample s = ProcessSelectionAndPublish(self, "customize", true);
        VextCellSwap vswCust = SwapInVextSelector(self, s, "customize");
        outfit::SetHeadEquipDecideActive(true);
        void* r = g_OrigDecideActCustomize(self, out, p3, p4);
        outfit::SetHeadEquipDecideActive(false);
        RestoreVextSelector(vswCust);
        return r;
    }

    static void* __fastcall hkDecideActSupplyDrop(
        void* self, void* out, void* p3, void* p4)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigDecideActSupplyDrop(self, out, p3, p4);

        const SelectionSample sSup = ProcessSelectionAndPublish(self, "supply", false);
        FixupSupplyCellSelector(self, sSup, "supply");


        outfit::SetSupplyDropClickLatch();


        SelectionSample s{};
        if (TryReadSelection(self, s) && s.haveSample)
        {


            const bool isSuitClick = (s.equipKind == kEquipKindMissionPrepSuit
                                   || s.equipKind == kEquipKindSupplyDropSuit);
            const bool isCustomSelector =
                (s.selectorCode >= outfit::kCustomSelectorStart
                 && s.selectorCode <= outfit::kCustomSelectorEnd);

            if (isSuitClick || isCustomSelector)
            {


                const outfit::OutfitEntry* entry = nullptr;
                std::uint8_t variantIdx = 0;
                bool matched = false;

                if (isCustomSelector
                    && outfit::TryGetOutfitByVariantSelector(
                        s.selectorCode, &entry, &variantIdx)
                    && entry)
                {
                    matched = true;
                }
                else if (outfit::TryGetOutfitByFlowIndex(
                             s.selectedId, &entry) && entry)
                {
                    matched = true;
                    variantIdx = entry->defaultVariant;
                }
                else if (outfit::TryGetOutfitByDevelopId(
                             s.selectedId, &entry) && entry)
                {
                    matched = true;
                    variantIdx = entry->defaultVariant;
                }

                if (matched && entry
                    && !outfit::IsOutfitBound(entry->developId)
                    && !outfit::BindOutfit(entry->developId, true,
                                           "supply-click"))
                {
                    LogDebug("[OutfitItemSelector:supply] order refused: live byte "
                        "pools full for developId=%u - unequip an outfit to "
                        "free a slot\n",
                        static_cast<unsigned>(entry->developId));
                    matched = false;
                }

                if (matched && entry)
                {
                    outfit::SetPendingSupplyDropDevelopId(entry->developId);
                    outfit::SetPendingSupplyDropVariantIdx(variantIdx);


                    outfit::SetPendingOutfitDevelopId(entry->developId);

#ifdef _DEBUG
                    LogDebug("[OutfitItemSelector:supply] stashed "
                             "pendingSupplyDropDevelopId=%u variantIdx=%u "
                             "(equipKind=0x%X isSuitClick=%d isCustomSelector=%d) "
                             "for crate-pickup force-equip\n",
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(variantIdx),
                        s.equipKind,
                        isSuitClick ? 1 : 0,
                        isCustomSelector ? 1 : 0);
#endif
                }
            }
        }

        outfit::SetHeadEquipDecideActive(true);
        void* r = g_OrigDecideActSupplyDrop(self, out, p3, p4);
        outfit::SetHeadEquipDecideActive(false);
        return r;
    }


    static void __fastcall hkSetSupplyCBoxInfo(
        void* self, std::uint16_t flowIndex)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
        {
            if (g_OrigSetSupplyCBoxInfo)
                g_OrigSetSupplyCBoxInfo(self, flowIndex);
            return;
        }

        const outfit::OutfitEntry* entry = nullptr;
        bool isCustom =
            outfit::TryGetOutfitByFlowIndex(flowIndex, &entry) && entry;

        if (isCustom && !entry->bound)
        {
            if (outfit::BindOutfit(entry->developId, true, "rnd-request"))
                isCustom = outfit::TryGetOutfitByFlowIndex(flowIndex, &entry)
                           && entry;
            else
            {
                LogDebug("[OutfitItemSelector:devmenu] R&D request for developId=%u "
                         "not stashed: live byte pools full - unequip an outfit to "
                         "free a slot\n",
                    static_cast<unsigned>(entry->developId));
                isCustom = false;
            }
        }

        if (isCustom)
        {
            outfit::SetPendingOutfitDevelopId(entry->developId);
            outfit::SetPendingSupplyDropDevelopId(entry->developId);


            outfit::SetPendingSupplyDropVariantIdx(entry->defaultVariant);
#ifdef _DEBUG
            LogDebug("[OutfitItemSelector:devmenu] R&D request for custom outfit "
                     "flowIndex=%u developId=%u partsType=0x%02X selector=0x%02X - "
                     "stashed both pending ids (variantIdx=0) for crate-pickup "
                     "recovery\n",
                static_cast<unsigned>(flowIndex),
                static_cast<unsigned>(entry->developId),
                static_cast<unsigned>(entry->partsType),
                static_cast<unsigned>(entry->selectorCode));
#endif
        }

        if (g_OrigSetSupplyCBoxInfo)
            g_OrigSetSupplyCBoxInfo(self, flowIndex);

        if (outfit::IsCustomHeadEquipId(flowIndex) && self)
        {
            __try
            {
                *reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(self) + 0x23A0) = 0xFFFFFFFFu;
#ifdef _DEBUG
                LogDebug("[OutfitItemSelector:devmenu] custom HEAD row flowIndex=%u "
                         "is not orderable - drop request neutralized (heads equip "
                         "via the HEAD OPTION submenu)\n",
                    static_cast<unsigned>(flowIndex));
#endif
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[OutfitItemSelector:devmenu] SEH neutralizing head-row "
                    "drop request (self=%p)\n", self);
            }
            return;
        }

        if (isCustom && self)
        {
            __try
            {
                auto* base = reinterpret_cast<std::uint8_t*>(self);
                auto* reqType = reinterpret_cast<std::uint32_t*>(base + 0x23A0);
                auto* flags   = reinterpret_cast<std::uint32_t*>(base + 0x246C);
                auto* payload = base + 0x23B0;

                const std::uint32_t prevType  = *reqType;
                const std::uint32_t prevFlags = *flags;
                const std::uint8_t  prevCamo  = payload[1];

                *reqType   = 3;
                *flags     = 0x1u | 0x80u;
                payload[0] = 0;
                payload[1] = entry->variantSelectorCodes[entry->defaultVariant];
                payload[2] = 0;
                payload[3] = 0;

#ifdef _DEBUG
                LogDebug("[OutfitItemSelector:devmenu] post-orig "
                         "SupplyCboxDropRequest repair: type 0x%X->3 flags "
                         "0x%X->0x81 camo 0x%02X->0x%02X (flowIndex=%u) - coherent "
                         "suit crate; pickup resolves identity from the camo byte\n",
                    prevType, prevFlags, prevCamo, payload[1],
                    static_cast<unsigned>(flowIndex));
#endif
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[OutfitItemSelector:devmenu] SEH writing "
                         "SupplyCboxDropRequest at self+0x23A0..0x246C (self=%p) - "
                         "the offset assumption may be wrong\n", self);
            }
        }
    }
}

namespace outfit
{
    bool Install_OutfitItemSelector_Hook()
    {
        if (g_Installed && g_InstalledSupplyDrop) return true;

        if (!g_Installed)
        {
            void* target = ResolveGameAddress(
                gAddr.ItemSelectorCallbackImpl_DecideActMissionPreparationSetEquipMode);
            if (!target)
            {
                LogDebug("[OutfitItemSelector] prep target unresolved; module disabled\n");
                return false;
            }

            g_Installed = CreateAndEnableHook(
                target,
                reinterpret_cast<void*>(&hkDecideActMissionPrep),
                reinterpret_cast<void**>(&g_OrigDecideActMissionPrep));

            if (g_Installed)
                LogDebug("[OutfitItemSelector] prep installed OK (target=%p)\n",
                         target);
            else
                Log("[OutfitItemSelector] prep install FAILED (target=%p)\n",
                    target);
        }

        if (!g_InstalledSupplyDrop)
        {
            void* target = ResolveGameAddress(
                gAddr.ItemSelectorCallbackImpl_DecideActMotherBaseDeviceSupportDropMode);
            if (!target)
            {
                LogDebug("[OutfitItemSelector] supply-drop target unresolved; "
                    "supply-drop hook disabled\n");
            }
            else
            {
                g_InstalledSupplyDrop = CreateAndEnableHook(
                    target,
                    reinterpret_cast<void*>(&hkDecideActSupplyDrop),
                    reinterpret_cast<void**>(&g_OrigDecideActSupplyDrop));

                if (g_InstalledSupplyDrop)
                    LogDebug("[OutfitItemSelector] supply installed OK "
                             "(target=%p)\n", target);
                else
                    Log("[OutfitItemSelector] supply install FAILED "
                        "(target=%p)\n", target);
            }
        }

        if (!g_InstalledCustomize)
        {
            void* target = ResolveGameAddress(
                gAddr.ItemSelectorCallbackImpl_DecideActMotherBaseCustomize);
            if (!target)
            {
                LogDebug("[OutfitItemSelector] customize target unresolved; "
                    "Mother-Base customize hook disabled\n");
            }
            else
            {
                g_InstalledCustomize = CreateAndEnableHook(
                    target,
                    reinterpret_cast<void*>(&hkDecideActCustomize),
                    reinterpret_cast<void**>(&g_OrigDecideActCustomize));

                if (g_InstalledCustomize)
                    LogDebug("[OutfitItemSelector] customize installed OK "
                             "(target=%p)\n", target);
                else
                    Log("[OutfitItemSelector] customize install FAILED "
                        "(target=%p)\n", target);
            }
        }

        if (!g_InstalledSetSupplyCBox)
        {
            void* target = ResolveGameAddress(
                gAddr.EquipDevelopCallbackImpl_SetSupplyCBoxInfo);
            if (!target)
            {
                LogDebug("[OutfitItemSelector] R&D dev-menu SetSupplyCBoxInfo "
                    "target unresolved; R&D-menu request hook disabled\n");
            }
            else
            {
                g_InstalledSetSupplyCBox = CreateAndEnableHook(
                    target,
                    reinterpret_cast<void*>(&hkSetSupplyCBoxInfo),
                    reinterpret_cast<void**>(&g_OrigSetSupplyCBoxInfo));

                if (g_InstalledSetSupplyCBox)
                    LogDebug("[OutfitItemSelector] devmenu SetSupplyCBoxInfo "
                             "installed OK (target=%p)\n", target);
                else
                    Log("[OutfitItemSelector] devmenu SetSupplyCBoxInfo install "
                        "FAILED (target=%p)\n", target);
            }
        }

        return g_Installed;
    }

    void Uninstall_OutfitItemSelector_Hook()
    {
        if (g_Installed)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.ItemSelectorCallbackImpl_DecideActMissionPreparationSetEquipMode))
                DisableAndRemoveHook(t);
            g_OrigDecideActMissionPrep = nullptr;
            g_Installed                = false;
        }
        if (g_InstalledSupplyDrop)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.ItemSelectorCallbackImpl_DecideActMotherBaseDeviceSupportDropMode))
                DisableAndRemoveHook(t);
            g_OrigDecideActSupplyDrop = nullptr;
            g_InstalledSupplyDrop     = false;
        }
        if (g_InstalledCustomize)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.ItemSelectorCallbackImpl_DecideActMotherBaseCustomize))
                DisableAndRemoveHook(t);
            g_OrigDecideActCustomize = nullptr;
            g_InstalledCustomize     = false;
        }
        if (g_InstalledSetSupplyCBox)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.EquipDevelopCallbackImpl_SetSupplyCBoxInfo))
                DisableAndRemoveHook(t);
            g_OrigSetSupplyCBoxInfo  = nullptr;
            g_InstalledSetSupplyCBox = false;
        }
#ifdef _DEBUG
        LogDebug("[OutfitItemSelector] removed\n");
#endif
    }
}
