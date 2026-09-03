#include "pch.h"

#include "FobPlayerCharacters.h"

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "log.h"
#include "../outfit/OutfitRegistry.h"

namespace
{
    constexpr std::size_t kMaxPatchLen = 8;

    struct PatchSite
    {
        const char*         name;
        const char*         explanation;
        const std::uint8_t* pattern;
        const char*         mask;
        std::size_t         patternLen;
        std::size_t         patchOffset;
        std::size_t         patchLen;
        bool                relativeJump;

        std::uint8_t*       site;
        std::uint8_t        original[kMaxPatchLen];
        bool                applied;
    };

    const std::uint8_t kUniqueCharaListGatePattern[] = {
        0x48, 0x8B, 0x07,
        0x48, 0x8B, 0xCF,
        0xFF, 0x90, 0xF0, 0x04, 0x00, 0x00,
        0x84, 0xC0,
        0x0F, 0x84
    };
    const char kUniqueCharaListGateMask[] = "xxxxxxxxxxxxxxxx";

    const std::uint8_t kPartsStatusResetPattern[] = {
        0x8D, 0x48, 0xFB,
        0x80, 0xF9, 0x01,
        0x0F, 0x87, 0x00, 0x00, 0x00, 0x00,
        0x8B, 0x8E, 0x00, 0x00, 0x00, 0x00,
        0xC1, 0xE9, 0x00,
        0xF6, 0xC1, 0x00,
        0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
        0xF6, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,
        0x32, 0xC0
    };
    const char kPartsStatusResetMask[] =
        "xxxxxxxx????xx????xx?xx?xx????xx?????xx????xx";

    PatchSite g_Sites[] = {
        {
            "unique-character list gate",
            "the character list only offers Ocelot and Quiet while the "
            "S_IS_SORTIE_PREPARATION game status is set",
            kUniqueCharaListGatePattern, kUniqueCharaListGateMask,
            sizeof(kUniqueCharaListGatePattern), 6, 6, false,
            nullptr, {}, false
        },
        {
            "parts-status player type reset",
            "UpdatePartsStatus rewrites player type 5 and 6 back to Snake "
            "every tick outside FOB",
            kPartsStatusResetPattern, kPartsStatusResetMask,
            sizeof(kPartsStatusResetPattern), 37, 6, true,
            nullptr, {}, false
        },
    };

    bool MatchesAt(const std::uint8_t* at, const std::uint8_t* pattern,
                   const char* mask, std::size_t len)
    {
        for (std::size_t i = 0; i < len; ++i)
        {
            if (mask[i] == '?') continue;
            if (at[i] != pattern[i]) return false;
        }
        return true;
    }

    bool ScanRange(const std::uint8_t* begin, const std::uint8_t* end,
                   const std::uint8_t* pattern, const char* mask,
                   std::size_t len, std::uint8_t** hit, int* hitCount)
    {
        if (static_cast<std::size_t>(end - begin) < len)
            return true;

        const std::uint8_t  first = pattern[0];
        const std::uint8_t* p     = begin;
        const std::uint8_t* last  = end - len;

        while (p <= last)
        {
            const void* found =
                std::memchr(p, first, static_cast<std::size_t>(last - p) + 1);
            if (!found) break;
            p = static_cast<const std::uint8_t*>(found);
            if (MatchesAt(p, pattern, mask, len))
            {
                if (*hitCount == 0) *hit = const_cast<std::uint8_t*>(p);
                if (++(*hitCount) > 1) return false;
            }
            ++p;
        }
        return true;
    }

    std::uint8_t* FindUniqueCode(const std::uint8_t* pattern, const char* mask,
                                 std::size_t len, const char* what)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base) return nullptr;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        const auto* sec = IMAGE_FIRST_SECTION(nt);
        std::uint8_t* hit  = nullptr;
        int           hits = 0;

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        {
            if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
            const DWORD size = sec->Misc.VirtualSize
                ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            if (size == 0) continue;

            std::uint8_t* begin = base + sec->VirtualAddress;
            if (!ScanRange(begin, begin + size, pattern, mask, len, &hit, &hits))
                break;
        }

        if (hits != 1)
        {
            LogDebug("[FobChars] %s: %s in this build - that part is skipped\n",
                what,
                hits == 0 ? "no matching code found" : "more than one match");
            return nullptr;
        }
        return hit;
    }

    bool WriteBytes(void* target, const std::uint8_t* src, std::size_t size)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            LogDebug("[FobChars] VirtualProtect failed at %p (err=%lu)\n",
                target, GetLastError());
            return false;
        }
        std::memcpy(target, src, size);
        DWORD restored = 0;
        VirtualProtect(target, size, oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, size);
        return true;
    }

    bool ApplySite(PatchSite& s)
    {
        if (s.applied) return true;
        if (!s.site)
        {
            std::uint8_t* hit =
                FindUniqueCode(s.pattern, s.mask, s.patternLen, s.name);
            if (!hit)
            {
                LogDebug("[FobChars] %s not patched - Ocelot and Quiet stay FOB "
                    "only\n", s.name);
                return false;
            }
            s.site = hit + s.patchOffset;
        }

        std::memcpy(s.original, s.site, s.patchLen);

        std::uint8_t patch[kMaxPatchLen] = {};
        if (s.relativeJump)
        {
            std::int32_t rel = 0;
            std::memcpy(&rel, s.site + 2, sizeof(rel));
            const std::int32_t jmpRel = rel + 1;
            patch[0] = 0xE9;
            std::memcpy(patch + 1, &jmpRel, sizeof(jmpRel));
            patch[5] = 0x90;
        }
        else
        {
            patch[0] = 0xB0;
            patch[1] = 0x01;
            for (std::size_t i = 2; i < s.patchLen; ++i) patch[i] = 0x90;
        }

        if (!WriteBytes(s.site, patch, s.patchLen))
            return false;

        s.applied = true;
        LogDebug("[FobChars] %s neutralised at %p (%s)\n",
            s.name, s.site, s.explanation);
        return true;
    }

    void RestoreSite(PatchSite& s)
    {
        if (!s.applied || !s.site) return;
        if (WriteBytes(s.site, s.original, s.patchLen))
        {
            s.applied = false;
            LogDebug("[FobChars] %s restored at %p\n", s.name, s.site);
        }
    }
}

namespace
{
    using GetQuarkSystemTable_t = void* (__fastcall*)();

    constexpr std::size_t   kStateOff_PlayerType        = 0xFB;
    constexpr std::uint32_t kInfoFlag_CarriesPlayerType = 0x100u;

    static GetQuarkSystemTable_t g_GetQuarkSystemTable = nullptr;
    static std::atomic<int>      g_Selected{ -1 };

    static std::uint8_t* QuarkPlayerState()
    {
        if (!g_GetQuarkSystemTable)
        {
            g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
                ResolveGameAddress(gAddr.GetQuarkSystemTable));
        }
        if (!g_GetQuarkSystemTable) return nullptr;

        __try
        {
            auto* table =
                reinterpret_cast<std::uint8_t*>(g_GetQuarkSystemTable());
            if (!table) return nullptr;
            auto* holder = *reinterpret_cast<std::uint8_t**>(table + 0x98);
            if (!holder) return nullptr;
            return *reinterpret_cast<std::uint8_t**>(holder + 0x10);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static bool ReadPlayerTypeByte(std::uint8_t* state, std::uint8_t* out)
    {
        __try
        {
            *out = state[kStateOff_PlayerType];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool WritePlayerTypeByte(std::uint8_t* state, std::uint8_t value)
    {
        __try
        {
            state[kStateOff_PlayerType] = value;
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
    const std::uint8_t kSlotListRowGatePattern[] = {
        0x49, 0x8B, 0x4F, 0x48,
        0x48, 0x8B, 0x01,
        0xFF, 0x90, 0xA0, 0x01, 0x00, 0x00,
        0x83, 0xF8, 0x05,
        0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x8B, 0x4F, 0x48,
        0x48, 0x8B, 0x01,
        0xFF, 0x90, 0xA0, 0x01, 0x00, 0x00,
        0x83, 0xF8, 0x06,
        0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
        0x41, 0x8B, 0x87, 0xA0, 0x3C, 0x00, 0x00,
        0x85, 0xC0,
        0x0F, 0x85, 0x00, 0x00, 0x00, 0x00
    };
    const char kSlotListRowGateMask[] =
        "xxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxx????xxxxxxxxxxx????";

    constexpr std::size_t kSlotRowRelVanillaA = 18;
    constexpr std::size_t kSlotRowRelVanillaB = 40;
    constexpr std::size_t kSlotRowRelPerRow   = 55;
    constexpr std::size_t kSlotRowStubSize    = 128;

    static std::uint8_t* g_SlotRowGate    = nullptr;
    static std::uint8_t* g_SlotRowStub    = nullptr;
    static std::uint8_t  g_SlotRowOriginal[8]{};
    static bool          g_SlotRowApplied = false;

    static std::uint8_t __fastcall SlotRowKeepVanillaFill()
    {
        return MissionCodeGuard::ShouldBypassHooks()
            ? static_cast<std::uint8_t>(1)
            : static_cast<std::uint8_t>(0);
    }

    static void* AllocateStubNear(std::uintptr_t nearAddr, std::size_t size)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::uintptr_t granularity = si.dwAllocationGranularity;
        const std::uintptr_t rounded     = nearAddr & ~(granularity - 1);
        const std::uintptr_t maxDistance = 0x60000000ull;

        for (std::uintptr_t off = granularity;
             off < maxDistance; off += granularity)
        {
            if (rounded >= off)
            {
                if (void* p = VirtualAlloc(
                        reinterpret_cast<LPVOID>(rounded - off), size,
                        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                    return p;
            }
            if (void* p = VirtualAlloc(
                    reinterpret_cast<LPVOID>(rounded + off), size,
                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return p;
        }
        return nullptr;
    }

    static void BuildSlotRowStub(std::uint8_t* stub,
                                 std::uintptr_t vanillaTarget,
                                 std::uintptr_t perRowTarget)
    {
        static const std::uint8_t kCode[] = {
            0x41, 0x8B, 0x87, 0xA0, 0x3C, 0x00, 0x00,
            0x85, 0xC0,
            0x74, 0x35,
            0x55,
            0x48, 0x8B, 0xEC,
            0x48, 0x83, 0xE4, 0xF0,
            0x48, 0x83, 0xEC, 0x20,
            0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,
            0x41, 0xFF, 0xD3,
            0x48, 0x8B, 0xE5,
            0x5D,
            0x84, 0xC0,
            0x75, 0x14,
            0x41, 0x8B, 0x87, 0xA0, 0x3C, 0x00, 0x00,
            0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,
            0x41, 0xFF, 0xE3,
            0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,
            0x41, 0xFF, 0xE3
        };

        std::memset(stub, 0xCC, kSlotRowStubSize);
        std::memcpy(stub, kCode, sizeof(kCode));

        const auto fn =
            reinterpret_cast<std::uint64_t>(&SlotRowKeepVanillaFill);
        std::memcpy(stub + 25, &fn, sizeof(fn));

        const auto perRow = static_cast<std::uint64_t>(perRowTarget);
        std::memcpy(stub + 53, &perRow, sizeof(perRow));

        const auto vanilla = static_cast<std::uint64_t>(vanillaTarget);
        std::memcpy(stub + 66, &vanilla, sizeof(vanilla));
    }

    bool ApplySlotRowGate()
    {
        if (g_SlotRowApplied) return true;

        std::uint8_t* hit = FindUniqueCode(
            kSlotListRowGatePattern, kSlotListRowGateMask,
            sizeof(kSlotListRowGatePattern),
            "sortie-prep slot row fill");
        if (!hit)
        {
            LogDebug("[FobChars] the sortie-prep slot list still paints every "
                "row with the chosen character, so the buddy and vehicle rows "
                "keep showing Ocelot or Quiet\n");
            return false;
        }

        std::int32_t relVanilla = 0;
        std::int32_t relPerRow  = 0;
        std::memcpy(&relVanilla, hit + kSlotRowRelVanillaA, sizeof(relVanilla));
        std::memcpy(&relPerRow,  hit + kSlotRowRelPerRow,   sizeof(relPerRow));

        const auto vanillaTarget =
            reinterpret_cast<std::uintptr_t>(hit + kSlotRowRelVanillaA + 4)
            + static_cast<std::intptr_t>(relVanilla);
        const auto perRowTarget =
            reinterpret_cast<std::uintptr_t>(hit + kSlotRowRelPerRow + 4)
            + static_cast<std::intptr_t>(relPerRow);

        auto* stub = static_cast<std::uint8_t*>(AllocateStubNear(
            reinterpret_cast<std::uintptr_t>(hit), kSlotRowStubSize));
        if (!stub)
        {
            LogDebug("[FobChars] no executable page could be reserved within "
                "reach of %p - the slot rows keep the vanilla fill\n", hit);
            return false;
        }

        const auto stubAddr = reinterpret_cast<std::intptr_t>(stub);
        const std::intptr_t deltaA = stubAddr
            - reinterpret_cast<std::intptr_t>(hit + kSlotRowRelVanillaA + 4);
        const std::intptr_t deltaB = stubAddr
            - reinterpret_cast<std::intptr_t>(hit + kSlotRowRelVanillaB + 4);

        if (deltaA > INT32_MAX || deltaA < INT32_MIN ||
            deltaB > INT32_MAX || deltaB < INT32_MIN)
        {
            LogDebug("[FobChars] the reserved page at %p is out of branch reach "
                "of %p - the slot rows keep the vanilla fill\n", stub, hit);
            VirtualFree(stub, 0, MEM_RELEASE);
            return false;
        }

        BuildSlotRowStub(stub, vanillaTarget, perRowTarget);
        FlushInstructionCache(GetCurrentProcess(), stub, kSlotRowStubSize);

        std::memcpy(g_SlotRowOriginal,     hit + kSlotRowRelVanillaA, 4);
        std::memcpy(g_SlotRowOriginal + 4, hit + kSlotRowRelVanillaB, 4);

        const auto newA = static_cast<std::int32_t>(deltaA);
        const auto newB = static_cast<std::int32_t>(deltaB);

        if (!WriteBytes(hit + kSlotRowRelVanillaA,
                reinterpret_cast<const std::uint8_t*>(&newA), 4))
            return false;

        if (!WriteBytes(hit + kSlotRowRelVanillaB,
                reinterpret_cast<const std::uint8_t*>(&newB), 4))
        {
            WriteBytes(hit + kSlotRowRelVanillaA, g_SlotRowOriginal, 4);
            return false;
        }

        g_SlotRowGate    = hit;
        g_SlotRowStub    = stub;
        g_SlotRowApplied = true;

        LogDebug("[FobChars] sortie-prep slot row fill rerouted at %p via %p - as "
                 "Ocelot or Quiet the game paints EVERY slot row with that "
                 "character's name and photo because a FOB sortie locks them; "
                 "outside FOB only the character row takes that fill\n", hit, stub);
        return true;
    }

    void RestoreSlotRowGate()
    {
        if (!g_SlotRowApplied || !g_SlotRowGate) return;

        WriteBytes(g_SlotRowGate + kSlotRowRelVanillaA, g_SlotRowOriginal, 4);
        WriteBytes(g_SlotRowGate + kSlotRowRelVanillaB,
            g_SlotRowOriginal + 4, 4);

        g_SlotRowApplied = false;
        LogDebug("[FobChars] sortie-prep slot row fill restored at %p\n",
            g_SlotRowGate);
    }
}

namespace
{
    using FindStaffByUniqueTypeId_t =
        bool (__fastcall*)(void*, std::uint16_t*, std::uint8_t);

    constexpr std::uint8_t kUniqueStaffId_Ocelot = 0xF9;
    constexpr std::uint8_t kUniqueStaffId_Quiet  = 0xFB;

    constexpr std::uint8_t kStaffUniqueMark = 0x3F;

    constexpr std::size_t  kStaffCountOff  = 0x9E6C;
    constexpr std::size_t  kStaffSeedLoOff = 0x9C78;
    constexpr std::size_t  kStaffSeedHiOff = 0x9C80;

    const std::uint8_t kFindStaffPattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10,
        0x57,
        0x0F, 0xB7, 0x81, 0x6C, 0x9E, 0x00, 0x00,
        0x41, 0x0F, 0xB6, 0xD8,
        0x48, 0x8B, 0xFA,
        0x4C, 0x8B, 0xD1,
        0x66, 0x85, 0xC0
    };
    const char kFindStaffMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxx";

    static FindStaffByUniqueTypeId_t g_FindStaffOrig   = nullptr;
    static void*                     g_FindStaffTarget = nullptr;
    static std::atomic<int>          g_LoggedOcelot{ 0 };
    static std::atomic<int>          g_LoggedQuiet{ 0 };

    static bool ReadRoster(void* sc, std::uint16_t* count,
                           const std::uint32_t** lo, const std::uint32_t** hi)
    {
        __try
        {
            auto* base = reinterpret_cast<const std::uint8_t*>(sc);
            *count = *reinterpret_cast<const std::uint16_t*>(
                base + kStaffCountOff);
            *lo = *reinterpret_cast<const std::uint32_t* const*>(
                base + kStaffSeedLoOff);
            *hi = *reinterpret_cast<const std::uint32_t* const*>(
                base + kStaffSeedHiOff);
            return *count != 0 && *lo != nullptr && *hi != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ScanRosterForUniqueId(const std::uint32_t* lo,
                                      const std::uint32_t* hi,
                                      std::uint16_t count,
                                      std::uint8_t wantId,
                                      std::uint16_t* outIdx,
                                      std::uint8_t* outMark)
    {
        __try
        {
            for (std::uint16_t i = 0; i < count; ++i)
            {
                const auto mark = static_cast<std::uint8_t>((lo[i] >> 7) & 0x3F);
                if (mark != kStaffUniqueMark) continue;
                if (static_cast<std::uint8_t>(hi[i] >> 24) != wantId) continue;
                *outIdx  = i;
                *outMark = mark;
                return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return false;
    }

    static int CollectUniqueBand(const std::uint32_t* lo,
                                 const std::uint32_t* hi,
                                 std::uint16_t count, char* buf,
                                 std::size_t bufSize)
    {
        int used   = 0;
        int listed = 0;
        buf[0] = '\0';

        __try
        {
            for (std::uint16_t i = 0; i < count && listed < 24; ++i)
            {
                const auto mark = static_cast<std::uint8_t>((lo[i] >> 7) & 0x3F);
                if (mark != kStaffUniqueMark) continue;

                const auto id = static_cast<std::uint8_t>(hi[i] >> 24);
                const int n = std::snprintf(buf + used, bufSize - used,
                    "%s%02X(row %u)", used ? ", " : "",
                    id, static_cast<unsigned>(i));
                if (n <= 0 || static_cast<std::size_t>(used + n) >= bufSize)
                    break;
                used += n;
                ++listed;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return listed;
    }

    static void LogUniqueBand(void* sc)
    {
        std::uint16_t        count = 0;
        const std::uint32_t* lo    = nullptr;
        const std::uint32_t* hi    = nullptr;
        if (!ReadRoster(sc, &count, &lo, &hi))
        {
            LogDebug("[FobChars] the staff roster at %p could not be read - the "
                "unique-character rows are built out of it, so neither Ocelot "
                "nor Quiet can be listed\n", sc);
            return;
        }

        char      buf[512];
        const int listed = CollectUniqueBand(lo, hi, count, buf, sizeof(buf));

        LogDebug("[FobChars] roster holds %u staff; unique-band records: %s (a "
            "character is offered only when its id is present AND its mark "
            "reads 3F - Ocelot is F9, Quiet is FB)\n",
            static_cast<unsigned>(count), listed ? buf : "none");
    }

    static bool __fastcall hkFindStaffByUniqueTypeId(
        void* sc, std::uint16_t* out, std::uint8_t id)
    {
        bool found = g_FindStaffOrig(sc, out, id);

        const bool isQuiet  = (id == kUniqueStaffId_Quiet);
        const bool isOcelot = (id == kUniqueStaffId_Ocelot);
        if (!isQuiet && !isOcelot) return found;
        if (MissionCodeGuard::ShouldBypassHooks()) return found;

        std::uint16_t idx       = 0;
        std::uint8_t  mark      = 0;
        bool          recovered = false;

        if (!found && sc && out)
        {
            std::uint16_t        count = 0;
            const std::uint32_t* lo    = nullptr;
            const std::uint32_t* hi    = nullptr;
            if (ReadRoster(sc, &count, &lo, &hi) &&
                ScanRosterForUniqueId(lo, hi, count, id, &idx, &mark))
            {
                *out      = idx;
                found     = true;
                recovered = true;
            }
        }

        std::atomic<int>& logged = isQuiet ? g_LoggedQuiet : g_LoggedOcelot;
        if (logged.exchange(1, std::memory_order_relaxed) == 0)
        {
            const char* who = isQuiet ? "Quiet" : "Ocelot";
            if (isQuiet) LogUniqueBand(sc);
            if (recovered)
                LogDebug("[FobChars] the character list asked the roster for unique "
                         "id %02X and the game's scan said no, but that character "
                         "IS at row %u with mark %02X instead of 3F - served from "
                         "there, since otherwise %s is never offered\n",
                    static_cast<unsigned>(id), static_cast<unsigned>(idx),
                    static_cast<unsigned>(mark), who);
            else if (!found)
            {
                LogDebug("[FobChars] the character list asked the roster for unique "
                         "id %02X and no record carries it, so the game skips the "
                         "%s row before any patched gate matters\n", static_cast<unsigned>(id), who);
                LogUniqueBand(sc);
            }
            else
                LogDebug("[FobChars] the staff roster served unique id %02X for "
                    "the %s row - the list build reached that row\n",
                    static_cast<unsigned>(id), who);
        }

        return found;
    }

    bool InstallStaffLookupFallback()
    {
        if (g_FindStaffTarget) return true;

        std::uint8_t* hit = FindUniqueCode(kFindStaffPattern, kFindStaffMask,
            sizeof(kFindStaffPattern), "unique-staff roster lookup");
        if (!hit)
        {
            LogDebug("[FobChars] the unique-staff roster lookup was not found - a "
                     "character whose roster mark is not 3F still gets no row in "
                     "the character list\n");
            return false;
        }

        if (!CreateAndEnableHook(hit, &hkFindStaffByUniqueTypeId,
                reinterpret_cast<void**>(&g_FindStaffOrig)))
        {
            LogDebug("[FobChars] the unique-staff roster lookup at %p could not "
                "be hooked - Quiet keeps whatever row the roster's own scan "
                "gives her\n", hit);
            return false;
        }

        g_FindStaffTarget = hit;
        return true;
    }

    void UninstallStaffLookupFallback()
    {
        if (!g_FindStaffTarget) return;
        DisableAndRemoveHook(g_FindStaffTarget);
        g_FindStaffTarget = nullptr;
        g_FindStaffOrig   = nullptr;
    }
}

namespace
{
    using SetupStaffList_t = std::uint64_t (__fastcall*)(void*);
    using SlotRowPainter_t = void (__fastcall*)(void*);

    using GenerateStaffParameter_t = std::uint64_t (__fastcall*)(
        void*, std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t,
        std::uint8_t);

    using CanSortieBuddyType_t = std::uint8_t (__fastcall*)(void*, std::uint8_t);

    constexpr std::size_t kStaffControllerOff     = 0x118;
    constexpr std::size_t kGenerateStaffParamVtbl = 0x1B0;

    constexpr std::size_t  kPrepStaffControllerOff = 0x60;
    constexpr std::size_t  kQuarkAppInterfaceOff    = 0x98;
    constexpr std::size_t  kBuddyServiceQuarkOff    = 0xE8;
    constexpr std::size_t  kCanSortieBuddyTypeVtbl  = 0x3E0;
    constexpr std::uint8_t kBuddyType_Quiet         = 3;

    constexpr std::uint8_t kQuietScratchIds[] = { 0xFA, 0xFC, 0xFD };

    const std::uint8_t kSlotRowPainterPrologue[] = {
        0x48, 0x89, 0x5C, 0x24, 0x20,
        0x55,
        0x56,
        0x57,
        0x41, 0x54,
        0x41, 0x57,
        0x48, 0x8B, 0xEC,
        0x48, 0x83, 0xEC, 0x30
    };
    const char kSlotRowPainterPrologueMask[] = "xxxxxxxxxxxxxxxxxxx";

    const std::uint8_t kSetupStaffListPrologue[] = {
        0x48, 0x8B, 0xC4,
        0x57,
        0x41, 0x54,
        0x41, 0x55,
        0x41, 0x56,
        0x41, 0x57,
        0x48, 0x81, 0xEC
    };
    const char kSetupStaffListPrologueMask[] = "xxxxxxxxxxxxxxx";

    constexpr std::size_t kSetupStaffListScanBack = 0x800;

    static SetupStaffList_t g_SetupStaffListOrig   = nullptr;
    static void*            g_SetupStaffListTarget = nullptr;

    static SlotRowPainter_t g_SlotRowPainterOrig   = nullptr;
    static void*            g_SlotRowPainterTarget = nullptr;

    static bool          g_QuietSeedValid = false;
    static std::uint32_t g_QuietSeedLo    = 0;
    static std::uint32_t g_QuietSeedHi    = 0;

    static bool          g_QuietRowActive = false;
    static std::uint16_t g_QuietRowIndex  = 0;
    static std::uint32_t g_QuietRowLo     = 0;
    static std::uint32_t g_QuietRowHi     = 0;

    static int g_QuietRowDepth = 0;

    static std::atomic<int> g_LoggedQuietRow{ 0 };
    static std::atomic<int> g_LoggedQuietRowFail{ 0 };
    static std::atomic<int> g_LoggedQuietUnavailable{ 0 };

    static void* StaffControllerAt(void* menuThis, std::size_t off)
    {
        if (!menuThis) return nullptr;
        __try
        {
            return *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(menuThis) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static bool GenerateUniqueStaffSeed(void* sc, std::uint8_t uniqueId,
                                        std::uint32_t* outLo,
                                        std::uint32_t* outHi)
    {
        __try
        {
            auto** vtbl = *reinterpret_cast<void***>(sc);
            if (!vtbl) return false;

            auto gen = reinterpret_cast<GenerateStaffParameter_t>(
                vtbl[kGenerateStaffParamVtbl / sizeof(void*)]);
            if (!gen) return false;

            const std::uint64_t seed = gen(sc, 0, 0x3F, 0x0A, 0, uniqueId);

            const auto lo = static_cast<std::uint32_t>(seed);
            const auto hi = static_cast<std::uint32_t>(
                (seed >> 0x15) & 0xFFFFF800u);

            if (static_cast<std::uint8_t>((lo >> 7) & 0x3F) != kStaffUniqueMark)
                return false;
            if (static_cast<std::uint8_t>(hi >> 24) != uniqueId)
                return false;

            *outLo = lo;
            *outHi = hi;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool FindQuietScratchRow(const std::uint32_t* lo,
                                    const std::uint32_t* hi,
                                    std::uint16_t count,
                                    std::uint16_t* outRow,
                                    std::uint8_t* outId)
    {
        bool found = false;
        __try
        {
            for (std::uint16_t i = 0; i < count; ++i)
            {
                if (static_cast<std::uint8_t>((lo[i] >> 7) & 0x3F)
                        != kStaffUniqueMark)
                    continue;

                const auto id = static_cast<std::uint8_t>(hi[i] >> 24);
                for (std::size_t c = 0;
                     c < sizeof(kQuietScratchIds) / sizeof(kQuietScratchIds[0]);
                     ++c)
                {
                    if (id != kQuietScratchIds[c]) continue;
                    *outRow = i;
                    *outId  = id;
                    found   = true;
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return found;
    }

    static bool SwapRosterSeed(void* sc, std::uint16_t row,
                               std::uint32_t newLo, std::uint32_t newHi,
                               std::uint32_t* savedLo, std::uint32_t* savedHi)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(sc);
            auto* lo = *reinterpret_cast<std::uint32_t**>(base + kStaffSeedLoOff);
            auto* hi = *reinterpret_cast<std::uint32_t**>(base + kStaffSeedHiOff);
            if (!lo || !hi) return false;

            if (savedLo) *savedLo = lo[row];
            if (savedHi) *savedHi = hi[row];

            lo[row] = newLo;
            hi[row] = newHi;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void RestoreQuietRow(void* sc)
    {
        if (!g_QuietRowActive) return;
        g_QuietRowActive = false;
        if (!sc) return;

        SwapRosterSeed(sc, g_QuietRowIndex, g_QuietRowLo, g_QuietRowHi,
            nullptr, nullptr);
    }

    static std::uint8_t* QuarkAppInterface()
    {
        if (!g_GetQuarkSystemTable)
        {
            g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
                ResolveGameAddress(gAddr.GetQuarkSystemTable));
        }
        if (!g_GetQuarkSystemTable) return nullptr;

        __try
        {
            auto* table =
                reinterpret_cast<std::uint8_t*>(g_GetQuarkSystemTable());
            if (!table) return nullptr;
            return *reinterpret_cast<std::uint8_t**>(
                table + kQuarkAppInterfaceOff);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static bool QuietCanSortie()
    {
        std::uint8_t* app = QuarkAppInterface();
        if (!app) return false;

        __try
        {
            void* svc = *reinterpret_cast<void**>(app + kBuddyServiceQuarkOff);
            if (!svc) return false;

            auto** vtbl = *reinterpret_cast<void***>(svc);
            if (!vtbl) return false;

            auto canSortie = reinterpret_cast<CanSortieBuddyType_t>(
                vtbl[kCanSortieBuddyTypeVtbl / sizeof(void*)]);
            if (!canSortie) return false;

            return canSortie(svc, kBuddyType_Quiet) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void PrepareQuietRowOn(void* sc)
    {
        g_QuietRowActive = false;

        if (MissionCodeGuard::ShouldBypassHooks()) return;

        if (!QuietCanSortie())
        {
            if (g_LoggedQuietUnavailable.exchange(1, std::memory_order_relaxed)
                    == 0)
                LogDebug("[FobChars] the buddy service reports Quiet cannot "
                         "sortie on this save, so no record is built for her and "
                         "the character list leaves her out\n");
            return;
        }

        if (!sc) return;

        std::uint16_t        count = 0;
        const std::uint32_t* lo    = nullptr;
        const std::uint32_t* hi    = nullptr;
        if (!ReadRoster(sc, &count, &lo, &hi)) return;

        std::uint16_t present = 0;
        std::uint8_t  mark    = 0;
        if (ScanRosterForUniqueId(lo, hi, count, kUniqueStaffId_Quiet,
                &present, &mark))
            return;

        std::uint16_t row      = 0;
        std::uint8_t  borrowed = 0;
        if (!FindQuietScratchRow(lo, hi, count, &row, &borrowed))
        {
            if (g_LoggedQuietRowFail.exchange(1, std::memory_order_relaxed) == 0)
                LogDebug("[FobChars] Quiet has no staff record and this roster "
                         "carries no spare unique row (id FA, FC or FD) to build "
                         "one in - her row stays missing from the character "
                         "list\n");
            return;
        }

        if (!g_QuietSeedValid)
        {
            std::uint32_t newLo = 0;
            std::uint32_t newHi = 0;
            if (!GenerateUniqueStaffSeed(sc, kUniqueStaffId_Quiet,
                    &newLo, &newHi))
            {
                if (g_LoggedQuietRowFail.exchange(1, std::memory_order_relaxed)
                        == 0)
                    LogDebug("[FobChars] the staff generator refused to mint a "
                             "record for unique id FB - Quiet's row stays "
                             "missing from the character list\n");
                return;
            }

            g_QuietSeedLo    = newLo;
            g_QuietSeedHi    = newHi;
            g_QuietSeedValid = true;
        }

        if (!SwapRosterSeed(sc, row, g_QuietSeedLo, g_QuietSeedHi,
                &g_QuietRowLo, &g_QuietRowHi))
        {
            if (g_LoggedQuietRowFail.exchange(1, std::memory_order_relaxed) == 0)
                LogDebug("[FobChars] roster row %u could not be written - Quiet's "
                         "row stays missing from the character list\n",
                    static_cast<unsigned>(row));
            return;
        }

        g_QuietRowIndex  = row;
        g_QuietRowActive = true;

        if (g_LoggedQuietRow.exchange(1, std::memory_order_relaxed) == 0)
            LogDebug("[FobChars] Quiet has no staff record on this roster, so the "
                     "list build skipped her; row %u (unique id %02X, which this "
                     "build never looks up and which the deployable-staff filter "
                     "rejects either way) carries a generated FB record for the "
                     "length of the build and is restored the moment it ends\n",
                static_cast<unsigned>(row), static_cast<unsigned>(borrowed));
    }

    static std::uint64_t __fastcall hkSetupStaffList(void* self)
    {
        const bool outermost = (g_QuietRowDepth == 0);
        ++g_QuietRowDepth;

        void* sc = StaffControllerAt(self, kStaffControllerOff);
        if (outermost) PrepareQuietRowOn(sc);

        std::uint64_t result = 0;
        __try
        {
            result = g_SetupStaffListOrig(self);
        }
        __finally
        {
            --g_QuietRowDepth;
            if (outermost) RestoreQuietRow(sc);
        }
        return result;
    }

    static void __fastcall hkSetupCharacterSlotRow(void* self)
    {
        const bool outermost = (g_QuietRowDepth == 0);
        ++g_QuietRowDepth;

        void* sc = StaffControllerAt(self, kPrepStaffControllerOff);
        if (outermost) PrepareQuietRowOn(sc);

        __try
        {
            g_SlotRowPainterOrig(self);
        }
        __finally
        {
            --g_QuietRowDepth;
            if (outermost) RestoreQuietRow(sc);
        }
    }

    static std::uint8_t* ResolveEntryBefore(std::uint8_t* gate,
                                            const std::uint8_t* prologue,
                                            const char* mask, std::size_t len)
    {
        if (!gate) return nullptr;

        __try
        {
            for (std::size_t back = len;
                 back <= kSetupStaffListScanBack; ++back)
            {
                std::uint8_t* p = gate - back;
                if (MatchesAt(p, prologue, mask, len)) return p;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return nullptr;
    }

    bool InstallQuietRowSynthesis()
    {
        if (g_SetupStaffListTarget) return true;

        std::uint8_t* entry = ResolveEntryBefore(g_Sites[0].site,
            kSetupStaffListPrologue, kSetupStaffListPrologueMask,
            sizeof(kSetupStaffListPrologue));
        if (!entry)
        {
            LogDebug("[FobChars] the character list build could not be located "
                     "around %p - a roster with no FB record keeps Quiet out of "
                     "the list\n", g_Sites[0].site);
            return false;
        }

        if (!CreateAndEnableHook(entry, &hkSetupStaffList,
                reinterpret_cast<void**>(&g_SetupStaffListOrig)))
        {
            LogDebug("[FobChars] the character list build at %p could not be "
                     "hooked - a roster with no FB record keeps Quiet out of the "
                     "list\n", entry);
            return false;
        }

        g_SetupStaffListTarget = entry;
        return true;
    }

    bool InstallQuietPrepRowSynthesis()
    {
        if (g_SlotRowPainterTarget) return true;

        std::uint8_t* entry = ResolveEntryBefore(g_SlotRowGate,
            kSlotRowPainterPrologue, kSlotRowPainterPrologueMask,
            sizeof(kSlotRowPainterPrologue));
        if (!entry)
        {
            LogDebug("[FobChars] the sortie-prep row painter could not be "
                     "located around %p - as Quiet the prep rows fall back to "
                     "Snake's name and photo\n", g_SlotRowGate);
            return false;
        }

        if (!CreateAndEnableHook(entry, &hkSetupCharacterSlotRow,
                reinterpret_cast<void**>(&g_SlotRowPainterOrig)))
        {
            LogDebug("[FobChars] the sortie-prep row painter at %p could not be "
                     "hooked - as Quiet the prep rows fall back to Snake's name "
                     "and photo\n", entry);
            return false;
        }

        g_SlotRowPainterTarget = entry;
        return true;
    }

    void UninstallQuietRowSynthesis()
    {
        if (g_SlotRowPainterTarget)
        {
            DisableAndRemoveHook(g_SlotRowPainterTarget);
            g_SlotRowPainterTarget = nullptr;
            g_SlotRowPainterOrig   = nullptr;
        }

        if (!g_SetupStaffListTarget) return;
        DisableAndRemoveHook(g_SetupStaffListTarget);
        g_SetupStaffListTarget = nullptr;
        g_SetupStaffListOrig   = nullptr;
        g_QuietRowActive       = false;
    }
}

namespace fobchars
{
    void NoteLoadoutPlayerType(std::uint8_t playerType, std::uint32_t flags)
    {
        if ((flags & kInfoFlag_CarriesPlayerType) == 0) return;
        if (MissionCodeGuard::ShouldBypassHooks()) return;

        const bool fobOnly =
            playerType == kPlayerType_Ocelot || playerType == kPlayerType_Quiet;

        const int want = fobOnly ? static_cast<int>(playerType) : -1;
        const int had  = g_Selected.exchange(want, std::memory_order_relaxed);
        if (had == want) return;

        if (fobOnly)
            LogDebug("[FobChars] character select committed player type %u - "
                     "carried across mission loads, because single player never "
                     "writes the choice back to the saved player vars\n", static_cast<unsigned>(playerType));
        else if (had >= 0)
            LogDebug("[FobChars] character select committed player type %u - the "
                "carried FOB character (%d) was released\n",
                static_cast<unsigned>(playerType), had);
    }

    std::uint8_t GetSelectedPlayerType()
    {
        const int want = g_Selected.load(std::memory_order_relaxed);
        if (want < 0) return 0xFF;
        return static_cast<std::uint8_t>(want);
    }

    void ReassertSelectedCharacter()
    {
        const int want = g_Selected.load(std::memory_order_relaxed);
        if (want < 0) return;
        if (MissionCodeGuard::ShouldBypassHooks()) return;

        std::uint8_t* state = QuarkPlayerState();
        if (!state) return;

        std::uint8_t live = 0;
        if (!ReadPlayerTypeByte(state, &live)) return;
        if (live == static_cast<std::uint8_t>(want)) return;

        if (!WritePlayerTypeByte(state, static_cast<std::uint8_t>(want))) return;

        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 16)
            LogDebug("[FobChars] player type had fallen back to %u - restored the "
                     "selected character %d (the mission-start loadout restore "
                     "rebuilds it from the saved vars)\n",
                static_cast<unsigned>(live), want);
    }
}

bool Install_FobPlayerCharacters_Patches()
{
    int applied = 0;
    for (auto& s : g_Sites)
        if (ApplySite(s)) ++applied;

    ApplySlotRowGate();
    InstallStaffLookupFallback();
    InstallQuietRowSynthesis();
    InstallQuietPrepRowSynthesis();

    if (applied == 0)
    {
        LogDebug("[FobChars] no patch site matched this build - Ocelot and Quiet "
            "remain FOB only\n");
        return true;
    }

    if (!g_Sites[0].applied)
    {
        LogDebug("[FobChars] the list gate is still in place, so Ocelot and Quiet "
                 "are not offered outside a FOB sortie even though %d other "
                 "patch(es) applied\n", applied);
    }
    else if (!g_Sites[1].applied)
    {
        LogDebug("[FobChars] Ocelot and Quiet are listed, but the parts-status "
                 "reset is still in place - the engine drops the choice back to "
                 "Snake when the mission starts\n");
    }
    else
    {
        LogDebug("[FobChars] Ocelot and Quiet are selectable outside FOB "
            "(%d/%d patches applied)\n",
            applied, static_cast<int>(sizeof(g_Sites) / sizeof(g_Sites[0])));
    }

    return true;
}

void Uninstall_FobPlayerCharacters_Patches()
{
    g_Selected.store(-1, std::memory_order_relaxed);
    UninstallQuietRowSynthesis();
    UninstallStaffLookupFallback();
    RestoreSlotRowGate();
    for (auto& s : g_Sites)
        RestoreSite(s);
}
