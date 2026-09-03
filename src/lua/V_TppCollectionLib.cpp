#include "pch.h"

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
}

#include <cstdint>
#include <set>
#include <string>

#include "V_TppCollectionLib.h"
#include "../hooks/collection/TppCollectionRuntime.h"
#include "../core/FoxHashes.h"
#include "LuaApi.h"
#include "log.h"

namespace
{
    static std::set<std::string> g_InjectedTypeConstants;

    static void InjectTypeConstant(lua_State* L, const char* typeName, std::int32_t typeId)
    {
        if (!typeName || !*typeName || !ResolveLuaApi() || !g_lua_settable)
            return;

        const int top = GetLuaTop(L);

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppCollection");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            g_lua_settop(L, top);
            return;
        }

        LuaGetField(L, -1, typeName);
        const bool vacant = (LuaType(L, -1) == LUA_TNIL);
        g_lua_settop(L, top + 1);

        if (!vacant && g_InjectedTypeConstants.count(typeName) == 0)
        {
            Log("[TppCollection] TppCollection.%s already exists (vanilla constant or "
                "function) - the custom type still works by name, but no Lua constant "
                "was injected for it\n", typeName);
            g_lua_settop(L, top);
            return;
        }

        PushLuaString(L, typeName);
        PushLuaNumber(L, static_cast<float>(typeId));
        g_lua_settable(L, -3);
        g_lua_settop(L, top);
        g_InjectedTypeConstants.insert(typeName);
    }

    static int __cdecl l_AddCollection(lua_State* L)
    {
        const char* name = GetLuaString(L, 1);

        int typeId = -1;
        if (LuaIsNumber(L, 2))
            typeId = GetLuaInt(L, 2);
        else
            typeId = TppCollection_FindTypeByName(GetLuaString(L, 2));

        const float x = GetLuaNumber(L, 3);
        const float y = GetLuaNumber(L, 4);
        const float z = GetLuaNumber(L, 5);
        const float rotY = LuaIsNumber(L, 6) ? GetLuaNumber(L, 6) : 0.f;

        PushLuaBool(L, typeId >= 0 && TppCollection_AddCustom(
            name, static_cast<std::uint32_t>(typeId), x, y, z, rotY));
        return 1;
    }


    static int __cdecl l_RemoveCollection(lua_State* L)
    {
        const char* name = GetLuaString(L, 1);

        PushLuaBool(L, TppCollection_RemoveCustom(name));
        return 1;
    }


    static float SpecNumber(lua_State* L, const char* key, float fallback)
    {
        const int top = GetLuaTop(L);
        LuaGetField(L, 2, key);
        const float v = LuaIsNumber(L, -1) ? GetLuaNumber(L, -1) : fallback;
        g_lua_settop(L, top);
        return v;
    }

    static float SpecVectorNumber(lua_State* L, const char* group, const char* key, float fallback)
    {
        const int top = GetLuaTop(L);
        float v = fallback;

        LuaGetField(L, 2, group);
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            LuaGetField(L, -1, key);
            if (LuaIsNumber(L, -1))
                v = GetLuaNumber(L, -1);
        }

        g_lua_settop(L, top);
        return v;
    }

    static bool SpecBool(lua_State* L, const char* key, bool fallback)
    {
        const int top = GetLuaTop(L);
        LuaGetField(L, 2, key);
        const bool v = (LuaType(L, -1) == LUA_TNIL) ? fallback : GetLuaBool(L, -1);
        g_lua_settop(L, top);
        return v;
    }

    static std::string SpecString(lua_State* L, const char* key)
    {
        const int top = GetLuaTop(L);
        LuaGetField(L, 2, key);
        const char* s = GetLuaString(L, -1);
        std::string out = s ? s : "";
        g_lua_settop(L, top);
        return out;
    }

    static int __cdecl l_RegisterCollectionType(lua_State* L)
    {
        const char* typeName = GetLuaString(L, 1);

        if (LuaType(L, 2) != LUA_TTABLE)
        {
            Log("[TppCollection] RegisterCollectionType('%s') needs a spec TABLE as its second "
                "argument - the old positional form was replaced, so this call registered "
                "nothing: { Model = \"/Assets/...\", Color = {r=,g=,b=,a=}, "
                "Strength = {x=,y=,z=,w=}, Yoffset = , GroundEffectSize = }\n",
                typeName ? typeName : "");
            PushLuaNil(L);
            return 1;
        }

        TppCollectionTypeDesc desc;
        const std::string modelPath = SpecString(L, "Model");
        desc.modelPath = modelPath.c_str();

        desc.r = SpecVectorNumber(L, "Color", "r", desc.r);
        desc.g = SpecVectorNumber(L, "Color", "g", desc.g);
        desc.b = SpecVectorNumber(L, "Color", "b", desc.b);
        desc.a = SpecVectorNumber(L, "Color", "a", desc.a);

        desc.fxStrengthX = SpecVectorNumber(L, "Strength", "x", desc.fxStrengthX);
        desc.fxStrengthY = SpecVectorNumber(L, "Strength", "y", desc.fxStrengthY);
        desc.fxStrengthZ = SpecVectorNumber(L, "Strength", "z", desc.fxStrengthZ);
        desc.fxStrengthW = SpecVectorNumber(L, "Strength", "w", desc.fxStrengthW);

        desc.effectYOffset    = SpecNumber(L, "Yoffset", desc.effectYOffset);
        desc.groundEffectSize = SpecNumber(L, "GroundEffectSize", desc.groundEffectSize);

        const std::string rootModel = SpecString(L, "RootModel");
        const std::string fova      = SpecString(L, "Fova");
        desc.rootModelPath = rootModel.empty() ? nullptr : rootModel.c_str();
        desc.fovaPath      = fova.empty() ? nullptr : fova.c_str();

        desc.isHerb     = SpecBool(L, "IsHerb", desc.isHerb);
        desc.isMaterial = SpecBool(L, "IsMaterial", desc.isMaterial);
        desc.isDiamond  = SpecBool(L, "IsDiamond", desc.isDiamond);

        const std::int32_t typeId = TppCollection_RegisterType(typeName, desc);
        if (typeId < 0)
        {
            PushLuaNil(L);
            return 1;
        }

        InjectTypeConstant(L, typeName, typeId);

        PushLuaNumber(L, static_cast<float>(typeId));
        return 1;
    }


    static void ReadTypeArg(lua_State* L, const char*& outName, std::int32_t& outId)
    {
        if (LuaIsNumber(L, 1))
        {
            outName = nullptr;
            outId   = static_cast<std::int32_t>(GetLuaNumber(L, 1));
            return;
        }
        outName = GetLuaString(L, 1);
        outId   = -1;
    }


    static int __cdecl l_SetCollectionTypeIcon(lua_State* L)
    {
        const char* name = nullptr;
        std::int32_t id = -1;
        ReadTypeArg(L, name, id);

        PushLuaBool(L, TppCollection_SetTypeIcon(name, id, GetLuaString(L, 2)));
        return 1;
    }


    static int __cdecl l_SetCollectionTypeLangId(lua_State* L)
    {
        const char* name = nullptr;
        std::int32_t id = -1;
        ReadTypeArg(L, name, id);

        std::uint64_t langId = 0;
        if (LuaIsNumber(L, 2))
            langId = static_cast<std::uint64_t>(static_cast<std::int64_t>(GetLuaNumber(L, 2)));
        else if (const char* key = GetLuaString(L, 2))
            if (*key)
                langId = FoxHashes::StrCode64(key);

        PushLuaBool(L, TppCollection_SetTypeLangId(name, id, langId));
        return 1;
    }


    static luaL_Reg g_VTppCollectionLib[] =
    {
        { "AddCollection",            l_AddCollection },
        { "RemoveCollection",         l_RemoveCollection },
        { "RegisterCollectionType",   l_RegisterCollectionType },
        { "SetCollectionTypeIcon",    l_SetCollectionTypeIcon },
        { "SetCollectionTypeLangId",  l_SetCollectionTypeLangId },

        { nullptr, nullptr }
    };
}

bool Register_V_TppCollectionLibrary(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_TppCollection", g_VTppCollectionLib);
}
