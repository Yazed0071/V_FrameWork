#include "pch.h"
#include "MbDvcMapCallbackIconImpl_DispOnNewMarkerIcon.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <iterator>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "HeadMarkMarkerEvCall_SetIconSubType.h"

namespace
{
    constexpr std::size_t   kImpl_Base           = 0x30;
    constexpr std::size_t   kBase_CommonData     = 0xC8;
    constexpr std::size_t   kRec_Stride          = 0x30;
    constexpr std::size_t   kRec_Bias            = 2;
    constexpr std::size_t   kRec_HandleId        = 0x18;
    constexpr std::uint32_t kMarkerSlotCount     = 0x80;

    constexpr std::size_t   kBase_Utility        = 0x20;
    constexpr std::size_t   kUtility_SetColourRgb = 0x308;
    constexpr std::size_t   kUtility_SetColourGroup  = 0x318;
    constexpr std::size_t   kUtility_SetColourGroup2 = 0x320;

    constexpr std::uint32_t kGroupSentinel     = 0x56460000u;
    constexpr std::uint32_t kGroupSentinelMask = 0xFFFF0000u;
    constexpr std::size_t   kComponent_GetColour = 0xC8;

    constexpr std::uint32_t kMapIconCategoryMask = 0xFFFF0000u;
    constexpr std::uint32_t kMapIconCategory     = 0x00010000u;
    constexpr std::uint32_t kMapIconSlotMask     = 0x000000FFu;

    constexpr std::uint16_t kInvalidMarkerId     = 0xFFFFu;

    const std::uint8_t kDispatchPrologue[] = {
        0x4C, 0x8B, 0xDC,
        0x55,
        0x56,
        0x41, 0x55,
        0x49, 0x8D, 0xAB, 0xA8, 0xF3, 0xFF, 0xFF,
        0x48, 0x81, 0xEC, 0x40, 0x0D, 0x00, 0x00,
        0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x33, 0xC4,
    };

    const std::uint8_t kDispatchMask[] = {
        1, 1, 1,
        1,
        1,
        1, 1,
        1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 0, 0, 0, 0,
        1, 1, 1,
    };

    constexpr std::uint8_t  kJumpRel32            = 0xE9;

    constexpr std::ptrdiff_t kDispatchScanBack    = 0x8000;
    constexpr std::ptrdiff_t kDispatchScanForward = 0x8000;

    const std::size_t kMarkerNodes[] = { 0x00 };
    const std::size_t kGoalNodes[]   = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30 };
    const std::size_t kGeneralNodes[]      = { 0x00, 0x08, 0x10, 0x18 };
    const std::size_t kTargetMarkerNodes[] = { 0x00, 0x08 };
    const std::size_t kAreaNodes[]         = { 0x00, 0x08, 0x10 };

    const std::uint8_t kPrologue[] = {
        0x44, 0x89, 0x44, 0x24, 0x18,
        0x53,
        0x55,
        0x56,
        0x57,
        0x41, 0x56,
        0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x33, 0xC4,
        0x48, 0x89, 0x44, 0x24, 0x50,
        0x48, 0x8B, 0xF1,
        0x48, 0x8B, 0x49, 0x30,
        0x41, 0x8B, 0xC0,
        0x48, 0x8B, 0x79, 0x20,
        0x25, 0x00, 0x00, 0xFF, 0xFF,
    };

    const std::uint8_t kPrologueMask[] = {
        1, 1, 1, 1, 1,
        1,
        1,
        1,
        1,
        1, 1,
        1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 0, 0, 0, 0,
        1, 1, 1,
        1, 1, 1, 1, 1,
        1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1, 1,
    };

    const std::uint8_t kImportantPrologue[] = {
        0x48, 0x89, 0x5C, 0x24, 0x20,
        0x44, 0x89, 0x44, 0x24, 0x18,
        0x55,
        0x56,
        0x57,
        0x48, 0x83, 0xEC, 0x40,
        0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x33, 0xC4,
        0x48, 0x89, 0x44, 0x24, 0x30,
        0x48, 0x8B, 0xE9,
        0x48, 0x8B, 0x49, 0x30,
        0x41, 0x8B, 0xC0,
        0x48, 0x8B, 0x79, 0x20,
        0x25, 0x00, 0x00, 0xFF, 0xFF,
    };

    const std::uint8_t kImportantMask[] = {
        1, 1, 1, 1, 1,
        1, 1, 1, 1, 1,
        1,
        1,
        1,
        1, 1, 1, 1,
        1, 1, 1, 0, 0, 0, 0,
        1, 1, 1,
        1, 1, 1, 1, 1,
        1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1, 1,
    };

    const std::uint8_t kGeneralPrologue[] = {
        0x44, 0x89, 0x44, 0x24, 0x18,
        0x55, 0x56, 0x57, 0x41, 0x56,
        0x48, 0x8D, 0x6C, 0x24, 0xC1,
        0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,
    };

    const std::uint8_t kGeneralMask[] = {
        1, 1, 1, 1, 1,
        1, 1, 1, 1, 1,
        1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1,
    };

    const std::uint8_t kDisplayParamPrologue[] = {
        0x89, 0x54, 0x24, 0x10,
        0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x68,
    };

    const std::uint8_t kDisplayParamMask[] = {
        1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1,
    };

    const std::uint8_t kAreaPrologue[] = {
        0x48, 0x8B, 0xC4,
        0x44, 0x89, 0x40, 0x18,
        0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00,
    };

    const std::uint8_t kAreaMask[] = {
        1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1,
    };

    const std::uint8_t kTargetMarkerPrologue[] = {
        0x44, 0x89, 0x44, 0x24, 0x18,
        0x53, 0x55, 0x56, 0x57, 0x41, 0x56,
        0x48, 0x83, 0xEC, 0x70,
    };

    const std::uint8_t kTargetMarkerMask[] = {
        1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1,
        1, 1, 1, 1,
    };

    const std::uint8_t kGoalPrologue[] = {
        0x4C, 0x8B, 0xDC,
        0x45, 0x89, 0x43, 0x18,
        0x55,
        0x53,
        0x56,
        0x41, 0x56,
        0x41, 0x57,
        0x49, 0x8D, 0x6B, 0xC8,
        0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00,
        0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x33, 0xC4,
        0x48, 0x89, 0x44, 0x24, 0x70,
        0x48, 0x8B, 0x41, 0x30,
        0x44, 0x89, 0x4C, 0x24, 0x3C,
        0x41, 0x8B, 0xD8,
        0x48, 0x8B, 0x70, 0x20,
        0x41, 0x8B, 0xC0,
        0x4C, 0x8B, 0xF2,
        0x25, 0x00, 0x00, 0xFF, 0xFF,
        0x4C, 0x8B, 0xF9,
        0x3D, 0x00, 0x00, 0x01, 0x00,
    };

    const std::uint8_t kGoalMask[] = {
        1, 1, 1,
        1, 1, 1, 1,
        1,
        1,
        1,
        1, 1,
        1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 0, 0, 0, 0,
        1, 1, 1,
        1, 1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1, 1,
        1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1, 1, 1, 1,
        1, 1, 1,
        1, 1, 1, 1, 1,
    };

    using DispOnGoalIcon_t = void(__fastcall*)(void* self, void* icon, std::uint32_t mapIconId,
                                               std::uint32_t a4, std::uint32_t a5,
                                               std::uint32_t a6, std::uint32_t a7);

        using DispOnImportantIcon_t = void(__fastcall*)(void* self, void* icon, std::uint32_t mapIconId);

    using DispOnNewMarkerIcon_t = void(__fastcall*)(void* self, void* icon, std::uint32_t mapIconId,
                                                   std::uint8_t a, std::uint8_t b, std::uint8_t c);
    using ComponentGetColour_t  = void(__fastcall*)(void* component, float* outColour);
    using UtilitySetRgb_t       = void(__fastcall*)(void* utility, void* component,
                                                    float r, float g, float b);
    using UtilitySetGroup_t     = void(__fastcall*)(void* utility, void* component,
                                                    std::uint32_t group);
    using DispOnListIcon_t      = void(__fastcall*)(void* self, void* icon,
                                                    std::uint32_t mapIconId, float a4);
    using GetIconDisplayParam_t = bool(__fastcall*)(void* self, std::uint32_t mapIconId,
                                                    std::uint32_t* outColourGroup,
                                                    std::uint64_t* outIconPath,
                                                    float* outScaleX, float* outScaleY,
                                                    float* outAlpha, float* outParam);
    using IsFobMode_t           = bool(__fastcall*)();
    using DisplayMarkerIcons_t  = void(__fastcall*)(void* self, std::uintptr_t a2,
                                                    std::uintptr_t a3, std::uintptr_t a4);

    DispOnNewMarkerIcon_t  g_Orig          = nullptr;
    DispOnImportantIcon_t  g_OrigImportant = nullptr;
    DispOnGoalIcon_t       g_OrigGoal      = nullptr;
    DispOnListIcon_t       g_OrigGeneral      = nullptr;
    DispOnListIcon_t       g_OrigTargetMarker = nullptr;
    DispOnListIcon_t       g_OrigArea         = nullptr;
    GetIconDisplayParam_t  g_OrigDisplayParam = nullptr;
    UtilitySetGroup_t      g_OrigSetGroup     = nullptr;
    UtilitySetGroup_t      g_OrigSetGroup2    = nullptr;
    bool                   g_SetGroupTried    = false;

    struct SlotRgb
    {
        float        rgb[3];
        std::uint8_t gen;
        bool         live;
    };

    SlotRgb g_SlotRgb[kMarkerSlotCount] = {};
    DisplayMarkerIcons_t   g_OrigDispatch  = nullptr;
    unsigned               g_DispatchNoted = 0;
    IsFobMode_t           g_IsFobMode = nullptr;
    bool                  g_TintFailureLogged = false;
    unsigned              g_Probe[6]          = {};
    unsigned              g_NoId[6]           = {};
    unsigned              g_NonMarker[6]      = {};
    constexpr unsigned    kNonMarkerLimit     = 2;
    unsigned              g_ParamProbe        = 0;
    constexpr unsigned    kParamProbeLimit    = 24;
    unsigned              g_CensusCount       = 0;
    std::uint32_t         g_LastCensus        = 0;
    constexpr unsigned    kCensusLimit        = 6;
    constexpr unsigned    kNoIdLimit          = 3;
    unsigned              g_Entered[6]        = {};
    constexpr unsigned    kProbeLimit         = 12;
    constexpr bool        kInstallGoalIcon    = true;

    constexpr std::size_t kData_GetIconType    = 0xD0;
    constexpr std::size_t kData_IsHidden       = 0x120;
    constexpr std::size_t kData_UseMarkerIcon  = 0x138;
    constexpr std::size_t kData_UseImportant   = 0x140;
    constexpr std::size_t kData_IsActive       = 0x150;
    constexpr std::size_t kData_UseGoalIcon    = 0x160;

    constexpr std::uint16_t kSoldierBandLow    = 0x0400u;
    constexpr std::uint16_t kSoldierBandHigh   = 0x05FFu;

    using DataGetU32_t = std::uint32_t(__fastcall*)(void* data, std::uint32_t slot);
    using DataGetU8_t  = std::uint8_t (__fastcall*)(void* data, std::uint32_t slot);

    bool CallIsFobModeSeh()
    {
        __try
        {
            return g_IsFobMode();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    bool IsOnlineSession()
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return true;
        if (!g_IsFobMode)
            return false;
        return CallIsFobModeSeh();
    }

    bool ResolveSystemIdSeh(void* self, std::uint32_t slot, std::uint16_t& outId)
    {
        if (slot >= kMarkerSlotCount)
            return false;

        __try
        {
            auto base = *reinterpret_cast<std::uint8_t**>(
                static_cast<std::uint8_t*>(self) + kImpl_Base);
            if (!base)
                return false;

            auto data = *reinterpret_cast<std::uint8_t**>(base + kBase_CommonData);
            if (!data)
                return false;

            outId = *reinterpret_cast<const std::uint16_t*>(
                data + (static_cast<std::size_t>(slot) + kRec_Bias) * kRec_Stride + kRec_HandleId);
            return outId != kInvalidMarkerId;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct TintDiag
    {
        void* component;
        void* utility;
        void* getFn;
        void* rgbFn;
    };

    bool ReadIconTargetSeh(void* self, void* icon, std::size_t offset, TintDiag& d)
    {
        __try
        {
            d.component = *reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(icon) + offset);
            if (!d.component)
                return false;

            auto base = *reinterpret_cast<std::uint8_t**>(
                static_cast<std::uint8_t*>(self) + kImpl_Base);
            if (!base)
                return false;

            d.utility = *reinterpret_cast<void**>(base + kBase_Utility);
            if (!d.utility)
                return false;

            auto uv = *reinterpret_cast<void***>(d.utility);
            if (!uv)
                return false;
            d.rgbFn = uv[kUtility_SetColourRgb / sizeof(void*)];

            auto cv = *reinterpret_cast<void***>(d.component);
            d.getFn = cv ? cv[kComponent_GetColour / sizeof(void*)] : nullptr;

            return d.rgbFn != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TintViaUtilitySeh(const TintDiag& d, const float rgb[3], float* before, float* after)
    {
        __try
        {
            auto setRgb = reinterpret_cast<UtilitySetRgb_t>(d.rgbFn);
            auto get    = reinterpret_cast<ComponentGetColour_t>(d.getFn);

            if (get)
                get(d.component, before);

            setRgb(d.utility, d.component, rgb[0], rgb[1], rgb[2]);

            if (get)
                get(d.component, after);

            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct MarkerGates
    {
        std::uint32_t raw;
        std::uint8_t  type;
        std::uint8_t  active;
        std::uint8_t  goal;
        std::uint8_t  marker;
        std::uint8_t  important;
        std::uint8_t  hidden;
    };

    bool ReadMarkerGatesSeh(void* self, std::uint32_t slot, MarkerGates& g)
    {
        __try
        {
            auto base = *reinterpret_cast<std::uint8_t**>(
                static_cast<std::uint8_t*>(self) + kImpl_Base);
            if (!base)
                return false;

            auto data = *reinterpret_cast<std::uint8_t**>(base + kBase_CommonData);
            if (!data)
                return false;

            auto vt = *reinterpret_cast<void***>(data);
            if (!vt)
                return false;

            g.raw = reinterpret_cast<DataGetU32_t>(
                vt[kData_GetIconType / sizeof(void*)])(data, slot);
            g.type      = static_cast<std::uint8_t>(g.raw & 0xFFu);
            g.active    = reinterpret_cast<DataGetU8_t>(
                vt[kData_IsActive / sizeof(void*)])(data, slot);
            g.goal      = reinterpret_cast<DataGetU8_t>(
                vt[kData_UseGoalIcon / sizeof(void*)])(data, slot);
            g.marker    = reinterpret_cast<DataGetU8_t>(
                vt[kData_UseMarkerIcon / sizeof(void*)])(data, slot);
            g.important = reinterpret_cast<DataGetU8_t>(
                vt[kData_UseImportant / sizeof(void*)])(data, slot);
            g.hidden    = reinterpret_cast<DataGetU8_t>(
                vt[kData_IsHidden / sizeof(void*)])(data, slot);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* SortListForType(std::uint8_t type)
    {
        switch (type)
        {
        case 0x11:
        case 0x13:
        case 0x31: return "AreaIcon 0x140F19B50";
        case 0x03: return "TargetMarkerIcon 0x140F1B4E0";
        case 0x08:
        case 0x2E:
        case 0x3D: return "FUN_140F1BDA0 (not hooked)";
        case 0x04:
        case 0x07:
        case 0x0B:
        case 0x0F:
        case 0x12:
        case 0x15:
        case 0x17:
        case 0x20:
        case 0x21:
        case 0x3C:
        case 0x42:
        case 0x43:
        case 0x44: return nullptr;
        default:   return "GeneralIcon 0x140F1A6B0";
        }
    }

    const char* SwitchDrawerForType(std::uint8_t type)
    {
        switch (type)
        {
        case 0x04: return "0x140F1BC50";
        case 0x0B: return "0x140F1C1C0";
        case 0x0F: return "0x140F1B260";
        case 0x12: return "0x140F1B780";
        case 0x15: return "0x140F1A180";
        case 0x20: return "0x140F1A020";
        case 0x3C: return "0x140F1C540";
        default:   return nullptr;
        }
    }

    void DescribeOwners(const MarkerGates& g, char* out, std::size_t cap)
    {
        char list[192];
        int  used = 0;

        list[0] = 0;

        if (g.active == 0 && g.goal == 0)
        {
            _snprintf_s(out, cap, _TRUNCATE,
                        "nothing (both the active and goal predicates are clear, so the"
                        " draw loop skips this slot outright)");
            return;
        }

        if (g.active != 0 && (g.raw & 0x80000000u) == 0
            && g.type >= 0x04 && g.type <= 0x3C)
        {
            if (const char* d = SwitchDrawerForType(g.type))
            {
                int n = _snprintf_s(list + used, sizeof(list) - used, _TRUNCATE,
                                    "%stype-switch drawer %s", used ? ", " : "", d);
                if (n > 0)
                    used += n;
            }
        }

        if (g.goal != 0 && (g.raw & 0x0000F000u) != 0)
        {
            int n = _snprintf_s(list + used, sizeof(list) - used, _TRUNCATE,
                                "%sGoalIcon", used ? ", " : "");
            if (n > 0)
                used += n;
        }

        if (g.marker != 0)
        {
            int n = _snprintf_s(list + used, sizeof(list) - used, _TRUNCATE,
                                "%sNewMarkerIcon", used ? ", " : "");
            if (n > 0)
                used += n;
        }

        if (g.important != 0 && g.hidden == 0)
        {
            int n = _snprintf_s(list + used, sizeof(list) - used, _TRUNCATE,
                                "%sImportantIcon", used ? ", " : "");
            if (n > 0)
                used += n;
        }

        if (const char* sorted = SortListForType(g.type))
        {
            int n = _snprintf_s(list + used, sizeof(list) - used, _TRUNCATE,
                                "%s%s", used ? ", " : "", sorted);
            if (n > 0)
                used += n;
        }

        if (used == 0)
            _snprintf_s(out, cap, _TRUNCATE,
                        "no drawer at all - the type is outside the 0x04-0x3C jump table, every"
                        " icon-family predicate is clear, and no pass-1 sort list claims it");
        else
            _snprintf_s(out, cap, _TRUNCATE, "%s", list);
    }

    void CensusMarkerTable(void* self, const char* which)
    {
        if (g_CensusCount >= kCensusLimit)
            return;

        unsigned      valid    = 0;
        unsigned      soldiers = 0;
        std::uint32_t sig      = 0;

        for (std::uint32_t s = 0; s < kMarkerSlotCount; ++s)
        {
            std::uint16_t id = kInvalidMarkerId;
            if (!ResolveSystemIdSeh(self, s, id))
                continue;

            ++valid;
            sig = sig * 31u + ((s << 16) | id);
        }

        if (valid == 0 || sig == g_LastCensus)
            return;

        g_LastCensus = sig;
        ++g_CensusCount;

        for (std::uint32_t s = 0; s < kMarkerSlotCount && soldiers < 8; ++s)
        {
            std::uint16_t id = kInvalidMarkerId;
            if (!ResolveSystemIdSeh(self, s, id))
                continue;
            if (id < kSoldierBandLow || id > kSoldierBandHigh)
                continue;

            ++soldiers;

            MarkerGates g{};
            if (!ReadMarkerGatesSeh(self, s, g))
            {
                Log("[HeadMarkMap] slot %u id 0x%04X: the icon-type and predicate reads"
                    " faulted, so this build's marker data does not lay out the way this"
                    " module expects\n", s, id);
                continue;
            }

            char owners[256];
            DescribeOwners(g, owners, sizeof(owners));

            Log("[HeadMarkMap] slot %u id 0x%04X: iconType=0x%02X raw=0x%08X"
                " active=%u goal=%u marker=%u important=%u hidden=%u -> drawn by %s\n",
                s, id, static_cast<unsigned>(g.type), g.raw,
                static_cast<unsigned>(g.active),
                static_cast<unsigned>(g.goal),
                static_cast<unsigned>(g.marker),
                static_cast<unsigned>(g.important),
                static_cast<unsigned>(g.hidden),
                owners);
        }

        Log("[HeadMarkMap] marker table while %s was drawing: %u slot(s) carry a marked"
            " entity, %u of them in the soldier band 0x0400-0x05FF (detailed above)."
            " A slot whose owner is a type-switch drawer is NOT one of the three this"
            " module hooks, which is why its colour stays vanilla\n",
            which, valid, soldiers);
    }

    void NoteEntry(unsigned family, const char* which, void* self, void* icon,
                   std::uint32_t mapIconId)
    {
        if (family >= 6 || g_Entered[family])
            return;

        g_Entered[family] = 1;
        Log("[HeadMarkMap] %s reached the detour (mapIconId 0x%08X self=%p icon=%p) - the drawer "
            "runs, so a missing tint after this is the colour path, not the hook\n",
            which, mapIconId, self, icon);
    }

    void TintForIcon(void* self, void* icon, std::uint32_t mapIconId, const char* which,
                     unsigned family, const std::size_t* nodes, unsigned nodeCount)
    {
        NoteEntry(family, which, self, icon, mapIconId);

        const bool probe = family < 6 && g_Probe[family] < kProbeLimit;

        if (!self || !icon)
        {
            if (probe)
            {
                ++g_Probe[family];
                Log("[HeadMarkMap] %s: the drawer ran with self=%p icon=%p, so there is no icon "
                    "component to colour\n", which, self, icon);
            }
            return;
        }

        if (IsOnlineSession())
        {
            if (probe)
            {
                ++g_Probe[family];
                Log("[HeadMarkMap] %s: treated as an online session, so the map icon keeps its "
                    "vanilla colour\n", which);
            }
            return;
        }

        const std::uint32_t slot = mapIconId & kMapIconSlotMask;

        if ((mapIconId & kMapIconCategoryMask) != kMapIconCategory)
        {
            if (family < 6 && g_NonMarker[family] < kNonMarkerLimit)
            {
                ++g_NonMarker[family];
                Log("[HeadMarkMap] %s: mapIconId 0x%08X is not a marker-slot id, so this icon "
                    "belongs to something other than a marked entity (counted separately so "
                    "these cannot use up the marker-slot diagnostics)\n",
                    which, mapIconId);
            }
            return;
        }

        CensusMarkerTable(self, which);

        std::uint16_t systemId = kInvalidMarkerId;
        if (!ResolveSystemIdSeh(self, slot, systemId))
        {
            if (family < 6 && g_NoId[family] < kNoIdLimit)
            {
                ++g_NoId[family];
                Log("[HeadMarkMap] %s slot %u: that marker slot carries no marked entity (handle"
                    " 0xFFFF), so this icon belongs to an objective rather than a marked"
                    " target\n", which, slot);
            }
            return;
        }

        const bool inBand = systemId >= kSoldierBandLow && systemId <= kSoldierBandHigh;

        float rgb[3] = {};
        int   reason = 0;
        if (!GetHeadMarkColourForSlot(systemId, slot, rgb, &reason))
        {
            if (probe && inBand)
            {
                ++g_Probe[family];
                char store[256];
                DescribeHeadMarkOverrides(systemId, store, sizeof(store));
                Log("[HeadMarkMap] %s slot %u id 0x%04X state %d: no colour to apply (reason %d; "
                    "30x = that state has no override, 40x = its palette colour is not resolved "
                    "yet) - map icon keeps vanilla. Override store: %s\n",
                    which, slot, systemId, reason % 100, reason, store);
            }
            return;
        }

        TintDiag d{};
        alignas(16) float before[4] = {};
        alignas(16) float after[4]  = {};
        unsigned          applied   = 0;

        char offsets[96];
        int  offsetsUsed = 0;

        offsets[0] = 0;

        for (unsigned i = 0; i < nodeCount; ++i)
        {
            TintDiag          node{};
            alignas(16) float nodeBefore[4] = {};
            alignas(16) float nodeAfter[4]  = {};

            if (!ReadIconTargetSeh(self, icon, nodes[i], node))
            {
                int n = _snprintf_s(offsets + offsetsUsed, sizeof(offsets) - offsetsUsed,
                                    _TRUNCATE, "%s+0x%02X:null",
                                    offsetsUsed ? " " : "", static_cast<unsigned>(nodes[i]));
                if (n > 0)
                    offsetsUsed += n;
                continue;
            }
            if (!TintViaUtilitySeh(node, rgb, nodeBefore, nodeAfter))
            {
                int n = _snprintf_s(offsets + offsetsUsed, sizeof(offsets) - offsetsUsed,
                                    _TRUNCATE, "%s+0x%02X:refused",
                                    offsetsUsed ? " " : "", static_cast<unsigned>(nodes[i]));
                if (n > 0)
                    offsetsUsed += n;
                continue;
            }

            {
                int n = _snprintf_s(offsets + offsetsUsed, sizeof(offsets) - offsetsUsed,
                                    _TRUNCATE, "%s+0x%02X:set",
                                    offsetsUsed ? " " : "", static_cast<unsigned>(nodes[i]));
                if (n > 0)
                    offsetsUsed += n;
            }

            if (applied == 0)
            {
                d = node;
                for (int c = 0; c < 4; ++c)
                {
                    before[c] = nodeBefore[c];
                    after[c]  = nodeAfter[c];
                }
            }
            ++applied;
        }

        const bool tinted = applied != 0;

        if (probe && inBand)
        {
            ++g_Probe[family];
            Log("[HeadMarkMap] %s slot %u id 0x%04X state %d: rgb %.3f %.3f %.3f -> %u of %u "
                "node(s) [%s] (first %p was %.2f %.2f %.2f a%.2f, now %.2f %.2f %.2f)\n",
                which, slot, systemId, reason - 100, rgb[0], rgb[1], rgb[2],
                applied, nodeCount, offsets, d.component,
                before[0], before[1], before[2], before[3],
                after[0], after[1], after[2]);
        }

        if (!tinted && !g_TintFailureLogged)
        {
            g_TintFailureLogged = true;
            Log("[HeadMarkDying] the iDroid map icon would not take a colour, so map markers keep "
                "their vanilla colour while head-marks still follow the override\n");
        }
    }

    void __fastcall hkDispOnNewMarkerIcon(void* self, void* icon, std::uint32_t mapIconId,
                                          std::uint8_t a, std::uint8_t b, std::uint8_t c)
    {
        if (g_Orig)
            g_Orig(self, icon, mapIconId, a, b, c);

        TintForIcon(self, icon, mapIconId, "NewMarkerIcon", 0, kMarkerNodes,
                    static_cast<unsigned>(std::size(kMarkerNodes)));
    }

    void __fastcall hkDispOnImportantIcon(void* self, void* icon, std::uint32_t mapIconId)
    {
        if (g_OrigImportant)
            g_OrigImportant(self, icon, mapIconId);

        TintForIcon(self, icon, mapIconId, "ImportantIcon", 1, kMarkerNodes,
                    static_cast<unsigned>(std::size(kMarkerNodes)));
    }

    void __fastcall hkDispOnGoalIcon(void* self, void* icon, std::uint32_t mapIconId,
                                     std::uint32_t a4, std::uint32_t a5,
                                     std::uint32_t a6, std::uint32_t a7)
    {
        if (g_OrigGoal)
            g_OrigGoal(self, icon, mapIconId, a4, a5, a6, a7);

        TintForIcon(self, icon, mapIconId, "GoalIcon", 2, kGoalNodes,
                    static_cast<unsigned>(std::size(kGoalNodes)));
    }

    bool ApplyRgbDirectSeh(void* utility, void* component, const float rgb[3])
    {
        __try
        {
            if (!utility || !component)
                return false;

            auto uv = *reinterpret_cast<void***>(utility);
            if (!uv)
                return false;

            auto setRgb = reinterpret_cast<UtilitySetRgb_t>(
                uv[kUtility_SetColourRgb / sizeof(void*)]);
            if (!setRgb)
                return false;

            setRgb(utility, component, rgb[0], rgb[1], rgb[2]);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ConvertSentinelGroup(void* utility, void* component, std::uint32_t group)
    {
        if ((group & kGroupSentinelMask) != kGroupSentinel)
            return false;

        const std::uint32_t slot = group & 0xFFu;
        if (slot < kMarkerSlotCount && g_SlotRgb[slot].live)
            ApplyRgbDirectSeh(utility, component, g_SlotRgb[slot].rgb);
        return true;
    }

    void __fastcall hkSetColourGroup(void* utility, void* component, std::uint32_t group)
    {
        if (ConvertSentinelGroup(utility, component, group))
            return;

        if (g_OrigSetGroup)
            g_OrigSetGroup(utility, component, group);
    }

    void __fastcall hkSetColourGroup2(void* utility, void* component, std::uint32_t group)
    {
        if (ConvertSentinelGroup(utility, component, group))
            return;

        if (g_OrigSetGroup2)
            g_OrigSetGroup2(utility, component, group);
    }

    void* ResolveUtilitySlotSeh(void* self, std::size_t slot)
    {
        __try
        {
            auto base = *reinterpret_cast<std::uint8_t**>(
                static_cast<std::uint8_t*>(self) + kImpl_Base);
            if (!base)
                return nullptr;

            auto utility = *reinterpret_cast<void**>(base + kBase_Utility);
            if (!utility)
                return nullptr;

            auto uv = *reinterpret_cast<void***>(utility);
            if (!uv)
                return nullptr;

            return uv[slot / sizeof(void*)];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void EnsureSetColourGroupHook(void* self)
    {
        if (g_SetGroupTried || !self)
            return;

        void* fn  = ResolveUtilitySlotSeh(self, kUtility_SetColourGroup);
        void* fn2 = ResolveUtilitySlotSeh(self, kUtility_SetColourGroup2);
        if (!fn)
            return;

        g_SetGroupTried = true;

        if (!CreateAndEnableHook(fn, &hkSetColourGroup,
                                 reinterpret_cast<void**>(&g_OrigSetGroup)))
        {
            Log("[HeadMarkDying] ERROR: the map icon palette setter at %p refused a hook, so a"
                " marked entity's map icon keeps the palette colour the engine picked\n", fn);
            return;
        }

        LogDebug("[HeadMarkDying] map icon palette setter armed at %p\n", fn);

        if (!fn2 || fn2 == fn)
        {
            LogDebug("[HeadMarkDying] the map icon important-target palette setter is the same"
                     " function as the general one, so one hook covers both\n");
        }
        else if (!CreateAndEnableHook(fn2, &hkSetColourGroup2,
                                      reinterpret_cast<void**>(&g_OrigSetGroup2)))
        {
            Log("[HeadMarkDying] ERROR: the important-target palette setter at %p refused a hook,"
                " so an important-target map icon keeps the engine's palette colour\n", fn2);
        }
        else
        {
            LogDebug("[HeadMarkDying] important-target palette setter armed at %p\n", fn2);
        }
    }

    bool WriteU32Seh(std::uint32_t* p, std::uint32_t value)
    {
        __try
        {
            if (!p)
                return false;
            *p = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadU32Seh(const std::uint32_t* p, std::uint32_t& out)
    {
        __try
        {
            if (!p)
                return false;
            out = *p;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadU64Seh(const std::uint64_t* p, std::uint64_t& out)
    {
        __try
        {
            if (!p)
                return false;
            out = *p;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ReportDisplayParam(void* self, std::uint32_t mapIconId, bool used,
                            const std::uint32_t* outColourGroup,
                            const std::uint64_t* outIconPath, bool substituted)
    {
        if ((mapIconId & kMapIconCategoryMask) != kMapIconCategory)
            return;
        if (g_ParamProbe >= kParamProbeLimit)
            return;

        const std::uint32_t slot     = mapIconId & kMapIconSlotMask;
        std::uint16_t       systemId = kInvalidMarkerId;
        const bool          haveId   = ResolveSystemIdSeh(self, slot, systemId);

        if (!haveId || systemId < kSoldierBandLow || systemId > kSoldierBandHigh)
            return;

        ++g_ParamProbe;

        MarkerGates g{};
        const bool  haveGates = ReadMarkerGatesSeh(self, slot, g);

        std::uint32_t group = 0;
        std::uint64_t path  = 0;
        ReadU32Seh(outColourGroup, group);
        ReadU64Seh(outIconPath, path);

        char store[256];
        store[0] = 0;
        if (haveId)
            DescribeHeadMarkOverrides(systemId, store, sizeof(store));

        Log("[HeadMarkMap] icon param: slot %u id 0x%04X iconType=0x%02X used=%u "
            "colourGroup=0x%08X iconPath=0x%016llX override=%s - the colour group is a palette "
            "id, so an override has to replace the group rather than write RGB over it. %s\n",
            slot,
            systemId,
            haveGates ? g.type : 0xFFu,
            used ? 1u : 0u,
            group,
            static_cast<unsigned long long>(path),
            substituted ? "SUBSTITUTED" : "none",
            store);
    }

    bool __fastcall hkGetIconDisplayParam(void* self, std::uint32_t mapIconId,
                                          std::uint32_t* outColourGroup,
                                          std::uint64_t* outIconPath,
                                          float* outScaleX, float* outScaleY,
                                          float* outAlpha, float* outParam)
    {
        bool used = false;
        if (g_OrigDisplayParam)
            used = g_OrigDisplayParam(self, mapIconId, outColourGroup, outIconPath,
                                      outScaleX, outScaleY, outAlpha, outParam);

        bool substituted = false;

        if ((mapIconId & kMapIconCategoryMask) == kMapIconCategory)
        {
            EnsureSetColourGroupHook(self);

            const std::uint32_t slot = mapIconId & kMapIconSlotMask;
            if (slot < kMarkerSlotCount)
            {
                float         rgb[3]   = {};
                int           reason   = 0;
                std::uint16_t systemId = kInvalidMarkerId;

                const bool have = !IsOnlineSession()
                                  && ResolveSystemIdSeh(self, slot, systemId)
                                  && GetHeadMarkColourForSlot(systemId, slot, rgb, &reason);

                if (have)
                {
                    SlotRgb& e = g_SlotRgb[slot];

                    const bool changed = !e.live
                                         || e.rgb[0] != rgb[0]
                                         || e.rgb[1] != rgb[1]
                                         || e.rgb[2] != rgb[2];
                    if (changed)
                        ++e.gen;

                    e.rgb[0] = rgb[0];
                    e.rgb[1] = rgb[1];
                    e.rgb[2] = rgb[2];
                    e.live   = true;

                    substituted = WriteU32Seh(
                        outColourGroup,
                        kGroupSentinel | (static_cast<std::uint32_t>(e.gen) << 8) | slot);
                }
                else
                {
                    g_SlotRgb[slot].live = false;
                }
            }
        }

        ReportDisplayParam(self, mapIconId, used, outColourGroup, outIconPath, substituted);
        return used;
    }

    void __fastcall hkDispOnAreaIcon(void* self, void* icon, std::uint32_t mapIconId,
                                     float a4)
    {
        if (g_OrigArea)
            g_OrigArea(self, icon, mapIconId, a4);

        TintForIcon(self, icon, mapIconId, "AreaIcon", 5, kAreaNodes,
                    static_cast<unsigned>(std::size(kAreaNodes)));
    }

    void __fastcall hkDispOnGeneralIcon(void* self, void* icon, std::uint32_t mapIconId,
                                        float a4)
    {
        if (g_OrigGeneral)
            g_OrigGeneral(self, icon, mapIconId, a4);

        TintForIcon(self, icon, mapIconId, "GeneralIcon", 3, kGeneralNodes,
                    static_cast<unsigned>(std::size(kGeneralNodes)));
    }

    void __fastcall hkDispOnTargetMarkerIcon(void* self, void* icon, std::uint32_t mapIconId,
                                             float a4)
    {
        if (g_OrigTargetMarker)
            g_OrigTargetMarker(self, icon, mapIconId, a4);

        TintForIcon(self, icon, mapIconId, "TargetMarkerIcon", 4, kTargetMarkerNodes,
                    static_cast<unsigned>(std::size(kTargetMarkerNodes)));
    }

    std::uint8_t* FollowThunkSeh(std::uint8_t* at)
    {
        if (!at)
            return nullptr;

        __try
        {
            for (int hop = 0; hop < 4; ++hop)
            {
                if (at[0] != kJumpRel32)
                    return at;

                const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(at + 1);
                at = at + 5 + rel;
            }
            return at;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool PrologueMatchesSeh(const std::uint8_t* at, const std::uint8_t* expect,
                            const std::uint8_t* mask, std::size_t len)
    {
        __try
        {
            for (std::size_t i = 0; i < len; ++i)
            {
                if (mask[i] && at[i] != expect[i])
                    return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace
{
    void __fastcall hkDisplayMarkerIcons(void* self, std::uintptr_t a2,
                                         std::uintptr_t a3, std::uintptr_t a4)
    {
        if (!g_DispatchNoted)
        {
            g_DispatchNoted = 1;
            Log("[HeadMarkMap] the map icon pass ran (self=%p) - from here on a census line names"
                " every marker slot that carries a marked entity, and silence from the drawers"
                " means that slot's icon is drawn by a function this module does not hook\n",
                self);
        }

        if (self && !IsOnlineSession())
            CensusMarkerTable(self, "the map icon pass");

        if (g_OrigDispatch)
            g_OrigDispatch(self, a2, a3, a4);
    }

    std::uint8_t* FindDispatcherSeh(std::uint8_t* anchor)
    {
        for (std::ptrdiff_t d = -kDispatchScanBack; d <= kDispatchScanForward; ++d)
        {
            std::uint8_t* at = anchor + d;
            if (PrologueMatchesSeh(at, kDispatchPrologue, kDispatchMask,
                                   sizeof(kDispatchPrologue)))
                return at;
        }
        return nullptr;
    }
}

bool Install_MbDvcMapCallbackIconImpl_DispOnNewMarkerIcon_Patch()
{
    auto anchor = reinterpret_cast<std::uint8_t*>(ResolveGameAddress(
        gAddr.MbDvcMapCallbackIconImpl_DispOnNewMarkerIcon));
    auto entry  = FollowThunkSeh(anchor);
    if (!entry)
    {
        LogDebug("[HeadMarkDying] no DispOnNewMarkerIcon address for this build - head-mark "
                 "colours apply in the world but iDroid map markers stay vanilla\n");
        return false;
    }

    if (!PrologueMatchesSeh(entry, kPrologue, kPrologueMask, sizeof(kPrologue)))
    {
        Log("[HeadMarkDying] the bytes at %p are not DispOnNewMarkerIcon on this build, so iDroid "
            "map markers stay vanilla\n", entry);
        return false;
    }

    g_IsFobMode = reinterpret_cast<IsFobMode_t>(
        ResolveGameAddress(gAddr.Barrier_IsFobMode));

    if (!CreateAndEnableHook(entry, &hkDispOnNewMarkerIcon, reinterpret_cast<void**>(&g_Orig)))
    {
        Log("[HeadMarkDying] ERROR: the DispOnNewMarkerIcon hook was refused at %p, so iDroid map "
            "markers stay vanilla\n", entry);
        return false;
    }

    auto important = FollowThunkSeh(reinterpret_cast<std::uint8_t*>(
        ResolveGameAddress(gAddr.MbDvcMapCallbackIconImpl_DispOnImportantIcon)));
    if (!important)
    {
        LogDebug("[HeadMarkDying] no DispOnImportantIcon address for this build - marked-enemy map "
                 "icons stay vanilla\n");
    }
    else if (!PrologueMatchesSeh(important, kImportantPrologue, kImportantMask,
                                 sizeof(kImportantPrologue)))
    {
        Log("[HeadMarkDying] the bytes at %p are not DispOnImportantIcon on this build, so "
            "marked-enemy map icons stay vanilla\n", important);
    }
    else if (!CreateAndEnableHook(important, &hkDispOnImportantIcon,
                                  reinterpret_cast<void**>(&g_OrigImportant)))
    {
        Log("[HeadMarkDying] ERROR: the DispOnImportantIcon hook was refused at %p, so "
            "marked-enemy map icons stay vanilla\n", important);
    }
    else
    {
        LogDebug("[HeadMarkDying] iDroid marked-enemy map icon armed at %p\n", important);
    }

    if constexpr (kInstallGoalIcon)
    {
        auto goal = FollowThunkSeh(reinterpret_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.MbDvcMapCallbackIconImpl_DispOnGoalIcon)));
        if (!goal)
        {
            LogDebug("[HeadMarkDying] no DispOnGoalIcon address for this build - objective-marker "
                     "map icons stay vanilla\n");
        }
        else if (!PrologueMatchesSeh(goal, kGoalPrologue, kGoalMask, sizeof(kGoalPrologue)))
        {
            Log("[HeadMarkDying] the bytes at %p are not DispOnGoalIcon on this build, so "
                "objective-marker map icons stay vanilla\n", goal);
        }
        else if (!CreateAndEnableHook(goal, &hkDispOnGoalIcon,
                                      reinterpret_cast<void**>(&g_OrigGoal)))
        {
            Log("[HeadMarkDying] ERROR: the DispOnGoalIcon hook was refused at %p, so "
                "objective-marker map icons stay vanilla\n", goal);
        }
        else
        {
            LogDebug("[HeadMarkDying] iDroid objective map icon armed at %p\n", goal);
        }
    }
    else
    {
        (void)&hkDispOnGoalIcon;
        (void)kGoalPrologue;
        (void)kGoalMask;
        LogDebug("[HeadMarkDying] the objective-marker map icon is deliberately left vanilla - it "
                 "shares a code region with the two hooked drawers and is held back until they "
                 "are confirmed firing\n");
    }

    auto general = FollowThunkSeh(reinterpret_cast<std::uint8_t*>(
        ResolveGameAddress(gAddr.MbDvcMapCallbackIconImpl_DispOnGeneralIcon)));
    if (!general)
    {
        LogDebug("[HeadMarkDying] no DispOnGeneralIcon address for this build - the map icons the"
                 " sort pass routes through IsUseGeneralIcon stay vanilla\n");
    }
    else if (!PrologueMatchesSeh(general, kGeneralPrologue, kGeneralMask,
                                 sizeof(kGeneralPrologue)))
    {
        Log("[HeadMarkDying] the bytes at %p are not DispOnGeneralIcon on this build, so the map"
            " icons routed through IsUseGeneralIcon stay vanilla\n", general);
    }
    else if (!CreateAndEnableHook(general, &hkDispOnGeneralIcon,
                                  reinterpret_cast<void**>(&g_OrigGeneral)))
    {
        Log("[HeadMarkDying] ERROR: the DispOnGeneralIcon hook was refused at %p, so the map icons"
            " routed through IsUseGeneralIcon stay vanilla\n", general);
    }
    else
    {
        LogDebug("[HeadMarkDying] iDroid general map icon armed at %p\n", general);
    }

    auto targetMarker = FollowThunkSeh(reinterpret_cast<std::uint8_t*>(
        ResolveGameAddress(gAddr.MbDvcMapCallbackIconImpl_DispOnTargetMarkerIcon)));
    if (!targetMarker)
    {
        LogDebug("[HeadMarkDying] no DispOnTargetMarkerIcon address for this build - icon type 3"
                 " map markers stay vanilla\n");
    }
    else if (!PrologueMatchesSeh(targetMarker, kTargetMarkerPrologue, kTargetMarkerMask,
                                 sizeof(kTargetMarkerPrologue)))
    {
        Log("[HeadMarkDying] the bytes at %p are not DispOnTargetMarkerIcon on this build, so icon"
            " type 3 map markers stay vanilla\n", targetMarker);
    }
    else if (!CreateAndEnableHook(targetMarker, &hkDispOnTargetMarkerIcon,
                                  reinterpret_cast<void**>(&g_OrigTargetMarker)))
    {
        Log("[HeadMarkDying] ERROR: the DispOnTargetMarkerIcon hook was refused at %p, so icon"
            " type 3 map markers stay vanilla\n", targetMarker);
    }
    else
    {
        LogDebug("[HeadMarkDying] iDroid type-3 map marker armed at %p\n", targetMarker);
    }

    auto displayParam = FollowThunkSeh(reinterpret_cast<std::uint8_t*>(
        ResolveGameAddress(gAddr.MbDvcMapCallbackIconImpl_GetIconDisplayParam)));
    if (!displayParam)
    {
        LogDebug("[HeadMarkDying] no GetIconDisplayParam address for this build - the per-icon"
                 " colour report is unavailable\n");
    }
    else if (!PrologueMatchesSeh(displayParam, kDisplayParamPrologue, kDisplayParamMask,
                                 sizeof(kDisplayParamPrologue)))
    {
        Log("[HeadMarkDying] the bytes at %p are not GetIconDisplayParam on this build, so the"
            " per-icon colour report is unavailable\n", displayParam);
    }
    else if (!CreateAndEnableHook(displayParam, &hkGetIconDisplayParam,
                                  reinterpret_cast<void**>(&g_OrigDisplayParam)))
    {
        Log("[HeadMarkDying] ERROR: the GetIconDisplayParam hook was refused at %p, so the"
            " per-icon colour report is unavailable\n", displayParam);
    }
    else
    {
        LogDebug("[HeadMarkDying] map icon colour source armed at %p\n", displayParam);
    }

    auto area = FollowThunkSeh(reinterpret_cast<std::uint8_t*>(
        ResolveGameAddress(gAddr.MbDvcMapCallbackIconImpl_DispOnAreaIcon)));
    if (!area)
    {
        LogDebug("[HeadMarkDying] no DispOnAreaIcon address for this build - the map icons the"
                 " sort pass routes through IsUseAreaIcon stay vanilla\n");
    }
    else if (!PrologueMatchesSeh(area, kAreaPrologue, kAreaMask, sizeof(kAreaPrologue)))
    {
        Log("[HeadMarkDying] the bytes at %p are not DispOnAreaIcon on this build, so the map"
            " icons routed through IsUseAreaIcon stay vanilla\n", area);
    }
    else if (!CreateAndEnableHook(area, &hkDispOnAreaIcon,
                                  reinterpret_cast<void**>(&g_OrigArea)))
    {
        Log("[HeadMarkDying] ERROR: the DispOnAreaIcon hook was refused at %p, so the map icons"
            " routed through IsUseAreaIcon stay vanilla\n", area);
    }
    else
    {
        LogDebug("[HeadMarkDying] iDroid area map icon armed at %p\n", area);
    }

    auto dispatch = FindDispatcherSeh(anchor);
    if (!dispatch)
    {
        Log("[HeadMarkDying] DisplayMarkerIcons was not found near %p, so a run where no map icon"
            " is coloured cannot say whether the map drew at all\n", entry);
    }
    else if (!CreateAndEnableHook(dispatch, &hkDisplayMarkerIcons,
                                  reinterpret_cast<void**>(&g_OrigDispatch)))
    {
        Log("[HeadMarkDying] ERROR: the DisplayMarkerIcons probe was refused at %p\n", dispatch);
    }
    else
    {
        LogDebug("[HeadMarkDying] map icon pass probe armed at %p\n", dispatch);
    }

    LogDebug("[HeadMarkDying] iDroid map marker colour armed at %p (single-player only)\n", entry);
    return true;
}

void Uninstall_MbDvcMapCallbackIconImpl_DispOnNewMarkerIcon_Patch()
{
    g_Orig          = nullptr;
    g_OrigImportant = nullptr;
    g_OrigGoal      = nullptr;
    g_OrigGeneral      = nullptr;
    g_OrigTargetMarker = nullptr;
    g_OrigArea         = nullptr;
    g_OrigDisplayParam = nullptr;
    g_OrigSetGroup     = nullptr;
    g_OrigSetGroup2    = nullptr;
    g_SetGroupTried    = false;
    g_OrigDispatch  = nullptr;
    g_IsFobMode     = nullptr;
}
