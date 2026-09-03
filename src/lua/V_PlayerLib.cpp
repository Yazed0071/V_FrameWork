#include "pch.h"

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
}

#include <cstdint>

#include "LuaApi.h"
#include "V_PlayerLib.h"
#include "PlayerFunctions.h"
#include "OutfitLuaBindings.h"

namespace
{
    static luaL_Reg g_VTppPlayerLib[] =
    {
        { "SetPlayerVoiceFpkPathForType",             l_SetPlayerVoiceFpkPathForType },
        { "ClearPlayerVoiceFpkPathForType",           l_ClearPlayerVoiceFpkPathForType },
        { "ClearAllPlayerVoiceFpkOverrides",          l_ClearAllPlayerVoiceFpkOverrides },

        { "IsBarrierActive",                          l_IsBarrierActive },

        { "RequestToSetTargetCqcStance",              l_RequestToSetTargetCqcStance },

        { "IsThereEnoughSpaceAroundPlayer",           l_IsThereEnoughSpaceAroundPlayer },
        { "RequestToAttachInDemo",                    l_RequestToAttachInDemo },
        { "ClearAttachInDemo",                        l_ClearAttachInDemo },

        { "SetQuietHoldCqc",                          l_SetQuietHoldCqc },
        { "SetQuietInterrogate",                      l_SetQuietInterrogate },

        { "RegisterOutfit",                           l_RegisterOutfit },
        { "RegisterHeadOption",                       l_RegisterHeadOption },
        { "ExtendVanillaOutfit",                      l_ExtendVanillaOutfit },
        { "GetOutfitInfo",                            l_GetOutfitInfo },

        { nullptr,          nullptr }
    };
}

bool Register_V_TppPlayerLibrary(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_Player", g_VTppPlayerLib);
}
