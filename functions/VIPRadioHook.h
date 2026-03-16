#pragma once

#include <cstdint>

// Installs the VIP radio hooks.
// Params: none
bool Install_VIPRadio_Hook();

// Removes the VIP radio hooks.
// Params: none
bool Uninstall_VIPRadio_Hook();

// Adds one important target by gameObjectId.
// Params: gameObjectId, isOfficer
void Add_VIPRadioImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer);

// Adds one important target by gameObjectId + soldierIndex.
// Params: gameObjectId, soldierIndex, isOfficer
void Add_VIPRadioImportantTarget(std::uint32_t gameObjectId, std::uint16_t soldierIndex, bool isOfficer);

// Removes one important target by gameObjectId.
// Params: gameObjectId
void Remove_VIPRadioImportantGameObjectId(std::uint32_t gameObjectId);

// Clears all important targets and runtime state.
// Params: none
void Clear_VIPRadioImportantGameObjectIds();

// Queues a discovered important body using gameObjectId.
// Params: foundGameObjectId
bool Notify_VIPRadioBodyDiscovered(std::uint32_t foundGameObjectId);

// Queues a discovered important body using gameObjectId + soldierIndex.
// Params: foundGameObjectId, foundSoldierIndex
bool Notify_VIPRadioBodyDiscoveredTarget(std::uint32_t foundGameObjectId, std::uint16_t foundSoldierIndex);

// Returns true only when there is exactly one recent important corpse from RequestCorpse.
// Params: outSoldierIndex, outIsOfficer
bool Try_GetSingleRecentImportantCorpseIndex(std::uint16_t& outSoldierIndex, bool& outIsOfficer);