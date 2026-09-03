#include "pch.h"

#include "UniqueCharacterSuitSlot.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "OutfitRegistry.h"
#include "UniqueCharacterPartsTypePin.h"

#include <atomic>
#include "log.h"

namespace
{
    constexpr std::size_t kPatchLen  = 2;
    constexpr std::size_t kSiteCount = 2;

    struct Site
    {
        std::ptrdiff_t offset;
        std::uint8_t   original[kPatchLen];
        std::uint8_t   patched[kPatchLen];
    };

    const Site kSites[kSiteCount] = {
        { 0x00, { 0x74, 0x5C }, { 0x90, 0x90 } },
        { 0x12, { 0x74, 0x4A }, { 0x90, 0x90 } },
    };

    static bool g_Applied = false;

    using SlotNum_t = std::uint32_t (__fastcall*)(void*);
    static SlotNum_t g_OrigSlotNum   = nullptr;
    static bool      g_HookInstalled = false;

    constexpr std::size_t kMenuModeOff  = 0x3CA0;
    constexpr std::uint32_t kCountWithHeadOption = 3;
    constexpr std::uint32_t kCountWithoutHead    = 2;

    static bool TryReadMenuMode(void* self, std::int32_t* out)
    {
        __try
        {
            *out = *reinterpret_cast<std::int32_t*>(
                static_cast<std::uint8_t*>(self) + kMenuModeOff);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static std::uint32_t __fastcall hkCharaSlotSelectionNum(void* self)
    {
        const std::uint32_t ret = g_OrigSlotNum ? g_OrigSlotNum(self) : 0;
        if (ret != kCountWithHeadOption || !self) return ret;
        if (MissionCodeGuard::ShouldBypassHooks()) return ret;

        const std::uint8_t livePT = outfit::ReadLivePlayerType();
        if (!outfit::IsUniqueCharacterPlayerType(livePT)) return ret;

        std::int32_t mode = -1;
        if (!TryReadMenuMode(self, &mode)) return ret;
        if (mode != 0 && mode != 2) return ret;

        const std::uint8_t parts = outfit::ReadLivePartsType();
        bool offered = false;
        if (!uniquecharpin::IsOwnSuitPartsType(livePT, parts))
        {
            if (parts >= outfit::kCustomPartsTypeStart
             && parts <= outfit::kCustomPartsTypeEnd)
            {
                const outfit::OutfitEntry* entry = nullptr;
                if (!outfit::TryGetOutfitByPartsType(parts, &entry) || !entry)
                    return ret;
                offered = entry->HasHeadOptionsForVariant(
                    livePT, outfit::GetActiveVariant(parts));
            }
            else
            {
                offered = outfit::VanillaExtHasAnyHeadOptions(
                    parts, livePT, outfit::ReadLiveSelectorCode());
            }
        }

        if (offered) return ret;

        static std::atomic<int> s_trimLogged{ 0 };
        if (s_trimLogged.fetch_add(1, std::memory_order_relaxed) == 0)
            LogDebug("[SuitSlot] player type %u wears partsType 0x%02X, which "
                "offers no head option, so the slot list drops the empty HEAD "
                "OPTION row (count %u -> %u) - the engine asks its own gate, "
                "which answers for a vanilla suit id\n",
                static_cast<unsigned>(livePT),
                static_cast<unsigned>(parts),
                kCountWithHeadOption, kCountWithoutHead);
        return kCountWithoutHead;
    }

    static bool InstallSlotNumHook()
    {
        if (g_HookInstalled) return true;

        void* target = ResolveGameAddress(gAddr.CharaSlotSelectionNum);
        if (!target)
        {
            LogDebug("[SuitSlot] no chara-slot-count address for this build - a "
                     "suit with no head option keeps an empty HEAD OPTION row\n");
            return false;
        }

        g_HookInstalled = CreateAndEnableHook(target,
            reinterpret_cast<void*>(&hkCharaSlotSelectionNum),
            reinterpret_cast<void**>(&g_OrigSlotNum));
        if (!g_HookInstalled)
            Log("[SuitSlot] chara-slot-count hook FAILED at %p - Ocelot and Quiet "
                "keep an empty HEAD OPTION row whenever their suit offers none\n",
                target);
        return g_HookInstalled;
    }


    static bool WriteBytes(void* target, const std::uint8_t* src, std::size_t len)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("[SuitSlot] VirtualProtect failed at %p (err=%lu) - Ocelot and "
                "Quiet keep a character-only slot list\n", target, GetLastError());
            return false;
        }

        std::memcpy(target, src, len);

        DWORD restored = 0;
        VirtualProtect(target, len, oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, len);
        return true;
    }

    static bool Apply(bool on)
    {
        if (g_Applied == on) return true;

        const std::uintptr_t rva = gAddr.CharaSlotSelectionNumGate;
        if (!rva)
        {
            LogDebug("[SuitSlot] no address for this build - Ocelot and Quiet keep "
                     "a character-only slot list, so no uniform row is reachable\n");
            return false;
        }

        auto* base = static_cast<std::uint8_t*>(ResolveGameAddress(rva));
        if (!base)
        {
            LogDebug("[SuitSlot] rva %llX could not be resolved - Ocelot and Quiet "
                     "keep a character-only slot list\n",
                static_cast<unsigned long long>(rva));
            return false;
        }

        for (std::size_t i = 0; i < kSiteCount; ++i)
        {
            std::uint8_t*       site   = base + kSites[i].offset;
            const std::uint8_t* expect = on ? kSites[i].original : kSites[i].patched;
            if (std::memcmp(site, expect, kPatchLen) != 0)
            {
                LogDebug("[SuitSlot] found %02X %02X at %p instead of the expected "
                         "run - refusing to write either site, so the slot list "
                         "stays as it is\n", site[0], site[1], site);
                return false;
            }
        }

        for (std::size_t i = 0; i < kSiteCount; ++i)
        {
            std::uint8_t* site = base + kSites[i].offset;
            if (!WriteBytes(site, on ? kSites[i].patched : kSites[i].original,
                    kPatchLen))
            {
                for (std::size_t back = 0; back < i; ++back)
                    WriteBytes(base + kSites[back].offset,
                        on ? kSites[back].original : kSites[back].patched, kPatchLen);
                return false;
            }
        }

        g_Applied = on;
        LogDebug("[SuitSlot] character slot list %s at %p - Ocelot and Quiet %s\n",
            on ? "widened" : "narrowed", base,
            on ? "now get the uniform row (and the head-option row when the suit "
                 "allows one); vanilla returns a count of 1 for player types 5 and "
                 "6, so only the character row was ever reachable"
               : "are back to a character-only list");
        return true;
    }
}

void UniqueCharacterSuitSlot_EnforceMissionGuard()
{
    if (!g_HookInstalled) return;
    const bool want = !MissionCodeGuard::ShouldBypassHooks();
    if (g_Applied == want) return;
    Apply(want);
}

bool Install_UniqueCharacterSuitSlot()
{
    if (!InstallSlotNumHook())
    {
        Log("[SuitSlot] the chara-slot-count hook did not install, so the slot "
            "list is left as vanilla - widening it without the hook would give "
            "Ocelot and Quiet a permanently empty HEAD OPTION row\n");
        return true;
    }
    if (MissionCodeGuard::ShouldBypassHooks()) return true;
    Apply(true);
    return true;
}

void Uninstall_UniqueCharacterSuitSlot()
{
    if (!Apply(false))
    {
        Log("[SuitSlot] the widening NOPs could not be reverted, so the "
            "chara-slot-count hook stays installed - removing it would leave the "
            "list widened with nothing left to trim the empty HEAD OPTION row\n");
        return;
    }
    if (g_HookInstalled)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.CharaSlotSelectionNum));
        g_OrigSlotNum   = nullptr;
        g_HookInstalled = false;
    }
}
