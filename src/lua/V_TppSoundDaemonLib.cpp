#include "pch.h"

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
}

#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "V_TppSoundDaemonLib.h"
#include "SoundDaemonFunctions.h"
#include "LuaApi.h"
#include "../hooks/sound/GameOverMusic.h"

namespace
{
    static int __cdecl l_SetGameOverMusic(lua_State* L)
    {
        const bool isEnable = GetLuaBool(L, 1);
        const int  typeRaw  = GetLuaInt(L, 2);
        const unsigned int playHash = GetLuaFnvHash32Arg(L, 3);
        const unsigned int stopHash = GetLuaFnvHash32Arg(L, 4);

        if (typeRaw < GAME_OVER_GENERAL || typeRaw > GAME_OVER_CYPRUS)
        {
            LogDebug("[GameOverMusic] SetGameOverMusic: invalid type=%d (expected 0..3)\n", typeRaw);
            PushLuaBool(L, false);
            return 1;
        }

        if (isEnable && (playHash == 0 || stopHash == 0))
        {
            LogDebug("[GameOverMusic] SetGameOverMusic: enable=true requires a valid play/stop event (name string or hash number)\n");
            PushLuaBool(L, false);
            return 1;
        }

        const bool ok = SetGameOverMusic(isEnable,
                                         static_cast<GAME_OVER_TYPE>(typeRaw),
                                         playHash,
                                         stopHash);
        PushLuaBool(L, ok);
        return 1;
    }

    static luaL_Reg g_VTppSoundDaemonLib[] =
    {
        { "SetGameOverMusic",               l_SetGameOverMusic },

        { nullptr, nullptr }
    };
}

bool Register_V_TppSoundDaemonLibrary(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_TppSoundDaemon", g_VTppSoundDaemonLib);
}
