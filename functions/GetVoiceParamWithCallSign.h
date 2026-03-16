#pragma once

#include <cstdint>

// Marks one soldier to use hardcoded extra call-sign overrides.
// Params: gameObjectId
void Add_CallSignExtraSoldier(std::uint32_t gameObjectId);

// Removes one soldier from hardcoded extra call-sign overrides.
// Params: gameObjectId
void Remove_CallSignExtraSoldier(std::uint32_t gameObjectId);

// Clears all soldiers from hardcoded extra call-sign overrides.
// Params: none
void Clear_CallSignExtraSoldiers();

// Installs the hardcoded  GetVoiceParamWithCallSign hook.
// Params: none
bool Install_CallSignExtra_Hook();

// Removes the hardcoded  GetVoiceParamWithCallSign hook.
// Params: none
bool Uninstall_CallSignExtra_Hook();