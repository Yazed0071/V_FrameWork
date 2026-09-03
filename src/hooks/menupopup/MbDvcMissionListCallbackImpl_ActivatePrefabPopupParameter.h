#pragma once

#include <cstdint>

bool Set_MissionDeployWarning(std::uint16_t missionCode, const char* langId, const char* colorName);
void Clear_AllMissionDeployWarnings();

bool Install_MissionDeployWarning_Hook();
bool Uninstall_MissionDeployWarning_Hook();
