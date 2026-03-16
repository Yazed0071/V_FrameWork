#include "pch.h"
#include "UiTextureOverrides.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>

#include "FoxHashes.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"

namespace
{
    using SetEquipBackgroundTexture_t = uint8_t(__fastcall*)(int equipId, void* sortieWeaponNode);
    using SetTextureName_t = void(__fastcall*)(void* modelNodeMesh, uint64_t textureHash, uint64_t slotHash, int unk);
    using LoadingScreenOrGameOverSplash2_t = void(__fastcall*)(void* self);
    using GameOverSetVisible_t = void(__fastcall*)(uint64_t* layout, char visible);

    // Absolute address of ui::equip::SetEquipBackgroundTexture.
    // Params: equipId (int), sortieWeaponNode (void*)
    static constexpr uintptr_t ABS_SetEquipBackgroundTexture = 0x145F236F0ull;

    // Absolute address of fox::ui::ModelNodeMesh::SetTextureName.
    // Params: modelNodeMesh (void*), textureHash (uint64_t), slotHash (uint64_t), unk (int)
    static constexpr uintptr_t ABS_SetTextureName = 0x141DC78F0ull;

    // Absolute address of ui::loading::LoadingScreenOrGameOverSplash2.
    // Params: self (void*)
    static constexpr uintptr_t ABS_LoadingScreenOrGameOverSplash2 = 0x145CD0630ull;

    // Absolute address of tpp::ui::menu::GameOverEvCall::MainLayout::SetVisible.
    // Params: layout (uint64_t*), visible (char)
    static constexpr uintptr_t ABS_GameOverSetVisible = 0x145CB8890ull;

    // Slot hash used by the equip background function for Mask_Texture.
    static uint64_t g_MaskTextureSlotHash = 0;

    // Slot hash used by loading/game-over splash main textures.
    static constexpr uint64_t SLOT_MAIN_TEXTURE = 0x3bbf9889ull;

    // Slot hash used by the blur layer on game-over nodes 8 and 9.
    static constexpr uint64_t SLOT_BLUR_LAYER = 0x8d982b8eull;

    static SetEquipBackgroundTexture_t      g_OrigSetEquipBackgroundTexture = nullptr;
    static SetTextureName_t                 g_SetTextureName = nullptr;
    static LoadingScreenOrGameOverSplash2_t g_OrigLoadingScreenOrGameOverSplash2 = nullptr;
    static GameOverSetVisible_t             g_OrigGameOverSetVisible = nullptr;

    static uint64_t g_DefaultTexture = 0;
    static uint64_t g_EnemyWeaponTexture = 0;
    static std::unordered_map<int, uint64_t> g_PerEquipTextures;
    static std::unordered_map<int, uint64_t> g_PerEnemyEquipTextures;

    struct SplashConfig
    {
        uint64_t mainTexture = 0;
        uint64_t blurTexture = 0;
    };

    static SplashConfig g_LoadingConfig;
    static SplashConfig g_GameOverConfig;

    // Returns true when the vanilla function would draw the DD weapon background.
    // Params: equipId (int), sortieWeaponNode (void*)
    static bool ShouldUseVanillaDdWeaponBg(int equipId, void* sortieWeaponNode)
    {
        if (sortieWeaponNode == nullptr)
            return false;

        if (equipId == 0)
            return false;

        if (static_cast<uint32_t>(equipId - 1) <= 0x47u)
            return false;

        if (static_cast<uint32_t>(equipId - 0x7d) <= 3u)
            return false;

        return true;
    }

    // Returns true when the equipId is one of the unique prosthetic arm IDs.
    // Params: equipId (int)
    static bool IsUniqueArmEquip(int equipId)
    {
        return equipId >= 0x203 && equipId <= 0x206;
    }

    // Returns true when the equipId is in the non-DD category that vanilla skips.
    // Params: equipId (int), sortieWeaponNode (void*)
    static bool IsEnemyWeaponLikeCategory(int equipId, void* sortieWeaponNode)
    {
        if (sortieWeaponNode == nullptr)
            return false;

        if (equipId == 0)
            return false;

        return !ShouldUseVanillaDdWeaponBg(equipId, sortieWeaponNode);
    }

    // Applies a texture directly to a ModelNodeMesh.
    // Params: modelNodeMesh (void*), textureHash (uint64_t)
    static uint8_t ApplyTexture(void* modelNodeMesh, uint64_t textureHash)
    {
        if (!modelNodeMesh || !textureHash || !g_SetTextureName)
            return 0;

        g_SetTextureName(modelNodeMesh, textureHash, g_MaskTextureSlotHash, 2);
        return 1;
    }

    // Resolves the direct-call helpers used by this module.
    // Params: none
    static bool ResolveUiHelpers()
    {
        if (!g_SetTextureName)
        {
            g_SetTextureName = reinterpret_cast<SetTextureName_t>(
                ResolveGameAddress(ABS_SetTextureName));
        }

        if (g_MaskTextureSlotHash == 0)
        {
            g_MaskTextureSlotHash = static_cast<uint64_t>(FoxHashes::StrCode32("Mask_Texture"));
        }

        return g_SetTextureName != nullptr && g_MaskTextureSlotHash != 0;
    }
}

// Sets the default DD equip background texture hash.
// Params: textureHash (uint64_t)
void EquipBg_SetDefaultTexture(uint64_t textureHash)
{
    g_DefaultTexture = textureHash;
    Log("[EquipBg] DefaultTexture = 0x%llX\n",
        static_cast<unsigned long long>(textureHash));
}

// Clears the default DD equip background texture override.
// Params: none
void EquipBg_ClearDefaultTexture()
{
    g_DefaultTexture = 0;
    Log("[EquipBg] DefaultTexture cleared\n");
}

// Sets the enemy-weapon equip background texture hash.
// Params: textureHash (uint64_t)
void EquipBg_SetEnemyWeaponTexture(uint64_t textureHash)
{
    g_EnemyWeaponTexture = textureHash;
    Log("[EquipBg] EnemyWeaponTexture = 0x%llX\n",
        static_cast<unsigned long long>(textureHash));
}

// Clears the enemy-weapon equip background texture override.
// Params: none
void EquipBg_ClearEnemyWeaponTexture()
{
    g_EnemyWeaponTexture = 0;
    Log("[EquipBg] EnemyWeaponTexture cleared\n");
}



// Sets a custom equip background texture for a specific equipId.
// Params: equipId (int), textureHash (uint64_t)
void EquipBg_SetEquipTexture(int equipId, uint64_t textureHash)
{
    g_PerEquipTextures[equipId] = textureHash;
    Log("[EquipBg] EquipId %d -> 0x%llX\n",
        equipId,
        static_cast<unsigned long long>(textureHash));
}

// Clears a custom equip background texture for a specific equipId.
// Params: equipId (int)
void EquipBg_ClearEquipTexture(int equipId)
{
    g_PerEquipTextures.erase(equipId);
    Log("[EquipBg] EquipId %d cleared\n", equipId);
}

// Sets a custom enemy equip background texture for a specific equipId.
// Params: equipId (int), textureHash (uint64_t)
void EquipBg_SetEnemyEquipTexture(int equipId, uint64_t textureHash)
{
    g_PerEnemyEquipTextures[equipId] = textureHash;
    Log("[EquipBg][EnemyPerEquip] EquipId %d -> 0x%llX\n",
        equipId,
        static_cast<unsigned long long>(textureHash));
}

// Clears a custom enemy equip background texture for a specific equipId.
// Params: equipId (int)
void EquipBg_ClearEnemyEquipTexture(int equipId)
{
    g_PerEnemyEquipTextures.erase(equipId);
    Log("[EquipBg][EnemyPerEquip] EquipId %d cleared\n", equipId);
}

// Clears all per-equip background overrides.
// Params: none
// Clears all per-equip background overrides.
// Params: none
void EquipBg_ClearAllEquipTextures()
{
    g_PerEquipTextures.clear();
    g_PerEnemyEquipTextures.clear();
    Log("[EquipBg] All per-equip textures cleared\n");
}

// Sets the loading splash main texture hash.
// Params: textureHash (uint64_t)
void LoadingSplash_SetMainTexture(uint64_t textureHash)
{
    g_LoadingConfig.mainTexture = textureHash;
    Log("[LoadingSplash] MainTexture = 0x%llX\n",
        static_cast<unsigned long long>(textureHash));
}

// Clears the loading splash main texture override.
// Params: none
void LoadingSplash_ClearMainTexture()
{
    g_LoadingConfig.mainTexture = 0;
    Log("[LoadingSplash] MainTexture cleared\n");
}

// Sets the loading splash blur texture hash.
// Params: textureHash (uint64_t)
void LoadingSplash_SetBlurTexture(uint64_t textureHash)
{
    g_LoadingConfig.blurTexture = textureHash;
    Log("[LoadingSplash] BlurTexture = 0x%llX\n",
        static_cast<unsigned long long>(textureHash));
}

// Clears the loading splash blur texture override.
// Params: none
void LoadingSplash_ClearBlurTexture()
{
    g_LoadingConfig.blurTexture = 0;
    Log("[LoadingSplash] BlurTexture cleared\n");
}

// Clears both loading splash overrides.
// Params: none
void LoadingSplash_ClearTextures()
{
    g_LoadingConfig.mainTexture = 0;
    g_LoadingConfig.blurTexture = 0;
    Log("[LoadingSplash] All textures cleared\n");
}

// Sets the game over splash main texture hash.
// Params: textureHash (uint64_t)
void GameOverSplash_SetMainTexture(uint64_t textureHash)
{
    g_GameOverConfig.mainTexture = textureHash;
    Log("[GameOverSplash] MainTexture = 0x%llX\n",
        static_cast<unsigned long long>(textureHash));
}

// Clears the game over splash main texture override.
// Params: none
void GameOverSplash_ClearMainTexture()
{
    g_GameOverConfig.mainTexture = 0;
    Log("[GameOverSplash] MainTexture cleared\n");
}

// Sets the game over splash blur texture hash.
// Params: textureHash (uint64_t)
void GameOverSplash_SetBlurTexture(uint64_t textureHash)
{
    g_GameOverConfig.blurTexture = textureHash;
    Log("[GameOverSplash] BlurTexture = 0x%llX\n",
        static_cast<unsigned long long>(textureHash));
}

// Clears the game over splash blur texture override.
// Params: none
void GameOverSplash_ClearBlurTexture()
{
    g_GameOverConfig.blurTexture = 0;
    Log("[GameOverSplash] BlurTexture cleared\n");
}

// Clears both game over splash overrides.
// Params: none
void GameOverSplash_ClearTextures()
{
    g_GameOverConfig.mainTexture = 0;
    g_GameOverConfig.blurTexture = 0;
    Log("[GameOverSplash] All textures cleared\n");
}

// Hooked version of ui::equip::SetEquipBackgroundTexture.
// Params: equipId (int), sortieWeaponNode (void*)
static uint8_t __fastcall hkSetEquipBackgroundTexture(int equipId, void* sortieWeaponNode)
{
    if (!ResolveUiHelpers())
        return g_OrigSetEquipBackgroundTexture(equipId, sortieWeaponNode);

    const auto it = g_PerEquipTextures.find(equipId);
    if (it != g_PerEquipTextures.end())
    {
        Log("[EquipBg][PerEquip] equipId=%d -> 0x%llX\n",
            equipId,
            static_cast<unsigned long long>(it->second));
        return ApplyTexture(sortieWeaponNode, it->second);
    }

    if (IsEnemyWeaponLikeCategory(equipId, sortieWeaponNode))
    {
        const auto enemyIt = g_PerEnemyEquipTextures.find(equipId);
        if (enemyIt != g_PerEnemyEquipTextures.end())
        {
            Log("[EquipBg][EnemyPerEquip] equipId=%d -> 0x%llX\n",
                equipId,
                static_cast<unsigned long long>(enemyIt->second));
            return ApplyTexture(sortieWeaponNode, enemyIt->second);
        }

        if (g_EnemyWeaponTexture != 0)
        {
            Log("[EquipBg][EnemyWeapon] equipId=%d -> 0x%llX\n",
                equipId,
                static_cast<unsigned long long>(g_EnemyWeaponTexture));
            return ApplyTexture(sortieWeaponNode, g_EnemyWeaponTexture);
        }
    }

    if (ShouldUseVanillaDdWeaponBg(equipId, sortieWeaponNode) &&
        !IsUniqueArmEquip(equipId) &&
        g_DefaultTexture != 0)
    {
        Log("[EquipBg][Default] equipId=%d -> 0x%llX\n",
            equipId,
            static_cast<unsigned long long>(g_DefaultTexture));
        return ApplyTexture(sortieWeaponNode, g_DefaultTexture);
    }

    return g_OrigSetEquipBackgroundTexture(equipId, sortieWeaponNode);
}

// Hooked version of ui::loading::LoadingScreenOrGameOverSplash2.
// Params: self (void*)
static void __fastcall hkLoadingScreenOrGameOverSplash2(void* self)
{
    g_OrigLoadingScreenOrGameOverSplash2(self);

    if (!self || !ResolveUiHelpers())
        return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(self);
    void* const mainNode = *reinterpret_cast<void**>(base + 0x9d8);
    void* const blurNode = *reinterpret_cast<void**>(base + 0x9e0);

    if (g_LoadingConfig.mainTexture != 0 && mainNode)
    {
        Log("[LoadingSplash][OverrideMain] node=%p -> 0x%llX\n",
            mainNode,
            static_cast<unsigned long long>(g_LoadingConfig.mainTexture));
        g_SetTextureName(mainNode, g_LoadingConfig.mainTexture, SLOT_MAIN_TEXTURE, 2);
    }

    if (g_LoadingConfig.blurTexture != 0 && blurNode)
    {
        Log("[LoadingSplash][OverrideBlur] node=%p -> 0x%llX\n",
            blurNode,
            static_cast<unsigned long long>(g_LoadingConfig.blurTexture));
        g_SetTextureName(blurNode, g_LoadingConfig.blurTexture, SLOT_MAIN_TEXTURE, 2);
    }
}


static void __fastcall hkGameOverSetVisible(uint64_t* layout, char visible)
{
    if (MissionCodeGuard::ShouldBypassHooks())
    {
        g_OrigGameOverSetVisible(layout, visible);
        return;
    }

    g_OrigGameOverSetVisible(layout, visible);

    if (!visible || !layout || !ResolveUiHelpers())
        return;

    void* const node8 = reinterpret_cast<void*>(layout[8]);
    void* const node9 = reinterpret_cast<void*>(layout[9]);
    void* const node10 = reinterpret_cast<void*>(layout[10]);
    void* const node11 = reinterpret_cast<void*>(layout[11]);

    if (g_GameOverConfig.mainTexture != 0)
    {
        if (node8)
            g_SetTextureName(node8, g_GameOverConfig.mainTexture, SLOT_MAIN_TEXTURE, 2);
        if (node9)
            g_SetTextureName(node9, g_GameOverConfig.mainTexture, SLOT_MAIN_TEXTURE, 2);

        Log("[GameOverSplash][OverrideMain] main=0x%llX\n",
            static_cast<unsigned long long>(g_GameOverConfig.mainTexture));
    }

    if (g_GameOverConfig.blurTexture != 0)
    {
        if (node8)
            g_SetTextureName(node8, g_GameOverConfig.blurTexture, SLOT_BLUR_LAYER, 2);
        if (node9)
            g_SetTextureName(node9, g_GameOverConfig.blurTexture, SLOT_BLUR_LAYER, 2);
        if (node10)
            g_SetTextureName(node10, g_GameOverConfig.blurTexture, SLOT_MAIN_TEXTURE, 2);
        if (node11)
            g_SetTextureName(node11, g_GameOverConfig.blurTexture, SLOT_MAIN_TEXTURE, 2);

        Log("[GameOverSplash][OverrideBlur] blur=0x%llX\n",
            static_cast<unsigned long long>(g_GameOverConfig.blurTexture));
    }
}

// Installs the unified UI texture override hooks.
// Params: none
bool Install_UiTextureOverrides_Hook()
{
    ResolveUiHelpers();

    void* targetEquip = ResolveGameAddress(ABS_SetEquipBackgroundTexture);
    void* targetLoading = ResolveGameAddress(ABS_LoadingScreenOrGameOverSplash2);
    void* targetGameOver = ResolveGameAddress(ABS_GameOverSetVisible);

    if (!targetEquip || !targetLoading || !targetGameOver || !g_SetTextureName)
        return false;

    const bool okA = CreateAndEnableHook(
        targetEquip,
        reinterpret_cast<void*>(&hkSetEquipBackgroundTexture),
        reinterpret_cast<void**>(&g_OrigSetEquipBackgroundTexture));

    const bool okB = CreateAndEnableHook(
        targetLoading,
        reinterpret_cast<void*>(&hkLoadingScreenOrGameOverSplash2),
        reinterpret_cast<void**>(&g_OrigLoadingScreenOrGameOverSplash2));

    const bool okC = CreateAndEnableHook(
        targetGameOver,
        reinterpret_cast<void*>(&hkGameOverSetVisible),
        reinterpret_cast<void**>(&g_OrigGameOverSetVisible));

    const bool ok = okA && okB && okC;
    Log("[Hook] UiTextureOverrides: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool Uninstall_UiTextureOverrides_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_SetEquipBackgroundTexture));
    DisableAndRemoveHook(ResolveGameAddress(ABS_LoadingScreenOrGameOverSplash2));
    DisableAndRemoveHook(ResolveGameAddress(ABS_GameOverSetVisible));

    g_OrigSetEquipBackgroundTexture = nullptr;
    g_OrigLoadingScreenOrGameOverSplash2 = nullptr;
    g_OrigGameOverSetVisible = nullptr;

    return true;
}