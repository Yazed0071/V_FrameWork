#include "pch.h"
#include "GameObjectSendCommand.h"

extern "C" {
    #include "lua.h"
}

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "log.h"
#include "../../../core/AddressSet.h"
#include "../../../core/HookUtils.h"
#include "../../../core/MissionCodeGuard.h"
#include "../../../core/MissionStateReset.h"
#include "../../sahelan/PhaseSneakAiImpl_PreUpdate.h"
#include "../../sahelan/RealizedSahelanFovaHook.h"
#include "../../sahelan/SetEyeLampColorHook.h"
#include "../../soldier/LostHostageHook.h"
#include "../../soldier/StepRadioDiscovery.h"
#include "../../soldier/VIPSleepFaintHook.h"
#include "../../soldier/VIPHoldupHook.h"
#include "../../soldier/VIPRadioHook.h"
#include "../../soldier/GetVoiceParamWithCallSign.h"
#include "../../soldier/ActionCoreImpl_UpdateOptCamo.h"
#include "../../soldier/NoticeControllerImpl_GetOccasionalChat.h"
#include "../../soldier/CautionStepNormalTimerHook.h"
#include "../../soldier/SoldierObjectRtpc.h"
#include "../../soldier/NoticeControllerImpl_CheckSightNoticePlayer.h"
#include "GetGameObjectIdWithIndex.h"
#include "../../soldier/InterrogationVoiceEvent.h"
#include "../../bullet/Bullet3Impl_ActivateBulletAtEmptyWorkPatch.h"
#include "../../../core/FoxHashes.h"
#include "../../../lua/LuaApi.h"

namespace
{

    using GameObjectSendCommand_t = int (__fastcall*)(lua_State* L);

    static GameObjectSendCommand_t g_OrigSendCommand = nullptr;
    static bool                    g_Installed       = false;

    static const char* ReadCommandId(lua_State* L, int cmdStackIdx, std::string* out)
    {
        out->clear();
        g_lua_pushstring(L, const_cast<char*>("id"));
        g_lua_gettable(L, cmdStackIdx);
        if (g_lua_type(L, -1) != LUA_TSTRING) return nullptr;
        const char* s = g_lua_tolstring(L, -1, nullptr);
        if (!s) return nullptr;
        *out = s;
        return out->c_str();
    }

    static double ReadCommandNumber(lua_State* L, int cmdStackIdx, const char* key)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        double v = 0.0;
        if (g_lua_type(L, -1) == LUA_TNUMBER)
            v = static_cast<double>(g_lua_tonumber(L, -1));
        return v;
    }

    static double ReadCommandNumberOr(lua_State* L, int cmdStackIdx, const char* key, double def)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        double v = def;
        if (g_lua_type(L, -1) == LUA_TNUMBER)
            v = static_cast<double>(g_lua_tonumber(L, -1));
        return v;
    }

    static bool ReadCommandBool(lua_State* L, int cmdStackIdx, const char* key)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        bool v = false;
        const int t = g_lua_type(L, -1);
        if (t == LUA_TBOOLEAN)
            v = g_lua_toboolean(L, -1) != 0;
        else if (t == LUA_TNUMBER)
            v = static_cast<int>(g_lua_tonumber(L, -1)) != 0;
        return v;
    }

    static std::uint32_t ReadCommandStrCode32(lua_State* L, int cmdStackIdx, const char* key)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        std::uint32_t v = 0;
        const int t = g_lua_type(L, -1);
        if (t == LUA_TNUMBER)
            v = static_cast<std::uint32_t>(static_cast<long long>(g_lua_tonumber(L, -1)));
        else if (t == LUA_TSTRING)
        {
            const char* s = g_lua_tolstring(L, -1, nullptr);
            if (s && s[0])
                v = FoxHashes::StrCode32(s);
        }
        return v;
    }

    static std::uint32_t FnvHash32Of(const char* s)
    {
        using FNV_t = unsigned int(__fastcall*)(const char*);
        static FNV_t fn = nullptr;
        if (!fn && gAddr.FNVHash32)
            fn = reinterpret_cast<FNV_t>(ResolveGameAddress(gAddr.FNVHash32));
        if (!fn || !s || !s[0])
            return 0;
        __try { return static_cast<std::uint32_t>(fn(s)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static std::uint32_t ReadCommandFnvHash(lua_State* L, int cmdStackIdx, const char* key,
                                            bool* present)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        std::uint32_t v = 0;
        bool have = false;
        const int t = g_lua_type(L, -1);
        if (t == LUA_TNUMBER)
        {
            v = static_cast<std::uint32_t>(static_cast<long long>(g_lua_tonumber(L, -1)));
            have = true;
        }
        else if (t == LUA_TSTRING)
        {
            const char* s = g_lua_tolstring(L, -1, nullptr);
            if (s && s[0])
                v = FnvHash32Of(s);
            have = true;
        }
        if (present)
            *present = have;
        return v;
    }

    static std::uint32_t ReadCommandTargetId(lua_State* L)
    {
        if (g_lua_type(L, 1) == LUA_TNUMBER)
            return static_cast<std::uint32_t>(g_lua_tointeger(L, 1) & 0xFFFFFFFFLL);
        return 0;
    }

    static std::size_t ReadLabelArray(lua_State* L, int cmdStackIdx, std::uint32_t* out, const char* key)
    {
        std::size_t n = 0;

        if (g_lua_objlen && g_lua_rawgeti)
        {
            g_lua_pushstring(L, const_cast<char*>(key));
            g_lua_gettable(L, cmdStackIdx);
            if (g_lua_type(L, -1) == LUA_TTABLE)
            {
                const int tbl = g_lua_gettop(L);
                const std::size_t len = g_lua_objlen(L, tbl);
                for (std::size_t i = 1; i <= len && n < 255; ++i)
                {
                    g_lua_rawgeti(L, tbl, static_cast<int>(i));
                    const int et = g_lua_type(L, -1);
                    if (et == LUA_TNUMBER)
                        out[n++] = static_cast<std::uint32_t>(static_cast<long long>(g_lua_tonumber(L, -1)));
                    else if (et == LUA_TSTRING)
                    {
                        const char* s = g_lua_tolstring(L, -1, nullptr);
                        if (s) out[n++] = FoxHashes::StrCode32(s);
                    }
                    g_lua_settop(L, tbl);
                }
            }
        }

        return n;
    }

    static float SmartScaleA(float a)
    {
        return (a > 1.0f) ? (a * (1.0f / 255.0f)) : a;
    }

    static void SmartScaleRgb(float* r, float* g, float* b)
    {
        if (*r > 1.0f || *g > 1.0f || *b > 1.0f)
        {
            *r *= (1.0f / 255.0f);
            *g *= (1.0f / 255.0f);
            *b *= (1.0f / 255.0f);
        }
    }

    static void ReadColor(lua_State* L, int cmdStackIdx, float* r, float* g, float* b, float* a, float defaultA)
    {
        *r = 0.0f; *g = 0.0f; *b = 0.0f; *a = defaultA;
        g_lua_pushstring(L, const_cast<char*>("color"));
        g_lua_gettable(L, cmdStackIdx);
        if (g_lua_type(L, -1) == LUA_TTABLE)
        {
            const int t = g_lua_gettop(L);
            g_lua_pushstring(L, const_cast<char*>("r")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *r = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
            g_lua_pushstring(L, const_cast<char*>("g")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *g = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
            g_lua_pushstring(L, const_cast<char*>("b")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *b = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
            g_lua_pushstring(L, const_cast<char*>("a")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *a = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
        }
        SmartScaleRgb(r, g, b);
        *a = SmartScaleA(*a);
    }

    static bool CautionPerCpTarget(lua_State* L)
    {
        const int top = g_lua_gettop(L);
        bool isPerCp = false;
        const int a1 = g_lua_type(L, 1);
        if (a1 == LUA_TNUMBER)
        {
            isPerCp = true;
        }
        else if (a1 == LUA_TTABLE)
        {
            g_lua_pushstring(L, const_cast<char*>("index"));
            g_lua_gettable(L, 1);
            isPerCp = (g_lua_type(L, -1) != LUA_TNIL);
        }
        g_lua_settop(L, top);
        return isPerCp;
    }

    static int __fastcall hk_SendCommand(lua_State* L)
    {
        if (!g_OrigSendCommand) return 0;

        MISSION_GUARD_ORIGINAL_RET(g_OrigSendCommand, L);

        MissionStateReset::PollMissionChange();

        if (!ResolveLuaApi()) return g_OrigSendCommand(L);

        MissionStateReset::EnsureFinalizerRegistered(L);

        const int top = g_lua_gettop(L);
        if (top < 2 || g_lua_type(L, 2) != LUA_TTABLE)
            return g_OrigSendCommand(L);

        std::string idStr;
        const char* id = ReadCommandId(L, 2, &idStr);
        if (!id || !*id)
        {
            g_lua_settop(L, top);
            return g_OrigSendCommand(L);
        }
        g_lua_settop(L, top);

        if (idStr == "SetSahelanPhase")
        {
            const std::int32_t phase =
                static_cast<std::int32_t>(ReadCommandNumber(L, 2, "phase"));
            g_lua_settop(L, top);
            ::Set_SahelanForcePhase(phase);
            return 0;
        }
        if (idStr == "GetSahelanPhase")
        {
            const double phase = static_cast<double>(::Get_SahelanCurrentPhase());
            g_lua_pushnumber(L, phase);
            return 1;
        }
        if (idStr == "SetEscapeState")
        {
            std::uint32_t gameObjectId = 0;
            if (g_lua_type(L, 1) == LUA_TNUMBER)
            {
                gameObjectId = static_cast<std::uint32_t>(
                    g_lua_tointeger(L, 1) & 0xFFFFFFFFLL);
            }
            const bool enable = ReadCommandBool(L, 2, "enable");
            g_lua_settop(L, top);
            ::PlayerTookHostage(gameObjectId, enable);
            return 0;
        }
        if (idStr == "SetOccasionalChatList")
        {
            std::uint32_t labels[256];
            const std::size_t n = ReadLabelArray(L, 2, labels, "labels");
            g_lua_settop(L, top);
            ::SetOccasionalChatList(labels, n);
            return 0;
        }
        if (idStr == "InsertToOccasionalChatList")
        {
            std::uint32_t labels[256];
            const std::size_t n = ReadLabelArray(L, 2, labels, "labels");
            g_lua_settop(L, top);
            ::InsertToOccasionalChatList(labels, n);
            return 0;
        }
        if (idStr == "RemoveFromOccasionalChatList")
        {
            std::uint32_t labels[256];
            const std::size_t n = ReadLabelArray(L, 2, labels, "labels");
            g_lua_settop(L, top);
            ::RemoveFromOccasionalChatList(labels, n);
            return 0;
        }

        if (idStr == "SetCautionPhaseDuration")
        {
            const double duration = ReadCommandNumber(L, 2, "duration");

            bool isPerCp = false;
            const int a1 = g_lua_type(L, 1);
            if (a1 == LUA_TNUMBER)
            {
                isPerCp = true;
            }
            else if (a1 == LUA_TTABLE)
            {
                g_lua_pushstring(L, const_cast<char*>("index"));
                g_lua_gettable(L, 1);
                isPerCp = (g_lua_type(L, -1) != LUA_TNIL);
            }
            g_lua_settop(L, top);

            if (isPerCp)
            {
                ::Set_PendingCautionDurationForCp(static_cast<float>(duration));
                return g_OrigSendCommand(L);
            }

            ::Set_CautionStepNormalDurationSeconds(static_cast<float>(duration));
            return 0;
        }
        if (idStr == "GetCautionPhaseDuration")
        {
            if (CautionPerCpTarget(L))
            {
                ::Arm_CautionCpCapture();
                g_OrigSendCommand(L);
                const std::uint32_t cp = ::Take_CautionCpIndex();
                g_lua_settop(L, top);
                g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalDurationSecondsForCp(cp)));
                return 1;
            }
            g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalDurationSeconds()));
            return 1;
        }
        if (idStr == "UnsetCautionPhaseDuration")
        {
            if (CautionPerCpTarget(L))
            {
                ::Arm_CautionCpCapture();
                g_OrigSendCommand(L);
                const std::uint32_t cp = ::Take_CautionCpIndex();
                ::Unset_CautionStepNormalDurationSecondsForCp(cp);
                g_lua_settop(L, top);
                return 0;
            }
            g_lua_settop(L, top);
            ::Unset_CautionStepNormalDurationSeconds();
            return 0;
        }
        if (idStr == "GetCautionPhaseRemaining")
        {
            if (CautionPerCpTarget(L))
            {
                ::Arm_CautionCpCapture();
                g_OrigSendCommand(L);
                const std::uint32_t cp = ::Take_CautionCpIndex();
                g_lua_settop(L, top);
                g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalRemainingSecondsForCp(cp)));
                return 1;
            }
            g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalRemainingSeconds()));
            return 1;
        }

        if (idStr == "SetRestrictNotice")
        {
            const std::uint32_t id = ReadCommandTargetId(L);

            std::uint8_t mask = 0;
            if (ReadCommandBool(L, 2, "enabled"))
            {
                if (ReadCommandBool(L, 2, "ignorePlayer"))       mask |= SoldierNoticeIgnore::kPlayer;
                if (ReadCommandBool(L, 2, "ignoreHostage"))      mask |= SoldierNoticeIgnore::kHostage;
                if (ReadCommandBool(L, 2, "ignoreNoticeObject")) mask |= SoldierNoticeIgnore::kNoticeObject;
                if (ReadCommandBool(L, 2, "ignoreCBox"))         mask |= SoldierNoticeIgnore::kCBox;
                if (ReadCommandBool(L, 2, "ignoreNoise"))        mask |= SoldierNoticeIgnore::kNoise;
            }
            g_lua_settop(L, top);

            const int r = g_OrigSendCommand(L);

            if ((id >> 9) == TppGameObjectType::kSoldier2)
                ::Set_SoldierNoticeIgnoreMask(id, mask);
            return r;
        }
        if (idStr == "SetVoicePitch")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const float cents = static_cast<float>(ReadCommandNumber(L, 2, "pitch"));
            g_lua_settop(L, top);
            if (!::Set_SoldierVoicePitch(id, cents))
                Log("[SoldierVoicePitch] ERROR: SetVoicePitch could not apply pitch %.2f to game "
                    "object 0x%X - that soldier has no live sound object, so it keeps its vanilla "
                    "voice pitch.\n", cents, id);
            return 0;
        }
        if (idStr == "SetVIPImportant")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const bool isOfficer = ReadCommandBool(L, 2, "isOfficer");
            const std::uint32_t deadBodyLabel = ReadCommandStrCode32(L, 2, "deadBodyLabel");
            g_lua_settop(L, top);
            ::Add_VIPSleepFaintImportantGameObjectId(id, isOfficer);
            ::Add_VIPHoldupImportantGameObjectId(id, isOfficer);
            ::Add_VIPRadioImportantGameObjectId(id, isOfficer, deadBodyLabel);
            return 0;
        }
        if (idStr == "RemoveVIPImportant")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            g_lua_settop(L, top);
            ::Remove_VIPSleepFaintImportantGameObjectId(id);
            ::Remove_VIPHoldupImportantGameObjectId(id);
            ::Remove_VIPRadioImportantGameObjectId(id);
            return 0;
        }
        if (idStr == "SetUseConcernedHoldupRecovery")
        {
            const bool enable = ReadCommandBool(L, 2, "enable");
            g_lua_settop(L, top);
            ::Set_UseCustomNonVipHoldupRecovery(enable);
            return 0;
        }
        if (idStr == "AddCallSignPatrolSoldier")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            g_lua_settop(L, top);
            ::Add_CallSignExtraSoldier(id);
            return 0;
        }
        if (idStr == "RemoveCallSignPatrolSoldier")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            g_lua_settop(L, top);
            ::Remove_CallSignExtraSoldier(id);
            return 0;
        }
        if (idStr == "ClearLostHostages")
        {
            g_lua_settop(L, top);
            ::Clear_LostHostagesTrap();
            ::Clear_LostHostageDiscovery();
            return 0;
        }
        if (idStr == "SetLostHostage")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const int hostageType = static_cast<int>(ReadCommandNumber(L, 2, "hostageType"));
            const std::uint32_t customLostLabel = ReadCommandStrCode32(L, 2, "customLostLabel");
            g_lua_settop(L, top);
            ::Add_LostHostageTrap(id, hostageType, customLostLabel);
            ::Add_LostHostageDiscovery(id, hostageType);
            return 0;
        }
        if (idStr == "RemoveLostHostage")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            g_lua_settop(L, top);
            ::Remove_LostHostageTrap(id);
            ::Remove_LostHostageDiscovery(id);
            return 0;
        }
        if (idStr == "EnableSoldierStealthCamo")
        {
            const std::uint32_t mappedIndex = ReadCommandTargetId(L);
            const bool enable = ReadCommandBool(L, 2, "enable");
            g_lua_settop(L, top);
            ::Set_UpdateOptCamoEnableMappedIndex(mappedIndex, enable);
            return 0;
        }
        if (idStr == "SetSahelanFova")
        {
            std::string fv2;
            g_lua_pushstring(L, const_cast<char*>("fv2"));
            g_lua_gettable(L, 2);
            if (g_lua_type(L, -1) == LUA_TSTRING)
            {
                const char* s = g_lua_tolstring(L, -1, nullptr);
                if (s) fv2 = s;
            }
            g_lua_settop(L, top);
            ::Set_SahelanFovaPath(fv2.c_str());
            return 0;
        }
        if (idStr == "SetEyeLampColor")
        {
            float r, g, b, a;
            ReadColor(L, 2, &r, &g, &b, &a, 1.0f);
            const int   mode = static_cast<int>(ReadCommandNumberOr(L, 2, "phase", -1.0));
            g_lua_settop(L, top);
            ::Set_EyeLampColor(mode, r, g, b, a);
            return 0;
        }
        if (idStr == "SetEyeLampDisco")
        {
            const bool  enabled = ReadCommandBool(L, 2, "enabled");
            const float speed   = static_cast<float>(ReadCommandNumber(L, 2, "speed"));
            const float a       = SmartScaleA(static_cast<float>(ReadCommandNumberOr(L, 2, "a", 1.0)));
            g_lua_settop(L, top);
            ::Set_EyeLampDisco(enabled, speed, a);
            return 0;
        }
        if (idStr == "SetHeartLightColor")
        {
            float r, g, b, a;
            ReadColor(L, 2, &r, &g, &b, &a, 1.0f);
            const int mode = static_cast<int>(ReadCommandNumberOr(L, 2, "phase", -1.0));
            g_lua_settop(L, top);
            ::Set_HeartLightColor(mode, r, g, b, a);
            return 0;
        }
        if (idStr == "SetHeartLightDisco")
        {
            const bool  enabled = ReadCommandBool(L, 2, "enabled");
            const float speed   = static_cast<float>(ReadCommandNumber(L, 2, "speed"));
            const float a       = SmartScaleA(static_cast<float>(ReadCommandNumberOr(L, 2, "a", 1.0)));
            g_lua_settop(L, top);
            ::Set_HeartLightDisco(enabled, speed, a);
            return 0;
        }

        if (idStr == "SetFriendlyFire")
        {
            const bool enable = ReadCommandBool(L, 2, "enable");
            g_lua_settop(L, top);
            ::Set_FriendlyFire(enable);
            return 0;
        }
        if (idStr == "IsFriendlyFire")
        {
            g_lua_settop(L, top);
            g_lua_pushnumber(L, ::Get_FriendlyFire() ? 1.0 : 0.0);
            return 1;
        }

        if (idStr == "AssignInterrogationWithVoice")
        {
            bool hasEvent = false;
            const std::uint32_t ev =
                ReadCommandFnvHash(L, 2, "soundDialogueEvent", &hasEvent);
            g_lua_settop(L, top);

            ::Arm_CautionCpCapture();
            const int r = g_OrigSendCommand(L);
            const std::uint32_t cp = ::Take_CautionCpIndex();

            if (hasEvent)
            {
                if (cp == 0xFFFFFFFFu)
                    Log("[InterrogationVoice] ERROR: AssignInterrogationWithVoice did not reach the "
                        "Command Post dispatcher, so its CP index could not be captured - "
                        "soundDialogueEvent 0x%X was NOT registered and this CP keeps the vanilla "
                        "interrogation voice.\n", ev);
                else
                    ::Register_InterrogationVoiceEvent(cp, ev);
            }
            return r;
        }

        return g_OrigSendCommand(L);
    }
}

bool Install_GameObjectSendCommand_Hook()
{
    if (g_Installed) return true;

    if (!gAddr.GameObject_SendCommand)
    {
        LogDebug("[GameObjectSendCommand] address is 0 (unsupported build)\n");
        return false;
    }

    void* target = ResolveGameAddress(gAddr.GameObject_SendCommand);
    if (!target)
    {
        Log("[GameObjectSendCommand] resolve failed\n");
        return false;
    }

    if (!ResolveLuaApi())
    {
        Log("[GameObjectSendCommand] lua API resolve failed; aborting install\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hk_SendCommand),
        reinterpret_cast<void**>(&g_OrigSendCommand));

    if (ok)
    {
        g_Installed = true;
#ifdef _DEBUG
        LogDebug("[GameObjectSendCommand] hook installed @ %p (orig=%p)\n",
            target, reinterpret_cast<void*>(g_OrigSendCommand));
#endif
    }
    else
    {
        Log("[GameObjectSendCommand] hook install FAILED @ %p\n", target);
    }
    return ok;
}

bool Uninstall_GameObjectSendCommand_Hook()
{
    if (!g_Installed) return true;
    void* target = ResolveGameAddress(gAddr.GameObject_SendCommand);
    if (target) DisableAndRemoveHook(target);
    g_OrigSendCommand = nullptr;
    g_Installed       = false;
    return true;
}
