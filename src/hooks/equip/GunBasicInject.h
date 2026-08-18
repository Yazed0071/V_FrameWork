#pragma once

inline constexpr int kGunBasicParameters2StockSlots = 514;

struct lua_State;

int __cdecl l_SetGunBasic(lua_State* L);

int GunBasic_AllocateWeaponIdForName(const char* name);

bool GunBasic_ReadRowBytes(int weaponId, unsigned char* out12);

int GunBasic_MaxWeaponId();
int GunBasic_PackedSubIdMax();

int GunBasic_RebindWidePartsForWeapon(int weaponId);
bool GunBasic_WeaponNeedsLaneBind(int weaponId);
int GunBasic_ClearLaneFromRows(int space, int lane);
int GunBasic_GetWideSlotLanes(int weaponId, int space, unsigned char outLanes[12]);
int GunBasic_GetLogicalPart(int weaponId, int space);
bool GunBasic_GetLogicalParts(int weaponId, int out11[11]);

bool Install_TppEquip_ReloadEquipParameterTables2_Hook();
bool Uninstall_TppEquip_ReloadEquipParameterTables2_Hook();
