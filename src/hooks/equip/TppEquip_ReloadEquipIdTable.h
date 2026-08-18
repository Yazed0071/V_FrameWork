#pragma once

#include <cstdint>

struct lua_State;

int __cdecl l_AddToEquipIdTable(lua_State* L);

int TppEquip_GetSubIdForEquipId(int equipId);
int TppEquip_ReleaseEquipRow(int equipId);

struct V_ExtendedEquipRow
{
    int equipType;
    int subId;
    int block;
    std::uint64_t partsHash;
    std::uint64_t packHash;
};

bool TppEquip_GetExtendedEquipRow(int equipId, V_ExtendedEquipRow* out);

bool TppEquip_EnsureInfoListMirror();

bool Install_TppEquip_ReloadEquipIdTable_Hook();
bool Uninstall_TppEquip_ReloadEquipIdTable_Hook();
