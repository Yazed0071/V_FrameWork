#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "FoxHashes.h"
#include "PlayerVoiceFpkHook.h"

namespace
{
    using LoadPlayerVoiceFpk_t = void* (__fastcall*)(void* fileSlotPath, std::uint32_t playerType, std::uint32_t playerFaceId);
    using FoxPath_Path_t = void* (__fastcall*)(void* outPath, std::uint64_t pathCode64Ext);

    // Absolute address of player::voice::LoadPlayerVoiceFpk.
    // Params: fileSlotPath (void*), playerType (uint32_t), playerFaceId (uint32_t)
    static constexpr std::uintptr_t ABS_LoadPlayerVoiceFpk = 0x146867240ull;

    // Absolute address of fox::Path::Path(Path*, PathCode64Ext).
    // Params: outPath (void*), pathCode64Ext (uint64_t)
    static constexpr std::uintptr_t ABS_FoxPath_Path = 0x1400855B0ull;

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

// Resolves the fox::Path::Path helper used to write the selected path into the output object.
// Params: none
static bool ResolvePlayerVoiceApi()
{
    if (!g_FoxPath_Path)
    {
        g_FoxPath_Path = reinterpret_cast<FoxPath_Path_t>(
            ResolveGameAddress(ABS_FoxPath_Path)
            );
    }

    return g_FoxPath_Path != nullptr;
}

// Returns true when the type should use an exact stored slot instead of fallback.
// Params: playerType (uint32_t)
static bool IsExplicitType(std::uint32_t playerType)
{
    return playerType == 1 || playerType == 2;
}

// Returns a copy of the effective override for an incoming player type.
// Exact types 1 and 2 use map entries. All others use the fallback slot.
// Params: playerType (uint32_t)
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

// Writes a selected voice pack path into the game's output Path object.
// Params: outPath (void*), pathCode64Ext (uint64_t)
static void* WritePlayerVoicePath(void* outPath, std::uint64_t pathCode64Ext)
{
    if (!ResolvePlayerVoiceApi() || !outPath || pathCode64Ext == 0)
        return outPath;

    return g_FoxPath_Path(outPath, pathCode64Ext);
}

// Hooked version of player::voice::LoadPlayerVoiceFpk.
// If an override is active for the incoming player type, it writes the custom path and skips vanilla.
// Otherwise it falls back to the original function.
// Params: fileSlotPath (void*), playerType (uint32_t), playerFaceId (uint32_t)
static void* __fastcall hkLoadPlayerVoiceFpk(void* fileSlotPath, std::uint32_t playerType, std::uint32_t playerFaceId)
{
    const VoiceOverrideSlot slot = GetEffectiveOverrideForType(playerType);
    if (!slot.enabled || slot.pathCode64Ext == 0)
    {
        return g_OrigLoadPlayerVoiceFpk(fileSlotPath, playerType, playerFaceId);
    }

    Log(
        "[PlayerVoiceFpk] Override hit: playerType=%u faceId=%u pathCode64Ext=0x%016llX\n",
        playerType,
        playerFaceId,
        static_cast<unsigned long long>(slot.pathCode64Ext)
    );

    WritePlayerVoicePath(fileSlotPath, slot.pathCode64Ext);
    return fileSlotPath;
}

// Installs the player voice FPK selector hook.
// Params: none
bool Install_PlayerVoiceFpk_Hook()
{
    ResolvePlayerVoiceApi();

    void* target = ResolveGameAddress(ABS_LoadPlayerVoiceFpk);
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

    Log("[Hook] PlayerVoiceFpk: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes the player voice FPK selector hook and clears stored overrides.
// Params: none
bool Uninstall_PlayerVoiceFpk_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_LoadPlayerVoiceFpk));
    g_OrigLoadPlayerVoiceFpk = nullptr;
    g_FoxPath_Path = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_OverrideMutex);
        g_TypeOverrides.clear();
        g_FallbackOverride = {};
    }

    Log("[Hook] PlayerVoiceFpk: removed\n");
    return true;
}

// Sets a player-type-specific voice FPK override from a raw path string.
// Supported intended values:
//   1 = male DD
//   2 = female DD
//   anything else = fallback/non-DD
// Params: playerType (uint32_t), rawPath (const char*)
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

    Log(
        "[PlayerVoiceFpk] Type override set: playerType=%u path=%s -> 0x%016llX\n",
        playerType,
        rawPath,
        static_cast<unsigned long long>(pathCode64Ext)
    );
}

// Clears a player-type-specific voice FPK override.
// Supported intended values:
//   1 = male DD
//   2 = female DD
//   anything else = fallback/non-DD
// Params: playerType (uint32_t)
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

    Log("[PlayerVoiceFpk] Type override cleared: playerType=%u\n", playerType);
}

// Clears all player voice FPK overrides.
// Params: none
void Clear_AllPlayerVoiceFpkOverrides()
{
    std::lock_guard<std::mutex> lock(g_OverrideMutex);

    g_TypeOverrides.clear();
    g_FallbackOverride = {};

    Log("[PlayerVoiceFpk] All overrides cleared\n");
}