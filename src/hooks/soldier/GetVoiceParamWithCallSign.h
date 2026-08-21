#pragma once

#include <cstdint>


void Set_SoldierCallSign(std::uint32_t gameObjectId, std::uint8_t callSign);


void Remove_SoldierCallSign(std::uint32_t gameObjectId);


void Clear_SoldierCallSigns();


bool Install_SoldierCallSign_Hook();


bool Uninstall_SoldierCallSign_Hook();
