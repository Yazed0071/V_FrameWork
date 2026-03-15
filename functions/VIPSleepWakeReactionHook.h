#pragma once

#include <cstdint>

// Adds or updates one important target.
// Params:
//   gameObjectId = soldier GameObjectId from Lua/GameObject.GetGameObjectId
//   isRussian    = true to use the russian wake/holdup variants
//   isOfficer    = true to use the officer CP body-report label
void Add_VIPImportantGameObjectId(std::uint32_t gameObjectId, bool isRussian, bool isOfficer);

// Removes one important target.
// Params:
//   gameObjectId = soldier GameObjectId from Lua/GameObject.GetGameObjectId
void Remove_VIPImportantGameObjectId(std::uint32_t gameObjectId);

// Clears all important targets and runtime corpse cache.
// Params: none
void Clear_VIPImportantGameObjectIds();

// Installs the VIP wake/holdup/body-report hooks.
// Params: none
bool Install_VIPSleepWakeReaction_Hook();

// Removes the VIP wake/holdup/body-report hooks.
// Params: none
bool Uninstall_VIPSleepWakeReaction_Hook();