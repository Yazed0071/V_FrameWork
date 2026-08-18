#pragma once

#include <cstddef>
#include <cstdint>

namespace EquipIdCompression
{
    constexpr std::int32_t kCompressedSlotBound = 0x289;

    constexpr std::int32_t ComputeCompressed(std::int32_t equipId)
    {
        if (equipId < 0)         return -1;
        if (equipId < 0x400)     return equipId;
        if (equipId < 0x600)     return equipId - 0x1D0;
        return equipId - 0x380;
    }

    inline bool IsCompressedInBounds(std::int32_t compressed)
    {
        return compressed >= 0 && compressed < kCompressedSlotBound;
    }

    inline bool IsEquipIdSafeForNativeTable(std::int32_t equipId)
    {
        return IsCompressedInBounds(ComputeCompressed(equipId));
    }

    constexpr std::int32_t kItemBandFirst        = 1;
    constexpr std::int32_t kItemBandLast         = 0x22F;
    constexpr std::int32_t kWeaponBandFirst      = 0x230;
    constexpr std::int32_t kWeaponBandLastUsable = 0x288;
    static_assert(kWeaponBandLastUsable == kCompressedSlotBound - 1,
                  "weapon band must end at the native table bound");
    static_assert(kItemBandLast + 1 == kWeaponBandFirst,
                  "band split must be contiguous");

    void  MarkCompressedSlotUsed(std::int32_t compressed);
    void  ClearCompressedSlotUsed(std::int32_t compressed);
    bool  IsCompressedSlotUsed(std::int32_t compressed);

    std::size_t SyncFromNativeTable();

    constexpr std::int32_t kChimeraEquipIdFirst = 0x367;
    constexpr std::int32_t kChimeraEquipIdLast  = 0x36C;


    constexpr std::int32_t kExtendedEquipIdFirst = kCompressedSlotBound;   // 0x289
    constexpr std::int32_t kExtendedEquipIdLast  = 0xFFFF;
    constexpr std::int32_t kExtendedAllocFirst   = 0x700;
    constexpr std::int32_t kExtendedFoldedFirst  =
        ComputeCompressed(kExtendedAllocFirst);
    constexpr std::int32_t kExtendedFoldedLast   =
        ComputeCompressed(kExtendedEquipIdLast);

    static_assert(kExtendedFoldedFirst >= kCompressedSlotBound,
                  "extended ids must fold clear of every vanilla row: vanilla folds "
                  "all land below the native table bound, so anything at or above it "
                  "can never alias a vanilla equipId");
    static_assert(kExtendedFoldedFirst > kChimeraEquipIdLast &&
                  kExtendedAllocFirst > kChimeraEquipIdLast,
                  "extended ids must clear the chimera band raw AND folded - an id "
                  "inside it is rewritten to a gunsmith work-slot id at loadout spawn");
    static_assert(kExtendedFoldedLast > kExtendedFoldedFirst,
                  "the fold must stay monotonic across the extended band");

    inline bool IsExtendedEquipId(std::int32_t equipId)
    {
        return equipId >= kExtendedEquipIdFirst && equipId <= kExtendedEquipIdLast;
    }

    void  MarkExtendedEquipIdUsed(std::int32_t equipId);
    bool  IsExtendedEquipIdUsed(std::int32_t equipId);
    std::int32_t FindLowestFreeExtendedEquipId();

    template <typename SessionUsedFn>
    inline std::int32_t FindLowestFreeEquipId(SessionUsedFn isSessionUsed,
                                              std::int32_t minimumEquipId = 0)
    {
        const std::int32_t start = (minimumEquipId > 0) ? minimumEquipId : 0;
        for (std::int32_t equipId = start;
             equipId < kCompressedSlotBound;
             ++equipId)
        {
            if (equipId >= kChimeraEquipIdFirst && equipId <= kChimeraEquipIdLast)
                continue;
            if (equipId == 0x400) continue;
            if (IsCompressedSlotUsed(equipId)) continue;
            if (isSessionUsed(equipId))        continue;
            return equipId;
        }
        return -1;
    }

    template <typename SessionUsedFn>
    inline std::int32_t FindLowestFreeEquipIdInRange(SessionUsedFn isSessionUsed,
                                                     std::int32_t first,
                                                     std::int32_t last)
    {
        if (first < 0) first = 0;
        if (last >= kCompressedSlotBound) last = kCompressedSlotBound - 1;
        for (std::int32_t equipId = first; equipId <= last; ++equipId)
        {
            if (equipId >= kChimeraEquipIdFirst && equipId <= kChimeraEquipIdLast)
                continue;
            if (equipId == 0x400) continue;
            if (IsCompressedSlotUsed(equipId)) continue;
            if (isSessionUsed(equipId))        continue;
            return equipId;
        }
        return -1;
    }

}
