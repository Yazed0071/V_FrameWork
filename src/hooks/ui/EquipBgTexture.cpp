#include "pch.h"
#include "EquipBgTexture.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "FoxHashes.h"
#include "HookUtils.h"
#include "log.h"
#include "AddressSet.h"
#include "MissionCodeGuard.h"

namespace
{
    using SetWeaponPanelLogo_t = uint8_t(__fastcall*)(int equipId, void* node);
    using SetTextureName_t = void(__fastcall*)(void* modelNodeMesh, uint64_t textureHash, uint64_t slotHash, int unk);
    using GetUixUtility_t = void** (__fastcall*)();
    using GetQuarkSystemTable_t = void* (__fastcall*)();

    using TexStatusCreate_t = char(__fastcall*)(void*, void*, unsigned);

    TexStatusCreate_t     g_OrigTexStatusCreate = nullptr;

    constexpr uintptr_t   kAddr_TexStatusCreate_En154 = 0x141DBC2D0;

    bool DescriptorReadableSEH(const void* src)
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(src);
        if (v < 0x10000ull || v >= 0x7FFFFFFFFFFFull)
            return false;
        __try
        {
            volatile unsigned probe =
                *reinterpret_cast<const unsigned*>(
                    static_cast<const char*>(src) + 0x18);
            (void)probe;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    char __fastcall hkTexStatusCreate(void* out, void* src, unsigned flags)
    {
        if (src && !DescriptorReadableSEH(src))
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                Log("[UiTextureGuard] a UI material texture slot holds the "
                    "corrupt streamer descriptor %p - the bind is skipped so "
                    "fox::gr::TextureStreamerStatus does not fault on it; "
                    "that slot draws untextured instead of crashing the "
                    "render worker\n", src);
            }
            return 0;
        }
        return g_OrigTexStatusCreate(out, src, flags);
    }

    SetWeaponPanelLogo_t  g_OrigSetWeaponPanelLogo = nullptr;
    SetTextureName_t      g_SetTextureName = nullptr;
    GetUixUtility_t       g_GetUixUtility = nullptr;
    GetQuarkSystemTable_t g_GetQuarkSystemTable = nullptr;
    uint64_t              g_MaskSlot = 0;

    constexpr uint64_t MAIN_SLOT = 0x3BBF9889ull;

    constexpr uintptr_t NODE_COLOR_OFFSET = 0x50;

    constexpr uint64_t VANILLA_BG = 0x15695ED8A56AE919ull;

    struct BgTex { uint64_t hash = 0; bool colored = false; float opacity = 1.0f; };
    BgTex g_Default;
    BgTex g_EnemyDefault;
    std::unordered_map<int, BgTex> g_PerEquipTextures;
    std::unordered_map<int, BgTex> g_PerEnemyEquipTextures;
    std::unordered_map<void*, bool> g_TouchedNodes;


    uint16_t CurrentHandEquip()
    {
        if (!g_GetQuarkSystemTable)
            return 0;
        __try
        {
            char* quark = reinterpret_cast<char*>(g_GetQuarkSystemTable());
            if (!quark) return 0;
            const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(quark + 0x98);
            if (!mgr) return 0;
            const uintptr_t vars = *reinterpret_cast<uintptr_t*>(mgr + 0x10);
            if (!vars) return 0;
            return *reinterpret_cast<uint16_t*>(vars + 0x5d4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }


    bool IsBionicArm(int equipId)
    {
        if (equipId >= 0x203 && equipId <= 0x207)
            return true;
        if (equipId >= 0x367 && equipId <= 0x36C)
            return true;
        const uint16_t hand = CurrentHandEquip();
        return hand != 0 && equipId == static_cast<int>(hand);
    }


    bool Resolve()
    {
        if (!g_SetTextureName)
            g_SetTextureName = reinterpret_cast<SetTextureName_t>(ResolveGameAddress(gAddr.SetTextureName));
        if (!g_GetQuarkSystemTable && gAddr.GetQuarkSystemTable != 0)
            g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(ResolveGameAddress(gAddr.GetQuarkSystemTable));
        if (g_MaskSlot == 0)
            g_MaskSlot = static_cast<uint64_t>(FoxHashes::StrCode32("Mask_Texture"));
        return g_SetTextureName != nullptr && g_MaskSlot != 0;
    }


    void Prefetch(uint64_t textureHash)
    {
        if (textureHash == 0 || gAddr.GetUixUtilityToFeedQuarkEnvironment == 0)
            return;
        if (!g_GetUixUtility)
            g_GetUixUtility = reinterpret_cast<GetUixUtility_t>(ResolveGameAddress(gAddr.GetUixUtilityToFeedQuarkEnvironment));
        if (!g_GetUixUtility)
            return;
        __try
        {
            void** util = g_GetUixUtility();
            if (!util) return;
            void** vtbl = *reinterpret_cast<void***>(util);
            if (!vtbl) return;
            auto fn = reinterpret_cast<void(__fastcall*)(void*, uint64_t, int)>(vtbl[0x548 / sizeof(void*)]);
            if (fn) fn(util, textureHash, 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool IsSortieEquip(int equipId)
    {
        if (!g_GetQuarkSystemTable)
            return true;
        __try
        {
            char* quark = reinterpret_cast<char*>(g_GetQuarkSystemTable());
            if (!quark) return true;
            const uintptr_t mgr = *reinterpret_cast<uintptr_t*>(quark + 0x98);
            if (!mgr) return true;
            const uintptr_t lm = *reinterpret_cast<uintptr_t*>(mgr + 0x130);
            if (!lm) return true;
            const uint8_t idx = *reinterpret_cast<uint8_t*>(lm + 0x3b4);
            char* info = reinterpret_cast<char*>(lm + 0x10 + static_cast<uintptr_t>(idx) * 0xe8);

            if (*reinterpret_cast<int*>(info + 0x18) == equipId) return true;
            if (*reinterpret_cast<int*>(info + 0x2c) == equipId) return true;
            if (*reinterpret_cast<int*>(info + 0x40) == equipId) return true;
            for (int i = 0; i < 8; ++i)
                if (*reinterpret_cast<int*>(info + 0x54 + i * 4) == equipId) return true;
            for (int i = 0; i < 8; ++i)
                if (*reinterpret_cast<int*>(info + 0x74 + i * 4) == equipId) return true;
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    }


    void SetNodeColor(void* node, float r, float g, float b, float a)
    {
        if (!node)
            return;
        __try
        {
            float* c = reinterpret_cast<float*>(reinterpret_cast<char*>(node) + NODE_COLOR_OFFSET);
            c[0] = r; c[1] = g; c[2] = b; c[3] = a;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }


    void ClearColorLayer(void* node)
    {
        const auto it = g_TouchedNodes.find(node);
        if (it == g_TouchedNodes.end())
            return;
        if (it->second)
        {
            Prefetch(VANILLA_BG);
            g_SetTextureName(node, VANILLA_BG, MAIN_SLOT, 2);
        }
        SetNodeColor(node, 1.0f, 1.0f, 1.0f, 1.0f);
        g_TouchedNodes.erase(it);
    }


    void Apply(void* node, const BgTex& t)
    {
        Prefetch(t.hash);
        g_SetTextureName(node, t.hash, g_MaskSlot, 2);
        if (t.colored)
        {
            g_SetTextureName(node, t.hash, MAIN_SLOT, 2);
        }
        else
        {
            ClearColorLayer(node);
        }
        if (t.colored || t.opacity != 1.0f)
        {
            SetNodeColor(node, 1.0f, 1.0f, 1.0f, t.opacity);
            g_TouchedNodes[node] = t.colored;
        }
    }


    uint8_t EquipBgApply(int equipId, void* node, SetWeaponPanelLogo_t orig)
    {
        if (node && Resolve())
        {
            if (equipId == 0)
            {
                const auto it = g_PerEquipTextures.find(0);
                if (it != g_PerEquipTextures.end())
                {
                    Apply(node, it->second);
                    return 1;
                }
                ClearColorLayer(node);
                return orig(equipId, node);
            }

            if (IsBionicArm(equipId))
            {
                const auto it = g_PerEquipTextures.find(equipId);
                if (it != g_PerEquipTextures.end())
                {
                    Apply(node, it->second);
                    return 1;
                }
                ClearColorLayer(node);
                return orig(equipId, node);
            }

            const bool sortie = IsSortieEquip(equipId);

            const BgTex* t = nullptr;
            if (sortie)
            {
                const auto it = g_PerEquipTextures.find(equipId);
                if (it != g_PerEquipTextures.end())
                    t = &it->second;
                else if (g_Default.hash != 0)
                    t = &g_Default;
            }
            else
            {
                const auto it = g_PerEnemyEquipTextures.find(equipId);
                if (it != g_PerEnemyEquipTextures.end())
                    t = &it->second;
                else if (g_EnemyDefault.hash != 0)
                    t = &g_EnemyDefault;
            }

            if (t != nullptr)
            {
                Apply(node, *t);
                return 1;
            }

            ClearColorLayer(node);
        }
        return orig(equipId, node);
    }


    uint8_t __fastcall hkSetWeaponPanelLogo(int equipId, void* node)
    {
        MISSION_GUARD_ORIGINAL_RET(g_OrigSetWeaponPanelLogo, equipId, node);
        return EquipBgApply(equipId, node, g_OrigSetWeaponPanelLogo);
    }
}


void EquipBg_SetDefaultTexture(uint64_t textureHash, bool colored, float opacity)
{
    g_Default = { textureHash, colored, opacity };
    Prefetch(textureHash);
}

void EquipBg_ClearDefaultTexture()
{
    g_Default = {};
}

void EquipBg_SetEquipTexture(int equipId, uint64_t textureHash, bool colored, float opacity)
{
    g_PerEquipTextures[equipId] = { textureHash, colored, opacity };
    Prefetch(textureHash);
}

void EquipBg_ClearEquipTexture(int equipId)
{
    g_PerEquipTextures.erase(equipId);
}

void EquipBg_SetEnemyWeaponTexture(uint64_t textureHash, bool colored, float opacity)
{
    g_EnemyDefault = { textureHash, colored, opacity };
    Prefetch(textureHash);
}

void EquipBg_ClearEnemyWeaponTexture()
{
    g_EnemyDefault = {};
}

void EquipBg_SetEnemyEquipTexture(int equipId, uint64_t textureHash, bool colored, float opacity)
{
    g_PerEnemyEquipTextures[equipId] = { textureHash, colored, opacity };
    Prefetch(textureHash);
}

void EquipBg_ClearEnemyEquipTexture(int equipId)
{
    g_PerEnemyEquipTextures.erase(equipId);
}

void EquipBg_ClearAllEquipTextures()
{
    g_PerEquipTextures.clear();
    g_PerEnemyEquipTextures.clear();
}


bool Install_EquipBgTexture_Hook()
{
    void* target = ResolveGameAddress(gAddr.SetEquipBackgroundTexture);
    if (!target)
        return false;

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkSetWeaponPanelLogo),
        reinterpret_cast<void**>(&g_OrigSetWeaponPanelLogo));

    Resolve();

    if (::AddressSetRuntime::IsEn154Family(gGameBuild))
    {
        void* guard = ResolveGameAddress(kAddr_TexStatusCreate_En154);
        if (guard && !CreateAndEnableHook(
                guard,
                reinterpret_cast<void*>(&hkTexStatusCreate),
                reinterpret_cast<void**>(&g_OrigTexStatusCreate)))
            Log("[UiTextureGuard] install FAILED - a corrupt UI texture "
                "descriptor will fault the render worker inside "
                "fox::gr::TextureStreamerStatus instead of being skipped\n");
    }

#ifdef _DEBUG
    Log("[Hook] EquipBgTexture: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[Hook] EquipBgTexture: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_EquipBgTexture_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.SetEquipBackgroundTexture));

    if (g_OrigTexStatusCreate)
    {
        DisableAndRemoveHook(ResolveGameAddress(kAddr_TexStatusCreate_En154));
        g_OrigTexStatusCreate = nullptr;
    }

    g_OrigSetWeaponPanelLogo = nullptr;
    return true;
}
