#include "pch.h"

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
}

#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "V_FoxLib.h"
#include "FoxFunctions.h"
#include "LuaApi.h"
#include "FoxPath.h"

namespace
{
    static std::atomic<bool> g_GameConsoleEnabled{ false };

    static int __cdecl hk_FoxLog(lua_State* L)
    {
        if (g_GameConsoleEnabled.load(std::memory_order_relaxed))
        {
            size_t len = 0;
            const char* msg = g_lua_tolstring ? g_lua_tolstring(L, 1, &len) : nullptr;
            LogDebug("[Fox.Log] %s\n", msg ? msg : "(non-string arg)");
        }
        return 0;
    }

    static bool RebindFoxLog(lua_State* L)
    {
        if (!L || !ResolveLuaApi())
            return false;
        const int top = g_lua_gettop(L);
        g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("Fox"));
        const int foxType = g_lua_type(L, -1);
        bool ok = false;
        if (foxType == LUA_TTABLE)
        {
            g_lua_pushstring(L, const_cast<char*>("Log"));
            g_lua_pushcclosure(L, &hk_FoxLog, 0);
            g_lua_rawset(L, -3);
            ok = true;
        }
        g_lua_settop(L, top);
        if (!ok)
            Log("[GameLogConsole] WARN: 'Fox' global is type %d, not a table -- Fox.Log capture unavailable\n", foxType);
        return ok;
    }

    static int __cdecl l_ShowConsole(lua_State* L)
    {
        const bool enable = GetLuaBool(L, 1) != 0;
        g_GameConsoleEnabled.store(enable, std::memory_order_relaxed);
        if (enable)
        {
            EnsureConsole();
            if (HWND hwnd = GetConsoleWindow())
                ShowWindow(hwnd, SW_SHOW);
            const bool ok = RebindFoxLog(L);
            Log("[GameLogConsole] enabled -- %s; V_FrameWork.Log mirrored too\n",
                ok ? "Fox.Log re-bound" : "Fox.Log NOT re-bound (see WARN above)");
        }
        else if (HWND hwnd = GetConsoleWindow())
        {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }

    static luaL_Reg g_VFoxLib[] =
    {
        { "FNVHash32",                  l_FNVHash32 },
        { "ShowConsole",                l_ShowConsole },

        { nullptr,          nullptr }
    };
}

bool Register_V_FoxLibrary(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_Fox", g_VFoxLib);
}
