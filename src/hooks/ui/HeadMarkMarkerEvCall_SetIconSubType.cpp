#include "pch.h"
#include "HeadMarkMarkerEvCall_SetIconSubType.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "ActionCoreImpl_UpdateOptCamo.h"
#include "FoxHashes.h"
#include "MbDvcMapCallbackIconImpl_DispOnNewMarkerIcon.h"

namespace
{
    const std::uint8_t kConstBlock[] = {
        0x48, 0xB9, 0x3C, 0x18, 0x82, 0xCD, 0x2B, 0xD0, 0x00, 0x00,
        0x48, 0xB8, 0x97, 0x5D, 0x31, 0xC2, 0xA1, 0xDE, 0x00, 0x00,
        0x48, 0x89, 0x44, 0x24, 0x20,
        0x48, 0x89, 0x4C, 0x24, 0x28,
        0x48, 0x89, 0x4C, 0x24, 0x40,
    };

    const std::uint8_t kPrologue[] = {
        0x48, 0x85, 0xD2,
        0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x74, 0x24, 0x20,
        0x57,
        0x48, 0x83, 0xEC, 0x50,
    };

    const std::uint8_t kPrologueMask[] = {
        1, 1, 1,
        1, 1, 0, 0, 0, 0,
        1, 1, 1, 1, 1,
        1,
        1, 1, 1, 1,
    };

    constexpr std::size_t   kConstBlockToEntry   = 0x212;

    constexpr std::size_t   kMarker_Element      = 0xA8;
    constexpr std::size_t   kMarker_Type         = 0xBC;
    constexpr std::size_t   kMarker_SubType      = 0xC0;
    constexpr std::size_t   kMarker_Slot         = 0xC2;
    constexpr std::size_t   kEvCall_Manager      = 0xE0;
    constexpr std::size_t   kEvCall_CommonDataMgr= 0xD0;
    constexpr std::size_t   kMgr_UiMarkerData    = 0xF08;
    constexpr std::size_t   kRec_Stride          = 0x30;
    constexpr std::size_t   kRec_Bias            = 2;
    constexpr std::size_t   kRec_HandleId        = 0x18;
    constexpr std::uint32_t kMarkerSlotCount     = 0x80;
    constexpr std::size_t   kManager_SetColour   = 0x318;
    constexpr std::size_t   kSetColourScanLen    = 0x100;
    constexpr std::size_t   kVtblSlotLimit       = 0x1000;
    constexpr std::size_t   kNode_Colour         = 0x50;

    constexpr std::uint32_t kMarkerType_Soldier  = 0x01;
    constexpr std::uint32_t kMarkerType_Soldier2 = 0x33;
    constexpr std::uint32_t kMarkerType_Alt      = 0x05;
    constexpr std::uint32_t kMarkerType_Alt2     = 0x44;

    constexpr std::uint8_t  kSubType_Enemy       = 1;
    constexpr std::uint8_t  kSubType_Dying       = 4;
    constexpr std::uint8_t  kSubTypeCount        = 5;
    constexpr std::uint8_t  kSubType_NeverPainted= 5;
    constexpr std::uint32_t kSubTypeSelectBit    = 1u << 24;
    constexpr std::uint32_t kColour_EnemyDying   = 0xA79B1385;
    constexpr std::uint32_t kColour_ProbeA       = 0xC2315D97;
    constexpr std::uint32_t kColour_ProbeB       = 0xCD82183C;

    constexpr std::uint8_t  kMaxStops            = 6;
    constexpr float         kDefaultSpeed        = 1.0f;

    constexpr std::uint16_t kInvalidMarkerId     = 0xFFFFu;
    constexpr std::uint32_t kSoldierBand         = 0x0400u;
    constexpr std::uint32_t kSoldierBandMask     = 0xFE00u;
    constexpr std::uint32_t kSoldierIndexMask    = 0x01FFu;

    struct StateColour
    {
        std::uint8_t       count;
        bool               blend;
        float              speed;
        float              fade;
        HeadMarkColourStop stop[kMaxStops];
    };

    StateColour g_GlobalState[kSubTypeCount] = {};

    struct EntityColours
    {
        StateColour state[kSubTypeCount];
    };

    std::unordered_map<std::uint16_t, EntityColours> g_EntityColours;
    std::mutex                                       g_EntityMutex;

    void InitGlobalDefaults()
    {
        StateColour& dying = g_GlobalState[kSubType_Dying];
        dying.count            = 1;
        dying.blend            = true;
        dying.speed            = kDefaultSpeed;
        dying.fade             = 0.0f;
        dying.stop[0].isRgb    = false;
        dying.stop[0].paletteId= kColour_EnemyDying;
    }

    bool EntityWantsState(std::uint16_t systemId, std::uint8_t state)
    {
        if (systemId == kInvalidMarkerId)
            return false;
        std::lock_guard<std::mutex> lock(g_EntityMutex);
        auto it = g_EntityColours.find(systemId);
        return it != g_EntityColours.end() && it->second.state[state].count != 0;
    }

    bool LookupState(std::uint16_t systemId, std::uint8_t state, StateColour& out)
    {
        if (systemId != kInvalidMarkerId)
        {
            std::lock_guard<std::mutex> lock(g_EntityMutex);
            auto it = g_EntityColours.find(systemId);
            if (it != g_EntityColours.end() && it->second.state[state].count != 0)
            {
                out = it->second.state[state];
                return true;
            }
        }

        if (g_GlobalState[state].count == 0)
            return false;

        out = g_GlobalState[state];
        return true;
    }

    double NowSeconds()
    {
        static LARGE_INTEGER freq  = {};
        static LARGE_INTEGER start = {};
        if (freq.QuadPart == 0)
        {
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&start);
        }
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        if (freq.QuadPart == 0)
            return 0.0;
        return static_cast<double>(now.QuadPart - start.QuadPart) /
               static_cast<double>(freq.QuadPart);
    }

    constexpr std::size_t   kStatus_Stride       = 0x28;
    constexpr std::size_t   kStatus_Flags        = 0x20;
    constexpr std::uint32_t kStatus_DyingBit     = 0x80000000u;

    using SetIconSubType_t = void(__fastcall*)(void* self, void* marker, std::uint32_t flags);
    using SetColour_t      = void(__fastcall*)(void* self, void* element, std::uint32_t colour);
    using SetColourRgb_t   = void(__fastcall*)(void* self, void* element, float r, float g, float b);
    using IsFobMode_t      = bool(__fastcall*)();

    SetIconSubType_t g_Orig      = nullptr;
    IsFobMode_t      g_IsFobMode = nullptr;

    bool g_ColourFailureLogged   = false;
    bool g_RgbProbeDone          = false;
    bool g_HashProbeDone         = false;
    int  g_Readback              = 0;
    struct SlotState
    {
        bool   painted;
        bool   hasSubType;
        std::uint8_t subType;
        float  lastFade;
        bool   fading;
        double fadeStart;
        float  fadeDur;
        float  from[3];
        float  to[3];
        bool   toValid;
    };

    SlotState g_Slot[kMarkerSlotCount] = {};

    struct PaletteRgb
    {
        float r;
        float g;
        float b;
    };

    std::unordered_map<std::uint32_t, PaletteRgb> g_PaletteRgb;
    std::size_t g_RgbSlot        = 0;
    bool g_IdentityFailureLogged = false;

    constexpr std::uint32_t kNoSoldierIndex     = 0xFFFFFFFFu;


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

    struct Marker
    {
        std::uint32_t type;
        std::uint16_t systemId;
        std::uint8_t  slot;
        std::uint8_t  subType;
        bool          ok;
    };

    Marker ReadMarkerSeh(void* marker)
    {
        Marker m{};
        __try
        {
            auto base = static_cast<const std::uint8_t*>(marker);
            m.type    = *reinterpret_cast<const std::uint32_t*>(base + kMarker_Type);
            m.subType = *(base + kMarker_SubType);
            m.slot    = *(base + kMarker_Slot);
            m.ok      = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            m.ok = false;
        }
        return m;
    }

    bool ForceRepaintNextFrameSeh(void* marker)
    {
        __try
        {
            *(static_cast<std::uint8_t*>(marker) + kMarker_SubType) = kSubType_NeverPainted;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadSubTypeSeh(void* marker, std::uint8_t& outSubType)
    {
        __try
        {
            outSubType = *(static_cast<const std::uint8_t*>(marker) + kMarker_SubType);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadStatusFlagsSeh(std::uint64_t address, std::uint32_t& outFlags)
    {
        __try
        {
            outFlags = *reinterpret_cast<const std::uint32_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ResolveMarkerSystemIdSeh(void* self, std::uint8_t slot, std::uint16_t& outId)
    {
        if (slot >= kMarkerSlotCount)
            return false;
        __try
        {
            auto mgr = *reinterpret_cast<std::uint8_t**>(
                static_cast<std::uint8_t*>(self) + kEvCall_CommonDataMgr);
            if (!mgr)
                return false;

            auto data = *reinterpret_cast<std::uint8_t**>(mgr + kMgr_UiMarkerData);
            if (!data)
                return false;

            outId = *reinterpret_cast<const std::uint16_t*>(
                data + (static_cast<std::size_t>(slot) + kRec_Bias) * kRec_Stride + kRec_HandleId);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::uint32_t SoldierIndexFromSystemId(std::uint16_t systemId)
    {
        if (systemId == kInvalidMarkerId)
            return kNoSoldierIndex;
        if ((systemId & kSoldierBandMask) != kSoldierBand)
            return kNoSoldierIndex;
        return systemId & kSoldierIndexMask;
    }

    bool IsIndexDying(std::uint32_t index)
    {
        std::uint64_t base  = 0;
        std::uint32_t count = 0;
        if (!GetSoldierStatusArray(base, count))
            return false;
        if (index >= count)
            return false;

        std::uint32_t flags = 0;
        if (!ReadStatusFlagsSeh(base + index * kStatus_Stride + kStatus_Flags, flags))
            return false;

        return (flags & kStatus_DyingBit) != 0;
    }

    bool IsSoldierDying(std::uint16_t systemId)
    {
        std::uint64_t base  = 0;
        std::uint32_t count = 0;
        if (!GetSoldierStatusArray(base, count))
            return false;

        const std::uint32_t index = SoldierIndexFromSystemId(systemId);
        if (index == kNoSoldierIndex)
        {
            if (!g_IdentityFailureLogged)
            {
                g_IdentityFailureLogged = true;
                Log("[HeadMarkDying] a soldier head-mark carries marker-system id 0x%04X, which is "
                    "no soldier index among %u soldiers - that marker keeps the vanilla enemy "
                    "colour while dying\n",
                    systemId, count);
            }
            return false;
        }
        return IsIndexDying(index);
    }

    bool ResolveTargetsSeh(void* self, void* marker, void*& outManager, void*& outElement,
                           void**& outVtbl)
    {
        __try
        {
            outManager =
                *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kEvCall_Manager);
            if (!outManager)
                return false;

            outElement =
                *reinterpret_cast<void**>(static_cast<std::uint8_t*>(marker) + kMarker_Element);
            if (!outElement)
                return false;

            outVtbl = *reinterpret_cast<void***>(outManager);
            return outVtbl != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool AddressInMainModuleSeh(const void* address)
    {
        static std::uint8_t* base = nullptr;
        static std::size_t   size = 0;

        if (!base)
        {
            base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!base)
                return false;
            __try
            {
                auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                auto nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                size     = nt->OptionalHeader.SizeOfImage;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                size = 0;
            }
        }

        if (!size)
            return false;

        auto p = reinterpret_cast<const std::uint8_t*>(address);
        return p >= base && p < base + size;
    }

    bool DeriveRgbSlotSeh(void** vtbl, std::size_t& outSlot)
    {
        __try
        {
            auto fn = reinterpret_cast<const std::uint8_t*>(vtbl[kManager_SetColour / sizeof(void*)]);
            if (!fn)
                return false;

            std::size_t found = 0;
            unsigned    hits  = 0;

            for (std::size_t i = 0; i + 6 <= kSetColourScanLen; ++i)
            {
                if (fn[i] != 0xFF)
                    continue;

                const std::uint8_t modrm = fn[i + 1];
                if ((modrm & 0x38) != 0x10)
                    continue;
                if ((modrm & 0xC0) != 0x80)
                    continue;
                if ((modrm & 0x07) == 0x04)
                    continue;

                const std::uint32_t disp = *reinterpret_cast<const std::uint32_t*>(fn + i + 2);
                if (disp < 0x10 || disp >= kVtblSlotLimit || (disp & 7) != 0)
                    continue;

                void* target = vtbl[disp / sizeof(void*)];
                if (!target || target == reinterpret_cast<const void*>(fn))
                    continue;
                if (!AddressInMainModuleSeh(target))
                    continue;

                if (++hits == 1)
                    found = disp;
            }

            if (hits != 1)
                return false;

            outSlot = found;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool EnsureRgbSlot(void** vtbl)
    {
        if (g_RgbProbeDone)
            return g_RgbSlot != 0;

        g_RgbProbeDone = true;
        std::size_t slot = 0;
        if (!DeriveRgbSlotSeh(vtbl, slot))
        {
            Log("[HeadMarkDying] the raw-colour setter could not be read out of the palette "
                "setter, so head-marks given an r/g/b colour keep the vanilla colour - use "
                "palette colour names instead on this build\n");
            return false;
        }

        g_RgbSlot = slot;
        LogDebug("[HeadMarkDying] raw-colour setter derived at manager vtable +0x%zX\n", slot);
        return true;
    }

    bool ReadElementColourSeh(void* element, float out[4])
    {
        __try
        {
            auto c = reinterpret_cast<const float*>(
                static_cast<std::uint8_t*>(element) + kNode_Colour);
            for (int i = 0; i < 4; ++i)
            {
                const float v = c[i];
                if (!(v >= -0.001f && v <= 1.001f))
                    return false;
                out[i] = v;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool EnsureColourReadback(void* manager, void** vtbl, void* element);

    bool ApplyPaletteSeh(void* manager, void** vtbl, void* element, std::uint32_t colour)
    {
        __try
        {
            auto setColour = reinterpret_cast<SetColour_t>(vtbl[kManager_SetColour / sizeof(void*)]);
            if (!setColour)
                return false;
            setColour(manager, element, colour);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ApplyRgbSeh(void* manager, void** vtbl, void* element, float r, float g, float b)
    {
        if (!EnsureRgbSlot(vtbl))
            return false;

        __try
        {
            auto setRgb = reinterpret_cast<SetColourRgb_t>(vtbl[g_RgbSlot / sizeof(void*)]);
            if (!setRgb)
                return false;
            setRgb(manager, element, r, g, b);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    HeadMarkColourStop SampleState(const StateColour& sc)
    {
        if (sc.count <= 1)
            return sc.stop[0];

        const double speed = (sc.speed > 0.0f) ? static_cast<double>(sc.speed)
                                               : static_cast<double>(kDefaultSpeed);
        const double ring  = NowSeconds() * speed * static_cast<double>(sc.count);
        const double floorRing = (ring >= 0.0) ? static_cast<double>(static_cast<long long>(ring))
                                               : 0.0;

        const std::uint8_t index = static_cast<std::uint8_t>(
            static_cast<unsigned long long>(floorRing) % sc.count);
        const std::uint8_t next  = static_cast<std::uint8_t>((index + 1) % sc.count);

        const HeadMarkColourStop& from = sc.stop[index];
        if (!sc.blend || !from.isRgb || !sc.stop[next].isRgb)
            return from;

        const HeadMarkColourStop& to = sc.stop[next];
        const float t = static_cast<float>(ring - floorRing);

        HeadMarkColourStop out{};
        out.isRgb = true;
        out.r = from.r + (to.r - from.r) * t;
        out.g = from.g + (to.g - from.g) * t;
        out.b = from.b + (to.b - from.b) * t;
        return out;
    }

    bool EnsureColourReadback(void* manager, void** vtbl, void* element)
    {
        if (g_Readback != 0)
            return g_Readback == 1;

        float a[4] = {};
        float b[4] = {};

        const bool sampled =
            ApplyPaletteSeh(manager, vtbl, element, kColour_ProbeA) &&
            ReadElementColourSeh(element, a) &&
            ApplyPaletteSeh(manager, vtbl, element, kColour_ProbeB) &&
            ReadElementColourSeh(element, b);

        if (sampled && (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]))
        {
            g_Readback = 1;
            LogDebug("[HeadMarkDying] head-mark colour read-back confirmed at element +0x%zX\n",
                kNode_Colour);
            return true;
        }

        g_Readback = 2;
        Log("[HeadMarkDying] the head-mark element does not carry its colour where this build "
            "expects it, so colours still apply but never fade between states\n");
        return false;
    }

    bool ResolveTargetRgbSeh(void* manager, void** vtbl, void* element, const StateColour& sc,
                             float out[3], bool& applied, bool& rgbKnown)
    {
        applied  = false;
        rgbKnown = false;

        const HeadMarkColourStop stop = SampleState(sc);

        if (stop.isRgb)
        {
            out[0] = stop.r;
            out[1] = stop.g;
            out[2] = stop.b;
            rgbKnown = true;
            return true;
        }

        if (!ApplyPaletteSeh(manager, vtbl, element, stop.paletteId))
            return false;

        applied = true;

        float c[4] = {};
        if (ReadElementColourSeh(element, c))
        {
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            rgbKnown = true;

            PaletteRgb& cached = g_PaletteRgb[stop.paletteId];
            cached.r = c[0];
            cached.g = c[1];
            cached.b = c[2];
        }
        return true;
    }

    void __fastcall hkSetIconSubType(void* self, void* marker, std::uint32_t flags)
    {
        if (!self || !marker || IsOnlineSession())
        {
            if (g_Orig)
                g_Orig(self, marker, flags);
            return;
        }

        const Marker before = ReadMarkerSeh(marker);
        if (!before.ok)
        {
            if (g_Orig)
                g_Orig(self, marker, flags);
            return;
        }

        void*  manager = nullptr;
        void*  element = nullptr;
        void** vtbl    = nullptr;
        const bool haveTargets = ResolveTargetsSeh(self, marker, manager, element, vtbl);

        float      outgoing[4] = {};
        const bool outgoingOk  = haveTargets && g_Readback == 1 &&
                                 ReadElementColourSeh(element, outgoing);

        std::uint16_t systemId = kInvalidMarkerId;
        ResolveMarkerSystemIdSeh(self, before.slot, systemId);

        if (before.type == kMarkerType_Soldier || before.type == kMarkerType_Soldier2 ||
            before.type == kMarkerType_Alt     || before.type == kMarkerType_Alt2)
        {
            flags &= ~kSubTypeSelectBit;

            const bool dyingWanted = g_GlobalState[kSubType_Dying].count != 0 ||
                                     EntityWantsState(systemId, kSubType_Dying);

            if (dyingWanted &&
                (before.type == kMarkerType_Soldier || before.type == kMarkerType_Soldier2) &&
                systemId != kInvalidMarkerId &&
                IsSoldierDying(systemId))
            {
                flags |= kSubTypeSelectBit;
            }
        }

        if (g_Orig)
            g_Orig(self, marker, flags);

        if (!haveTargets || before.slot >= kMarkerSlotCount)
            return;

        std::uint8_t after = 0;
        if (!ReadSubTypeSeh(marker, after))
            return;

        if (after >= kSubTypeCount)
            return;

        SlotState&  slot = g_Slot[before.slot];
        slot.hasSubType = true;
        slot.subType    = after;

        StateColour sc{};
        const bool  have = LookupState(systemId, after, sc);

        if (after != before.subType)
        {
            const float duration = have ? sc.fade : slot.lastFade;

            slot.fading  = false;
            slot.toValid = false;

            if (duration > 0.0f && outgoingOk && EnsureRgbSlot(vtbl))
            {
                slot.fading    = true;
                slot.fadeStart = NowSeconds();
                slot.fadeDur   = duration;
                slot.from[0]   = outgoing[0];
                slot.from[1]   = outgoing[1];
                slot.from[2]   = outgoing[2];

                if (!have)
                {
                    float vanilla[4] = {};
                    if (ReadElementColourSeh(element, vanilla))
                    {
                        slot.to[0]   = vanilla[0];
                        slot.to[1]   = vanilla[1];
                        slot.to[2]   = vanilla[2];
                        slot.toValid = true;
                    }
                    else
                    {
                        slot.fading = false;
                    }
                }
            }
        }

        if (!have && !slot.fading)
        {
            if (slot.painted)
            {
                slot.painted = false;
                ForceRepaintNextFrameSeh(marker);
            }
            slot.lastFade = 0.0f;
            return;
        }

        if (have && sc.count == 1 && !slot.fading &&
            after == before.subType && slot.painted)
            return;

        EnsureColourReadback(manager, vtbl, element);

        float target[3] = {};
        bool  applied   = false;
        bool  rgbKnown  = false;
        bool  targetOk  = false;

        if (have)
        {
            targetOk = ResolveTargetRgbSeh(manager, vtbl, element, sc, target, applied, rgbKnown);
        }
        else if (slot.toValid)
        {
            target[0] = slot.to[0];
            target[1] = slot.to[1];
            target[2] = slot.to[2];
            rgbKnown  = true;
            targetOk  = true;
        }

        if (!targetOk)
        {
            slot.fading = false;
            if (!have && slot.painted)
            {
                slot.painted = false;
                ForceRepaintNextFrameSeh(marker);
            }
            if (!g_ColourFailureLogged)
            {
                g_ColourFailureLogged = true;
                Log("[HeadMarkDying] the head-mark colour setter could not be reached, so marker "
                    "state %u keeps its vanilla colour\n", static_cast<unsigned>(after));
            }
            return;
        }

        if (slot.fading && !rgbKnown)
            slot.fading = false;

        if (slot.fading)
        {
            const double elapsed = NowSeconds() - slot.fadeStart;
            const double t       = (slot.fadeDur > 0.0f)
                                 ? elapsed / static_cast<double>(slot.fadeDur)
                                 : 1.0;

            if (t >= 1.0)
            {
                slot.fading = false;
            }
            else
            {
                const float f = static_cast<float>(t < 0.0 ? 0.0 : t);
                float mixed[3];
                for (int i = 0; i < 3; ++i)
                    mixed[i] = slot.from[i] + (target[i] - slot.from[i]) * f;

                if (ApplyRgbSeh(manager, vtbl, element, mixed[0], mixed[1], mixed[2]))
                {
                    slot.painted  = true;
                    slot.lastFade = have ? sc.fade : 0.0f;
                    return;
                }
                slot.fading = false;
            }
        }

        bool painted = applied;
        if (!painted && rgbKnown)
            painted = ApplyRgbSeh(manager, vtbl, element, target[0], target[1], target[2]);

        if (painted)
        {
            slot.painted  = true;
            slot.lastFade = have ? sc.fade : 0.0f;
        }
        else if (!g_ColourFailureLogged)
        {
            g_ColourFailureLogged = true;
            Log("[HeadMarkDying] the head-mark colour setter could not be reached, so marker "
                "state %u keeps its vanilla colour\n", static_cast<unsigned>(after));
        }
    }

    struct Region
    {
        std::uint8_t* base;
        std::size_t   size;
    };

    bool CollectExecutableRegionsSeh(std::vector<Region>& out)
    {
        auto base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base)
            return false;

        __try
        {
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return false;

            auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return false;

            auto sec = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            {
                if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                    continue;
                if (sec->Misc.VirtualSize < sizeof(kConstBlock))
                    continue;
                out.push_back(Region{ base + sec->VirtualAddress, sec->Misc.VirtualSize });
            }
            return !out.empty();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ScanRegionSeh(const Region& region, std::vector<std::uint8_t*>& out)
    {
        __try
        {
            if (region.size < sizeof(kConstBlock))
                return;
            const std::size_t last = region.size - sizeof(kConstBlock);
            for (std::size_t i = 0; i <= last; ++i)
            {
                if (region.base[i] != kConstBlock[0])
                    continue;
                if (std::memcmp(region.base + i, kConstBlock, sizeof(kConstBlock)) != 0)
                    continue;
                out.push_back(region.base + i);
                i += sizeof(kConstBlock) - 1;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool PrologueMatchesSeh(std::uint8_t* at)
    {
        __try
        {
            for (std::size_t i = 0; i < sizeof(kPrologue); ++i)
            {
                if (kPrologueMask[i] && at[i] != kPrologue[i])
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

bool Install_HeadMarkMarkerEvCall_SetIconSubType_Patch()
{
    std::vector<Region> regions;
    if (!CollectExecutableRegionsSeh(regions))
    {
        Log("[HeadMarkDying] ERROR: no executable section could be read, so the dying-soldier "
            "marker colour is off\n");
        return false;
    }

    std::vector<std::uint8_t*> sites;
    for (const Region& region : regions)
        ScanRegionSeh(region, sites);

    if (sites.size() != 1)
    {
        Log("[HeadMarkDying] the marker colour table matched %zu places instead of 1, so "
            "SetIconSubType could not be located and dying soldiers keep the enemy colour\n",
            sites.size());
        return false;
    }

    std::uint8_t* entry = sites[0] - kConstBlockToEntry;
    if (!PrologueMatchesSeh(entry))
    {
        Log("[HeadMarkDying] the bytes at %p do not match the SetIconSubType prologue, so dying "
            "soldiers keep the enemy colour\n", entry);
        return false;
    }

    InitGlobalDefaults();

    g_IsFobMode = reinterpret_cast<IsFobMode_t>(
        ResolveGameAddress(gAddr.Barrier_IsFobMode));
    if (!g_IsFobMode)
        Log("[HeadMarkDying] no IsFobMode address for this build - the dying colour falls back to "
            "the mission-code gate alone to stay out of FOB\n");

    if (!CreateAndEnableHook(entry, &hkSetIconSubType, reinterpret_cast<void**>(&g_Orig)))
    {
        Log("[HeadMarkDying] ERROR: the SetIconSubType hook was refused at %p, so dying soldiers "
            "keep the enemy colour\n", entry);
        return false;
    }

    Install_MbDvcMapCallbackIconImpl_DispOnNewMarkerIcon_Patch();

    LogDebug("[HeadMarkDying] dying-soldier marker colour armed at %p (single-player only)\n", entry);
    return true;
}

void Uninstall_HeadMarkMarkerEvCall_SetIconSubType_Patch()
{
    Uninstall_MbDvcMapCallbackIconImpl_DispOnNewMarkerIcon_Patch();
    g_Orig = nullptr;
    g_IsFobMode = nullptr;
}


std::uint32_t ResolveHeadMarkColourId(const char* name)
{
    if (!name || !*name)
        return 0;

    if (!g_HashProbeDone)
    {
        g_HashProbeDone = true;
        const std::uint32_t probe =
            static_cast<std::uint32_t>(FoxHashes::StrCode64("cmn-col-marker-enemy-dying") & 0xFFFFFFFFull);
        if (probe != kColour_EnemyDying)
        {
            Log("[HeadMarkDying] StrCode64 low32 gives 0x%08X for cmn-col-marker-enemy-dying but the "
                "palette uses 0x%08X - colour NAMES will not resolve on this build, pass the raw "
                "low 32 bits of the palette hash instead\n", probe, kColour_EnemyDying);
        }
    }

    return static_cast<std::uint32_t>(FoxHashes::StrCode64(name) & 0xFFFFFFFFull);
}


void DescribeHeadMarkOverrides(std::uint16_t systemId, char* out, std::size_t cap)
{
    if (!out || cap == 0)
        return;
    out[0] = 0;

    unsigned entityStates = 0;
    unsigned globalStates = 0;
    std::size_t entities   = 0;
    bool        haveEntity = false;

    {
        std::lock_guard<std::mutex> lock(g_EntityMutex);
        entities = g_EntityColours.size();
        auto it  = g_EntityColours.find(systemId);
        if (it != g_EntityColours.end())
        {
            haveEntity = true;
            for (unsigned i = 0; i < kSubTypeCount; ++i)
                if (it->second.state[i].count != 0)
                    entityStates |= (1u << i);
        }
    }

    for (unsigned i = 0; i < kSubTypeCount; ++i)
        if (g_GlobalState[i].count != 0)
            globalStates |= (1u << i);

    _snprintf_s(out, cap, _TRUNCATE,
                "%zu entit%s carry an override; this one is %s (its state mask 0x%X);"
                " the global state mask is 0x%X",
                entities, entities == 1 ? "y" : "ies",
                haveEntity ? "registered" : "NOT registered",
                entityStates, globalStates);
}

bool GetHeadMarkColourForSlot(std::uint16_t systemId, unsigned slot, float outRgb[3],
                              int* outReason)
{
    const auto fail = [outReason](int reason) { if (outReason) *outReason = reason; return false; };

    if (outReason)
        *outReason = 0;

    if (slot >= kMarkerSlotCount)
        return fail(1);

    const SlotState& s = g_Slot[slot];

    std::uint8_t state;
    if (s.hasSubType)
        state = s.subType;
    else
        state = IsSoldierDying(systemId) ? kSubType_Dying : kSubType_Enemy;

    if (state >= kSubTypeCount)
        return fail(2);

    if (outReason)
        *outReason = 100 + state;

    StateColour sc{};
    if (!LookupState(systemId, state, sc))
        return fail(300 + state);

    const HeadMarkColourStop stop = SampleState(sc);

    float target[3];
    if (stop.isRgb)
    {
        target[0] = stop.r;
        target[1] = stop.g;
        target[2] = stop.b;
    }
    else
    {
        auto it = g_PaletteRgb.find(stop.paletteId);
        if (it == g_PaletteRgb.end())
            return fail(400 + state);
        target[0] = it->second.r;
        target[1] = it->second.g;
        target[2] = it->second.b;
    }

    if (s.fading && s.fadeDur > 0.0f)
    {
        const double t = (NowSeconds() - s.fadeStart) / static_cast<double>(s.fadeDur);
        if (t < 1.0)
        {
            const float f = static_cast<float>(t < 0.0 ? 0.0 : t);
            for (int i = 0; i < 3; ++i)
                target[i] = s.from[i] + (target[i] - s.from[i]) * f;
        }
    }

    outRgb[0] = target[0];
    outRgb[1] = target[1];
    outRgb[2] = target[2];
    return true;
}


bool SetHeadMarkEntityColour(std::uint32_t gameObjectId, int state,
                             const HeadMarkColourStop* stops, unsigned count,
                             float speed, bool blend, float fade)
{
    if (state >= static_cast<int>(kSubTypeCount))
        return false;

    const std::uint16_t key = static_cast<std::uint16_t>(gameObjectId);
    if (key == kInvalidMarkerId)
        return false;

    if (count > kMaxStops)
        count = kMaxStops;
    if (!stops)
        count = 0;

    StateColour built{};
    built.count = static_cast<std::uint8_t>(count);
    built.blend = blend;
    built.speed = (speed > 0.0f) ? speed : kDefaultSpeed;
    built.fade  = (fade  > 0.0f) ? fade  : 0.0f;
    for (unsigned i = 0; i < count; ++i)
        built.stop[i] = stops[i];

    std::lock_guard<std::mutex> lock(g_EntityMutex);

    if (count == 0 && state < 0)
    {
        g_EntityColours.erase(key);
        return true;
    }

    EntityColours& e = g_EntityColours[key];

    if (state < 0)
    {
        for (std::uint8_t i = 0; i < kSubTypeCount; ++i)
            e.state[i] = built;
        return true;
    }

    e.state[state] = built;

    for (std::uint8_t i = 0; i < kSubTypeCount; ++i)
        if (e.state[i].count != 0)
            return true;

    g_EntityColours.erase(key);
    return true;
}


void Clear_HeadMarkEntityColours()
{
    std::lock_guard<std::mutex> lock(g_EntityMutex);
    g_EntityColours.clear();

    for (std::uint8_t i = 0; i < kMarkerSlotCount; ++i)
        g_Slot[i] = SlotState{};
}
