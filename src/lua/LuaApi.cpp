#include "pch.h"
#include "LuaApi.h"

#include "AddressSet.h"
#include "FoxHashes.h"
#include "HookUtils.h"
#include "log.h"

FoxLuaRegisterLibrary_t g_FoxLuaRegisterLibrary = nullptr;
lua_tolstring_t    g_lua_tolstring    = nullptr;
lua_tointeger_t    g_lua_tointeger    = nullptr;
lua_tonumber_t     g_lua_tonumber     = nullptr;
lua_pushnumber_t   g_lua_pushnumber   = nullptr;
lua_toboolean_t    g_lua_toboolean    = nullptr;
lua_gettop_t       g_lua_gettop       = nullptr;
lua_settop_t       g_lua_settop       = nullptr;
lua_getfield_t     g_lua_getfield     = nullptr;
lua_rawgeti_t      g_lua_rawgeti      = nullptr;
lua_type_t         g_lua_type         = nullptr;
lua_isstring_t     g_lua_isstring     = nullptr;
lua_isnumber_t     g_lua_isnumber     = nullptr;
lua_objlen_t       g_lua_objlen       = nullptr;
lua_pushboolean_t  g_lua_pushboolean  = nullptr;
lua_pushstring_t   g_lua_pushstring   = nullptr;
lua_createtable_t  g_lua_createtable  = nullptr;
lua_rawset_t       g_lua_rawset       = nullptr;
lua_settable_t     g_lua_settable     = nullptr;
lua_pushnil_t      g_lua_pushnil      = nullptr;
lua_next_t         g_lua_next         = nullptr;
lua_gettable_t     g_lua_gettable     = nullptr;
lua_pushvalue_t    g_lua_pushvalue    = nullptr;
lua_pushcclosure_t g_lua_pushcclosure = nullptr;
lua_pcall_t        g_lua_pcall        = nullptr;

bool ResolveLuaApi()
{
    if (!g_FoxLuaRegisterLibrary)
        g_FoxLuaRegisterLibrary = reinterpret_cast<FoxLuaRegisterLibrary_t>(ResolveGameAddress(gAddr.FoxLuaRegisterLibrary));
    if (!g_lua_tolstring)
        g_lua_tolstring = reinterpret_cast<lua_tolstring_t>(ResolveGameAddress(gAddr.lua_tolstring));
    if (!g_lua_tointeger)
        g_lua_tointeger = reinterpret_cast<lua_tointeger_t>(ResolveGameAddress(gAddr.lua_tointeger));
    if (!g_lua_tonumber)
        g_lua_tonumber = reinterpret_cast<lua_tonumber_t>(ResolveGameAddress(gAddr.lua_tonumber));
    if (!g_lua_toboolean)
        g_lua_toboolean = reinterpret_cast<lua_toboolean_t>(ResolveGameAddress(gAddr.lua_toboolean));
    if (!g_lua_pushnumber)
        g_lua_pushnumber = reinterpret_cast<lua_pushnumber_t>(ResolveGameAddress(gAddr.lua_pushnumber));
    if (!g_lua_gettop)
        g_lua_gettop = reinterpret_cast<lua_gettop_t>(ResolveGameAddress(gAddr.lua_gettop));
    if (!g_lua_settop)
        g_lua_settop = reinterpret_cast<lua_settop_t>(ResolveGameAddress(gAddr.lua_settop));
    if (!g_lua_getfield)
        g_lua_getfield = reinterpret_cast<lua_getfield_t>(ResolveGameAddress(gAddr.lua_getfield));
    if (!g_lua_rawgeti)
        g_lua_rawgeti = reinterpret_cast<lua_rawgeti_t>(ResolveGameAddress(gAddr.lua_rawgeti));
    if (!g_lua_type)
        g_lua_type = reinterpret_cast<lua_type_t>(ResolveGameAddress(gAddr.lua_type));
    if (!g_lua_isstring)
        g_lua_isstring = reinterpret_cast<lua_isstring_t>(ResolveGameAddress(gAddr.lua_isstring));
    if (!g_lua_isnumber)
        g_lua_isnumber = reinterpret_cast<lua_isnumber_t>(ResolveGameAddress(gAddr.lua_isnumber));
    if (!g_lua_objlen)
        g_lua_objlen = reinterpret_cast<lua_objlen_t>(ResolveGameAddress(gAddr.lua_objlen));
    if (!g_lua_pushboolean)
        g_lua_pushboolean = reinterpret_cast<lua_pushboolean_t>(ResolveGameAddress(gAddr.lua_pushboolean));
    if (!g_lua_pushstring)
        g_lua_pushstring = reinterpret_cast<lua_pushstring_t>(ResolveGameAddress(gAddr.lua_pushstring));
    if (!g_lua_createtable)
        g_lua_createtable = reinterpret_cast<lua_createtable_t>(ResolveGameAddress(gAddr.lua_createtable));
    if (!g_lua_rawset)
        g_lua_rawset = reinterpret_cast<lua_rawset_t>(ResolveGameAddress(gAddr.lua_rawset));
    if (!g_lua_settable)
        g_lua_settable = reinterpret_cast<lua_settable_t>(ResolveGameAddress(gAddr.lua_settable));
    if (!g_lua_pushnil)
        g_lua_pushnil = reinterpret_cast<lua_pushnil_t>(ResolveGameAddress(gAddr.lua_pushnil));
    if (!g_lua_next)
        g_lua_next = reinterpret_cast<lua_next_t>(ResolveGameAddress(gAddr.lua_next));
    if (!g_lua_gettable)
        g_lua_gettable = reinterpret_cast<lua_gettable_t>(ResolveGameAddress(gAddr.lua_gettable));
    if (!g_lua_pushvalue)
        g_lua_pushvalue = reinterpret_cast<lua_pushvalue_t>(ResolveGameAddress(gAddr.lua_pushvalue));
    if (!g_lua_pushcclosure)
        g_lua_pushcclosure = reinterpret_cast<lua_pushcclosure_t>(ResolveGameAddress(gAddr.lua_pushcclosure));
    if (!g_lua_pcall)
        g_lua_pcall = reinterpret_cast<lua_pcall_t>(ResolveGameAddress(gAddr.lua_pcall));

    return g_FoxLuaRegisterLibrary &&
        g_lua_tolstring && g_lua_tointeger && g_lua_tonumber && g_lua_toboolean && g_lua_pushnumber &&
        g_lua_gettop && g_lua_settop && g_lua_getfield && g_lua_gettable && g_lua_rawgeti && g_lua_type &&
        g_lua_isstring && g_lua_isnumber && g_lua_objlen && g_lua_pushboolean && g_lua_pushvalue &&
        g_lua_pushstring && g_lua_createtable && g_lua_rawset && g_lua_settable && g_lua_pushnil && g_lua_next;
}

int GetLuaTop(lua_State* L)
{
    if (!ResolveLuaApi() || !g_lua_gettop)
        return 0;
    return g_lua_gettop(L);
}

const char* GetLuaString(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tolstring)
        return nullptr;
    return g_lua_tolstring(L, idx, nullptr);
}

int GetLuaInt(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tointeger)
        return 0;
    return static_cast<int>(g_lua_tointeger(L, idx));
}

std::uint64_t GetLuaInt64(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tointeger)
        return 0;
    return static_cast<std::uint64_t>(g_lua_tointeger(L, idx));
}

bool GetLuaBool(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_toboolean)
        return false;
    return g_lua_toboolean(L, idx) != 0;
}

float GetLuaNumber(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tonumber)
        return 0.0f;
    return static_cast<float>(g_lua_tonumber(L, idx));
}

int LuaType(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_type)
        return -1;
    return g_lua_type(L, idx);
}

bool LuaIsString(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_isstring)
        return false;
    return g_lua_isstring(L, idx) != 0;
}

bool LuaIsNumber(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_isnumber)
        return false;
    return g_lua_isnumber(L, idx) != 0;
}

size_t LuaObjLen(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_objlen)
        return 0;
    return g_lua_objlen(L, idx);
}

std::uint32_t GetLuaStrCode32Arg(lua_State* L, int idx)
{
    if (GetLuaTop(L) < idx)
        return 0u;
    if (LuaIsNumber(L, idx))
        return static_cast<std::uint32_t>(GetLuaInt64(L, idx));
    if (LuaIsString(L, idx))
    {
        const char* s = GetLuaString(L, idx);
        if (!s || !s[0])
            return 0u;
        return FoxHashes::StrCode32(s);
    }
    return 0u;
}

std::uint32_t GetLuaFnvHash32Arg(lua_State* L, int idx)
{
    if (GetLuaTop(L) < idx)
        return 0u;
    if (LuaIsNumber(L, idx))
        return static_cast<std::uint32_t>(GetLuaInt64(L, idx));
    if (LuaIsString(L, idx))
    {
        const char* s = GetLuaString(L, idx);
        if (!s || !s[0])
            return 0u;
        return FoxHashes::FNVHash32(s);
    }
    return 0u;
}

void PushLuaBool(lua_State* L, bool value)
{
    if (!ResolveLuaApi() || !g_lua_pushboolean)
        return;
    g_lua_pushboolean(L, value ? 1 : 0);
}

void PushLuaNumber(lua_State* L, float value)
{
    if (!ResolveLuaApi() || !g_lua_pushnumber)
        return;
    g_lua_pushnumber(L, static_cast<lua_Number>(value));
}

void PushLuaString(lua_State* L, const char* s)
{
    if (!ResolveLuaApi() || !g_lua_pushstring)
        return;
    g_lua_pushstring(L, const_cast<char*>(s ? s : ""));
}

void PushLuaNil(lua_State* L)
{
    if (!ResolveLuaApi() || !g_lua_pushnil)
        return;
    g_lua_pushnil(L);
}

void LuaPop(lua_State* L, int count)
{
    if (!ResolveLuaApi() || !g_lua_settop)
        return;
    g_lua_settop(L, -count - 1);
}

void LuaGetField(lua_State* L, int idx, const char* fieldName)
{
    if (!ResolveLuaApi() || !g_lua_getfield || !fieldName)
        return;
    g_lua_getfield(L, idx, const_cast<char*>(fieldName));
}

void LuaRawGetI(lua_State* L, int idx, int n)
{
    if (!ResolveLuaApi() || !g_lua_rawgeti)
        return;
    g_lua_rawgeti(L, idx, n);
}

bool RegisterLuaLibrary(lua_State* L, const char* libName, luaL_Reg* funcs)
{
    if (!ResolveLuaApi() || !L || !libName || !funcs)
        return false;
    g_FoxLuaRegisterLibrary(L, libName, funcs);
#ifdef _DEBUG
    LogDebug("[V_FrameWork] Registered library: %s (L=%p)\n", libName, L);
#endif
    return true;
}
