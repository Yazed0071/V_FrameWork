#pragma once

struct lua_State;

int __cdecl l_SetGunBasic(lua_State* L);

int GunBasic_AllocateWeaponIdForName(const char* name);

bool GunBasic_ReadRowBytes(int weaponId, unsigned char* out12);

int GunBasic_RebindWidePartsForWeapon(int weaponId);

bool Install_TppEquip_ReloadEquipParameterTables2_Hook();
bool Uninstall_TppEquip_ReloadEquipParameterTables2_Hook();
