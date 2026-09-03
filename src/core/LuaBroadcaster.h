#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

struct lua_State;

lua_State* V_FrameWork_AnyLuaState();
unsigned long V_FrameWork_LuaOwnerThreadId();
void V_FrameWork_NoteLiveLuaState(lua_State* L);

namespace V_FrameWork
{
    static constexpr int kMaxBroadcastArgs = 8;

    struct LuaBroadcastArg
    {
        enum class Type
        {
            Nil,
            Boolean,
            Number,
            String
        };

        Type type = Type::Nil;
        double number = 0.0;
        bool boolean = false;
        std::string stringValue;

        LuaBroadcastArg() = default;

        LuaBroadcastArg(std::nullptr_t)
            : type(Type::Nil)
        {
        }

        LuaBroadcastArg(bool value)
            : type(Type::Boolean),
            boolean(value)
        {
        }

        LuaBroadcastArg(const char* value)
        {
            if (value)
            {
                type = Type::String;
                stringValue = value;
            }
            else
            {
                type = Type::Nil;
            }
        }

        LuaBroadcastArg(char* value)
            : LuaBroadcastArg(static_cast<const char*>(value))
        {
        }

        LuaBroadcastArg(const std::string& value)
            : type(Type::String),
            stringValue(value)
        {
        }

        LuaBroadcastArg(std::string&& value)
            : type(Type::String),
            stringValue(std::move(value))
        {
        }

        template <
            typename T,
            typename std::enable_if<
            std::is_arithmetic<T>::value &&
            !std::is_same<typename std::decay<T>::type, bool>::value,
            int
        >::type = 0>
        LuaBroadcastArg(T value)
            : type(Type::Number),
            number(static_cast<double>(value))
        {
        }

        template <
            typename T,
            typename std::enable_if<std::is_enum<T>::value, int>::type = 0>
        LuaBroadcastArg(T value)
            : type(Type::Number),
            number(static_cast<double>(
                static_cast<typename std::underlying_type<T>::type>(value)))
        {
        }
    };

    void EmitMessageValues(const char* category,
        const char* msg,
        const LuaBroadcastArg* args,
        int argCount);

    void EnterLuaPcall();
    void ExitLuaPcall();
    void DrainDeferredBroadcasts();

    inline void EmitMessage(const char* category, const char* msg)
    {
        EmitMessageValues(category, msg, nullptr, 0);
    }

    template <typename First, typename... Rest>
    inline void EmitMessage(const char* category,
        const char* msg,
        First&& first,
        Rest&&... rest)
    {
        static_assert(1 + sizeof...(Rest) <= kMaxBroadcastArgs,
            "V_FrameWork::EmitMessage supports a maximum of 8 optional arguments.");

        const LuaBroadcastArg values[] = {
            LuaBroadcastArg(std::forward<First>(first)),
            LuaBroadcastArg(std::forward<Rest>(rest))...
        };

        EmitMessageValues(category,
            msg,
            values,
            static_cast<int>(1 + sizeof...(Rest)));
    }
}