#pragma once

#include <cstdint>


void Add_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer);


void Remove_VIPHoldupImportantGameObjectId(std::uint32_t gameObjectId);


void Clear_VIPHoldupImportantGameObjectIds();


bool IsVIPHoldupImportantSoldierIndex(std::uint16_t soldierIndex, bool* outIsOfficer);

void Reset_CustomNonVipRecoveryTracking();


bool Install_VIPHoldup_Hook();


bool Uninstall_VIPHoldup_Hook();