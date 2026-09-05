#pragma once

#include <cstdint>

bool Install_MissionTelopBgTexture_Hook();
bool Uninstall_MissionTelopBgTexture_Hook();

void Set_MissionTelopSplashTexturePath(const char* path, std::uint32_t missionCode = 0);
void Unset_MissionTelopSplashTexturePath(std::uint32_t missionCode = 0);
