#pragma once

#include <cstdint>

struct lua_State;

bool EquipDevelop_UndevelopByDevelopId(std::uint32_t developId);

bool EquipDevelop_DevelopByDevelopId(std::uint32_t developId);

bool EquipDevelop_IsDevelopedByDevelopId(std::uint32_t developId);

void EquipDevelop_DrainPendingUndevelops();

void EquipDevelop_RequestDevelopRestore();
bool EquipDevelop_DrainIfRestorePending();

void EquipDevelop_InstallDevelopSyncHooks();

bool EquipDevelop_ShouldSuppressNativeDevelop(void* controller, std::uint16_t index);
void EquipDevelop_NotifyNativeDevelopChanged(void* controller, std::uint16_t index,
                                             bool developed);

int __cdecl l_SetEquipUndeveloped(lua_State* L);
int __cdecl l_SetEquipDeveloped(lua_State* L);
int __cdecl l_IsEquipDeveloped(lua_State* L);

bool EquipDevelop_SetNewByDevelopId(std::uint32_t developId, bool isNew);
bool EquipDevelop_IsNewByDevelopId(std::uint32_t developId);

int __cdecl l_SetEquipNew(lua_State* L);
int __cdecl l_IsEquipNew(lua_State* L);

bool EquipDevelop_SetVisibleByDevelopId(std::uint32_t developId, bool visible);
int __cdecl l_SetEquipDevelopVisible(lua_State* L);

void EquipDevelop_TriggerRequirementsMetAnnounce();

std::int32_t EquipDevelop_AnnouncingDevelopId();

bool EquipDevelop_IsDevelopableByDevelopId(std::uint32_t developId);
int __cdecl l_IsEquipDevelopable(lua_State* L);

void EquipDevelop_SetDevelopParent(std::uint32_t developId, std::uint32_t baseDevelopId);

bool EquipDevelop_IsDevelopInitiallyAvailable(std::uint32_t developId);

void EquipDevelop_SetDevelopInitiallyAvailable(std::uint32_t developId,
                                               bool initiallyAvailable);

int  EquipDevelop_BeginFobListSuppress();
bool EquipDevelop_IsFobListSuppressActive();
void EquipDevelop_EndFobListSuppress();

void* EquipDevelop_ResolveDevelopController();

bool EquipDevelop_IsDevelopTimerActive(std::uint16_t flowIndex);

void EquipDevelop_SetRecordNameOffsets(int nameOff, int infoOff,
                                       int listNameOff = -1);
