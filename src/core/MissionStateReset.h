#pragma once

struct lua_State;

namespace MissionStateReset
{
    void EnsureFinalizerRegistered(lua_State* L);
    void PollMissionChange();
}
