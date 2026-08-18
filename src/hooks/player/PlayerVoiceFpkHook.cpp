#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "FoxHashes.h"
#include "PlayerVoiceFpkHook.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"
#include "OutfitRegistry.h"
#include "ShadowState.h"

namespace
{
    using LoadPlayerVoiceFpk_t = void* (__fastcall*)(void* fileSlotPath, std::uint32_t playerType, std::uint32_t playerFaceId);
    using FoxPath_Path_t = void* (__fastcall*)(void* outPath, std::uint64_t pathCode64Ext);


    static LoadPlayerVoiceFpk_t g_OrigLoadPlayerVoiceFpk = nullptr;
    static FoxPath_Path_t g_FoxPath_Path = nullptr;

    struct VoiceOverrideSlot
    {
        bool enabled = false;
        std::uint64_t pathCode64Ext = 0;
    };

    static std::unordered_map<std::uint32_t, VoiceOverrideSlot> g_TypeOverrides;
    static VoiceOverrideSlot g_FallbackOverride;
    static std::mutex g_OverrideMutex;
}


static bool ResolvePlayerVoiceApi()
{
    if (!g_FoxPath_Path)
    {
        g_FoxPath_Path = reinterpret_cast<FoxPath_Path_t>(
            ResolveGameAddress(gAddr.FoxPath_Path)
            );
    }

    return g_FoxPath_Path != nullptr;
}


static bool IsExplicitType(std::uint32_t playerType)
{
    return playerType == 1 || playerType == 2;
}


static VoiceOverrideSlot GetEffectiveOverrideForType(std::uint32_t playerType)
{
    std::lock_guard<std::mutex> lock(g_OverrideMutex);

    if (IsExplicitType(playerType))
    {
        const auto it = g_TypeOverrides.find(playerType);
        if (it != g_TypeOverrides.end())
            return it->second;

        return {};
    }

    return g_FallbackOverride;
}


static void* WritePlayerVoicePath(void* outPath, std::uint64_t pathCode64Ext)
{
    if (!ResolvePlayerVoiceApi() || !outPath || pathCode64Ext == 0)
        return outPath;

    return g_FoxPath_Path(outPath, pathCode64Ext);
}


static bool ResolveWornOutfitVoice(std::uint32_t playerType, std::uint64_t* outVoiceCode)
{
    if (!outfit::shadow::HasCurrentSlot())
        return false;

    outfit::shadow::Slot s;
    if (!outfit::shadow::Get(outfit::shadow::GetCurrentSlot(), &s))
        return false;

    const std::uint8_t pt = static_cast<std::uint8_t>(playerType & 0xFF);
    if (s.realPlayerType != pt)
        return false;

    const outfit::OutfitEntry* entry = nullptr;
    if (!outfit::TryGetOutfitByPartsType(s.realPartsType, &entry) || !entry)
        return false;
    if (!entry->IsPlayerTypeSupported(pt))
        return false;

    const std::uint8_t variant = entry->HasVariants()
        ? outfit::GetActiveVariant(entry->partsType) : 0;
    const std::uint64_t code = entry->GetVariantVoiceFpk(pt, variant);
    if (code <= outfit::kSubAssetUseVanilla)
        return false;

    if (outVoiceCode) *outVoiceCode = code;
    return true;
}

static bool ResolveVanillaExtVoice(std::uint32_t playerType, std::uint64_t* outVoiceCode)
{
    const std::uint8_t vpt = outfit::ReadLivePartsType();
    if (vpt == 0xFF || vpt >= outfit::kCustomPartsTypeStart)
        return false;

    const std::uint8_t pt = static_cast<std::uint8_t>(playerType & 0xFF);

    if (!MissionCodeGuard::ShouldBypassHooks())
    {
        const std::uint8_t v = outfit::GetActiveVariant(vpt);
        if (v != 0)
        {
            if (const outfit::VanillaSuitVariantAsset* var =
                    outfit::VanillaExtGetVariantBridged(vpt, pt, v))
            {
                if (var->voiceFpk > outfit::kSubAssetUseVanilla)
                {
                    if (outVoiceCode) *outVoiceCode = var->voiceFpk;
                    return true;
                }
            }
        }
    }

    const std::uint64_t suit = outfit::VanillaExtGetSuitVoiceFpk(
        vpt, pt, outfit::ReadLiveSelectorCode());
    if (suit <= outfit::kSubAssetUseVanilla)
        return false;
    if (outVoiceCode) *outVoiceCode = suit;
    return true;
}

static void* __fastcall hkLoadPlayerVoiceFpk(void* fileSlotPath, std::uint32_t playerType, std::uint32_t playerFaceId)
{
    std::uint64_t vextVoice = 0;
    if (ResolveVanillaExtVoice(playerType, &vextVoice))
    {
#ifdef _DEBUG
        LogDebug("[PlayerVoiceFpk] vanilla-ext voice applied (playerType=%u code=0x%016llX bypass=%d)\n",
            playerType, static_cast<unsigned long long>(vextVoice),
            MissionCodeGuard::ShouldBypassHooks() ? 1 : 0);
#endif
        WritePlayerVoicePath(fileSlotPath, vextVoice);
        return fileSlotPath;
    }

    if (MissionCodeGuard::ShouldBypassHooks())
    {
        return g_OrigLoadPlayerVoiceFpk(fileSlotPath, playerType, playerFaceId);
    }

    std::uint64_t outfitVoice = 0;
    if (ResolveWornOutfitVoice(playerType, &outfitVoice))
    {
#ifdef _DEBUG
        LogDebug("[PlayerVoiceFpk] per-outfit voice applied (playerType=%u code=0x%016llX)\n",
            playerType, static_cast<unsigned long long>(outfitVoice));
#endif
        WritePlayerVoicePath(fileSlotPath, outfitVoice);
        return fileSlotPath;
    }

    const VoiceOverrideSlot slot = GetEffectiveOverrideForType(playerType);
    if (!slot.enabled || slot.pathCode64Ext == 0)
    {
        return g_OrigLoadPlayerVoiceFpk(fileSlotPath, playerType, playerFaceId);
    }

    WritePlayerVoicePath(fileSlotPath, slot.pathCode64Ext);
    return fileSlotPath;
}


bool Install_PlayerVoiceFpk_Hook()
{
    ResolvePlayerVoiceApi();

    void* target = ResolveGameAddress(gAddr.LoadPlayerVoiceFpk);
    if (!target)
    {
        Log("[Hook] PlayerVoiceFpk: target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkLoadPlayerVoiceFpk),
        reinterpret_cast<void**>(&g_OrigLoadPlayerVoiceFpk)
    );

#ifdef _DEBUG
    Log("[Hook] PlayerVoiceFpk: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[Hook] PlayerVoiceFpk: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_PlayerVoiceFpk_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.LoadPlayerVoiceFpk));
    g_OrigLoadPlayerVoiceFpk = nullptr;
    g_FoxPath_Path = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_OverrideMutex);
        g_TypeOverrides.clear();
        g_FallbackOverride = {};
    }

#ifdef _DEBUG
    LogDebug("[Hook] PlayerVoiceFpk: removed\n");
#endif
    return true;
}


void Set_PlayerVoiceFpkPathForType(std::uint32_t playerType, const char* rawPath)
{
    if (!rawPath || !*rawPath)
        return;

    const std::uint64_t pathCode64Ext = FoxHashes::PathCode64Ext(rawPath);

    std::lock_guard<std::mutex> lock(g_OverrideMutex);

    if (IsExplicitType(playerType))
    {
        VoiceOverrideSlot& slot = g_TypeOverrides[playerType];
        slot.enabled = true;
        slot.pathCode64Ext = pathCode64Ext;
    }
    else
    {
        g_FallbackOverride.enabled = true;
        g_FallbackOverride.pathCode64Ext = pathCode64Ext;
    }

#ifdef _DEBUG
    Log(
        "[PlayerVoiceFpk] Type override set: playerType=%u path=%s -> 0x%016llX\n",
        playerType,
        rawPath,
        static_cast<unsigned long long>(pathCode64Ext)
    );
#endif
}


void Clear_PlayerVoiceFpkPathForType(std::uint32_t playerType)
{
    std::lock_guard<std::mutex> lock(g_OverrideMutex);

    if (IsExplicitType(playerType))
    {
        g_TypeOverrides.erase(playerType);
    }
    else
    {
        g_FallbackOverride = {};
    }

#ifdef _DEBUG
    LogDebug("[PlayerVoiceFpk] Type override cleared: playerType=%u\n", playerType);
#endif
}


void Clear_AllPlayerVoiceFpkOverrides()
{
    std::lock_guard<std::mutex> lock(g_OverrideMutex);

    g_TypeOverrides.clear();
    g_FallbackOverride = {};

#ifdef _DEBUG
    LogDebug("[PlayerVoiceFpk] All overrides cleared\n");
#endif
}