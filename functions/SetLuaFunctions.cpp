#include "pch.h"
#include <Windows.h>
#include <cstdint>
#include <unordered_set>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "FoxHashes.h"
#include "UiTextureOverrides.h"
#include "CautionStepNormalTimerHook.h"
#include "PlayerVoiceFpkHook.h"
#include "VIPSleepFaintHook.h"
#include "VIPHoldupHook.h"
#include <VIPRadioHook.h>
#include <State_EnterStandHoldup1.h>
#include <GetVoiceParamWithCallSign.h>
#include "LostHostageHook.h"

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
    #include "lualib.h"

}

namespace
{
    using SetLuaFunctions_t = void(__fastcall*)(lua_State* L);
    using FoxLuaRegisterLibrary_t = void(__fastcall*)(lua_State* L, const char* libName, luaL_Reg* funcs);
    using lua_tolstring_t = const char* (__fastcall*)(lua_State* L, int idx, size_t* len);
    using lua_tointeger_t = long long(__fastcall*)(lua_State* L, int idx);
    using lua_tonumber_t = lua_Number(__fastcall*)(lua_State* L, int idx);
    using lua_pushnumber_t = void(__fastcall*)(lua_State* L, lua_Number n);
    using lua_toboolean_t = int(__fastcall*)(lua_State* L, int idx);

    // Absolute address of tpp::ui::UiCommand::SetLuaFunctions.
    // Params: L (lua_State*)
    static constexpr uintptr_t ABS_SetLuaFunctions = 0x1408D78A0ull;

    // Absolute address of fox::LuaRegisterLibrary.
    // Params: L (lua_State*), libName (const char*), funcs (luaL_Reg*)
    static constexpr uintptr_t ABS_FoxLuaRegisterLibrary = 0x14006B6D0ull;

    // Absolute address of the game's lua_tolstring thunk.
    // Params: L (lua_State*), idx (int), len (size_t*)
    static constexpr uintptr_t ABS_lua_tolstring = 0x141A123C0ull;

    // Absolute address of the game's lua_tointeger thunk.
    // Params: L (lua_State*), idx (int)
    static constexpr uintptr_t ABS_lua_tointeger = 0x141A12390ull;

    // Absolute address of the game's lua_tonumber thunk.
    // Params: L (lua_State*), idx (int)
    static constexpr uintptr_t ABS_lua_tonumber = 0x141A12460ull;

    // Absolute address of the game's lua_pushnumber thunk.
    // Params: L (lua_State*), n (lua_Number)
    static constexpr uintptr_t ABS_lua_pushnumber = 0x141A11BC0ull;

    // Absolute address of the game's lua_toboolean thunk.
    // Params: L (lua_State*), idx (int)
    static constexpr uintptr_t ABS_lua_toboolean = 0x141A12330ull;

    static SetLuaFunctions_t       g_OrigSetLuaFunctions = nullptr;
    static FoxLuaRegisterLibrary_t g_FoxLuaRegisterLibrary = nullptr;
    static lua_tolstring_t         g_lua_tolstring = nullptr;
    static lua_tointeger_t         g_lua_tointeger = nullptr;
    static lua_tonumber_t          g_lua_tonumber = nullptr;
    static lua_pushnumber_t        g_lua_pushnumber = nullptr;
    static lua_toboolean_t         g_lua_toboolean = nullptr;

    static std::unordered_set<lua_State*> g_RegisteredLuaStates;
    static std::mutex g_RegisteredLuaStatesMutex;
}

// Resolves the Lua/game functions used by this bridge file.
// Params: none
static bool ResolveLuaApi()
{
    if (!g_FoxLuaRegisterLibrary)
    {
        g_FoxLuaRegisterLibrary = reinterpret_cast<FoxLuaRegisterLibrary_t>(
            ResolveGameAddress(ABS_FoxLuaRegisterLibrary));
    }

    if (!g_lua_tolstring)
    {
        g_lua_tolstring = reinterpret_cast<lua_tolstring_t>(
            ResolveGameAddress(ABS_lua_tolstring));
    }

    if (!g_lua_tointeger)
    {
        g_lua_tointeger = reinterpret_cast<lua_tointeger_t>(
            ResolveGameAddress(ABS_lua_tointeger));
    }

    if (!g_lua_tonumber)
    {
        g_lua_tonumber = reinterpret_cast<lua_tonumber_t>(
            ResolveGameAddress(ABS_lua_tonumber));
    }

    if (!g_lua_toboolean)
    {
        g_lua_toboolean = reinterpret_cast<lua_toboolean_t>(
            ResolveGameAddress(ABS_lua_toboolean));
    }

    if (!g_lua_pushnumber)
    {
        g_lua_pushnumber = reinterpret_cast<lua_pushnumber_t>(
            ResolveGameAddress(ABS_lua_pushnumber));
    }

    return g_FoxLuaRegisterLibrary &&
        g_lua_tolstring &&
        g_lua_tointeger &&
        g_lua_tonumber &&
        g_lua_toboolean &&
        g_lua_pushnumber;
}

// Registers the V_FrameWork Lua library in the given Lua state.
// Params: L (lua_State*), libName (const char*), funcs (luaL_Reg*)
static bool RegisterLuaLibrary(lua_State* L, const char* libName, luaL_Reg* funcs)
{
    if (!ResolveLuaApi() || !L || !libName || !funcs)
        return false;

    g_FoxLuaRegisterLibrary(L, libName, funcs);
    Log("[V_FrameWork] Registered library: %s (L=%p)\n", libName, L);
    return true;
}

// Returns a Lua string argument or nullptr if unavailable.
// Params: L (lua_State*), idx (int)
static const char* GetLuaString(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tolstring)
        return nullptr;

    return g_lua_tolstring(L, idx, nullptr);
}

// Returns a Lua integer argument using the game's Lua thunk.
// Params: L (lua_State*), idx (int)
static int GetLuaInt(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tointeger)
        return 0;

    return static_cast<int>(g_lua_tointeger(L, idx));
}

// Returns a Lua integer as 64-bit using the game's Lua thunk.
// Params: L (lua_State*), idx (int)
static std::uint64_t GetLuaInt64(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tointeger)
        return 0;

    return static_cast<std::uint64_t>(g_lua_tointeger(L, idx));
}

// Returns a Lua boolean argument using the game's Lua thunk.
// Params: L (lua_State*), idx (int)
static bool GetLuaBool(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_toboolean)
        return false;

    return g_lua_toboolean(L, idx) != 0;
}

// Returns a Lua number argument using the game's Lua thunk.
// Params: L (lua_State*), idx (int)
static float GetLuaNumber(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tonumber)
        return 0.0f;

    return static_cast<float>(g_lua_tonumber(L, idx));
}

// Pushes a Lua number using the game's Lua thunk.
// Params: L (lua_State*), value (float)
static void PushLuaNumber(lua_State* L, float value)
{
    if (!ResolveLuaApi() || !g_lua_pushnumber)
        return;

    g_lua_pushnumber(L, static_cast<lua_Number>(value));
}

// Returns true if the Lua state was already registered.
// Params: L (lua_State*)
static bool IsLuaStateRegistered(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    return g_RegisteredLuaStates.find(L) != g_RegisteredLuaStates.end();
}

// Tracks a Lua state after successful registration.
// Params: L (lua_State*)
static void TrackLuaState(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    g_RegisteredLuaStates.insert(L);
}

// Clears all tracked Lua states.
// Params: none
static void ClearTrackedLuaStates()
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    g_RegisteredLuaStates.clear();
}

// Sets the default equip background texture from an FTEX path.
// Params: path (string)
static int __cdecl l_SetDefaultEquipBgTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    EquipBg_SetDefaultTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Clears the default equip background texture override.
// Params: none
static int __cdecl l_ClearDefaultEquipBgTexture(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    EquipBg_ClearDefaultTexture();
    return 0;
}

// Sets the enemy-weapon equip background texture from an FTEX path.
// Params: path (string)
static int __cdecl l_SetEnemyWeaponBgTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    EquipBg_SetEnemyWeaponTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Clears the enemy-weapon equip background texture override.
// Params: none
static int __cdecl l_ClearEnemyWeaponBgTexture(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    EquipBg_ClearEnemyWeaponTexture();
    return 0;
}

// Sets a per-enemy equip background texture from an FTEX path.
// Params: equipId (number), path (string)
static int __cdecl l_SetEnemyEquipBgTexturePath(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    const char* rawPath = GetLuaString(L, 2);

    if (!rawPath || !*rawPath)
        return 0;

    EquipBg_SetEnemyEquipTexture(equipId, FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Clears a per-enemy equip background texture override.
// Params: equipId (number)
static int __cdecl l_ClearEnemyEquipBgTexture(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    EquipBg_ClearEnemyEquipTexture(equipId);
    return 0;
}

// Sets a per-equip background texture from an FTEX path.
// Params: equipId (number), path (string)
static int __cdecl l_SetEquipBgTexturePath(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    const char* rawPath = GetLuaString(L, 2);

    if (!rawPath || !*rawPath)
        return 0;

    EquipBg_SetEquipTexture(equipId, FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Clears a per-equip background texture override.
// Params: equipId (number)
static int __cdecl l_ClearEquipBgTexture(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    EquipBg_ClearEquipTexture(equipId);
    return 0;
}

// Clears all per-equip background texture overrides.
// Params: none
static int __cdecl l_ClearAllEquipBgTextures(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    EquipBg_ClearAllEquipTextures();
    return 0;
}

// Sets the loading splash main texture from an FTEX path.
// Params: path (string)
static int __cdecl l_SetLoadingSplashMainTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    LoadingSplash_SetMainTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Sets the loading splash blur texture from an FTEX path.
// Params: path (string)
static int __cdecl l_SetLoadingSplashBlurTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    LoadingSplash_SetBlurTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Clears both loading splash textures.
// Params: none
static int __cdecl l_ClearLoadingSplashTextures(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    LoadingSplash_ClearTextures();
    return 0;
}

// Sets the game over splash main texture from an FTEX path.
// Params: path (string)
static int __cdecl l_SetGameOverSplashMainTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    GameOverSplash_SetMainTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Sets the game over splash blur texture from an FTEX path.
// Params: path (string)
static int __cdecl l_SetGameOverSplashBlurTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    GameOverSplash_SetBlurTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}

// Clears both game over splash textures.
// Params: none
static int __cdecl l_ClearGameOverSplashTextures(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    GameOverSplash_ClearTextures();
    return 0;
}

// Sets the custom caution duration in seconds from Lua.
// Params: seconds (number)
static int l_SetCautionStepNormalDurationSeconds(lua_State* L)
{
    const float seconds = GetLuaNumber(L, 1);
    Set_CautionStepNormalDurationSeconds(seconds);
    return 0;
}

// Returns the current custom caution duration in seconds to Lua.
// Params: none
static int l_GetCautionStepNormalDurationSeconds(lua_State* L)
{
    PushLuaNumber(L, Get_CautionStepNormalDurationSeconds());
    return 1;
}

// Disables the custom caution duration override from Lua.
// Params: none
static int l_UnsetCautionStepNormalDurationSeconds(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    Unset_CautionStepNormalDurationSeconds();
    return 0;
}

// Returns the last observed remaining caution time in seconds to Lua.
// Params: none
static int l_GetCautionStepNormalRemainingSeconds(lua_State* L)
{
    PushLuaNumber(L, Get_CautionStepNormalRemainingSeconds());
    return 1;
}

// Sets a player-type-specific voice FPK override from Lua.
// Params: playerType (number), path (string)
static int __cdecl l_SetPlayerVoiceFpkPathForType(lua_State* L)
{
    const int playerType = GetLuaInt(L, 1);
    const char* rawPath = GetLuaString(L, 2);

    if (!rawPath || !*rawPath)
        return 0;

    Set_PlayerVoiceFpkPathForType(static_cast<std::uint32_t>(playerType), rawPath);
    return 0;
}

// Clears a player-type-specific voice FPK override from Lua.
// Params: playerType (number)
static int __cdecl l_ClearPlayerVoiceFpkPathForType(lua_State* L)
{
    const int playerType = GetLuaInt(L, 1);
    Clear_PlayerVoiceFpkPathForType(static_cast<std::uint32_t>(playerType));
    return 0;
}

// Clears all player voice FPK overrides from Lua.
// Params: none
static int __cdecl l_ClearAllPlayerVoiceFpkOverrides(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    Clear_AllPlayerVoiceFpkOverrides();
    return 0;
}

// Sets one VIP-important soldier.
// Lua params: gameObjectId, isOfficer
static int __cdecl l_SetVIPImportant(lua_State* L)
{
    const std::uint32_t gameObjectId =
        static_cast<std::uint32_t>(GetLuaInt64(L, 1));

    const bool isOfficer = GetLuaBool(L, 2);

    Add_VIPSleepFaintImportantGameObjectId(gameObjectId, isOfficer);
    Add_VIPHoldupImportantGameObjectId(gameObjectId, isOfficer);
	Add_VIPRadioImportantGameObjectId(gameObjectId, isOfficer);

    return 0;
}

// Removes one VIP-important soldier.
// Lua params: gameObjectId
static int __cdecl l_RemoveVIPImportant(lua_State* L)
{
    const std::uint32_t gameObjectId =
        static_cast<std::uint32_t>(GetLuaInt64(L, 1));

    Remove_VIPSleepFaintImportantGameObjectId(gameObjectId);
    Remove_VIPHoldupImportantGameObjectId(gameObjectId);
    Remove_VIPRadioImportantGameObjectId(gameObjectId);
    return 0;
}

// Clears all VIP-important soldiers.
// Lua params: none
static int __cdecl l_ClearVIPImportant(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);

    Clear_VIPSleepFaintImportantGameObjectIds();
    Clear_VIPHoldupImportantGameObjectIds();
    Clear_VIPRadioImportantGameObjectIds();
    return 0;
}

static int l_SetUseConcernedHoldupRecovery(lua_State* L)
{
    const bool enabled = GetLuaBool(L, 1) != 0;
    Set_UseCustomNonVipHoldupRecovery(enabled);
    return 0;
}

static int l_HoldUpReactionCowardlyReactions(lua_State* L)
{
    const bool enabled = GetLuaBool(L, 1);
    Set_HoldUpReactionCowardlyReactions(enabled);
    return 0;
}

// Marks one soldier to use the hardcoded call-sign extra override.
// Lua params: gameObjectId
static int __cdecl l_AddCallSignExtraSoldier(lua_State* L)
{
    const std::uint32_t gameObjectId =
        static_cast<std::uint32_t>(GetLuaInt64(L, 1));

    Add_CallSignExtraSoldier(gameObjectId);
    return 0;
}

// Removes one soldier from the hardcoded call-sign extra override set.
// Lua params: gameObjectId
static int __cdecl l_RemoveCallSignExtraSoldier(lua_State* L)
{
    const std::uint32_t gameObjectId =
        static_cast<std::uint32_t>(GetLuaInt64(L, 1));

    Remove_CallSignExtraSoldier(gameObjectId);
    return 0;
}

// Clears all soldiers from the hardcoded call-sign extra override set.
// Lua params: none
static int __cdecl l_ClearCallSignExtraSoldiers(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    Clear_CallSignExtraSoldiers();
    return 0;
}

// Registers one hostage to track for escape reporting.
// Lua params: gameObjectId, hostageType
static int __cdecl l_SetLostHostage(lua_State* L)
{
    const std::uint32_t gameObjectId =
        static_cast<std::uint32_t>(GetLuaInt64(L, 1));

    const int hostageType = GetLuaInt(L, 2);

    Add_LostHostage(gameObjectId, hostageType);
    return 0;
}

// Removes one tracked hostage.
// Lua params: gameObjectId
static int __cdecl l_RemoveLostHostage(lua_State* L)
{
    const std::uint32_t gameObjectId =
        static_cast<std::uint32_t>(GetLuaInt64(L, 1));

    Remove_LostHostage(gameObjectId);
    return 0;
}

// Clears all tracked hostages.
// Lua params: none
static int __cdecl l_ClearLostHostages(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    Clear_LostHostages();
    return 0;
}

static int __cdecl l_SetLostHostageFromPlayer(lua_State* L)
{
    const bool playerTookHostage = GetLuaBool(L, 1);
    SetLostHostageFromPlayer(playerTookHostage);
    return 0;
}
static luaL_Reg g_VFrameWorkLib[] =
{
    { "SetDefaultEquipBgTexturePath",           l_SetDefaultEquipBgTexturePath },
    { "ClearDefaultEquipBgTexture",             l_ClearDefaultEquipBgTexture },
    { "SetEquipBgTexturePath",                  l_SetEquipBgTexturePath },
    { "ClearEquipBgTexture",                    l_ClearEquipBgTexture },
    { "SetEnemyWeaponBgTexturePath",            l_SetEnemyWeaponBgTexturePath },
    { "ClearEnemyWeaponBgTexture",              l_ClearEnemyWeaponBgTexture },
    { "SetEnemyEquipBgTexturePath",             l_SetEnemyEquipBgTexturePath },
    { "ClearEnemyEquipBgTexture",               l_ClearEnemyEquipBgTexture },
    { "ClearAllEquipBgTextures",                l_ClearAllEquipBgTextures },
    { "SetLoadingSplashMainTexturePath",        l_SetLoadingSplashMainTexturePath },
    { "SetLoadingSplashBlurTexturePath",        l_SetLoadingSplashBlurTexturePath },
    { "ClearLoadingSplashTextures",             l_ClearLoadingSplashTextures },
    { "SetGameOverSplashMainTexturePath",       l_SetGameOverSplashMainTexturePath },
    { "SetGameOverSplashBlurTexturePath",       l_SetGameOverSplashBlurTexturePath },
    { "ClearGameOverSplashTextures",            l_ClearGameOverSplashTextures },
    { "SetCautionStepNormalDurationSeconds",    l_SetCautionStepNormalDurationSeconds },
    { "GetCautionStepNormalDurationSeconds",    l_GetCautionStepNormalDurationSeconds },
    { "UnsetCautionStepNormalDurationSeconds",  l_UnsetCautionStepNormalDurationSeconds },
    { "GetCautionStepNormalRemainingSeconds",   l_GetCautionStepNormalRemainingSeconds },
    { "SetPlayerVoiceFpkPathForType",           l_SetPlayerVoiceFpkPathForType },
    { "ClearPlayerVoiceFpkPathForType",         l_ClearPlayerVoiceFpkPathForType },
    { "ClearAllPlayerVoiceFpkOverrides",        l_ClearAllPlayerVoiceFpkOverrides },
    { "SetVIPImportant",                        l_SetVIPImportant },
    { "SetUseConcernedHoldupRecovery",          l_SetUseConcernedHoldupRecovery },
    { "RemoveVIPImportant",                     l_RemoveVIPImportant },
    { "ClearVIPImportant",                      l_ClearVIPImportant },
	{ "HoldUpReactionCowardlyReaction",         l_HoldUpReactionCowardlyReactions },
    { "AddCallSignPatrolSoldier",                l_AddCallSignExtraSoldier },
    { "RemoveCallSignPatrolSoldier",             l_RemoveCallSignExtraSoldier },
    { "ClearCallSignPatrolSoldiers",             l_ClearCallSignExtraSoldiers },
    { "SetLostHostage",                         l_SetLostHostage },
    { "RemoveLostHostage",                      l_RemoveLostHostage },
    { "ClearLostHostages",                      l_ClearLostHostages },
    { "SetLostHostageFromPlayer",              l_SetLostHostageFromPlayer },
    { nullptr, nullptr }
};

// Registers V_FrameWork into a UI Lua state only once.
// Params: L (lua_State*)
static void RegisterAllUiLuaLibraries(lua_State* L)
{
    if (!L)
        return;

    if (IsLuaStateRegistered(L))
        return;

    if (RegisterLuaLibrary(L, "V_FrameWork", g_VFrameWorkLib))
    {
        TrackLuaState(L);
    }
}

// Hooked version of SetLuaFunctions that appends V_FrameWork registration.
// Params: L (lua_State*)
static void __fastcall hkSetLuaFunctions(lua_State* L)
{
    g_OrigSetLuaFunctions(L);
    RegisterAllUiLuaLibraries(L);
}

// Exported Lua loader for require("V_FrameWork").
// Params: L (lua_State*)
extern "C" __declspec(dllexport) int __cdecl luaopen_V_FrameWork(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_FrameWork", g_VFrameWorkLib) ? 1 : 0;
}

// Installs the SetLuaFunctions hook.
// Params: none
bool Install_SetLuaFunctions_Hook()
{
    ResolveLuaApi();

    void* target = ResolveGameAddress(ABS_SetLuaFunctions);
    if (!target)
        return false;

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkSetLuaFunctions),
        reinterpret_cast<void**>(&g_OrigSetLuaFunctions));

    Log("[Hook] SetLuaFunctions: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes the SetLuaFunctions hook.
// Params: none
bool Uninstall_SetLuaFunctions_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_SetLuaFunctions));
    g_OrigSetLuaFunctions = nullptr;
    ClearTrackedLuaStates();
    return true;
}