#pragma once

#include <cstdint>

// Installs all UI texture override hooks.
// Params: none
bool Install_UiTextureOverrides_Hook();

// Removes all UI texture override hooks.
// Params: none
bool Uninstall_UiTextureOverrides_Hook();

// Sets the default DD equip background texture hash.
// Params: textureHash (uint64_t)
void EquipBg_SetDefaultTexture(uint64_t textureHash);

// Clears the default DD equip background texture override.
// Params: none
void EquipBg_ClearDefaultTexture();

// Sets the enemy-weapon equip background texture hash.
// Params: textureHash (uint64_t)
void EquipBg_SetEnemyWeaponTexture(uint64_t textureHash);

// Clears the enemy-weapon equip background texture override.
// Params: none
void EquipBg_ClearEnemyWeaponTexture();

// Sets a custom enemy equip background texture for a specific equipId.
// Params: equipId (int), textureHash (uint64_t)
void EquipBg_SetEnemyEquipTexture(int equipId, uint64_t textureHash);

// Clears a custom enemy equip background texture for a specific equipId.
// Params: equipId (int)
void EquipBg_ClearEnemyEquipTexture(int equipId);

// Sets a custom equip background texture for a specific equipId.
// Params: equipId (int), textureHash (uint64_t)
void EquipBg_SetEquipTexture(int equipId, uint64_t textureHash);

// Clears a custom equip background texture for a specific equipId.
// Params: equipId (int)
void EquipBg_ClearEquipTexture(int equipId);

// Clears all per-equip background overrides.
// Params: none
void EquipBg_ClearAllEquipTextures();

// Sets the loading splash main texture hash.
// Params: textureHash (uint64_t)
void LoadingSplash_SetMainTexture(uint64_t textureHash);

// Clears the loading splash main texture override.
// Params: none
void LoadingSplash_ClearMainTexture();

// Sets the loading splash blur texture hash.
// Params: textureHash (uint64_t)
void LoadingSplash_SetBlurTexture(uint64_t textureHash);

// Clears the loading splash blur texture override.
// Params: none
void LoadingSplash_ClearBlurTexture();

// Clears both loading splash overrides.
// Params: none
void LoadingSplash_ClearTextures();

// Sets the game over splash main texture hash.
// Params: textureHash (uint64_t)
void GameOverSplash_SetMainTexture(uint64_t textureHash);

// Clears the game over splash main texture override.
// Params: none
void GameOverSplash_ClearMainTexture();

// Sets the game over splash blur texture hash.
// Params: textureHash (uint64_t)
void GameOverSplash_SetBlurTexture(uint64_t textureHash);

// Clears the game over splash blur texture override.
// Params: none
void GameOverSplash_ClearBlurTexture();

// Clears both game over splash overrides.
// Params: none
void GameOverSplash_ClearTextures();