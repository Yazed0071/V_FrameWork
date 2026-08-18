#include "pch.h"

#include "BlockControllerImpl_LoadPartsNew.h"
#include "OutfitRegistry.h"
#include "CustomHeadRegistry.h"
#include "Player2UtilityImpl_SetSuitAndHandConditionWithLoadoutInfo.h"
#include "ShadowState.h"
#include "FoxPathInternal.h"
#include "MissionCodeGuard.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <intrin.h>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

#pragma intrinsic(_ReturnAddress)

namespace
{
    using FoxPath_Path_t = void* (__fastcall*)(void* outPath, std::uint64_t code64ext);

    using LoadPlayerPartsParts_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t playerType, std::uint32_t playerPartsType);
    using LoadPlayerPartsFpk_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t playerType, std::uint32_t playerPartsType);
    using LoadPlayerCamoFpk_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerCamoType);
    using LoadPlayerSnakeBlackDiamondFpk_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t applyBlackDiamond);
    using LoadPlayerBionicArm_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerHandType);
    using LoadPlayerSnakeFace_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerFaceId, char playerFaceEquipId);
    using LoadAvatarFace_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t avatarFaceA, std::uint32_t avatarFaceB);
    using LoadAvatarHeadOption_t = std::uint64_t* (__fastcall*)(
        std::uint64_t* outPath, std::uint32_t faceEquipId);
    using AvatarFaceEditUpdate_t = void (__fastcall*)(
        void* self, void* blockGroup, std::uint32_t blockIndex);

    struct LoadPartsPlayerInfo
    {
        std::uint8_t  playerType;
        std::uint8_t  playerPartsType;
        std::uint8_t  playerCamoType;
        std::uint8_t  playerArmType;
        std::int16_t  playerFaceId;
        std::uint8_t  playerFaceEquipId;
        std::uint8_t  reserved07;
        std::uint8_t  reserved08[0x4C];
        std::uint8_t  reserved54;
        std::uint8_t  reserved55;
        std::uint8_t  playerFaceEquipUnk;
        std::uint8_t  reserved57;
    };
    static_assert(sizeof(LoadPartsPlayerInfo) == 0x58,
                  "LoadPartsPlayerInfo size must match retail layout");

    using LoadPartsNew_t           = void (__fastcall*)(void*, std::uint32_t, LoadPartsPlayerInfo*, std::uint32_t);
    using DoesNeedFaceFova_t       = std::uint8_t (__fastcall*)(std::uint32_t);
    using SetHandSlotEnabled_t     = void (__fastcall*)(void*, std::uint32_t, std::uint8_t);
    using IsArtificialHandEnabled_t            = std::uint8_t (__fastcall*)(std::uint32_t, std::uint32_t);
    using IsArtificialHandEnabledForCurrent_t  = std::uint8_t (__fastcall*)();
    using ProcessSignal_t          = void (__fastcall*)(void*, void*, std::uint32_t, std::uint64_t*);
    using UpdatePartsStatus_t      = void (__fastcall*)(void*);
    using Player2ImplSetUpParts_t  = bool (__fastcall*)(void*, std::uint32_t, std::uint32_t,
                                                       std::uint32_t, std::uint32_t, std::uint32_t,
                                                       std::uint32_t, void*);
    using PluginFacialApplyMotion_t = void (__fastcall*)(void*, void*, void*, float);
    using GetPartsTypeAtCamoType_t  = std::uint32_t (__fastcall*)(void*, std::uint32_t);

    constexpr std::uint64_t kSignalRefreshFv2s             = 0x8483a342fa61ull;
    constexpr std::size_t   kP2GO_OffPerPlayerStruct       = 0x80;
    constexpr std::size_t   kP2GO_OffStateMachinePtr       = 0xb0;
    constexpr std::size_t   kPP_OffPlayerTypeArr           = 0x40;
    constexpr std::size_t   kPP_OffPartsTypeArr            = 0x48;
    constexpr std::size_t   kPP_OffCamoTypeArr             = 0x50;
    constexpr std::size_t   kPP_OffArmTypeArr              = 0x58;
    constexpr std::size_t   kPP_OffStateChangedBits        = 0x180;
    constexpr std::size_t   kPP_OffVariantChangedBits      = 0x184;
    constexpr std::size_t   kPP_OffAltStateBits            = 0x184;
    constexpr std::size_t   kPP_OffLoadoutReq              = 0xc0;
    constexpr std::size_t   kPP_LoadoutReqStride           = 0x3a;
    constexpr std::size_t   kPP_LoadoutReqEquipHashOff     = 0x8;
    constexpr std::uint8_t  kProcessSignalSpoofPartsType   = 0x01;
    constexpr std::uint32_t kBionicArmVanillaPartsTypeSubstitute = 0x01;

    static FoxPath_Path_t                       g_FoxPath_Path                       = nullptr;
    static LoadPlayerPartsParts_t               g_OrigLoadPartsParts                 = nullptr;
    static LoadPlayerPartsFpk_t                 g_OrigLoadPartsFpk                   = nullptr;
    static LoadPlayerCamoFpk_t                  g_OrigLoadCamoFpk                    = nullptr;
    static LoadPlayerSnakeBlackDiamondFpk_t     g_OrigLoadDiamondFpk                 = nullptr;
    static LoadPlayerCamoFpk_t                  g_OrigLoadCamoFv2                    = nullptr;
    static LoadPlayerSnakeBlackDiamondFpk_t     g_OrigLoadDiamondFv2                 = nullptr;
    static LoadPlayerBionicArm_t                g_OrigLoadBionicArmFv2               = nullptr;
    static LoadPlayerBionicArm_t                g_OrigLoadBionicArmFpk               = nullptr;
    static LoadPlayerSnakeFace_t                g_OrigLoadSnakeFaceFv2               = nullptr;
    static LoadPlayerSnakeFace_t                g_OrigLoadSnakeFaceFpk               = nullptr;
    static LoadAvatarFace_t                     g_OrigLoadAvatarFaceFv2              = nullptr;
    static LoadAvatarFace_t                     g_OrigLoadAvatarFaceFpk              = nullptr;
    static LoadAvatarHeadOption_t               g_OrigLoadAvatarHeadOptionFv2        = nullptr;
    static LoadAvatarHeadOption_t               g_OrigLoadAvatarHeadOptionFpk        = nullptr;
    static AvatarFaceEditUpdate_t               g_OrigAvatarFaceEditUpdate           = nullptr;
    static LoadPartsNew_t                       g_OrigLoadPartsNew                   = nullptr;
    static DoesNeedFaceFova_t                   g_OrigDoesNeedFaceFova               = nullptr;
    static DoesNeedFaceFova_t                   g_OrigDoesNeedFaceFovaForAvatar      = nullptr;
    static SetHandSlotEnabled_t                 g_OrigSetHandSlotEnabled             = nullptr;
    static IsArtificialHandEnabled_t            g_OrigIsArtificialHandEnabled        = nullptr;
    static IsArtificialHandEnabledForCurrent_t  g_OrigIsArtificialHandForCurrent     = nullptr;
    static ProcessSignal_t                      g_OrigProcessSignal                  = nullptr;
    static UpdatePartsStatus_t                  g_OrigUpdatePartsStatus              = nullptr;
    static Player2ImplSetUpParts_t              g_OrigPlayer2ImplSetUpParts          = nullptr;
    static PluginFacialApplyMotion_t            g_OrigPluginFacialApplyMotion        = nullptr;
    static GetPartsTypeAtCamoType_t             g_OrigGetPartsTypeAtCamoType         = nullptr;

    static bool g_InstalledParts                 = false;
    static bool g_InstalledFpk                   = false;
    static bool g_InstalledCamo                  = false;
    static bool g_InstalledDiamond               = false;
    static bool g_InstalledCamoFv2               = false;
    static bool g_InstalledDiamondFv2            = false;
    static bool g_InstalledBionicArmFv2          = false;
    static bool g_InstalledBionicArmFpk          = false;
    static bool g_InstalledSnakeFaceFv2          = false;
    static bool g_InstalledSnakeFaceFpk          = false;
    static bool g_InstalledAvatarFaceFv2         = false;
    static bool g_InstalledAvatarFaceFpk         = false;
    static bool g_InstalledAvatarHeadOptionFv2   = false;
    static bool g_InstalledAvatarHeadOptionFpk   = false;
    static bool g_InstalledAvatarFaceEdit        = false;
    static bool g_InstalledLpn                   = false;
    static bool g_InstalledDoesNeedFace          = false;
    static bool g_InstalledDoesNeedFaceForAvatar = false;
    static bool g_InstalledSetHandSlotEnabled    = false;
    static bool g_InstalledIsArtificialHand      = false;
    static bool g_InstalledIsArtHandForCurrent   = false;
    static bool g_InstalledProcessSignal         = false;
    static bool g_InstalledUpdatePartsStatus     = false;
    static bool g_InstalledPlayer2ImplSetUpParts = false;
    static bool g_InstalledFacialCrashGuard      = false;
    static bool g_InstalledPartsAtCamo           = false;

    static void* g_CapturedBlockController = nullptr;

    static thread_local std::uint8_t t_ActiveCustomFaceSlot = 0;

    static std::atomic<std::uint8_t> g_LastCustomFaceSlot{ 0 };

    static std::atomic<void*>         g_AvatarHideBc{ nullptr };
    static std::atomic<std::uint32_t> g_AvatarHideSlotMask{ 0 };

    static const outfit::CustomHeadEntry* ResolveAvatarCustomHead();

    static bool ResolveImplBcAndSlot(void* outerSelf, std::uint32_t outerSlot,
                                     void** outImpl, std::uint32_t* outSlot)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(outerSelf);
            void* impl = *reinterpret_cast<void**>(base + 0x10);
            if (!impl) return false;
            const std::uint32_t pivot =
                *reinterpret_cast<std::uint32_t*>(base + 0x18);
            std::uint32_t implSlot = outerSlot;
            if (outerSlot == pivot)     implSlot = 0;
            else if (outerSlot < pivot) implSlot = outerSlot + 1;
            *outImpl = impl;
            *outSlot = implSlot;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool         g_CaseDArmUnpinActive = false;
    static void*        g_CaseDTrampoline     = nullptr;
    static void*        g_CaseDPatchSite      = nullptr;
    static std::uint8_t g_CaseDOrigBytes[9]   = {};
    static std::uint8_t* g_CaseDArmTable = nullptr;

    static void RefreshCaseDArmFlag(std::uint8_t partsType, bool armEnabled)
    {
        if (g_CaseDArmTable
            && partsType >= outfit::kCustomPartsTypeStart
            && partsType <= outfit::kCustomPartsTypeEnd)
            g_CaseDArmTable[partsType - outfit::kCustomPartsTypeStart] =
                armEnabled ? std::uint8_t{1} : std::uint8_t{0};
    }

    static void* AllocExecNear(std::uintptr_t nearAddr, std::size_t size)
    {
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        const std::uintptr_t gran    = si.dwAllocationGranularity;
        const std::uintptr_t rounded = nearAddr & ~(gran - 1);
        for (std::uintptr_t off = gran; off < 0x60000000ull; off += gran)
        {
            if (rounded >= off)
                if (void* p = VirtualAlloc(reinterpret_cast<LPVOID>(rounded - off),
                        size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                    return p;
            if (void* p2 = VirtualAlloc(reinterpret_cast<LPVOID>(rounded + off),
                    size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return p2;
        }
        return nullptr;
    }

    static bool InstallCaseDArmUnpin()
    {
        if (g_CaseDArmUnpinActive) return true;
        const std::uintptr_t ups = reinterpret_cast<std::uintptr_t>(
            ResolveGameAddress(gAddr.UpdatePartsStatus));
        if (!ups) { LogDebug("[CaseDArmUnpin] UpdatePartsStatus unresolved; skip\n"); return false; }

        const std::uintptr_t site   = ups + 0xc77;
        const std::uintptr_t caseD3 = ups + 0xcfe;
        const std::uintptr_t resume = ups + 0xc80;
        static const std::uint8_t kExpect[9] =
            { 0x40,0x0f,0xb6,0xc7, 0x83,0xf8,0x19, 0x77,0x7e };
        bool ok = false;
        __try { ok = (std::memcmp(reinterpret_cast<void*>(site), kExpect, 9) == 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok)
        { LogDebug("[CaseDArmUnpin] site bytes mismatch @%p - build differs, skip\n",
              reinterpret_cast<void*>(site)); return false; }

        constexpr std::size_t kTableOff  = 0x80;
        constexpr std::size_t kBandWidth =
            static_cast<std::size_t>(outfit::kCustomPartsTypeEnd)
          - static_cast<std::size_t>(outfit::kCustomPartsTypeStart) + 1;
        constexpr std::size_t kTrampSize = 0x200;
        static_assert(outfit::kCustomPartsTypeStart <= 0x80,
            "band start must fit the lea disp8 encoding");
        static_assert(kTableOff + kBandWidth <= kTrampSize,
            "arm table overflows the trampoline allocation");

        std::uint8_t* tr =
            reinterpret_cast<std::uint8_t*>(AllocExecNear(site, kTrampSize));
        if (!tr) { Log("[CaseDArmUnpin] trampoline alloc failed\n"); return false; }
        std::memset(tr, 0xCC, kTrampSize);

        std::memset(tr + kTableOff, 0x01, kBandWidth);

        std::size_t o = 0;
        const auto emit = [&](std::initializer_list<std::uint8_t> b)
        { for (std::uint8_t x : b) tr[o++] = x; };
        const auto emitRel32To = [&](std::uintptr_t target)
        { std::int32_t rel = static_cast<std::int32_t>(static_cast<std::int64_t>(target)
              - (reinterpret_cast<std::int64_t>(tr) + static_cast<std::int64_t>(o) + 4));
          std::memcpy(tr+o,&rel,4); o += 4; };
        const auto emitImm32 = [&](std::uint32_t v)
        { std::memcpy(tr+o,&v,4); o += 4; };
        emit({0x40,0x0f,0xb6,0xc7});
        emit({0x8d,0x48,
              static_cast<std::uint8_t>(0x100u - outfit::kCustomPartsTypeStart)});
        emit({0x81,0xf9});
        emitImm32(static_cast<std::uint32_t>(kBandWidth - 1));
        emit({0x77,0x15});
        emit({0x48,0x8d,0x05});
        emitRel32To(reinterpret_cast<std::uintptr_t>(tr) + kTableOff);
        emit({0x0f,0xb6,0x04,0x08});
        emit({0x84,0xc0});
        tr[o++]=0x0f; tr[o++]=0x84;
        emitRel32To(caseD3);
        emit({0xb0,0x01});
        emit({0x83,0xf8,0x19});
        tr[o++]=0x0f; tr[o++]=0x87;
        emitRel32To(caseD3);
        tr[o++]=0xe9;
        emitRel32To(resume);

        const std::int64_t jrel =
            reinterpret_cast<std::int64_t>(tr) - (static_cast<std::int64_t>(site) + 5);
        if (jrel < INT32_MIN || jrel > INT32_MAX)
        { LogDebug("[CaseDArmUnpin] trampoline too far for rel32\n");
          VirtualFree(tr,0,MEM_RELEASE); return false; }

        std::uint8_t patch[9];
        patch[0]=0xe9; std::int32_t jr = static_cast<std::int32_t>(jrel);
        std::memcpy(patch+1,&jr,4);
        patch[5]=patch[6]=patch[7]=patch[8]=0x90;

        DWORD oldp=0;
        if (!VirtualProtect(reinterpret_cast<void*>(site),9,PAGE_EXECUTE_READWRITE,&oldp))
        { Log("[CaseDArmUnpin] VirtualProtect failed\n");
          VirtualFree(tr,0,MEM_RELEASE); return false; }
        std::memcpy(g_CaseDOrigBytes, reinterpret_cast<void*>(site), 9);
        std::memcpy(reinterpret_cast<void*>(site), patch, 9);
        DWORD tmp=0; VirtualProtect(reinterpret_cast<void*>(site),9,oldp,&tmp);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 9);

        g_CaseDTrampoline     = tr;
        g_CaseDPatchSite      = reinterpret_cast<void*>(site);
        g_CaseDArmTable       = tr + kTableOff;
        g_CaseDArmUnpinActive = true;
#ifdef _DEBUG
        LogDebug("[CaseDArmUnpin] installed: site=%p tramp=%p armTable=%p band=0x%02X-0x%02X "
            "(%zu slots) (arm-enabled custom partsType decodes the REAL tier; "
            "enableArm=false routes to the engine's own armless path - tier 0, "
            "hand slot off, flesh knock)\n",
            reinterpret_cast<void*>(site), tr, tr + kTableOff,
            outfit::kCustomPartsTypeStart, outfit::kCustomPartsTypeEnd, kBandWidth);
#endif
        return true;
    }


    static bool ResolveFoxPathApi()
    {
        if (!g_FoxPath_Path)
        {
            g_FoxPath_Path = reinterpret_cast<FoxPath_Path_t>(
                ResolveGameAddress(gAddr.FoxPath_Path));
        }
        return g_FoxPath_Path != nullptr;
    }

    static std::uint64_t* WriteFoxPath(std::uint64_t* outPath, std::uint64_t code64ext)
    {
        if (!outPath || !ResolveFoxPathApi()) return outPath;
        g_FoxPath_Path(outPath, code64ext);
        return outPath;
    }

    static std::uint32_t EffectivePartsType(std::uint32_t paramPartsType)
    {
        if (outfit::shadow::HasCurrentSlot())
        {
            outfit::shadow::Slot s;
            if (outfit::shadow::Get(outfit::shadow::GetCurrentSlot(), &s))
                return s.realPartsType;
        }
        return paramPartsType;
    }

    static std::uint32_t VanillaClampPartsType(std::uint32_t partsType)
    {
        const auto pt = static_cast<std::uint8_t>(partsType & 0xFF);
        if (pt >= outfit::kCustomPartsTypeStart && pt <= outfit::kCustomPartsTypeEnd)
            return kBionicArmVanillaPartsTypeSubstitute;
        return partsType;
    }

    static std::atomic<bool> g_AssetCheckTrusted{ false };

    static bool ShouldFallBackOnMissingAsset(std::uint64_t pathCode64)
    {
        if (pathCode64 == 0) return false;
        if ((pathCode64 >> 51) == 0) return true;
        if (fox::detail::PathExistsByCode(pathCode64))
        {
            g_AssetCheckTrusted.store(true, std::memory_order_relaxed);
            return false;
        }
        return g_AssetCheckTrusted.load(std::memory_order_relaxed);
    }

    static bool ResolveCustomEntry(std::uint32_t playerType, std::uint32_t playerPartsType,
                                   const outfit::OutfitEntry** outEntry)
    {
        const auto pt  = static_cast<std::uint8_t>(playerPartsType & 0xFF);
        const auto ply = static_cast<std::uint8_t>(playerType & 0xFF);

        if (pt < outfit::kCustomPartsTypeStart || pt > outfit::kCustomPartsTypeEnd)
            return false;

        const outfit::OutfitEntry* entry = nullptr;
        if (!outfit::TryGetOutfitByPartsType(pt, &entry) || !entry) return false;
        if (!entry->IsPlayerTypeSupported(ply)) return false;

        if (outEntry) *outEntry = entry;
        return true;
    }

    static std::uint8_t TranslateEquipHashToArmTier(std::uint16_t equipHash)
    {
        switch (equipHash)
        {
        case 0x203: return 2;
        case 0x204: return 3;
        case 0x205: return 4;
        case 0x206: return 5;
        case 0x208: return 6;
        case 0x209: return 7;
        default:    return 1;
        }
    }

    static std::uint8_t ReadLiveArmTierFromLoadoutRequest(void* p2go, std::size_t slot)
    {
        if (!p2go) return 0;
        std::uint8_t result = 0;
        __try
        {
            void* perPlayer = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(p2go) + kP2GO_OffPerPlayerStruct);
            if (!perPlayer) return 0;

            void* loadoutReqArr = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(perPlayer) + kPP_OffLoadoutReq);
            if (!loadoutReqArr) return 0;

            std::uint16_t equipHash = *reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uint8_t*>(loadoutReqArr)
                + slot * kPP_LoadoutReqStride
                + kPP_LoadoutReqEquipHashOff);
            result = TranslateEquipHashToArmTier(equipHash);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = 0; }
        return result;
    }

    static std::uint8_t ResolveSlotPartsTypeByteFromShadow(std::size_t slot,
                                                          std::uint8_t fallback)
    {
        outfit::shadow::Slot s;
        if (outfit::shadow::Get(slot, &s)) return s.realPartsType;
        return fallback;
    }

    static std::atomic<std::uint8_t> g_VextServedPt[outfit::shadow::kMaxSlots];
    static std::atomic<std::uint8_t> g_VextServedVar[outfit::shadow::kMaxSlots];

    static const outfit::VanillaSuitVariantAsset* ResolveVanillaExtActiveVariant(
        std::uint32_t playerType, std::uint32_t effectivePartsType)
    {
        const auto vpt = static_cast<std::uint8_t>(effectivePartsType & 0xFF);
        if (vpt >= outfit::kCustomPartsTypeStart) return nullptr;
        const std::uint8_t v = outfit::GetActiveVariant(vpt);
        if (v == 0) return nullptr;
        if (MissionCodeGuard::ShouldBypassHooks()) return nullptr;
        const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);
        return outfit::VanillaExtGetVariantBridged(vpt, pt, v);
    }

    static std::uint64_t ResolveVanillaExtVariantParts(std::uint32_t playerType,
                                                       std::uint32_t effectivePartsType)
    {
        const auto* var =
            ResolveVanillaExtActiveVariant(playerType, effectivePartsType);
        return var ? var->partsPathCode64 : 0;
    }

    static std::uint64_t ResolveVanillaExtVariantFpk(std::uint32_t playerType,
                                                     std::uint32_t effectivePartsType)
    {
        const auto* var =
            ResolveVanillaExtActiveVariant(playerType, effectivePartsType);
        return var ? var->fpkPathCode64 : 0;
    }

    static const outfit::CustomHeadEntry* ResolveVanillaSuitCustomHead(
        std::uint8_t pt, std::uint32_t playerPartsType, std::uint8_t faceEquipId)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return nullptr;
        const outfit::CustomHeadEntry* head =
            outfit::TryGetCustomHeadBySlot(faceEquipId);
        if (!head)
            head = outfit::TryGetCustomHeadBySlot(t_ActiveCustomFaceSlot);
        if (!head)
            head = outfit::TryGetCustomHeadBySlot(outfit::GetWornCustomHeadSlot());
        if (!head)
        {
            if (const std::uint16_t worn = outfit::GetCurrentWornHeadEquipId();
                worn != 0)
                head = outfit::TryGetCustomHeadByEquipId(worn);
        }
        if (!head || pt >= outfit::kPlayerTypeMax)
            return nullptr;
        if (!outfit::VanillaExtHasHeadOption(
                static_cast<std::uint8_t>(playerPartsType & 0xFF),
                head->equipId, pt, outfit::ReadLiveSelectorCode()))
            return nullptr;
        return head;
    }

    static std::uint8_t VanillaExtNeedsFaceFova(std::uint32_t effective)
    {
        if (effective >= outfit::kCustomPartsTypeStart)
            return 0;
        const auto livePT = outfit::ReadLivePlayerType();
        const outfit::CustomHeadEntry* head = ResolveVanillaSuitCustomHead(
            livePT, effective, t_ActiveCustomFaceSlot);
        return head ? std::uint8_t{1} : std::uint8_t{0};
    }


    static std::uint64_t* __fastcall hkLoadPlayerPartsParts(
        std::uint64_t* outPath, std::uint32_t playerType, std::uint32_t playerPartsType)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);
            const std::uint8_t v = entry->HasVariants()
                ? outfit::GetActiveVariant(entry->partsType) : 0;
            const std::uint64_t path = entry->GetVariantPartsPath(pt, v);
            if (path != 0)
            {
                if (ShouldFallBackOnMissingAsset(path))
                {
                    LogDebug("[OutfitRuntimeParts] BRICK-GUARD: custom .parts asset "
                        "missing (code=0x%016llX pt=%u) - loading vanilla\n",
                        static_cast<unsigned long long>(path),
                        static_cast<unsigned>(playerType));
                    return g_OrigLoadPartsParts(outPath, playerType,
                                                kBionicArmVanillaPartsTypeSubstitute);
                }
                return WriteFoxPath(outPath, path);
            }
        }
        if (const std::uint64_t path = ResolveVanillaExtVariantParts(
                playerType, effectivePartsType); path != 0)
        {
            const bool vfallback = ShouldFallBackOnMissingAsset(path);
            const bool vexists   = fox::detail::PathExistsByCode(path);
#ifdef _DEBUG
            static std::atomic<int> s_vextPartsDbg{0};
            if (int n = s_vextPartsDbg.load(std::memory_order_relaxed); n < 16)
            {
                s_vextPartsDbg.store(n + 1, std::memory_order_relaxed);
                LogDebug("[OutfitRuntimeParts:vextserve] PARTS pt=%u vpt=0x%02X "
                    "active=%u path=0x%016llX fallback=%d exists=%d -> %s\n",
                    static_cast<unsigned>(playerType),
                    static_cast<unsigned>(effectivePartsType & 0xFF),
                    static_cast<unsigned>(outfit::GetActiveVariant(
                        static_cast<std::uint8_t>(effectivePartsType & 0xFF))),
                    static_cast<unsigned long long>(path),
                    vfallback ? 1 : 0, vexists ? 1 : 0,
                    (!vfallback && vexists) ? "SERVE" : "fallback-vanilla");
            }
#endif
            if (!vfallback && vexists)
                return WriteFoxPath(outPath, path);
        }
        return g_OrigLoadPartsParts(outPath, playerType, VanillaClampPartsType(playerPartsType));
    }

    static std::uint64_t* __fastcall hkLoadPlayerPartsFpk(
        std::uint64_t* outPath, std::uint32_t playerType, std::uint32_t playerPartsType)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);
            const std::uint8_t v = entry->HasVariants()
                ? outfit::GetActiveVariant(entry->partsType) : 0;
            const std::uint64_t path = entry->GetVariantFpkPath(pt, v);
            if (path != 0)
            {
                if (ShouldFallBackOnMissingAsset(path))
                {
                    LogDebug("[OutfitRuntimeParts] BRICK-GUARD: custom .fpk asset "
                        "missing (code=0x%016llX pt=%u) - loading vanilla\n",
                        static_cast<unsigned long long>(path),
                        static_cast<unsigned>(playerType));
                    return g_OrigLoadPartsFpk(outPath, playerType,
                                              kBionicArmVanillaPartsTypeSubstitute);
                }
                return WriteFoxPath(outPath, path);
            }
        }
        if (effectivePartsType < outfit::kCustomPartsTypeStart)
        {
            const auto servedIdx = static_cast<std::size_t>(playerType & 0x3);
            const auto servedPt  = static_cast<std::uint8_t>(effectivePartsType & 0xFF);
            g_VextServedPt[servedIdx].store(servedPt, std::memory_order_relaxed);
            g_VextServedVar[servedIdx].store(outfit::GetActiveVariant(servedPt),
                                             std::memory_order_relaxed);
        }
        if (const std::uint64_t path = ResolveVanillaExtVariantFpk(
                playerType, effectivePartsType); path != 0)
        {
            const bool vfallback = ShouldFallBackOnMissingAsset(path);
            const bool vexists   = fox::detail::PathExistsByCode(path);
#ifdef _DEBUG
            static std::atomic<int> s_vextFpkDbg{0};
            if (int n = s_vextFpkDbg.load(std::memory_order_relaxed); n < 16)
            {
                s_vextFpkDbg.store(n + 1, std::memory_order_relaxed);
                LogDebug("[OutfitRuntimeParts:vextserve] FPK pt=%u vpt=0x%02X "
                    "active=%u path=0x%016llX fallback=%d exists=%d -> %s\n",
                    static_cast<unsigned>(playerType),
                    static_cast<unsigned>(effectivePartsType & 0xFF),
                    static_cast<unsigned>(outfit::GetActiveVariant(
                        static_cast<std::uint8_t>(effectivePartsType & 0xFF))),
                    static_cast<unsigned long long>(path),
                    vfallback ? 1 : 0, vexists ? 1 : 0,
                    (!vfallback && vexists) ? "SERVE" : "fallback-vanilla");
            }
#endif
            if (!vfallback && vexists)
                return WriteFoxPath(outPath, path);
        }
        return g_OrigLoadPartsFpk(outPath, playerType, VanillaClampPartsType(playerPartsType));
    }

    static std::uint32_t ClampVanillaCamo(std::uint32_t camo)
    {
        if (camo < 0x75) return camo;
        static std::atomic<int> s_log{0};
        if (int n = s_log.load(std::memory_order_relaxed); n < 4)
        {
            s_log.store(n + 1, std::memory_order_relaxed);
            LogDebug("[OutfitRuntimeParts] CAMO-CLAMP: out-of-table camo 0x%02X on a "
                "vanilla realize - clamping to 0 (vanilla camo path table has "
                "0x75 entries, unbounded in the engine; prevents fatal load)\n",
                camo);
        }
        return 0;
    }

    static std::uint64_t* __fastcall hkLoadPlayerCamoFpk(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerCamoType)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);

            const std::uint8_t v = entry->HasVariants()
                ? outfit::GetActiveVariant(entry->partsType) : 0;
            const std::uint64_t camo = entry->GetVariantCamoFpk(pt, v);
            if (camo > outfit::kSubAssetUseVanilla)
            {
                if (ShouldFallBackOnMissingAsset(camo))
                    return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
                return WriteFoxPath(outPath, camo);
            }
            return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
        }

        if (playerCamoType >= outfit::kCustomSelectorStart
            && playerCamoType <= outfit::kCustomSelectorEnd)
        {
            std::uint8_t vselPt = 0, vselIdx = 0;
            if (outfit::TryGetVanillaExtByVariantSelector(
                    static_cast<std::uint8_t>(playerCamoType), &vselPt, &vselIdx)
                && vselPt == static_cast<std::uint8_t>(effectivePartsType & 0xFF))
            {
                outfit::SetActiveVariant(vselPt, vselIdx);
                if (const auto* var = outfit::VanillaExtGetVariant(
                        vselPt, static_cast<std::uint8_t>(playerType & 0xFF), vselIdx))
                {
                    if (var->camoFpk > outfit::kSubAssetUseVanilla
                        && !ShouldFallBackOnMissingAsset(var->camoFpk)
                        && fox::detail::PathExistsByCode(var->camoFpk))
                        return WriteFoxPath(outPath, var->camoFpk);
                    const std::uint8_t vBaseCamo =
                        outfit::VanillaExtGetVariantSourceCamo(vselPt, vselIdx);
                    if (vBaseCamo != 0xFF)
                        return g_OrigLoadCamoFpk(outPath, playerType,
                                                 VanillaClampPartsType(playerPartsType),
                                                 ClampVanillaCamo(vBaseCamo));
                }
            }
        }
        if (const outfit::VanillaSuitVariantAsset* var =
                ResolveVanillaExtActiveVariant(playerType, effectivePartsType))
        {
            if (var->camoFpk > outfit::kSubAssetUseVanilla
                && !ShouldFallBackOnMissingAsset(var->camoFpk)
                && fox::detail::PathExistsByCode(var->camoFpk))
                return WriteFoxPath(outPath, var->camoFpk);
            const auto vpt = static_cast<std::uint8_t>(effectivePartsType & 0xFF);
            const std::uint8_t src = outfit::VanillaExtGetVariantSourceCamo(
                vpt, outfit::GetActiveVariant(vpt));
            if (src != 0xFF)
                return g_OrigLoadCamoFpk(outPath, playerType,
                                         VanillaClampPartsType(playerPartsType),
                                         ClampVanillaCamo(src));
        }
        return g_OrigLoadCamoFpk(outPath, playerType, VanillaClampPartsType(playerPartsType),
                                 ClampVanillaCamo(playerCamoType));
    }

    static std::uint64_t* __fastcall hkLoadPlayerSnakeBlackDiamondFpk(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t applyBlackDiamond)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);
            const std::uint8_t v = entry->HasVariants()
                ? outfit::GetActiveVariant(entry->partsType) : 0;
            const std::uint64_t diamond = entry->GetVariantDiamondFpk(pt, v);
            if (diamond == outfit::kSubAssetDisabled)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
            if (diamond > outfit::kSubAssetUseVanilla)
            {
                if (ShouldFallBackOnMissingAsset(diamond))
                    return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
                return WriteFoxPath(outPath, diamond);
            }
        }
        if (const outfit::VanillaSuitVariantAsset* var =
                ResolveVanillaExtActiveVariant(playerType, effectivePartsType))
        {
            if (var->diamondFpk == outfit::kSubAssetDisabled)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
            if (var->diamondFpk > outfit::kSubAssetUseVanilla
                && !ShouldFallBackOnMissingAsset(var->diamondFpk)
                && fox::detail::PathExistsByCode(var->diamondFpk))
                return WriteFoxPath(outPath, var->diamondFpk);
        }
        return g_OrigLoadDiamondFpk(outPath, playerType, VanillaClampPartsType(playerPartsType), applyBlackDiamond);
    }

    static std::uint64_t* __fastcall hkLoadPlayerCamoFv2(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerCamoType)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);

            const std::uint8_t v = entry->HasVariants()
                ? outfit::GetActiveVariant(entry->partsType) : 0;
            const std::uint64_t camo = entry->GetVariantCamoFv2(pt, v);
            if (camo > outfit::kSubAssetUseVanilla)
            {
                if (ShouldFallBackOnMissingAsset(camo))
                    return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
                return WriteFoxPath(outPath, camo);
            }
            if (camo == outfit::kSubAssetDisabled)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
        }

        if (playerCamoType >= outfit::kCustomSelectorStart
            && playerCamoType <= outfit::kCustomSelectorEnd)
        {
            std::uint8_t vselPt = 0, vselIdx = 0;
            if (outfit::TryGetVanillaExtByVariantSelector(
                    static_cast<std::uint8_t>(playerCamoType), &vselPt, &vselIdx)
                && vselPt == static_cast<std::uint8_t>(effectivePartsType & 0xFF))
            {
                outfit::SetActiveVariant(vselPt, vselIdx);
                if (const auto* var = outfit::VanillaExtGetVariant(
                        vselPt, static_cast<std::uint8_t>(playerType & 0xFF), vselIdx))
                {
                    if (var->camoFv2 > outfit::kSubAssetUseVanilla
                        && !ShouldFallBackOnMissingAsset(var->camoFv2)
                        && fox::detail::PathExistsByCode(var->camoFv2))
                        return WriteFoxPath(outPath, var->camoFv2);
                    const std::uint8_t vBaseCamo =
                        outfit::VanillaExtGetVariantSourceCamo(vselPt, vselIdx);
                    if (vBaseCamo != 0xFF)
                        return g_OrigLoadCamoFv2(outPath, playerType,
                                                 VanillaClampPartsType(playerPartsType),
                                                 ClampVanillaCamo(vBaseCamo));
                }
            }
        }
        if (const outfit::VanillaSuitVariantAsset* var =
                ResolveVanillaExtActiveVariant(playerType, effectivePartsType))
        {
            if (var->camoFv2 > outfit::kSubAssetUseVanilla
                && !ShouldFallBackOnMissingAsset(var->camoFv2)
                && fox::detail::PathExistsByCode(var->camoFv2))
                return WriteFoxPath(outPath, var->camoFv2);
            const auto vpt = static_cast<std::uint8_t>(effectivePartsType & 0xFF);
            const std::uint8_t src = outfit::VanillaExtGetVariantSourceCamo(
                vpt, outfit::GetActiveVariant(vpt));
            if (src != 0xFF)
                return g_OrigLoadCamoFv2(outPath, playerType,
                                         VanillaClampPartsType(playerPartsType),
                                         ClampVanillaCamo(src));
        }
        return g_OrigLoadCamoFv2(outPath, playerType, VanillaClampPartsType(playerPartsType),
                                 ClampVanillaCamo(playerCamoType));
    }

    static std::uint64_t* __fastcall hkLoadPlayerSnakeBlackDiamondFv2(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t applyBlackDiamond)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);
            const std::uint8_t v = entry->HasVariants()
                ? outfit::GetActiveVariant(entry->partsType) : 0;
            const std::uint64_t diamond = entry->GetVariantDiamondFv2(pt, v);
            if (diamond == outfit::kSubAssetDisabled)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
            if (diamond > outfit::kSubAssetUseVanilla)
            {
                if (ShouldFallBackOnMissingAsset(diamond))
                    return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
                return WriteFoxPath(outPath, diamond);
            }
        }
        if (const outfit::VanillaSuitVariantAsset* var =
                ResolveVanillaExtActiveVariant(playerType, effectivePartsType))
        {
            if (var->diamondFv2 == outfit::kSubAssetDisabled)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
            if (var->diamondFv2 > outfit::kSubAssetUseVanilla
                && !ShouldFallBackOnMissingAsset(var->diamondFv2)
                && fox::detail::PathExistsByCode(var->diamondFv2))
                return WriteFoxPath(outPath, var->diamondFv2);
        }
        return g_OrigLoadDiamondFv2(outPath, playerType, VanillaClampPartsType(playerPartsType), applyBlackDiamond);
    }

    static std::uint32_t RecoverArmTierForLeaf(std::uint32_t playerType,
                                               std::uint32_t passedHandType)
    {
        if (passedHandType != 0) return passedHandType;
        bool captured = false;
        std::uint8_t cachedTier = outfit::shadow::GetArmTier(playerType, &captured);
        return captured ? static_cast<std::uint32_t>(cachedTier) : 1u;
    }

    static std::uint64_t* __fastcall hkLoadPlayerBionicArmFv2(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerHandType)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);
        const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            if (!entry->IsArmEnabled(pt))
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
            const std::uint32_t hand = RecoverArmTierForLeaf(playerType, playerHandType);
            return g_OrigLoadBionicArmFv2(outPath, playerType,
                                          kBionicArmVanillaPartsTypeSubstitute, hand);
        }
        if (static_cast<std::uint8_t>(effectivePartsType & 0xFF) < outfit::kCustomPartsTypeStart)
        {
            bool armEnable = true;
            if (outfit::VanillaExtGetSuitArm(
                    static_cast<std::uint8_t>(effectivePartsType & 0xFF),
                    pt, outfit::ReadLiveSelectorCode(), &armEnable)
                && !armEnable)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
        }
        return g_OrigLoadBionicArmFv2(outPath, playerType, VanillaClampPartsType(playerPartsType), playerHandType);
    }

    static std::uint64_t* __fastcall hkLoadPlayerBionicArmFpk(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerHandType)
    {
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);
        const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);

        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            if (!entry->IsArmEnabled(pt))
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
            const std::uint32_t hand = RecoverArmTierForLeaf(playerType, playerHandType);
            return g_OrigLoadBionicArmFpk(outPath, playerType,
                                          kBionicArmVanillaPartsTypeSubstitute, hand);
        }
        if (static_cast<std::uint8_t>(effectivePartsType & 0xFF) < outfit::kCustomPartsTypeStart)
        {
            bool armEnable = true;
            if (outfit::VanillaExtGetSuitArm(
                    static_cast<std::uint8_t>(effectivePartsType & 0xFF),
                    pt, outfit::ReadLiveSelectorCode(), &armEnable)
                && !armEnable)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
        }
        return g_OrigLoadBionicArmFpk(outPath, playerType, VanillaClampPartsType(playerPartsType), playerHandType);
    }

    static std::uint64_t* __fastcall hkLoadPlayerSnakeFaceFv2(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerFaceId, char playerFaceEquipId)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigLoadSnakeFaceFv2(outPath, playerType,
                                          VanillaClampPartsType(playerPartsType),
                                          playerFaceId, playerFaceEquipId);
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);
        const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);
        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            if (!entry->IsHeadEnabled(pt))
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);

            const outfit::CustomHeadEntry* head =
                outfit::TryGetCustomHeadBySlot(static_cast<std::uint8_t>(playerFaceEquipId));
            if (!head)
                head = outfit::TryGetCustomHeadBySlot(t_ActiveCustomFaceSlot);
            if (!head)
                head = outfit::TryGetCustomHeadBySlot(outfit::GetWornCustomHeadSlot());
            if (!head)
            {
                if (const std::uint16_t worn = outfit::GetCurrentWornHeadEquipId(); worn != 0)
                    head = outfit::TryGetCustomHeadByEquipId(worn);
            }

#ifdef _DEBUG
            {
                static int s_snakeHeadDiag = 0;
                if (s_snakeHeadDiag < 24)
                {
                    ++s_snakeHeadDiag;
                    LogDebug("[SnakeHead] Fv2 hook: pt=%u faceEquipId=0x%02X activeSlot=0x%02X "
                        "-> resolved '%s' (fv2Code=0x%016llX)\n",
                        static_cast<unsigned>(pt),
                        static_cast<unsigned>(static_cast<std::uint8_t>(playerFaceEquipId)),
                        static_cast<unsigned>(t_ActiveCustomFaceSlot),
                        head ? head->name : "(none)",
                        head ? static_cast<unsigned long long>(head->faceFv2Code[pt]) : 0ull);
                }
            }
#endif

            if (head && pt < outfit::kPlayerTypeMax)
            {
                std::uint64_t code = head->faceFv2Code[pt];
                if (pt == outfit::kPlayerType_Snake)
                    if (const std::uint64_t st = outfit::GetCustomHeadSnakeStageFv2(
                            head->name, playerFaceId); st != 0)
                        code = st;
                if (code != 0)
                    return WriteFoxPath(outPath, code);
            }

            std::uint64_t* origFv2 = g_OrigLoadSnakeFaceFv2(outPath, playerType,
                                          kBionicArmVanillaPartsTypeSubstitute,
                                          playerFaceId, playerFaceEquipId);
#ifdef _DEBUG
            {
                static int s_fv2OrigDiag = 0;
                if (s_fv2OrigDiag < 12)
                {
                    ++s_fv2OrigDiag;
                    LogDebug("[SnakeHead] Fv2 ORIG: faceEquipId=0x%02X faceId=%u -> code=0x%016llX\n",
                        static_cast<unsigned>(static_cast<std::uint8_t>(playerFaceEquipId)),
                        static_cast<unsigned>(playerFaceId),
                        origFv2 ? static_cast<unsigned long long>(origFv2[0]) : 0ull);
                }
            }
#endif
            return origFv2;
        }
        if (static_cast<std::uint8_t>(effectivePartsType & 0xFF) < outfit::kCustomPartsTypeStart)
        {
            bool headEnable = true;
            if (outfit::VanillaExtGetSuitHead(
                    static_cast<std::uint8_t>(effectivePartsType & 0xFF),
                    pt, outfit::ReadLiveSelectorCode(), &headEnable)
                && !headEnable)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
        }
        const outfit::CustomHeadEntry* vextHead = ResolveVanillaSuitCustomHead(
            pt, playerPartsType, static_cast<std::uint8_t>(playerFaceEquipId));
#ifdef _DEBUG
        if (!vextHead)
        {
            const std::uint8_t wornSlot =
                (static_cast<std::uint8_t>(playerFaceEquipId) != 0)
                    ? static_cast<std::uint8_t>(playerFaceEquipId)
                    : outfit::GetWornCustomHeadSlot();
            if (outfit::IsCustomHeadSlot(wornSlot))
            {
                static int s_vextHeadMissDiag = 0;
                if (s_vextHeadMissDiag < 12)
                {
                    ++s_vextHeadMissDiag;
                    LogDebug("[SnakeHead] vanilla-suit Fv2: slot 0x%02X is a registered "
                        "custom head but partsType=0x%02X pt=%u selector=0x%02X does "
                        "not declare it - serving the vanilla face\n",
                        static_cast<unsigned>(wornSlot),
                        static_cast<unsigned>(effectivePartsType & 0xFF),
                        static_cast<unsigned>(pt),
                        static_cast<unsigned>(outfit::ReadLiveSelectorCode()));
                }
            }
        }
#endif
        if (const outfit::CustomHeadEntry* head = vextHead)
        {
            std::uint64_t code = head->faceFv2Code[pt];
            if (pt == outfit::kPlayerType_Snake)
                if (const std::uint64_t st = outfit::GetCustomHeadSnakeStageFv2(
                        head->name, playerFaceId); st != 0)
                    code = st;
#ifdef _DEBUG
            {
                static int s_vextHeadFv2Diag = 0;
                if (s_vextHeadFv2Diag < 24)
                {
                    ++s_vextHeadFv2Diag;
                    LogDebug("[SnakeHead] vanilla-suit Fv2: partsType=0x%02X pt=%u "
                        "faceEquipId=0x%02X activeSlot=0x%02X head='%s' "
                        "code=0x%016llX exists=%d\n",
                        static_cast<unsigned>(effectivePartsType & 0xFF),
                        static_cast<unsigned>(pt),
                        static_cast<unsigned>(
                            static_cast<std::uint8_t>(playerFaceEquipId)),
                        static_cast<unsigned>(t_ActiveCustomFaceSlot),
                        head->name,
                        static_cast<unsigned long long>(code),
                        (code != 0 && fox::detail::PathExistsByCode(code)) ? 1 : 0);
                }
            }
#endif
            if (code != 0)
            {
                if (!ShouldFallBackOnMissingAsset(code)
                    && fox::detail::PathExistsByCode(code))
                    return WriteFoxPath(outPath, code);

                static std::atomic<std::uint64_t> s_loggedMissingFv2{ 0 };
                if (s_loggedMissingFv2.exchange(code) != code)
                    LogDebug("[OutfitHeadOption] custom head '%s' face .fv2 is not in "
                        "any mounted archive (code=0x%016llX pt=%u partsType=0x%02X) "
                        "- the head model never loads, so the face renders invisible "
                        "and the face FOVA model slot stays null, which faults the "
                        "parts apply on the next player rebuild; falling back to the "
                        "vanilla face for this suit\n",
                        head->name, static_cast<unsigned long long>(code),
                        static_cast<unsigned>(pt),
                        static_cast<unsigned>(effectivePartsType & 0xFF));
            }
        }
        return g_OrigLoadSnakeFaceFv2(outPath, playerType, VanillaClampPartsType(playerPartsType),
                                      playerFaceId, playerFaceEquipId);
    }

    static std::uint64_t* __fastcall hkLoadPlayerSnakeFaceFpk(
        std::uint64_t* outPath, std::uint32_t playerType,
        std::uint32_t playerPartsType, std::uint32_t playerFaceId, char playerFaceEquipId)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigLoadSnakeFaceFpk(outPath, playerType,
                                          VanillaClampPartsType(playerPartsType),
                                          playerFaceId, playerFaceEquipId);
        const std::uint32_t effectivePartsType = EffectivePartsType(playerPartsType);
        const auto pt = static_cast<std::uint8_t>(playerType & 0xFF);
        const outfit::OutfitEntry* entry = nullptr;
        if (ResolveCustomEntry(playerType, effectivePartsType, &entry))
        {
            if (!entry->IsHeadEnabled(pt))
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);


            const std::uint8_t activeSlot = t_ActiveCustomFaceSlot;
            const outfit::CustomHeadEntry* head =
                outfit::TryGetCustomHeadBySlot(activeSlot);
            if (!head)
                head = outfit::TryGetCustomHeadBySlot(
                    static_cast<std::uint8_t>(playerFaceEquipId));
            if (!head)
                head = outfit::TryGetCustomHeadBySlot(outfit::GetWornCustomHeadSlot());
            if (!head)
            {
                if (const std::uint16_t worn = outfit::GetCurrentWornHeadEquipId(); worn != 0)
                    head = outfit::TryGetCustomHeadByEquipId(worn);
            }

#ifdef _DEBUG
            {
                static int s_snakeHeadFpkDiag = 0;
                if (s_snakeHeadFpkDiag < 24)
                {
                    ++s_snakeHeadFpkDiag;
                    LogDebug("[SnakeHead] Fpk hook: pt=%u faceEquipId=0x%02X activeSlot=0x%02X "
                        "-> resolved '%s' (fpkCode=0x%016llX)\n",
                        static_cast<unsigned>(pt),
                        static_cast<unsigned>(static_cast<std::uint8_t>(playerFaceEquipId)),
                        static_cast<unsigned>(activeSlot),
                        head ? head->name : "(none)",
                        head ? static_cast<unsigned long long>(head->faceFpkCode[pt]) : 0ull);
                }
            }
#endif

            if (head && pt < outfit::kPlayerTypeMax)
            {
                std::uint64_t code = head->faceFpkCode[pt];
                if (pt == outfit::kPlayerType_Snake)
                    if (const std::uint64_t st = outfit::GetCustomHeadSnakeStageFpk(
                            head->name, playerFaceId); st != 0)
                        code = st;
                if (code != 0)
                    return WriteFoxPath(outPath, code);
            }

            std::uint64_t* origFpk = g_OrigLoadSnakeFaceFpk(outPath, playerType,
                                          kBionicArmVanillaPartsTypeSubstitute,
                                          playerFaceId, playerFaceEquipId);
#ifdef _DEBUG
            {
                static int s_fpkOrigDiag = 0;
                if (s_fpkOrigDiag < 12)
                {
                    ++s_fpkOrigDiag;
                    LogDebug("[SnakeHead] Fpk ORIG: faceEquipId=0x%02X faceId=%u -> code=0x%016llX\n",
                        static_cast<unsigned>(static_cast<std::uint8_t>(playerFaceEquipId)),
                        static_cast<unsigned>(playerFaceId),
                        origFpk ? static_cast<unsigned long long>(origFpk[0]) : 0ull);
                }
            }
#endif
            return origFpk;
        }
        if (static_cast<std::uint8_t>(effectivePartsType & 0xFF) < outfit::kCustomPartsTypeStart)
        {
            bool headEnable = true;
            if (outfit::VanillaExtGetSuitHead(
                    static_cast<std::uint8_t>(effectivePartsType & 0xFF),
                    pt, outfit::ReadLiveSelectorCode(), &headEnable)
                && !headEnable)
                return WriteFoxPath(outPath, outfit::kSubAssetDisabled);
        }
        if (const outfit::CustomHeadEntry* head = ResolveVanillaSuitCustomHead(
                pt, playerPartsType, static_cast<std::uint8_t>(playerFaceEquipId)))
        {
            std::uint64_t code = head->faceFpkCode[pt];
            if (pt == outfit::kPlayerType_Snake)
                if (const std::uint64_t st = outfit::GetCustomHeadSnakeStageFpk(
                        head->name, playerFaceId); st != 0)
                    code = st;
#ifdef _DEBUG
            {
                static int s_vextHeadFpkDiag = 0;
                if (s_vextHeadFpkDiag < 24)
                {
                    ++s_vextHeadFpkDiag;
                    LogDebug("[SnakeHead] vanilla-suit Fpk: partsType=0x%02X pt=%u "
                        "faceEquipId=0x%02X activeSlot=0x%02X head='%s' "
                        "code=0x%016llX exists=%d\n",
                        static_cast<unsigned>(effectivePartsType & 0xFF),
                        static_cast<unsigned>(pt),
                        static_cast<unsigned>(
                            static_cast<std::uint8_t>(playerFaceEquipId)),
                        static_cast<unsigned>(t_ActiveCustomFaceSlot),
                        head->name,
                        static_cast<unsigned long long>(code),
                        (code != 0 && fox::detail::PathExistsByCode(code)) ? 1 : 0);
                }
            }
#endif
            if (code != 0)
            {
                if (!ShouldFallBackOnMissingAsset(code)
                    && fox::detail::PathExistsByCode(code))
                    return WriteFoxPath(outPath, code);

                static std::atomic<std::uint64_t> s_loggedMissingFpk{ 0 };
                if (s_loggedMissingFpk.exchange(code) != code)
                    LogDebug("[OutfitHeadOption] custom head '%s' face .fpk is not in "
                        "any mounted archive (code=0x%016llX pt=%u partsType=0x%02X) "
                        "- the head model package never mounts, so the face renders "
                        "invisible even when its .fv2 resolves; falling back to the "
                        "vanilla face for this suit\n",
                        head->name, static_cast<unsigned long long>(code),
                        static_cast<unsigned>(pt),
                        static_cast<unsigned>(effectivePartsType & 0xFF));
            }
        }
        return g_OrigLoadSnakeFaceFpk(outPath, playerType, VanillaClampPartsType(playerPartsType),
                                      playerFaceId, playerFaceEquipId);
    }

    using FoxModelFromHandle_t = long long (__fastcall*)(long long handle,
                                                         std::uint32_t idx);
    static FoxModelFromHandle_t g_FoxModelFromHandle = nullptr;
    constexpr std::uint32_t kFoxModelHidden  = 0xFFFFFFFFu;

    static const outfit::CustomHeadEntry* ResolveAvatarCustomHead()
    {
        if (!g_InstalledAvatarHeadOptionFv2 || !g_InstalledAvatarHeadOptionFpk)
            return nullptr;

        const outfit::CustomHeadEntry* head =
            outfit::TryGetCustomHeadBySlot(t_ActiveCustomFaceSlot);
        if (!head)
            head = outfit::TryGetCustomHeadBySlot(
                g_LastCustomFaceSlot.load(std::memory_order_relaxed));

        if (head
            && (head->faceFv2Code[outfit::kPlayerType_Avatar] == 0
                || head->faceFpkCode[outfit::kPlayerType_Avatar] == 0))
            return nullptr;
        return head;
    }

    static std::uint64_t* __fastcall hkLoadAvatarFaceFv2(
        std::uint64_t* outPath, std::uint32_t avatarFaceA, std::uint32_t avatarFaceB)
    {
        return g_OrigLoadAvatarFaceFv2(outPath, avatarFaceA, avatarFaceB);
    }

    static std::uint64_t* __fastcall hkLoadAvatarFaceFpk(
        std::uint64_t* outPath, std::uint32_t avatarFaceA, std::uint32_t avatarFaceB)
    {
        return g_OrigLoadAvatarFaceFpk(outPath, avatarFaceA, avatarFaceB);
    }

    static std::uint64_t* __fastcall hkLoadAvatarHeadOptionFv2(
        std::uint64_t* outPath, std::uint32_t faceId)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigLoadAvatarHeadOptionFv2(outPath, faceId);
        if (const outfit::CustomHeadEntry* head = ResolveAvatarCustomHead())
        {
#ifdef _DEBUG
            static int s_diag = 0;
            if (s_diag < 8)
            {
                ++s_diag;
                LogDebug("[SnakeHead] HeadOptionFv2 hook: avatar head '%s' replaces "
                    "the headwear fova (faceId=%u, fv2Code=0x%016llX)\n",
                    head->name, faceId,
                    static_cast<unsigned long long>(
                        head->faceFv2Code[outfit::kPlayerType_Avatar]));
            }
#endif
            return WriteFoxPath(outPath,
                head->faceFv2Code[outfit::kPlayerType_Avatar]);
        }
        return g_OrigLoadAvatarHeadOptionFv2(outPath, faceId);
    }

    static std::uint64_t* __fastcall hkLoadAvatarHeadOptionFpk(
        std::uint64_t* outPath, std::uint32_t faceId)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigLoadAvatarHeadOptionFpk(outPath, faceId);
        if (const outfit::CustomHeadEntry* head = ResolveAvatarCustomHead())
        {
#ifdef _DEBUG
            static int s_diag = 0;
            if (s_diag < 8)
            {
                ++s_diag;
                LogDebug("[SnakeHead] HeadOptionFpk hook: avatar head '%s' pack "
                    "mounted in the head-option slot (fpkCode=0x%016llX)\n",
                    head->name,
                    static_cast<unsigned long long>(
                        head->faceFpkCode[outfit::kPlayerType_Avatar]));
            }
#endif
            return WriteFoxPath(outPath,
                head->faceFpkCode[outfit::kPlayerType_Avatar]);
        }
        return g_OrigLoadAvatarHeadOptionFpk(outPath, faceId);
    }

    static void __fastcall hkAvatarFaceEditUpdate(
        void* self, void* blockGroup, std::uint32_t blockIndex)
    {
        if (g_OrigAvatarFaceEditUpdate)
            g_OrigAvatarFaceEditUpdate(self, blockGroup, blockIndex);
    }

    static void HideAvatarCreatorFacesPerFrame()
    {
        void* bcv = g_AvatarHideBc.load(std::memory_order_relaxed);
        const std::uint32_t mask =
            g_AvatarHideSlotMask.load(std::memory_order_relaxed);
        if (!bcv || mask == 0 || !g_FoxModelFromHandle)
            return;
        if (ResolveAvatarCustomHead() == nullptr)
            return;

        __try
        {
            auto* bc = reinterpret_cast<std::uint8_t*>(bcv);
            for (std::uint32_t i = 0; i < 32; ++i)
            {
                if ((mask & (1u << i)) == 0)
                    continue;

                const std::uint32_t seqState =
                    *reinterpret_cast<std::uint32_t*>(bc + 0x10c0 + i * 4);
#ifdef _DEBUG
                {
                    static std::uint32_t s_lastState[32] = {};
                    if (s_lastState[i] != seqState + 1)
                    {
                        s_lastState[i] = seqState + 1;
                        LogDebug("[SnakeHead] hide gate: bcSlot=%u seqState=%u "
                            "armFlag=%u (hide fires on 1 or 3)\n",
                            i, seqState,
                            *reinterpret_cast<std::uint32_t*>(bc + 0x1080 + i * 4));
                    }
                }
#endif
                if (seqState != 3u && seqState != 1u)
                    continue;
                long long controller =
                    reinterpret_cast<long long>(bc + 0x228 + i * 0x98);
                long long handle = *reinterpret_cast<long long*>(controller + 0x60);
                if (!handle) continue;
                long long model = g_FoxModelFromHandle(handle, 0);
                if (!model) continue;
                *reinterpret_cast<std::uint32_t*>(model + 0x1a4) = kFoxModelHidden;
#ifdef _DEBUG
                static int s_hideDiag = 0;
                if (s_hideDiag < 4)
                {
                    ++s_hideDiag;
                    LogDebug("[SnakeHead] creator face hidden (bcSlot %u, custom "
                        "head worn)\n", i);
                }
#endif
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }


    static std::uint8_t __fastcall hkDoesNeedFaceFova(std::uint32_t playerPartsType)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigDoesNeedFaceFova
                ? g_OrigDoesNeedFaceFova(playerPartsType) : 0;
        const std::uint32_t effective = EffectivePartsType(playerPartsType);
        if (effective >= outfit::kCustomPartsTypeStart && effective <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            const auto pt     = static_cast<std::uint8_t>(effective & 0xFF);
            const auto livePT = outfit::ReadLivePlayerType();
            if (outfit::TryGetOutfitByPartsType(pt, &entry) && entry)
                return entry->IsHeadEnabled(livePT) ? std::uint8_t{1} : std::uint8_t{0};
        }
        if (effective < outfit::kCustomPartsTypeStart)
        {
            bool headEnable = true;
            if (outfit::VanillaExtGetSuitHead(
                    static_cast<std::uint8_t>(effective & 0xFF),
                    outfit::ReadLivePlayerType(), outfit::ReadLiveSelectorCode(),
                    &headEnable)
                && !headEnable)
                return 0;
        }
        const std::uint8_t orig =
            g_OrigDoesNeedFaceFova ? g_OrigDoesNeedFaceFova(playerPartsType) : 0;
        if (orig == 0)
            return VanillaExtNeedsFaceFova(effective);
        return orig;
    }

    static std::uint8_t __fastcall hkDoesNeedFaceFovaForAvatar(std::uint32_t playerPartsType)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigDoesNeedFaceFovaForAvatar
                ? g_OrigDoesNeedFaceFovaForAvatar(playerPartsType) : 0;
        const std::uint32_t effective = EffectivePartsType(playerPartsType);
        if (effective >= outfit::kCustomPartsTypeStart && effective <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            const auto pt     = static_cast<std::uint8_t>(effective & 0xFF);
            const auto livePT = outfit::ReadLivePlayerType();
            if (outfit::TryGetOutfitByPartsType(pt, &entry) && entry)
                return entry->IsHeadEnabled(livePT) ? std::uint8_t{1} : std::uint8_t{0};
        }
        if (effective < outfit::kCustomPartsTypeStart)
        {
            bool headEnable = true;
            if (outfit::VanillaExtGetSuitHead(
                    static_cast<std::uint8_t>(effective & 0xFF),
                    outfit::ReadLivePlayerType(), outfit::ReadLiveSelectorCode(),
                    &headEnable)
                && !headEnable)
                return 0;
        }
        const std::uint8_t orig = g_OrigDoesNeedFaceFovaForAvatar
             ? g_OrigDoesNeedFaceFovaForAvatar(playerPartsType) : 0;
        if (orig == 0)
            return VanillaExtNeedsFaceFova(effective);
        return orig;
    }

    static void __fastcall hkSetHandSlotEnabled(void* self_equipController,
                                                std::uint32_t slot, std::uint8_t enabled)
    {
        if (enabled != 0)
        {
            const std::uint8_t livePT        = outfit::ReadLivePlayerType();
            const std::uint8_t livePartsType = outfit::ReadLivePartsType();
            if (livePartsType >= outfit::kCustomPartsTypeStart
             && livePartsType <= outfit::kCustomPartsTypeEnd)
            {
                const outfit::OutfitEntry* entry = nullptr;
                if (outfit::TryGetOutfitByPartsType(livePartsType, &entry) && entry
                    && entry->IsPlayerTypeSupported(livePT)
                    && !entry->IsArmEnabled(livePT))
                {
                    if (g_OrigSetHandSlotEnabled)
                        g_OrigSetHandSlotEnabled(self_equipController, slot, 0);
                    return;
                }
            }
            if (g_OrigSetHandSlotEnabled)
                g_OrigSetHandSlotEnabled(self_equipController, slot, enabled);
            return;
        }

        const std::uint8_t livePT        = outfit::ReadLivePlayerType();
        const std::uint8_t livePartsType = outfit::ReadLivePartsType();

        const bool liveIsSnakeOrAvatar =
               (livePT == outfit::kPlayerType_Snake)
            || (livePT == outfit::kPlayerType_Avatar);
        const bool liveIsCustomPartsType =
               (livePartsType >= outfit::kCustomPartsTypeStart
             && livePartsType <= outfit::kCustomPartsTypeEnd);

        if (liveIsSnakeOrAvatar && liveIsCustomPartsType)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByPartsType(livePartsType, &entry) && entry
                && entry->IsPlayerTypeSupported(livePT)
                && entry->IsArmEnabled(livePT))
            {
                if (g_OrigSetHandSlotEnabled)
                    g_OrigSetHandSlotEnabled(self_equipController, slot, 1);
                return;
            }
        }
        if (g_OrigSetHandSlotEnabled)
            g_OrigSetHandSlotEnabled(self_equipController, slot, enabled);
    }

    static std::uint8_t __fastcall hkIsArtificialHandEnabled(
        std::uint32_t playerType, std::uint32_t playerPartsType)
    {
        if (playerPartsType >= outfit::kCustomPartsTypeStart
         && playerPartsType <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            const auto pt  = static_cast<std::uint8_t>(playerPartsType & 0xFF);
            const auto ply = static_cast<std::uint8_t>(playerType & 0xFF);
            if (outfit::TryGetOutfitByPartsType(pt, &entry) && entry
                && entry->IsPlayerTypeSupported(ply)
                && entry->IsArmEnabled(ply))
                return 1;
        }
        return g_OrigIsArtificialHandEnabled
             ? g_OrigIsArtificialHandEnabled(playerType, playerPartsType) : 0;
    }

    static std::uint8_t __fastcall hkIsArtificialHandEnabledForCurrentPlayerType()
    {
        const std::uint8_t livePT        = outfit::ReadLivePlayerType();
        const std::uint8_t livePartsType = outfit::ReadLivePartsType();

        if (livePartsType >= outfit::kCustomPartsTypeStart
         && livePartsType <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByPartsType(livePartsType, &entry) && entry
                && entry->IsPlayerTypeSupported(livePT)
                && entry->IsArmEnabled(livePT))
                return 1;
        }
        return g_OrigIsArtificialHandForCurrent ? g_OrigIsArtificialHandForCurrent() : 0;
    }


    static void __fastcall hkProcessSignal(void* p1, void* p2,
                                           std::uint32_t slot, std::uint64_t* signalPtr)
    {
        if (!signalPtr || *signalPtr != kSignalRefreshFv2s)
        {
            if (g_OrigProcessSignal) g_OrigProcessSignal(p1, p2, slot, signalPtr);
            return;
        }

        std::uint8_t* partsTypeArr = nullptr;
        std::uint8_t  origByte     = 0;
        bool          spoofWritten = false;

        __try
        {
            void* perPlayer = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(p1) + kP2GO_OffPerPlayerStruct);
            if (perPlayer)
            {
                partsTypeArr = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(perPlayer) + kPP_OffPartsTypeArr);
                if (partsTypeArr)
                {
                    origByte = partsTypeArr[slot];
                    if (origByte >= outfit::kCustomPartsTypeStart
                     && origByte <= outfit::kCustomPartsTypeEnd)
                    {
                        outfit::shadow::SetCurrentSlot(slot);
                        partsTypeArr[slot] = kProcessSignalSpoofPartsType;
                        spoofWritten = true;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { partsTypeArr = nullptr; }

        if (g_OrigProcessSignal) g_OrigProcessSignal(p1, p2, slot, signalPtr);

        if (spoofWritten)
        {
            __try { partsTypeArr[slot] = origByte; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            outfit::shadow::ClearCurrentSlot();
        }
    }


    static void __fastcall hkUpdatePartsStatus(void* self)
    {
        EquipDevelopAdd::MaybeRefreshDynamicGates();

        struct SlotOverride { bool active; std::uint8_t restoreValue; };
        SlotOverride  overrides[outfit::shadow::kMaxSlots] = {};
        std::uint8_t* armTypeArr = nullptr;

        __try
        {
            void* perPlayer = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(self) + kP2GO_OffPerPlayerStruct);
            std::uint8_t* stateMachineArr = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(self) + kP2GO_OffStateMachinePtr);

            if (perPlayer)
            {
                std::uint8_t* partsTypeArr = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(perPlayer) + kPP_OffPartsTypeArr);
                std::uint8_t* playerTypeArr = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(perPlayer) + kPP_OffPlayerTypeArr);
                armTypeArr = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(perPlayer) + kPP_OffArmTypeArr);
                std::uint8_t* camoTypeArr = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(perPlayer) + kPP_OffCamoTypeArr);
                std::uint32_t* stateChangedBits = reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(perPlayer) + kPP_OffStateChangedBits);

                if (partsTypeArr && playerTypeArr && armTypeArr)
                {
                    std::uint32_t vextRestream = 0;
                    if (!MissionCodeGuard::ShouldBypassHooks())
                        for (std::size_t i = 0;
                             i < outfit::shadow::kMaxSlots; ++i)
                        {
                            const std::uint8_t pt = partsTypeArr[i];
                            if (pt >= outfit::kCustomPartsTypeStart) continue;
                            if (!stateMachineArr || stateMachineArr[i] != 3)
                                continue;
                            if (outfit::VanillaExtVariantSlotCount(pt) == 0)
                                continue;
                            const auto servedIdx =
                                static_cast<std::size_t>(playerTypeArr[i] & 0x3);
                            if (g_VextServedPt[servedIdx].load(
                                    std::memory_order_relaxed) != pt) continue;
                            const std::uint8_t want =
                                outfit::GetActiveVariant(pt);
                            const std::uint8_t have =
                                g_VextServedVar[servedIdx].load(
                                    std::memory_order_relaxed);
                            if (have == want) continue;
                            vextRestream |= (1u << i);
                        }
                    if (vextRestream)
                    {
                        *reinterpret_cast<std::uint32_t*>(
                            reinterpret_cast<std::uint8_t*>(perPlayer)
                            + kPP_OffVariantChangedBits) |= vextRestream;
                    }
#ifdef _DEBUG
                    {
                        static std::uint32_t s_lastRestreamMask = 0;
                        if (vextRestream != s_lastRestreamMask)
                        {
                            s_lastRestreamMask = vextRestream;
                            if (vextRestream)
                                LogDebug("[OutfitRuntimeParts:vextrestream] active "
                                    "vext variant differs from last-served - "
                                    "forcing slot re-stream (mask=0x%X)\n",
                                    vextRestream);
                        }
                    }
#endif

                    for (std::size_t i = 0; i < outfit::shadow::kMaxSlots; ++i)
                    {
                        const std::uint8_t pt  = partsTypeArr[i];
                        const std::uint8_t ply = playerTypeArr[i];

                        if (pt < outfit::kCustomPartsTypeStart
                         || pt > outfit::kCustomPartsTypeEnd) continue;

                        const outfit::OutfitEntry* entry = nullptr;
                        if (!outfit::TryGetOutfitByPartsType(pt, &entry) || !entry) continue;
                        if (!entry->IsPlayerTypeSupported(ply)) continue;

                        if (camoTypeArr)
                        {
                            const std::uint8_t camo = camoTypeArr[i];
                            if (camo < outfit::kCustomSelectorStart
                             || camo > outfit::kCustomSelectorEnd)
                            {
                                static std::uint16_t s_lastCollisionKey
                                    [outfit::shadow::kMaxSlots] =
                                    { 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu };
                                const std::uint16_t ck =
                                    static_cast<std::uint16_t>((pt << 8) | camo);
                                if (s_lastCollisionKey[i] != ck)
                                {
                                    s_lastCollisionKey[i] = ck;
                                    LogDebug("[OutfitRuntimeParts] camo/partsType "
                                        "collision guard: slot=%zu partsType=0x%02X "
                                        "camo=0x%02X ply=%u (vanilla camo in custom "
                                        "partsType range) - not a real custom slot, "
                                        "skipping to prevent mis-resolve hang\n",
                                        i, static_cast<unsigned>(pt),
                                        static_cast<unsigned>(camo),
                                        static_cast<unsigned>(ply));
                                }
                                continue;
                            }
                        }

                        RefreshCaseDArmFlag(pt, entry->IsArmEnabled(ply));

                        if (!entry->IsArmEnabled(ply)) continue;

                        const std::uint8_t liveTier =
                            ReadLiveArmTierFromLoadoutRequest(self, i);

                        bool cachedFlag = false;
                        const std::uint8_t cachedTier =
                            outfit::shadow::GetArmTier(ply, &cachedFlag);

                        const std::uint8_t resolvedTier =
                            (liveTier > 0) ? liveTier
                                           : (cachedFlag ? cachedTier : std::uint8_t{1});

                        if (liveTier > 0)
                            outfit::shadow::SetArmTier(ply, liveTier);

                        outfit::shadow::Slot ss;
                        if (!outfit::shadow::Get(i, &ss))
                            ss = outfit::shadow::Slot{};
                        ss.realPartsType  = pt;
                        ss.realCamoType   = entry->selectorCode;
                        ss.realArmType    = resolvedTier;
                        ss.realPlayerType = ply;
                        ss.developId      = entry->developId;
                        ss.variantIdx     = entry->HasVariants()
                                          ? outfit::GetActiveVariant(pt) : 0;
                        outfit::shadow::Set(i, ss);

                        overrides[i].active       = true;
                        overrides[i].restoreValue = resolvedTier;

                        (void)stateChangedBits;
                        if (!g_CaseDArmUnpinActive && armTypeArr) armTypeArr[i] = 0;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            for (std::size_t i = 0; i < outfit::shadow::kMaxSlots; ++i)
                overrides[i].active = false;
            armTypeArr = nullptr;
        }

        g_OrigUpdatePartsStatus(self);

        if (armTypeArr && !g_CaseDArmUnpinActive)
        {
            __try
            {
                for (std::size_t i = 0; i < outfit::shadow::kMaxSlots; ++i)
                    if (overrides[i].active)
                        armTypeArr[i] = overrides[i].restoreValue;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        HideAvatarCreatorFacesPerFrame();
    }


    static bool __fastcall hkPlayer2ImplSetUpParts(
        void* self, std::uint32_t slot,
        std::uint32_t playerType, std::uint32_t partsType,
        std::uint32_t camo, std::uint32_t armType,
        std::uint32_t faceId, void* avatarInfo)
    {
        if (!g_OrigPlayer2ImplSetUpParts) return false;

        std::uint32_t effectiveArmType = armType;
        if (partsType >= outfit::kCustomPartsTypeStart
         && partsType <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByPartsType(
                    static_cast<std::uint8_t>(partsType & 0xFF), &entry)
                && entry
                && entry->IsPlayerTypeSupported(
                       static_cast<std::uint8_t>(playerType & 0xFF)))
            {
                if (!entry->IsArmEnabled(static_cast<std::uint8_t>(playerType & 0xFF)))
                {
                    effectiveArmType = 0;
                }
                else if (armType == 0)
                {
                    bool cachedFlag = false;
                    std::uint8_t cachedTier =
                        outfit::shadow::GetArmTier(playerType, &cachedFlag);
                    effectiveArmType = cachedFlag ? static_cast<std::uint32_t>(cachedTier) : 1u;
                }
            }
        }

        return g_OrigPlayer2ImplSetUpParts(self, slot, playerType, partsType,
                                           camo, effectiveArmType, faceId, avatarInfo);
    }

    static std::uint32_t __fastcall hkGetPartsTypeAtCamoType(
        void* self, std::uint32_t camo)
    {
        const std::uint32_t r = g_OrigGetPartsTypeAtCamoType
            ? g_OrigGetPartsTypeAtCamoType(self, camo)
            : 0;

        const auto ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto stateInBox = reinterpret_cast<std::uintptr_t>(
            ResolveGameAddress(gAddr.SupplyCboxActionPluginImpl_StateInBox));
        const bool fromCratePickup =
            stateInBox != 0 && ra >= stateInBox && ra < stateInBox + 0xB90;

        if (fromCratePickup && camo < outfit::kCustomSelectorStart)
        {
            outfit::ResetAllVanillaExtVariants();
#ifdef _DEBUG
            static std::atomic<int> s_crateResetLog{0};
            if (int n = s_crateResetLog.load(std::memory_order_relaxed); n < 8)
            {
                s_crateResetLog.store(n + 1, std::memory_order_relaxed);
                LogDebug("[OutfitRuntimeParts] SupplyCbox pickup of plain vanilla "
                    "camo 0x%02X -> vext variants reset (crate delivers the "
                    "base suit, matching menu-equip semantics)\n", camo);
            }
#endif
        }

        if (r == 0
            && camo >= outfit::kCustomSelectorStart
            && camo <= outfit::kCustomSelectorEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            std::uint8_t variantIdx = 0;
            if (outfit::TryGetOutfitByVariantSelector(
                    static_cast<std::uint8_t>(camo), &entry, &variantIdx)
                && entry)
            {
                const bool activate = fromCratePickup;
                if (activate)
                {
                    if (outfit::PeekPendingSupplyDropDevelopId()
                            == entry->developId)
                    {
                        variantIdx =
                            outfit::ConsumePendingSupplyDropVariantIdx();
                        outfit::ConsumePendingSupplyDropDevelopId();
                        outfit::SetCrateDeliveredVariant(entry->developId,
                                                         variantIdx);
                    }
                    else if (outfit::PeekCrateDeliveredDevelopId()
                             == entry->developId)
                    {
                        variantIdx = outfit::PeekCrateDeliveredVariantIdx();
                    }
                    outfit::SetActiveVariant(entry->partsType, variantIdx);
                }
#ifdef _DEBUG
                static std::atomic<int> s_log{0};
                if (int n = s_log.load(std::memory_order_relaxed); n < 8)
                {
                    s_log.store(n + 1, std::memory_order_relaxed);
                    LogDebug("[OutfitRuntimeParts] SupplyCbox camo->partsType: "
                        "custom camo 0x%02X -> partsType 0x%02X "
                        "(developId=%u variantIdx=%u %s; vanilla map "
                        "returned 0)\n",
                        camo,
                        static_cast<unsigned>(entry->partsType),
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(variantIdx),
                        activate ? "ACTIVATED variant"
                                 : "resolved only (base, deferred to active)");
                }
#endif
                return entry->partsType;
            }

            std::uint8_t vpt = 0, vidx = 0;
            if (outfit::TryGetVanillaExtByVariantSelector(
                    static_cast<std::uint8_t>(camo), &vpt, &vidx))
            {
                if (fromCratePickup)
                    outfit::SetActiveVariant(vpt, vidx);
                return vpt;
            }

            LogDebug("[OutfitRuntimeParts] SupplyCbox camo->partsType: custom camo "
                "0x%02X UNRESOLVED - leaving vanilla 0 (dangling guard will "
                "degrade to vanilla suit, no hang)\n", camo);
        }

        return r;
    }


    static void __fastcall hkLoadPartsNew(
        void* self, std::uint32_t playerIndex,
        LoadPartsPlayerInfo* info, std::uint32_t flags)
    {
        if (!g_CapturedBlockController && self)
            g_CapturedBlockController = self;

        if (!info)
        {
            g_OrigLoadPartsNew(self, playerIndex, info, flags);
            return;
        }

        if (info->playerFaceEquipId >= outfit::kCustomHeadSlotBase
            && !outfit::IsCustomHeadSlot(info->playerFaceEquipId)
            && outfit::HasPendingCustomHeads())
        {
            if (outfit::DrainPendingHeads() > 0)
                LogDebug("[OutfitRuntimeParts] realize carries worn custom head "
                    "slot 0x%02X while its registration was still deferred - "
                    "drained the pending heads before resolving the face\n",
                    static_cast<unsigned>(info->playerFaceEquipId));
        }

        const bool isRealPlayerSlot =
            (info->playerType == outfit::ReadLivePlayerType());

        auto feedShadow = [&](const outfit::OutfitEntry* e, std::uint8_t variantIdx) {
            outfit::shadow::Slot ss{};
            ss.realPartsType  = e->partsType;
            ss.realCamoType   = e->selectorCode;
            ss.realArmType    = info->playerArmType;
            ss.realPlayerType = info->playerType;
            ss.developId      = e->developId;
            ss.variantIdx     = variantIdx;
            outfit::shadow::Set(playerIndex, ss);
        };

        if (info->playerPartsType == 0)
        {
            const std::uint8_t camo = info->playerCamoType;
            if (camo >= outfit::kCustomSelectorStart && camo <= outfit::kCustomSelectorEnd)
            {
                const outfit::OutfitEntry* entry = nullptr;
                std::uint8_t variantIdx = 0;
                if (outfit::TryGetOutfitByVariantSelector(camo, &entry, &variantIdx)
                    && entry
                    && entry->IsPlayerTypeSupported(info->playerType))
                {
                    if (outfit::PeekPendingSupplyDropDevelopId()
                            == entry->developId)
                    {
                        variantIdx =
                            outfit::ConsumePendingSupplyDropVariantIdx();
                        outfit::ConsumePendingSupplyDropDevelopId();
                        outfit::SetCrateDeliveredVariant(entry->developId,
                                                         variantIdx);
                        if (isRealPlayerSlot)
                            outfit::ResetAllVanillaExtVariants();
                    }
                    else if (outfit::PeekCrateDeliveredDevelopId()
                             == entry->developId)
                    {
                        variantIdx = outfit::PeekCrateDeliveredVariantIdx();
                    }
                    const std::uint8_t persistSel =
                        entry->GetVariantSelectorCode(variantIdx);
                    info->playerPartsType = entry->partsType;
                    info->playerCamoType  = persistSel;
                    outfit::SetActiveVariant(entry->partsType, variantIdx);
                    if (isRealPlayerSlot)
                        outfit::WriteLivePlayerOutfit(entry->partsType,
                                                      persistSel,
                                                      info->playerType);
                    feedShadow(entry, variantIdx);
                    outfit::ClearPendingOutfitDevelopId();
                }
                else
                {
                }
            }
            else if (camo == 0xFF)
            {
                const outfit::OutfitEntry* byPending = nullptr;
                const std::uint16_t pendingDevId = outfit::GetPendingOutfitDevelopId();
                if (pendingDevId != 0 && !outfit::IsOutfitBound(pendingDevId))
                    outfit::BindOutfit(pendingDevId, true, "pending-render");
                if (pendingDevId != 0
                    && outfit::TryGetOutfitByDevelopId(pendingDevId, &byPending) && byPending
                    && byPending->bound
                    && byPending->IsPlayerTypeSupported(info->playerType))
                {
                    const std::uint8_t pendVar = byPending->HasVariants()
                        ? outfit::GetActiveVariant(byPending->partsType)
                        : std::uint8_t{0};
                    const std::uint8_t persistSel =
                        byPending->GetVariantSelectorCode(pendVar);
                    info->playerPartsType = byPending->partsType;
                    info->playerCamoType  = persistSel;
                    if (isRealPlayerSlot)
                        outfit::WriteLivePlayerOutfit(byPending->partsType,
                                                      persistSel,
                                                      info->playerType);
                    feedShadow(byPending, pendVar);
                    outfit::ClearPendingOutfitDevelopId();
                }
                else
                {
                    LogDebug("[OutfitRuntimeParts] BRICK-GUARD: broken-custom signal "
                        "(partsType=0 camo=0xFF) with no pending developId "
                        "(pt=%u) - healing to vanilla NORMAL (0x01), no hang\n",
                        static_cast<unsigned>(info->playerType));
                    info->playerPartsType    = kBionicArmVanillaPartsTypeSubstitute;
                    info->playerCamoType     = 0;
                    info->playerFaceEquipId  = 0;
                    info->playerFaceEquipUnk =
                        static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
                    if (isRealPlayerSlot)
                        outfit::WriteLivePlayerOutfit(
                            kBionicArmVanillaPartsTypeSubstitute, 0, info->playerType);
                }
            }
        }

        if (info->playerPartsType != 0
            && (info->playerPartsType < outfit::kCustomPartsTypeStart
             || info->playerPartsType > outfit::kCustomPartsTypeEnd))
        {
            const std::uint8_t camo = info->playerCamoType;
            if (camo >= outfit::kCustomSelectorStart
             && camo <= outfit::kCustomSelectorEnd)
            {
                const outfit::OutfitEntry* mixEntry = nullptr;
                std::uint8_t variantIdx = 0;
                if (outfit::TryGetOutfitByVariantSelector(camo, &mixEntry, &variantIdx)
                    && mixEntry
                    && mixEntry->IsPlayerTypeSupported(info->playerType))
                {
                    if (outfit::PeekPendingSupplyDropDevelopId()
                            == mixEntry->developId)
                    {
                        variantIdx =
                            outfit::ConsumePendingSupplyDropVariantIdx();
                        outfit::ConsumePendingSupplyDropDevelopId();
                        outfit::SetCrateDeliveredVariant(mixEntry->developId,
                                                         variantIdx);
                        if (isRealPlayerSlot)
                            outfit::ResetAllVanillaExtVariants();
                    }
                    else if (outfit::PeekCrateDeliveredDevelopId()
                             == mixEntry->developId)
                    {
                        variantIdx = outfit::PeekCrateDeliveredVariantIdx();
                    }
                    LogDebug("[OutfitRuntimeParts] MIX-REINTERPRET: vanilla "
                        "partsType=0x%02X + custom camo=0x%02X (pt=%u) - "
                        "persisted mixed pair; reinterpreting as custom outfit "
                        "developId=%u partsType=0x%02X variantIdx=%u\n",
                        static_cast<unsigned>(info->playerPartsType),
                        static_cast<unsigned>(camo),
                        static_cast<unsigned>(info->playerType),
                        static_cast<unsigned>(mixEntry->developId),
                        static_cast<unsigned>(mixEntry->partsType),
                        static_cast<unsigned>(variantIdx));
                    const std::uint8_t persistSel =
                        mixEntry->GetVariantSelectorCode(variantIdx);
                    info->playerPartsType = mixEntry->partsType;
                    info->playerCamoType  = persistSel;
                    outfit::SetActiveVariant(mixEntry->partsType, variantIdx);
                    if (isRealPlayerSlot)
                        outfit::WriteLivePlayerOutfit(mixEntry->partsType,
                                                      persistSel,
                                                      info->playerType);
                    feedShadow(mixEntry, variantIdx);
                    outfit::ClearPendingOutfitDevelopId();
                }
            }
        }

        const outfit::OutfitEntry* entry = nullptr;
        bool isCustom = ResolveCustomEntry(info->playerType,
                                           info->playerPartsType, &entry);

        if (isCustom && entry)
        {
            if (outfit::PeekPendingSupplyDropDevelopId() == entry->developId)
            {
                const std::uint8_t orderedVar =
                    outfit::ConsumePendingSupplyDropVariantIdx();
                outfit::ConsumePendingSupplyDropDevelopId();
                outfit::SetCrateDeliveredVariant(entry->developId, orderedVar);
                outfit::SetActiveVariant(entry->partsType, orderedVar);
                if (isRealPlayerSlot)
                    outfit::ResetAllVanillaExtVariants();
                LogDebug("[OutfitRuntimeParts] ordered variant applied at realize: "
                    "developId=%u partsType=0x%02X variantIdx=%u (pending "
                    "supply order consumed)\n",
                    static_cast<unsigned>(entry->developId),
                    static_cast<unsigned>(entry->partsType),
                    static_cast<unsigned>(orderedVar));
            }
            else if (outfit::PeekCrateDeliveredDevelopId()
                     == entry->developId)
            {
                const std::uint8_t deliveredVar =
                    outfit::PeekCrateDeliveredVariantIdx();
                outfit::SetActiveVariant(entry->partsType, deliveredVar);
#ifdef _DEBUG
                static std::atomic<int> s_deliverLog{0};
                if (int n = s_deliverLog.load(std::memory_order_relaxed); n < 8)
                {
                    s_deliverLog.store(n + 1, std::memory_order_relaxed);
                    LogDebug("[OutfitRuntimeParts] crate-delivered variant "
                        "re-asserted at realize: developId=%u partsType=0x%02X "
                        "variantIdx=%u\n",
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(entry->partsType),
                        static_cast<unsigned>(deliveredVar));
                }
#endif
            }
        }

        if (isCustom && entry)
        {
            const std::uint8_t livePT =
                static_cast<std::uint8_t>(info->playerType & 0xFF);
            const std::uint8_t v = entry->HasVariants()
                ? outfit::GetActiveVariant(entry->partsType) : 0;
            const std::uint64_t declParts = entry->GetVariantPartsPath(livePT, v);
            const std::uint64_t declFpk   = entry->GetVariantFpkPath(livePT, v);
            const bool partsInvalid = declParts && (declParts >> 51) == 0;
            const bool fpkInvalid   = declFpk   && (declFpk   >> 51) == 0;
            const bool partsPresent =
                declParts && !partsInvalid && fox::detail::PathExistsByCode(declParts);
            const bool fpkPresent =
                declFpk   && !fpkInvalid   && fox::detail::PathExistsByCode(declFpk);
            const bool anyPresent = partsPresent || fpkPresent;
            if (anyPresent)
                g_AssetCheckTrusted.store(true, std::memory_order_relaxed);
            const bool partsMissing = declParts && !partsPresent;
            const bool fpkMissing   = declFpk   && !fpkPresent;
            const bool structurallyInvalid = partsInvalid || fpkInvalid;
            if ((partsMissing || fpkMissing) && (anyPresent || structurallyInvalid))
            {
                LogDebug("[OutfitRuntimeParts] BRICK-GUARD: resolved custom outfit "
                    "developId=%u partsType=0x%02X has a BAD asset path "
                    "(parts=%s fpk=%s; pt=%u; evidence=%s) - degrading to "
                    "vanilla NORMAL (0x01) to prevent infinite load\n",
                    static_cast<unsigned>(entry->developId),
                    static_cast<unsigned>(entry->partsType),
                    partsInvalid ? "INVALID-EXT" : (partsMissing ? "MISSING" : "ok"),
                    fpkInvalid   ? "INVALID-EXT" : (fpkMissing   ? "MISSING" : "ok"),
                    static_cast<unsigned>(info->playerType),
                    structurallyInvalid ? "structural" : "sibling-present");
                info->playerPartsType    = kBionicArmVanillaPartsTypeSubstitute;
                info->playerCamoType     = 0;
                info->playerFaceEquipId  = 0;
                info->playerFaceEquipUnk =
                    static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
                outfit::shadow::Clear(playerIndex);
                if (isRealPlayerSlot)
                    outfit::WriteLivePlayerOutfit(
                        kBionicArmVanillaPartsTypeSubstitute, 0, info->playerType);
                isCustom = false;
                entry    = nullptr;
            }
        }

        if (!isCustom)
        {
            const std::uint8_t pt  = info->playerPartsType;
            const std::uint8_t sel = info->playerCamoType;
            const bool ptIsCustomRange =
                pt >= outfit::kCustomPartsTypeStart && pt <= outfit::kCustomPartsTypeEnd;

            std::uint8_t vextPartsType = 0;
            std::uint8_t vextVariantIdx = 0;
            if (!ptIsCustomRange
                && outfit::TryGetVanillaExtByVariantSelector(
                       sel, &vextPartsType, &vextVariantIdx)
                && (pt == vextPartsType
                    || (pt == 0
                        && outfit::GetActiveVariant(vextPartsType) != 0)))
            {
                const outfit::VanillaSuitVariantAsset* vextVar =
                    outfit::VanillaExtGetVariant(
                        vextPartsType,
                        static_cast<std::uint8_t>(info->playerType & 0xFF),
                        vextVariantIdx);
                const std::uint64_t vp = vextVar ? vextVar->partsPathCode64 : 0;
                const std::uint64_t vf = vextVar ? vextVar->fpkPathCode64 : 0;
                const bool vpInvalid = vp && (vp >> 51) == 0;
                const bool vfInvalid = vf && (vf >> 51) == 0;
                const bool vpPresent =
                    vp && !vpInvalid && fox::detail::PathExistsByCode(vp);
                const bool vfPresent =
                    vf && !vfInvalid && fox::detail::PathExistsByCode(vf);
                const bool anyPresent = vpPresent || vfPresent;
                if (anyPresent)
                    g_AssetCheckTrusted.store(true, std::memory_order_relaxed);
                const bool missing = (vp && !vpPresent) || (vf && !vfPresent);
                const bool structurallyInvalid = vpInvalid || vfInvalid;
                const bool assetsBad =
                    !vextVar || (missing && (anyPresent || structurallyInvalid));
                const std::uint64_t vcf = vextVar ? vextVar->camoFv2 : 0;
                const std::uint64_t vcp = vextVar ? vextVar->camoFpk : 0;
                const int vcfState = (vcf <= outfit::kSubAssetUseVanilla) ? 2
                    : (fox::detail::PathExistsByCode(vcf) ? 1 : 0);
                const int vcpState = (vcp <= outfit::kSubAssetUseVanilla) ? 2
                    : (fox::detail::PathExistsByCode(vcp) ? 1 : 0);
                LogDebug("[OutfitRuntimeParts:vextdiag] pt=%u idx=%u vextVar=%d "
                         "vp=0x%016llX vf=0x%016llX vpPresent=%d vfPresent=%d "
                         "camoFv2=0x%016llX(%d) camoFpk=0x%016llX(%d) assetsBad=%d\n",
                         static_cast<unsigned>(info->playerType & 0xFF),
                         static_cast<unsigned>(vextVariantIdx),
                         vextVar ? 1 : 0,
                         static_cast<unsigned long long>(vp),
                         static_cast<unsigned long long>(vf),
                         vpPresent ? 1 : 0, vfPresent ? 1 : 0,
                         static_cast<unsigned long long>(vcf), vcfState,
                         static_cast<unsigned long long>(vcp), vcpState,
                         assetsBad ? 1 : 0);
                if (assetsBad)
                {
                    const std::uint8_t srcCamo =
                        outfit::VanillaExtGetVariantSourceCamo(vextPartsType,
                                                               vextVariantIdx);
                    info->playerPartsType = vextPartsType;
                    info->playerCamoType  =
                        (srcCamo != 0xFF) ? srcCamo : std::uint8_t{0};
                    LogDebug("[OutfitRuntimeParts] BRICK-GUARD: vext variant "
                        "partsType=0x%02X variantIdx=%u missing/invalid asset - "
                        "healing to source camo 0x%02X (vanilla base) to prevent "
                        "infinite load\n",
                        static_cast<unsigned>(vextPartsType),
                        static_cast<unsigned>(vextVariantIdx),
                        static_cast<unsigned>(info->playerCamoType));
                    if (isRealPlayerSlot)
                        outfit::WriteLivePlayerOutfit(vextPartsType,
                                                      info->playerCamoType,
                                                      info->playerType);
                }
                else
                {
                    outfit::SetActiveVariant(vextPartsType, vextVariantIdx);
                    info->playerPartsType = vextPartsType;
                    const std::uint8_t serveCamo =
                        outfit::VanillaExtGetVariantSourceCamo(vextPartsType,
                                                               vextVariantIdx);
                    if (serveCamo != 0xFF)
                        info->playerCamoType = serveCamo;
                    if (isRealPlayerSlot)
                        outfit::WriteLivePlayerOutfit(vextPartsType, sel,
                                                      info->playerType);
                }
            }
            else
            {
                const bool danglingPT = ptIsCustomRange;
                const bool danglingSel =
                    sel >= outfit::kCustomSelectorStart && sel <= outfit::kCustomSelectorEnd;
                if (danglingPT || danglingSel)
                {
                    const std::uint8_t healPartsType =
                        (!danglingPT && pt != 0)
                            ? pt : kBionicArmVanillaPartsTypeSubstitute;
                    LogDebug("[OutfitRuntimeParts] BRICK-GUARD: unresolved custom suit "
                        "partsType=0x%02X selector=0x%02X (pt=%u) - healing to "
                        "vanilla partsType=0x%02X / camo=0 / faceEquip=0x00 to "
                        "prevent infinite load\n",
                        static_cast<unsigned>(pt), static_cast<unsigned>(sel),
                        static_cast<unsigned>(info->playerType),
                        static_cast<unsigned>(healPartsType));
                    info->playerPartsType = healPartsType;
                    info->playerCamoType  = 0;
                    info->playerFaceEquipId  = 0;
                    info->playerFaceEquipUnk =
                        static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
                    if (isRealPlayerSlot)
                        outfit::WriteLivePlayerOutfit(healPartsType, 0, info->playerType);
                }
            }
        }

        if (isCustom)
        {
            const auto livePT = info->playerType;

            RefreshCaseDArmFlag(entry->partsType, entry->IsArmEnabled(livePT));

            if (!entry->IsArmEnabled(livePT))
            {
                info->playerArmType = 0;
            }
            else if (info->playerArmType == 0)
            {
                bool cachedFlag = false;
                std::uint8_t cachedTier = outfit::shadow::GetArmTier(livePT, &cachedFlag);
                info->playerArmType = cachedFlag ? cachedTier : std::uint8_t{1};
            }

            if (!entry->IsFaceEnabled(livePT))
            {
                info->playerFaceEquipId = 0;
                info->playerFaceEquipUnk =
                    static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
            }

            if (isRealPlayerSlot)
                outfit::RememberPlayerTypeOutfit(
                    livePT, info->playerPartsType, info->playerCamoType);

            feedShadow(entry, entry->HasVariants()
                              ? outfit::GetActiveVariant(entry->partsType) : 0);
        }

        {
            const std::uint8_t wornHead = info->playerFaceEquipId;
            if (wornHead >= outfit::kCustomHeadSlotBase
                && outfit::IsCustomHeadSlot(wornHead))
            {
                const outfit::CustomHeadEntry* h =
                    outfit::TryGetCustomHeadBySlot(wornHead);
                const auto vpt =
                    static_cast<std::uint8_t>(info->playerPartsType & 0xFF);
                const bool offered = h
                    && ((isCustom && entry
                         && entry->HasHeadOptionAnyVariant(h->equipId,
                                                           info->playerType))
                        || (!isCustom
                            && vpt < outfit::kCustomPartsTypeStart
                            && outfit::VanillaExtHasHeadOption(
                                   vpt, h->equipId, info->playerType,
                                   info->playerCamoType)));
                if (!offered || MissionCodeGuard::ShouldBypassHooks())
                {
                    LogDebug("[OutfitRuntimeParts] custom head slot 0x%02X dropped "
                        "at realize: equipId=%u partsType=0x%02X playerType=%u "
                        "(%s) - the head is not offered by the worn outfit for "
                        "this player type\n",
                        static_cast<unsigned>(wornHead),
                        static_cast<unsigned>(h ? h->equipId : 0),
                        static_cast<unsigned>(vpt),
                        static_cast<unsigned>(info->playerType),
                        MissionCodeGuard::ShouldBypassHooks()
                            ? "FOB bypass" : "not offered");
                    info->playerFaceEquipId  = 0;
                    info->playerFaceEquipUnk =
                        static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
                }
            }
            else if (isCustom && entry
                     && wornHead >= 1 && wornHead <= 5)
            {
                const std::uint16_t vanEquipId =
                    static_cast<std::uint16_t>(wornHead + 0x20D);
                if (!entry->HasHeadOptionAnyVariant(vanEquipId, info->playerType))
                {
                    info->playerFaceEquipId  = 0;
                    info->playerFaceEquipUnk =
                        static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
                }
            }
        }

        if (info->playerArmType != 0)
            outfit::shadow::SetArmTier(info->playerType, info->playerArmType);

        t_ActiveCustomFaceSlot = 0;
        if (info->playerType == outfit::kPlayerType_Snake
            || info->playerType == outfit::kPlayerType_Avatar)
        {
            if (info->playerFaceEquipId >= outfit::kCustomHeadSlotBase
                && outfit::IsCustomHeadSlot(info->playerFaceEquipId))
            {
                if (MissionCodeGuard::ShouldBypassHooks())
                {
                    info->playerFaceEquipId  = 0;
                    info->playerFaceEquipUnk =
                        static_cast<std::uint8_t>(
                            info->playerFaceEquipUnk & 0xF8);
                    g_LastCustomFaceSlot.store(0, std::memory_order_relaxed);
                }
                else
                {
#ifdef _DEBUG
                LogDebug("[SnakeHead] LoadPartsNew: normalized custom head faceEquipId "
                    "0x%02X -> 0x01 (bandana variation) for pt=%u; real slot kept "
                    "for the face hooks\n",
                    static_cast<unsigned>(info->playerFaceEquipId),
                    static_cast<unsigned>(info->playerType));
#endif
                t_ActiveCustomFaceSlot = info->playerFaceEquipId;
                g_LastCustomFaceSlot.store(info->playerFaceEquipId,
                                           std::memory_order_relaxed);
                info->playerFaceEquipId = 1;
                }
            }
            else
            {
                g_LastCustomFaceSlot.store(0, std::memory_order_relaxed);
            }

            constexpr bool kHideAvatarCreatorFace = false;
            if (info->playerType == outfit::kPlayerType_Avatar)
            {
                void* implBc = nullptr;
                std::uint32_t implSlot = 0;
                if (ResolveImplBcAndSlot(self, playerIndex, &implBc, &implSlot)
                    && implSlot < 32)
                {
                    if (kHideAvatarCreatorFace && t_ActiveCustomFaceSlot != 0)
                    {
                        g_AvatarHideBc.store(implBc, std::memory_order_relaxed);
                        g_AvatarHideSlotMask.fetch_or(1u << implSlot,
                                                      std::memory_order_relaxed);
                    }
                    else
                    {
                        g_AvatarHideSlotMask.fetch_and(~(1u << implSlot),
                                                       std::memory_order_relaxed);
                    }
                }
            }
        }

        const bool             spoofPartsType = isCustom && entry;
        const std::uint8_t     origPartsType  = info->playerPartsType;
        std::uint8_t*          shellTypeInfoPtr     = nullptr;
        std::uint8_t           prevShellPartsType   = 0;
        bool                   shellSentinelWritten = false;

        if (spoofPartsType)
        {
            outfit::shadow::SetCurrentSlot(playerIndex);

            std::uint8_t spoofTarget = 0x00;
            if (info->playerType == outfit::kPlayerType_Snake) spoofTarget = 0x01;
            info->playerPartsType = spoofTarget;

            __try
            {
                shellTypeInfoPtr = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(self)
                    + playerIndex * 8 + 0x1100);
                if (shellTypeInfoPtr)
                {
                    prevShellPartsType   = shellTypeInfoPtr[1];
                    shellTypeInfoPtr[1]  = 0xFE;
                    shellSentinelWritten = true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { shellTypeInfoPtr = nullptr; }
        }

        g_OrigLoadPartsNew(self, playerIndex, info, flags);

        t_ActiveCustomFaceSlot = 0;

        if (spoofPartsType)
        {
            info->playerPartsType = origPartsType;
            outfit::shadow::ClearCurrentSlot();
            __try
            {
                if (shellTypeInfoPtr) shellTypeInfoPtr[1] = origPartsType;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            (void)prevShellPartsType;
            (void)shellSentinelWritten;
        }

    }

}

namespace outfit
{
    std::uint8_t ResolveVanillaPartsTypeForCamo(std::uint8_t camoType)
    {
        if (!g_OrigGetPartsTypeAtCamoType) return 0xFF;
        if (camoType > kVanillaCamoTypeMax) return 0xFF;
        const std::uint32_t pt = g_OrigGetPartsTypeAtCamoType(nullptr, camoType);
        if (pt >= kCustomPartsTypeStart) return 0xFF;
        return static_cast<std::uint8_t>(pt);
    }

    static bool FacialBindingDereferenceable(void* self) noexcept
    {
        __try
        {
            const std::uint8_t* binding =
                *reinterpret_cast<std::uint8_t* const*>(
                    reinterpret_cast<const std::uint8_t*>(self) + 0x10);
            const volatile std::uint64_t probe =
                *reinterpret_cast<const volatile std::uint64_t*>(binding + 0xa0 + 0x50);
            (void)probe;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void __fastcall hkPluginFacialApplyMotion(void* self, void* a2, void* a3, float a4)
    {
        if (self && !FacialBindingDereferenceable(self))
        {
            static std::atomic<std::uint32_t> s_skips{ 0 };
            const std::uint32_t n = s_skips.fetch_add(1) + 1;
            if (n <= 8 || (n % 256) == 0)
                LogDebug("[OutfitFacialGuard] skipped facial apply with a wild AnimControl "
                    "binding (self=%p skip#%u) - prevented SetMotionDataCore AV from a "
                    "custom-outfit identity mismatch; face left unchanged (never-brick).\n",
                    self, static_cast<unsigned>(n));
            return;
        }
        if (g_OrigPluginFacialApplyMotion)
            g_OrigPluginFacialApplyMotion(self, a2, a3, a4);
    }

    bool Install_OutfitRuntimeParts_Hooks()
    {
        ResolveFoxPathApi();

        struct H { void* tgt; void* hk; void** orig; bool* installed; };
        H hooks[] = {
            { ResolveGameAddress(gAddr.LoadPlayerPartsParts),
              reinterpret_cast<void*>(&hkLoadPlayerPartsParts),
              reinterpret_cast<void**>(&g_OrigLoadPartsParts), &g_InstalledParts },
            { ResolveGameAddress(gAddr.LoadPlayerPartsFpk),
              reinterpret_cast<void*>(&hkLoadPlayerPartsFpk),
              reinterpret_cast<void**>(&g_OrigLoadPartsFpk), &g_InstalledFpk },
            { ResolveGameAddress(gAddr.LoadPlayerCamoFpk),
              reinterpret_cast<void*>(&hkLoadPlayerCamoFpk),
              reinterpret_cast<void**>(&g_OrigLoadCamoFpk), &g_InstalledCamo },
            { ResolveGameAddress(gAddr.LoadPlayerSnakeBlackDiamondFpk),
              reinterpret_cast<void*>(&hkLoadPlayerSnakeBlackDiamondFpk),
              reinterpret_cast<void**>(&g_OrigLoadDiamondFpk), &g_InstalledDiamond },
            { ResolveGameAddress(gAddr.LoadPlayerCamoFv2),
              reinterpret_cast<void*>(&hkLoadPlayerCamoFv2),
              reinterpret_cast<void**>(&g_OrigLoadCamoFv2), &g_InstalledCamoFv2 },
            { ResolveGameAddress(gAddr.LoadPlayerSnakeBlackDiamondFv2),
              reinterpret_cast<void*>(&hkLoadPlayerSnakeBlackDiamondFv2),
              reinterpret_cast<void**>(&g_OrigLoadDiamondFv2), &g_InstalledDiamondFv2 },
            { ResolveGameAddress(gAddr.LoadPlayerBionicArmFv2),
              reinterpret_cast<void*>(&hkLoadPlayerBionicArmFv2),
              reinterpret_cast<void**>(&g_OrigLoadBionicArmFv2), &g_InstalledBionicArmFv2 },
            { ResolveGameAddress(gAddr.LoadPlayerBionicArmFpk),
              reinterpret_cast<void*>(&hkLoadPlayerBionicArmFpk),
              reinterpret_cast<void**>(&g_OrigLoadBionicArmFpk), &g_InstalledBionicArmFpk },
            { ResolveGameAddress(gAddr.LoadPlayerSnakeFaceFv2),
              reinterpret_cast<void*>(&hkLoadPlayerSnakeFaceFv2),
              reinterpret_cast<void**>(&g_OrigLoadSnakeFaceFv2), &g_InstalledSnakeFaceFv2 },
            { ResolveGameAddress(gAddr.LoadPlayerSnakeFaceFpk),
              reinterpret_cast<void*>(&hkLoadPlayerSnakeFaceFpk),
              reinterpret_cast<void**>(&g_OrigLoadSnakeFaceFpk), &g_InstalledSnakeFaceFpk },
            { ResolveGameAddress(gAddr.Player2BlockController_LoadPartsNew),
              reinterpret_cast<void*>(&hkLoadPartsNew),
              reinterpret_cast<void**>(&g_OrigLoadPartsNew), &g_InstalledLpn },
            { ResolveGameAddress(gAddr.ResourceTable_DoesNeedFaceFova),
              reinterpret_cast<void*>(&hkDoesNeedFaceFova),
              reinterpret_cast<void**>(&g_OrigDoesNeedFaceFova), &g_InstalledDoesNeedFace },
            { ResolveGameAddress(gAddr.ResourceTable_DoesNeedFaceFovaForAvatar),
              reinterpret_cast<void*>(&hkDoesNeedFaceFovaForAvatar),
              reinterpret_cast<void**>(&g_OrigDoesNeedFaceFovaForAvatar), &g_InstalledDoesNeedFaceForAvatar },
            { ResolveGameAddress(gAddr.EquipController_SetHandSlotEnabled),
              reinterpret_cast<void*>(&hkSetHandSlotEnabled),
              reinterpret_cast<void**>(&g_OrigSetHandSlotEnabled), &g_InstalledSetHandSlotEnabled },
            { ResolveGameAddress(gAddr.Sys_IsArtificialHandEnabled),
              reinterpret_cast<void*>(&hkIsArtificialHandEnabled),
              reinterpret_cast<void**>(&g_OrigIsArtificialHandEnabled), &g_InstalledIsArtificialHand },
            { ResolveGameAddress(gAddr.Sys_IsArtificialHandEnabledForCurrentPlayerType),
              reinterpret_cast<void*>(&hkIsArtificialHandEnabledForCurrentPlayerType),
              reinterpret_cast<void**>(&g_OrigIsArtificialHandForCurrent), &g_InstalledIsArtHandForCurrent },
            { ResolveGameAddress(gAddr.Player2GameObjectImpl_ProcessSignal),
              reinterpret_cast<void*>(&hkProcessSignal),
              reinterpret_cast<void**>(&g_OrigProcessSignal), &g_InstalledProcessSignal },
            { ResolveGameAddress(gAddr.UpdatePartsStatus),
              reinterpret_cast<void*>(&hkUpdatePartsStatus),
              reinterpret_cast<void**>(&g_OrigUpdatePartsStatus), &g_InstalledUpdatePartsStatus },
            { ResolveGameAddress(gAddr.Player2Impl_SetUpParts),
              reinterpret_cast<void*>(&hkPlayer2ImplSetUpParts),
              reinterpret_cast<void**>(&g_OrigPlayer2ImplSetUpParts), &g_InstalledPlayer2ImplSetUpParts },
            { ResolveGameAddress(gAddr.PluginFacial_ApplyMotion),
              reinterpret_cast<void*>(&hkPluginFacialApplyMotion),
              reinterpret_cast<void**>(&g_OrigPluginFacialApplyMotion), &g_InstalledFacialCrashGuard },
            { ResolveGameAddress(gAddr.PlayerInfoInterfaceImpl_GetPartsTypeAtCamoType),
              reinterpret_cast<void*>(&hkGetPartsTypeAtCamoType),
              reinterpret_cast<void**>(&g_OrigGetPartsTypeAtCamoType), &g_InstalledPartsAtCamo },
            { ResolveGameAddress(gAddr.LoadAvatarFaceFv2),
              reinterpret_cast<void*>(&hkLoadAvatarFaceFv2),
              reinterpret_cast<void**>(&g_OrigLoadAvatarFaceFv2), &g_InstalledAvatarFaceFv2 },
            { ResolveGameAddress(gAddr.LoadAvatarFaceFpk),
              reinterpret_cast<void*>(&hkLoadAvatarFaceFpk),
              reinterpret_cast<void**>(&g_OrigLoadAvatarFaceFpk), &g_InstalledAvatarFaceFpk },
            { ResolveGameAddress(gAddr.AvatarFaceEditUpdate),
              reinterpret_cast<void*>(&hkAvatarFaceEditUpdate),
              reinterpret_cast<void**>(&g_OrigAvatarFaceEditUpdate), &g_InstalledAvatarFaceEdit },
            { ResolveGameAddress(gAddr.LoadAvatarHeadOptionFv2),
              reinterpret_cast<void*>(&hkLoadAvatarHeadOptionFv2),
              reinterpret_cast<void**>(&g_OrigLoadAvatarHeadOptionFv2), &g_InstalledAvatarHeadOptionFv2 },
            { ResolveGameAddress(gAddr.LoadAvatarHeadOptionFpk),
              reinterpret_cast<void*>(&hkLoadAvatarHeadOptionFpk),
              reinterpret_cast<void**>(&g_OrigLoadAvatarHeadOptionFpk), &g_InstalledAvatarHeadOptionFpk },
        };
        for (auto& h : hooks)
        {
            if (h.tgt) *h.installed = CreateAndEnableHook(h.tgt, h.hk, h.orig);
        }

        g_FoxModelFromHandle = reinterpret_cast<FoxModelFromHandle_t>(
            ResolveGameAddress(gAddr.Fox_ModelFromHandle));

        if (g_InstalledUpdatePartsStatus)
            InstallCaseDArmUnpin();

#ifdef _DEBUG
        LogDebug("[OutfitRuntimeParts] installed: parts=%s fpk=%s camo=%s diamond=%s "
            "camoFv2=%s diamondFv2=%s "
            "bionicArmFv2=%s bionicArmFpk=%s snakeFaceFv2=%s snakeFaceFpk=%s "
            "avatarFaceFv2=%s avatarFaceFpk=%s avatarFaceEdit=%s "
            "avatarHeadOptFv2=%s avatarHeadOptFpk=%s "
            "lpn=%s doesNeedFace=%s doesNeedFaceAvatar=%s setHandSlotEnabled=%s "
            "isArtificialHandEnabled=%s isArtHandForCurrent=%s processSignal=%s "
            "updatePartsStatus=%s setUpParts=%s facialCrashGuard=%s "
            "supplyCamoParts=%s\n",
            g_InstalledParts                 ? "OK" : "skip",
            g_InstalledFpk                   ? "OK" : "skip",
            g_InstalledCamo                  ? "OK" : "skip",
            g_InstalledDiamond               ? "OK" : "skip",
            g_InstalledCamoFv2               ? "OK" : "skip",
            g_InstalledDiamondFv2            ? "OK" : "skip",
            g_InstalledBionicArmFv2          ? "OK" : "skip",
            g_InstalledBionicArmFpk          ? "OK" : "skip",
            g_InstalledSnakeFaceFv2          ? "OK" : "skip",
            g_InstalledSnakeFaceFpk          ? "OK" : "skip",
            g_InstalledAvatarFaceFv2         ? "OK" : "skip",
            g_InstalledAvatarFaceFpk         ? "OK" : "skip",
            g_InstalledAvatarFaceEdit        ? "OK" : "skip",
            g_InstalledAvatarHeadOptionFv2   ? "OK" : "skip",
            g_InstalledAvatarHeadOptionFpk   ? "OK" : "skip",
            g_InstalledLpn                   ? "OK" : "skip",
            g_InstalledDoesNeedFace          ? "OK" : "skip",
            g_InstalledDoesNeedFaceForAvatar ? "OK" : "skip",
            g_InstalledSetHandSlotEnabled    ? "OK" : "skip",
            g_InstalledIsArtificialHand      ? "OK" : "skip",
            g_InstalledIsArtHandForCurrent   ? "OK" : "skip",
            g_InstalledProcessSignal         ? "OK" : "skip",
            g_InstalledUpdatePartsStatus     ? "OK" : "skip",
            g_InstalledPlayer2ImplSetUpParts ? "OK" : "skip",
            g_InstalledFacialCrashGuard      ? "OK" : "skip",
            g_InstalledPartsAtCamo           ? "OK" : "skip");
#endif

        return g_InstalledParts || g_InstalledFpk || g_InstalledCamo
            || g_InstalledDiamond || g_InstalledBionicArmFv2 || g_InstalledBionicArmFpk
            || g_InstalledSnakeFaceFv2 || g_InstalledSnakeFaceFpk || g_InstalledLpn;
    }

    void Uninstall_OutfitRuntimeParts_Hooks()
    {
        struct U { bool* installed; void* tgt; };
        U hooks[] = {
            { &g_InstalledParts,                 ResolveGameAddress(gAddr.LoadPlayerPartsParts) },
            { &g_InstalledFpk,                   ResolveGameAddress(gAddr.LoadPlayerPartsFpk) },
            { &g_InstalledCamo,                  ResolveGameAddress(gAddr.LoadPlayerCamoFpk) },
            { &g_InstalledDiamond,               ResolveGameAddress(gAddr.LoadPlayerSnakeBlackDiamondFpk) },
            { &g_InstalledCamoFv2,               ResolveGameAddress(gAddr.LoadPlayerCamoFv2) },
            { &g_InstalledDiamondFv2,            ResolveGameAddress(gAddr.LoadPlayerSnakeBlackDiamondFv2) },
            { &g_InstalledBionicArmFv2,          ResolveGameAddress(gAddr.LoadPlayerBionicArmFv2) },
            { &g_InstalledBionicArmFpk,          ResolveGameAddress(gAddr.LoadPlayerBionicArmFpk) },
            { &g_InstalledSnakeFaceFv2,          ResolveGameAddress(gAddr.LoadPlayerSnakeFaceFv2) },
            { &g_InstalledSnakeFaceFpk,          ResolveGameAddress(gAddr.LoadPlayerSnakeFaceFpk) },
            { &g_InstalledAvatarFaceFv2,         ResolveGameAddress(gAddr.LoadAvatarFaceFv2) },
            { &g_InstalledAvatarFaceFpk,         ResolveGameAddress(gAddr.LoadAvatarFaceFpk) },
            { &g_InstalledAvatarFaceEdit,        ResolveGameAddress(gAddr.AvatarFaceEditUpdate) },
            { &g_InstalledAvatarHeadOptionFv2,   ResolveGameAddress(gAddr.LoadAvatarHeadOptionFv2) },
            { &g_InstalledAvatarHeadOptionFpk,   ResolveGameAddress(gAddr.LoadAvatarHeadOptionFpk) },
            { &g_InstalledLpn,                   ResolveGameAddress(gAddr.Player2BlockController_LoadPartsNew) },
            { &g_InstalledDoesNeedFace,          ResolveGameAddress(gAddr.ResourceTable_DoesNeedFaceFova) },
            { &g_InstalledDoesNeedFaceForAvatar, ResolveGameAddress(gAddr.ResourceTable_DoesNeedFaceFovaForAvatar) },
            { &g_InstalledSetHandSlotEnabled,    ResolveGameAddress(gAddr.EquipController_SetHandSlotEnabled) },
            { &g_InstalledIsArtificialHand,      ResolveGameAddress(gAddr.Sys_IsArtificialHandEnabled) },
            { &g_InstalledIsArtHandForCurrent,   ResolveGameAddress(gAddr.Sys_IsArtificialHandEnabledForCurrentPlayerType) },
            { &g_InstalledProcessSignal,         ResolveGameAddress(gAddr.Player2GameObjectImpl_ProcessSignal) },
            { &g_InstalledUpdatePartsStatus,     ResolveGameAddress(gAddr.UpdatePartsStatus) },
            { &g_InstalledPlayer2ImplSetUpParts, ResolveGameAddress(gAddr.Player2Impl_SetUpParts) },
            { &g_InstalledFacialCrashGuard,      ResolveGameAddress(gAddr.PluginFacial_ApplyMotion) },
            { &g_InstalledPartsAtCamo,           ResolveGameAddress(gAddr.PlayerInfoInterfaceImpl_GetPartsTypeAtCamoType) },
        };
        for (auto& h : hooks)
        {
            if (*h.installed && h.tgt) DisableAndRemoveHook(h.tgt);
            *h.installed = false;
        }

        g_OrigLoadPartsParts            = nullptr;
        g_OrigLoadPartsFpk              = nullptr;
        g_OrigLoadCamoFpk               = nullptr;
        g_OrigLoadDiamondFpk            = nullptr;
        g_OrigLoadCamoFv2              = nullptr;
        g_OrigLoadDiamondFv2           = nullptr;
        g_OrigLoadBionicArmFv2          = nullptr;
        g_OrigLoadBionicArmFpk          = nullptr;
        g_OrigLoadSnakeFaceFv2          = nullptr;
        g_OrigLoadSnakeFaceFpk          = nullptr;
        g_OrigLoadAvatarFaceFv2         = nullptr;
        g_OrigLoadAvatarFaceFpk         = nullptr;
        g_OrigLoadAvatarHeadOptionFv2   = nullptr;
        g_OrigLoadAvatarHeadOptionFpk   = nullptr;
        g_OrigAvatarFaceEditUpdate      = nullptr;
        g_FoxModelFromHandle            = nullptr;
        g_OrigLoadPartsNew              = nullptr;
        g_OrigDoesNeedFaceFova          = nullptr;
        g_OrigDoesNeedFaceFovaForAvatar = nullptr;
        g_OrigSetHandSlotEnabled        = nullptr;
        g_OrigIsArtificialHandEnabled   = nullptr;
        g_OrigIsArtificialHandForCurrent = nullptr;
        g_OrigProcessSignal             = nullptr;
        g_OrigUpdatePartsStatus         = nullptr;
        g_OrigPlayer2ImplSetUpParts     = nullptr;
        g_OrigPluginFacialApplyMotion   = nullptr;
        g_OrigGetPartsTypeAtCamoType    = nullptr;
        g_FoxPath_Path                  = nullptr;
        g_CapturedBlockController       = nullptr;

        outfit::shadow::ResetAll("Uninstall_OutfitRuntimeParts_Hooks");
        outfit::shadow::ResetArmTierCache();

#ifdef _DEBUG
        LogDebug("[OutfitRuntimeParts] removed\n");
#endif
    }
}
