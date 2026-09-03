#pragma once

#include <cstdint>

struct lua_State;

void Register_V_TppGameObjectConstants(lua_State* L);
void Register_V_TppMbDevConstants(lua_State* L);
void Register_V_PlayerCqcStanceConstants(lua_State* L);
void Register_V_TppCallSignConstants(lua_State* L);
void Register_V_TppDataBaseConstants(lua_State* L);

bool IsListableDataBaseTab(std::int32_t tab);