#pragma once

#include <cstdint>

void GameOverSplash_SetMainTexture(uint64_t textureHash, uint32_t missionCode = 0);
void GameOverSplash_ClearMainTexture(uint32_t missionCode = 0);
void GameOverSplash_SetBlurTexture(uint64_t textureHash, uint32_t missionCode = 0);
void GameOverSplash_ClearBlurTexture(uint32_t missionCode = 0);
void GameOverSplash_ClearTextures(uint32_t missionCode = 0);

bool Install_GameOverSplash_Hook();
bool Uninstall_GameOverSplash_Hook();
