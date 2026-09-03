#pragma once

#include <cstdint>

namespace TppGameObjectType
{
    static constexpr std::uint16_t kUnknown  = 0xFFFF;
    static constexpr std::uint16_t kCommandPost = 1;
    static constexpr std::uint16_t kSoldier2 = 2;
}

bool Install_GetGameObjectIdWithIndex();
bool Uninstall_GetGameObjectIdWithIndex();
bool Is_GetGameObjectIdWithIndex_Installed();

bool GetGameObjectIdWithIndex(const char* typeName,
    std::uint32_t index,
    std::uint32_t& gameObjectIdOut);

bool GetSoldierGameObjectIdWithIndex(std::uint32_t soldierIndex,
    std::uint32_t& gameObjectIdOut);

std::uint32_t GetGameObjectIdByIndex(const char* typeName,
    std::uint32_t index);

bool GetGameObjectIdByName(const char* typeName,
    const char* instanceName,
    std::uint32_t& gameObjectIdOut);
