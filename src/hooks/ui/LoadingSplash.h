#pragma once

#include <cstdint>

void LoadingSplash_SetMainTexture(uint64_t textureHash, uint32_t missionCode = 0);
void LoadingSplash_ClearMainTexture(uint32_t missionCode = 0);
void LoadingSplash_SetBlurTexture(uint64_t textureHash, uint32_t missionCode = 0);
void LoadingSplash_ClearBlurTexture(uint32_t missionCode = 0);
void LoadingSplash_ClearTextures(uint32_t missionCode = 0);

bool Install_LoadingSplash_Hook();
bool Uninstall_LoadingSplash_Hook();
