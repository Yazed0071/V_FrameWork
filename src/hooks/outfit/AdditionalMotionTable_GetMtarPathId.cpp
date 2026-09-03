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
    constexpr std::size_t kMtarPathOff      = 0x30;
    constexpr std::size_t kMtarDataOff      = 0x98;
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

    struct DirectSlotBinding
    {
        std::atomic<void*>         bound{ nullptr };
        std::atomic<std::uint64_t> boundHash{ 0 };
        std::atomic<void*>         desired{ nullptr };
    };
    static DirectSlotBinding          g_DirectSlots[kMotionTableCount];
    static std::atomic<std::uint32_t> g_DirectSlotMask{ 0 };
    static std::atomic<int>           g_CensusPending{ 0 };

    using MtarAdd_t    = int  (__fastcall*)(void*, void*);
    using MtarRemove_t = void (__fastcall*)(void*, void*);
    static MtarAdd_t    g_AddMtar    = nullptr;
    static MtarRemove_t g_RemoveMtar = nullptr;

    constexpr std::size_t kAnimCtlCountOff  = 0x1C;
    constexpr std::size_t kAnimCtlArrayOff  = 0x98;
    constexpr std::size_t kAnimEntryStride  = 0x30;
    constexpr std::size_t kAnimEntryFileOff = 0x18;

    constexpr std::size_t kMaxAnimCtls = 8;
    static std::atomic<void*> g_AnimCtls[kMaxAnimCtls]{};

    constexpr std::size_t kMaxMtarHolders = 16;
    static std::atomic<void*> g_MtarHolders[kMaxMtarHolders]{};

    static void NoteDirectSlotBinding(int slot, void* file, std::uint64_t hash)
    {
        if (slot < 0 || static_cast<std::uint32_t>(slot) >= kMotionTableCount || !file)
            return;
        g_DirectSlots[slot].boundHash.store(hash, std::memory_order_release);
        g_DirectSlots[slot].bound.store(file, std::memory_order_release);
        g_DirectSlotMask.fetch_or(1u << static_cast<unsigned>(slot),
                                  std::memory_order_acq_rel);
    }

    constexpr int kReresolveMaxDeferTicks = 120;
    static std::atomic<bool> g_PartsPipelineBusy{ false };
    static std::atomic<bool> g_ReresolvePending{ false };
    static std::atomic<int>  g_ReresolveDeferTicks{ 0 };


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
                    const std::uint8_t varIdx = entry->HasVariants()
                        ? outfit::GetActiveVariantLockFree(parts)
                        : std::uint8_t{ 0 };
                    desired = entry->GetMotionMtarOverride(
                        pt, static_cast<std::size_t>(slot), varIdx);
                    if (desired == 0)
                    {
                        const std::uint8_t firstPt =
                            entry->FirstSupportedPlayerType();
                        if (firstPt != pt)
                            desired = entry->GetMotionMtarOverride(
                                firstPt, static_cast<std::size_t>(slot), varIdx);
#ifdef _DEBUG
                        if (desired == 0)
                        {
                            static std::atomic<int> s_missCount{ 0 };
                            if (s_missCount.fetch_add(1) < 40)
                                LogDebug("[OutfitMotionMtar] no override: "
                                         "parts=0x%02X pt=%u firstSupportedPt=%u "
                                         "variant=%u slot=%d - neither that variant "
                                         "nor either branch declares a motionMtars "
                                         "entry for this slot\n",
                                    static_cast<unsigned>(parts),
                                    static_cast<unsigned>(pt),
                                    static_cast<unsigned>(firstPt),
                                    static_cast<unsigned>(varIdx), slot);
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
                         "(pathId 0 = empty entry; slot -1 = not one of the 32 "
                         "vanilla player2 paths, so motionMtars can never match "
                         "it)\n",
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
        void* weak       = nullptr;
    };

    static bool MtarArchiveResident(void* file)
    {
        if (!file)
            return false;
        bool ok = false;
        __try
        {
            ok = *reinterpret_cast<void**>(
                     static_cast<std::uint8_t*>(file) + kMtarDataOff) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    static void* TakeArchiveFromBlock(void* blk, std::uint64_t pathId,
                                      bool requireResident, void** weak)
    {
        void* f = g_OrigBlockGetFileForPathId(blk, pathId);
        if (!f)
            return nullptr;
        if (requireResident && !MtarArchiveResident(f))
        {
            if (weak && !*weak)
                *weak = f;
            return nullptr;
        }
        return f;
    }

    static void* SearchPlayerBlocksForPathId_SEH(
        void* skipBlock, std::uint64_t pathId, SearchHit* hit = nullptr,
        bool requireResident = false)
    {
        void* weakHit = nullptr;
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
                        found = TakeArchiveFromBlock(
                            blk, pathId, requireResident, &weakHit);
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
                found = TakeArchiveFromBlock(
                    blk, pathId, requireResident, &weakHit);
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

        if (hit)
            hit->weak = weakHit;
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
        LogDebug("[OutfitMotionMtar] harvested block registry holds %zu of %zu "
                 "block(s) (swept after the two player controller groups)\n",
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
                         "- if the fpk carrying the custom mtars is in none of "
                         "these, no custom archive can resolve\n",
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
                LogDebug("[OutfitMotionMtar] VANILLA-ASK slot=%d %016llX (first "
                         "direct block lookup; slots that never appear here are "
                         "bound by some path other than GetFileForPathId)\n",
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
                                 "%016llX -> %016llX (a consumer asked a block "
                                 "directly, bypassing the 32-entry table - the only "
                                 "way to reach archives the table never lists)\n",
                            vanillaSlot, pathId, custom);
#endif
                    NoteDirectSlotBinding(askedSlot, sub, custom);
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
                             "(liveParts=0x%02X hintParts=0x%02X) - no override "
                             "resolved for this slot, so it binds vanilla until the "
                             "next load; when liveParts already matches the outfit's "
                             "partsType the cause is the preceding 'no override' "
                             "line, not the timing\n",
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
            NoteDirectSlotBinding(askedSlot, file, pathId);
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
                Log("[OutfitMotionMtar] WARN: custom motion mtar %016llX (slot %d) "
                    "is in no player block - %s. The .mtar is missing or lives in a "
                    "package the player does not mount; put it inside a player2 "
                    "motion fpk\n",
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
                         "block carries only the 32 vanilla entries)\n",
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
            else
            {
                if (custom)
                    ++*unresolved;
                if (idx < kMotionTableCount
                    && g_EntrySwaps[idx].valid
                    && g_EntrySwaps[idx].original != nullptr
                    && *filePtr == g_EntrySwaps[idx].written)
                {
                    *filePtr = g_EntrySwaps[idx].original;
                    g_EntrySwaps[idx].original = nullptr;
                    g_EntrySwaps[idx].written  = nullptr;
                    g_EntrySwaps[idx].valid    = false;
                    g_SwapsLive.fetch_sub(1, std::memory_order_relaxed);
                    ++*swapped;
                }
            }

#ifdef _DEBUG
            if (custom)
            {
                static std::atomic<int> s_provCount{ 0 };
                if (s_provCount.fetch_add(1) < 48)
                {
                    if (file && hit.fromSeen)
                        LogDebug("[OutfitMotionMtar] RESIDENT slot=%d %016llX found "
                                 "in harvested block#%d %p (outside the player "
                                 "controller groups)\n",
                            slot, hash, hit.blockIndex, hit.block);
                    else if (file)
                        LogDebug("[OutfitMotionMtar] RESIDENT slot=%d %016llX found "
                                 "in ctl#%d loader+0x%X block#%d - the table entry "
                                 "now points at it\n",
                            slot, hash, hit.ctlIndex, hit.loaderOff, hit.blockIndex);
                    else
                        LogDebug("[OutfitMotionMtar] NOT RESIDENT slot=%d %016llX - "
                                 "in no block the player has mounted; the table "
                                 "entry is unchanged\n",
                            slot, hash);
                }
            }
#endif
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void RefreshDirectSlotTargets()
    {
        const std::uint32_t mask = g_DirectSlotMask.load(std::memory_order_acquire);
        if (mask == 0)
            return;
        for (std::uint32_t slot = 0; slot < kMotionTableCount; ++slot)
        {
            if ((mask & (1u << slot)) == 0)
                continue;
            const std::uint64_t vanillaHash =
                outfit::MotionMtarVanillaHash(static_cast<std::size_t>(slot));
            if (vanillaHash == 0)
                continue;
            int resolved = -1;
            std::uint64_t want = ResolveDesiredHash_SEH(vanillaHash, &resolved);
            if (want == 0)
                want = vanillaHash;
            const std::uint64_t haveHash =
                g_DirectSlots[slot].boundHash.load(std::memory_order_acquire);
            if (haveHash == 0 || want == haveHash)
            {
                g_DirectSlots[slot].desired.store(nullptr,
                                                 std::memory_order_release);
                continue;
            }
            SearchHit hit{};
            void* file =
                SearchPlayerBlocksForPathId_SEH(nullptr, want, &hit, true);
            if (!file)
            {
                g_DirectSlots[slot].desired.store(nullptr,
                                                 std::memory_order_release);
                static std::atomic<int> s_dead{ 0 };
                if (s_dead.fetch_add(1, std::memory_order_relaxed) < 8)
                    Log("[OutfitMotionMtar] slot %u cannot be re-pointed at "
                        "%016llX - %s, so the archive it bound at level load "
                        "keeps playing until the next load\n",
                        slot, want,
                        hit.weak
                            ? "every copy of that path found has no resident "
                              "data, so it would answer no clip at all"
                            : "no loaded block holds that path");
                continue;
            }
            void* prev = g_DirectSlots[slot].desired.exchange(
                file, std::memory_order_acq_rel);
#ifdef _DEBUG
            if (prev != file)
            {
                static std::atomic<int> s_retarget{ 0 };
                if (s_retarget.fetch_add(1, std::memory_order_relaxed) < 32)
                    LogDebug("[OutfitMotionMtar] RETARGET slot=%u %016llX -> "
                             "%016llX file=%p (was %p) - this slot is bound once "
                             "per level load, so its clips are re-pointed at fetch "
                             "time; a slot reaches here only when the wanted "
                             "archive is a different path from the one it bound\n",
                        slot, haveHash, want, file, prev);
            }
#else
            (void)prev;
#endif
        }
    }

    static void ClearDirectSlotBindings()
    {
        for (std::uint32_t slot = 0; slot < kMotionTableCount; ++slot)
        {
            g_DirectSlots[slot].bound.store(nullptr, std::memory_order_release);
            g_DirectSlots[slot].boundHash.store(0, std::memory_order_release);
            g_DirectSlots[slot].desired.store(nullptr, std::memory_order_release);
        }
        g_DirectSlotMask.store(0, std::memory_order_release);
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
    static std::uint64_t ReadMtarPath_SEH(void* mtar)
    {
        std::uint64_t path = 0;
        __try
        {
            path = *reinterpret_cast<std::uint64_t*>(
                static_cast<std::uint8_t*>(mtar) + kMtarPathOff);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            path = 0;
        }
        return path;
    }

    static uintptr_t MtarAddAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4:  return 0x141a6b8f0ull;
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4:  return 0x141a6b860ull;
        case ::AddressSetRuntime::GameBuild::En_1_0_15_3:  return 0x141a6bb80ull;
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3:  return 0x141a6bca0ull;
        default:                                           return 0;
        }
    }

    static uintptr_t MtarRemoveAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4:  return 0x141a6cc90ull;
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4:  return 0x141a6cc00ull;
        case ::AddressSetRuntime::GameBuild::En_1_0_15_3:  return 0x141a6cf20ull;
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3:  return 0x141a6d040ull;
        default:                                           return 0;
        }
    }

    static void NoteCustomMtarHolder(void* holder)
    {
        if (!holder)
            return;
        for (std::size_t i = 0; i < kMaxMtarHolders; ++i)
        {
            void* cur = g_MtarHolders[i].load(std::memory_order_acquire);
            if (cur == holder)
                return;
            if (cur == nullptr)
            {
                void* expected = nullptr;
                if (g_MtarHolders[i].compare_exchange_strong(
                        expected, holder, std::memory_order_acq_rel))
                    return;
                if (expected == holder)
                    return;
            }
        }
    }

    static int __fastcall hkMtarAdd(void* holder, void* mtarFile)
    {
        const int r = g_AddMtar(holder, mtarFile);
        if (r != 0 && holder && mtarFile && AnyMotionMtarOverridesRegistered())
        {
            const std::uint64_t held = ReadMtarPath_SEH(mtarFile);
            if (held != 0 && IsMotionMtarOverrideHash(held, nullptr))
                NoteCustomMtarHolder(holder);
        }
        return r;
    }

    void NoteAnimControl(void* animControl)
    {
        if (!animControl)
            return;
        for (std::size_t i = 0; i < kMaxAnimCtls; ++i)
        {
            void* cur = g_AnimCtls[i].load(std::memory_order_acquire);
            if (cur == animControl)
                return;
            if (cur == nullptr)
            {
                void* expected = nullptr;
                if (g_AnimCtls[i].compare_exchange_strong(
                        expected, animControl, std::memory_order_acq_rel))
                    return;
                if (expected == animControl)
                    return;
            }
        }
    }

    struct CustomBinding
    {
        void*         file = nullptr;
        std::uint64_t hash = 0;
        int           slot = -1;
    };

    static int CollectCustomBindings_SEH(
        void* ac, CustomBinding* out, int maxOut)
    {
        int found = 0;
        __try
        {
            auto* base = static_cast<std::uint8_t*>(ac);
            const std::uint32_t count =
                *reinterpret_cast<std::uint32_t*>(base + kAnimCtlCountOff);
            auto* arr = *reinterpret_cast<std::uint8_t**>(
                base + kAnimCtlArrayOff);
            if (!arr || count == 0 || count > 64)
                return 0;
            for (std::uint32_t i = 0; i < count && found < maxOut; ++i)
            {
                auto* e = arr + i * kAnimEntryStride;
                void* file = *reinterpret_cast<void**>(e + kAnimEntryFileOff);
                if (!file)
                    continue;
                const std::uint64_t held = *reinterpret_cast<std::uint64_t*>(
                    static_cast<std::uint8_t*>(file) + kMtarPathOff);
                int slot = -1;
                if (held == 0 || !outfit::IsMotionMtarOverrideHash(held, &slot))
                    continue;
                out[found].file = file;
                out[found].hash = held;
                out[found].slot = slot;
                ++found;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return found;
    }

    static bool IsFileBound_SEH(void* ac, void* file)
    {
        bool bound = false;
        __try
        {
            auto* base = static_cast<std::uint8_t*>(ac);
            const std::uint32_t count =
                *reinterpret_cast<std::uint32_t*>(base + kAnimCtlCountOff);
            auto* arr = *reinterpret_cast<std::uint8_t**>(
                base + kAnimCtlArrayOff);
            if (!arr || count > 64)
                return false;
            for (std::uint32_t i = 0; i < count && !bound; ++i)
                bound = *reinterpret_cast<void**>(
                    arr + i * kAnimEntryStride + kAnimEntryFileOff) == file;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            bound = false;
        }
        return bound;
    }

    static bool SwapBinding_SEH(void* ac, void* oldFile, void* newFile)
    {
        bool ok = false;
        __try
        {
            const bool alreadyBound = IsFileBound_SEH(ac, newFile);
            g_RemoveMtar(ac, oldFile);
            if (alreadyBound)
                ok = true;
            else if (g_AddMtar(ac, newFile) != 0)
                ok = true;
            else
            {
                g_AddMtar(ac, oldFile);
                ok = false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    static int RebindHolderToVanilla(void* ac)
    {
        CustomBinding held[32]{};
        const int n = CollectCustomBindings_SEH(ac, held, 32);
        if (n == 0)
            return 0;

        int swapped = 0;
        int stuck   = 0;
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t want = outfit::MotionMtarVanillaHash(
                static_cast<std::size_t>(held[i].slot));
            if (want == 0 || want == held[i].hash)
                continue;
            SearchHit hit{};
            void* vanilla =
                SearchPlayerBlocksForPathId_SEH(nullptr, want, &hit, true);
            if (!vanilla || vanilla == held[i].file)
            {
                ++stuck;
                static std::atomic<int> s_miss{ 0 };
                if (s_miss.fetch_add(1, std::memory_order_relaxed) < 8)
                    Log("[OutfitMotionMtar] slot %d keeps its custom archive "
                        "%016llX - the vanilla archive %016llX resolves in no "
                        "loaded block with resident data, so unbinding it would "
                        "leave that slot with no clips at all\n",
                        held[i].slot, held[i].hash, want);
                continue;
            }
            if (SwapBinding_SEH(ac, held[i].file, vanilla))
                ++swapped;
            else
            {
                ++stuck;
                static std::atomic<int> s_full{ 0 };
                if (s_full.fetch_add(1, std::memory_order_relaxed) < 4)
                    Log("[OutfitMotionMtar] slot %d could not take the vanilla "
                        "archive %016llX - the anim control has no free entry, so "
                        "the custom one was put back rather than leaving the slot "
                        "with no clips\n",
                        held[i].slot, want);
            }
        }

        if (swapped || stuck)
            Log("[OutfitMotionMtar] mtar holder %p: %d of %d custom archive(s) "
                "unbound and replaced with the vanilla one through the engine's "
                "own AddMtar/RemoveMtar%s\n",
                ac, swapped, n,
                stuck ? " (the rest are listed above)" : "");
        return n;
    }

    static void RebindAnimControlsToVanilla()
    {
        if (!g_AddMtar || !g_RemoveMtar)
        {
            static std::atomic<int> s_noAddr{ 0 };
            if (s_noAddr.fetch_add(1, std::memory_order_relaxed) < 1)
                Log("[OutfitMotionMtar] the custom archives cannot be unbound on "
                    "this build - fox::anim AddMtar/RemoveMtar resolved to nothing, "
                    "so the anim control keeps playing them until the next level "
                    "load\n");
            return;
        }

        void*       visited[kMaxMtarHolders + kMaxAnimCtls] = {};
        std::size_t visitedCount = 0;
        int         totalFound   = 0;

        for (std::size_t pass = 0; pass < 2; ++pass)
        {
            const std::size_t count = pass == 0 ? kMaxMtarHolders : kMaxAnimCtls;
            for (std::size_t i = 0; i < count; ++i)
            {
                void* ac = pass == 0
                    ? g_MtarHolders[i].load(std::memory_order_acquire)
                    : g_AnimCtls[i].load(std::memory_order_acquire);
                if (!ac)
                    continue;
                bool seen = false;
                for (std::size_t v = 0; v < visitedCount && !seen; ++v)
                    seen = visited[v] == ac;
                if (seen)
                    continue;
                visited[visitedCount++] = ac;
                totalFound += RebindHolderToVanilla(ac);
            }
        }

        if (totalFound == 0 && g_SwapsLive.load(std::memory_order_relaxed) != 0)
        {
            static std::atomic<int> s_none{ 0 };
            if (s_none.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("[OutfitMotionMtar] none of the %zu mtar holder(s) we watched "
                    "take a custom archive still holds one, so there is nothing to "
                    "unbind - whatever is still playing the custom clips took them "
                    "by a route other than fox::anim AddMtar, and that motion stays "
                    "until the next level load\n",
                    visitedCount);
        }
    }

    bool ConsumeAnimControlCensusRequest()
    {
        int have = g_CensusPending.load(std::memory_order_acquire);
        while (have > 0)
        {
            if (g_CensusPending.compare_exchange_weak(
                    have, have - 1, std::memory_order_acq_rel))
                return true;
        }
        return false;
    }

    void* RedirectMotionMtarForClip(void* mtar)
    {
        if (!mtar)
            return nullptr;
        const std::uint32_t mask = g_DirectSlotMask.load(std::memory_order_acquire);
        if (mask == 0)
            return nullptr;

        const std::uint64_t livePath = ReadMtarPath_SEH(mtar);
        bool armed = false;

        for (std::uint32_t slot = 0; slot < kMotionTableCount; ++slot)
        {
            if ((mask & (1u << slot)) == 0)
                continue;
            void* want = g_DirectSlots[slot].desired.load(std::memory_order_acquire);
            if (!want || want == mtar)
                continue;
            armed = true;

            const bool byPath =
                livePath != 0
                && livePath == g_DirectSlots[slot].boundHash.load(
                                   std::memory_order_acquire);
            const bool byPtr =
                g_DirectSlots[slot].bound.load(std::memory_order_acquire) == mtar;
            if (!byPath && !byPtr)
                continue;

#ifdef _DEBUG
            {
                static std::atomic<int> s_fired{ 0 };
                if (s_fired.fetch_add(1, std::memory_order_relaxed) < 4)
                    LogDebug("[OutfitMotionMtar] slot %u clip fetch on archive %016llX "
                             "is served from %p instead, matched by %s\n",
                        slot, livePath, want, byPath ? "path" : "pointer");
            }
#endif
            return want;
        }

#ifdef _DEBUG
        if (armed && livePath)
        {
            static std::atomic<int> s_seen{ 0 };
            if (s_seen.fetch_add(1, std::memory_order_relaxed) < 12)
                LogDebug("[OutfitMotionMtar] a re-point is armed but this clip fetch is "
                         "on archive %016llX (%p), which matches no re-pointed slot - "
                         "that is the archive the anim control actually holds\n",
                    livePath, mtar);
        }
#endif
        return nullptr;
    }

    void RevertAdditionalMotionSwaps()
    {
        g_PartsPipelineBusy.store(false, std::memory_order_relaxed);
        ClearDirectSlotBindings();
        for (std::size_t i = 0; i < kMaxMtarHolders; ++i)
            g_MtarHolders[i].store(nullptr, std::memory_order_release);

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
                    "revert)\n",
                    mission, g_Installed ? 1 : 0, live, live == 1 ? "y" : "ies");
            return;
        }

        const int restored = RevertSwappedEntries_SEH();
        g_SwapEpochMission.store(mission, std::memory_order_relaxed);

        static std::atomic<int> s_done{ 0 };
        if (s_done.fetch_add(1, std::memory_order_relaxed) < 16)
            Log("[OutfitMotionMtar] mission is now %u - %d of %d swapped archive "
                "entr%s restored to the engine's own pointer; the rest it had "
                "already rebound. Our pointers come from blocks outside the "
                "player's controller groups, which unload on a mission change\n",
                mission, restored, live, live == 1 ? "y" : "ies");
    }

    static void RunAdditionalMotionReresolve()
    {
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
        RefreshDirectSlotTargets();
        g_InMotionFallback = wasInFallback;

        int reattached = ReattachLiveMtarGroups_SEH(savedMasks);

#ifdef _DEBUG
        {
            static std::atomic<int> s_reresolveCount{ 0 };
            if (s_reresolveCount.fetch_add(1) < 32)
                LogDebug("[OutfitMotionMtar] re-resolve: walked the archive table "
                         "(%d file(s) swapped, %d custom entr%s not found in any "
                         "player block, counters reset on %d loading controller(s), "
                         "%d mtar group(s) detached / %d re-attached)\n",
                    swapped, unresolved, unresolved == 1 ? "y" : "ies",
                    touched, detached, reattached);
        }
#endif
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
        if (!outfit::AnyMotionMtarOverridesRegistered())
            return;

        if (g_PartsPipelineBusy.load(std::memory_order_relaxed))
        {
            g_ReresolveDeferTicks.store(kReresolveMaxDeferTicks,
                                        std::memory_order_relaxed);
            g_ReresolvePending.store(true, std::memory_order_relaxed);
            return;
        }

        RunAdditionalMotionReresolve();
    }

    void NotePartsPipelineBusy(bool busy)
    {
        g_PartsPipelineBusy.store(busy, std::memory_order_relaxed);

        if (!g_ReresolvePending.load(std::memory_order_relaxed))
            return;
        if (MissionCodeGuard::ShouldBypassHooks())
            return;

        if (busy)
        {
            const int left =
                g_ReresolveDeferTicks.load(std::memory_order_relaxed);
            if (left > 1)
            {
                g_ReresolveDeferTicks.store(left - 1,
                                            std::memory_order_relaxed);
                return;
            }
            g_ReresolveDeferTicks.store(0, std::memory_order_relaxed);

            static std::atomic<int> s_lateLogged{ 0 };
            if (s_lateLogged.fetch_add(1, std::memory_order_relaxed) < 8)
                Log("[OutfitMotionMtar] a parts slot was still loading %d ticks "
                    "after the suit changed, so the motion archive re-bind ran "
                    "anyway - detaching the mtar groups under an in-flight load "
                    "can strand that slot, which then keeps the previous suit\n",
                    kReresolveMaxDeferTicks);
        }

        g_ReresolvePending.store(false, std::memory_order_relaxed);

        const std::uint32_t mission =
            static_cast<std::uint32_t>(MissionCodeGuard::GetCurrentMissionCode());
        if (g_SwapEpochMission.load(std::memory_order_relaxed) != mission)
        {
            RevertAdditionalMotionSwaps();
            return;
        }
        if (!g_FallbackReady) return;
        if (!outfit::AnyMotionMtarOverridesRegistered()) return;

        RunAdditionalMotionReresolve();
    }

    void NoteLiveOutfitIdentity(unsigned char partsType, bool settled)
    {
        if (!g_Installed || !g_FallbackReady)
            return;
        if (!settled)
            return;

        const std::uint8_t parts = static_cast<std::uint8_t>(partsType);
        if (parts >= outfit::kCustomPartsTypeStart
            && parts <= outfit::kCustomPartsTypeEnd)
            return;

        const std::uint8_t hint = outfit::GetMotionOutfitHintPartsType();
        const bool hintIsCustom = hint >= outfit::kCustomPartsTypeStart
                               && hint <= outfit::kCustomPartsTypeEnd;
        if (!hintIsCustom && g_SwapsLive.load(std::memory_order_relaxed) == 0)
            return;

        outfit::ClearMotionOutfitHint();

        static std::atomic<int> s_dropped{ 0 };
        if (s_dropped.fetch_add(1, std::memory_order_relaxed) < 12)
            Log("[OutfitMotionMtar] the live suit settled on vanilla parts type "
                "0x%02X, so the motion archive table is being put back - the "
                "outfit hint (0x%02X) is dropped and the %d swapped entr%s "
                "re-resolve to their vanilla archives; slots the engine binds "
                "once per level load, such as 'resident', are re-pointed at clip "
                "fetch instead\n",
                static_cast<unsigned>(parts), static_cast<unsigned>(hint),
                g_SwapsLive.load(std::memory_order_relaxed),
                g_SwapsLive.load(std::memory_order_relaxed) == 1 ? "y" : "ies");

        RequestAdditionalMotionReresolve();
        RebindAnimControlsToVanilla();
        g_CensusPending.store(4, std::memory_order_release);
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
            Log("[OutfitMotionMtar] WARN: fallback resolver unresolved (update=%p "
                "getFile=%p getBlockAt=%p) - motionMtars redirects SUPPRESSED; "
                "without it a redirected archive leaves table entry 0 empty and the "
                "mission load waits forever\n",
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
            Log("[OutfitMotionMtar] WARN: fallback hook install FAILED (update=%d "
                "getFile=%d) - motionMtars redirects SUPPRESSED so mission loads "
                "cannot hang\n",
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
                "rewrite the archive table but not re-push it, so a swapped mtar "
                "only applies on the next full player rebuild\n",
                addAllTarget, removeAll);
            return g_Installed;
        }

        if (!CreateAndEnableHook(
                addAllTarget,
                reinterpret_cast<void*>(&hkAddAdditionalMtarAll),
                reinterpret_cast<void**>(&g_OrigAddMtarAll)))
        {
            Log("[OutfitMotionMtar] WARN: AddAdditionalMtarAll hook install FAILED "
                "(target=%p) - the live player motion holder cannot be re-pushed "
                "after a mid-session archive swap\n", addAllTarget);
            g_OrigAddMtarAll = nullptr;
            return g_Installed;
        }

        g_RemoveMtarAll = reinterpret_cast<AdditionalMtarAll_t>(removeAll);

        void* addMtar    = ResolveGameAddress(MtarAddAddr());
        void* removeMtar = ResolveGameAddress(MtarRemoveAddr());
        if (addMtar && removeMtar)
        {
            g_RemoveMtar = reinterpret_cast<MtarRemove_t>(removeMtar);
            if (!CreateAndEnableHook(
                    addMtar,
                    reinterpret_cast<void*>(&hkMtarAdd),
                    reinterpret_cast<void**>(&g_AddMtar)))
            {
                g_AddMtar = reinterpret_cast<MtarAdd_t>(addMtar);
                Log("[OutfitMotionMtar] fox::anim AddMtar could not be hooked at "
                    "%p, so the holder each custom archive is bound into is never "
                    "recorded - a revert then reaches only the holders the clip "
                    "reader happened to touch, and the rest keep the custom motion "
                    "until the next level load\n", addMtar);
            }
        }
        else
        {
            Log("[OutfitMotionMtar] fox::anim AddMtar/RemoveMtar are unknown on "
                "this build, so a custom archive already bound into the anim "
                "control cannot be unbound mid-session - switching back to a "
                "vanilla outfit will keep the custom motion until the next "
                "level load\n");
        }

        return g_Installed;
    }

    void Uninstall_OutfitMotionMtar_Hook()
    {
        if (!g_Installed) return;
        g_FallbackReady = false;
        ClearDirectSlotBindings();
        if (g_OrigAddMtarAll)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.Player2Impl_AddAdditionalMtarAll))
                DisableAndRemoveHook(t);
            g_OrigAddMtarAll = nullptr;
        }
        g_RemoveMtarAll = nullptr;
        if (void* t = ResolveGameAddress(MtarAddAddr()))
            DisableAndRemoveHook(t);
        g_AddMtar       = nullptr;
        g_RemoveMtar    = nullptr;
        for (std::size_t i = 0; i < kMaxAnimCtls; ++i)
            g_AnimCtls[i].store(nullptr, std::memory_order_release);
        for (std::size_t i = 0; i < kMaxMtarHolders; ++i)
            g_MtarHolders[i].store(nullptr, std::memory_order_release);
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
