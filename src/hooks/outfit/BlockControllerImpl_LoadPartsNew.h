#pragma once

#include <cstdint>

namespace outfit
{


    std::uint32_t GetLocalPartsSlot();

    bool Install_OutfitRuntimeParts_Hooks();
    void Uninstall_OutfitRuntimeParts_Hooks();
}
