#pragma once

#include <cstdint>


void Add_LostHostageTrap(std::uint32_t gameObjectId, int hostageType,
                         std::uint32_t customLostLabel = 0,
                         std::uint32_t customLostLabelTaken = 0);


void Remove_LostHostageTrap(std::uint32_t gameObjectId);


void Clear_LostHostagesTrap();




bool Install_LostHostage_Hooks();


bool Uninstall_LostHostage_Hooks();