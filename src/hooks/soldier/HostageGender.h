#pragma once

#include <cstdint>

namespace HostageGender
{
    constexpr int kMale    = 0;
    constexpr int kFemale  = 1;
    constexpr int kChild   = 2;
    constexpr int kUnknown = -1;

    int Read(std::uint32_t gameObjectId);
}
