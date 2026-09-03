#pragma once


struct lua_State;

int __cdecl l_SetDefaultEquipBgTexturePath(lua_State* L);
int __cdecl l_ClearDefaultEquipBgTexture(lua_State* L);
int __cdecl l_SetEquipBgTexturePath(lua_State* L);
int __cdecl l_ClearEquipBgTexture(lua_State* L);
int __cdecl l_SetEnemyWeaponBgTexturePath(lua_State* L);
int __cdecl l_ClearEnemyWeaponBgTexture(lua_State* L);
int __cdecl l_SetEnemyEquipBgTexturePath(lua_State* L);
int __cdecl l_ClearEnemyEquipBgTexture(lua_State* L);
int __cdecl l_ClearAllEquipBgTextures(lua_State* L);
int __cdecl l_SetLoadingSplashMainTexturePath(lua_State* L);
int __cdecl l_SetLoadingSplashBlurTexturePath(lua_State* L);
int __cdecl l_ClearLoadingSplashTextures(lua_State* L);
int __cdecl l_SetGameOverSplashMainTexturePath(lua_State* L);
int __cdecl l_SetGameOverSplashBlurTexturePath(lua_State* L);
int __cdecl l_ClearGameOverSplashTextures(lua_State* L);

int __cdecl l_SetEquipIconFtexPath(lua_State* L);
int __cdecl l_ClearIconFtexPath(lua_State* L);
int __cdecl l_ClearAllIconFtexPaths(lua_State* L);

int __cdecl l_SetMissionEmergency(lua_State* L);
int __cdecl l_IsMissionEmergency(lua_State* L);
int __cdecl l_SetEmergencyMissionPopup(lua_State* L);
int __cdecl l_SetEmergencyMissionPopupLangId(lua_State* L);
int __cdecl l_ClearEmergencyMissionPopupOverride(lua_State* L);

int __cdecl l_ShowMissionIcon(lua_State* L);

int __cdecl l_ShowTimeCigaretteUi(lua_State* L);
int __cdecl l_HideTimeCigaretteUi(lua_State* L);


int __cdecl l_ShowMbDvcAnnouncePopupReport(lua_State* L);
int __cdecl l_ShowMbDvcAnnouncePopupReportLangId(lua_State* L);
int __cdecl l_ShowMbDvcAnnouncePopupReward(lua_State* L);
int __cdecl l_ShowMbDvcAnnouncePopupRewardLangId(lua_State* L);

int __cdecl l_SetEnemyInformationLangId(lua_State* L);
int __cdecl l_SetEnemyUnitName(lua_State* L);

int __cdecl l_SetEnemyInformationLangIdForSoldier(lua_State* L);
int __cdecl l_SetEnemyUnitNameForSoldier(lua_State* L);

int __cdecl l_SetAnnounceLogSE(lua_State* L);
int __cdecl l_RegisterAnnounceLogSfx(lua_State* L);
int __cdecl l_UnsetAnnounceLogSE(lua_State* L);
int __cdecl l_UnregisterAnnounceLogSfx(lua_State* L);

int __cdecl l_SetMissionTelopSplashTexturePath(lua_State* L);
int __cdecl l_UnsetMissionTelopSplashTexturePath(lua_State* L);

int __cdecl l_SetMissionAcceptWarning(lua_State* L);

int __cdecl l_SetMissionMenuHelp(lua_State* L);
int __cdecl l_ClearMissionMenuHelp(lua_State* L);
int __cdecl l_SetEquipLangInfo(lua_State* L);
