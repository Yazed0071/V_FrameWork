#include "pch.h"

extern "C" {
    #include "lua.h"
}

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "AddressSet.h"
#include "FoxHashes.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaBroadcaster.h"

namespace EquipDevelopAdd
{
    void PumpDevelopMenuWork();
}

namespace
{
    using lua_pcall_t = int(__fastcall*)(lua_State*, int, int, int);
    using lua_pushnumber_t = void(__fastcall*)(lua_State*, lua_Number);
    using lua_pushstring_t = void(__fastcall*)(lua_State*, char*);
    using lua_pushboolean_t = void(__fastcall*)(lua_State*, int);
    using lua_pushnil_t = void(__fastcall*)(lua_State*);
    using lua_settop_t = void(__fastcall*)(lua_State*, int);
    using lua_gettop_t = int(__fastcall*)(lua_State*);
    using lua_getfield_t = void(__fastcall*)(lua_State*, int, char*);
    using lua_type_t = int(__fastcall*)(lua_State*, int);
    using lua_tolstring_t = const char* (__fastcall*)(lua_State*, int, size_t*);
    using lua_pushvalue_t = void (__fastcall*)(lua_State*, int);
    using lua_rawset_t    = void (__fastcall*)(lua_State*, int);

    static constexpr int LUA_GLOBALSINDEX_51 = -10002;

    template <typename Fn>
    static Fn ResolveLua(uintptr_t resolvedAddr)
    {
        if (!resolvedAddr)
            return nullptr;

        return reinterpret_cast<Fn>(ResolveGameAddress(resolvedAddr));
    }

    struct LuaApi
    {
        lua_pcall_t       pcall = nullptr;
        lua_pushnumber_t  pushnumber = nullptr;
        lua_pushstring_t  pushstring = nullptr;
        lua_pushboolean_t pushboolean = nullptr;
        lua_pushnil_t     pushnil = nullptr;
        lua_settop_t      settop = nullptr;
        lua_gettop_t      gettop = nullptr;
        lua_getfield_t    getfield = nullptr;
        lua_type_t        type = nullptr;
        lua_tolstring_t   tolstring = nullptr;
        lua_pushvalue_t   pushvalue = nullptr;
        lua_rawset_t      rawset = nullptr;
    };

    static bool ResolveLuaApi(LuaApi& lua)
    {
        lua.pcall       = ResolveLua<lua_pcall_t>(gAddr.lua_pcall);
        lua.pushnumber  = ResolveLua<lua_pushnumber_t>(gAddr.lua_pushnumber);
        lua.pushstring  = ResolveLua<lua_pushstring_t>(gAddr.lua_pushstring);
        lua.pushboolean = ResolveLua<lua_pushboolean_t>(gAddr.lua_pushboolean);
        lua.pushnil     = ResolveLua<lua_pushnil_t>(gAddr.lua_pushnil);
        lua.settop      = ResolveLua<lua_settop_t>(gAddr.lua_settop);
        lua.gettop      = ResolveLua<lua_gettop_t>(gAddr.lua_gettop);
        lua.getfield    = ResolveLua<lua_getfield_t>(gAddr.lua_getfield);
        lua.type        = ResolveLua<lua_type_t>(gAddr.lua_type);
        lua.tolstring   = ResolveLua<lua_tolstring_t>(gAddr.lua_tolstring);
        lua.pushvalue   = ResolveLua<lua_pushvalue_t>(gAddr.lua_pushvalue);
        lua.rawset      = ResolveLua<lua_rawset_t>(gAddr.lua_rawset);

        return lua.pcall &&
            lua.pushnumber &&
            lua.pushstring &&
            lua.pushboolean &&
            lua.pushnil &&
            lua.settop &&
            lua.gettop &&
            lua.getfield &&
            lua.type &&
            lua.pushvalue &&
            lua.rawset;
    }

    static int ClampBroadcastArgCount(int argCount)
    {
        if (argCount <= 0)
            return 0;

        if (argCount > V_FrameWork::kMaxBroadcastArgs)
            return V_FrameWork::kMaxBroadcastArgs;

        return argCount;
    }

    thread_local int g_luaPcallDepth = 0;

    struct DeferredBroadcast
    {
        std::string category;
        std::string msg;
        std::vector<V_FrameWork::LuaBroadcastArg> args;
    };

    std::mutex                     g_deferredMutex;
    std::vector<DeferredBroadcast> g_deferredQueue;

    static bool SafeToEmitInline()
    {
        if (GetCurrentThreadId() != V_FrameWork_LuaOwnerThreadId())
            return false;
        return g_luaPcallDepth == 0;
    }

    static void EnqueueDeferredBroadcast(const char* category,
        const char* msg,
        const V_FrameWork::LuaBroadcastArg* args,
        int argCount)
    {
        DeferredBroadcast entry;
        entry.category = category;
        entry.msg = msg;
        const int n = ClampBroadcastArgCount(argCount);
        for (int i = 0; args && i < n; ++i)
            entry.args.push_back(args[i]);

        std::lock_guard<std::mutex> lk(g_deferredMutex);
        if (g_deferredQueue.size() < 512)
            g_deferredQueue.push_back(std::move(entry));
    }

    static constexpr std::size_t kLuaStateTopOffset       = 0x10;
    static constexpr std::size_t kLuaStateCiOffset        = 0x28;
    static constexpr std::size_t kLuaStateStackLastOffset = 0x38;
    static constexpr std::size_t kCallInfoTopOffset       = 0x10;
    static constexpr std::size_t kLuaTValueSize           = 0x10;
    static constexpr int         kBroadcastPushSlots      = 9;
    static constexpr int         kImplausibleFreeSlots    = 1000000;
    static constexpr int         kMaxHazardLogLines       = 20;

    struct LuaStackHeadroom
    {
        int physicalFree = 0;
        int frameFree    = 0;
    };

    static bool LuaStackHeadroomOf(lua_State* L, LuaStackHeadroom& out)
    {
        if (!L)
            return false;

        __try
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(L);
            const std::uintptr_t top =
                *reinterpret_cast<const std::uintptr_t*>(base + kLuaStateTopOffset);
            const std::uintptr_t stackLast =
                *reinterpret_cast<const std::uintptr_t*>(base + kLuaStateStackLastOffset);
            const std::uintptr_t ci =
                *reinterpret_cast<const std::uintptr_t*>(base + kLuaStateCiOffset);

            if (stackLast < top || !ci)
                return false;

            const std::uintptr_t ciTop =
                *reinterpret_cast<const std::uintptr_t*>(ci + kCallInfoTopOffset);
            if (ciTop < top)
                return false;

            const std::uintptr_t physical = (stackLast - top) / kLuaTValueSize;
            const std::uintptr_t frame    = (ciTop - top) / kLuaTValueSize;
            if (physical >= static_cast<std::uintptr_t>(kImplausibleFreeSlots)
                || frame >= static_cast<std::uintptr_t>(kImplausibleFreeSlots))
                return false;

            out.physicalFree = static_cast<int>(physical);
            out.frameFree    = static_cast<int>(frame);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool BroadcastStackHasRoom(lua_State* L, const char* category, const char* msg)
    {
        LuaStackHeadroom room{};
        if (!LuaStackHeadroomOf(L, room))
            return true;

        static int s_minFrame  = kImplausibleFreeSlots;
        static int s_skipLines = 0;

        const bool newMin = room.frameFree < s_minFrame;
        if (newMin)
            s_minFrame = room.frameFree;

        if (room.frameFree < kBroadcastPushSlots
            || room.physicalFree < kBroadcastPushSlots)
        {
            if (s_skipLines < kMaxHazardLogLines)
            {
                ++s_skipLines;
                Log("[LuaBroadcast] dropped category=%s msg=%s frameFree=%d physicalFree=%d "
                    "needed=%d - pushing here would run past the running Lua function's "
                    "ci->top and corrupt the VM, so this message is dropped instead.\n",
                    category, msg, room.frameFree, room.physicalFree, kBroadcastPushSlots);
            }
            return false;
        }

        if (newMin)
            Log("[LuaBroadcast] closest approach so far: frameFree=%d physicalFree=%d "
                "needed=%d category=%s msg=%s\n",
                room.frameFree, room.physicalFree, kBroadcastPushSlots, category, msg);

        return true;
    }

    static bool PushTppMainOnMessage(lua_State* L, const LuaApi& lua, int top0)
    {
        lua.getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppMain"));
        if (lua.type(L, -1) != LUA_TTABLE)
        {
            lua.settop(L, top0);
            return false;
        }

        lua.getfield(L, -1, const_cast<char*>("OnMessage"));
        if (lua.type(L, -1) != LUA_TFUNCTION)
        {
            lua.settop(L, top0);
            return false;
        }

        return true;
    }

    static volatile std::uint32_t* GetMessageResendCounter()
    {
        if (!gAddr.MessageResendCounter)
            return nullptr;

        return reinterpret_cast<volatile std::uint32_t*>(
            ResolveGameAddress(gAddr.MessageResendCounter));
    }

    static void PushRequiredBroadcastArgs(lua_State* L,
        const LuaApi& lua,
        const char* category,
        const char* msg)
    {
        lua.pushstring(L, const_cast<char*>(category));
        lua.pushstring(L, const_cast<char*>(msg));
    }

    static bool PushOneOptionalArg(lua_State* L,
        const LuaApi& lua,
        const V_FrameWork::LuaBroadcastArg& arg)
    {
        using Type = V_FrameWork::LuaBroadcastArg::Type;

        switch (arg.type)
        {
        case Type::Nil:
        {
            lua.pushnil(L);
            return true;
        }

        case Type::Boolean:
        {
            lua.pushboolean(L, arg.boolean ? 1 : 0);
            return true;
        }

        case Type::Number:
        {
            lua.pushnumber(L, static_cast<lua_Number>(arg.number));
            return true;
        }

        case Type::String:
        {
            lua.pushstring(L, const_cast<char*>(arg.stringValue.c_str()));
            return true;
        }
        }

        return false;
    }

    static int PushOptionalArgs(lua_State* L,
        const LuaApi& lua,
        const V_FrameWork::LuaBroadcastArg* args,
        int argCount)
    {
        if (!args || argCount <= 0)
            return 0;

        const int safeArgCount = ClampBroadcastArgCount(argCount);

        for (int i = 0; i < safeArgCount; ++i)
        {
            if (!PushOneOptionalArg(L, lua, args[i]))
                return i;
        }

        return safeArgCount;
    }

    static void LogBroadcastError(const LuaApi& lua,
        lua_State* L,
        int err,
        const char* category,
        const char* msg)
    {
        const char* errMsg = lua.tolstring ? lua.tolstring(L, -1, nullptr) : nullptr;

        Log("[V_FrameWork] Mission.SendMessage pcall err=%d category=%s msg=%s: %s\n",
            err,
            category,
            msg,
            errMsg ? errMsg : "<no message>");
    }

}

static void EmitInline(const char* category,
    const char* msg,
    const V_FrameWork::LuaBroadcastArg* args,
    int argCount)
{
    if (!category || !msg)
        return;

    lua_State* L = V_FrameWork_AnyLuaState();
    if (!L)
        return;

    LuaApi lua{};
    if (!ResolveLuaApi(lua))
        return;

    if (!BroadcastStackHasRoom(L, category, msg))
        return;

#ifdef _DEBUG
    const bool traceWalk =
        category[0] == 'U' && category[1] == 'I' && category[2] == '\0' &&
        std::strstr(msg, "WalkMan") != nullptr;
    static thread_local int s_walkTrace = 0;
#endif

    static thread_local int s_dispatchDepth = 0;
    if (s_dispatchDepth >= 8)
    {
        static thread_local int s_loggedDepth = 0;
        if (s_loggedDepth < 12)
        {
            ++s_loggedDepth;
            Log("[LuaBroadcast] re-entrant dispatch depth>=8: DROPPING category=%s msg=%s to "
                "break a message-recursion loop - a receiver of a prior message re-triggered an "
                "emit that led back here. If this is a WalkMan message, a walkman event is "
                "feeding itself (tape-finish auto-advance).\n",
                category, msg);
        }
        return;
    }
    ++s_dispatchDepth;

#ifdef _DEBUG
    if (traceWalk && s_walkTrace < 40)
    {
        ++s_walkTrace;
        Log("[LuaBroadcast] dispatch BEGIN %s.%s depth=%d - if no matching 'dispatch END' "
            "follows, TppMain.OnMessage for this walkman message hung (freeze is inside the "
            "Lua dispatch / a receiver)\n",
            category, msg, s_dispatchDepth);
    }
#endif

    DWORD  sehCode     = 0;
    PVOID  sehAddr     = nullptr;
    ULONG_PTR sehFault = 0;
    int    sehKind     = 0;

    __try
    {
        const int savedTop = lua.gettop(L);

        volatile std::uint32_t* counter = GetMessageResendCounter();
        const std::uint32_t     savedCounter = counter ? *counter : 0;
        if (counter) *counter = 0xFFFFu;

        if (PushTppMainOnMessage(L, lua, savedTop))
        {
            const std::uint32_t senderHash = FoxHashes::StrCode32(category);
            const std::uint32_t msgHash    = FoxHashes::StrCode32(msg);

            lua.pushnil(L);
            lua.pushnumber(L, static_cast<lua_Number>(senderHash));
            lua.pushnumber(L, static_cast<lua_Number>(msgHash));

            for (int i = 0; i < 4; ++i)
            {
                if (args && i < argCount)
                    PushOneOptionalArg(L, lua, args[i]);
                else
                    lua.pushnil(L);
            }

            const int err = lua.pcall(L, 7, 0, 0);
            if (err != 0)
            {
                const char* errMsg = lua.tolstring(L, -1, nullptr);
                Log("[V_FrameWork] TppMain.OnMessage pcall err=%d category=%s msg=%s: %s\n",
                    err, category, msg, errMsg ? errMsg : "<no message>");
            }
        }

        if (counter) *counter = savedCounter;
        lua.settop(L, savedTop);
    }
    __except ((sehCode = GetExceptionCode(),
               sehAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
               sehKind = (GetExceptionInformation()->ExceptionRecord->NumberParameters >= 2 ? 1 : 0),
               sehFault = (sehKind ? GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1] : 0),
               EXCEPTION_EXECUTE_HANDLER))
    {
        if (sehKind)
        {
            Log("[V_FrameWork] BroadcastMessage SEH category=%s msg=%s code=0x%08X at=%p faulting=0x%llX\n",
                category, msg, sehCode, sehAddr,
                static_cast<unsigned long long>(sehFault));
        }
        else
        {
            Log("[V_FrameWork] BroadcastMessage SEH category=%s msg=%s code=0x%08X at=%p\n",
                category, msg, sehCode, sehAddr);
        }
    }

    --s_dispatchDepth;

#ifdef _DEBUG
    if (traceWalk && s_walkTrace <= 40)
        Log("[LuaBroadcast] dispatch END   %s.%s\n", category, msg);
#endif
}

void V_FrameWork::EmitMessageValues(const char* category,
    const char* msg,
    const LuaBroadcastArg* args,
    int argCount)
{
    if (!category || !msg)
        return;

    if (SafeToEmitInline())
        EmitInline(category, msg, args, argCount);
    else
        EnqueueDeferredBroadcast(category, msg, args, argCount);
}

void V_FrameWork::DrainDeferredBroadcasts()
{
    static thread_local bool s_draining = false;
    if (s_draining)
        return;

    std::vector<DeferredBroadcast> local;
    {
        std::lock_guard<std::mutex> lk(g_deferredMutex);
        if (g_deferredQueue.empty())
            return;
        local.swap(g_deferredQueue);
    }

    s_draining = true;
    for (const DeferredBroadcast& d : local)
        EmitInline(d.category.c_str(), d.msg.c_str(),
            d.args.empty() ? nullptr : d.args.data(),
            static_cast<int>(d.args.size()));
    s_draining = false;
}

void V_FrameWork::EnterLuaPcall()
{
    ++g_luaPcallDepth;
}

void V_FrameWork::ExitLuaPcall()
{
    if (--g_luaPcallDepth <= 0)
    {
        g_luaPcallDepth = 0;
        DrainDeferredBroadcasts();
        EquipDevelopAdd::PumpDevelopMenuWork();
    }
}
