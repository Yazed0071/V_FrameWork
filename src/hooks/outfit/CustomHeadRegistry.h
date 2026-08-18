#pragma once

#include <cstddef>
#include <cstdint>

#include "OutfitRegistry.h"

namespace outfit
{


    constexpr std::uint8_t  kCustomHeadSlotBase = 0x06;
    constexpr std::size_t   kMaxCustomHeads     = 250;
    static_assert(kCustomHeadSlotBase + kMaxCustomHeads <= 0x100,
                  "slot byte allocations must fit in a uint8");


    constexpr std::uint16_t kDefaultTppEnemyFaceId = 0x22D;

    struct CustomHeadEntry
    {
        bool           used            = false;
        bool           provisional     = false;
        bool           hiddenInDevelop = false;
        std::uint16_t  equipId      = 0;
        std::uint16_t  developId    = 0;
        std::uint16_t  flowIndex    = 0;
        std::uint8_t   slotByte     = 0;

        std::uint16_t  TppEnemyFaceId[kPlayerTypeMax] = {};

        std::uint64_t  faceFv2Code[kPlayerTypeMax] = {};
        std::uint64_t  faceFpkCode[kPlayerTypeMax] = {};

        char           name[64]     = { 0 };
    };


    std::uint16_t RegisterHeadOption(
        const char* name,
        const std::uint16_t* TppEnemyFaceIdsPerPt,
        const std::uint64_t* faceFv2CodesPerPt = nullptr,
        const std::uint64_t* faceFpkCodesPerPt = nullptr,
        bool showInDevelopMenu = false);

    constexpr std::uint8_t kSnakeFaceStageCount = 3;

    void SetCustomHeadSnakeFaceStages(const char* name,
                                      const std::uint64_t* fv2ByStage,
                                      const std::uint64_t* fpkByStage);
    std::uint64_t GetCustomHeadSnakeStageFv2(const char* name,
                                             std::uint32_t stage);
    std::uint64_t GetCustomHeadSnakeStageFpk(const char* name,
                                             std::uint32_t stage);


    int DrainPendingHeads();

    bool HasPendingCustomHeads();


    const CustomHeadEntry* TryGetCustomHeadByName(const char* name);


    const CustomHeadEntry* TryGetCustomHeadByEquipId(std::uint16_t equipId);


    const CustomHeadEntry* TryGetCustomHeadBySlot(std::uint8_t slotByte);


    std::uint16_t GetCurrentWornHeadEquipId();

    bool GetCurrentEquippedSuitBytes(std::uint8_t* outPartsType,
                                     std::uint8_t* outSelector);


    inline bool IsCustomHeadEquipId(std::uint16_t equipId)
    {
        return TryGetCustomHeadByEquipId(equipId) != nullptr;
    }
    inline bool IsCustomHeadSlot(std::uint8_t slotByte)
    {
        return TryGetCustomHeadBySlot(slotByte) != nullptr;
    }
    inline bool IsCustomHeadFaceFv2(std::uint16_t faceFv2)
    {
        return IsCustomHeadEquipId(faceFv2);
    }
}
