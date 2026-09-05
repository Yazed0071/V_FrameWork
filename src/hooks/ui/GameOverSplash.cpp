#include "pch.h"
#include "GameOverSplash.h"

#include <cstdint>

#include "HookUtils.h"
#include "log.h"
#include "AddressSet.h"
#include "MissionCodeGuard.h"
#include "MissionTextureTable.h"

namespace
{
    using GameOverSetVisible_t = void(__fastcall*)(uint64_t* layout, char visible);
    using SetTextureName_t = void(__fastcall*)(void* modelNodeMesh, uint64_t textureHash, uint64_t slotHash, int unk);

    constexpr uint64_t SLOT_MAIN_TEXTURE = 0x3bbf9889ull;
    constexpr uint64_t SLOT_BLUR_LAYER   = 0x8d982b8eull;

    GameOverSetVisible_t g_OrigGameOverSetVisible = nullptr;
    SetTextureName_t     g_SetTextureName = nullptr;

    MissionTextureTable g_MainTextures;
    MissionTextureTable g_BlurTextures;


    bool Resolve()
    {
        if (!g_SetTextureName)
            g_SetTextureName = reinterpret_cast<SetTextureName_t>(ResolveGameAddress(gAddr.SetTextureName));
        return g_SetTextureName != nullptr;
    }


    void __fastcall hkGameOverSetVisible(uint64_t* layout, char visible)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
        {
            g_OrigGameOverSetVisible(layout, visible);
            return;
        }

        g_OrigGameOverSetVisible(layout, visible);

        if (!visible || !layout || !Resolve())
            return;

        const uint32_t mission = MissionCodeGuard::GetCurrentMissionCode();
        const uint64_t mainTex = g_MainTextures.Resolve(mission);
        const uint64_t blurTex = g_BlurTextures.Resolve(mission);
        if (mainTex == 0 && blurTex == 0)
            return;

        void* const node8 = reinterpret_cast<void*>(layout[8]);
        void* const node9 = reinterpret_cast<void*>(layout[9]);
        void* const node10 = reinterpret_cast<void*>(layout[10]);
        void* const node11 = reinterpret_cast<void*>(layout[11]);

        if (mainTex != 0)
        {
            if (node8) g_SetTextureName(node8, mainTex, SLOT_MAIN_TEXTURE, 2);
            if (node9) g_SetTextureName(node9, mainTex, SLOT_MAIN_TEXTURE, 2);
        }

        if (blurTex != 0)
        {
            if (node8)  g_SetTextureName(node8,  blurTex, SLOT_BLUR_LAYER, 2);
            if (node9)  g_SetTextureName(node9,  blurTex, SLOT_BLUR_LAYER, 2);
            if (node10) g_SetTextureName(node10, blurTex, SLOT_MAIN_TEXTURE, 2);
            if (node11) g_SetTextureName(node11, blurTex, SLOT_MAIN_TEXTURE, 2);
        }

        LogDebug("[GameOverSplash] mission %u -> main %016llX blur %016llX\n",
                 mission,
                 static_cast<unsigned long long>(mainTex),
                 static_cast<unsigned long long>(blurTex));
    }
}


void GameOverSplash_SetMainTexture(uint64_t textureHash, uint32_t missionCode)
{
    g_MainTextures.Set(missionCode, textureHash);
    LogDebug("[GameOverSplash] set main texture %016llX for mission %u\n",
             static_cast<unsigned long long>(textureHash), missionCode);
}

void GameOverSplash_ClearMainTexture(uint32_t missionCode)
{
    g_MainTextures.Clear(missionCode);
}

void GameOverSplash_SetBlurTexture(uint64_t textureHash, uint32_t missionCode)
{
    g_BlurTextures.Set(missionCode, textureHash);
    LogDebug("[GameOverSplash] set blur texture %016llX for mission %u\n",
             static_cast<unsigned long long>(textureHash), missionCode);
}

void GameOverSplash_ClearBlurTexture(uint32_t missionCode)
{
    g_BlurTextures.Clear(missionCode);
}

void GameOverSplash_ClearTextures(uint32_t missionCode)
{
    g_MainTextures.Clear(missionCode);
    g_BlurTextures.Clear(missionCode);
}


bool Install_GameOverSplash_Hook()
{
    void* target = ResolveGameAddress(gAddr.GameOverSetVisible);
    if (!target)
        return false;

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGameOverSetVisible),
        reinterpret_cast<void**>(&g_OrigGameOverSetVisible));

#ifdef _DEBUG
    Log("[Hook] GameOverSplash: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[Hook] GameOverSplash: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_GameOverSplash_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.GameOverSetVisible));
    g_OrigGameOverSetVisible = nullptr;
    return true;
}
