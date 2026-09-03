#pragma once

struct lua_State;

int __cdecl l_SetMagazine(lua_State* L);
int __cdecl l_SetStock(lua_State* L);
int __cdecl l_SetMuzzle(lua_State* L);
int __cdecl l_SetReceiver(lua_State* L);
int __cdecl l_SetSight(lua_State* L);
int __cdecl l_SetBarrel(lua_State* L);
int __cdecl l_SetUnderBarrel(lua_State* L);
int __cdecl l_SetOption(lua_State* L);
int __cdecl l_SetBullet(lua_State* L);
int __cdecl l_SetDamage(lua_State* L);
int __cdecl l_DeclareDamages(lua_State* L);

int EquipParam_AllocateMagazineSlotForName(const char* name);
int EquipParam_AllocateStockSlotForName(const char* name);
int EquipParam_AllocateMuzzleSlotForName(const char* name);
int EquipParam_AllocateReceiverSlotForName(const char* name);
int EquipParam_AllocateSightSlotForName(const char* name);
int EquipParam_AllocateBarrelSlotForName(const char* name);
int EquipParam_AllocateUnderBarrelSlotForName(const char* name);
int EquipParam_AllocateOptionSlotForName(const char* name);
int EquipParam_AllocateBulletSlotForName(const char* name);

bool Install_MotionLoader_ReceiverTypeHook();
void Uninstall_MotionLoader_ReceiverTypeHook();
bool Install_MotionLoader_UnderBarrelTypeHook();
void Uninstall_MotionLoader_UnderBarrelTypeHook();
bool Install_MotionLoader_BarrelTypeHook();
void Uninstall_MotionLoader_BarrelTypeHook();
bool Install_MotionLoader_MagazineTypeHook();
void Uninstall_MotionLoader_MagazineTypeHook();
bool Install_MotionLoader_SightTypeHook();
void Uninstall_MotionLoader_SightTypeHook();
bool Install_UiController_UiSightTypeHook();
void Uninstall_UiController_UiSightTypeHook();
int  EquipParam_InheritPartMotionTypes(int barrelDst, int barrelSrc,
                                       int magazineDst, int magazineSrc,
                                       int sightDst, int sightSrc,
                                       int* outEligible = nullptr);

bool Install_GetAttackIdGuard();
void Uninstall_GetAttackIdGuard();

bool Install_GunInfoGuard();
void Uninstall_GunInfoGuard();

bool Install_BulletEffectGuard();
void Uninstall_BulletEffectGuard();

bool Install_WeaponKeyLog();
void Uninstall_WeaponKeyLog();
void EquipParam_SetWeaponHandling(unsigned int fromKey, unsigned int toKey);

bool Install_FireSoundOverride_Hook();
void Uninstall_FireSoundOverride_Hook();

bool Install_LoadoutRequestGuard();
void Uninstall_LoadoutRequestGuard();

bool Install_LoadoutGunInfoGetOrBuild();
void Uninstall_LoadoutGunInfoGetOrBuild();

bool Install_SuppressorGauge_Hook();
void Uninstall_SuppressorGauge_Hook();

int EquipParam_AllocateDamageSlotForName(const char* name);
bool Install_DamageParameter_Hook();
void Uninstall_DamageParameter_Hook();

enum EquipVanillaSpace
{
    kVanillaSpace_Receiver = 0,
    kVanillaSpace_Barrel,
    kVanillaSpace_Magazine,
    kVanillaSpace_Bullet,
    kVanillaSpace_Stock,
    kVanillaSpace_MuzzleOption,
    kVanillaSpace_Sight,
    kVanillaSpace_UnderBarrel,
    kVanillaSpace_Option,
    kVanillaSpace_Weapon,
    kVanillaSpace_Damage,
    kVanillaSpace_Count
};

int  EquipParam_GetWideAlias(int space, int wideId);
int  EquipParam_GetWideReceiverDonor(int wideId);
int  EquipParam_GetDeclaredWeaponAttackId(int equipId);
void EquipParam_EnableWidePartIds(int newMaxId);
int  EquipParam_ResolvePartByte(int space, int id);
void EquipParam_VanillaPreWrite(int space, int id, const unsigned char* row, int stride);
void EquipParam_VanillaPostWrite(int space, int id, const unsigned char* row, int stride);
void EquipParam_VanillaForceTaint(int space, int id, const char* why);
bool EquipParam_IsEquipIdFobTainted(unsigned int equipId, int isWeaponSlot);
int  EquipParam_BulletFalloffSwapBegin(int bulletId);
void EquipParam_BulletFalloffSwapEnd();
