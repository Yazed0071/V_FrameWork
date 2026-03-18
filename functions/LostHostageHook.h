#pragma once

#include <cstdint>

// Registers one hostage to track for escape reports.
// Params: gameObjectId (uint32_t), hostageType (0=male, 1=female, 2=child)
void Add_LostHostageTrap(std::uint32_t gameObjectId, int hostageType);

// Removes one tracked hostage.
// Params: gameObjectId (uint32_t)
void Remove_LostHostageTrap(std::uint32_t gameObjectId);

// Clears all tracked hostages and pending escape state.
// Params: none
void Clear_LostHostagesTrap();

// Sets whether the hostage was taken by the player.
// This value is copied into the next pending lost-hostage report when the trap hook fires.
// Params: playerTookHostage (bool)
void PlayerTookHostage(std::uint32_t gameObjectId, bool playerTookHostage);

// Installs the lost-hostage hooks.
// Params: none
bool Install_LostHostage_Hooks();

// Removes the lost-hostage hooks.
// Params: none
bool Uninstall_LostHostage_Hooks();