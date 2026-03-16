#pragma once

#include <cstdint>

// Registers one hostage to track for escape reports.
// Params: gameObjectId (uint32_t), hostageType (0=male, 1=female, 2=child)
void Add_LostHostage(std::uint32_t gameObjectId, int hostageType);

// Removes one tracked hostage.
// Params: gameObjectId (uint32_t)
void Remove_LostHostage(std::uint32_t gameObjectId);

// Clears all tracked hostages and pending escape state.
// Params: none
void Clear_LostHostages();

// Sets the custom speech label for one hostage type.
// Params: hostageType (0=male, 1=female, 2=child), speechLabel (uint32_t)
void Set_LostHostageSpeechLabel(int hostageType, std::uint32_t speechLabel);

// Installs the lost-hostage hooks.
// Params: none
bool Install_LostHostage_Hooks();

// Removes the lost-hostage hooks.
// Params: none
bool Uninstall_LostHostage_Hooks();