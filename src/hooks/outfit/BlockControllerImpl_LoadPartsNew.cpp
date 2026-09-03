#include "pch.h"

#include "BlockControllerImpl_LoadPartsNew.h"
#include "OutfitRegistry.h"
#include "CustomHeadRegistry.h"
#include "Player2UtilityImpl_SetSuitAndHandConditionWithLoadoutInfo.h"
#include "ShadowState.h"
#include "FoxPathInternal.h"
#include "MissionCodeGuard.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"
#include "../equip/EquipDevelop_SetEquipUndeveloped.h"
#include "../player/FobPlayerCharacters.h"
#include "UniqueCharacterPartsTypePin.h"
#include "AdditionalMotionTable_GetMtarPathId.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <intrin.h>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "../player/PlayerAttachInDemo.h"

static inline bool V_WriteOutfitPartsLoad(std::uint8_t p, std::uint8_t c,
                                  std::uint8_t pt)
{
    return outfit::WriteLivePlayerOutfit(
        p, c, pt, outfit::OutfitWriteSource::PartsLoad);
}

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
    constexpr std::size_t   kP2GO_OffSlotCount             = 0x228;
    constexpr std::size_t   kP2GO_OffLocalPlayerSlot       = 0x234;
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
    static std::atomic<std::uint32_t> g_LocalPartsSlot{ 0 };

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
        LogDebug("[CaseDArmUnpin] installed: site=%p tramp=%p armTable=%p "
                 "band=0x%02X-0x%02X (%zu slots) - arm-enabled custom partsType "
                 "decodes the real tier; enableArm=false takes the engine's armless "
                 "path\n",
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

    static std::uint32_t EffectivePartsTypeFor(std::uint32_t playerType,
                                               std::uint32_t paramPartsType)
    {
        if (!outfit::shadow::HasCurrentSlot()) return paramPartsType;

        outfit::shadow::Slot s;
        if (!outfit::shadow::Get(outfit::shadow::GetCurrentSlot(), &s))
            return paramPartsType;

        if (s.used
         && s.realPlayerType != static_cast<std::uint8_t>(playerType & 0xFF))
        {
            static std::atomic<int> s_mismatch{ 0 };
            if (s_mismatch.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("[OutfitRuntimeParts] the thread-local parts slot holds player "
                    "type %u but the engine asked for %u - kept the engine's parts "
                    "type rather than substituting another character's suit\n",
                    static_cast<unsigned>(s.realPlayerType),
                    static_cast<unsigned>(playerType & 0xFF));
            return paramPartsType;
        }
        return s.realPartsType;
    }

    constexpr std::uint32_t kCamoDonorVanillaPartsType = 0x17;

    static bool UsesSnakeCamoDonor(std::uint32_t playerType,
                                   const outfit::OutfitEntry* entry)
    {
        if (!entry) return false;
        return outfit::IsUniqueCharacterPlayerType(
            static_cast<std::uint8_t>(playerType & 0xFF));
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

        if (!entry->DeclaresPlayerType(ply))
        {
            if (!outfit::DeclaresBranchInPartsGroupOf(*entry, ply))
            {
                static std::atomic<int> refused{ 0 };
                if (refused.fetch_add(1, std::memory_order_relaxed) < 8)
                    Log("[OutfitRuntimeParts] outfit developId=%u partsType="
                        "0x%02X declares no branch in the parts group player "
                        "type %u loads from, so its assets would be served "
                        "against a different skeleton - reverting this slot to "
                        "vanilla\n",
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(entry->partsType),
                        static_cast<unsigned>(ply));
                return false;
            }

            static std::atomic<int> logged{ 0 };
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                LogDebug("[OutfitRuntimeParts] outfit developId=%u partsType=0x%02X "
                         "declares no branch for live player type %u - serving its "
                         "first declared branch instead of reverting to vanilla\n",
                    static_cast<unsigned>(entry->developId),
                    static_cast<unsigned>(entry->partsType),
                    static_cast<unsigned>(ply));
        }

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

    static std::atomic<std::uint8_t> g_VextServedPt[outfit::kPlayerTypeMax];
    static std::atomic<std::uint8_t> g_VextServedVar[outfit::kPlayerTypeMax];
    static std::atomic<std::uint8_t> g_UniqueServedParts[outfit::shadow::kMaxSlots] = {
        std::atomic<std::uint8_t>{0xFF}, std::atomic<std::uint8_t>{0xFF},
        std::atomic<std::uint8_t>{0xFF}, std::atomic<std::uint8_t>{0xFF}
    };

    constexpr int kRestreamMaxDeferTicks = 120;
    static std::atomic<int> g_RestreamPendingTicks{ 0 };

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


    std::atomic<unsigned> g_PartsPathCalls{ 0 };
    std::atomic<unsigned> g_PartsPathLastPt{ 0xFFu };
    std::atomic<unsigned> g_PartsPathLastParts{ 0xFFu };

    static std::uint64_t* __fastcall hkLoadPlayerPartsParts(
        std::uint64_t* outPath, std::uint32_t playerType, std::uint32_t playerPartsType)
    {
        const std::uint32_t effectivePartsType =
            EffectivePartsTypeFor(playerType, playerPartsType);

        g_PartsPathCalls.fetch_add(1, std::memory_order_relaxed);
        g_PartsPathLastPt.store(playerType & 0xFFu, std::memory_order_relaxed);
        g_PartsPathLastParts.store(playerPartsType & 0xFFu, std::memory_order_relaxed);

        const outfit::OutfitEntry* entry = nullptr;
        const bool uniqueResolved =
            ResolveCustomEntry(playerType, effectivePartsType, &entry);

        if (outfit::IsUniqueCharacterPlayerType(
                static_cast<std::uint8_t>(playerType & 0xFF)))
        {
            static std::atomic<int> s_uniqParts{ 0 };
            if (int n = s_uniqParts.load(std::memory_order_relaxed); n < 24)
            {
                s_uniqParts.store(n + 1, std::memory_order_relaxed);
                const std::uint64_t probePath =
                    (uniqueResolved && entry)
                        ? entry->GetVariantPartsPath(
                              static_cast<std::uint8_t>(playerType & 0xFF),
                              entry->HasVariants()
                                  ? outfit::GetActiveVariant(entry->partsType) : 0)
                        : 0ull;
                LogDebug("[OutfitRuntimeParts:uniq] PARTS pt=%u partsType=0x%02X "
                    "effective=0x%02X resolved=%d path=0x%016llX exists=%d\n",
                    static_cast<unsigned>(playerType),
                    static_cast<unsigned>(playerPartsType & 0xFF),
                    static_cast<unsigned>(effectivePartsType & 0xFF),
                    uniqueResolved ? 1 : 0,
                    static_cast<unsigned long long>(probePath),
                    probePath ? (fox::detail::PathExistsByCode(probePath) ? 1 : 0) : -1);
            }
        }

        if (uniqueResolved)
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
        const std::uint32_t effectivePartsType =
            EffectivePartsTypeFor(playerType, playerPartsType);

        const outfit::OutfitEntry* entry = nullptr;
        const bool uniqueFpkResolved =
            ResolveCustomEntry(playerType, effectivePartsType, &entry);

        if (outfit::IsUniqueCharacterPlayerType(
                static_cast<std::uint8_t>(playerType & 0xFF)))
        {
            static std::atomic<int> s_uniqFpk{ 0 };
            if (int n = s_uniqFpk.load(std::memory_order_relaxed); n < 24)
            {
                s_uniqFpk.store(n + 1, std::memory_order_relaxed);
                const std::uint64_t probeFpk =
                    (uniqueFpkResolved && entry)
                        ? entry->GetVariantFpkPath(
                              static_cast<std::uint8_t>(playerType & 0xFF),
                              entry->HasVariants()
                                  ? outfit::GetActiveVariant(entry->partsType) : 0)
                        : 0ull;
                Log("[OutfitRuntimeParts:uniq] FPK pt=%u partsType=0x%02X "
                    "effective=0x%02X resolved=%d path=0x%016llX exists=%d\n",
                    static_cast<unsigned>(playerType),
                    static_cast<unsigned>(playerPartsType & 0xFF),
                    static_cast<unsigned>(effectivePartsType & 0xFF),
                    uniqueFpkResolved ? 1 : 0,
                    static_cast<unsigned long long>(probeFpk),
                    probeFpk ? (fox::detail::PathExistsByCode(probeFpk) ? 1 : 0) : -1);
            }
        }

        if (uniqueFpkResolved)
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
        if (effectivePartsType < outfit::kCustomPartsTypeStart
            && playerType < outfit::kPlayerTypeMax)
        {
            const auto servedIdx = static_cast<std::size_t>(playerType);
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
            LogDebug("[OutfitRuntimeParts] CAMO-CLAMP: out-of-table camo 0x%02X on "
                     "a vanilla realize - clamped to 0 (the vanilla camo path table "
                     "has 0x75 entries and is unbounded in the engine)\n",
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
        if (UsesSnakeCamoDonor(playerType, entry))
            return g_OrigLoadCamoFpk(outPath, outfit::kPlayerType_Snake,
                                     kCamoDonorVanillaPartsType,
                                     ClampVanillaCamo(playerCamoType));
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
        if (UsesSnakeCamoDonor(playerType, entry))
        {
            static std::atomic<int> s_log{ 0 };
            if (int n = s_log.load(std::memory_order_relaxed); n < 4)
            {
                s_log.store(n + 1, std::memory_order_relaxed);
                LogDebug("[OutfitRuntimeParts] CAMO-DONOR: player type %u has no "
                         "vanilla camo table of its own, so its custom suit takes "
                         "Snake's camo set for camo 0x%02X - declare camoFv2 on the "
                         "branch to override\n",
                    static_cast<unsigned>(playerType & 0xFF),
                    static_cast<unsigned>(playerCamoType & 0xFF));
            }
            return g_OrigLoadCamoFv2(outPath, outfit::kPlayerType_Snake,
                                     kCamoDonorVanillaPartsType,
                                     ClampVanillaCamo(playerCamoType));
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
                    LogDebug("[OutfitHeadOption] custom head '%s' face .fv2 is in "
                             "no mounted archive (code=0x%016llX pt=%u "
                             "partsType=0x%02X) - the face would render invisible "
                             "and the null FOVA slot faults the next parts apply; "
                             "falling back to the vanilla face\n",
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
                    LogDebug("[OutfitHeadOption] custom head '%s' face .fpk is in "
                             "no mounted archive (code=0x%016llX pt=%u "
                             "partsType=0x%02X) - the face renders invisible even "
                             "when its .fv2 resolves; falling back to the vanilla "
                             "face\n",
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
                LogDebug("[SnakeHead] HeadOptionFv2: avatar head '%s' replaces the "
                         "headwear fova (faceId=%u fv2Code=0x%016llX)\n",
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
                LogDebug("[SnakeHead] HeadOptionFpk: avatar head '%s' mounted in "
                         "the head-option slot (fpkCode=0x%016llX)\n",
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
            Note_PlayerGameObjectImpl(p1);
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
                        partsTypeArr[slot] = kProcessSignalSpoofPartsType;
                        outfit::shadow::SetCurrentSlot(slot);
                        spoofWritten = true;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { partsTypeArr = nullptr; }

        __try
        {
            Note_PlayerGameObjectImpl(p1);
            if (g_OrigProcessSignal) g_OrigProcessSignal(p1, p2, slot, signalPtr);
        }
        __finally
        {
            if (spoofWritten) outfit::shadow::ClearCurrentSlot();
        }

        if (spoofWritten)
        {
            __try { partsTypeArr[slot] = origByte; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            outfit::shadow::ClearCurrentSlot();
        }
    }


    constexpr int kPartsSettleTicks = 45;
    constexpr int kPartsStallTicks  = 900;
    constexpr int kFacialUnstickTicks = 60;

    struct PartsPipeline
    {
        bool          valid = false;
        bool          settled = false;
        bool          busy  = false;
        std::uint32_t slot  = 0;
        std::uint32_t count = 0;
        std::uint8_t  state[8] = {};
    };

    static void ReadPartsPipeline(void* self, PartsPipeline* out)
    {
        __try
        {
            std::uint8_t* base = reinterpret_cast<std::uint8_t*>(self);
            const std::uint8_t* stateArr =
                *reinterpret_cast<std::uint8_t**>(base + kP2GO_OffStateMachinePtr);
            const std::uint32_t count =
                *reinterpret_cast<std::uint32_t*>(base + kP2GO_OffSlotCount);
            const std::uint32_t slot =
                *reinterpret_cast<std::uint32_t*>(base + kP2GO_OffLocalPlayerSlot);
            if (!stateArr || count > 64u || slot >= count) return;

            out->valid = true;
            out->slot  = slot;
            if (slot < outfit::shadow::kMaxSlots)
                g_LocalPartsSlot.store(slot, std::memory_order_relaxed);
            out->count = count;
            for (std::uint32_t i = 0; i < count && i < 8u; ++i)
                out->state[i] = stateArr[i];
            const std::uint8_t st = stateArr[slot];
            out->busy = (st == 1u || st == 2u);
            out->settled = (st == 3u);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out->valid = false; out->busy = false; out->settled = false;
        }
    }

    static void TracePartsState(const PartsPipeline& pp)
    {
        static std::uint8_t s_last[8] = { 0xFEu, 0xFEu, 0xFEu, 0xFEu,
                                          0xFEu, 0xFEu, 0xFEu, 0xFEu };
        static std::atomic<int> s_lines{ 0 };
        static std::atomic<bool> s_wasValid{ false };

        if (!pp.valid)
        {
            if (s_wasValid.exchange(false)
             && s_lines.fetch_add(1, std::memory_order_relaxed) < 128)
                Log("[PartsState] the parts pipeline became unreadable - every "
                    "transition after this is invisible until it comes back\n");
            return;
        }
        s_wasValid.store(true, std::memory_order_relaxed);

        for (std::uint32_t i = 0; i < pp.count && i < 8u; ++i)
        {
            if (s_last[i] == pp.state[i]) continue;
            const std::uint8_t prev = s_last[i];
            s_last[i] = pp.state[i];

            if (s_lines.fetch_add(1, std::memory_order_relaxed) >= 128) return;

            char all[64];
            int n = 0;
            for (std::uint32_t j = 0; j < pp.count && j < 8u
                                   && n + 4 < static_cast<int>(sizeof all); ++j)
                n += std::snprintf(all + n, sizeof all - n, "%u%s",
                                   static_cast<unsigned>(pp.state[j]),
                                   (j + 1 < pp.count && j + 1 < 8u) ? "," : "");
            all[n < 0 ? 0 : n] = 0;

            Log("[PartsState] slot %u: %u -> %u %s| all=[%s] local=%u/%u "
                "pt=%u parts=0x%02X camo=0x%02X\n",
                i, static_cast<unsigned>(prev),
                static_cast<unsigned>(pp.state[i]),
                (i == pp.slot) ? "(LOCAL) " : "",
                all, pp.slot, pp.count,
                static_cast<unsigned>(outfit::ReadLivePlayerType()),
                static_cast<unsigned>(outfit::ReadLivePartsType()),
                static_cast<unsigned>(outfit::ReadLiveSelectorCode()));
        }
    }

    std::atomic<int> g_PushLogged{ 0 };

    constexpr std::size_t kP2BC_OffImpl         = 0x10;
    constexpr std::size_t kP2BC_OffLocalIndex   = 0x18;
    constexpr std::size_t kBCI_OffResidentCount = 0x168;
    constexpr std::size_t kBCI_OffStreamCount   = 0x16c;
    constexpr std::size_t kBCI_OffRequestState  = 0x17c;
    constexpr std::size_t kBCI_OffBlockState    = 0x1bc;
    constexpr std::size_t kBCI_OffJobArm        = 0x1080;
    constexpr std::size_t kBCI_OffJobState      = 0x10c0;
    constexpr std::size_t kBCI_OffFacialGroup   = 0x160;
    constexpr std::size_t kBCI_OffFacialReq     = 0x214;
    constexpr std::size_t kBCI_OffFacialState   = 0x218;
    constexpr std::size_t kBCI_OffFacialPath    = 0x220;
    constexpr std::uint32_t kBCI_BlockArrayLen  = 16u;

    constexpr int kPartsGroupsHoldMaxTicks = 240;

    struct PartsBlockView
    {
        bool          read     = false;
        std::uint32_t resident = 0;
        std::uint32_t streamed = 0;
        std::uint32_t implSlot = 0;
        std::int32_t  req      = -1;
        std::int32_t  req4[4]  = { -1, -1, -1, -1 };
        std::int32_t  blockState  = -1;
        std::int32_t  jobArm      = -1;
        std::int32_t  jobState    = -1;
        bool          facialGroup = false;
        std::int32_t  facialReq   = -1;
        std::int32_t  facialState = -1;
        std::uint64_t facialPath  = 0;
    };

    static void ReadPartsBlockView(std::uint32_t playerSlot, PartsBlockView* out)
    {
        void* wrapper = g_CapturedBlockController;
        if (!wrapper) return;
        __try
        {
            auto* w = reinterpret_cast<std::uint8_t*>(wrapper);
            auto* impl = *reinterpret_cast<std::uint8_t**>(w + kP2BC_OffImpl);
            if (!impl) return;

            const std::uint32_t localIndex =
                *reinterpret_cast<std::uint32_t*>(w + kP2BC_OffLocalIndex);

            std::uint32_t block = playerSlot;
            if (playerSlot == localIndex)     block = 0u;
            else if (playerSlot < localIndex) block = playerSlot + 1u;

            out->implSlot = block;
            out->resident =
                *reinterpret_cast<std::uint32_t*>(impl + kBCI_OffResidentCount);
            out->streamed =
                *reinterpret_cast<std::uint32_t*>(impl + kBCI_OffStreamCount);

            for (int i = 0; i < 4; ++i)
                out->req4[i] = *reinterpret_cast<std::int32_t*>(
                    impl + kBCI_OffRequestState + static_cast<std::size_t>(i) * 4u);

            if (block < 4u) out->req = out->req4[block];

            if (block < kBCI_BlockArrayLen)
            {
                const std::size_t off = static_cast<std::size_t>(block) * 4u;
                out->blockState = *reinterpret_cast<std::int32_t*>(
                    impl + kBCI_OffBlockState + off);
                out->jobArm = *reinterpret_cast<std::int32_t*>(
                    impl + kBCI_OffJobArm + off);
                out->jobState = *reinterpret_cast<std::int32_t*>(
                    impl + kBCI_OffJobState + off);
            }

            out->facialGroup =
                *reinterpret_cast<void**>(impl + kBCI_OffFacialGroup) != nullptr;
            out->facialReq =
                *reinterpret_cast<std::int32_t*>(impl + kBCI_OffFacialReq);
            out->facialState =
                *reinterpret_cast<std::int32_t*>(impl + kBCI_OffFacialState);
            out->facialPath =
                *reinterpret_cast<std::uint64_t*>(impl + kBCI_OffFacialPath);

            out->read = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { out->read = false; }
    }

    static bool ReleaseAdditionalFacialBlock()
    {
        void* wrapper = g_CapturedBlockController;
        if (!wrapper) return false;
        __try
        {
            auto* w = reinterpret_cast<std::uint8_t*>(wrapper);
            auto* impl = *reinterpret_cast<std::uint8_t**>(w + kP2BC_OffImpl);
            if (!impl) return false;
            if (!*reinterpret_cast<void**>(impl + kBCI_OffFacialGroup)) return false;
            *reinterpret_cast<std::int32_t*>(impl + kBCI_OffFacialReq) = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool PartsGroupsLive(const PartsBlockView& v)
    {
        if (!v.read) return true;
        if (v.implSlot >= v.resident + v.streamed) return false;
        return v.req != 0;
    }

    static bool ReadEngineIdentitySeh(void* self, std::uint32_t slot,
                                      std::uint8_t* outPt, std::uint8_t* outParts,
                                      std::uint8_t* outCamo)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(self);
            void* perPlayer =
                *reinterpret_cast<void**>(base + kP2GO_OffPerPlayerStruct);
            if (!perPlayer)
                return false;
            auto* pps = reinterpret_cast<std::uint8_t*>(perPlayer);

            auto* ptArr =
                *reinterpret_cast<std::uint8_t**>(pps + kPP_OffPlayerTypeArr);
            auto* partsArr =
                *reinterpret_cast<std::uint8_t**>(pps + kPP_OffPartsTypeArr);
            auto* camoArr =
                *reinterpret_cast<std::uint8_t**>(pps + kPP_OffCamoTypeArr);
            if (!ptArr || !partsArr || !camoArr)
                return false;

            *outPt    = ptArr[slot];
            *outParts = partsArr[slot];
            *outCamo  = camoArr[slot];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void PushIdentityToEngineArrays(void* self, const PartsPipeline& pp)
    {
        if (!pp.valid) return;

        const std::uint8_t livePT    = outfit::ReadLivePlayerType();
        const std::uint8_t liveParts = outfit::ReadLivePartsType();
        const std::uint8_t liveCamo  = outfit::ReadLiveSelectorCode();

        if (!outfit::IsUniqueCharacterPlayerType(livePT)) return;
        if (liveParts < outfit::kCustomPartsTypeStart
         || liveParts > outfit::kCustomPartsTypeEnd) return;
        if (liveCamo == 0) return;

        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(self);
            void* perPlayer =
                *reinterpret_cast<void**>(base + kP2GO_OffPerPlayerStruct);
            if (!perPlayer) return;
            auto* pps = reinterpret_cast<std::uint8_t*>(perPlayer);

            auto* ptArr =
                *reinterpret_cast<std::uint8_t**>(pps + kPP_OffPlayerTypeArr);
            auto* partsArr =
                *reinterpret_cast<std::uint8_t**>(pps + kPP_OffPartsTypeArr);
            auto* camoArr =
                *reinterpret_cast<std::uint8_t**>(pps + kPP_OffCamoTypeArr);
            if (!ptArr || !partsArr || !camoArr) return;

            std::uint32_t syncCount = pp.count;
            if (syncCount == 0 || syncCount > outfit::shadow::kMaxSlots)
                syncCount = 1;
            if (pp.slot < outfit::shadow::kMaxSlots && syncCount <= pp.slot)
                syncCount = pp.slot + 1;

            if (pp.slot >= outfit::shadow::kMaxSlots)
            {
                static std::atomic<int> s_slotOob{ 0 };
                if (s_slotOob.fetch_add(1, std::memory_order_relaxed) < 8)
                    Log("[OutfitRuntimeParts] the parts pipeline reports slot %u, "
                        "past the %zu-slot mirror - the engine identity arrays are "
                        "left alone for it, so that slot keeps whatever suit it "
                        "already had\n",
                        pp.slot, outfit::shadow::kMaxSlots);
                return;
            }

            for (std::uint32_t i = 0; i < syncCount; ++i)
            {
                if (i != pp.slot)
                {
                    const bool sameCharacter = (ptArr[i] == livePT);
                    const bool otherUnique =
                        outfit::IsUniqueCharacterPlayerType(ptArr[i]);
                    const bool staleCustom =
                        partsArr[i] >= outfit::kCustomPartsTypeStart
                     && partsArr[i] <= outfit::kCustomPartsTypeEnd;
                    if (!sameCharacter && !otherUnique && !staleCustom)
                        continue;
                }

                if (ptArr[i] == livePT
                 && partsArr[i] == liveParts
                 && camoArr[i] == liveCamo) continue;

                if (g_PushLogged.fetch_add(1, std::memory_order_relaxed) < 32)
                    LogDebug("[OutfitRuntimeParts] engine identity arrays for slot %u "
                        "read parts=0x%02X camo=0x%02X playerType=%u but the live quark "
                        "holds parts=0x%02X camo=0x%02X playerType=%u - syncing every "
                        "slot of this pipeline, not just the current one: the re-stream "
                        "reloads them all, so a slot left on the previous suit "
                        "re-resolves that outfit and the two slots serve different "
                        "suits\n",
                        i,
                        static_cast<unsigned>(partsArr[i]),
                        static_cast<unsigned>(camoArr[i]),
                        static_cast<unsigned>(ptArr[i]),
                        static_cast<unsigned>(liveParts),
                        static_cast<unsigned>(liveCamo),
                        static_cast<unsigned>(livePT));

                ptArr[i]    = livePT;
                partsArr[i] = liveParts;
                camoArr[i]  = liveCamo;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall hkUpdatePartsStatus(void* self)
    {
        PartsPipeline pp;
        ReadPartsPipeline(self, &pp);

        static std::atomic<int> s_partsIdleTicks { kPartsSettleTicks };
        static std::atomic<int> s_partsStallTicks{ 0 };
        static std::atomic<int> s_partsStallLogged{ 0 };
        static std::atomic<int> s_partsUnservedTicks { 0 };
        static std::atomic<int> s_partsUnservedLogged{ 0 };

        if (pp.busy)
        {
            s_partsIdleTicks.store(0, std::memory_order_relaxed);
            const int held =
                s_partsStallTicks.fetch_add(1, std::memory_order_relaxed) + 1;

            if (held >= kFacialUnstickTicks && pp.valid && pp.state[pp.slot] == 1u)
            {
                PartsBlockView fv;
                ReadPartsBlockView(pp.slot, &fv);
                if (fv.read && fv.facialGroup
                 && fv.facialReq == 1 && fv.facialState == 1
                 && ReleaseAdditionalFacialBlock())
                {
                    static std::atomic<int> s_facialFreed{ 0 };
                    if (s_facialFreed.fetch_add(1, std::memory_order_relaxed) < 8)
                        Log("[OutfitRuntimeParts] the additional facial block this mission "
                            "created was never handed a path for player type %u, so it is "
                            "stuck unrequested - IsPartsBlockActiveNew refuses every parts "
                            "block while that is true, so the engine never builds a player "
                            "model and the loading screen never ends; released it the way "
                            "DeleteAdditionalFacialBlock does\n",
                            static_cast<unsigned>(outfit::ReadLivePlayerType()));
                }
            }

            if (held == kPartsStallTicks
             && s_partsStallLogged.fetch_add(1, std::memory_order_relaxed) < 4)
            {
                std::uint8_t ePt = 0xFFu, eParts = 0xFFu, eCamo = 0xFFu;
                const bool eOk =
                    pp.valid && ReadEngineIdentitySeh(self, pp.slot, &ePt, &eParts, &eCamo);

                Log("[OutfitRuntimeParts] the local player's parts pipeline has been "
                    "mid-load for %d straight ticks - the engine is still waiting on a "
                    "player model, so the loading screen will not end; live "
                    "partsType=0x%02X camo=0x%02X playerType=%u | localSlot=%u of %u "
                    "state=[%u %u %u %u]\n",
                    kPartsStallTicks,
                    static_cast<unsigned>(outfit::ReadLivePartsType()),
                    static_cast<unsigned>(outfit::ReadLiveSelectorCode()),
                    static_cast<unsigned>(outfit::ReadLivePlayerType()),
                    pp.slot, pp.count,
                    static_cast<unsigned>(pp.state[0]),
                    static_cast<unsigned>(pp.state[1]),
                    static_cast<unsigned>(pp.state[2]),
                    static_cast<unsigned>(pp.state[3]));

                Log("[OutfitRuntimeParts] stall detail: the identity the engine actually "
                    "snapshotted for slot %u reads parts=0x%02X camo=0x%02X playerType=%u "
                    "(%s); the .parts path resolver has run %u time(s) this session, last "
                    "asked for playerType=%u partsType=0x%02X. A snapshot that does not "
                    "match the live pair is the pin losing the race; a resolver count that "
                    "does not move while the machine sits at state 1 means the engine never "
                    "asked for a model at all\n",
                    pp.slot,
                    static_cast<unsigned>(eParts),
                    static_cast<unsigned>(eCamo),
                    static_cast<unsigned>(ePt),
                    eOk ? "read ok" : "arrays unreadable",
                    g_PartsPathCalls.load(std::memory_order_relaxed),
                    g_PartsPathLastPt.load(std::memory_order_relaxed),
                    g_PartsPathLastParts.load(std::memory_order_relaxed));

                PartsBlockView sv;
                if (pp.valid) ReadPartsBlockView(pp.slot, &sv);

                Log("[OutfitRuntimeParts] stall detail 2: the block controller was %s - "
                    "local player slot %u maps to parts block %u, resident groups=%u "
                    "streamed=%u, request state=[%d %d %d %d]. LoadPartsNew returns "
                    "without asking for a model whenever that block's request state is "
                    "0, and GetPartsBlockStateNew reports nothing at all once the block "
                    "index passes resident+streamed\n",
                    sv.read ? "read ok"
                            : (g_CapturedBlockController ? "unreadable"
                                                         : "never captured"),
                    pp.slot, sv.implSlot, sv.resident, sv.streamed,
                    sv.req4[0], sv.req4[1], sv.req4[2], sv.req4[3]);

                Log("[OutfitRuntimeParts] stall detail 3: block %u reports load state "
                    "%d, realize arm=%d job=%d; the additional facial block is %s with "
                    "request=%d state=%d path=0x%016llX. IsPartsBlockActiveNew needs the "
                    "block at 3 with request 3, and it needs the additional facial block "
                    "either absent or at request 3 state 3 - any other combination keeps "
                    "the parts machine at state 1 forever\n",
                    sv.implSlot, sv.blockState, sv.jobArm, sv.jobState,
                    sv.facialGroup ? "live" : "absent",
                    sv.facialReq, sv.facialState,
                    static_cast<unsigned long long>(sv.facialPath));
            }
        }
        else
        {
            s_partsStallTicks.store(0, std::memory_order_relaxed);
            const int idle =
                s_partsIdleTicks.fetch_add(1, std::memory_order_relaxed) + 1;

            PartsBlockView bv;
            if (pp.valid) ReadPartsBlockView(pp.slot, &bv);
            bool partsAlive = PartsGroupsLive(bv);

            static std::atomic<int> s_groupsHeldTicks{ 0 };
            if (!partsAlive)
            {
                const int held =
                    s_groupsHeldTicks.fetch_add(1, std::memory_order_relaxed) + 1;
                if (held > kPartsGroupsHoldMaxTicks)
                {
                    partsAlive = true;
                    static std::atomic<int> s_releaseLogged{ 0 };
                    if (s_releaseLogged.fetch_add(1, std::memory_order_relaxed) < 8)
                        Log("[OutfitRuntimeParts] the block controller has reported the "
                            "local player's parts block %u unloadable for %d straight "
                            "ticks (resident=%u streamed=%u request state=%d) - holding "
                            "the outfit reassert any longer would leave the character "
                            "and their suit unrestored for the whole mission, so the "
                            "hold is being released\n",
                            bv.implSlot, held, bv.resident, bv.streamed, bv.req);
                }
                else
                {
                    static std::atomic<int> s_deadLogged{ 0 };
                    if (s_deadLogged.fetch_add(1, std::memory_order_relaxed) < 16)
                        LogDebug("[OutfitRuntimeParts] the local player's parts block %u "
                            "cannot be loaded yet (resident=%u streamed=%u request "
                            "state=%d) - LoadPartsNew returns without asking for a model "
                            "while that request state is 0, so the outfit reassert is "
                            "held off until the mission unload's release is undone\n",
                            bv.implSlot, bv.resident, bv.streamed, bv.req);
                }
            }
            else
            {
                s_groupsHeldTicks.store(0, std::memory_order_relaxed);
            }

            TracePartsState(pp);

            const bool servedSlot = partsAlive &&
                pp.valid && (pp.settled || pp.state[pp.slot] == 0u);

            if (idle >= kPartsSettleTicks && servedSlot)
            {
                s_partsUnservedTicks.store(0, std::memory_order_relaxed);

                static std::atomic<int> s_reassertLogged{ 0 };
                if (s_reassertLogged.fetch_add(1, std::memory_order_relaxed) < 64)
                    LogDebug("[OutfitRuntimeParts] outfit reassert allowed at parts "
                        "load state %u (3 = served, 0 = before the engine snapshots "
                        "identity) slot=%u of %u\n",
                        static_cast<unsigned>(pp.state[pp.slot]),
                        pp.slot, pp.count);

                fobchars::ReassertSelectedCharacter();

                uniquecharpin::ReassertAfterRestore();

                PushIdentityToEngineArrays(self, pp);
            }
            else if (idle >= kPartsSettleTicks)
            {
                const int held =
                    s_partsUnservedTicks.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                if (held == kPartsStallTicks
                 && s_partsUnservedLogged.fetch_add(
                        1, std::memory_order_relaxed) < 4)
                    Log("[OutfitRuntimeParts] the local player's parts slot has "
                        "sat at load state %u (3 = served, 0 = pre-snapshot, 0xFF = the "
                        "parts pipeline could not be read at all) for %d "
                        "ticks, so the outfit reassert is still being held off - "
                        "writing the live parts type now would start a load the "
                        "re-stream can never re-issue; live partsType=0x%02X "
                        "camo=0x%02X playerType=%u | localSlot=%u of %u\n",
                        pp.valid ? pp.state[pp.slot] : 0xFFu, held,
                        static_cast<unsigned>(outfit::ReadLivePartsType()),
                        static_cast<unsigned>(outfit::ReadLiveSelectorCode()),
                        static_cast<unsigned>(outfit::ReadLivePlayerType()),
                        pp.slot, pp.count);
            }
        }

        uniquecharpin::SyncSupport(outfit::ReadLivePartsType(),
                                   outfit::ReadLiveSelectorCode(),
                                   MissionCodeGuard::ShouldBypassHooks());

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
                                static_cast<std::size_t>(playerTypeArr[i]);
                            if (servedIdx >= outfit::kPlayerTypeMax) continue;
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
                    if (!MissionCodeGuard::ShouldBypassHooks() && stateMachineArr)
                    {
                        bool uniqueChanged = false;
                        bool anyBusy       = false;
                        for (std::size_t i = 0;
                             i < outfit::shadow::kMaxSlots; ++i)
                        {
                            if (stateMachineArr[i] == 1
                             || stateMachineArr[i] == 2) anyBusy = true;
                            if (!outfit::IsUniqueCharacterPlayerType(
                                    playerTypeArr[i])) continue;
                            if (stateMachineArr[i] != 3) continue;
                            if (g_UniqueServedParts[i].load(
                                    std::memory_order_relaxed)
                                != partsTypeArr[i]) uniqueChanged = true;
                        }

                        outfit::NotePartsPipelineBusy(anyBusy);

                        int pending =
                            g_RestreamPendingTicks.load(std::memory_order_relaxed);
                        if (uniqueChanged && pending == 0)
                            pending = kRestreamMaxDeferTicks;

                        if (pending > 0)
                        {
                            const bool deadline = (pending == 1);
                            if (!anyBusy || deadline)
                            {
                                for (std::size_t i = 0;
                                     i < outfit::shadow::kMaxSlots; ++i)
                                {
                                    if (stateMachineArr[i] == 3)
                                        vextRestream |= (1u << i);
                                    g_UniqueServedParts[i].store(
                                        partsTypeArr[i],
                                        std::memory_order_relaxed);
                                }
                                pending = 0;

                                if (deadline && anyBusy)
                                {
                                    static std::atomic<int> s_lateLogged{ 0 };
                                    if (s_lateLogged.fetch_add(
                                            1, std::memory_order_relaxed) < 8)
                                        Log("[OutfitRuntimeParts] a slot was still "
                                            "mid-load %d ticks after the suit "
                                            "changed, so the re-stream fired "
                                            "without it - that slot keeps the "
                                            "previous suit until its next load\n",
                                            kRestreamMaxDeferTicks);
                                }
                            }
                            else
                            {
                                --pending;
                            }
                        }

                        g_RestreamPendingTicks.store(
                            pending, std::memory_order_relaxed);
                    }

                    if (vextRestream)
                    {
                        *reinterpret_cast<std::uint32_t*>(
                            reinterpret_cast<std::uint8_t*>(perPlayer)
                            + kPP_OffVariantChangedBits) |= vextRestream;
                        if (stateChangedBits)
                            *stateChangedBits |= vextRestream;
                    }
#ifdef _DEBUG
                    {
                        static std::uint32_t s_lastRestreamMask = 0;
                        if (vextRestream != s_lastRestreamMask)
                        {
                            s_lastRestreamMask = vextRestream;
                            if (vextRestream)
                                LogDebug("[OutfitRuntimeParts] active vext variant "
                                         "differs from last served - forcing a slot "
                                         "re-stream (mask=0x%X)\n",
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
                            std::uint8_t camo = camoTypeArr[i];
                            if (camo < outfit::kCustomSelectorStart
                             || camo > outfit::kCustomSelectorEnd)
                            {
                                outfit::shadow::Slot sh{};
                                if (outfit::shadow::Get(i, &sh)
                                 && sh.used
                                 && sh.realPartsType == pt
                                 && sh.realCamoType >= outfit::kCustomSelectorStart
                                 && sh.realCamoType <= outfit::kCustomSelectorEnd)
                                {
                                    camo = sh.realCamoType;
                                }
                                else
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
                                                 "collision guard: slot=%zu "
                                                 "partsType=0x%02X camo=0x%02X ply=%u - "
                                                 "the shadow holds no custom selector for "
                                                 "this parts type either, so this is not a "
                                                 "real custom slot; skipped to avoid a "
                                                 "mis-resolve hang\n",
                                            i, static_cast<unsigned>(pt),
                                            static_cast<unsigned>(camo),
                                            static_cast<unsigned>(ply));
                                    }
                                    continue;
                                }
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
                         "camo 0x%02X -> vext variants reset (the crate delivers "
                         "the base suit)\n", camo);
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
                     "0x%02X UNRESOLVED - left at vanilla 0; the dangling guard "
                     "degrades to the vanilla suit\n", camo);
        }

        return r;
    }


    static void __fastcall hkLoadPartsNew(
        void* self, std::uint32_t playerIndex,
        LoadPartsPlayerInfo* info, std::uint32_t flags)
    {
        if (self && g_CapturedBlockController != self)
        {
            if (g_CapturedBlockController)
            {
                static std::atomic<int> s_bcRebound{ 0 };
                if (s_bcRebound.fetch_add(1, std::memory_order_relaxed) < 4)
                    Log("[OutfitRuntimeParts] the block controller changed - "
                        "rebound to the live one; the previous pointer was kept "
                        "for the whole session and its allocation can be reused, "
                        "which turns the facial-unstick write into heap "
                        "corruption\n");
            }
            g_CapturedBlockController = self;
        }

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
                         "slot 0x%02X while its registration was deferred - drained "
                         "the pending heads first\n",
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
                        V_WriteOutfitPartsLoad(entry->partsType,
                                                      persistSel,
                                                      info->playerType);
                    feedShadow(entry, variantIdx);
                    outfit::ClearPendingOutfitDevelopId();
                }
                else
                {
                    static std::atomic<int> s_selMissLogged{ 0 };
                    if (s_selMissLogged.fetch_add(
                            1, std::memory_order_relaxed) < 12)
                        Log("[OutfitRuntimeParts] custom selector 0x%02X on slot %u "
                            "resolved to %s for player type %u - the descriptor "
                            "names a suit this load cannot serve, so the dangling "
                            "guard heals it to vanilla and the slot's shadow is "
                            "dropped\n",
                            static_cast<unsigned>(camo),
                            playerIndex,
                            entry ? "an outfit that does not declare that character"
                                  : "no registered outfit",
                            static_cast<unsigned>(info->playerType));
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
                        V_WriteOutfitPartsLoad(byPending->partsType,
                                                      persistSel,
                                                      info->playerType);
                    feedShadow(byPending, pendVar);
                    outfit::ClearPendingOutfitDevelopId();
                }
                else
                {
                    LogDebug("[OutfitRuntimeParts] BRICK-GUARD: broken-custom "
                             "signal (partsType=0 camo=0xFF) with no pending "
                             "developId (pt=%u) - healing to vanilla NORMAL "
                             "(0x01)\n",
                        static_cast<unsigned>(info->playerType));
                    info->playerPartsType    = kBionicArmVanillaPartsTypeSubstitute;
                    info->playerCamoType     = 0;
                    info->playerFaceEquipId  = 0;
                    info->playerFaceEquipUnk =
                        static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
                    if (isRealPlayerSlot)
                        V_WriteOutfitPartsLoad(
                            kBionicArmVanillaPartsTypeSubstitute, 0, info->playerType);
                    outfit::shadow::Clear(playerIndex);
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
                             "persisted mixed pair read as custom outfit "
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
                        V_WriteOutfitPartsLoad(mixEntry->partsType,
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
                LogDebug("[OutfitRuntimeParts] BRICK-GUARD: custom outfit "
                         "developId=%u partsType=0x%02X has a BAD asset path "
                         "(parts=%s fpk=%s pt=%u evidence=%s) - degrading to "
                         "vanilla NORMAL (0x01) to prevent an infinite load\n",
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
                    V_WriteOutfitPartsLoad(
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
                             "partsType=0x%02X variantIdx=%u has a missing/invalid "
                             "asset - healing to source camo 0x%02X\n",
                        static_cast<unsigned>(vextPartsType),
                        static_cast<unsigned>(vextVariantIdx),
                        static_cast<unsigned>(info->playerCamoType));
                    if (isRealPlayerSlot)
                        V_WriteOutfitPartsLoad(vextPartsType,
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
                        V_WriteOutfitPartsLoad(vextPartsType, sel,
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
                    LogDebug("[OutfitRuntimeParts] BRICK-GUARD: unresolved custom "
                             "suit partsType=0x%02X selector=0x%02X (pt=%u) - "
                             "healing to vanilla partsType=0x%02X, camo 0, "
                             "faceEquip 0\n",
                        static_cast<unsigned>(pt), static_cast<unsigned>(sel),
                        static_cast<unsigned>(info->playerType),
                        static_cast<unsigned>(healPartsType));
                    info->playerPartsType = healPartsType;
                    info->playerCamoType  = 0;
                    info->playerFaceEquipId  = 0;
                    info->playerFaceEquipUnk =
                        static_cast<std::uint8_t>(info->playerFaceEquipUnk & 0xF8);
                    if (isRealPlayerSlot)
                        V_WriteOutfitPartsLoad(healPartsType, 0, info->playerType);
                    outfit::shadow::Clear(playerIndex);
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
                             "(%s) - not offered by the worn outfit for this player "
                             "type\n",
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
                LogDebug("[SnakeHead] LoadPartsNew: custom head faceEquipId 0x%02X "
                         "normalized to 0x01 (bandana variation) for pt=%u; the "
                         "real slot is kept for the face hooks\n",
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

        __try
        {
            g_OrigLoadPartsNew(self, playerIndex, info, flags);
        }
        __finally
        {
            if (spoofPartsType) outfit::shadow::ClearCurrentSlot();
        }

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

    using ApplyQuietSuitShaderParams_t = void (__fastcall*)(void*, int);

    static ApplyQuietSuitShaderParams_t g_OrigApplyQuietSuitShaderParams = nullptr;
    static bool g_InstalledQuietSuitShaderParams = false;

    using SuitShaderProduce_t    = void (__fastcall*)(void*, int);
    using SuitShaderBlendVec_t   = void (__fastcall*)(void*, int, const void*, float);
    using SuitShaderBlendPair_t  = void (__fastcall*)(void*, int, float, float);

    static SuitShaderProduce_t   g_OrigSuitShaderProduce   = nullptr;
    static void* g_SuitShaderProduceTarget = nullptr;
    static SuitShaderBlendVec_t  g_OrigSuitShaderBlendVec  = nullptr;
    static SuitShaderBlendPair_t g_OrigSuitShaderBlendPair = nullptr;
    static void* g_SuitShaderBlendVecTarget  = nullptr;
    static void* g_SuitShaderBlendPairTarget = nullptr;

    static std::atomic<int>  g_QspRepairLogged{ 0 };
    static std::atomic<int>  g_QspBlendAttempts{ 0 };
    static std::atomic<bool> g_QspBlendArmed{ false };
    static std::atomic<bool> g_QspBlendGaveUp{ false };
    static std::atomic<bool> g_QspFaultLogged{ false };
    static std::atomic<bool> g_QspBlendFaultLogged{ false };

    constexpr std::size_t    kQSP_OffPlayer2GameObject = 0x138;
    constexpr std::size_t    kQSP_OffPartsControl      = 0x110;
    constexpr std::size_t    kQSP_OffModelArray        = 0x30;
    constexpr std::size_t    kQSP_OffParamEntryCache   = 0x38;
    constexpr std::size_t    kQSP_OffModelsPerSlot     = 0x40;
    constexpr std::size_t    kQSP_OffSlotCount         = 0x54;
    constexpr std::uint32_t  kQSP_KeysPerModel         = 6;
    constexpr std::size_t    kSPT_OffKeyCount          = 0x20;
    constexpr std::size_t    kSPT_OffKeyData           = 0x28;
    constexpr std::size_t    kSPT_OffValueCount        = 0x30;
    constexpr std::size_t    kSPT_OffValueCapacity     = 0x34;
    constexpr std::size_t    kSPT_OffValueData         = 0x38;
    constexpr std::uint32_t  kQSP_MaxSaneModelsPerSlot = 0x100;
    constexpr std::uint32_t  kQSP_MaxSaneSlotCount     = 0x20;
    constexpr std::uint32_t  kSPT_MaxSaneParams        = 0x4000;
    constexpr std::uint32_t  kSPT_EntryIndexMask       = 0x1FFFFFFFu;
    constexpr std::uintptr_t kQSP_UserAddressCeiling   = 0x0000800000000000ull;
    constexpr std::size_t    kQSP_VtblProduce          = 1;
    constexpr std::size_t    kQSP_VtblBlendPair        = 2;
    constexpr std::size_t    kQSP_VtblBlendVec         = 3;
    constexpr std::size_t    kQSP_FnScanBytes          = 0x200;
    constexpr std::size_t    kQSP_FnScanStep           = 0x10;
    constexpr std::size_t    kQSP_FnHeadBytes          = 0x40;
    constexpr int            kQSP_MaxBlendAttempts     = 8;

    static bool ShaderParamRangeIsSane(std::uintptr_t data, std::uint32_t count,
                                       std::uint32_t stride)
    {
        if (count > kSPT_MaxSaneParams) return false;
        if (count == 0) return true;
        if (data == 0 || data >= kQSP_UserAddressCeiling) return false;
        return data + static_cast<std::uintptr_t>(count) * stride
               < kQSP_UserAddressCeiling;
    }

    static bool ShaderParamTableIsLive(const std::uint8_t* tbl)
    {
        if (reinterpret_cast<std::uintptr_t>(tbl) >= kQSP_UserAddressCeiling)
            return false;

        if (!ShaderParamRangeIsSane(
                *reinterpret_cast<const std::uintptr_t*>(tbl + kSPT_OffKeyData),
                *reinterpret_cast<const std::uint32_t*>(tbl + kSPT_OffKeyCount), 8u))
            return false;

        const std::uint32_t valCount =
            *reinterpret_cast<const std::uint32_t*>(tbl + kSPT_OffValueCount);
        const std::uint32_t valCap =
            *reinterpret_cast<const std::uint32_t*>(tbl + kSPT_OffValueCapacity);
        if (valCount > valCap) return false;

        return ShaderParamRangeIsSane(
            *reinterpret_cast<const std::uintptr_t*>(tbl + kSPT_OffValueData),
            valCap, 16u);
    }

    static bool CachedEntryIsStale(const std::uint8_t* tbl, const void* cached)
    {
        if (!cached) return false;

        if (*reinterpret_cast<const std::uintptr_t*>(tbl + kSPT_OffValueData) == 0)
            return true;

        const std::uintptr_t keyData =
            *reinterpret_cast<const std::uintptr_t*>(tbl + kSPT_OffKeyData);
        const std::uint32_t keyCount =
            *reinterpret_cast<const std::uint32_t*>(tbl + kSPT_OffKeyCount);
        if (keyData == 0 || keyCount == 0) return true;

        const std::uintptr_t c = reinterpret_cast<std::uintptr_t>(cached);
        if (c < keyData) return true;
        if (c >= keyData + static_cast<std::uintptr_t>(keyCount) * 8u) return true;
        if (((c - keyData) % 8u) != 0) return true;

        const std::uint32_t valCap =
            *reinterpret_cast<const std::uint32_t*>(tbl + kSPT_OffValueCapacity);
        const std::uint32_t idx =
            *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(cached) + 4)
            & kSPT_EntryIndexMask;
        return idx >= valCap;
    }

    static void* ResolveGateParts(void* self)
    {
        void* parts = nullptr;
        __try
        {
            auto* go = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(self) + kQSP_OffPlayer2GameObject);
            if (go) parts = *reinterpret_cast<void**>(go + kQSP_OffPartsControl);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { parts = nullptr; }
        return parts;
    }

    static std::uint32_t SanitizeShaderParamSlot(void* partsControl, int slot)
    {
        std::uint32_t repaired = 0;
        __try
        {
            auto* parts = reinterpret_cast<std::uint8_t*>(partsControl);
            if (parts)
            {
                const std::uint32_t slotCount =
                    *reinterpret_cast<std::uint8_t*>(parts + kQSP_OffSlotCount);
                const std::uint32_t perSlot = *reinterpret_cast<std::uint32_t*>(
                    parts + kQSP_OffModelsPerSlot);
                auto** tables =
                    *reinterpret_cast<void***>(parts + kQSP_OffModelArray);
                auto** cache =
                    *reinterpret_cast<void***>(parts + kQSP_OffParamEntryCache);

                if (slot >= 0 && slotCount != 0
                 && slotCount <= kQSP_MaxSaneSlotCount
                 && static_cast<std::uint32_t>(slot) < slotCount
                 && tables && perSlot != 0
                 && perSlot <= kQSP_MaxSaneModelsPerSlot)
                {
                    const std::uint32_t base =
                        static_cast<std::uint32_t>(slot) * perSlot;
                    for (std::uint32_t i = 0; i < perSlot; ++i)
                    {
                        auto* tbl =
                            reinterpret_cast<std::uint8_t*>(tables[base + i]);
                        if (!tbl) continue;

                        const std::uint32_t first =
                            (base + i) * kQSP_KeysPerModel;

                        if (!ShaderParamTableIsLive(tbl))
                        {
                            tables[base + i] = nullptr;
                            if (cache)
                                for (std::uint32_t k = 0; k < kQSP_KeysPerModel; ++k)
                                    cache[first + k] = nullptr;
                            ++repaired;
                            continue;
                        }

                        if (!cache) continue;
                        for (std::uint32_t k = 0; k < kQSP_KeysPerModel; ++k)
                        {
                            if (!CachedEntryIsStale(tbl, cache[first + k])) continue;
                            cache[first + k] = nullptr;
                            ++repaired;
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return repaired;
    }

    static void SanitizeAndReport(void* partsControl, int slot)
    {
        const std::uint32_t repaired = SanitizeShaderParamSlot(partsControl, slot);
        if (repaired != 0
         && g_QspRepairLogged.fetch_add(1, std::memory_order_relaxed) < 8)
            Log("[OutfitRuntimeParts] retired %u stale shader-param cache entry/"
                "table(s) on slot %d before the engine read them - a suit change had "
                "replaced this parts control's parameter tables while its key-entry "
                "cache still pointed into the previous suit's arrays, and the engine "
                "only rebuilds that cache while the slot's camo byte is Quiet's 0x74, "
                "which a custom outfit is not for the first few hundred ms\n",
                static_cast<unsigned>(repaired), slot);
    }

    static std::size_t SafeCopyBoundedCode(const void* fn, std::uint8_t* dst,
                                           std::size_t n)
    {
        std::size_t got = 0;
        while (got + kQSP_FnScanStep <= n)
        {
            __try
            {
                std::memcpy(dst + got,
                            static_cast<const std::uint8_t*>(fn) + got,
                            kQSP_FnScanStep);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            got += kQSP_FnScanStep;
        }
        return got;
    }

    static bool SuitShaderBlendPatternMatches(const void* fn)
    {
        if (!fn) return false;

        std::uint8_t code[kQSP_FnScanBytes];
        std::size_t len = SafeCopyBoundedCode(fn, code, sizeof code);
        if (len < kQSP_FnHeadBytes) return false;

        for (std::size_t i = 0; i + 4 <= len; ++i)
            if (code[i] == 0xCC && code[i + 1] == 0xCC
             && code[i + 2] == 0xCC && code[i + 3] == 0xCC) { len = i; break; }

        const std::size_t head = len < kQSP_FnHeadBytes ? len : kQSP_FnHeadBytes;
        bool readsModelsPerSlot = false;
        for (std::size_t i = 0; i + 3 <= head; ++i)
            if (code[i] == 0x8B && code[i + 1] == 0x41 && code[i + 2] == 0x40)
            { readsModelsPerSlot = true; break; }
        if (!readsModelsPerSlot) return false;

        for (std::size_t i = 0; i + 6 <= len; ++i)
            if (code[i] == 0x81 && code[i + 1] == 0xE1
             && code[i + 2] == 0xFF && code[i + 3] == 0xFF
             && code[i + 4] == 0xFF && code[i + 5] == 0x1F)
                return true;
        return false;
    }

    static bool SuitShaderProducePatternMatches(const void* fn)
    {
        if (!fn) return false;

        std::uint8_t code[kQSP_FnScanBytes];
        std::size_t len = SafeCopyBoundedCode(fn, code, sizeof code);
        if (len < kQSP_FnHeadBytes) return false;

        for (std::size_t i = 0; i + 4 <= len; ++i)
            if (code[i] == 0xCC && code[i + 1] == 0xCC
             && code[i + 2] == 0xCC && code[i + 3] == 0xCC) { len = i; break; }

        const std::size_t head = len < kQSP_FnHeadBytes ? len : kQSP_FnHeadBytes;
        bool readsModelsPerSlot = false;
        for (std::size_t i = 0; i + 3 <= head; ++i)
            if (code[i] == 0x8B && code[i + 1] == 0x41 && code[i + 2] == 0x40)
            { readsModelsPerSlot = true; break; }
        if (!readsModelsPerSlot) return false;

        for (std::size_t i = 0; i + 6 <= len; ++i)
            if (code[i] == 0x81 && code[i + 1] == 0xE1
             && code[i + 2] == 0xFF && code[i + 3] == 0xFF
             && code[i + 4] == 0xFF && code[i + 5] == 0x1F)
                return false;

        for (std::size_t i = 0; i + 4 <= len; ++i)
            if (code[i] == 0x48 && code[i + 1] == 0x89
             && code[i + 2] == 0x04 && code[i + 3] == 0xD1)
                return true;
        return false;
    }

    static bool g_QspSanitizeInProducer = true;

    static void CallSuitShaderProduceGuarded(void* partsControl, int slot)
    {
        __try
        {
            g_OrigSuitShaderProduce(partsControl, slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_QspBlendFaultLogged.exchange(true))
                Log("[OutfitRuntimeParts] the suit shader-param rebuild faulted on "
                    "slot %d and was swallowed - it wrote into a parameter table the "
                    "suit change had already freed; the suit keeps the material "
                    "params it had last frame instead of taking the game down\n", slot);
        }
    }

    static void __fastcall hkSuitShaderProduce(void* partsControl, int slot)
    {
        if (!g_OrigSuitShaderProduce) return;

        static std::atomic<bool> s_noted{ false };
        if (!g_QspSanitizeInProducer && !s_noted.exchange(true))
            Log("[OutfitRuntimeParts] the shader-param rebuild runs unsanitised "
                "(A/B): the crash guard still wraps it, but stale cache entries are "
                "no longer retired on this path - if a character that used to render "
                "comes back, the sanitize was blanking its material params\n");

        if (g_QspSanitizeInProducer)
            SanitizeAndReport(partsControl, slot);
        CallSuitShaderProduceGuarded(partsControl, slot);
    }

    static void CallSuitShaderBlendVecGuarded(void* partsControl, int slot,
                                              const void* colour, float weight)
    {
        __try
        {
            g_OrigSuitShaderBlendVec(partsControl, slot, colour, weight);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_QspBlendFaultLogged.exchange(true))
                Log("[OutfitRuntimeParts] the per-frame suit shader-param blend "
                    "faulted on slot %d and was swallowed - it read a cached "
                    "parameter entry belonging to a parameter table the suit change "
                    "had already replaced; the suit keeps last frame's material "
                    "params instead of taking the game down\n", slot);
        }
    }

    static void CallSuitShaderBlendPairGuarded(void* partsControl, int slot,
                                               float a, float b)
    {
        __try
        {
            g_OrigSuitShaderBlendPair(partsControl, slot, a, b);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_QspBlendFaultLogged.exchange(true))
                Log("[OutfitRuntimeParts] the per-frame suit shader-param blend "
                    "faulted on slot %d and was swallowed - it read a cached "
                    "parameter entry belonging to a parameter table the suit change "
                    "had already replaced; the suit keeps last frame's material "
                    "params instead of taking the game down\n", slot);
        }
    }

    static void __fastcall hkSuitShaderBlendVec(void* partsControl, int slot,
                                                const void* colour, float weight)
    {
        if (!g_OrigSuitShaderBlendVec) return;
        SanitizeAndReport(partsControl, slot);
        CallSuitShaderBlendVecGuarded(partsControl, slot, colour, weight);
    }

    static void __fastcall hkSuitShaderBlendPair(void* partsControl, int slot,
                                                 float a, float b)
    {
        if (!g_OrigSuitShaderBlendPair) return;
        SanitizeAndReport(partsControl, slot);
        CallSuitShaderBlendPairGuarded(partsControl, slot, a, b);
    }

    static bool ReadPartsControlVtbl(void* partsControl, void** outVec,
                                     void** outPair, void** outProduce)
    {
        bool ok = false;
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(partsControl);
            if (vtbl)
            {
                *outVec  = vtbl[kQSP_VtblBlendVec];
                *outPair = vtbl[kQSP_VtblBlendPair];
                *outProduce = vtbl[kQSP_VtblProduce];
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        return ok;
    }

    static void InstallSuitShaderBlendGuards(void* partsControl)
    {
        if (g_QspBlendArmed.load(std::memory_order_relaxed)) return;
        if (g_QspBlendAttempts.fetch_add(1, std::memory_order_relaxed)
                >= kQSP_MaxBlendAttempts)
        {
            if (!g_QspBlendGaveUp.exchange(true))
                Log("[OutfitRuntimeParts] could not identify the per-frame suit "
                    "shader-param blend on the parts control vtable - the stale "
                    "cache is still retired whenever the camo-0x74 pass runs, but "
                    "the window between a custom suit change and that pass stays "
                    "unguarded\n");
            return;
        }

        void* fnVec     = nullptr;
        void* fnPair    = nullptr;
        void* fnProduce = nullptr;
        if (!ReadPartsControlVtbl(partsControl, &fnVec, &fnPair, &fnProduce))
            return;
        if (!SuitShaderBlendPatternMatches(fnVec)) return;
        if (!SuitShaderBlendPatternMatches(fnPair)) return;
        if (!SuitShaderProducePatternMatches(fnProduce)) return;

        const bool okVec = CreateAndEnableHook(
            fnVec, reinterpret_cast<void*>(&hkSuitShaderBlendVec),
            reinterpret_cast<void**>(&g_OrigSuitShaderBlendVec));
        const bool okPair = CreateAndEnableHook(
            fnPair, reinterpret_cast<void*>(&hkSuitShaderBlendPair),
            reinterpret_cast<void**>(&g_OrigSuitShaderBlendPair));
        const bool okProduce = CreateAndEnableHook(
            fnProduce, reinterpret_cast<void*>(&hkSuitShaderProduce),
            reinterpret_cast<void**>(&g_OrigSuitShaderProduce));

        if (!okVec || !okPair || !okProduce)
        {
            if (okProduce)
            {
                DisableAndRemoveHook(fnProduce);
                g_OrigSuitShaderProduce = nullptr;
            }
            if (okVec)
            {
                DisableAndRemoveHook(fnVec);
                g_OrigSuitShaderBlendVec = nullptr;
            }
            if (okPair)
            {
                DisableAndRemoveHook(fnPair);
                g_OrigSuitShaderBlendPair = nullptr;
            }
            Log("[OutfitRuntimeParts] the per-frame suit shader-param blend guard "
                "FAILED to install (vec=%s pair=%s rebuild=%s) - a custom suit "
                "change can still leave the engine reading or writing a parameter "
                "table the previous suit owned\n",
                okVec ? "OK" : "fail", okPair ? "OK" : "fail",
                okProduce ? "OK" : "fail");
            return;
        }

        g_SuitShaderBlendVecTarget  = fnVec;
        g_SuitShaderBlendPairTarget = fnPair;
        g_SuitShaderProduceTarget   = fnProduce;
        g_QspBlendArmed.store(true, std::memory_order_release);

#ifdef _DEBUG
        LogDebug("[OutfitRuntimeParts] per-frame suit shader-param blend guard armed: "
                 "vec=%p pair=%p rebuild=%p (identified by the parameter-entry "
                 "index decode and cache store they carry, taken off the live parts "
                 "control vtable rather than a hard-coded address)\n",
                 fnVec, fnPair, fnProduce);
#endif
    }

    static void CallQuietSuitShaderParamsGuarded(void* self, int slot)
    {
        __try
        {
            g_OrigApplyQuietSuitShaderParams(self, slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_QspFaultLogged.exchange(true))
                Log("[OutfitRuntimeParts] the engine's suit shader-param pass faulted "
                    "on slot %d and was swallowed - it reached a model whose "
                    "shader-param table a suit change had already freed; the suit "
                    "keeps the material params it had last frame instead of taking "
                    "the game down\n", slot);
        }
    }

    static void __fastcall hkApplyQuietSuitShaderParams(void* self, int slot)
    {
        if (!g_OrigApplyQuietSuitShaderParams) return;

        if (self)
        {
            void* parts = ResolveGateParts(self);
            if (parts)
            {
                InstallSuitShaderBlendGuards(parts);
                SanitizeAndReport(parts, slot);
            }
        }

        CallQuietSuitShaderParamsGuarded(self, slot);
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
                LogDebug("[OutfitFacialGuard] skipped a facial apply with a wild "
                         "AnimControl binding (self=%p skip#%u) - prevented a "
                         "SetMotionDataCore AV; face left unchanged\n",
                    self, static_cast<unsigned>(n));
            return;
        }
        if (g_OrigPluginFacialApplyMotion)
            g_OrigPluginFacialApplyMotion(self, a2, a3, a4);
    }

    std::uint32_t GetLocalPartsSlot()
    {
        return g_LocalPartsSlot.load(std::memory_order_relaxed);
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
            { ResolveGameAddress(gAddr.Player2BlockController_ApplyQuietSuitShaderParams),
              reinterpret_cast<void*>(&hkApplyQuietSuitShaderParams),
              reinterpret_cast<void**>(&g_OrigApplyQuietSuitShaderParams),
              &g_InstalledQuietSuitShaderParams },
        };
        for (auto& h : hooks)
        {
            if (h.tgt) *h.installed = CreateAndEnableHook(h.tgt, h.hk, h.orig);
        }

        if (ResolveGameAddress(
                gAddr.Player2BlockController_ApplyQuietSuitShaderParams)
            && !g_InstalledQuietSuitShaderParams)
            Log("[OutfitRuntimeParts] the Quiet suit shader-param guard FAILED to "
                "install - the engine keeps pushing her vanilla sneaking-suit "
                "shader params onto custom outfits, and a suit change during that "
                "pass crashes on a freed model entry\n");

#ifdef _DEBUG
        LogDebug("[OutfitRuntimeParts] Quiet suit shader-param guard: %s\n",
                 g_InstalledQuietSuitShaderParams
                     ? "armed"
                     : "skip (no address for this build)");
#endif

        g_FoxModelFromHandle = reinterpret_cast<FoxModelFromHandle_t>(
            ResolveGameAddress(gAddr.Fox_ModelFromHandle));

        if (g_InstalledUpdatePartsStatus)
        {
            InstallCaseDArmUnpin();
            uniquecharpin::Install();
        }

#ifdef _DEBUG
        LogDebug("[OutfitRuntimeParts] installed: parts=%s fpk=%s camo=%s "
                 "diamond=%s camoFv2=%s diamondFv2=%s armFv2=%s armFpk=%s "
                 "snakeFaceFv2=%s snakeFaceFpk=%s avFaceFv2=%s avFaceFpk=%s "
                 "avFaceEdit=%s avHeadOptFv2=%s avHeadOptFpk=%s lpn=%s needFace=%s "
                 "needFaceAv=%s handSlot=%s artHand=%s artHandCur=%s procSignal=%s "
                 "updParts=%s setUpParts=%s facialGuard=%s supplyCamo=%s\n",
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
            { &g_InstalledQuietSuitShaderParams, ResolveGameAddress(gAddr.Player2BlockController_ApplyQuietSuitShaderParams) },
        };
        for (auto& h : hooks)
        {
            if (*h.installed && h.tgt) DisableAndRemoveHook(h.tgt);
            *h.installed = false;
        }

        if (g_QspBlendArmed.exchange(false, std::memory_order_acq_rel))
        {
            if (g_SuitShaderBlendVecTarget)
                DisableAndRemoveHook(g_SuitShaderBlendVecTarget);
            if (g_SuitShaderBlendPairTarget)
                DisableAndRemoveHook(g_SuitShaderBlendPairTarget);
            if (g_SuitShaderProduceTarget)
                DisableAndRemoveHook(g_SuitShaderProduceTarget);
            g_SuitShaderBlendVecTarget  = nullptr;
            g_SuitShaderBlendPairTarget = nullptr;
            g_SuitShaderProduceTarget   = nullptr;
            g_OrigSuitShaderBlendVec    = nullptr;
            g_OrigSuitShaderBlendPair   = nullptr;
            g_OrigSuitShaderProduce     = nullptr;
        }

        if (g_CaseDArmUnpinActive && g_CaseDPatchSite)
        {
            DWORD oldp = 0;
            if (VirtualProtect(g_CaseDPatchSite, 9, PAGE_EXECUTE_READWRITE, &oldp))
            {
                std::memcpy(g_CaseDPatchSite, g_CaseDOrigBytes, 9);
                DWORD tmp = 0;
                VirtualProtect(g_CaseDPatchSite, 9, oldp, &tmp);
                FlushInstructionCache(GetCurrentProcess(), g_CaseDPatchSite, 9);
                if (g_CaseDTrampoline)
                    VirtualFree(g_CaseDTrampoline, 0, MEM_RELEASE);
            }
            else
            {
                Log("[CaseDArmUnpin] the patched site could not be restored at "
                    "uninstall, so its trampoline is LEAKED rather than freed - the "
                    "jump written into the game still points into it\n");
            }
            g_CaseDTrampoline     = nullptr;
            g_CaseDPatchSite      = nullptr;
            g_CaseDArmUnpinActive = false;
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

        uniquecharpin::Uninstall();

        outfit::shadow::ResetAll("Uninstall_OutfitRuntimeParts_Hooks");
        outfit::shadow::ResetArmTierCache();

#ifdef _DEBUG
        LogDebug("[OutfitRuntimeParts] removed\n");
#endif
    }
}
