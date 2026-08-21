#include "pch.h"

#include "FobPlayerCharacters.h"

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "log.h"

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

    const std::uint8_t kQuietAvailabilityGatePattern[] = {
        0x48, 0x8B, 0x8E, 0x20, 0x01, 0x00, 0x00,
        0x48, 0x8B, 0x01,
        0xB2, 0x03,
        0xFF, 0x90, 0xE0, 0x03, 0x00, 0x00,
        0x84, 0xC0,
        0x0F, 0x84
    };
    const char kQuietAvailabilityGateMask[] = "xxxxxxxxxxxxxxxxxxxxxx";

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
            "Quiet availability gate",
            "Quiet carries a second availability check that only answers yes "
            "inside a FOB sortie",
            kQuietAvailabilityGatePattern, kQuietAvailabilityGateMask,
            sizeof(kQuietAvailabilityGatePattern), 12, 6, false,
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

        LogDebug("[FobChars] sortie-prep slot row fill rerouted at %p via %p "
            "(as Ocelot or Quiet the game paints EVERY slot row - buddy, "
            "vehicle, weapons - with the character's own name and Mother Base "
            "photo, because a FOB sortie locks those slots; outside FOB only "
            "the character row takes that fill now and the other rows keep "
            "their own content)\n", hit, stub);
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
            LogDebug("[FobChars] character select committed player type %u - it "
                "is now carried across mission loads, because single player "
                "never reflects the choice back into the saved player vars the "
                "way a FOB sortie does\n", static_cast<unsigned>(playerType));
        else if (had >= 0)
            LogDebug("[FobChars] character select committed player type %u - the "
                "carried FOB character (%d) was released\n",
                static_cast<unsigned>(playerType), had);
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
        if (live == kPlayerType_Avatar) return;

        if (!WritePlayerTypeByte(state, static_cast<std::uint8_t>(want))) return;

        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 16)
            LogDebug("[FobChars] player type had fallen back to %u - restored "
                "the selected character %d (the loadout restore at mission "
                "start rebuilds it from the saved vars)\n",
                static_cast<unsigned>(live), want);
    }
}

bool Install_FobPlayerCharacters_Patches()
{
    int applied = 0;
    for (auto& s : g_Sites)
        if (ApplySite(s)) ++applied;

    ApplySlotRowGate();

    if (applied == 0)
    {
        LogDebug("[FobChars] no patch site matched this build - Ocelot and Quiet "
            "remain FOB only\n");
        return true;
    }

    if (!g_Sites[0].applied)
    {
        LogDebug("[FobChars] the list gate is still in place, so Ocelot and "
            "Quiet will not be offered outside a FOB sortie even though %d "
            "other patch(es) applied\n", applied);
    }
    else if (!g_Sites[2].applied)
    {
        LogDebug("[FobChars] Ocelot and Quiet are now listed, but the "
            "parts-status reset is still in place - the engine will drop the "
            "choice back to Snake as soon as the mission starts\n");
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
    RestoreSlotRowGate();
    for (auto& s : g_Sites)
        RestoreSite(s);
}
