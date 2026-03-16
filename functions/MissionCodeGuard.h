#pragma once

#include <cstdint>

namespace MissionCodeGuard
{
    std::uint16_t GetCurrentMissionCode();
    bool IsMissionBlocked(std::uint16_t missionCode);
    bool ShouldBypassHooks();
}

// For hook callbacks that return void and should fall back to the original game function.
#define MISSION_GUARD_ORIGINAL_VOID(origFn, ...)         \
    do                                                   \
    {                                                    \
        if (MissionCodeGuard::ShouldBypassHooks())       \
        {                                                \
            if ((origFn) != nullptr)                     \
                (origFn)(__VA_ARGS__);                   \
            return;                                      \
        }                                                \
    } while (0)

// For plain void helpers/APIs that should just do nothing.
#define MISSION_GUARD_RETURN_VOID()                      \
    do                                                   \
    {                                                    \
        if (MissionCodeGuard::ShouldBypassHooks())       \
            return;                                      \
    } while (0)

// For bool helpers/APIs that should fail closed.
#define MISSION_GUARD_RETURN_FALSE()                     \
    do                                                   \
    {                                                    \
        if (MissionCodeGuard::ShouldBypassHooks())       \
            return false;                                \
    } while (0)