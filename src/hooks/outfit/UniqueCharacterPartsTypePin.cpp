#include "pch.h"

#include "UniqueCharacterPartsTypePin.h"
#include "OutfitAbilities.h"

#include "OutfitRegistry.h"
#include "ShadowState.h"
#include "MissionCodeGuard.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

static inline bool V_WriteOutfitPin(std::uint8_t p, std::uint8_t c,
                                  std::uint8_t pt)
{
    return outfit::WriteLivePlayerOutfit(
        p, c, pt, outfit::OutfitWriteSource::Pin);
}

namespace
{
    constexpr std::size_t kSiteOffset   = 0xAE2;
    constexpr std::size_t kResumeOffset = 0xB01;
    constexpr std::size_t kSiteLength   = 0x1F;

    constexpr std::size_t kTrampSize = 0x400;
    constexpr std::size_t kReadyOff  = 0xFE;
    constexpr std::size_t kArmedOff  = 0xFF;
    constexpr std::size_t kTableOff  = 0x100;
    constexpr std::size_t kCodeBOff  = 0x200;
    constexpr std::size_t kMaskOff   = 0x300;

    constexpr std::size_t kSiteBLength = 8;
    constexpr std::size_t kBcdDelta    = 0x4E;

    constexpr std::uint8_t kPinBandStart = 0x00;
    constexpr std::uint8_t kPinBandEnd   = outfit::kCustomPartsTypeEnd;

    constexpr std::size_t kBandWidth =
        static_cast<std::size_t>(kPinBandEnd)
      - static_cast<std::size_t>(kPinBandStart) + 1;

    static_assert(kTableOff + kBandWidth <= kTrampSize,
        "support table overflows the trampoline allocation");
    static_assert(kTableOff + kBandWidth <= kCodeBOff,
        "support table overruns the quietMovement camo code");
    static_assert(kArmedOff + 1 == kTableOff,
        "the armed flag must sit immediately below the table base");
    static_assert(kReadyOff + 1 == kArmedOff,
        "the ready flag must sit immediately below the armed flag");
    static_assert(kMaskOff + kBandWidth <= kTrampSize,
        "quietMovement mask table overflows the trampoline allocation");

    constexpr std::uint8_t kSupportsOcelot = 0x01;
    constexpr std::uint8_t kSupportsQuiet  = 0x02;
    constexpr std::uint8_t kQuietMoveOcelot = 0x04;
    constexpr std::uint8_t kQuietMoveQuiet  = 0x08;

    constexpr std::uint8_t kOcelotPinParts = 0x1A;
    constexpr std::uint8_t kOcelotPinCamo  = 0x73;
    constexpr std::uint8_t kQuietPinParts  = 0x1B;
    constexpr std::uint8_t kQuietPinCamo   = 0x74;

    struct RememberedOutfit
    {
        std::atomic<std::uint8_t> partsType{ 0 };
        std::atomic<std::uint8_t> selectorCode{ 0 };
    };

    RememberedOutfit g_Remembered[2];

    int SlotIndex(std::uint8_t playerType)
    {
        if (playerType == outfit::kPlayerType_Ocelot) return 0;
        if (playerType == outfit::kPlayerType_Quiet)  return 1;
        return -1;
    }

    const std::uint8_t kSiteExpect[kSiteLength] = {
        0x3C, 0x05,
        0x75, 0x07,
        0x40, 0xB7, 0x1A,
        0xB3, 0x73,
        0xEB, 0x09,
        0x3C, 0x06,
        0x75, 0x09,
        0x40, 0xB7, 0x1B,
        0xB3, 0x74,
        0x88, 0x5C, 0x24, 0x44,
        0x41, 0x88, 0x9D, 0xF9, 0x00, 0x00, 0x00
    };

    const std::uint8_t kSiteBExpect[kSiteBLength] = {
        0x3C, 0x06,
        0x74, 0x4A,
        0x40, 0x80, 0xFF, 0x1B
    };

    void*         g_SiteB = nullptr;
    std::uint8_t  g_OrigBytesB[kSiteBLength] = {};

    bool          g_Active = false;
    std::uint8_t* g_Tramp  = nullptr;
    void*         g_Site   = nullptr;
    std::uint8_t  g_OrigBytes[kSiteLength] = {};

    void* AllocExecNear(std::uintptr_t nearAddr, std::size_t size)
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
}

namespace uniquecharpin
{
    bool IsOwnSuitPartsType(std::uint8_t playerType, std::uint8_t partsType)
    {
        if (playerType == outfit::kPlayerType_Ocelot)
            return partsType == kOcelotPinParts;
        if (playerType == outfit::kPlayerType_Quiet)
            return partsType == kQuietPinParts;
        return false;
    }

    void SyncSupport(std::uint8_t livePartsType, std::uint8_t liveSelector,
                     bool bypass)
    {
        if (!g_Active || !g_Tramp) return;

        g_Tramp[kArmedOff] = bypass ? std::uint8_t{0} : std::uint8_t{1};

        std::uint8_t ready = 0;
        if (livePartsType >= outfit::kCustomPartsTypeStart
         && livePartsType <= outfit::kCustomPartsTypeEnd)
        {
            for (std::size_t i = 0; i < outfit::shadow::kMaxSlots; ++i)
            {
                outfit::shadow::Slot ss{};
                if (outfit::shadow::Get(i, &ss) && ss.used
                 && ss.realPartsType == livePartsType)
                {
                    ready = 1;
                    break;
                }
            }
        }
        g_Tramp[kReadyOff] = ready;

        const auto mark = [](const outfit::OutfitEntry* entry)
        {
            if (!entry) return;
            if (entry->partsType < outfit::kCustomPartsTypeStart
             || entry->partsType > outfit::kCustomPartsTypeEnd) return;

            std::uint8_t mask = 0;
            if (entry->IsPlayerTypeSupported(outfit::kPlayerType_Ocelot))
                mask |= kSupportsOcelot;
            if (entry->IsPlayerTypeSupported(outfit::kPlayerType_Quiet))
                mask |= kSupportsQuiet;

            if (const outfit::OutfitPlayerTypeData* od =
                    entry->GetPTData(outfit::kPlayerType_Ocelot))
                if (od->suitParamKind != 0) mask |= kQuietMoveOcelot;
            if (const outfit::OutfitPlayerTypeData* qd =
                    entry->GetPTData(outfit::kPlayerType_Quiet))
                if (qd->suitParamKind != 0) mask |= kQuietMoveQuiet;

            std::uint8_t moveMask = 0;
            for (std::uint8_t pt = 0; pt < outfit::kPlayerTypeMax; ++pt)
            {
                if (pt == outfit::kPlayerType_Quiet) continue;
                if (!entry->IsPlayerTypeSupported(pt)) continue;
                const outfit::OutfitPlayerTypeData* d = entry->GetPTData(pt);
                if (d && d->suitParamKind != 0)
                    moveMask |= static_cast<std::uint8_t>(1u << pt);
            }
            g_Tramp[kMaskOff + entry->partsType
                  - kPinBandStart] = moveMask;
            g_Tramp[kTableOff + entry->partsType
                  - kPinBandStart] = mask;
        };

        if (livePartsType >= outfit::kCustomPartsTypeStart
         && livePartsType <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByPartsType(livePartsType, &entry))
                mark(entry);
            else
                g_Tramp[kTableOff + livePartsType
                      - kPinBandStart] = 0;
        }

        if (livePartsType != 0
         && livePartsType < outfit::kCustomPartsTypeStart
         && outfit::VanillaExtHasAnyAbilities())
        {
            std::uint8_t support  = 0;
            std::uint8_t moveMask = 0;
            outfit::VanillaExtPinFlags pf{};
            if (outfit::VanillaExtGetPinFlags(livePartsType, &pf))
            {
                if (pf.supported[outfit::kPlayerType_Ocelot])
                    support |= kSupportsOcelot;
                if (pf.supported[outfit::kPlayerType_Quiet])
                    support |= kSupportsQuiet;
                if (pf.quietMove[outfit::kPlayerType_Ocelot])
                    support |= kQuietMoveOcelot;
                if (pf.quietMove[outfit::kPlayerType_Quiet])
                    support |= kQuietMoveQuiet;
                for (std::uint8_t pt = 0; pt < outfit::kPlayerType_Ocelot; ++pt)
                    if (pf.quietMove[pt])
                        moveMask |= static_cast<std::uint8_t>(1u << pt);
                if (moveMask != 0)
                    g_Tramp[kReadyOff] = 1;
            }
            g_Tramp[kMaskOff  + livePartsType - kPinBandStart] = moveMask;
            g_Tramp[kTableOff + livePartsType - kPinBandStart] = support;
        }

        if (liveSelector >= outfit::kCustomSelectorStart
         && liveSelector <= outfit::kCustomSelectorEnd)
        {
            const outfit::OutfitEntry* entry = nullptr;
            std::uint8_t variantIdx = 0;
            if (outfit::TryGetOutfitByVariantSelector(liveSelector, &entry,
                                                      &variantIdx))
                mark(entry);
        }
    }


    void RememberCustom(std::uint8_t playerType, std::uint8_t partsType,
                        std::uint8_t selectorCode)
    {
        const int s = SlotIndex(playerType);
        if (s < 0) return;
        if (partsType < outfit::kCustomPartsTypeStart
         || partsType > outfit::kCustomPartsTypeEnd) return;
        g_Remembered[s].partsType.store(partsType, std::memory_order_relaxed);
        g_Remembered[s].selectorCode.store(selectorCode,
                                           std::memory_order_relaxed);
    }

    bool TryGetRemembered(std::uint8_t playerType,
                          std::uint8_t* outPartsType,
                          std::uint8_t* outSelectorCode)
    {
        const int s = SlotIndex(playerType);
        if (s < 0) return false;
        const std::uint8_t p =
            g_Remembered[s].partsType.load(std::memory_order_relaxed);
        if (p < outfit::kCustomPartsTypeStart
         || p > outfit::kCustomPartsTypeEnd) return false;
        if (outPartsType) *outPartsType = p;
        if (outSelectorCode)
            *outSelectorCode =
                g_Remembered[s].selectorCode.load(std::memory_order_relaxed);
        return true;
    }

    void ForgetCustom(std::uint8_t playerType)
    {
        const int s = SlotIndex(playerType);
        if (s < 0) return;
        g_Remembered[s].partsType.store(0, std::memory_order_relaxed);
        g_Remembered[s].selectorCode.store(0, std::memory_order_relaxed);
    }

    void ReassertAfterRestore()
    {
        if (MissionCodeGuard::ShouldBypassHooks()) return;

        const std::uint8_t pt = outfit::ReadLivePlayerType();
        const int s = SlotIndex(pt);
        if (s < 0) return;

        const std::uint8_t want =
            g_Remembered[s].partsType.load(std::memory_order_relaxed);
        if (want == 0) return;

        const std::uint8_t live = outfit::ReadLivePartsType();
        if (live == want) return;

        const std::uint8_t pinParts =
            (pt == outfit::kPlayerType_Ocelot) ? kOcelotPinParts : kQuietPinParts;
        if (live != pinParts) return;

        const outfit::OutfitEntry* entry = nullptr;
        if (!outfit::TryGetOutfitByPartsType(want, &entry) || !entry
            || !entry->IsPlayerTypeSupported(pt))
        {
            ForgetCustom(pt);
            return;
        }

        const std::uint8_t sel =
            g_Remembered[s].selectorCode.load(std::memory_order_relaxed);
        outfit::NoteOwnSuitOverwrittenByPin(
            live, outfit::ReadLiveSelectorCode(), pt);
        V_WriteOutfitPin(want, sel, pt);

        for (std::size_t i = 0; i < outfit::shadow::kMaxSlots; ++i)
        {
            outfit::shadow::Slot ss;
            if (!outfit::shadow::Get(i, &ss)) continue;
            if (ss.realPartsType == want && ss.realPlayerType == pt) continue;
            ss.used           = true;
            ss.realPartsType  = want;
            ss.realCamoType   = sel;
            ss.realPlayerType = pt;
            ss.developId      = entry->developId;
            outfit::shadow::Set(i, ss);
        }

        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 16)
            LogDebug("[UniqueCharPin] the suit had fallen back to the pinned "
                     "0x%02X - restored the selected outfit 0x%02X/0x%02X for "
                     "player type %u (the mission-start restore rebuilds the "
                     "player as DDMale and never re-applies their suit)\n",
                static_cast<unsigned>(live), static_cast<unsigned>(want),
                static_cast<unsigned>(sel), static_cast<unsigned>(pt));
    }

    bool Install()
    {
        if (g_Active) return true;

        const std::uintptr_t ups = reinterpret_cast<std::uintptr_t>(
            ResolveGameAddress(gAddr.UpdatePartsStatus));
        if (!ups)
        {
            LogDebug("[UniqueCharPin] UpdatePartsStatus unresolved - Ocelot and Quiet "
                     "keep the engine's pinned suit and custom outfits cannot apply\n");
            return false;
        }

        const std::uintptr_t site   = ups + kSiteOffset;
        const std::uintptr_t resume = ups + kResumeOffset;

        bool ok = false;
        __try { ok = (std::memcmp(reinterpret_cast<void*>(site),
                                  kSiteExpect, kSiteLength) == 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok)
        {
            LogDebug("[UniqueCharPin] site bytes mismatch @%p - this build pins the "
                     "unique-character suit elsewhere, so custom Ocelot and Quiet "
                     "outfits stay unapplied\n", reinterpret_cast<void*>(site));
            return false;
        }

        std::uint8_t* tr =
            reinterpret_cast<std::uint8_t*>(AllocExecNear(site, kTrampSize));
        if (!tr)
        {
            Log("[UniqueCharPin] trampoline alloc failed - custom Ocelot and Quiet "
                "outfits stay unapplied\n");
            return false;
        }
        std::memset(tr, 0xCC, kTrampSize);
        std::memset(tr + kReadyOff, 0x00, 2 + kBandWidth);

        std::size_t o = 0;
        const auto emit = [&](std::initializer_list<std::uint8_t> b)
        { for (std::uint8_t x : b) tr[o++] = x; };
        const auto emitImm32 = [&](std::uint32_t v)
        { std::memcpy(tr + o, &v, 4); o += 4; };

        emit({0x51});
        emit({0x52});
        emit({0x48, 0x8D, 0x15});
        {
            const std::int32_t rel = static_cast<std::int32_t>(
                static_cast<std::int64_t>(
                    reinterpret_cast<std::uintptr_t>(tr) + kTableOff)
              - (reinterpret_cast<std::int64_t>(tr)
                 + static_cast<std::int64_t>(o) + 4));
            emitImm32(static_cast<std::uint32_t>(rel));
        }
        emit({0x80, 0x7A, 0xFF, 0x00});
        emit({0x74, 0x57});

        emit({0x40, 0x0F, 0xB6, 0xCF});
        emit({0x81, 0xE9});
        emitImm32(kPinBandStart);
        emit({0x81, 0xF9});
        emitImm32(static_cast<std::uint32_t>(kBandWidth - 1));
        emit({0x77, 0x14});
        emit({0x0F, 0xB6, 0x0C, 0x0A});
        emit({0x3C, 0x06});
        emit({0x75, 0x05});
        emit({0xF6, 0xC1, kSupportsQuiet});
        emit({0xEB, 0x03});
        emit({0xF6, 0xC1, kSupportsOcelot});
        emit({0x74, 0x02});
        emit({0xEB, 0x43});

        emit({0x41, 0x0F, 0xB6, 0x8D, 0xF8, 0x00, 0x00, 0x00});
        emit({0x81, 0xE9});
        emitImm32(kPinBandStart);
        emit({0x81, 0xF9});
        emitImm32(static_cast<std::uint32_t>(kBandWidth - 1));
        emit({0x77, 0x1B});
        emit({0x0F, 0xB6, 0x0C, 0x0A});
        emit({0x3C, 0x06});
        emit({0x75, 0x05});
        emit({0xF6, 0xC1, kSupportsQuiet});
        emit({0xEB, 0x03});
        emit({0xF6, 0xC1, kSupportsOcelot});
        emit({0x74, 0x09});
        emit({0x41, 0x8A, 0xBD, 0xF8, 0x00, 0x00, 0x00});
        emit({0xEB, 0x12});

        emit({0x3C, 0x06});
        emit({0x75, 0x07});
        emit({0x40, 0xB7, 0x1B});
        emit({0xB3, 0x74});
        emit({0xEB, 0x05});
        emit({0x40, 0xB7, 0x1A});
        emit({0xB3, 0x73});
        emit({0xEB, 0x14});

        emit({0x3C, 0x06});
        emit({0x75, 0x09});
        emit({0xF6, 0xC1, kQuietMoveQuiet});
        emit({0x74, 0x0B});
        emit({0xB3, kQuietPinCamo});
        emit({0xEB, 0x07});
        emit({0xF6, 0xC1, kQuietMoveOcelot});
        emit({0x74, 0x02});
        emit({0xB3, kQuietPinCamo});

        emit({0x5A});
        emit({0x59});
        emit({0x88, 0x5C, 0x24, 0x44});
        emit({0x41, 0x88, 0x9D, 0xF9, 0x00, 0x00, 0x00});
        emit({0xE9});
        {
            const std::int32_t rel = static_cast<std::int32_t>(
                static_cast<std::int64_t>(resume)
              - (reinterpret_cast<std::int64_t>(tr)
                 + static_cast<std::int64_t>(o) + 4));
            emitImm32(static_cast<std::uint32_t>(rel));
        }

        if (o > kReadyOff)
        {
            Log("[UniqueCharPin] trampoline code overruns the support table - "
                "custom Ocelot and Quiet outfits stay unapplied\n");
            VirtualFree(tr, 0, MEM_RELEASE);
            return false;
        }

        const std::uintptr_t siteB = reinterpret_cast<std::uintptr_t>(
            ResolveGameAddress(gAddr.UpdatePartsStatus_QuietCamoStrip));
        bool okB = false;
        if (siteB)
        {
            __try { okB = (std::memcmp(reinterpret_cast<void*>(siteB),
                                       kSiteBExpect, kSiteBLength) == 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { okB = false; }
        }

        if (okB)
        {
            const std::uintptr_t bcd    = siteB + kBcdDelta;
            const std::uintptr_t resumB = siteB + kSiteBLength;
            std::size_t b = kCodeBOff;
            const auto eb = [&](std::initializer_list<std::uint8_t> v)
            { for (std::uint8_t x : v) tr[b++] = x; };
            const auto ebRel = [&](std::uintptr_t target)
            {
                const std::int32_t r = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(target)
                  - (reinterpret_cast<std::int64_t>(tr)
                     + static_cast<std::int64_t>(b) + 4));
                std::memcpy(tr + b, &r, 4); b += 4;
            };

            eb({0x3C, 0x06});
            eb({0x0F, 0x84}); ebRel(bcd);
            eb({0x80, 0x3D});
            {
                const std::int32_t r = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(
                        reinterpret_cast<std::uintptr_t>(tr) + kArmedOff)
                  - (reinterpret_cast<std::int64_t>(tr)
                     + static_cast<std::int64_t>(b) + 5));
                std::memcpy(tr + b, &r, 4); b += 4;
            }
            eb({0x00});
            eb({0x74, 0x52});
            eb({0x80, 0x3D});
            {
                const std::int32_t r = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(
                        reinterpret_cast<std::uintptr_t>(tr) + kReadyOff)
                  - (reinterpret_cast<std::int64_t>(tr)
                     + static_cast<std::int64_t>(b) + 5));
                std::memcpy(tr + b, &r, 4); b += 4;
            }
            eb({0x00});
            eb({0x74, 0x49});
            eb({0x51});
            eb({0x52});
            eb({0x41, 0x0F, 0xB6, 0x8D, 0xF8, 0x00, 0x00, 0x00});
            eb({0x81, 0xE9});
            { const std::uint32_t v = kPinBandStart;
              std::memcpy(tr + b, &v, 4); b += 4; }
            eb({0x81, 0xF9});
            { const std::uint32_t v =
                  static_cast<std::uint32_t>(kBandWidth - 1);
              std::memcpy(tr + b, &v, 4); b += 4; }
            eb({0x77, 0x2F});
            eb({0x48, 0x8D, 0x15});
            {
                const std::int32_t r = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(
                        reinterpret_cast<std::uintptr_t>(tr) + kMaskOff)
                  - (reinterpret_cast<std::int64_t>(tr)
                     + static_cast<std::int64_t>(b) + 4));
                std::memcpy(tr + b, &r, 4); b += 4;
            }
            eb({0x0F, 0xB6, 0x0C, 0x0A});
            eb({0x0F, 0xB6, 0xD0});
            eb({0x0F, 0xA3, 0xD1});
            eb({0x73, 0x1C});
            eb({0x5A});
            eb({0x59});
            eb({0x41, 0x8A, 0xBD, 0xF8, 0x00, 0x00, 0x00});
            eb({0xB3, kQuietPinCamo});
            eb({0x41, 0xC6, 0x85, 0xF9, 0x00, 0x00, 0x00, kQuietPinCamo});
            eb({0x88, 0x5C, 0x24, 0x44});
            eb({0xE9}); ebRel(bcd);
            eb({0x5A});
            eb({0x59});
            eb({0x40, 0x80, 0xFF, 0x1B});
            eb({0xE9}); ebRel(resumB);

            if (b > kMaskOff)
            {
                Log("[UniqueCharPin] quietMovement camo code overruns its mask "
                    "table - quietMovement stays limited to Ocelot and Quiet\n");
            }
            else
            {
                const std::int64_t brel =
                    reinterpret_cast<std::int64_t>(tr + kCodeBOff)
                  - (static_cast<std::int64_t>(siteB) + 5);
                if (brel >= INT32_MIN && brel <= INT32_MAX)
                {
                    std::uint8_t pb[kSiteBLength];
                    std::memset(pb, 0x90, kSiteBLength);
                    pb[0] = 0xE9;
                    const std::int32_t br = static_cast<std::int32_t>(brel);
                    std::memcpy(pb + 1, &br, 4);
                    DWORD oldb = 0;
                    if (VirtualProtect(reinterpret_cast<void*>(siteB),
                            kSiteBLength, PAGE_EXECUTE_READWRITE, &oldb))
                    {
                        std::memcpy(g_OrigBytesB,
                            reinterpret_cast<void*>(siteB), kSiteBLength);
                        std::memcpy(reinterpret_cast<void*>(siteB), pb,
                            kSiteBLength);
                        DWORD tb = 0;
                        VirtualProtect(reinterpret_cast<void*>(siteB),
                            kSiteBLength, oldb, &tb);
                        FlushInstructionCache(GetCurrentProcess(),
                            reinterpret_cast<void*>(siteB), kSiteBLength);
                        g_SiteB = reinterpret_cast<void*>(siteB);
                    }
                }
            }
        }
        else
        {
            LogDebug("[UniqueCharPin] the Quiet camo strip site is unresolved or does "
                     "not match on this build - quietMovement works only for Ocelot "
                     "and Quiet; other player types keep their own camo and get no "
                     "borrowed abilities\n");
        }

        auto abandonInstall = [&](void* tramp)
        {
            if (g_SiteB)
            {
                DWORD ob = 0;
                if (VirtualProtect(g_SiteB, kSiteBLength,
                                   PAGE_EXECUTE_READWRITE, &ob))
                {
                    std::memcpy(g_SiteB, g_OrigBytesB, kSiteBLength);
                    DWORD tb = 0;
                    VirtualProtect(g_SiteB, kSiteBLength, ob, &tb);
                    FlushInstructionCache(GetCurrentProcess(),
                                          g_SiteB, kSiteBLength);
                    g_SiteB = nullptr;
                }
                else
                {
                    Log("[UniqueCharPin] site B could not be restored, so "
                        "the trampoline is LEAKED rather than freed - the "
                        "patched jump still points into it\n");
                    g_SiteB = nullptr;
                    return;
                }
            }
            VirtualFree(tramp, 0, MEM_RELEASE);
        };

        const std::int64_t jrel =
            reinterpret_cast<std::int64_t>(tr)
          - (static_cast<std::int64_t>(site) + 5);
        if (jrel < INT32_MIN || jrel > INT32_MAX)
        {
            LogDebug("[UniqueCharPin] trampoline too far for rel32 - custom Ocelot "
                     "and Quiet outfits stay unapplied\n");
            abandonInstall(tr);
            return false;
        }

        std::uint8_t patch[kSiteLength];
        std::memset(patch, 0x90, kSiteLength);
        patch[0] = 0xE9;
        const std::int32_t jr = static_cast<std::int32_t>(jrel);
        std::memcpy(patch + 1, &jr, 4);

        DWORD oldp = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), kSiteLength,
                            PAGE_EXECUTE_READWRITE, &oldp))
        {
            Log("[UniqueCharPin] VirtualProtect failed - custom Ocelot and Quiet "
                "outfits stay unapplied\n");
            abandonInstall(tr);
            return false;
        }
        std::memcpy(g_OrigBytes, reinterpret_cast<void*>(site), kSiteLength);
        std::memcpy(reinterpret_cast<void*>(site), patch, kSiteLength);
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<void*>(site), kSiteLength, oldp, &tmp);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(site), kSiteLength);

        g_Tramp  = tr;
        g_Site   = reinterpret_cast<void*>(site);
        g_Active = true;

#ifdef _DEBUG
        LogDebug("[UniqueCharPin] installed: site=%p tramp=%p table=%p - "
                 "UpdatePartsStatus rewrites the live partsType and camo to "
                 "0x1A/0x73 (Ocelot) or 0x1B/0x74 (Quiet) every tick; the pin now "
                 "yields when the live partsType is a registered custom outfit for "
                 "that character; an outfit that declares quietMovement also keeps "
                 "the engine's own camo byte, so identity-keyed systems still read "
                 "the character while the parts type stays custom\n",
            reinterpret_cast<void*>(site), tr, tr + kTableOff);
#endif
        return true;
    }

    void Uninstall()
    {
        if (g_SiteB)
        {
            DWORD oldb = 0;
            if (VirtualProtect(g_SiteB, kSiteBLength,
                               PAGE_EXECUTE_READWRITE, &oldb))
            {
                std::memcpy(g_SiteB, g_OrigBytesB, kSiteBLength);
                DWORD tb = 0;
                VirtualProtect(g_SiteB, kSiteBLength, oldb, &tb);
                FlushInstructionCache(GetCurrentProcess(), g_SiteB,
                                      kSiteBLength);
            }
            g_SiteB = nullptr;
        }
        if (!g_Active) return;

        DWORD oldp = 0;
        if (VirtualProtect(g_Site, kSiteLength, PAGE_EXECUTE_READWRITE, &oldp))
        {
            std::memcpy(g_Site, g_OrigBytes, kSiteLength);
            DWORD tmp = 0;
            VirtualProtect(g_Site, kSiteLength, oldp, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_Site, kSiteLength);
        }
        if (g_Tramp) VirtualFree(g_Tramp, 0, MEM_RELEASE);

        g_Tramp  = nullptr;
        g_Site   = nullptr;
        g_Active = false;
    }
}
