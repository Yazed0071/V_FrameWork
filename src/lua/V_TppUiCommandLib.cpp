#include "pch.h"

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
}

#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "V_TppUiCommandLib.h"
#include "UiCommandFunctions.h"
#include "LuaApi.h"

namespace
{
    static luaL_Reg g_VTppUiCommandLib[] =
    {
        { "SetDefaultEquipBgTexturePath",             l_SetDefaultEquipBgTexturePath },
        { "ClearDefaultEquipBgTexture",               l_ClearDefaultEquipBgTexture },
        { "SetEquipBgTexturePath",                    l_SetEquipBgTexturePath },
        { "ClearEquipBgTexture",                      l_ClearEquipBgTexture },
        { "SetEnemyWeaponBgTexturePath",              l_SetEnemyWeaponBgTexturePath },
        { "ClearEnemyWeaponBgTexture",                l_ClearEnemyWeaponBgTexture },
        { "SetEnemyEquipBgTexturePath",               l_SetEnemyEquipBgTexturePath },
        { "ClearEnemyEquipBgTexture",                 l_ClearEnemyEquipBgTexture },
        { "ClearAllEquipBgTextures",                  l_ClearAllEquipBgTextures },
        { "SetLoadingSplashMainTexturePath",          l_SetLoadingSplashMainTexturePath },
        { "SetLoadingSplashBlurTexturePath",          l_SetLoadingSplashBlurTexturePath },
        { "ClearLoadingSplashTextures",               l_ClearLoadingSplashTextures },
        { "SetGameOverSplashMainTexturePath",         l_SetGameOverSplashMainTexturePath },
        { "SetGameOverSplashBlurTexturePath",         l_SetGameOverSplashBlurTexturePath },
        { "ClearGameOverSplashTextures",              l_ClearGameOverSplashTextures },
        { "SetMissionTelopSplashTexturePath",         l_SetMissionTelopSplashTexturePath },
        { "UnsetMissionTelopSplashTexturePath",       l_UnsetMissionTelopSplashTexturePath },

        { "SetEquipIconFtexPath",                     l_SetEquipIconFtexPath },
        { "ClearIconFtexPath",                        l_ClearIconFtexPath },
        { "ClearAllIconFtexPaths",                    l_ClearAllIconFtexPaths },
        { "SetMissionEmergency",                      l_SetMissionEmergency },
        { "IsMissionEmergency",                       l_IsMissionEmergency },
        { "SetEmergencyMissionPopup",                 l_SetEmergencyMissionPopup },
        { "SetEmergencyMissionPopupLangId",           l_SetEmergencyMissionPopupLangId },
        { "ClearEmergencyMissionPopupOverride",       l_ClearEmergencyMissionPopupOverride },
        { "ShowMissionIcon",                          l_ShowMissionIcon },
        { "ShowTimeCigaretteUi",                      l_ShowTimeCigaretteUi },
        { "HideTimeCigaretteUi",                      l_HideTimeCigaretteUi },
        { "ShowMbDvcAnnouncePopupReport",             l_ShowMbDvcAnnouncePopupReport },
        { "ShowMbDvcAnnouncePopupReportLangId",       l_ShowMbDvcAnnouncePopupReportLangId },
        { "ShowMbDvcAnnouncePopupReward",             l_ShowMbDvcAnnouncePopupReward },
        { "ShowMbDvcAnnouncePopupRewardLangId",       l_ShowMbDvcAnnouncePopupRewardLangId },

        { "SetEnemyInformationLangId",                l_SetEnemyInformationLangId },
        { "SetEnemyUnitName",                         l_SetEnemyUnitName },

        { "SetEnemyInformationLangIdForSoldier",      l_SetEnemyInformationLangIdForSoldier },
        { "SetEnemyUnitNameForSoldier",               l_SetEnemyUnitNameForSoldier },

        { "SetAnnounceLogSE",                         l_SetAnnounceLogSE },
        { "RegisterAnnounceLogSfx",                   l_RegisterAnnounceLogSfx },
        { "UnsetAnnounceLogSE",                       l_UnsetAnnounceLogSE },
        { "UnregisterAnnounceLogSfx",                 l_UnregisterAnnounceLogSfx },

        { "SetMissionAcceptWarning",                  l_SetMissionAcceptWarning },

        { "SetMissionMenuHelp",                       l_SetMissionMenuHelp },
        { "ClearMissionMenuHelp",                     l_ClearMissionMenuHelp },
        { "SetEquipLangInfo",                         l_SetEquipLangInfo },


        { nullptr,          nullptr }
    };
}

bool Register_V_TppUiCommandLibrary(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_TppUiCommand", g_VTppUiCommandLib);
}
