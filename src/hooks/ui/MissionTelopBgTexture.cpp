#include "pch.h"
#include "MissionTelopBgTexture.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "AddressSet.h"
#include "FoxHashes.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "MissionTextureTable.h"


namespace
{
    using SetTextureName_t = void(__fastcall*)(void* node, std::uint64_t textureHash, std::uint64_t slotHash, int type);
    using SetBgTexture_t   = void(__fastcall*)(void* self);
    using GetUixUtility_t  = void** (__fastcall*)();
    using GetLayout_t      = void* (__fastcall*)(void* holder, std::uint32_t id);
    using GetModel_t       = void* (__fastcall*)(void* layout, std::uint32_t id);

    static SetTextureName_t g_OrigSetTextureName = nullptr;
    static SetBgTexture_t   g_OrigSetBgTexture   = nullptr;
    static GetUixUtility_t  g_GetUixUtility      = nullptr;
    static GetLayout_t      g_GetLayout          = nullptr;
    static GetModel_t       g_GetModel           = nullptr;

    static std::atomic<bool>   g_InTelop { false };
    static MissionTextureTable g_Textures;

    constexpr std::uint64_t VANILLA_TELOP_BG = 0x156846156e516e5eull;
    constexpr std::uint64_t SLOT_MASK = 0xbcae534bull;
    constexpr std::uint64_t SLOT_MAIN = 0x3bbf9889ull;

    constexpr std::uint32_t TELOP_LAYOUT_ID = 0x5997da9cu;
    constexpr std::uint32_t TELOP_MODEL_ID  = 0xa7965f2eu;

    struct TelopBuildAddrs { std::uintptr_t setBgTexture; std::uintptr_t getLayout; std::uintptr_t getModel; };

    static TelopBuildAddrs AddrsForThisBuild()
    {
        return { gAddr.TelopStartTitleEvCall_SetBgTexture, gAddr.Layout_GetLayout, gAddr.Layout_GetModel };
    }

    static std::uint64_t CurrentCustom()
    {
        return g_Textures.Resolve(MissionCodeGuard::GetCurrentMissionCode());
    }

    static void Prefetch(std::uint64_t textureHash)
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
            auto fn = reinterpret_cast<void(__fastcall*)(void*, std::uint64_t, int)>(vtbl[0x548 / sizeof(void*)]);
            if (fn) fn(util, textureHash, 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void BindCustom(void* node, std::uint64_t custom)
    {
        g_OrigSetTextureName(node, custom, SLOT_MASK, 2);
        g_OrigSetTextureName(node, custom, SLOT_MAIN, 2);
    }

    static void* GetTelopModel(void* self)
    {
        if (!self || !g_GetLayout || !g_GetModel)
            return nullptr;
        __try
        {
            void* window = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0xc0);
            if (!window) return nullptr;
            void** wvtbl = *reinterpret_cast<void***>(window);
            if (!wvtbl) return nullptr;
            auto getHolder = reinterpret_cast<void*(__fastcall*)(void*)>(wvtbl[0xb0 / sizeof(void*)]);
            if (!getHolder) return nullptr;
            void* holder = getHolder(window);
            if (!holder) return nullptr;
            void* layout = g_GetLayout(holder, TELOP_LAYOUT_ID);
            if (!layout) return nullptr;
            return g_GetModel(layout, TELOP_MODEL_ID);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static void ApplyCustomToMeshes(void* model, std::uint64_t custom)
    {
        if (!model || custom == 0 || !g_OrigSetTextureName)
            return;
        __try
        {
            void** data = *reinterpret_cast<void***>(reinterpret_cast<char*>(model) + 0x98);
            const std::int32_t count = *reinterpret_cast<std::int32_t*>(reinterpret_cast<char*>(model) + 0x90);
            if (!data || count <= 0 || count > 4096)
                return;
            for (std::int32_t i = 0; i < count; ++i)
            {
                void* node = data[i];
                if (!node) continue;
                if (*reinterpret_cast<std::uint8_t*>(reinterpret_cast<char*>(node) + 0x72) != 2)
                    continue;

                BindCustom(node, custom);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

#ifdef _DEBUG
    static std::atomic<int> g_DdTraceBudget { 80 };

    static bool IsDdFamilyTexture(std::uint64_t hash)
    {
        return hash == 0x156846156e516e5eull || hash == 0x156848f067a209acull
            || hash == 0x156943e30b3ba648ull || hash == 0x156be0c43f60cc54ull;
    }

    static void TraceDdFamilyBind(void* node, std::uint64_t textureHash, std::uint64_t slotHash, int type)
    {
        if (!IsDdFamilyTexture(textureHash))
            return;
        if (g_DdTraceBudget.fetch_sub(1, std::memory_order_relaxed) <= 0)
            return;

        void* frames[8] = {};
        const USHORT count = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
        const std::uintptr_t base = GetExeBase();

        char chain[400];
        std::size_t used = 0;
        for (USHORT i = 0; i < count && used < sizeof(chain) - 40; ++i)
        {
            const std::uintptr_t va = reinterpret_cast<std::uintptr_t>(frames[i]);
            int n = 0;
            if (base && va >= base && va < base + 0xA200000ull)
                n = snprintf(chain + used, sizeof(chain) - used, " game+0x%llX",
                             static_cast<unsigned long long>(va - base));
            else
                n = snprintf(chain + used, sizeof(chain) - used, " ext:%p", frames[i]);
            if (n <= 0) break;
            used += static_cast<std::size_t>(n);
        }
        chain[used] = 0;

        LogDebug("[UiTexTrace] DD-family bind hash=%016llX slot=%08llX pool=%d node=%p mission=%u callers:%s\n",
                 static_cast<unsigned long long>(textureHash),
                 static_cast<unsigned long long>(slotHash),
                 type, node,
                 static_cast<unsigned>(MissionCodeGuard::GetCurrentMissionCode()),
                 chain);
    }
#endif

    static void __fastcall hk_SetTextureName(void* node, std::uint64_t textureHash, std::uint64_t slotHash, int type)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigSetTextureName, node, textureHash, slotHash, type);

#ifdef _DEBUG
        TraceDdFamilyBind(node, textureHash, slotHash, type);
#endif

        std::uint64_t mainTex = 0;
        if (textureHash == VANILLA_TELOP_BG && g_InTelop.load(std::memory_order_relaxed))
        {
            const std::uint64_t custom = CurrentCustom();
            if (custom != 0)
            {
                Prefetch(custom);
                textureHash = custom;
                mainTex = custom;
            }
        }

        if (g_OrigSetTextureName)
            g_OrigSetTextureName(node, textureHash, slotHash, type);

        if (mainTex != 0 && g_OrigSetTextureName)
        {
            g_OrigSetTextureName(node, mainTex, SLOT_MAIN, 2);
            if (slotHash != SLOT_MASK)
                g_OrigSetTextureName(node, mainTex, SLOT_MASK, 2);
        }
    }

    static void __fastcall hk_SetBgTexture(void* self)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigSetBgTexture, self);

        g_InTelop.store(true, std::memory_order_relaxed);
        if (g_OrigSetBgTexture)
            g_OrigSetBgTexture(self);
        g_InTelop.store(false, std::memory_order_relaxed);

        if (self)
        {
            const std::uint64_t custom = CurrentCustom();
            if (custom != 0)
            {
                Prefetch(custom);
                ApplyCustomToMeshes(GetTelopModel(self), custom);
                LogDebug("[MissionTelopBg] mission %u -> %016llX\n",
                         static_cast<unsigned>(MissionCodeGuard::GetCurrentMissionCode()),
                         static_cast<unsigned long long>(custom));
            }
        }
    }
}

void Set_MissionTelopSplashTexturePath(const char* path, std::uint32_t missionCode)
{
    if (!path || !path[0])
        return;

    const std::uint64_t h = FoxHashes::PathCode64Ext(path);
    g_Textures.Set(missionCode, h);
    Prefetch(h);
    LogDebug("[MissionTelopBg] set texture %016llX for mission %u (%s)\n",
             static_cast<unsigned long long>(h), missionCode, path);
}

void Unset_MissionTelopSplashTexturePath(std::uint32_t missionCode)
{
    g_Textures.Clear(missionCode);
}

bool Install_MissionTelopBgTexture_Hook()
{
    const TelopBuildAddrs a = AddrsForThisBuild();
    if (a.setBgTexture == 0)
        return true;

    if (!gAddr.SetTextureName)
    {
        Log("[MissionTelopBg] ERROR: SetTextureName address missing -- telop bg override disabled\n");
        return false;
    }

    void* texTarget = ResolveGameAddress(gAddr.SetTextureName);
    const bool texOk = texTarget &&
        CreateAndEnableHook(texTarget, reinterpret_cast<void*>(&hk_SetTextureName), reinterpret_cast<void**>(&g_OrigSetTextureName));

    void* bgTarget = ResolveGameAddress(a.setBgTexture);
    const bool bgOk = bgTarget &&
        CreateAndEnableHook(bgTarget, reinterpret_cast<void*>(&hk_SetBgTexture), reinterpret_cast<void**>(&g_OrigSetBgTexture));

    g_GetLayout = reinterpret_cast<GetLayout_t>(ResolveGameAddress(a.getLayout));
    g_GetModel  = reinterpret_cast<GetModel_t>(ResolveGameAddress(a.getModel));

    if (!texOk || !bgOk || !g_GetLayout || !g_GetModel)
        Log("[MissionTelopBg] ERROR: hook/resolve failed (tex=%d bg=%d layout=%d model=%d) -- telop bg override disabled\n",
            texOk ? 1 : 0, bgOk ? 1 : 0, g_GetLayout ? 1 : 0, g_GetModel ? 1 : 0);

    return texOk && bgOk;
}

bool Uninstall_MissionTelopBgTexture_Hook()
{
    const TelopBuildAddrs a = AddrsForThisBuild();
    if (a.setBgTexture == 0)
        return true;

    if (gAddr.SetTextureName)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.SetTextureName));
    DisableAndRemoveHook(ResolveGameAddress(a.setBgTexture));

    g_OrigSetTextureName = nullptr;
    g_OrigSetBgTexture   = nullptr;
    g_Textures.Clear(0);
    g_InTelop.store(false, std::memory_order_relaxed);
    return true;
}
