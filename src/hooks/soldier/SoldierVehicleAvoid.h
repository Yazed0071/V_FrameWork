#pragma once

#include <cstdint>

bool Install_SoldierVehicleAvoid_Hook();
bool Uninstall_SoldierVehicleAvoid_Hook();

void Set_SoldierIgnoreVehicle(std::uint32_t gameObjectId, bool enabled);
bool Soldier_IgnoresVehicle(std::uint32_t soldierIndex);
bool SoldierVehicleAvoid_ShouldDropNotice(std::uint32_t soldierIndex, std::uint8_t noticeType,
                                          const void* noticeBlob);
void Clear_AllSoldierIgnoreVehicle();
