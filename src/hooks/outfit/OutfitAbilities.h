#pragma once

#include <cstdint>

namespace outfit
{
    bool Install_OutfitAbilities_Hooks();
    void Uninstall_OutfitAbilities_Hooks();
    void Reset_PartsTypeChangeTracking();
    void NoteOwnSuitOverwrittenByPin(std::uint8_t ownParts, std::uint8_t sel,
                                     std::uint8_t pt);
}
