#pragma once

struct lua_State;


int __cdecl l_RegisterOutfit(lua_State* L);
int __cdecl l_RegisterHeadOption(lua_State* L);
int __cdecl l_ExtendVanillaOutfit(lua_State* L);
int __cdecl l_GetOutfitInfo(lua_State* L);
int __cdecl l_AddToEquipDevelopTable(lua_State* L);

void OutfitLua_EnsureEquipDevelopBound();
