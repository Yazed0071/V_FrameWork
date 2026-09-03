#pragma once

#include <cstddef>
#include <cstdint>

namespace outfit::shadow
{
    constexpr std::size_t kMaxSlots = 4;
    constexpr std::size_t kPlayerTypeMax = 7;

    struct Slot
    {
        bool          used              = false;
        std::uint8_t  realPartsType     = 0;
        std::uint8_t  realCamoType      = 0;
        std::uint8_t  realArmType       = 0;
        std::uint8_t  realPlayerType    = 0;
        std::uint16_t developId         = 0;
        std::uint8_t  variantIdx        = 0;
    };

    void Set(std::size_t slot, const Slot& s);
    void Clear(std::size_t slot);
    void ResetAll(const char* reason);

    bool Get(std::size_t slot, Slot* out);
    bool IsActive(std::size_t slot);

    void          SetArmTier(std::uint32_t playerType, std::uint8_t tier);
    std::uint8_t  GetArmTier(std::uint32_t playerType, bool* outCaptured);
    void          ResetArmTierCache();

    void         SetCurrentSlot(std::size_t slot);
    void         ClearCurrentSlot();
    std::size_t  GetCurrentSlot();
    bool         HasCurrentSlot();
}
