#pragma once

#include <cstdint>

void RewardPopupBg_SetTexture(uint64_t textureHash, uint32_t missionCode = 0);
void RewardPopupBg_ClearTexture(uint32_t missionCode = 0);
void RewardPopupBg_SetCurrentPopupTexture(uint64_t textureHash);

bool Install_RewardPopupBgTexture_Hook();
bool Uninstall_RewardPopupBgTexture_Hook();
