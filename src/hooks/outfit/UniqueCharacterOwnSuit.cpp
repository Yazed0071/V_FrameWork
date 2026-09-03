#include "pch.h"

#include "UniqueCharacterOwnSuit.h"

#include "OutfitRegistry.h"
#include "FoxHashes.h"
#include "log.h"

#include <atomic>

namespace
{
    struct OwnSuitSlot
    {
        std::atomic<std::uint16_t> flowIndex{ 0 };
        std::atomic<std::uint64_t> nameHash{ 0 };
        std::atomic<std::uint64_t> iconHash{ 0 };
        std::atomic<std::uint64_t> infoHash{ 0 };
    };

    OwnSuitSlot g_Slot[2];

    struct OwnSuitDefault
    {
        int         slot;
        const char* character;
        const char* name;
        const char* icon;
        const char* info;
    };

    const OwnSuitDefault kOwnSuitDefaults[] =
    {
        { 0, "ocelot", "name_dd_24007", nullptr, "name_dd_24007" },
        { 1, "quiet",  "name_qe_51000", nullptr, "info_qe_51000" },
    };

    std::atomic<bool> g_DefaultsReady{ false };

    void EnsureDefaults()
    {
        if (g_DefaultsReady.load(std::memory_order_acquire)) return;

        bool anyResolved = false;
        for (const auto& d : kOwnSuitDefaults)
        {
            const std::uint64_t n = FoxHashes::StrCode64(d.name);
            const std::uint64_t i =
                d.icon ? FoxHashes::PathCode64Ext(d.icon) : 0ull;
            if (n == 0 && i == 0) continue;

            anyResolved = true;

            auto& slot = g_Slot[d.slot];
            std::uint64_t expected = 0;
            slot.nameHash.compare_exchange_strong(expected, n,
                                                  std::memory_order_relaxed);
            expected = 0;
            slot.iconHash.compare_exchange_strong(expected, i,
                                                  std::memory_order_relaxed);
            expected = 0;
            slot.infoHash.compare_exchange_strong(
                expected, FoxHashes::StrCode64(d.info), std::memory_order_relaxed);
        }

        if (!anyResolved) return;

        for (const auto& d : kOwnSuitDefaults)
        {
            const auto& slot = g_Slot[d.slot];
            if (slot.nameHash.load(std::memory_order_relaxed) != 0
             || slot.iconHash.load(std::memory_order_relaxed) != 0) continue;

            Log("[UniqueOwnSuit] neither '%s' nor '%s' hashed for %s, so their "
                "default-outfit row keeps the borrowed vanilla suit label - override "
                "it by giving the row a langEquipName and iconFtexPath\n",
                d.name, d.icon, d.character, d.character);
        }

        g_DefaultsReady.store(true, std::memory_order_release);
    }

    int SlotIndex(std::uint8_t playerType)
    {
        if (playerType == outfit::kPlayerType_Ocelot) return 0;
        if (playerType == outfit::kPlayerType_Quiet)  return 1;
        return -1;
    }
}

namespace uniqueownsuit
{
    bool AdoptOrMatchRow(std::uint8_t playerType, std::uint16_t flowIndex,
                         bool acceptedForSnake)
    {
        const int s = SlotIndex(playerType);
        if (s < 0 || flowIndex == 0) return false;

        const std::uint16_t held =
            g_Slot[s].flowIndex.load(std::memory_order_relaxed);
        if (held != 0) return held == flowIndex;

        if (!acceptedForSnake) return false;

        std::uint16_t expected = 0;
        if (!g_Slot[s].flowIndex.compare_exchange_strong(
                expected, flowIndex, std::memory_order_relaxed))
            return expected == flowIndex;

        static std::atomic<bool> s_iconNoticeLogged{ false };
        if (!s_iconNoticeLogged.exchange(true))
            Log("[UniqueOwnSuit] player type %u has no default-outfit "
                "develop row, so their Uniform row borrows a vanilla "
                "suit row and keeps that row's icon - point it at a "
                "real texture in the "
                "default-outfit spec table\n",
                static_cast<unsigned>(playerType));

#ifdef _DEBUG
        LogDebug("[UniqueOwnSuit] adopted flowIndex=%u as the own-suit row for "
                 "player type %u - their default outfit is not a developable item, "
                 "so the Uniform list borrows the first suit row the engine already "
                 "offers Snake; picking it hands SetSuit a vanilla pair and the pin "
                 "restores their own suit\n",
            static_cast<unsigned>(flowIndex),
            static_cast<unsigned>(playerType));
#endif
        return true;
    }

    bool IsOwnSuitRow(std::uint8_t playerType, std::uint16_t flowIndex)
    {
        const int s = SlotIndex(playerType);
        if (s < 0 || flowIndex == 0) return false;
        return g_Slot[s].flowIndex.load(std::memory_order_relaxed) == flowIndex;
    }

    std::uint16_t GetOwnSuitRow(std::uint8_t playerType)
    {
        const int s = SlotIndex(playerType);
        if (s < 0) return 0;
        return g_Slot[s].flowIndex.load(std::memory_order_relaxed);
    }

    bool TryGetLabel(std::uint8_t playerType, std::uint16_t flowIndex,
                     std::uint64_t* outNameHash, std::uint64_t* outIconHash)
    {
        EnsureDefaults();

        const int s = SlotIndex(playerType);
        if (s < 0 || !IsOwnSuitRow(playerType, flowIndex)) return false;

        const std::uint64_t n = g_Slot[s].nameHash.load(std::memory_order_relaxed);
        const std::uint64_t i = g_Slot[s].iconHash.load(std::memory_order_relaxed);
        if (n == 0 && i == 0) return false;

        if (outNameHash) *outNameHash = n;
        if (outIconHash) *outIconHash = i;
        return true;
    }

    bool TryGetRowLabel(std::uint8_t playerType, std::uint64_t* outNameHash,
                        std::uint64_t* outInfoHash)
    {
        EnsureDefaults();

        const int s = SlotIndex(playerType);
        if (s < 0) return false;

        const std::uint64_t n = g_Slot[s].nameHash.load(std::memory_order_relaxed);
        const std::uint64_t f = g_Slot[s].infoHash.load(std::memory_order_relaxed);
        if (n == 0 && f == 0) return false;

        if (outNameHash) *outNameHash = n;
        if (outInfoHash) *outInfoHash = f;
        return true;
    }

    void ResetAdoption()
    {
        for (auto& slot : g_Slot)
            slot.flowIndex.store(0, std::memory_order_relaxed);
    }
}
