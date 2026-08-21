#include "pch.h"

#include "AdditionalMotionTable_GetMtarPathId.h"
#include "OutfitRegistry.h"

#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"


namespace
{
    using GetMtarPathId_t     = std::uint64_t* (__fastcall*)(std::uint64_t*, std::uint32_t);
    using UpdateMotionBlock_t = void (__fastcall*)(void*);
    using GetFileForPathId_t  = void* (__fastcall*)(void*, std::uint64_t);
    using GetBlockAtIndex_t   = void* (__fastcall*)(void*, std::uint32_t);
    using AdditionalMtarAll_t = void (__fastcall*)(void*, std::uint32_t);

    static GetMtarPathId_t     g_OrigGetMtarPathId        = nullptr;
    static UpdateMotionBlock_t g_OrigUpdateMotionBlock    = nullptr;
    static GetFileForPathId_t  g_OrigBlockGetFileForPathId = nullptr;
    static GetBlockAtIndex_t   g_GetBlockAtIndex          = nullptr;
    static AdditionalMtarAll_t g_OrigAddMtarAll           = nullptr;
    static AdditionalMtarAll_t g_RemoveMtarAll            = nullptr;
    static bool                g_Installed                = false;
    static bool                g_FallbackReady            = false;

    constexpr std::size_t      kMaxPlayerImpls   = 4;
    constexpr std::size_t      kImplAddedMaskOff = 0x680;
    static std::atomic<void*>  g_PlayerImpls[kMaxPlayerImpls] = {};

    constexpr std::size_t      kMaxMotionCtls = 4;
    static std::atomic<void*>  g_MotionBlockCtls[kMaxMotionCtls] = {};
    static thread_local bool   g_InMotionFallback = false;

    constexpr std::size_t     kMaxSeenBlocks = 96;
    static std::atomic<void*> g_SeenBlocks[kMaxSeenBlocks] = {};

    static void RememberBlock(void* block)
    {
        if (!block)
            return;
        for (std::size_t i = 0; i < kMaxSeenBlocks; ++i)
        {
            void* cur = g_SeenBlocks[i].load(std::memory_order_acquire);
            if (cur == block)
                return;
            if (cur == nullptr)
            {
                void* expected = nullptr;
                if (g_SeenBlocks[i].compare_exchange_strong(
                        expected, block, std::memory_order_acq_rel))
                    return;
                if (expected == block)
                    return;
            }
        }
    }

    constexpr std::size_t kCtlLoaderOff     = 0xB8;
    constexpr std::size_t kCtlMainLoaderOff = 0x160;
    constexpr std::size_t kLoaderGroupOff   = 0x50;
    constexpr std::size_t kGroupCountOff    = 0x1C;
    constexpr std::size_t kCtlWalkStateOff  = 0x124;
    constexpr std::size_t kCtlWalkIndexOff  = 0x128;
    constexpr std::uint32_t kMotionTableCount = 32;

    struct MotionEntrySwap
    {
        void* original;
        void* written;
        bool  valid;
    };
    static MotionEntrySwap            g_EntrySwaps[kMotionTableCount] = {};
    static std::atomic<int>           g_SwapsLive{ 0 };
    static std::atomic<std::uint32_t> g_SwapEpochMission{ 0xFFFFFFFFu };


    static std::uint64_t ResolveDesiredHash_SEH(
        std::uint64_t currentHash, int* outSlot)
    {
        if (outSlot) *outSlot = -1;
        if (!g_FallbackReady)
            return 0;
        if (currentHash == 0)
            return 0;

        int slot = outfit::MotionMtarSlotFromVanillaHash(currentHash);
        if (slot < 0 && !outfit::IsMotionMtarOverrideHash(currentHash, &slot))
            return 0;
        if (slot < 0)
            return 0;
        if (outSlot) *outSlot = slot;

        std::uint64_t desired = 0;
        __try
        {
            std::uint8_t parts = outfit::ReadLivePartsType();
            std::uint8_t pt    = outfit::ReadLivePlayerType();
            if (parts < outfit::kCustomPartsTypeStart
                || parts > outfit::kCustomPartsTypeEnd)
            {
                parts = outfit::GetMotionOutfitHintPartsType();
                pt    = outfit::GetMotionOutfitHintPlayerType();
            }
            if (parts >= outfit::kCustomPartsTypeStart
                && parts <= outfit::kCustomPartsTypeEnd)
            {
                const outfit::OutfitEntry* entry = nullptr;
                if (outfit::TryGetOutfitByPartsType(parts, &entry) && entry)
                {
                    desired = entry->GetMotionMtarOverride(
                        pt, static_cast<std::size_t>(slot));
                    if (desired == 0)
                    {
                        const std::uint8_t firstPt =
                            entry->FirstSupportedPlayerType();
                        if (firstPt != pt)
                            desired = entry->GetMotionMtarOverride(
                                firstPt, static_cast<std::size_t>(slot));
#ifdef _DEBUG
                        if (desired == 0)
                        {
                            static std::atomic<int> s_missCount{ 0 };
                            if (s_missCount.fetch_add(1) < 40)
                                LogDebug("[OutfitMotionMtar] no override: parts=0x%02X "
                                    "pt=%u firstSupportedPt=%u slot=%d - the resolved "
                                    "outfit declares no motionMtars entry for this "
                                    "archive slot on either branch\n",
                                    static_cast<unsigned>(parts),
                                    static_cast<unsigned>(pt),
                                    static_cast<unsigned>(firstPt), slot);
                        }
#endif
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            desired = 0;
        }

        if (desired == 0)
            desired = outfit::MotionMtarVanillaHash(static_cast<std::size_t>(slot));
        if (desired == 0 || desired == currentHash)
            return 0;
        return desired;
    }

    static std::uint64_t* __fastcall hkGetMtarPathId(
        std::uint64_t* outPath, std::uint32_t index)
    {
        MISSION_GUARD_ORIGINAL_RET(g_OrigGetMtarPathId, outPath, index);

        std::uint64_t* r = g_OrigGetMtarPathId(outPath, index);
        if (!r)
            return r;

#ifdef _DEBUG
        {
            static std::atomic<int> s_probeCount{ 0 };
            if (s_probeCount.fetch_add(1) < 200)
            {
                std::uint64_t seen = 0;
                __try { seen = *r; } __except (EXCEPTION_EXECUTE_HANDLER) { seen = 0; }
                LogDebug("[OutfitMotionMtar] probe idx=%u pathId=%016llX slot=%d "
                    "(pathId 0 = empty entry the engine skips; slot -1 = this "
                    "pathId is NOT one of the 32 vanilla player2 archive paths, "
                    "so motionMtars can never match it)\n",
                    index, seen, outfit::MotionMtarSlotFromVanillaHash(seen));
            }
        }
#endif

        const std::uint64_t original = *r;
        int slot = -1;
        const std::uint64_t desired = ResolveDesiredHash_SEH(original, &slot);
        if (desired != 0)
        {
            *r = desired;
#ifdef _DEBUG
            static std::atomic<int> s_applyCount{ 0 };
            if (s_applyCount.fetch_add(1) < 64)
                LogDebug("[OutfitMotionMtar] REDIRECT idx=%u slot=%d %016llX -> "
                    "%016llX (hintParts=0x%02X liveParts=0x%02X)\n",
                    index, slot, original, desired,
                    static_cast<unsigned>(outfit::GetMotionOutfitHintPartsType()),
                    static_cast<unsigned>(outfit::ReadLivePartsType()));
#endif
        }

        return r;
    }

    static void __fastcall hkUpdateAdditionalMotionBlock(void* self)
    {
        for (std::size_t i = 0; i < kMaxMotionCtls; ++i)
        {
            void* cur = g_MotionBlockCtls[i].load(std::memory_order_acquire);
            if (cur == self)
                break;
            if (cur == nullptr)
            {
                void* expected = nullptr;
                if (g_MotionBlockCtls[i].compare_exchange_strong(
                        expected, self, std::memory_order_acq_rel))
                    break;
                if (expected == self)
                    break;
            }
        }
        g_OrigUpdateMotionBlock(self);
    }

    static void __fastcall hkAddAdditionalMtarAll(void* self, std::uint32_t group)
    {
        if (self)
        {
            for (std::size_t i = 0; i < kMaxPlayerImpls; ++i)
            {
                void* cur = g_PlayerImpls[i].load(std::memory_order_acquire);
                if (cur == self)
                    break;
                if (cur == nullptr)
                {
                    void* expected = nullptr;
                    if (g_PlayerImpls[i].compare_exchange_strong(
                            expected, self, std::memory_order_acq_rel))
                        break;
                    if (expected == self)
                        break;
                }
            }
        }
        g_OrigAddMtarAll(self, group);
    }

    static int DetachLiveMtarGroups_SEH(std::uint32_t* savedMasks)
    {
        if (!g_RemoveMtarAll || !g_OrigAddMtarAll)
            return 0;

        int detached = 0;
        for (std::size_t i = 0; i < kMaxPlayerImpls; ++i)
        {
            void* impl = g_PlayerImpls[i].load(std::memory_order_acquire);
            if (!impl)
                continue;
            __try
            {
                const std::uint32_t mask = *reinterpret_cast<std::uint32_t*>(
                    static_cast<std::uint8_t*>(impl) + kImplAddedMaskOff);
                savedMasks[i] = mask;
                for (std::uint32_t g = 0; g < 32; ++g)
                {
                    if ((mask & (1u << g)) == 0)
                        continue;
                    g_RemoveMtarAll(impl, g);
                    ++detached;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                savedMasks[i] = 0;
            }
        }
        return detached;
    }

    static int ReattachLiveMtarGroups_SEH(const std::uint32_t* savedMasks)
    {
        if (!g_RemoveMtarAll || !g_OrigAddMtarAll)
            return 0;

        int reattached = 0;
        for (std::size_t i = 0; i < kMaxPlayerImpls; ++i)
        {
            void* impl = g_PlayerImpls[i].load(std::memory_order_acquire);
            if (!impl || savedMasks[i] == 0)
                continue;
            __try
            {
                for (std::uint32_t g = 0; g < 32; ++g)
                {
                    if ((savedMasks[i] & (1u << g)) == 0)
                        continue;
                    g_OrigAddMtarAll(impl, g);
                    ++reattached;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return reattached;
    }

    struct SearchHit
    {
        int   ctlIndex   = -1;
        int   loaderOff  = -1;
        int   blockIndex = -1;
        bool  fromSeen   = false;
        void* block      = nullptr;
    };

    static void* SearchPlayerBlocksForPathId_SEH(
        void* skipBlock, std::uint64_t pathId, SearchHit* hit = nullptr)
    {
        static const std::size_t kLoaderOffsets[] = {
            kCtlMainLoaderOff, kCtlLoaderOff };

        void* found = nullptr;
        __try
        {
            for (std::size_t ci = 0; ci < kMaxMotionCtls && !found; ++ci)
            {
                void* ctl = g_MotionBlockCtls[ci].load(std::memory_order_acquire);
                if (!ctl)
                    continue;

                for (std::size_t li = 0;
                     li < sizeof(kLoaderOffsets) / sizeof(kLoaderOffsets[0])
                         && !found;
                     ++li)
                {
                    auto* loader = *reinterpret_cast<std::uint8_t**>(
                        static_cast<std::uint8_t*>(ctl) + kLoaderOffsets[li]);
                    if (!loader)
                        continue;

                    auto* group = *reinterpret_cast<std::uint8_t**>(
                        loader + kLoaderGroupOff);
                    if (!group)
                        continue;

                    std::uint32_t count = *reinterpret_cast<std::uint32_t*>(
                        group + kGroupCountOff);
                    if (count > 64)
                        count = 64;

                    for (std::uint32_t i = 0; i < count && !found; ++i)
                    {
                        void* blk = g_GetBlockAtIndex(group, i);
                        if (!blk || blk == skipBlock)
                            continue;
                        found = g_OrigBlockGetFileForPathId(blk, pathId);
                        if (found && hit)
                        {
                            hit->ctlIndex   = static_cast<int>(ci);
                            hit->loaderOff  = static_cast<int>(kLoaderOffsets[li]);
                            hit->blockIndex = static_cast<int>(i);
                            hit->block      = blk;
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            found = nullptr;
        }

        for (std::size_t i = 0; i < kMaxSeenBlocks && !found; ++i)
        {
            void* blk = g_SeenBlocks[i].load(std::memory_order_acquire);
            if (!blk || blk == skipBlock)
                continue;
            __try
            {
                found = g_OrigBlockGetFileForPathId(blk, pathId);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_SeenBlocks[i].store(nullptr, std::memory_order_release);
                found = nullptr;
            }
            if (found && hit)
            {
                hit->ctlIndex   = -1;
                hit->loaderOff  = -1;
                hit->blockIndex = static_cast<int>(i);
                hit->fromSeen   = true;
                hit->block      = blk;
            }
        }
        return found;
    }

    static void LogBlockCensus_SEH()
    {
#ifdef _DEBUG
        static const std::size_t kLoaderOffsets[] = {
            kCtlMainLoaderOff, kCtlLoaderOff };

        static std::atomic<int> s_censusCount{ 0 };
        if (s_censusCount.fetch_add(1) >= 8)
            return;

        std::size_t seen = 0;
        for (std::size_t i = 0; i < kMaxSeenBlocks; ++i)
            if (g_SeenBlocks[i].load(std::memory_order_acquire))
                ++seen;
        LogDebug("[OutfitMotionMtar] harvested block registry holds %zu of %zu block(s) "
            "(every block the engine has ever resolved a file in; the archive "
            "search sweeps these after the two player controller groups)\n",
            seen, kMaxSeenBlocks);

        for (std::size_t ci = 0; ci < kMaxMotionCtls; ++ci)
        {
            void* ctl = g_MotionBlockCtls[ci].load(std::memory_order_acquire);
            if (!ctl)
                continue;
            __try
            {
                unsigned mainCount = 0;
                unsigned addCount  = 0;
                void*    mainGroup = nullptr;
                void*    addGroup  = nullptr;
                for (std::size_t li = 0; li < 2; ++li)
                {
                    auto* loader = *reinterpret_cast<std::uint8_t**>(
                        static_cast<std::uint8_t*>(ctl) + kLoaderOffsets[li]);
                    if (!loader)
                        continue;
                    auto* group = *reinterpret_cast<std::uint8_t**>(
                        loader + kLoaderGroupOff);
                    if (!group)
                        continue;
                    const std::uint32_t n =
                        *reinterpret_cast<std::uint32_t*>(group + kGroupCountOff);
                    if (li == 0) { mainGroup = group; mainCount = n; }
                    else         { addGroup  = group; addCount  = n; }
                }
                LogDebug("[OutfitMotionMtar] block census: ctl#%zu=%p main(+0x160) "
                    "group=%p blocks=%u | additional(+0xB8) group=%p blocks=%u "
                    "(these are every block the archive search can reach; if the "
                    "resident player fpk that carries the custom mtars is not "
                    "mounted in one of them, no custom archive can ever resolve)\n",
                    ci, ctl, mainGroup, mainCount, addGroup, addCount);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
#endif
    }

    static void* __fastcall hkBlockGetFileForPathId(
        void* block, std::uint64_t pathId)
    {
        MISSION_GUARD_ORIGINAL_RET(g_OrigBlockGetFileForPathId, block, pathId);

        RememberBlock(block);

        if (g_InMotionFallback)
            return g_OrigBlockGetFileForPathId(block, pathId);

        const int askedSlot = g_FallbackReady
            ? outfit::MotionMtarSlotFromVanillaHash(pathId)
            : -1;
#ifdef _DEBUG
        if (askedSlot >= 0)
        {
            static std::atomic<std::uint32_t> s_askedMask{ 0 };
            const std::uint32_t bit = 1u << static_cast<unsigned>(askedSlot);
            if ((s_askedMask.fetch_or(bit) & bit) == 0)
                LogDebug("[OutfitMotionMtar] VANILLA-ASK slot=%d %016llX (first direct "
                    "block lookup of this vanilla player2 archive - slots that "
                    "never appear here are bound by some path other than "
                    "GetFileForPathId and cannot be redirected this way)\n",
                    askedSlot, pathId);
        }
#endif

        if (askedSlot >= 0)
        {
            int vanillaSlot = -1;
            const std::uint64_t custom =
                ResolveDesiredHash_SEH(pathId, &vanillaSlot);
            if (custom != 0 && outfit::IsMotionMtarOverrideHash(custom, nullptr))
            {
                g_InMotionFallback = true;
                void* sub = g_OrigBlockGetFileForPathId(block, custom);
                if (!sub)
                    sub = SearchPlayerBlocksForPathId_SEH(block, custom);
                g_InMotionFallback = false;
                if (sub)
                {
#ifdef _DEBUG
                    static std::atomic<int> s_subCount{ 0 };
                    if (s_subCount.fetch_add(1) < 64)
                        LogDebug("[OutfitMotionMtar] SUBSTITUTE-AT-LOOKUP slot=%d "
                            "%016llX -> %016llX (a consumer asked a block for the "
                            "VANILLA archive directly, bypassing the 32-entry "
                            "table - this is the only way to reach archives the "
                            "table never lists, e.g. player2_resident)\n",
                            vanillaSlot, pathId, custom);
#endif
                    return sub;
                }
#ifdef _DEBUG
                static std::atomic<int> s_subMiss{ 0 };
                if (s_subMiss.fetch_add(1) < 32)
                    LogDebug("[OutfitMotionMtar] SUBSTITUTE-MISS slot=%d %016llX -> "
                        "%016llX: the override resolved but the custom archive "
                        "is in no block reachable right now\n",
                        vanillaSlot, pathId, custom);
#endif
            }
#ifdef _DEBUG
            else
            {
                static std::atomic<int> s_gateMiss{ 0 };
                if (s_gateMiss.fetch_add(1) < 32)
                    LogDebug("[OutfitMotionMtar] NO-OVERRIDE-AT-ASK slot=%d %016llX "
                        "(liveParts=0x%02X hintParts=0x%02X) - the archive was "
                        "requested while this outfit was NOT the live suit, so "
                        "it binds vanilla and stays vanilla until the next load\n",
                        askedSlot, pathId,
                        static_cast<unsigned>(outfit::ReadLivePartsType()),
                        static_cast<unsigned>(
                            outfit::GetMotionOutfitHintPartsType()));
            }
#endif
        }

        g_InMotionFallback = true;
        void* file = g_OrigBlockGetFileForPathId(block, pathId);
        if (file)
        {
            g_InMotionFallback = false;
            return file;
        }

        int slot = -1;
        if (!outfit::IsMotionMtarOverrideHash(pathId, &slot))
        {
            g_InMotionFallback = false;
            return nullptr;
        }

        void* found = SearchPlayerBlocksForPathId_SEH(block, pathId);
        if (!found)
        {
            std::uint64_t vanillaHash = 0;
            if (slot >= 0)
                vanillaHash = outfit::MotionMtarVanillaHash(
                    static_cast<std::size_t>(slot));
            if (vanillaHash != 0)
                found = g_OrigBlockGetFileForPathId(block, vanillaHash);

            static std::atomic<int> s_missWarn{ 0 };
            if (s_missWarn.fetch_add(1) < 64)
                Log("[OutfitMotionMtar] WARN: custom motion mtar %016llX "
                    "(slot %d) was not found in any player block - %s. The "
                    "custom .mtar is missing from the installed game data or "
                    "lives in a package the player does not mount; put it "
                    "inside one of the player2 motion fpk packages\n",
                    pathId, slot,
                    found ? "serving the VANILLA archive for this slot so the "
                            "load can finish (motion stays vanilla)"
                          : "and the vanilla archive also failed to resolve, "
                            "so this table entry stays empty");
        }
#ifdef _DEBUG
        else
        {
            static std::atomic<int> s_fallbackCount{ 0 };
            if (s_fallbackCount.fetch_add(1) < 64)
                LogDebug("[OutfitMotionMtar] FALLBACK-RESOLVE %016llX (slot %d) "
                    "served from a sibling player block (the additional-motion "
                    "block only carries the 32 vanilla file entries)\n",
                    pathId, slot);
        }
#endif
        g_InMotionFallback = false;
        return found;
    }

    static void DirectSwapEntryFile_SEH(
        std::uint32_t idx, std::uint64_t* pathPtr, int* swapped, int* unresolved)
    {
        __try
        {
            const std::uint64_t hash = *pathPtr;
            if (hash == 0)
                return;
            int slot = -1;
            const bool custom = outfit::IsMotionMtarOverrideHash(hash, &slot);
            void** filePtr = reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(pathPtr) + 0x10);
            SearchHit hit;
            void* file = SearchPlayerBlocksForPathId_SEH(nullptr, hash, &hit);
            if (file)
            {
                if (*filePtr != file)
                {
                    if (idx < kMotionTableCount)
                    {
                        if (!g_EntrySwaps[idx].valid)
                            g_SwapsLive.fetch_add(1, std::memory_order_relaxed);
                        if (!g_EntrySwaps[idx].valid
                            || *filePtr != g_EntrySwaps[idx].written)
                            g_EntrySwaps[idx].original = *filePtr;
                        g_EntrySwaps[idx].valid = true;
                    }
                    *filePtr = file;
                    if (idx < kMotionTableCount)
                        g_EntrySwaps[idx].written = file;
                    ++*swapped;
                }
            }
            else if (custom)
            {
                ++*unresolved;
            }

#ifdef _DEBUG
            if (custom)
            {
                static std::atomic<int> s_provCount{ 0 };
                if (s_provCount.fetch_add(1) < 48)
                {
                    if (file && hit.fromSeen)
                        LogDebug("[OutfitMotionMtar] RESIDENT slot=%d %016llX found in "
                            "harvested block#%d %p (outside the two player "
                            "controller groups - this is the package that "
                            "actually carries the custom archive)\n",
                            slot, hash, hit.blockIndex, hit.block);
                    else if (file)
                        LogDebug("[OutfitMotionMtar] RESIDENT slot=%d %016llX found in "
                            "ctl#%d loader+0x%X block#%d - the custom archive IS "
                            "loaded and the table entry now points at it\n",
                            slot, hash, hit.ctlIndex, hit.loaderOff, hit.blockIndex);
                    else
                        LogDebug("[OutfitMotionMtar] NOT RESIDENT slot=%d %016llX - the "
                            "custom archive is in no block the player has mounted "
                            "right now; the table entry keeps whatever it held\n",
                            slot, hash);
                }
            }
#endif
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static int RevertSwappedEntries_SEH()
    {
        if (!g_OrigGetMtarPathId)
            return 0;

        int restored = 0;
        for (std::uint32_t idx = 0; idx < kMotionTableCount; ++idx)
        {
            if (!g_EntrySwaps[idx].valid)
                continue;
            __try
            {
                std::uint64_t out = 0;
                std::uint64_t* r = g_OrigGetMtarPathId(&out, idx);
                if (r)
                {
                    void** filePtr = reinterpret_cast<void**>(
                        reinterpret_cast<std::uint8_t*>(r) + 0x10);
                    if (*filePtr == g_EntrySwaps[idx].written)
                    {
                        *filePtr = g_EntrySwaps[idx].original;
                        ++restored;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            g_EntrySwaps[idx].original = nullptr;
            g_EntrySwaps[idx].written  = nullptr;
            g_EntrySwaps[idx].valid    = false;
        }
        g_SwapsLive.store(0, std::memory_order_relaxed);
        return restored;
    }
}

namespace outfit
{
    void RevertAdditionalMotionSwaps()
    {
        const std::uint32_t mission =
            static_cast<std::uint32_t>(MissionCodeGuard::GetCurrentMissionCode());

        const int live = g_SwapsLive.load(std::memory_order_relaxed);
        if (!g_Installed || live == 0)
        {
            g_SwapEpochMission.store(mission, std::memory_order_relaxed);
            static std::atomic<int> s_idle{ 0 };
            if (s_idle.fetch_add(1, std::memory_order_relaxed) < 16)
                Log("[OutfitMotionMtar] mission is now %u - no archive entry to "
                    "restore (installed=%d, %d entr%s swapped since the last "
                    "revert), so this load starts with the engine's own file "
                    "pointers in the table.\n",
                    mission, g_Installed ? 1 : 0, live, live == 1 ? "y" : "ies");
            return;
        }

        const int restored = RevertSwappedEntries_SEH();
        g_SwapEpochMission.store(mission, std::memory_order_relaxed);

        static std::atomic<int> s_done{ 0 };
        if (s_done.fetch_add(1, std::memory_order_relaxed) < 16)
            Log("[OutfitMotionMtar] mission is now %u - %d of %d swapped player "
                "archive entr%s restored to the engine's own file pointer; any "
                "remainder already held a pointer the engine itself rebound and "
                "was left alone. Our pointers come from blocks outside the "
                "player's own controller groups, and such a block is unloaded on "
                "a mission change.\n",
                mission, restored, live, live == 1 ? "y" : "ies");
    }

    void RequestAdditionalMotionReresolve()
    {
        const std::uint32_t mission =
            static_cast<std::uint32_t>(MissionCodeGuard::GetCurrentMissionCode());
        if (g_SwapEpochMission.load(std::memory_order_relaxed) != mission)
            RevertAdditionalMotionSwaps();

        MISSION_GUARD_RETURN_VOID();
        if (!g_FallbackReady)
            return;

        int touched = 0;
        for (std::size_t ci = 0; ci < kMaxMotionCtls; ++ci)
        {
            void* ctl = g_MotionBlockCtls[ci].load(std::memory_order_acquire);
            if (!ctl)
                continue;
            __try
            {
                auto* base = static_cast<std::uint8_t*>(ctl);
                if (*reinterpret_cast<void**>(base + kCtlLoaderOff) == nullptr)
                    continue;
                *reinterpret_cast<std::uint32_t*>(base + kCtlWalkIndexOff) = 0;
                *reinterpret_cast<std::uint32_t*>(base + kCtlWalkStateOff) = 0;
                ++touched;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        LogBlockCensus_SEH();

        std::uint32_t savedMasks[kMaxPlayerImpls] = {};
        int detached = DetachLiveMtarGroups_SEH(savedMasks);

        int swapped    = 0;
        int unresolved = 0;
        const bool wasInFallback = g_InMotionFallback;
        g_InMotionFallback = true;
        for (std::uint32_t idx = 0; idx < kMotionTableCount; ++idx)
        {
            std::uint64_t out = 0;
            std::uint64_t* r = hkGetMtarPathId(&out, idx);
            if (!r)
                continue;
            DirectSwapEntryFile_SEH(idx, r, &swapped, &unresolved);
        }
        g_InMotionFallback = wasInFallback;

        int reattached = ReattachLiveMtarGroups_SEH(savedMasks);

#ifdef _DEBUG
        {
            static std::atomic<int> s_reresolveCount{ 0 };
            if (s_reresolveCount.fetch_add(1) < 32)
                LogDebug("[OutfitMotionMtar] re-resolve: walked the archive table "
                    "directly (%d file(s) swapped, %d custom entr%s not found "
                    "in any player block, walk counters reset on %d loading "
                    "controller(s), %d live mtar group(s) detached / %d "
                    "re-attached to the player motion holder)\n",
                    swapped, unresolved, unresolved == 1 ? "y" : "ies",
                    touched, detached, reattached);
        }
#endif
    }

    bool Install_OutfitMotionMtar_Hook()
    {
        if (g_Installed) return true;

        void* target = ResolveGameAddress(gAddr.AdditionalMotionTable_GetMtarPathId);
        if (!target)
        {
            LogDebug("[OutfitMotionMtar] target unresolved; module disabled "
                "(motionMtars overrides will not apply)\n");
            return false;
        }

        g_Installed = CreateAndEnableHook(
            target,
            reinterpret_cast<void*>(&hkGetMtarPathId),
            reinterpret_cast<void**>(&g_OrigGetMtarPathId));

        if (!g_Installed)
        {
            Log("[OutfitMotionMtar] hook install FAILED (target=%p); motionMtars "
                "overrides will not apply\n", target);
            return false;
        }

        void* updateTarget = ResolveGameAddress(
            gAddr.BlockControllerImpl_UpdateAdditionalMotionBlock);
        void* getFileTarget = ResolveGameAddress(gAddr.Fox_Block_GetFileForPathId);
        void* getBlockAt    = ResolveGameAddress(gAddr.Fox_BlockGroup_GetBlockAtIndex);

        if (!updateTarget || !getFileTarget || !getBlockAt)
        {
            Log("[OutfitMotionMtar] WARN: fallback resolver unresolved "
                "(update=%p getFile=%p getBlockAt=%p) - motionMtars redirects "
                "are SUPPRESSED on this build, because without the fallback a "
                "redirected archive the additional-motion block cannot see "
                "leaves table entry 0 empty and the mission load waits forever\n",
                updateTarget, getFileTarget, getBlockAt);
            return g_Installed;
        }

        g_GetBlockAtIndex = reinterpret_cast<GetBlockAtIndex_t>(getBlockAt);

        const bool updateOk = CreateAndEnableHook(
            updateTarget,
            reinterpret_cast<void*>(&hkUpdateAdditionalMotionBlock),
            reinterpret_cast<void**>(&g_OrigUpdateMotionBlock));
        const bool getFileOk = updateOk && CreateAndEnableHook(
            getFileTarget,
            reinterpret_cast<void*>(&hkBlockGetFileForPathId),
            reinterpret_cast<void**>(&g_OrigBlockGetFileForPathId));

        if (!updateOk || !getFileOk)
        {
            Log("[OutfitMotionMtar] WARN: fallback hook install FAILED "
                "(update=%d getFile=%d) - motionMtars redirects are SUPPRESSED "
                "so mission loads cannot hang on an unresolvable archive\n",
                updateOk ? 1 : 0, getFileOk ? 1 : 0);
            if (updateOk)
                DisableAndRemoveHook(updateTarget);
            g_OrigUpdateMotionBlock = nullptr;
            return g_Installed;
        }

        g_FallbackReady = true;

        void* addAllTarget = ResolveGameAddress(
            gAddr.Player2Impl_AddAdditionalMtarAll);
        void* removeAll = ResolveGameAddress(
            gAddr.Player2Impl_RemoveAdditionalMtarAll);

        if (!addAllTarget || !removeAll)
        {
            Log("[OutfitMotionMtar] WARN: Player2Impl Add/RemoveAdditionalMtarAll "
                "unresolved (add=%p remove=%p) - a mid-session outfit change can "
                "rewrite the archive table but cannot re-push it into the live "
                "player motion holder, so a swapped-in custom mtar only takes "
                "effect on the next full player rebuild\n",
                addAllTarget, removeAll);
            return g_Installed;
        }

        if (!CreateAndEnableHook(
                addAllTarget,
                reinterpret_cast<void*>(&hkAddAdditionalMtarAll),
                reinterpret_cast<void**>(&g_OrigAddMtarAll)))
        {
            Log("[OutfitMotionMtar] WARN: AddAdditionalMtarAll hook install "
                "FAILED (target=%p) - the live player motion holder cannot be "
                "re-pushed after a mid-session archive swap\n", addAllTarget);
            g_OrigAddMtarAll = nullptr;
            return g_Installed;
        }

        g_RemoveMtarAll = reinterpret_cast<AdditionalMtarAll_t>(removeAll);
        return g_Installed;
    }

    void Uninstall_OutfitMotionMtar_Hook()
    {
        if (!g_Installed) return;
        g_FallbackReady = false;
        if (g_OrigAddMtarAll)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.Player2Impl_AddAdditionalMtarAll))
                DisableAndRemoveHook(t);
            g_OrigAddMtarAll = nullptr;
        }
        g_RemoveMtarAll = nullptr;
        for (std::size_t i = 0; i < kMaxPlayerImpls; ++i)
            g_PlayerImpls[i].store(nullptr, std::memory_order_release);
        if (void* t = ResolveGameAddress(gAddr.Fox_Block_GetFileForPathId))
            DisableAndRemoveHook(t);
        if (void* t = ResolveGameAddress(
                gAddr.BlockControllerImpl_UpdateAdditionalMotionBlock))
            DisableAndRemoveHook(t);
        if (void* t = ResolveGameAddress(gAddr.AdditionalMotionTable_GetMtarPathId))
            DisableAndRemoveHook(t);
        g_OrigBlockGetFileForPathId = nullptr;
        g_OrigUpdateMotionBlock     = nullptr;
        g_OrigGetMtarPathId         = nullptr;
        for (std::size_t i = 0; i < kMaxMotionCtls; ++i)
            g_MotionBlockCtls[i].store(nullptr, std::memory_order_release);
        g_Installed = false;
    }
}
