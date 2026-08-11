#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "LuaErrorThrow.h"
#include "log.h"
#include "../../lua/LuaApi.h"

namespace
{
    constexpr std::uintptr_t kAddr_LuaErrorThrow_En154 = 0x14006ACD0;

    using LuaErrorThrow_t = void (__fastcall*)(lua_State* L);

    LuaErrorThrow_t g_OrigLuaErrorThrow = nullptr;
    void*           g_HookLuaErrorThrow = nullptr;

    const char* ReadLuaErrorMessageSEH(lua_State* L)
    {
        __try
        {
            return L ? GetLuaString(L, -1) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void __fastcall hkLuaErrorThrow(lua_State* L)
    {
        const char* msg = nullptr;
        if (L && ResolveLuaApi())
            msg = ReadLuaErrorMessageSEH(L);

        Log("[LuaError] a Lua script raised an error; the engine converts it "
            "into a C++ exception that nothing catches, so the process dies "
            "here: %s\n",
            (msg && *msg) ? msg : "(message could not be read off the stack)");

        g_OrigLuaErrorThrow(L);
    }
}

bool Install_LuaErrorThrow()
{
    if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
        return true;

    void* target = ResolveGameAddress(kAddr_LuaErrorThrow_En154);
    if (!target)
        return true;

    if (!CreateAndEnableHook(target,
                             reinterpret_cast<void*>(&hkLuaErrorThrow),
                             reinterpret_cast<void**>(&g_OrigLuaErrorThrow)))
    {
        Log("[LuaError] hook install FAILED - a Lua script error will still "
            "kill the process, but without naming the script or the message\n");
        return true;
    }

    g_HookLuaErrorThrow = target;
    return true;
}

void Uninstall_LuaErrorThrow()
{
    if (g_HookLuaErrorThrow)
    {
        DisableAndRemoveHook(g_HookLuaErrorThrow);
        g_HookLuaErrorThrow = nullptr;
    }
    g_OrigLuaErrorThrow = nullptr;
}
