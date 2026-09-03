#pragma once

#include <cstdint>

struct lua_State;

namespace uniquedefaultoutfit
{
    void QueueDevelopRowsEarly();

    bool EnsureRegistered(lua_State* L);

    bool IsRegisteredFor(std::uint8_t playerType);

    bool IsDefaultOutfitRow(std::uint8_t playerType,
                            std::uint16_t flowIndex);

    std::uint16_t GetDefaultOutfitRow(std::uint8_t playerType);

    bool IsDefaultOutfitPartsType(std::uint8_t partsType,
                                  std::uint8_t* outPlayerType);

    bool IsRegisteringFobAllowedRow();

    const char* GetDefaultOutfitKey(std::uint8_t playerType);

    bool IsDefaultOutfitKey(const char* key);

    bool TryGetPlayerTypeForBinding(std::uint8_t partsType,
                                    std::uint8_t selector,
                                    std::uint8_t* outPlayerType);

    bool TryGetBindingFor(std::uint8_t playerType,
                          std::uint8_t* outPartsType,
                          std::uint8_t* outSelector);
}
