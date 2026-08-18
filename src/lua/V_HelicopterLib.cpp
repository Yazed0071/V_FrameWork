#include "pch.h"

#include <cstdint>

#include "LuaApi.h"
#include "log.h"
#include "V_HelicopterLib.h"

#include "../hooks/sound/HeliVoice.h"
#include "../hooks/heli/HeliSoundController.h"
#include "../hooks/heli/FieldTaxiMenu.h"

namespace
{
    static int __cdecl l_SetEnableHeliVoice(lua_State* L)
    {
        const bool isEnable = GetLuaBool(L, 1);
        const char* voiceEvt = GetLuaString(L, 2);
        const char* radioEvt = GetLuaString(L, 3);

        if (isEnable && (!voiceEvt || !*voiceEvt || !radioEvt || !*radioEvt))
        {
            LogDebug("[HeliVoice] SetEnableHeliVoice: enable=true requires non-empty voice/radio event strings\n");
            PushLuaBool(L, false);
            return 1;
        }

        const bool ok = SetEnableHeliVoice(isEnable,
                                           voiceEvt ? voiceEvt : "",
                                           radioEvt ? radioEvt : "");
        PushLuaBool(L, ok);
        return 1;
    }

    static int __cdecl l_PilotCallVoice(lua_State* L)
    {
        const std::uint32_t voiceId = GetLuaFnvHash32Arg(L, 1);
        PushLuaBool(L, Play_PilotCallVoice(voiceId, 0u, 0u, 0u));
        return 1;
    }

    static int __cdecl l_PilotCallRadio(lua_State* L)
    {
        const std::uint32_t line1 = GetLuaFnvHash32Arg(L, 1);
        if (GetLuaTop(L) >= 2 && (LuaIsString(L, 2) || LuaIsNumber(L, 2)))
        {
            const std::uint32_t line2 = GetLuaFnvHash32Arg(L, 2);
            PushLuaBool(L, Play_PilotCallVoice(line2, 0, 2, line1));
        }
        else
        {
            PushLuaBool(L, Play_PilotCallVoice(line1, 0, 1, 0));
        }
        return 1;
    }

    static int __cdecl l_SetFieldTaxiMissionEnabled(lua_State* L)
    {
        const unsigned int code    = static_cast<unsigned int>(GetLuaNumber(L, 1));
        const bool         enabled = (GetLuaTop(L) >= 2) ? GetLuaBool(L, 2) : true;
        FieldTaxi_SetMissionEnabled(code, enabled);
        return 0;
    }

    static int __cdecl l_SetTaxiLandingZoneHidden(lua_State* L)
    {
        const unsigned int lzNameHash = GetLuaStrCode32Arg(L, 1);
        const bool         hidden     = (GetLuaTop(L) >= 2) ? GetLuaBool(L, 2) : true;
        FieldTaxi_SetTaxiLandingZoneHidden(lzNameHash, hidden);
        return 0;
    }

    static int __cdecl l_SetTaxiRideState(lua_State* L)
    {
        FieldTaxi_SetTaxiRideState(static_cast<unsigned int>(GetLuaNumber(L, 1)));
        return 0;
    }

    static int __cdecl l_SetTaxiRideLog(lua_State* L)
    {
        FieldTaxi_SetTaxiRideLog((GetLuaTop(L) >= 1) ? GetLuaBool(L, 1) : true);
        return 0;
    }

    static int __cdecl l_ResetTaxiState(lua_State* /*L*/)
    {
        FieldTaxi_ResetTaxiState();
        return 0;
    }

    static luaL_Reg g_VHelicopterLib[] =
    {
        { "SetEnableHeliVoice",                    l_SetEnableHeliVoice },
        { "PilotCallVoice",                        l_PilotCallVoice },
        { "PilotCallRadio",                        l_PilotCallRadio },

        { "SetFieldTaxiMissionEnabled",            l_SetFieldTaxiMissionEnabled },
        { "SetTaxiLandingZoneHidden",              l_SetTaxiLandingZoneHidden },
        { "SetTaxiRideState",                      l_SetTaxiRideState },
        { "SetTaxiRideLog",                        l_SetTaxiRideLog },
        { "ResetTaxiState",                        l_ResetTaxiState },

        { nullptr, nullptr }
    };
}

bool Register_V_HelicopterLibrary(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_Helicopter", g_VHelicopterLib);
}
