#pragma once

#include <cstdint>
#include <string>

struct lua_State;

namespace bluePrint
{
    std::int32_t Capacity();

    std::int32_t Register(const char* key);
    std::int32_t Find(const char* key);

    std::int32_t PublicId(std::int32_t slot);
    std::int32_t SlotFromPublicId(std::int32_t publicId);
    std::string  KeyFromSlot(std::int32_t slot);

    bool Has(std::int32_t slot);
    bool Set(std::int32_t slot, bool owned);

    bool GetNew(std::int32_t slot);
    bool SetNew(std::int32_t slot, bool isNew);

    bool HasKey(const char* key);
    bool SetKey(const char* key, bool owned);

    void InjectConstants(lua_State* L);
    void InjectConstantFor(lua_State* L, const char* key);
}
