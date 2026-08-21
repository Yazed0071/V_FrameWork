#include "pch.h"
#include "CqcActionPluginImpl_StateHoldMove.h"
#include "PlayerSpaceQuery.h"
extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
    #include "lualib.h"
}

#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <vector>

#include "HookUtils.h"
#include "log.h"
#include "FoxHashes.h"
#include "UiUtility_GetWeaponItemNameLangId.h"
#include "EquipBgTexture.h"
#include "MissionTelopBgTexture.h"
#include "LoadingSplash.h"
#include "GameOverSplash.h"
#include "VIPSleepFaintHook.h"
#include "VIPHoldupHook.h"
#include "VIPRadioHook.h"
#include "State_EnterStandHoldup1.h"
#include "GetVoiceParamWithCallSign.h"
#include "LostHostageHook.h"
#include "../core/V_FrameWorkState.h"
#include "../core/LuaBroadcaster.h"
#include "StepRadioDiscovery.h"
#include "ActionCoreImpl_UpdateOptCamo.h"
#include "MbDvcCassetteTapeCallbackImpl_PlayOrPauseSelectedTrack.h"
#include "CassetteWalkmanEvents.h"
#include "SoundMusicPlayer_SetupMusicInfos.h"
#include "CustomTapeOwnership.h"
#include "GetTapeTrackDirectPlayId.h"
#include "SoundSystemImpl_BeginSoundSystem.h"
#include "CustomRadioCassette.h"
#include "TppPickableRuntime.h"

#include "AddressSet.h"
#include "LuaApi.h"
#include "OutfitLuaBindings.h"
#include "V_TppEquipLib.h"
#include "utility_GetIconFtexPath.h"
#include "PlayerVoiceFpkHook.h"
#include "SoldierVoiceTypeQuery.h"
#include "SoldierObjectRtpc.h"
#include "../hooks/sound/VoicePitchOverride.h"
#include "../hooks/sahelan/RealizedSahelanFovaHook.h"
#include "../hooks/sahelan/SetEyeLampColorHook.h"
#include "../hooks/sahelan/PhaseSneakAiImpl_PreUpdate.h"
#include "../hooks/sound/GameOverMusic.h"
#include "../hooks/sound/HeliVoice.h"
#include "../hooks/heli/HeliSoundController.h"
#include "../hooks/menupopup/MbDvcCustomPopupHook.h"
#include "../hooks/ui/EnemyLangIdOverride.h"
#include "../hooks/ui/MissionEmergencyHook.h"
#include "../hooks/ui/ShowMissionIcon.h"
#include "../hooks/ui/BarrierEffectSpawn.h"
#include "../hooks/heli/FieldTaxiMenu.h"
#include "../hooks/player/TimeCigaretteUiHook.h"
#include "../hooks/equip/EquipDevelop_AddToEquipDevelopTable.h"

#include "V_TppUiCommandLib.h"
#include "V_TppGameObjectConstants.h"
#include "UiCommandFunctions.h"
#include "V_TppSoundDaemonLib.h"
#include "SoundDaemonFunctions.h"
#include "V_TppCassetteLib.h"
#include "CassetteFunctions.h"
#include "V_TppSahelanLib.h"
#include "SahelanFunctions.h"
#include "V_PlayerLib.h"
#include "PlayerFunctions.h"
#include "V_FoxLib.h"
#include "V_PickableLib.h"
#include "V_HelicopterLib.h"
#include "V_TppMotherBaseManagementLib.h"
#include "FoxFunctions.h"


namespace
{
    using SetLuaFunctions_t = void(__fastcall*)(lua_State* L);

    static SetLuaFunctions_t       g_OrigSetLuaFunctions = nullptr;

    static std::unordered_set<lua_State*> g_RegisteredLuaStates;
    static std::mutex g_RegisteredLuaStatesMutex;
    static bool g_SetLuaFunctionsHookInstalled = false;
    static DWORD g_LuaOwnerThreadId = 0;

    using lua_pcall_native_t = int(__fastcall*)(lua_State*, int, int, int);
    static lua_pcall_native_t g_OrigLuaPcallPump = nullptr;
    static bool g_LuaPcallPumpInstalled = false;
}


static void SetLuaTop(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_settop)
        return;

    g_lua_settop(L, idx);
}


static bool IsLuaStateRegistered(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    return g_RegisteredLuaStates.find(L) != g_RegisteredLuaStates.end();
}


static void TrackLuaState(lua_State* L)
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    g_RegisteredLuaStates.insert(L);
    g_LuaOwnerThreadId = GetCurrentThreadId();
}

lua_State* V_FrameWork_AnyLuaState()
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    if (g_RegisteredLuaStates.empty()) return nullptr;
    return *g_RegisteredLuaStates.begin();
}

unsigned long V_FrameWork_LuaOwnerThreadId()
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    return g_LuaOwnerThreadId;
}


static void ClearTrackedLuaStates()
{
    std::lock_guard<std::mutex> lock(g_RegisteredLuaStatesMutex);
    g_RegisteredLuaStates.clear();
}


static bool LuaReadRequiredStringField(lua_State* L, const char* fieldName, std::string& outValue)
{
    outValue.clear();

    LuaGetField(L, -1, fieldName);
    const bool ok = LuaIsString(L, -1);

    if (ok)
    {
        const char* value = GetLuaString(L, -1);
        if (value && value[0] != '\0')
        {
            outValue = value;
        }
        else
        {
            LuaPop(L, 1);
            return false;
        }
    }

    LuaPop(L, 1);
    return ok && !outValue.empty();
}

static double LuaToNumber(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_tonumber)
        return 0.0;

    return static_cast<double>(g_lua_tonumber(L, idx));
}


static std::int32_t LuaReadOptionalIntField(lua_State* L, const char* fieldName, std::int32_t defaultValue)
{
    LuaGetField(L, -1, fieldName);

    std::int32_t value = defaultValue;
    if (LuaIsNumber(L, -1))
    {
        value = static_cast<std::int32_t>(GetLuaInt(L, -1));
    }

    LuaPop(L, 1);
    return value;
}


static std::uint32_t LuaReadOptionalUIntField(lua_State* L, const char* fieldName, std::uint32_t defaultValue)
{
    LuaGetField(L, -1, fieldName);

    std::uint32_t value = defaultValue;
    if (LuaIsNumber(L, -1))
    {
        value = static_cast<std::uint32_t>(GetLuaInt(L, -1));
    }

    LuaPop(L, 1);
    return value;
}

static bool TryReadTableIntField(lua_State* L, int tableIndex, const char* fieldName, int& outValue)
{
    outValue = 0;
    LuaGetField(L, tableIndex, const_cast<char*>(fieldName));

    const bool ok = (LuaType(L, -1) == 3);
    if (ok)
        outValue = GetLuaInt(L, -1);

    SetLuaTop(L, -2);
    return ok;
}

static bool TryReadTableStringField(lua_State* L, int tableIndex, const char* fieldName, const char*& outValue)
{
    outValue = nullptr;
    LuaGetField(L, tableIndex, const_cast<char*>(fieldName));

    const bool ok = (LuaType(L, -1) == 4);
    if (ok)
        outValue = GetLuaString(L, -1);

    SetLuaTop(L, -2);
    return ok;
}

static bool TryReadTableBoolField(lua_State* L, int tableIndex, const char* fieldName, bool defaultValue)
{
    LuaGetField(L, tableIndex, const_cast<char*>(fieldName));

    const int type = LuaType(L, -1);
    bool result = defaultValue;

    if (type != 0)
        result = GetLuaBool(L, -1) != 0;

    SetLuaTop(L, -2);
    return result;
}


static bool TryReadTableSubAssetField(
    lua_State* L, int tableIndex, const char* fieldName,
    const char*& outPath, bool& outVanilla, bool defaultVanilla)
{
    outPath = nullptr;
    outVanilla = defaultVanilla;

    LuaGetField(L, tableIndex, const_cast<char*>(fieldName));
    const int type = LuaType(L, -1);

    if (type == 4)
    {
        outPath = GetLuaString(L, -1);
        outVanilla = false;
    }
    else if (type == 1)
    {
        outVanilla = GetLuaBool(L, -1);
    }


    SetLuaTop(L, -2);
    return true;
}

static void LuaPushString(lua_State* L, const char* value)
{
    if (!ResolveLuaApi() || !g_lua_pushstring || !value)
        return;

    g_lua_pushstring(L, const_cast<char*>(value));
}

static void LuaCreateTable(lua_State* L, int narr, int nrec)
{
    if (!ResolveLuaApi() || !g_lua_createtable)
        return;

    g_lua_createtable(L, narr, nrec);
}

static void LuaGetTable(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_gettable)
        return;

    g_lua_gettable(L, idx);
}

static void LuaRawSet(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_rawset)
        return;

    g_lua_rawset(L, idx);
}

static void LuaSetTable(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_settable)
        return;

    g_lua_settable(L, idx);
}

static void LuaPushNil(lua_State* L)
{
    if (!ResolveLuaApi() || !g_lua_pushnil)
        return;

    g_lua_pushnil(L);
}

static int LuaNext(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_next)
        return 0;

    return g_lua_next(L, idx);
}

static void LuaPushValue(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_pushvalue)
        return;

    g_lua_pushvalue(L, idx);
}


int __cdecl l_SetDefaultEquipBgTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    const int coloredType = LuaType(L, 2);
    const bool colored = (coloredType != LUA_TNONE && coloredType != LUA_TNIL) && GetLuaBool(L, 2) != 0;
    const int opacityType = LuaType(L, 3);
    const float opacity = (opacityType != LUA_TNONE && opacityType != LUA_TNIL) ? GetLuaNumber(L, 3) : 1.0f;
    EquipBg_SetDefaultTexture(FoxHashes::PathCode64Ext(rawPath), colored, opacity);
    return 0;
}


int __cdecl l_ClearDefaultEquipBgTexture(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    EquipBg_ClearDefaultTexture();
    return 0;
}


int __cdecl l_SetEquipBgTexturePath(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    const char* rawPath = GetLuaString(L, 2);

    if (!rawPath || !*rawPath)
        return 0;

    const int coloredType = LuaType(L, 3);
    const bool colored = (coloredType != LUA_TNONE && coloredType != LUA_TNIL) && GetLuaBool(L, 3) != 0;
    const int opacityType = LuaType(L, 4);
    const float opacity = (opacityType != LUA_TNONE && opacityType != LUA_TNIL) ? GetLuaNumber(L, 4) : 1.0f;
    EquipBg_SetEquipTexture(equipId, FoxHashes::PathCode64Ext(rawPath), colored, opacity);
    return 0;
}


int __cdecl l_ClearEquipBgTexture(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    EquipBg_ClearEquipTexture(equipId);
    return 0;
}


int __cdecl l_SetEnemyWeaponBgTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    const int coloredType = LuaType(L, 2);
    const bool colored = (coloredType != LUA_TNONE && coloredType != LUA_TNIL) && GetLuaBool(L, 2) != 0;
    const int opacityType = LuaType(L, 3);
    const float opacity = (opacityType != LUA_TNONE && opacityType != LUA_TNIL) ? GetLuaNumber(L, 3) : 1.0f;
    EquipBg_SetEnemyWeaponTexture(FoxHashes::PathCode64Ext(rawPath), colored, opacity);
    return 0;
}


int __cdecl l_ClearEnemyWeaponBgTexture(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    EquipBg_ClearEnemyWeaponTexture();
    return 0;
}


int __cdecl l_SetEnemyEquipBgTexturePath(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    const char* rawPath = GetLuaString(L, 2);

    if (!rawPath || !*rawPath)
        return 0;

    const int coloredType = LuaType(L, 3);
    const bool colored = (coloredType != LUA_TNONE && coloredType != LUA_TNIL) && GetLuaBool(L, 3) != 0;
    const int opacityType = LuaType(L, 4);
    const float opacity = (opacityType != LUA_TNONE && opacityType != LUA_TNIL) ? GetLuaNumber(L, 4) : 1.0f;
    EquipBg_SetEnemyEquipTexture(equipId, FoxHashes::PathCode64Ext(rawPath), colored, opacity);
    return 0;
}


int __cdecl l_ClearEnemyEquipBgTexture(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    EquipBg_ClearEnemyEquipTexture(equipId);
    return 0;
}


int __cdecl l_ClearAllEquipBgTextures(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    EquipBg_ClearAllEquipTextures();
    return 0;
}


int __cdecl l_SetLoadingSplashMainTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    LoadingSplash_SetMainTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}


int __cdecl l_SetLoadingSplashBlurTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    LoadingSplash_SetBlurTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}


int __cdecl l_ClearLoadingSplashTextures(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    LoadingSplash_ClearTextures();
    return 0;
}


int __cdecl l_SetGameOverSplashMainTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    GameOverSplash_SetMainTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}


int __cdecl l_SetGameOverSplashBlurTexturePath(lua_State* L)
{
    const char* rawPath = GetLuaString(L, 1);
    if (!rawPath || !*rawPath)
        return 0;

    GameOverSplash_SetBlurTexture(FoxHashes::PathCode64Ext(rawPath));
    return 0;
}


int __cdecl l_ClearGameOverSplashTextures(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    GameOverSplash_ClearTextures();
    return 0;
}


int __cdecl l_SetPlayerVoiceFpkPathForType(lua_State* L)
{
    const int playerType = GetLuaInt(L, 1);
    const char* rawPath = GetLuaString(L, 2);

    if (!rawPath || !*rawPath)
        return 0;

    Set_PlayerVoiceFpkPathForType(static_cast<std::uint32_t>(playerType), rawPath);
    return 0;
}


int __cdecl l_ClearPlayerVoiceFpkPathForType(lua_State* L)
{
    const int playerType = GetLuaInt(L, 1);
    Clear_PlayerVoiceFpkPathForType(static_cast<std::uint32_t>(playerType));
    return 0;
}


int __cdecl l_ClearAllPlayerVoiceFpkOverrides(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    Clear_AllPlayerVoiceFpkOverrides();
    return 0;
}


int __cdecl l_RequestToSetTargetCqcStance(lua_State* L)
{
    const int stance = GetLuaInt(L, 1);
    if (stance != kCqcStanceStanding && stance != kCqcStanceCrouching)
    {
        Log("[TargetCqcStance] ERROR: V_Player.RequestToSetTargetCqcStance was given stance %d - "
            "expected 0 (standing) or 1 (crouching); the CQC hold keeps its current stance.\n",
            stance);
        return 0;
    }
    ::Set_TargetCqcStance(stance);
    if (::IsInCqcHold())
        ::Request_PlayerTargetStance(L, stance);
    return 0;
}


static bool ReadPlayerTransformFromVars(lua_State* L, float* posX, float* posY, float* posZ,
                                        float* rotY)
{
    if (!ResolveLuaApi() || L == nullptr)
        return false;

    const int top = GetLuaTop(L);
    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("vars"));

    if (LuaType(L, -1) != LUA_TTABLE)
    {
        LuaPop(L, GetLuaTop(L) - top);
        return false;
    }

    const char* const keys[4]  = { "playerPosX", "playerPosY", "playerPosZ", "playerRotY" };
    float* const      slots[4] = { posX, posY, posZ, rotY };

    bool ok = true;
    for (int i = 0; i < 4; ++i)
    {
        g_lua_getfield(L, -1, const_cast<char*>(keys[i]));
        if (LuaIsNumber(L, -1))
            *slots[i] = GetLuaNumber(L, -1);
        else
            ok = false;
        LuaPop(L, 1);
    }

    LuaPop(L, GetLuaTop(L) - top);
    return ok;
}

int __cdecl l_IsThereEnoughSpaceAroundPlayer(lua_State* L)
{
    const float minX = GetLuaNumber(L, 1);
    const float maxX = GetLuaNumber(L, 2);
    const float minZ = GetLuaNumber(L, 3);
    const float maxZ = GetLuaNumber(L, 4);

    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    float rotY = 0.0f;

    if (!ReadPlayerTransformFromVars(L, &posX, &posY, &posZ, &rotY))
    {
        static std::uint64_t s_lastTick = 0;
        const std::uint64_t  now        = GetTickCount64();
        if (s_lastTick == 0 || now - s_lastTick >= 5000)
        {
            s_lastTick = now;
            Log("[PlayerSpace] ERROR: vars.playerPosX/Y/Z or vars.playerRotY could not be read, so "
                "V_Player.IsThereEnoughSpaceAroundPlayer has no player transform to test around and "
                "answers 'not enough space'.\n");
        }
        PushLuaBool(L, false);
        return 1;
    }

    PushLuaBool(L, PlayerSpace_IsClearAround(minX, maxX, minZ, maxZ, posX, posY, posZ, rotY));
    return 1;
}


int __cdecl l_IsBarrierActive(lua_State* L)
{
    PushLuaBool(L, BarrierEffect_IsShieldDeployed());
    return 1;
}




int __cdecl l_PlayCassetteTapeByTrackId(lua_State* L)
{
    const std::uint32_t albumIndex = 0;
    const std::uint32_t trackId = static_cast<std::uint32_t>(GetLuaInt(L, 1));

    bool loopPlay = false;
    bool playAll = false;

    const int arg2Type = LuaType(L, 2);
    if (arg2Type != LUA_TNONE && arg2Type != LUA_TNIL)
    {
        loopPlay = GetLuaBool(L, 2);
    }

    const int arg3Type = LuaType(L, 3);
    if (arg3Type != LUA_TNONE && arg3Type != LUA_TNIL)
    {
        playAll = GetLuaBool(L, 3);
    }

    const bool ok = PlayCassetteByTrackId(albumIndex, trackId, loopPlay, playAll);
    PushLuaBool(L, ok);
    return 1;
}

int __cdecl l_GetTapeTrackId(lua_State* L)
{
    if (!LuaIsString(L, 1))
    {
        PushLuaNumber(L, -1.0f);
        return 1;
    }

    const char* trackName = GetLuaString(L, 1);
    const std::int32_t directPlayTrackId = ResolveTapeTrackDirectPlayId(trackName);

    PushLuaNumber(L, static_cast<float>(directPlayTrackId));
    return 1;
}

int __cdecl l_GetCassettePlayingTime(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);

    const std::uint32_t value = GetCassettePlayingTime();
    PushLuaNumber(L, static_cast<float>(value));
    return 1;
}

int __cdecl l_GetCassettePlayingTrackId(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);

    const std::uint32_t value = GetCassettePlayingTrackId();
    PushLuaNumber(L, static_cast<float>(value));
    return 1;
}

int __cdecl l_PauseCassette(lua_State* L)
{
    std::uint32_t fadeMs = 0;

    const int arg1Type = LuaType(L, 1);
    if (arg1Type != LUA_TNONE && arg1Type != LUA_TNIL)
    {
        fadeMs = static_cast<std::uint32_t>(GetLuaInt(L, 1));
    }

    const std::int32_t errorCode = PauseCassette(fadeMs);
    PushLuaNumber(L, static_cast<float>(errorCode));
    return 1;
}

int __cdecl l_ResumeCassette(lua_State* L)
{
    std::uint32_t fadeMs = 0;

    const int arg1Type = LuaType(L, 1);
    if (arg1Type != LUA_TNONE && arg1Type != LUA_TNIL)
    {
        fadeMs = static_cast<std::uint32_t>(GetLuaInt(L, 1));
    }

    const std::int32_t errorCode = ResumeCassette(fadeMs);
    PushLuaNumber(L, static_cast<float>(errorCode));
    return 1;
}

int __cdecl l_StopCassette(lua_State* L)
{
    std::uint32_t fadeMs = 0;

    const int arg1Type = LuaType(L, 1);
    if (arg1Type != LUA_TNONE && arg1Type != LUA_TNIL)
    {
        fadeMs = static_cast<std::uint32_t>(GetLuaInt(L, 1));
    }

    const std::int32_t errorCode = StopCassette(fadeMs, false);
    PushLuaNumber(L, static_cast<float>(errorCode));
    return 1;
}


int __cdecl l_IsCassetteSpeakerEnabled(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);

    bool enabled = false;
    const bool ok = IsCassetteSpeakerEnabled(enabled);

    if (!ok)
    {
        PushLuaBool(L, false);
        return 1;
    }

    PushLuaBool(L, enabled);
    return 1;
}


int __cdecl l_SetCassetteSpeakerEnabled(lua_State* L)
{
    const bool enabled = GetLuaBool(L, 1);
    const bool ok = SetCassetteSpeakerEnabled(enabled);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_RegisterRadioCassette(lua_State* L)
{
    if (!LuaIsString(L, 1))
    {
        LogDebug("[RadioCassette] RegisterRadioCassette: gimmickName must be a string\n");
        PushLuaBool(L, false);
        return 1;
    }

    const char* gimmickName = GetLuaString(L, 1);
    if (!gimmickName || !*gimmickName)
    {
        PushLuaBool(L, false);
        return 1;
    }

    const char* fox2Path = LuaIsString(L, 2) ? GetLuaString(L, 2) : nullptr;

    std::uint32_t wwiseEventId = 0;
    if (LuaIsString(L, 3))
    {
        wwiseEventId = FoxHashes::FNVHash32(GetLuaString(L, 3));
    }
    else
    {
        const int arg3Type = LuaType(L, 3);
        if (arg3Type != LUA_TNONE && arg3Type != LUA_TNIL)
            wwiseEventId = static_cast<std::uint32_t>(GetLuaInt64(L, 3));
    }

    if (wwiseEventId == 0)
    {
        LogDebug("[RadioCassette] RegisterRadioCassette: wwiseEvent missing/invalid\n");
        PushLuaBool(L, false);
        return 1;
    }

    const char* fileName = LuaIsString(L, 4) ? GetLuaString(L, 4) : nullptr;
    const std::uint32_t trackNameId =
        (fileName && *fileName) ? FoxHashes::StrCode32(fileName) : 0u;
    const std::uint32_t nameHash = FoxHashes::StrCode32(gimmickName);
    const std::uint32_t fox2PathHash =
        (fox2Path && *fox2Path) ? FoxHashes::StrCode32(fox2Path) : 0u;

    const bool ok = Register_CustomRadioCassette(nameHash, fox2PathHash, wwiseEventId, trackNameId, fileName);

#ifdef _DEBUG
    Log("[RadioCassette] RegisterRadioCassette name='%s' fox2='%s' nameHash=%08X fox2Hash=%08X wwise=%08X track=%08X -> %s\n",
        gimmickName,
        fox2Path ? fox2Path : "(none)",
        static_cast<unsigned int>(nameHash),
        static_cast<unsigned int>(fox2PathHash),
        static_cast<unsigned int>(wwiseEventId),
        static_cast<unsigned int>(trackNameId),
        ok ? "OK" : "FAIL");
#endif

    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_SetEquipIconFtexPath(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    const char* rawPath = GetLuaString(L, 2);

    if (!rawPath || !*rawPath)
        return 0;

    EquipIcon_SetEquipIdIconFtexPath(equipId, FoxHashes::PathCode64Ext(rawPath));
    return 0;
}


int __cdecl l_ClearIconFtexPath(lua_State* L)
{
    const int equipId = GetLuaInt(L, 1);
    EquipIcon_ClearIconFtexPath(equipId);
    return 0;
}


int __cdecl l_ClearAllIconFtexPaths(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    EquipIcon_ClearAllIconFtexPaths();
    return 0;
}


static bool g_VFI_IsEmergencyMissionOverrideInstalled = false;


static int __cdecl l_VFI_IsEmergencyMissionOverride(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;


    if (g_lua_isnumber && g_lua_isnumber(L, 1))
    {
        const long long mc = g_lua_tointeger ? g_lua_tointeger(L, 1) : 0;
        if (mc > 0 && mc <= 0xFFFF &&
            MissionEmergency_IsEnabled(static_cast<std::uint16_t>(mc)))
        {
            g_lua_pushboolean(L, 1);
            return 1;
        }
    }


    if (!g_lua_pushvalue || !g_lua_pcall || !g_lua_gettop || !g_lua_settop || !g_lua_pushnil)
        return 0;

    const int nargs = g_lua_gettop(L);

    g_lua_pushvalue(L, LUA_UPVALUEINDEX_51_1);
    for (int i = 1; i <= nargs; ++i)
        g_lua_pushvalue(L, i);

    const int err = g_lua_pcall(L, nargs, 1, 0);
    if (err != 0)
    {
        const char* errMsg = g_lua_tolstring ? g_lua_tolstring(L, -1, nullptr) : nullptr;
        LogDebug("[MissionEmergency] IsEmergencyMission OVERRIDE: original pcall err=%d: %s\n",
            err, errMsg ? errMsg : "<no message>");
        g_lua_settop(L, nargs);
        g_lua_pushnil(L);
    }
    return 1;
}


static bool InstallIsEmergencyMissionOverride(lua_State* L)
{
    if (g_VFI_IsEmergencyMissionOverrideInstalled)
        return true;

    if (!ResolveLuaApi() || !g_lua_pushcclosure || !g_lua_pcall)
    {
        LogDebug("[MissionEmergency] InstallIsEmergencyMissionOverride: Lua FFI unavailable; skipped\n");
        return false;
    }

    const int top0 = g_lua_gettop(L);

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppMission"));
    if (g_lua_type(L, -1) != LUA_TTABLE)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] InstallIsEmergencyMissionOverride: TppMission not loaded yet; deferred\n");
        return false;
    }

    g_lua_getfield(L, -1, const_cast<char*>("IsEmergencyMission"));
    if (g_lua_type(L, -1) != LUA_TFUNCTION)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] InstallIsEmergencyMissionOverride: original function missing\n");
        return false;
    }

    g_lua_pushcclosure(L, &l_VFI_IsEmergencyMissionOverride, 1);

    g_lua_pushstring(L, const_cast<char*>("IsEmergencyMission"));
    g_lua_pushvalue(L, -2);
    g_lua_rawset(L, -4);

    g_lua_settop(L, top0);

    g_VFI_IsEmergencyMissionOverrideInstalled = true;
#ifdef _DEBUG
    LogDebug("[MissionEmergency] InstallIsEmergencyMissionOverride: installed\n");
#endif
    return true;
}


static void AppendToEmergencyMissionList(lua_State* L, int missionCode)
{
    if (!ResolveLuaApi())
        return;

    const int top0 = g_lua_gettop(L);

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppDefine"));
    if (g_lua_type(L, -1) != LUA_TTABLE) { g_lua_settop(L, top0); return; }

    g_lua_getfield(L, -1, const_cast<char*>("EMERGENCY_MISSION_LIST"));
    if (g_lua_type(L, -1) != LUA_TTABLE) { g_lua_settop(L, top0); return; }

    const int len = static_cast<int>(g_lua_objlen(L, -1));

    for (int i = 1; i <= len; ++i)
    {
        g_lua_rawgeti(L, -1, i);
        const bool isMatch =
            g_lua_isnumber(L, -1) &&
            (static_cast<int>(g_lua_tointeger(L, -1)) == missionCode);
        g_lua_settop(L, -2);
        if (isMatch)
        {
            g_lua_settop(L, top0);
            return;
        }
    }

    g_lua_pushnumber(L, static_cast<lua_Number>(len + 1));
    g_lua_pushnumber(L, static_cast<lua_Number>(missionCode));
    g_lua_rawset(L, -3);

    g_lua_settop(L, top0);
}


static void RemoveFromEmergencyMissionList(lua_State* L, int missionCode)
{
    if (!ResolveLuaApi())
        return;

    const int top0 = g_lua_gettop(L);

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppDefine"));
    if (g_lua_type(L, -1) != LUA_TTABLE) { g_lua_settop(L, top0); return; }

    g_lua_getfield(L, -1, const_cast<char*>("EMERGENCY_MISSION_LIST"));
    if (g_lua_type(L, -1) != LUA_TTABLE) { g_lua_settop(L, top0); return; }

    const int len = static_cast<int>(g_lua_objlen(L, -1));
    int foundIdx = -1;

    for (int i = 1; i <= len; ++i)
    {
        g_lua_rawgeti(L, -1, i);
        const bool isMatch =
            g_lua_isnumber(L, -1) &&
            (static_cast<int>(g_lua_tointeger(L, -1)) == missionCode);
        g_lua_settop(L, -2);
        if (isMatch)
        {
            foundIdx = i;
            break;
        }
    }

    if (foundIdx < 0)
    {
        g_lua_settop(L, top0);
        return;
    }


    for (int i = foundIdx; i < len; ++i)
    {
        g_lua_rawgeti(L, -1, i + 1);
        g_lua_pushnumber(L, static_cast<lua_Number>(i));
        g_lua_pushvalue(L, -2);
        g_lua_rawset(L, -4);
        g_lua_settop(L, -2);
    }


    g_lua_pushnumber(L, static_cast<lua_Number>(len));
    g_lua_pushnil(L);
    g_lua_rawset(L, -3);

    g_lua_settop(L, top0);
}


namespace
{
    static bool g_VFI_AcceptEmergencyMissionOverrideInstalled = false;
    static bool g_VFI_GoToEmergencyMissionOverrideInstalled = false;
}


static int __cdecl l_VFI_AcceptEmergencyMissionOverride(lua_State* L)
{
    if (!ResolveLuaApi() ||
        !g_lua_pushvalue || !g_lua_pcall || !g_lua_gettop ||
        !g_lua_isnumber || !g_lua_tointeger)
    {
        return 0;
    }

    const int       nargs       = g_lua_gettop(L);
    const long long missionCode = (nargs >= 1 && g_lua_isnumber(L, 1)) ? g_lua_tointeger(L, 1) : 0;

    g_lua_pushvalue(L, LUA_UPVALUEINDEX_51_1);
    for (int i = 1; i <= nargs; ++i)
        g_lua_pushvalue(L, i);

    const int err = g_lua_pcall(L, nargs, 0, 0);
    if (err != 0)
    {
        const char* errMsg = g_lua_tolstring ? g_lua_tolstring(L, -1, nullptr) : nullptr;
        LogDebug("[MissionEmergency] AcceptEmergencyMission original pcall ERR=%d mc=%lld: %s\n",
            err, missionCode, errMsg ? errMsg : "<no message>");
    }
    return 0;
}


static int __cdecl l_VFI_GoToEmergencyMissionOverride(lua_State* L)
{
    if (!ResolveLuaApi() ||
        !g_lua_getfield || !g_lua_type || !g_lua_settop || !g_lua_gettop ||
        !g_lua_isnumber || !g_lua_tointeger || !g_lua_pushvalue || !g_lua_pcall ||
        !g_lua_createtable || !g_lua_pushstring || !g_lua_pushnumber || !g_lua_settable)
    {
        LogDebug("[MissionEmergency] GoToEmergencyMission OVERRIDE: required FFI missing; bailing\n");
        return 0;
    }

    const int top0 = g_lua_gettop(L);


    long long mc = 0;
    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("gvars"));
    if (g_lua_type(L, -1) == LUA_TTABLE)
    {
        g_lua_getfield(L, -1, const_cast<char*>("mis_nextMissionCodeForEmergency"));
        if (g_lua_isnumber(L, -1)) mc = g_lua_tointeger(L, -1);
    }
    g_lua_settop(L, top0);

    const bool isAllowlisted =
        mc > 0 && mc <= 0xFFFF &&
        MissionEmergency_IsEnabled(static_cast<std::uint16_t>(mc));

    long long startRoute = 0;
    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("gvars"));
    if (g_lua_type(L, -1) == LUA_TTABLE)
    {
        g_lua_getfield(L, -1, const_cast<char*>("mis_nextMissionStartRouteForEmergency"));
        if (g_lua_isnumber(L, -1)) startRoute = g_lua_tointeger(L, -1);
    }
    g_lua_settop(L, top0);
    const bool hasRoute = (startRoute != 0);


    if (!isAllowlisted)
    {
        g_lua_pushvalue(L, LUA_UPVALUEINDEX_51_1);
        const int err = g_lua_pcall(L, 0, 0, 0);
        if (err != 0)
        {
            const char* errMsg = g_lua_tolstring ? g_lua_tolstring(L, -1, nullptr) : nullptr;
            LogDebug("[MissionEmergency] original GoToEmergencyMission pcall ERR=%d mc=%lld: %s\n",
                err, mc, errMsg ? errMsg : "<no message>");
            g_lua_settop(L, top0);
        }
        return 0;
    }


    long long clusterId = 2;
    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("gvars"));
    if (g_lua_type(L, -1) == LUA_TTABLE)
    {
        g_lua_getfield(L, -1, const_cast<char*>("mis_nextClusterIdForEmergency"));
        if (g_lua_isnumber(L, -1))
        {
            const long long c = g_lua_tointeger(L, -1);
            if (c > 0) clusterId = c;
        }
    }
    g_lua_settop(L, top0);

    long long missionClearTypeFromHelispace = 1;
    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppDefine"));
    if (g_lua_type(L, -1) == LUA_TTABLE)
    {
        g_lua_getfield(L, -1, const_cast<char*>("MISSION_CLEAR_TYPE"));
        if (g_lua_type(L, -1) == LUA_TTABLE)
        {
            g_lua_getfield(L, -1, const_cast<char*>("FROM_HELISPACE"));
            if (g_lua_isnumber(L, -1))
                missionClearTypeFromHelispace = g_lua_tointeger(L, -1);
        }
    }
    g_lua_settop(L, top0);

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppMission"));
    if (g_lua_type(L, -1) != LUA_TTABLE)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] GoToEmergencyMission OVERRIDE: TppMission missing\n");
        return 0;
    }
    g_lua_getfield(L, -1, const_cast<char*>("ReserveMissionClear"));
    if (g_lua_type(L, -1) != LUA_TFUNCTION)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] GoToEmergencyMission OVERRIDE: ReserveMissionClear missing\n");
        return 0;
    }

    g_lua_createtable(L, 0, hasRoute ? 4 : 3);

    g_lua_pushstring(L, const_cast<char*>("missionClearType"));
    g_lua_pushnumber(L, static_cast<lua_Number>(missionClearTypeFromHelispace));
    g_lua_settable(L, -3);

    g_lua_pushstring(L, const_cast<char*>("nextMissionId"));
    g_lua_pushnumber(L, static_cast<lua_Number>(mc));
    g_lua_settable(L, -3);

    if (hasRoute)
    {
        g_lua_pushstring(L, const_cast<char*>("nextHeliRoute"));
        g_lua_pushnumber(L, static_cast<lua_Number>(startRoute));
        g_lua_settable(L, -3);
    }

    g_lua_pushstring(L, const_cast<char*>("nextClusterId"));
    g_lua_pushnumber(L, static_cast<lua_Number>(clusterId));
    g_lua_settable(L, -3);

    const int err = g_lua_pcall(L, 1, 0, 0);
    if (err != 0)
    {
        const char* errMsg = g_lua_tolstring ? g_lua_tolstring(L, -1, nullptr) : nullptr;
        LogDebug("[MissionEmergency] GoToEmergencyMission ReserveMissionClear pcall ERR=%d mc=%lld: %s\n",
            err, mc, errMsg ? errMsg : "<no message>");
    }

    g_lua_settop(L, top0);
    return 0;
}


static bool InstallAcceptEmergencyMissionOverride(lua_State* L)
{
    if (g_VFI_AcceptEmergencyMissionOverrideInstalled)
        return true;

    if (!ResolveLuaApi() || !g_lua_pushcclosure || !g_lua_pcall)
    {
        LogDebug("[MissionEmergency] InstallAcceptEmergencyMissionOverride: Lua FFI unavailable; skipped\n");
        return false;
    }

    const int top0 = g_lua_gettop(L);

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppMission"));
    if (g_lua_type(L, -1) != LUA_TTABLE)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] InstallAcceptEmergencyMissionOverride: TppMission not loaded; deferred\n");
        return false;
    }

    g_lua_getfield(L, -1, const_cast<char*>("AcceptEmergencyMission"));
    if (g_lua_type(L, -1) != LUA_TFUNCTION)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] InstallAcceptEmergencyMissionOverride: original function missing\n");
        return false;
    }

    g_lua_pushcclosure(L, &l_VFI_AcceptEmergencyMissionOverride, 1);

    g_lua_pushstring(L, const_cast<char*>("AcceptEmergencyMission"));
    g_lua_pushvalue(L, -2);
    g_lua_rawset(L, -4);

    g_lua_settop(L, top0);

    g_VFI_AcceptEmergencyMissionOverrideInstalled = true;
#ifdef _DEBUG
    LogDebug("[MissionEmergency] InstallAcceptEmergencyMissionOverride: installed\n");
#endif
    return true;
}


static bool InstallGoToEmergencyMissionOverride(lua_State* L)
{
    if (g_VFI_GoToEmergencyMissionOverrideInstalled)
        return true;

    if (!ResolveLuaApi() || !g_lua_pushcclosure || !g_lua_pcall)
    {
        LogDebug("[MissionEmergency] InstallGoToEmergencyMissionOverride: Lua FFI unavailable; skipped\n");
        return false;
    }

    const int top0 = g_lua_gettop(L);

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppMission"));
    if (g_lua_type(L, -1) != LUA_TTABLE)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] InstallGoToEmergencyMissionOverride: TppMission not loaded; deferred\n");
        return false;
    }

    g_lua_getfield(L, -1, const_cast<char*>("GoToEmergencyMission"));
    if (g_lua_type(L, -1) != LUA_TFUNCTION)
    {
        g_lua_settop(L, top0);
        LogDebug("[MissionEmergency] InstallGoToEmergencyMissionOverride: original function missing\n");
        return false;
    }

    g_lua_pushcclosure(L, &l_VFI_GoToEmergencyMissionOverride, 1);

    g_lua_pushstring(L, const_cast<char*>("GoToEmergencyMission"));
    g_lua_pushvalue(L, -2);
    g_lua_rawset(L, -4);

    g_lua_settop(L, top0);

    g_VFI_GoToEmergencyMissionOverrideInstalled = true;
#ifdef _DEBUG
    LogDebug("[MissionEmergency] InstallGoToEmergencyMissionOverride: installed\n");
#endif
    return true;
}


int __cdecl l_SetMissionEmergency(lua_State* L)
{
    const int  missionCodeRaw = GetLuaInt(L, 1);
    const bool enabled        = GetLuaBool(L, 2) != 0;

    if (missionCodeRaw <= 0 || missionCodeRaw > 0xFFFF)
    {
        LogDebug("[MissionEmergency] SetMissionEmergency: missionCode %d out of range; bailing\n",
            missionCodeRaw);
        return 0;
    }

    const std::uint16_t missionCode = static_cast<std::uint16_t>(missionCodeRaw);

    MissionEmergency_SetEnabled(missionCode, enabled);

    InstallIsEmergencyMissionOverride(L);
    InstallAcceptEmergencyMissionOverride(L);
    InstallGoToEmergencyMissionOverride(L);

    if (enabled)
        AppendToEmergencyMissionList(L, missionCodeRaw);
    else
        RemoveFromEmergencyMissionList(L, missionCodeRaw);

    return 0;
}


int __cdecl l_IsMissionEmergency(lua_State* L)
{
    const int missionCodeRaw = GetLuaInt(L, 1);

    bool result = false;
    if (missionCodeRaw > 0 && missionCodeRaw <= 0xFFFF)
    {
        result = MissionEmergency_IsEnabled(
            static_cast<std::uint16_t>(missionCodeRaw));
    }

    PushLuaBool(L, result);
    return 1;
}


int __cdecl l_SetEmergencyMissionPopup(lua_State* L)
{
    const char* title = LuaIsString(L, 1) ? GetLuaString(L, 1) : nullptr;
    const char* body  = LuaIsString(L, 2) ? GetLuaString(L, 2) : nullptr;

    const bool ok = Set_MbDvcEmergencyPopup(title, body);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_SetEmergencyMissionPopupLangId(lua_State* L)
{
    const char* titleLabel = LuaIsString(L, 1) ? GetLuaString(L, 1) : nullptr;
    const char* bodyLabel  = LuaIsString(L, 2) ? GetLuaString(L, 2) : nullptr;

    const bool ok = Set_MbDvcEmergencyPopupLangId(titleLabel, bodyLabel);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_ClearEmergencyMissionPopupOverride(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    Clear_MbDvcEmergencyPopupOverride();
    return 0;
}


int __cdecl l_ShowMissionIcon(lua_State* L)
{
    if (!ResolveLuaApi() ||
        !g_lua_gettop || !g_lua_type || !g_lua_settop ||
        !g_lua_getfield || !g_lua_pushvalue || !g_lua_pcall || !g_lua_tolstring ||
        !g_lua_pushstring || !g_lua_pushnumber || !g_lua_isnumber || !g_lua_tonumber)
    {
        return 0;
    }

    const int top0 = g_lua_gettop(L);

    const int titleType = g_lua_type(L, 1);
    const int bodyType  = g_lua_type(L, 2);

    lua_Number timeNum = 6.0;
    if (g_lua_isnumber(L, 3))
        timeNum = g_lua_tonumber(L, 3);

    if (titleType == LUA_TSTRING)
    {
        const char* titleLabel = g_lua_tolstring(L, 1, nullptr);
        if (titleLabel && titleLabel[0])
            ShowMissionIcon_SetTitleHash(FoxHashes::StrCode64(titleLabel) & 0x0000FFFFFFFFFFFFull);
        else
            ShowMissionIcon_SetTitleHash(0);
    }
    else if (titleType == LUA_TNUMBER)
    {
        ShowMissionIcon_SetTitleHash(static_cast<std::uint64_t>(g_lua_tointeger(L, 1)) & 0x0000FFFFFFFFFFFFull);
    }
    else
    {
        ShowMissionIcon_SetTitleHash(0);
    }

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("TppUiCommand"));
    if (g_lua_type(L, -1) != LUA_TTABLE)
    {
        LogDebug("[V_FrameWork.ShowMissionIcon] TppUiCommand not loaded\n");
        g_lua_settop(L, top0);
        return 0;
    }

    g_lua_getfield(L, -1, const_cast<char*>("ShowMissionIcon"));
    if (g_lua_type(L, -1) != LUA_TFUNCTION)
    {
        LogDebug("[V_FrameWork.ShowMissionIcon] TppUiCommand.ShowMissionIcon missing\n");
        g_lua_settop(L, top0);
        return 0;
    }

    g_lua_pushstring(L, const_cast<char*>("urgent_time"));
    g_lua_pushnumber(L, timeNum);

    if (bodyType == LUA_TSTRING)
        g_lua_pushvalue(L, 2);
    else
        g_lua_pushstring(L, const_cast<char*>("announce_online_900_from_0_prio_0"));

    g_lua_pushnil(L);

    const int err = g_lua_pcall(L, 4, 0, 0);
    if (err != 0)
    {
        const char* errMsg = g_lua_tolstring(L, -1, nullptr);
        LogDebug("[V_FrameWork.ShowMissionIcon] pcall ERR=%d: %s\n",
            err, errMsg ? errMsg : "<no message>");
    }

    g_lua_settop(L, top0);
    return 0;
}


int __cdecl l_ShowTimeCigaretteUi(lua_State* L)
{
    PushLuaBool(L, Show_TimeCigaretteUi());
    return 1;
}


int __cdecl l_HideTimeCigaretteUi(lua_State* L)
{
    PushLuaBool(L, Hide_TimeCigaretteUi());
    return 1;
}


std::uint32_t Set_AnnounceLogSE(const char* announceLabel, std::uint32_t seId);
std::uint32_t Set_AnnounceLogEvent(const char* announceLabel, const char* eventName);
std::uint32_t Set_AnnounceLogVoice(const char* announceLabel, const char* voiceName);
std::uint32_t Set_AnnounceLogDialogue(const char* announceLabel, std::uint32_t condition,
                                      std::uint32_t chara, std::uint32_t dialogueEvent);
std::uint32_t Set_AnnounceLogSfx(const char* announceLabel, const char* eventName);
bool Register_AnnounceLogSfx(const char* eventName);
bool IsAnnounceLogSfxRegistered(const char* eventName);
bool Unset_AnnounceLogSE(const char* announceLabel);
bool Unregister_AnnounceLogSfx(const char* eventName);

int __cdecl l_SetAnnounceLogSE(lua_State* L)
{
    const char* label = GetLuaString(L, 1);
    std::uint32_t announceType = 0u;

    if (label && *label)
    {
        if (GetLuaTop(L) >= 2 && LuaIsNumber(L, 2))
        {
            const std::uint32_t n = static_cast<std::uint32_t>(GetLuaInt64(L, 2));
            if (n > 0xFFu)
            {
                const std::uint32_t chara = (GetLuaTop(L) >= 3 && LuaIsNumber(L, 3))
                                          ? static_cast<std::uint32_t>(GetLuaInt64(L, 3)) : 0u;
                const std::uint32_t dlgEv = (GetLuaTop(L) >= 4 && LuaIsNumber(L, 4))
                                          ? static_cast<std::uint32_t>(GetLuaInt64(L, 4)) : 0u;
                announceType = Set_AnnounceLogDialogue(label, n, chara, dlgEv);
            }
            else
            {
                announceType = Set_AnnounceLogSE(label, n);
            }
        }
        else if (GetLuaTop(L) >= 2 && LuaIsString(L, 2))
        {
            const char* s = GetLuaString(L, 2);
            const bool isVoice = s && s[0] == 'V' && s[1] == 'O' && s[2] == 'I' &&
                                 s[3] == 'C' && s[4] == 'E' && s[5] == '_';
            announceType = isVoice ? Set_AnnounceLogVoice(label, s)
                         : IsAnnounceLogSfxRegistered(s) ? Set_AnnounceLogSfx(label, s)
                                   : Set_AnnounceLogEvent(label, s);
        }
        else
            announceType = Set_AnnounceLogSE(label, 0u);
    }

    ResolveLuaApi();
    if (g_lua_pushnumber)
        g_lua_pushnumber(L, static_cast<lua_Number>(announceType));
    return 1;
}


int __cdecl l_RegisterAnnounceLogSfx(lua_State* L)
{
    const char* name = GetLuaString(L, 1);
    const bool ok = Register_AnnounceLogSfx(name);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_UnsetAnnounceLogSE(lua_State* L)
{
    const char* label = GetLuaString(L, 1);
    const bool ok = (label && *label) ? Unset_AnnounceLogSE(label) : false;
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_UnregisterAnnounceLogSfx(lua_State* L)
{
    const char* name = GetLuaString(L, 1);
    const bool ok = Unregister_AnnounceLogSfx(name);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_SetMissionTelopSplashTexturePath(lua_State* L)
{
    const char* path = GetLuaString(L, 1);
    const bool ok = (path != nullptr && path[0] != '\0');
    if (ok)
        Set_MissionTelopSplashTexturePath(path);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_UnsetMissionTelopSplashTexturePath(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    Unset_MissionTelopSplashTexturePath();
    return 0;
}


static int __cdecl l_Log(lua_State* L)
{
    const char* msg = GetLuaString(L, 1);
    if (msg && *msg)
    {


        Log("%s\n", msg);
    }
    return 0;
}

static std::uint64_t ParseSahelanFovaArg(const char* text)
{
    if (!text || !*text)
        return 0;


    const bool hasHexPrefix = (text[0] == '0') && (text[1] == 'x' || text[1] == 'X');

    bool looksLikePath = false;
    for (const char* p = text; *p; ++p)
    {
        const char c = *p;
        if (c == '/' || c == '\\')
        {
            looksLikePath = true;
            break;
        }
    }

    if (!looksLikePath)
    {
        const char* hexStart = hasHexPrefix ? text + 2 : text;
        bool allHex = true;
        size_t hexLen = 0;
        for (const char* p = hexStart; *p; ++p)
        {
            const char c = *p;
            const bool isHexDigit =
                (c >= '0' && c <= '9') ||
                (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F');
            if (!isHexDigit)
            {
                allHex = false;
                break;
            }
            ++hexLen;
        }

        if (allHex && hexLen > 0 && hexLen <= 16)
        {
            return std::strtoull(hexStart, nullptr, 16);
        }
    }


    return FoxHashes::PathCode64Ext(text);
}


int __cdecl l_SetEyeLampColorLogging(lua_State* L)
{
    const bool enabled = GetLuaBool(L, 1);
    ::Set_EyeLampColorLogging(enabled);
    return 0;
}


int __cdecl l_FNVHash32(lua_State* L)
{
    const char* s = GetLuaString(L, 1);
    const std::uint32_t h = (s && *s) ? FoxHashes::FNVHash32(s) : 0u;
    ResolveLuaApi();
    if (g_lua_pushnumber)
        g_lua_pushnumber(L, static_cast<lua_Number>(h));
    return 1;
}


int __cdecl l_ShowMbDvcAnnouncePopupReport(lua_State* L)
{
    const char* title = GetLuaString(L, 1);
    const char* body  = GetLuaString(L, 2);
    if (!title) title = "";
    if (!body)  body  = "";

    const bool ok = Show_MbDvcAnnouncePopupReport(title, body);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_ShowMbDvcAnnouncePopupReportLangId(lua_State* L)
{
    const char* titleLabel = GetLuaString(L, 1);
    const char* bodyLabel  = GetLuaString(L, 2);
    if (!titleLabel) titleLabel = "";
    if (!bodyLabel)  bodyLabel  = "";

    const bool ok = Show_MbDvcAnnouncePopupByLangId(titleLabel, bodyLabel);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_ShowMbDvcAnnouncePopupReward(lua_State* L)
{
    const char* title = GetLuaString(L, 1);
    const char* body  = GetLuaString(L, 2);
    if (!title) title = "";
    if (!body)  body  = "";

    const bool ok = Show_MbDvcAnnouncePopupReward(title, body);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_ShowMbDvcAnnouncePopupRewardLangId(lua_State* L)
{
    const char* titleLabel = GetLuaString(L, 1);
    const char* bodyLabel  = GetLuaString(L, 2);
    if (!titleLabel) titleLabel = "";
    if (!bodyLabel)  bodyLabel  = "";

    const bool ok = Show_MbDvcAnnouncePopupRewardLangId(titleLabel, bodyLabel);
    PushLuaBool(L, ok);
    return 1;
}


int __cdecl l_SetEnemyInformationLangId(lua_State* L)
{
    const char* name = GetLuaString(L, 1);
    if (!name || !*name)
        return 0;

    EnemyLangId_SetMapOverride(FoxHashes::StrCode64(name));
    return 0;
}



int __cdecl l_SetEnemyUnitName(lua_State* L)
{
    const char* name = GetLuaString(L, 1);
    if (!name || !*name)
        return 0;

    EnemyLangId_SetBinoOverride(FoxHashes::StrCode64(name));
    return 0;
}



int __cdecl l_SetEnemyInformationLangIdForSoldier(lua_State* L)
{
    const std::uint32_t gameObjectId = static_cast<std::uint32_t>(GetLuaInt64(L, 1));
    const char* name = GetLuaString(L, 2);
    if (!name || !*name)
        return 0;

    EnemyLangId_SetMapOverrideForSoldier(gameObjectId, FoxHashes::StrCode64(name));
    return 0;
}




int __cdecl l_SetEnemyUnitNameForSoldier(lua_State* L)
{
    const std::uint32_t gameObjectId = static_cast<std::uint32_t>(GetLuaInt64(L, 1));
    const char* name = GetLuaString(L, 2);
    if (!name || !*name)
        return 0;

    EnemyLangId_SetBinoOverrideForSoldier(gameObjectId, FoxHashes::StrCode64(name));
    return 0;
}




static void PushLuaUInt32(lua_State* L, std::uint32_t v)
{
    if (ResolveLuaApi() && g_lua_pushnumber)
        g_lua_pushnumber(L, static_cast<lua_Number>(static_cast<double>(v)));
}


static bool CT_ReadStrField(lua_State* L, int t, const char* key, std::string& out)
{
    g_lua_pushstring(L, const_cast<char*>(key));
    g_lua_gettable(L, t);
    bool ok = false;
    if (g_lua_type(L, -1) == LUA_TSTRING)
    {
        const char* s = g_lua_tolstring(L, -1, nullptr);
        if (s) { out = s; ok = true; }
    }
    g_lua_settop(L, g_lua_gettop(L) - 1);
    return ok;
}

static long long CT_ReadIntField(lua_State* L, int t, const char* key, long long def)
{
    g_lua_pushstring(L, const_cast<char*>(key));
    g_lua_gettable(L, t);
    long long v = def;
    if (g_lua_type(L, -1) == LUA_TNUMBER)
        v = static_cast<long long>(g_lua_tonumber(L, -1));
    g_lua_settop(L, g_lua_gettop(L) - 1);
    return v;
}

int __cdecl l_RegisterCustomTapes(lua_State* L)
{
    if (!ResolveLuaApi() || g_lua_type(L, 1) != LUA_TTABLE)
    {
        PushLuaBool(L, false);
        return 1;
    }

    std::vector<CustomTapeAlbumDefinition> albums;
    std::vector<CustomTapeTrackDefinition> tracks;

    g_lua_pushstring(L, const_cast<char*>("albums"));
    g_lua_gettable(L, 1);
    if (g_lua_type(L, -1) == LUA_TTABLE)
    {
        const int at = GetLuaTop(L);
        const std::size_t n = g_lua_objlen(L, at);
        for (std::size_t i = 1; i <= n; ++i)
        {
            g_lua_rawgeti(L, at, static_cast<int>(i));
            const int et = GetLuaTop(L);
            if (g_lua_type(L, et) == LUA_TTABLE)
            {
                CustomTapeAlbumDefinition def;
                const bool a = CT_ReadStrField(L, et, "albumId", def.albumId);
                const bool b = CT_ReadStrField(L, et, "langId", def.langId);
                const bool c = CT_ReadStrField(L, et, "type", def.type);
                def.typeValue = static_cast<std::int32_t>(CT_ReadIntField(L, et, "typeValue", -1));
                if (a && b && (c || def.typeValue >= 0)) albums.push_back(def);
            }
            g_lua_settop(L, at);
        }
    }
    g_lua_settop(L, g_lua_gettop(L) - 1);

    g_lua_pushstring(L, const_cast<char*>("tracks"));
    g_lua_gettable(L, 1);
    if (g_lua_type(L, -1) == LUA_TTABLE)
    {
        const int tt = GetLuaTop(L);
        const std::size_t n = g_lua_objlen(L, tt);
        for (std::size_t i = 1; i <= n; ++i)
        {
            g_lua_rawgeti(L, tt, static_cast<int>(i));
            const int et = GetLuaTop(L);
            if (g_lua_type(L, et) == LUA_TTABLE)
            {
                CustomTapeTrackDefinition def;
                const bool a = CT_ReadStrField(L, et, "albumId", def.albumId);
                const bool b = CT_ReadStrField(L, et, "langId", def.langId);
                const bool c = CT_ReadStrField(L, et, "fileName", def.fileName);
                def.saveIndex  = -1;
                def.dataTimeJp = static_cast<std::uint32_t>(CT_ReadIntField(L, et, "dataTimeJp", 0));
                def.dataTimeEn = static_cast<std::uint32_t>(CT_ReadIntField(L, et, "dataTimeEn", 0));
                def.important  = static_cast<std::uint16_t>(CT_ReadIntField(L, et, "important", 0));
                def.special    = static_cast<std::uint16_t>(CT_ReadIntField(L, et, "special", 0));
                def.unlocked   = CT_ReadIntField(L, et, "unlocked", 0) != 0;
                if (a && b && c) tracks.push_back(def);
            }
            g_lua_settop(L, tt);
        }
    }
    g_lua_settop(L, g_lua_gettop(L) - 1);

    const bool ok = Register_CustomTapes(albums, tracks);
#ifdef _DEBUG
    Log("[CustomTapes] RegisterCustomTapes albums=%zu tracks=%zu -> %s\n",
        albums.size(), tracks.size(), ok ? "OK" : "FAIL");
#endif
    PushLuaBool(L, ok);
    return 1;
}


static bool CT_ResolveTapeSaveIndexArg(lua_State* L, std::int16_t& outSaveIndex)
{
    outSaveIndex = -1;
    const int t = LuaType(L, 1);
    if (t == LUA_TSTRING)
        outSaveIndex = ResolveCassetteSaveIndexByTrackName(GetLuaString(L, 1));
    else if (t == LUA_TNUMBER)
        outSaveIndex = static_cast<std::int16_t>(GetLuaInt(L, 1));
    return outSaveIndex >= 0;
}

int __cdecl l_HideCassetteTape(lua_State* L)
{
    std::int16_t saveIndex = -1;
    if (CT_ResolveTapeSaveIndexArg(L, saveIndex))
        Hide_CassetteTape(saveIndex);
    return 0;
}

int __cdecl l_ShowCassetteTape(lua_State* L)
{
    std::int16_t saveIndex = -1;
    if (CT_ResolveTapeSaveIndexArg(L, saveIndex))
        Show_CassetteTape(saveIndex);
    return 0;
}

int __cdecl l_SetOwnershipCassetteTape(lua_State* L)
{
    std::int16_t saveIndex = -1;
    if (CT_ResolveTapeSaveIndexArg(L, saveIndex))
        Set_CassetteTapeOwned(saveIndex, GetLuaBool(L, 2));
    return 0;
}

int __cdecl l_SetNewFlagCassetteTape(lua_State* L)
{
    std::int16_t saveIndex = -1;
    if (CT_ResolveTapeSaveIndexArg(L, saveIndex))
        Set_CassetteTapeNewFlag(saveIndex, GetLuaBool(L, 2));
    return 0;
}

static luaL_Reg g_VFrameWorkLib[] =
{
    { "Log",                                    l_Log },
    { nullptr, nullptr }
};


static void RegisterAllUiLuaLibraries(lua_State* L)
{
    if (!L)
        return;

    if (IsLuaStateRegistered(L))
        return;

    OutfitLua_EnsureEquipDevelopBound();

    if (RegisterLuaLibrary(L, "V_FrameWork", g_VFrameWorkLib))
    {
        Register_V_TppUiCommandLibrary(L);
        Register_V_TppSoundDaemonLibrary(L);
        Register_V_TppGameObjectConstants(L);
        Register_V_TppMbDevConstants(L);
        Register_V_PlayerCqcStanceConstants(L);
        Register_V_TppCallSignConstants(L);
        Register_V_TppCassetteLibrary(L);
        Register_V_TppSahelanLibrary(L);
        Register_V_TppPlayerLibrary(L);
        Register_V_FoxLibrary(L);
        Register_V_PickableLibrary(L);
        Register_V_HelicopterLibrary(L);
        Register_V_TppMotherBaseManagementLibrary(L);
        Register_V_TppEquipLibrary(L);
        TrackLuaState(L);
    }
}


static void __fastcall hkSetLuaFunctions(lua_State* L)
{
    if (g_OrigSetLuaFunctions)
    {
        g_OrigSetLuaFunctions(L);
    }

    RegisterAllUiLuaLibraries(L);
}


extern "C" __declspec(dllexport) int __cdecl luaopen_V_FrameWork(lua_State* L)
{
    if (!L)
        return 0;

    if (IsLuaStateRegistered(L))
        return 0;

    OutfitLua_EnsureEquipDevelopBound();

    Register_V_TppUiCommandLibrary(L);
    Register_V_TppSoundDaemonLibrary(L);
    Register_V_TppGameObjectConstants(L);
    Register_V_TppMbDevConstants(L);
    Register_V_PlayerCqcStanceConstants(L);
    Register_V_TppCallSignConstants(L);
    Register_V_TppCassetteLibrary(L);
    Register_V_TppSahelanLibrary(L);
    Register_V_TppPlayerLibrary(L);
    Register_V_FoxLibrary(L);
    Register_V_PickableLibrary(L);
    Register_V_HelicopterLibrary(L);
    Register_V_TppMotherBaseManagementLibrary(L);
    Register_V_TppEquipLibrary(L);

    if (!RegisterLuaLibrary(L, "V_FrameWork", g_VFrameWorkLib))
        return 0;

    TrackLuaState(L);
    return 1;
}


static int __fastcall hkLuaPcallPump(lua_State* L, int nargs, int nresults,
                                     int errfunc)
{
    V_FrameWork::EnterLuaPcall();
    const int r = g_OrigLuaPcallPump
        ? g_OrigLuaPcallPump(L, nargs, nresults, errfunc)
        : 0;
    V_FrameWork::ExitLuaPcall();
    return r;
}


static void Install_LuaPcallPump_Hook()
{
    if (g_LuaPcallPumpInstalled)
        return;
    void* target = ResolveGameAddress(gAddr.lua_pcall);
    if (!target)
    {
        LogDebug("[Hook] lua_pcall pump: no address on this build - demand "
            "paging falls back to the visibility-check trigger.\n");
        return;
    }
    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkLuaPcallPump),
        reinterpret_cast<void**>(&g_OrigLuaPcallPump));
    if (ok)
        g_LuaPcallPumpInstalled = true;
    else
        Log("[Hook] lua_pcall pump: FAILED target=%p - demand paging falls "
            "back to the visibility-check trigger.\n", target);
}


static void Uninstall_LuaPcallPump_Hook()
{
    if (!g_LuaPcallPumpInstalled)
        return;
    DisableAndRemoveHook(ResolveGameAddress(gAddr.lua_pcall));
    g_OrigLuaPcallPump = nullptr;
    g_LuaPcallPumpInstalled = false;
}


bool Set_MissionDeployWarning(std::uint16_t missionCode, const char* langId, const char* colorName);
void Clear_MissionDeployWarning(std::uint16_t missionCode);

int __cdecl l_SetMissionAcceptWarning(lua_State* L)
{
    const int code = GetLuaInt(L, 1);
    const char* langId = LuaIsString(L, 2) ? GetLuaString(L, 2) : nullptr;
    const char* colorName = LuaIsString(L, 3) ? GetLuaString(L, 3) : nullptr;

    bool ok = false;
    if (code > 0 && code <= 0xFFFF)
        ok = Set_MissionDeployWarning(static_cast<std::uint16_t>(code), langId, colorName);

    PushLuaBool(L, ok);
    return 1;
}

int __cdecl l_ClearMissionAcceptWarning(lua_State* L)
{
    const int code = GetLuaInt(L, 1);
    if (code > 0 && code <= 0xFFFF)
        Clear_MissionDeployWarning(static_cast<std::uint16_t>(code));
    return 0;
}


bool Set_MissionMenuHelp(std::uint16_t missionCode, const char* langId, const char* colorName);
void Clear_MissionMenuHelp(std::uint16_t missionCode);

int __cdecl l_SetMissionMenuHelp(lua_State* L)
{
    const int code = GetLuaInt(L, 1);
    const char* langId = LuaIsString(L, 2) ? GetLuaString(L, 2) : nullptr;
    const char* colorName = LuaIsString(L, 3) ? GetLuaString(L, 3) : nullptr;

    bool ok = false;
    if (code > 0 && code <= 0xFFFF)
        ok = Set_MissionMenuHelp(static_cast<std::uint16_t>(code), langId, colorName);

    PushLuaBool(L, ok);
    return 1;
}

int __cdecl l_ClearMissionMenuHelp(lua_State* L)
{
    const int code = GetLuaInt(L, 1);
    if (code > 0 && code <= 0xFFFF)
        Clear_MissionMenuHelp(static_cast<std::uint16_t>(code));
    return 0;
}



bool Install_SetLuaFunctions_Hook()
{
    if (g_SetLuaFunctionsHookInstalled)
    {
#ifdef _DEBUG
        LogDebug("[Hook] SetLuaFunctions: already installed\n");
#endif
        return true;
    }

    ResolveLuaApi();

    void* target = ResolveGameAddress(gAddr.SetLuaFunctions);
    if (!target)
        return false;

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkSetLuaFunctions),
        reinterpret_cast<void**>(&g_OrigSetLuaFunctions));

    if (ok)
    {
        g_SetLuaFunctionsHookInstalled = true;
        Install_LuaPcallPump_Hook();
    }

    if (ok)
        LogDebug("[Hook] SetLuaFunctions: OK target=%p orig=%p\n",
                 target, g_OrigSetLuaFunctions);
    else
        Log("[Hook] SetLuaFunctions: FAILED target=%p\n", target);
    return ok;
}


bool Uninstall_SetLuaFunctions_Hook()
{
    Uninstall_LuaPcallPump_Hook();
    DisableAndRemoveHook(ResolveGameAddress(gAddr.SetLuaFunctions));
    g_OrigSetLuaFunctions = nullptr;
    ClearTrackedLuaStates();
    return true;
}
