#pragma once

#include <cstdint>

namespace uniqueownsuit
{
    bool AdoptOrMatchRow(std::uint8_t playerType, std::uint16_t flowIndex,
                         bool acceptedForSnake);

    bool IsOwnSuitRow(std::uint8_t playerType, std::uint16_t flowIndex);

    std::uint16_t GetOwnSuitRow(std::uint8_t playerType);

    bool TryGetLabel(std::uint8_t playerType, std::uint16_t flowIndex,
                     std::uint64_t* outNameHash, std::uint64_t* outIconHash);


    bool TryGetRowLabel(std::uint8_t playerType, std::uint64_t* outNameHash,
                        std::uint64_t* outInfoHash);

    void ResetAdoption();
}
