#pragma once

struct lua_State;

constexpr int kCqcStanceNone      = -1;
constexpr int kCqcStanceStanding  = 0;
constexpr int kCqcStanceCrouching = 1;

bool Install_TargetCqcStance_Hook();
bool Uninstall_TargetCqcStance_Hook();

void Set_TargetCqcStance(int stance);
int  Get_TargetCqcStance();
void Clear_TargetCqcStance();
bool Request_PlayerTargetStance(lua_State* L, int stance);
bool IsInCqcHold();
