#pragma once

#include <cstdint>

// Installs the VIP reaction hooks.
// Params: none
bool Install_VIPSleepWakeReaction_Hook();

// Removes the VIP reaction hooks.
// Params: none
bool Uninstall_VIPSleepWakeReaction_Hook();

// Adds an important target using a Lua/GameObjectId value.
// Params:
//   gameObjectId   = soldier GameObjectId from Lua
//   useRussianLine = true to use the Russian-specific unused hashes
void Add_VIPImportantGameObjectId(std::uint32_t gameObjectId, bool useRussianLine);

// Removes an important target using a Lua/GameObjectId value.
// Params: gameObjectId (uint32_t)
void Remove_VIPImportantGameObjectId(std::uint32_t gameObjectId);

// Clears all important targets.
// Params: none
void Clear_VIPImportantGameObjectIds();