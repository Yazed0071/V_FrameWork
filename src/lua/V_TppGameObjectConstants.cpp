#include "pch.h"

extern "C" {
    #include "lua.h"
}

#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "V_TppGameObjectConstants.h"
#include "LuaApi.h"


namespace
{

    struct ConstEntry { const char* name; lua_Number value; };

    static const ConstEntry V_TppGameObject[] = {
        {"GAME_OVER_GENERAL", 0},
        {"GAME_OVER_PARADOX", 1},
        {"GAME_OVER_STEALTH", 2},
        {"GAME_OVER_CYPRUS",  3},
        { nullptr, 0.0 }
    };
    static const ConstEntry V_TppMbDev[] = {
        {"EQP_OUTFIT_VARIANT_GRADE", 0},
        {"EQP_OUTFIT_VARIANT_NAME", 79},
        { nullptr, 0.0 }
    };
    static const ConstEntry V_PlayerCqcStance[] = {
        {"STAND", 0},
        {"SQUAT", 1},
        { nullptr, 0.0 }
    };
    static const ConstEntry V_TppCallSign[] = {
        {"NONE",     0},
        {"ZULU_1",   1},  {"ZOYA_1",     1},
        {"ZULU_4",   2},  {"ZOYA_4",     2},
        {"ZULU_7",   3},  {"ZOYA_7",     3},
        {"ZULU_2",   4},  {"ZOYA_2",     4},
        {"ZULU_6",   5},  {"ZOYA_6",     5},
        {"ZULU_10",  6},  {"ZOYA_10",    6},
        {"DELTA_2",  7},  {"DMITRY_2",   7},
        {"DELTA_6",  8},  {"DMITRY_6",   8},
        {"DELTA_9",  9},  {"DMITRY_9",   9},
        {"PATROL",  10},
        {"DELTA_1", 11},  {"DMITRY_1",  11},
        {"DELTA_3", 12},  {"DMITRY_3",  12},
        {"DELTA_4", 13},  {"DMITRY_4",  13},
        { nullptr, 0.0 }
    };
}

void Register_V_TppGameObjectConstants(lua_State* L)
{
    if (!L || !ResolveLuaApi())
        return;

    g_lua_pushstring(L, const_cast<char*>("V_TppGameObject"));
    g_lua_createtable(L, 0, 0);

    int registered = 0;
    for (const auto& e : V_TppGameObject)
    {
        if (!e.name) break;
        g_lua_pushstring(L, const_cast<char*>(e.name));
        g_lua_pushnumber(L, e.value);
        g_lua_settable(L, -3);
        ++registered;
    }

    g_lua_settable(L, LUA_GLOBALSINDEX_51);
#ifdef _DEBUG
    LogDebug("[V_FrameWork] Registered global V_TppGameObject (%d constants)\n", registered);
#endif
}

void Register_V_TppMbDevConstants(lua_State* L)
{
    if (!L || !ResolveLuaApi())
        return;

    g_lua_pushstring(L, const_cast<char*>("V_TppMbDev"));
    g_lua_createtable(L, 0, 0);

    int registered = 0;
    for (const auto& e : V_TppMbDev)
    {
        if (!e.name) break;
        g_lua_pushstring(L, const_cast<char*>(e.name));
        g_lua_pushnumber(L, e.value);
        g_lua_settable(L, -3);
        ++registered;
    }

    g_lua_settable(L, LUA_GLOBALSINDEX_51);
#ifdef _DEBUG
    LogDebug("[V_FrameWork] Registered global V_TppMbDev (%d constants)\n", registered);
#endif
}

void Register_V_PlayerCqcStanceConstants(lua_State* L)
{
    if (!L || !ResolveLuaApi())
        return;

    g_lua_pushstring(L, const_cast<char*>("V_PlayerCqcStance"));
    g_lua_createtable(L, 0, 0);

    int registered = 0;
    for (const auto& e : V_PlayerCqcStance)
    {
        if (!e.name) break;
        g_lua_pushstring(L, const_cast<char*>(e.name));
        g_lua_pushnumber(L, e.value);
        g_lua_settable(L, -3);
        ++registered;
    }

    g_lua_settable(L, LUA_GLOBALSINDEX_51);
    #ifdef _DEBUG
    LogDebug("[V_FrameWork] Registered global V_PlayerCqcStance (%d constants)\n", registered);
    #endif
}

void Register_V_TppCallSignConstants(lua_State* L)
{
    if (!L || !ResolveLuaApi())
        return;

    g_lua_pushstring(L, const_cast<char*>("V_TppCallSign"));
    g_lua_createtable(L, 0, 0);

    int registered = 0;
    for (const auto& e : V_TppCallSign)
    {
        if (!e.name) break;
        g_lua_pushstring(L, const_cast<char*>(e.name));
        g_lua_pushnumber(L, e.value);
        g_lua_settable(L, -3);
        ++registered;
    }

    g_lua_settable(L, LUA_GLOBALSINDEX_51);
#ifdef _DEBUG
    LogDebug("[V_FrameWork] Registered global V_TppCallSign (%d constants)\n", registered);
#endif
}
