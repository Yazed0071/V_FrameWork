#pragma once

#include <cstdint>

// Adds one important soldier by original GameObjectId (example: 0x0408).
// Params: gameObjectId, isOfficer
void Add_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer);

// Removes one important soldier by original GameObjectId.
// Params: gameObjectId
void Remove_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId);

// Clears all registered important soldiers and runtime wake cache.
// Params: none
void Clear_VIPSleepFaintImportantGameObjectIds();

// Installs the sleep/faint-only hooks.
// Params: none
bool Install_VIPSleepFaint_Hook();

// Removes the sleep/faint-only hooks.
// Params: none
bool Uninstall_VIPSleepFaint_Hook();