#include "pch.h"

#include <cstdint>

#include "HookUtils.h"
#include "MissionCodeGuard.h"

namespace
{
    static constexpr std::uintptr_t ABS_GetCurrentMissionCode = 0x145E5EE70ull;

    using GetCurrentMissionCode_t = std::uint16_t(__fastcall*)();

    static GetCurrentMissionCode_t g_GetCurrentMissionCode = nullptr;
}

namespace MissionCodeGuard
{
    std::uint16_t GetCurrentMissionCode()
    {
        if (!g_GetCurrentMissionCode)
        {
            g_GetCurrentMissionCode =
                reinterpret_cast<GetCurrentMissionCode_t>(ResolveGameAddress(ABS_GetCurrentMissionCode));
        }

        if (!g_GetCurrentMissionCode)
            return 0;

        return g_GetCurrentMissionCode();
    }

    bool IsMissionBlocked(std::uint16_t missionCode)
    {
        return missionCode == 50050u || missionCode == 50055u;
    }

    bool ShouldBypassHooks()
    {
        return IsMissionBlocked(GetCurrentMissionCode());
    }
}