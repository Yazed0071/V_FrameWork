#pragma once

#include <cstdint>

// Adds one important soldier by original GameObjectId (example: 0x0408).
// Params: gameObjectId, isOfficer
void Add_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer);

// Removes one important soldier by original GameObjectId.
// Params: gameObjectId
void Remove_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId);

// Clears all registered important soldiers and runtime holdup cache.
// Params: none
void Clear_VIPHoldupImportantGameObjectIds();

void Set_UseCustomNonVipHoldupRecovery(bool enabled);
bool Get_UseCustomNonVipHoldupRecovery();

// Installs the holdup-only hook.
// Params: none
bool Install_VIPHoldup_Hook();

// Removes the holdup-only hook.
// Params: none
bool Uninstall_VIPHoldup_Hook();