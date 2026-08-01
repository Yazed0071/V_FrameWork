#pragma once

#include <cstdint>

void Set_CassetteWalkmanProgrammatic(bool programmatic);
void Emit_CassetteWalkmanStart(std::uint32_t trackId);

bool Install_CassetteWalkmanEvents_Hook();
bool Uninstall_CassetteWalkmanEvents_Hook();
