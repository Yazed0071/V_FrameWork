#include "pch.h"

#include "MissionPreparationSystemImpl_IsEnableHeadOptionSuit.h"
#include "OutfitRegistry.h"
#include "CustomHeadRegistry.h"

#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "../equip/DevelopArrayGrow.h"
#include "../equip/EquipDevelop_SetEquipUndeveloped.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"
#include "../equip/EquipPartParams.h"

namespace
{


    using IsEnableCurrentHeadOption_t = std::uint8_t (__fastcall*)(void* self);

    static IsEnableCurrentHeadOption_t g_OrigIsEnableHead = nullptr;
    static bool                        g_Installed        = false;


    using IsEnableCurrentSuit_t = std::uint8_t (__fastcall*)(void* self);
    static IsEnableCurrentSuit_t g_OrigIsEnableCurrentSuit       = nullptr;
    static bool                  g_IsEnableCurrentSuitInstalled  = false;


    using IsEnableHeadOptionSuit_t =
        std::uint8_t (__fastcall*)(void* self, std::uint16_t flowIndex);
    static IsEnableHeadOptionSuit_t g_OrigIsEnableHeadOptionSuit       = nullptr;
    static bool                     g_IsEnableHeadOptionSuitInstalled  = false;


    using MenuGetLangText_t = const char* (__fastcall*)(void* mgr, std::uint64_t key);
    static MenuGetLangText_t g_OrigMenuGetLangText   = nullptr;
    static void*             g_MenuGetLangTextTarget = nullptr;
    static std::atomic<bool> g_FobBlockVfwOnly{ false };

    constexpr std::uint64_t kFobBlockVanillaLangKey = 0xEF471B5B1850ull;
    constexpr std::uint64_t kFobBlockVfwLangKey     = 0x45C697D13F30ull;
    constexpr std::uint64_t kLangKeyMask            = 0xFFFFFFFFFFFFull;
    constexpr std::size_t   kMenuUiMgrOffset        = 0x38;
    constexpr std::size_t   kMenuGetLangTextVtblOff = 0x750;

    static const char* __fastcall hkMenuGetLangText(void* mgr, std::uint64_t key)
    {
        if ((key & kLangKeyMask) == kFobBlockVanillaLangKey
            && g_FobBlockVfwOnly.load(std::memory_order_relaxed))
        {
            const char* swapped = nullptr;
            __try
            {
                swapped = g_OrigMenuGetLangText(
                    mgr, (key & ~kLangKeyMask) | kFobBlockVfwLangKey);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                swapped = nullptr;
            }
            if (swapped && swapped[0])
                return swapped;
        }
        return g_OrigMenuGetLangText(mgr, key);
    }

    static void EnsureMenuLangHookSEH(void* menuCallback)
    {
        __try
        {
            void* mgr = *reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(menuCallback) + kMenuUiMgrOffset);
            if (!mgr)
                return;
            void** vtbl = *reinterpret_cast<void***>(mgr);
            if (!vtbl)
                return;
            void* fn = vtbl[kMenuGetLangTextVtblOff / sizeof(void*)];
            if (!fn)
                return;
            if (CreateAndEnableHook(fn, reinterpret_cast<void*>(&hkMenuGetLangText),
                                    reinterpret_cast<void**>(&g_OrigMenuGetLangText)))
            {
                g_MenuGetLangTextTarget = fn;
                Log("[FobGuard] FOB-block popup lang swap hooked "
                    "(menu GetLangText impl=%p)\n", fn);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }


    using ConverFaceId_t = std::uint16_t (__fastcall*)(
        std::uint8_t playerType, std::int16_t faceId,
        std::uint8_t playerFaceEquipId);
    static ConverFaceId_t g_OrigConverFaceId           = nullptr;
    static bool           g_ConverFaceIdInstalled      = false;

    static std::uint16_t __fastcall hkConverFaceIdWithFaceEquipId(
        std::uint8_t playerType, std::int16_t faceId,
        std::uint8_t playerFaceEquipId)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
        {
            const std::uint8_t safeSlot =
                outfit::IsCustomHeadSlot(playerFaceEquipId)
                    ? std::uint8_t{0} : playerFaceEquipId;
            return g_OrigConverFaceId
                ? g_OrigConverFaceId(playerType, faceId, safeSlot)
                : faceId;
        }

        if (outfit::IsCustomHeadSlot(playerFaceEquipId))
        {
            if (const auto* head =
                outfit::TryGetCustomHeadBySlot(playerFaceEquipId))
            {
                const std::uint8_t livePT = outfit::ReadLivePartsType();
                const outfit::OutfitEntry* oe = nullptr;
                const bool offered =
                    (livePT >= outfit::kCustomPartsTypeStart
                     && livePT <= outfit::kCustomPartsTypeEnd
                     && outfit::TryGetOutfitByPartsType(livePT, &oe) && oe
                     && oe->HasHeadOptionAnyVariant(head->equipId, playerType))
                    || (livePT < outfit::kCustomPartsTypeStart
                        && outfit::VanillaExtHasHeadOption(livePT, head->equipId,
                                                           playerType,
                                                           outfit::ReadLiveSelectorCode()));
                if (offered)
                {
                    const std::uint8_t pt =
                        (playerType < outfit::kPlayerTypeMax) ? playerType : 0;
                    std::uint16_t fid = head->TppEnemyFaceId[pt];
                    if (fid == 0) fid = head->TppEnemyFaceId[0];
                    return fid;
                }
            }
        }
        else if (playerFaceEquipId >= 1 && playerFaceEquipId <= 5)
        {
            const std::uint8_t livePT = outfit::ReadLivePartsType();
            const outfit::OutfitEntry* oe = nullptr;
            if (livePT >= outfit::kCustomPartsTypeStart
                && livePT <= outfit::kCustomPartsTypeEnd
                && outfit::TryGetOutfitByPartsType(livePT, &oe) && oe)
            {
                const std::uint16_t vanEquipId =
                    static_cast<std::uint16_t>(playerFaceEquipId + 0x20D);
                if (!oe->HasHeadOptionAnyVariant(vanEquipId, playerType))
                {
                    return g_OrigConverFaceId
                        ? g_OrigConverFaceId(playerType, faceId, 0)
                        : faceId;
                }
            }
        }

        return g_OrigConverFaceId
            ? g_OrigConverFaceId(playerType, faceId, playerFaceEquipId)
            : faceId;
    }

    using ConvertHeadEquipModelType_t = std::uint64_t (__fastcall*)(
        void* self, std::uint32_t soldierIndex, void* offerList,
        std::uint32_t offerCount);
    static ConvertHeadEquipModelType_t g_OrigConvertHeadEquip      = nullptr;
    static bool                        g_ConvertHeadEquipInstalled = false;
    constexpr std::uint64_t            kNoHeadEquipModelType       = 0x2e;

    static std::uint64_t __fastcall hkConvertHeadEquipModelType(
        void* self, std::uint32_t soldierIndex, void* offerList,
        std::uint32_t offerCount)
    {
        if (self && soldierIndex != 0x1ff)
        {
            __try
            {
                auto* container = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(self) + 0xF8);
                auto* arrHdr = container
                    ? *reinterpret_cast<std::uint8_t**>(container + 0x98)
                    : nullptr;
                auto* base = arrHdr
                    ? *reinterpret_cast<std::uint8_t**>(arrHdr + 0x8)
                    : nullptr;
                const std::uint32_t count = arrHdr
                    ? *reinterpret_cast<std::uint32_t*>(arrHdr + 0x10)
                    : 0;
                if (base && soldierIndex < count)
                {
                    auto* entry =
                        base + static_cast<std::size_t>(soldierIndex) * 0x70;
                    const std::uint16_t wornId =
                        *reinterpret_cast<std::uint16_t*>(entry + 0x4e);
                    if (wornId != 0)
                    {
                        const outfit::CustomHeadEntry* h =
                            outfit::TryGetCustomHeadByEquipId(wornId);
                        if (h)
                        {
                            if (MissionCodeGuard::ShouldBypassHooks())
                                return kNoHeadEquipModelType;
                            const std::uint8_t livePT =
                                outfit::ReadLivePartsType();
                            const outfit::OutfitEntry* oe = nullptr;
                            const bool offered =
                                (livePT >= outfit::kCustomPartsTypeStart
                                 && livePT <= outfit::kCustomPartsTypeEnd
                                 && outfit::TryGetOutfitByPartsType(livePT, &oe)
                                 && oe
                                 && oe->HasHeadOptionAnyVariant(
                                        h->equipId, outfit::ReadLivePlayerType()))
                                || (livePT < outfit::kCustomPartsTypeStart
                                    && outfit::VanillaExtHasHeadOption(
                                           livePT, h->equipId,
                                           outfit::ReadLivePlayerType(),
                                           outfit::ReadLiveSelectorCode()));
                            if (!offered)
                                return kNoHeadEquipModelType;
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                static std::atomic<int> s_fault{0};
                if (int n = s_fault.load(std::memory_order_relaxed); n < 8)
                {
                    s_fault.store(n + 1, std::memory_order_relaxed);
                    Log("[OutfitHeadOption] HeadEquipType FAULT: sIdx=%u "
                        "(read chain bad -> offsets wrong)\n", soldierIndex);
                }
            }
        }
        return g_OrigConvertHeadEquip
            ? g_OrigConvertHeadEquip(self, soldierIndex, offerList, offerCount)
            : kNoHeadEquipModelType;
    }

    constexpr std::size_t kVtblSlot_PrepIsFobSortie     = 0x4F0 / 8;
    constexpr std::size_t kVtblSlot_PrepGetWeapon       = 0x180 / 8;
    constexpr std::size_t kVtblSlot_PrepGetItem         = 0x1C8 / 8;
    constexpr std::size_t kVtblSlot_PrepGetSupport      = 0x1D8 / 8;
    constexpr std::size_t kVtblSlot_DevIdxFromEquipId   = 0xF0 / 8;
    constexpr std::size_t kVtblSlot_DevIsFobAvailable   = 0x478 / 8;
    constexpr std::size_t kCallback_PrepSystemOffset    = 0x48;

    static bool IsFobSortieContext(void* self)
    {
        __try
        {
            void* sys = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(self) + kCallback_PrepSystemOffset);
            if (!sys)
                return false;
            using Ctx_t = std::uint8_t (__fastcall*)(void*);
            auto ctx = reinterpret_cast<Ctx_t>(
                (*reinterpret_cast<void***>(sys))[kVtblSlot_PrepIsFobSortie]);
            return ctx && ctx(sys) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool IsFobSortiePrepSystem(void* sys)
    {
        __try
        {
            if (!sys)
                return false;
            using Ctx_t = std::uint8_t (__fastcall*)(void*);
            auto ctx = reinterpret_cast<Ctx_t>(
                (*reinterpret_cast<void***>(sys))[kVtblSlot_PrepIsFobSortie]);
            return ctx && ctx(sys) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    using PrepGetU16_t = std::uint16_t (__fastcall*)(void*, std::uint8_t);

    static std::uint16_t SafeCallPrepGetU16(PrepGetU16_t fn, void* sys,
                                            std::uint8_t i)
    {
        __try { return fn(sys, i); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0xEEEE; }
    }

    constexpr std::size_t kQuark_AppOffset          = 0x98;
    constexpr std::size_t kApp_LoadoutHolderOffset  = 0x130;

    static void* ResolveLoadoutHolder()
    {
        using GetQuark_t = void* (__fastcall*)();
        auto getQuark = reinterpret_cast<GetQuark_t>(
            ResolveGameAddress(gAddr.GetQuarkSystemTable));
        if (!getQuark)
            return nullptr;
        __try
        {
            auto* quark = static_cast<std::uint8_t*>(getQuark());
            if (!quark)
                return nullptr;
            auto* app = *reinterpret_cast<std::uint8_t**>(
                quark + kQuark_AppOffset);
            if (!app)
                return nullptr;
            return *reinterpret_cast<void**>(app + kApp_LoadoutHolderOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static bool FobLoadoutHasBannedManagedEquip(void* self)
    {
        static_cast<void>(self);
        __try
        {
            void* sys = ResolveLoadoutHolder();
            if (!sys)
                return false;
            void** sysVt = *reinterpret_cast<void***>(sys);

            void* ctrl = EquipDevelop_ResolveDevelopController();
            if (!ctrl)
                return false;
            void** ctrlVt = *reinterpret_cast<void***>(ctrl);

            using GetU16_t = PrepGetU16_t;
            using MapIdx_t = std::uint16_t (__fastcall*)(void*, std::uint16_t,
                                                         std::uint8_t);
            using FobBit_t = std::uint8_t (__fastcall*)(void*, std::uint16_t);

            auto mapIdx = reinterpret_cast<MapIdx_t>(
                ctrlVt[kVtblSlot_DevIdxFromEquipId]);
            auto fobBit = reinterpret_cast<FobBit_t>(
                ctrlVt[kVtblSlot_DevIsFobAvailable]);
            if (!mapIdx || !fobBit)
                return false;

            struct SlotRange { std::size_t slot; std::uint8_t count; };
            const SlotRange ranges[] = {
                { kVtblSlot_PrepGetWeapon,  3 },
                { kVtblSlot_PrepGetItem,    8 },
                { kVtblSlot_PrepGetSupport, 8 },
            };

            struct ScanEntry
            {
                std::uint16_t slotOff;
                std::uint8_t  index;
                std::uint16_t equipId;
                std::uint16_t devIdx;
                std::uint8_t  managed;
                std::uint8_t  fobOk;
                std::uint8_t  tainted;
            };
            ScanEntry entries[19] = {};
            int       entryCount = 0;
            int       bannedAt   = -1;
            std::uint64_t digest = 1469598103934665603ull;

            for (const SlotRange& r : ranges)
            {
                auto get = reinterpret_cast<GetU16_t>(sysVt[r.slot]);
                if (!get)
                    continue;
                for (std::uint8_t i = 0; i < r.count; ++i)
                {
                    const std::uint16_t equipId = SafeCallPrepGetU16(get, sys, i);
                    digest = (digest ^ equipId) * 1099511628211ull;
                    if (equipId == 0 || equipId == 0xEEEE)
                        continue;
                    const std::uint16_t idx = mapIdx(ctrl, equipId, 0);
                    const bool managed = equip::IsValidFlowIndex(idx)
                        && EquipDevelopAdd::IsManagedFlowIndex(idx);
                    const bool tainted = !managed
                        && EquipParam_IsEquipIdFobTainted(
                               equipId,
                               (r.slot == kVtblSlot_PrepGetWeapon) ? 1 : 0);
                    const bool fobOk = !tainted
                        && (!managed || fobBit(ctrl, idx) != 0);
                    if (entryCount < 19)
                    {
                        entries[entryCount].slotOff =
                            static_cast<std::uint16_t>(r.slot * 8);
                        entries[entryCount].index   = i;
                        entries[entryCount].equipId = equipId;
                        entries[entryCount].devIdx  = idx;
                        entries[entryCount].managed = managed ? 1 : 0;
                        entries[entryCount].fobOk   = fobOk ? 1 : 0;
                        entries[entryCount].tainted = tainted ? 1 : 0;
                        if ((managed || tainted) && !fobOk && bannedAt < 0)
                            bannedAt = entryCount;
                        ++entryCount;
                    }
                    else if ((managed || tainted) && !fobOk && bannedAt < 0)
                    {
                        bannedAt = 19;
                    }
                }
            }
#ifdef _DEBUG
            {
                static std::uint64_t s_lastDigest = 0;
                if (digest != s_lastDigest)
                {
                    s_lastDigest = digest;
                    for (int e = 0; e < entryCount; ++e)
                        Log("[FobDeploy] loadout slot=0x%X i=%u equipId=%u "
                            "(0x%X) devIdx=%u managed=%u fobOk=%u tainted=%u\n",
                            static_cast<unsigned>(entries[e].slotOff),
                            static_cast<unsigned>(entries[e].index),
                            static_cast<unsigned>(entries[e].equipId),
                            static_cast<unsigned>(entries[e].equipId),
                            static_cast<unsigned>(entries[e].devIdx),
                            static_cast<unsigned>(entries[e].managed),
                            static_cast<unsigned>(entries[e].fobOk),
                            static_cast<unsigned>(entries[e].tainted));
                }
            }
#endif
            if (bannedAt >= 0)
            {
                if (bannedAt < entryCount)
                {
                    if (entries[bannedAt].tainted)
                        Log("[FobGuard] FOB deploy blocked: vanilla equipId=%u "
                            "uses vanilla parts or damage rows modified by a "
                            "module - unequip it (or remove the mod's vanilla "
                            "edits) to deploy\n",
                            static_cast<unsigned>(entries[bannedAt].equipId));
                    else
                        Log("[OutfitHeadOption] FOB deploy blocked: managed "
                            "equipId=%u (flowIndex=%u) is equipped and not "
                            "FOB-available\n",
                            static_cast<unsigned>(entries[bannedAt].equipId),
                            static_cast<unsigned>(entries[bannedAt].devIdx));
                }
                return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return false;
    }

    static std::uint8_t __fastcall hkIsEnableHeadOptionSuit(
        void* self, std::uint16_t param2)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigIsEnableHeadOptionSuit(self, param2);

        if (IsFobSortiePrepSystem(self))
            return g_OrigIsEnableHeadOptionSuit(self, param2);

        const std::uint8_t pt     = outfit::ReadLivePartsType();
        const std::uint8_t livePT = outfit::ReadLivePlayerType();

        if (pt >= outfit::kCustomPartsTypeStart && pt <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByPartsType(pt, &entry) && entry)
            {
                return entry->HasHeadOptionsForVariant(
                           livePT, outfit::GetActiveVariant(pt)) ? 1 : 0;
            }
        }
        if (pt < outfit::kCustomPartsTypeStart
            && outfit::VanillaExtHasAnyHeadOptions(
                   pt, livePT, outfit::ReadLiveSelectorCode()))
            return 1;

        return g_OrigIsEnableHeadOptionSuit(self, param2);
    }

    static std::uint8_t __fastcall hkIsEnableCurrentHeadOption(void* self)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigIsEnableHead(self);

        if (IsFobSortieContext(self))
            return g_OrigIsEnableHead(self);

        const std::uint8_t pt     = outfit::ReadLivePartsType();
        const std::uint8_t livePT = outfit::ReadLivePlayerType();

        if (pt >= outfit::kCustomPartsTypeStart && pt <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByPartsType(pt, &entry) && entry)
            {
                return entry->HasHeadOptionsForVariant(
                           livePT, outfit::GetActiveVariant(pt)) ? 1 : 0;
            }
        }
        if (pt < outfit::kCustomPartsTypeStart
            && outfit::VanillaExtHasAnyHeadOptions(
                   pt, livePT, outfit::ReadLiveSelectorCode()))
            return 1;

        return g_OrigIsEnableHead(self);
    }

    static std::uint8_t __fastcall hkIsEnableCurrentSuit(void* self)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigIsEnableCurrentSuit(self);

        {
            const bool fobCtx = IsFobSortieContext(self);
#ifdef _DEBUG
            {
                static int s_calls = 0;
                if (s_calls < 24)
                {
                    ++s_calls;
                    Log("[FobDeploy] IsEnableCurrentSuit called: fobCtx=%d\n",
                        fobCtx ? 1 : 0);
                }
            }
#endif
            if (fobCtx)
            {
                if (!g_OrigMenuGetLangText)
                    EnsureMenuLangHookSEH(self);
                if (FobLoadoutHasBannedManagedEquip(self))
                {
                    g_FobBlockVfwOnly.store(
                        g_OrigIsEnableCurrentSuit(self) != 0,
                        std::memory_order_relaxed);
                    return 0;
                }
                const std::uint16_t wornHeadEquipId =
                    outfit::GetCurrentWornHeadEquipId();
                if (wornHeadEquipId != 0)
                {
                    Log("[OutfitHeadOption] FOB deploy blocked: custom head "
                        "option worn (equipId=%u) - remove it to deploy\n",
                        static_cast<unsigned>(wornHeadEquipId));
                    g_FobBlockVfwOnly.store(
                        g_OrigIsEnableCurrentSuit(self) != 0,
                        std::memory_order_relaxed);
                    return 0;
                }
                const std::uint8_t wornPt = outfit::ReadLivePartsType();
                if (wornPt < outfit::kCustomPartsTypeStart
                    && outfit::GetActiveVariant(wornPt) != 0)
                {
                    Log("[OutfitHeadOption] FOB deploy blocked: extended "
                        "vanilla variant active on worn partsType=0x%02X - "
                        "switch to the base suit to deploy\n",
                        static_cast<unsigned>(wornPt));
                    g_FobBlockVfwOnly.store(
                        g_OrigIsEnableCurrentSuit(self) != 0,
                        std::memory_order_relaxed);
                    return 0;
                }
                g_FobBlockVfwOnly.store(false, std::memory_order_relaxed);
                return g_OrigIsEnableCurrentSuit(self);
            }
        }

        const std::uint8_t pt     = outfit::ReadLivePartsType();
        const std::uint8_t livePT = outfit::ReadLivePlayerType();

        if (pt >= outfit::kCustomPartsTypeStart && pt <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByPartsType(pt, &entry) && entry)
            {
                return entry->HasHeadOptionsForVariant(
                           livePT, outfit::GetActiveVariant(pt)) ? 1 : 0;
            }
        }
        if (pt < outfit::kCustomPartsTypeStart
            && outfit::VanillaExtHasAnyHeadOptions(
                   pt, livePT, outfit::ReadLiveSelectorCode()))
            return 1;

        return g_OrigIsEnableCurrentSuit(self);
    }
}

namespace outfit
{
    bool Install_OutfitHeadOption_Hook()
    {
        if (g_Installed) return true;

        void* target = ResolveGameAddress(gAddr.MissionPrep_IsEnableCurrentHeadOption);
        if (target)
        {
            g_Installed = CreateAndEnableHook(
                target,
                reinterpret_cast<void*>(&hkIsEnableCurrentHeadOption),
                reinterpret_cast<void**>(&g_OrigIsEnableHead));
            if (g_Installed)
                LogDebug("[OutfitHeadOption] enable-gate hook installed OK "
                         "(target=%p)\n", target);
            else
                Log("[OutfitHeadOption] enable-gate hook install FAILED "
                    "(target=%p)\n", target);
        }
        else
        {
            Log("[OutfitHeadOption] enable-gate unresolved; skipped (submenu may "
                "show greyed, head render still installs below)\n");
        }

        void* suitTarget = ResolveGameAddress(gAddr.IsEnableCurrentSuit);
        if (suitTarget)
        {
            g_IsEnableCurrentSuitInstalled = CreateAndEnableHook(
                suitTarget,
                reinterpret_cast<void*>(&hkIsEnableCurrentSuit),
                reinterpret_cast<void**>(&g_OrigIsEnableCurrentSuit));
            if (g_IsEnableCurrentSuitInstalled)
                LogDebug("[OutfitHeadOption:SuitGate] override hook installed "
                         "(target=%p)\n", suitTarget);
            else
                Log("[OutfitHeadOption:SuitGate] override hook install FAILED "
                    "(target=%p)\n", suitTarget);
        }
        else
        {
            Log("[OutfitHeadOption:SuitGate] target unresolved; skipped\n");
        }


        void* hosTarget = ResolveGameAddress(
            gAddr.MissionPrepSystem_IsEnableHeadOptionSuit);
        if (hosTarget)
        {
            g_IsEnableHeadOptionSuitInstalled = CreateAndEnableHook(
                hosTarget,
                reinterpret_cast<void*>(&hkIsEnableHeadOptionSuit),
                reinterpret_cast<void**>(&g_OrigIsEnableHeadOptionSuit));
            if (g_IsEnableHeadOptionSuitInstalled)
                LogDebug("[OutfitHeadOption:HOSuit] override hook installed "
                         "(target=%p)\n", hosTarget);
            else
                Log("[OutfitHeadOption:HOSuit] override hook install FAILED "
                    "(target=%p)\n", hosTarget);
        }
        else
        {
            Log("[OutfitHeadOption:HOSuit] target unresolved; skipped\n");
        }


        void* cfTarget = ResolveGameAddress(gAddr.Player_ConverFaceIdWithFaceEquipId);
        if (cfTarget)
        {
            g_ConverFaceIdInstalled = CreateAndEnableHook(
                cfTarget,
                reinterpret_cast<void*>(&hkConverFaceIdWithFaceEquipId),
                reinterpret_cast<void**>(&g_OrigConverFaceId));
            if (g_ConverFaceIdInstalled)
                LogDebug("[OutfitHeadOption:ConverFace] hook installed "
                         "(target=%p)\n", cfTarget);
            else
                Log("[OutfitHeadOption:ConverFace] hook install FAILED "
                    "(target=%p)\n", cfTarget);
        }
        else
        {
            Log("[OutfitHeadOption:ConverFace] target unresolved; skipped\n");
        }

        void* heTarget =
            ResolveGameAddress(gAddr.RealizedSoldier2Impl_ConvertHeadEquipModelType);
        if (heTarget)
        {
            g_ConvertHeadEquipInstalled = CreateAndEnableHook(
                heTarget,
                reinterpret_cast<void*>(&hkConvertHeadEquipModelType),
                reinterpret_cast<void**>(&g_OrigConvertHeadEquip));
            if (g_ConvertHeadEquipInstalled)
                LogDebug("[OutfitHeadOption:HeadEquipType] hook installed "
                         "(target=%p)\n", heTarget);
            else
                Log("[OutfitHeadOption:HeadEquipType] hook install FAILED "
                    "(target=%p)\n", heTarget);
        }
        else
        {
            Log("[OutfitHeadOption:HeadEquipType] target unresolved; skipped\n");
        }

        return g_ConverFaceIdInstalled || g_Installed
            || g_IsEnableCurrentSuitInstalled || g_IsEnableHeadOptionSuitInstalled
            || g_ConvertHeadEquipInstalled;
    }

    void Uninstall_OutfitHeadOption_Hook()
    {
        if (g_ConvertHeadEquipInstalled)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.RealizedSoldier2Impl_ConvertHeadEquipModelType))
                DisableAndRemoveHook(t);
            g_OrigConvertHeadEquip      = nullptr;
            g_ConvertHeadEquipInstalled = false;
        }

        if (g_ConverFaceIdInstalled)
        {
            if (void* t = ResolveGameAddress(gAddr.Player_ConverFaceIdWithFaceEquipId))
                DisableAndRemoveHook(t);
            g_OrigConverFaceId      = nullptr;
            g_ConverFaceIdInstalled = false;
        }

        if (g_IsEnableHeadOptionSuitInstalled)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.MissionPrepSystem_IsEnableHeadOptionSuit))
                DisableAndRemoveHook(t);
            g_OrigIsEnableHeadOptionSuit      = nullptr;
            g_IsEnableHeadOptionSuitInstalled = false;
        }

        if (g_IsEnableCurrentSuitInstalled)
        {
            if (void* t = ResolveGameAddress(gAddr.IsEnableCurrentSuit))
                DisableAndRemoveHook(t);
            g_OrigIsEnableCurrentSuit       = nullptr;
            g_IsEnableCurrentSuitInstalled  = false;
        }

        if (g_MenuGetLangTextTarget)
        {
            DisableAndRemoveHook(g_MenuGetLangTextTarget);
            g_MenuGetLangTextTarget = nullptr;
            g_OrigMenuGetLangText   = nullptr;
        }

        if (!g_Installed) return;
        if (void* t = ResolveGameAddress(gAddr.MissionPrep_IsEnableCurrentHeadOption))
            DisableAndRemoveHook(t);
        g_OrigIsEnableHead = nullptr;
        g_Installed        = false;
#ifdef _DEBUG
        Log("[OutfitHeadOption] removed\n");
#endif
    }
}
