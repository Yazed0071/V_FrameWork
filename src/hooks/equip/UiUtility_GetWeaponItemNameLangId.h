#pragma once

#include <cstdint>

bool Install_UiUtility_GetWeaponItemNameLangIdHook();
void Uninstall_UiUtility_GetWeaponItemNameLangIdHook();

void EquipLangInfo_Set(int equipId,
                       bool hasName,     std::uint64_t nameId,
                       bool hasInfo,     std::uint64_t infoId,
                       bool hasRealName, std::uint64_t realNameId);
void EquipLangInfo_Clear();
