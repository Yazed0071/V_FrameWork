#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "PlayerAlertPhase.h"

namespace
{
    using GetQuarkSystemTable_t = std::uint8_t* (__fastcall*)();

    constexpr std::size_t kQuarkTableScriptVars  = 0x98;
    constexpr std::size_t kScriptVarsPtr         = 0x10;
    constexpr std::size_t kScriptVarsPlayerPhase = 0x2e7b;
}

int PlayerAlertPhase::Read()
{
    auto qst = reinterpret_cast<GetQuarkSystemTable_t>(
        ResolveGameAddress(gAddr.Fox_GetQuarkSystemTable));
    if (!qst)
        return -1;

    __try
    {
        std::uint8_t* table = qst();
        if (!table)
            return -1;

        auto* holder = *reinterpret_cast<std::uint8_t**>(table + kQuarkTableScriptVars);
        if (!holder)
            return -1;

        auto* scriptVars = *reinterpret_cast<std::uint8_t**>(holder + kScriptVarsPtr);
        if (!scriptVars)
            return -1;

        return static_cast<int>(*(scriptVars + kScriptVarsPlayerPhase));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

bool PlayerAlertPhase::IsAtOrAbove(int phase)
{
    const int current = Read();
    return current >= 0 && current >= phase;
}
