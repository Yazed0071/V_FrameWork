#pragma once

#include <cstdint>

namespace uniqueStaff
{
    bool         IsValidKey(const char* key);
    std::int32_t Register(const char* key);
    std::int32_t Find(const char* key);
    const char*  KeyHoldingId(std::int32_t typeId);
    std::int32_t FreeSlotCount();
    std::int32_t PoolSize();
}
