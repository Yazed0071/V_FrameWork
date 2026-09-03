#pragma once

#include <cstdint>

bool Install_TppCollectionHooks();
void Uninstall_TppCollectionHooks();

bool TppCollection_TypesAvailable();

bool TppCollection_AddCustom(const char* name, std::uint32_t typeId,
                             float x, float y, float z, float rotYDeg);
bool TppCollection_RemoveCustom(const char* name);

struct TppCollectionTypeDesc
{
    const char* modelPath        = nullptr;
    float       r                = 0.9f;
    float       g                = 0.9f;
    float       b                = 0.5f;
    float       a                = 0.5f;
    float       fxStrengthX      = 0.5f;
    float       fxStrengthY      = 1.0f;
    float       fxStrengthZ      = 0.5f;
    float       fxStrengthW      = 1.0f;
    float       effectYOffset    = 0.25f;
    float       groundEffectSize = 0.4f;
    const char* rootModelPath    = nullptr;
    const char* fovaPath         = nullptr;
    bool        isHerb           = false;
    bool        isMaterial       = false;
    bool        isDiamond        = false;
};

std::int32_t TppCollection_RegisterType(const char* typeName, const TppCollectionTypeDesc& desc);
bool TppCollection_SetTypeIcon(const char* typeName, std::int32_t typeId, const char* iconPath);
bool TppCollection_SetTypeLangId(const char* typeName, std::int32_t typeId, std::uint64_t langId);
std::int32_t TppCollection_FindTypeByName(const char* typeName);
