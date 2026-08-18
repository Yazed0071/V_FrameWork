#include "pch.h"

extern "C" {
    #include "lua.h"
}

#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "FoxHashes.h"
#include "log.h"
#include "LuaBroadcaster.h"
#include "LuaStackGuard.h"
#include "GetGameObjectIdWithIndex.h"

namespace
{
    static constexpr std::uint32_t kInvalidGameObjectId = 0xFFFFu;

    struct NativeGameObjectId
    {
        std::uint16_t value = 0xFFFF;
    };

    using GetGameObjectIdWithIndex_t =
        void(__fastcall*)(NativeGameObjectId* out,
            std::uint32_t typeArg,
            std::uint32_t index);

    static GetGameObjectIdWithIndex_t g_GetGameObjectIdWithIndex = nullptr;

    static bool IsTypeName(const char* lhs, const char* rhs)
    {
        if (!lhs || !rhs)
            return false;

        return std::strcmp(lhs, rhs) == 0;
    }

    static std::uint16_t LookupTypeIndex(const char* typeName)
    {
        if (IsTypeName(typeName, "TppSoldier2"))
            return TppGameObjectType::kSoldier2;

        return TppGameObjectType::kUnknown;
    }

    static std::uint32_t PackGameObjectId(std::uint16_t typeIndex,
        std::uint32_t index)
    {
        return (static_cast<std::uint32_t>(typeIndex) << 9) |
            (index & 0x1FFu);
    }

    using lua_getfield_t   = void(__fastcall*)(lua_State*, int, char*);
    using lua_type_t       = int(__fastcall*)(lua_State*, int);
    using lua_pushstring_t = void(__fastcall*)(lua_State*, char*);
    using lua_pcall_t      = int(__fastcall*)(lua_State*, int, int, int);
    using lua_tointeger_t  = std::intptr_t(__fastcall*)(lua_State*, int);
    using lua_gettop_t     = int(__fastcall*)(lua_State*);
    using lua_settop_t     = void(__fastcall*)(lua_State*, int);

    static constexpr int kLuaGlobalsIndex51 = -10002;

    template <typename Fn>
    static Fn ResolveLuaFn(std::uintptr_t resolvedAddr)
    {
        if (!resolvedAddr)
            return nullptr;
        return reinterpret_cast<Fn>(ResolveGameAddress(resolvedAddr));
    }

    static thread_local bool g_InByNameResolve = false;

    struct ByNameReentryGuard
    {
        bool entered;
        ByNameReentryGuard() : entered(!g_InByNameResolve)
        {
            if (entered)
                g_InByNameResolve = true;
        }
        ~ByNameReentryGuard()
        {
            if (entered)
                g_InByNameResolve = false;
        }
    };

    static bool ResolveByNameViaLua_Unguarded(const char* typeName,
        const char* instanceName,
        std::uint32_t& gameObjectIdOut)
    {
        lua_State* L = V_FrameWork_AnyLuaState();
        if (!L)
            return false;

        auto getfield   = ResolveLuaFn<lua_getfield_t>(gAddr.lua_getfield);
        auto luatype    = ResolveLuaFn<lua_type_t>(gAddr.lua_type);
        auto pushstring = ResolveLuaFn<lua_pushstring_t>(gAddr.lua_pushstring);
        auto pcall      = ResolveLuaFn<lua_pcall_t>(gAddr.lua_pcall);
        auto tointeger  = ResolveLuaFn<lua_tointeger_t>(gAddr.lua_tointeger);
        auto gettop     = ResolveLuaFn<lua_gettop_t>(gAddr.lua_gettop);
        auto settop     = ResolveLuaFn<lua_settop_t>(gAddr.lua_settop);

        if (!getfield || !luatype || !pushstring || !pcall || !tointeger ||
            !gettop || !settop)
            return false;

        bool ok = false;

        __try
        {
            const int savedTop = gettop(L);

            if (!luaguard::HasStackRoom(L, 6))
            {
                static bool s_logged = false;
                if (!s_logged)
                {
                    s_logged = true;
                    LogDebug("[GetGameObjectIdWithIndex] by-name resolve skipped (type=%s name=%s): "
                        "the running Lua frame is too tight to push safely; the step machine "
                        "retries next frame. Skipping prevents a Lua stack-overflow crash.\n",
                        typeName, instanceName);
                }
                return false;
            }

            getfield(L, kLuaGlobalsIndex51, const_cast<char*>("GameObject"));
            if (luatype(L, -1) != LUA_TTABLE)
            {
                settop(L, savedTop);
                return false;
            }

            getfield(L, -1, const_cast<char*>("GetGameObjectId"));
            if (luatype(L, -1) != LUA_TFUNCTION)
            {
                settop(L, savedTop);
                return false;
            }

            pushstring(L, const_cast<char*>(typeName));
            pushstring(L, const_cast<char*>(instanceName));

            const int err = pcall(L, 2, 1, 0);
            if (err == 0 && luatype(L, -1) == LUA_TNUMBER)
            {
                const std::uint32_t id =
                    static_cast<std::uint32_t>(tointeger(L, -1)) & 0x1FFFFu;
                if (id != 0xFFFFu)
                {
                    gameObjectIdOut = id;
                    ok = true;
                }
            }

            settop(L, savedTop);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[GetGameObjectIdWithIndex] SEH in by-name resolve type=%s name=%s\n",
                typeName, instanceName);
            return false;
        }

        return ok;
    }

    static bool TryResolveByNameViaLua(const char* typeName,
        const char* instanceName,
        std::uint32_t& gameObjectIdOut)
    {
        ByNameReentryGuard guard;
        if (!guard.entered)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LogDebug("[GetGameObjectIdWithIndex] re-entrant by-name resolve REFUSED "
                    "(type=%s name=%s) - the call arrived from inside our own "
                    "lua_pcall; that recursion is what wedged the main thread\n",
                    typeName, instanceName);
            }
            return false;
        }

        return ResolveByNameViaLua_Unguarded(typeName, instanceName,
                                             gameObjectIdOut);
    }

    static bool TryNativeWithStrCode32(const char* typeName,
        std::uint32_t index,
        std::uint32_t& gameObjectIdOut)
    {
        if (!g_GetGameObjectIdWithIndex)
            return false;

        const std::uint32_t typeNameId = FoxHashes::StrCode32(typeName);
        NativeGameObjectId result{};

        __try
        {
            g_GetGameObjectIdWithIndex(&result, typeNameId, index);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[GetGameObjectIdWithIndex] SEH exception type=%s index=%u\n",
                typeName,
                index);
            return false;
        }

        if (result.value == 0xFFFFu)
            return false;

        gameObjectIdOut = static_cast<std::uint32_t>(result.value);
        return true;
    }
}

bool Install_GetGameObjectIdWithIndex()
{
    if (g_GetGameObjectIdWithIndex)
        return true;

    if (!gAddr.GetGameObjectIdWithIndex)
    {
        Log("[GetGameObjectIdWithIndex] ERROR: address is missing for this build.\n");
        return false;
    }

    void* target = ResolveGameAddress(gAddr.GetGameObjectIdWithIndex);
    if (!target)
    {
        Log("[GetGameObjectIdWithIndex] ERROR: ResolveGameAddress failed. abs=0x%llX\n",
            static_cast<unsigned long long>(gAddr.GetGameObjectIdWithIndex));
        return false;
    }

    g_GetGameObjectIdWithIndex =
        reinterpret_cast<GetGameObjectIdWithIndex_t>(target);

    return true;
}

bool Uninstall_GetGameObjectIdWithIndex()
{
    g_GetGameObjectIdWithIndex = nullptr;

#ifdef _DEBUG
    LogDebug("[GetGameObjectIdWithIndex] uninstalled.\n");
#endif
    return true;
}

bool Is_GetGameObjectIdWithIndex_Installed()
{
    return g_GetGameObjectIdWithIndex != nullptr;
}

bool GetGameObjectIdWithIndex(const char* typeName,
    std::uint32_t index,
    std::uint32_t& gameObjectIdOut)
{
    gameObjectIdOut = kInvalidGameObjectId;

    if (!typeName || !typeName[0])
        return false;

    const std::uint16_t typeIndex = LookupTypeIndex(typeName);
    if (typeIndex != TppGameObjectType::kUnknown)
    {
        gameObjectIdOut = PackGameObjectId(typeIndex, index);
        return true;
    }

    if (!g_GetGameObjectIdWithIndex)
        Install_GetGameObjectIdWithIndex();

    if (TryNativeWithStrCode32(typeName, index, gameObjectIdOut))
        return true;

    LogDebug("[GetGameObjectIdWithIndex] unmapped type=%s index=%u (add it to TppGameObjectType)\n",
        typeName,
        index);

    return false;
}

bool GetSoldierGameObjectIdWithIndex(std::uint32_t soldierIndex,
    std::uint32_t& gameObjectIdOut)
{
    return GetGameObjectIdWithIndex("TppSoldier2",
        soldierIndex,
        gameObjectIdOut);
}

std::uint32_t GetGameObjectIdByIndex(const char* typeName,
    std::uint32_t index)
{
    std::uint32_t out = 0;
    return GetGameObjectIdWithIndex(typeName, index, out) ? out : 0;
}

bool GetGameObjectIdByName(const char* typeName,
    const char* instanceName,
    std::uint32_t& gameObjectIdOut)
{
    gameObjectIdOut = kInvalidGameObjectId;

    if (!typeName || !typeName[0] || !instanceName || !instanceName[0])
        return false;

    return TryResolveByNameViaLua(typeName, instanceName, gameObjectIdOut);
}
