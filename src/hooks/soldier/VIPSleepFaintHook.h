#pragma once

#include <cstdint>


void Add_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId, bool isOfficer);


void Remove_VIPSleepFaintImportantGameObjectId(std::uint32_t gameObjectId);


void Clear_VIPSleepFaintImportantGameObjectIds();


bool IsVIPSleepFaintImportantSoldierIndex(std::uint16_t soldierIndex, bool* outIsOfficer);


bool Install_VIPSleepFaint_Hook();


bool Uninstall_VIPSleepFaint_Hook();