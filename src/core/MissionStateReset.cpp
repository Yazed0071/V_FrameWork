#include "pch.h"

#include <atomic>
#include <cstdint>

#include "LuaBroadcaster.h"
#include "MissionCodeGuard.h"
#include "MissionStateReset.h"
#include "../hooks/player/QuietCqcPatches.h"
#include "../hooks/outfit/UniqueCharacterSuitSlot.h"
#include "log.h"
#include "DeployGuard.h"

#include "../lua/LuaApi.h"

#include "../hooks/bullet/Bullet3Impl_ActivateBulletAtEmptyWorkPatch.h"
#include "../hooks/outfit/AdditionalMotionTable_GetMtarPathId.h"
#include "../hooks/menupopup/MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter.h"
#include "../hooks/outfit/OutfitAbilities.h"
#include "../hooks/player/CqcActionPluginImpl_StateHoldMove.h"
#include "../hooks/sahelan/RealizedSahelanFovaHook.h"
#include "../hooks/sahelan/SetEyeLampColorHook.h"
#include "../hooks/soldier/ActionCoreImpl_UpdateOptCamo.h"
#include "../hooks/soldier/CautionStepNormalTimerHook.h"
#include "../hooks/soldier/GetVoiceParamWithCallSign.h"
#include "../hooks/soldier/InterrogationVoiceEvent.h"
#include "../hooks/soldier/LostHostageHook.h"
#include "../hooks/soldier/SoldierVehicleAvoid.h"
#include "../hooks/soldier/NoticeControllerImpl_CheckSightNoticePlayer.h"
#include "../hooks/soldier/NoticeControllerImpl_GetOccasionalChat.h"
#include "../hooks/soldier/SoldierAkObjIdMap.h"
#include "../hooks/soldier/SoldierObjectRtpc.h"
#include "../hooks/soldier/StepRadioDiscovery.h"
#include "../hooks/soldier/VIPHoldupHook.h"
#include "../hooks/soldier/VIPRadioHook.h"
#include "../hooks/soldier/VIPSleepFaintHook.h"
#include "../hooks/ui/EnemyLangIdOverride.h"
#include "../hooks/ui/HeadMarkMarkerEvCall_SetIconSubType.h"

namespace
{
    std::atomic<std::uint32_t> g_epochMissionCode{ 0xFFFFFFFFu };
    std::atomic<bool>          g_finalizerRegistered{ false };
    std::atomic<bool>          g_finalizerFired{ false };
    std::atomic<bool>          g_broadcastPending{ false };

    void RunAllClears()
    {
        outfit::Reset_PartsTypeChangeTracking();
        Unset_AllSoldierVoicePitch();
        SoldierAkObjIdMap::ClearCommandPostVoiceCents();
        Clear_AllCautionStepNormalDurationOverrides();
        Clear_SoldierCallSigns();
        Clear_UpdateOptCamoMappedIndexOverrides();
        EnemyLangId_ClearMapOverride();
        EnemyLangId_ClearBinoOverride();
        EnemyLangId_ClearAllMapOverridesForSoldier();
        EnemyLangId_ClearAllBinoOverridesForSoldier();
        Clear_LostHostagesTrap();
        Clear_LostHostageDiscovery();
        Clear_SahelanFovaOverride();
        Clear_EyeLampColor();
        Clear_HeartLightColor();
        Clear_InterrogationVoiceEvents();
        ClearOccasionalChatListOverride();
        Clear_VIPSleepFaintImportantGameObjectIds();
        Clear_VIPHoldupImportantGameObjectIds();
        Reset_CustomNonVipRecoveryTracking();
        Clear_VIPRadioImportantGameObjectIds();
        Clear_AllSoldierIgnorePlayer();
        Clear_AllSoldierIgnoreVehicle();
        Clear_TargetCqcStance();
        Clear_HeadMarkEntityColours();
        Clear_AllMissionDeployWarnings();
        Set_FriendlyFire(false);
        outfit::RevertAdditionalMotionSwaps();

        g_broadcastPending.store(true, std::memory_order_relaxed);
    }

    int __cdecl l_MissionStateFinalizer(lua_State* L)
    {
        UNREFERENCED_PARAMETER(L);

        const std::uint32_t code = MissionCodeGuard::GetCurrentMissionCode();

        g_finalizerFired.store(true, std::memory_order_relaxed);
        g_epochMissionCode.store(code, std::memory_order_relaxed);
        RunAllClears();

        LogDebug("[MissionStateReset] mission %u teardown: cleared all per-mission "
                 "overrides via the mission finalizer\n",
            code);
        return 0;
    }
}

void MissionStateReset::EnsureFinalizerRegistered(lua_State* L)
{
    if (!L || g_finalizerRegistered.load(std::memory_order_relaxed))
        return;
    if (!ResolveLuaApi())
        return;

    const int top = g_lua_gettop(L);

    g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>("Mission"));
    if (g_lua_type(L, -1) != LUA_TTABLE)
    {
        g_lua_settop(L, top);
        return;
    }

    g_lua_pushstring(L, const_cast<char*>("AddResidentFinalizer"));
    g_lua_gettable(L, -2);
    if (g_lua_type(L, -1) != LUA_TFUNCTION)
    {
        g_lua_settop(L, top);
        return;
    }

    g_lua_pushcclosure(L, &l_MissionStateFinalizer, 0);
    const int rc = g_lua_pcall(L, 1, 0, 0);
    g_lua_settop(L, top);

    g_finalizerRegistered.store(true, std::memory_order_relaxed);

    if (rc != 0)
        Log("[MissionStateReset] ERROR: Mission.AddResidentFinalizer refused the "
            "per-mission reset finalizer (lua error %d) - overrides now depend on "
            "the mission-code poll, which cannot clear a mission that issues no "
            "GameObject.SendCommand\n", rc);
}

void MissionStateReset::PollMissionChange()
{
    const std::uint32_t code = MissionCodeGuard::GetCurrentMissionCode();
    DeployGuard::OnMissionCode(code);
    QuietCqc_EnforceMissionGuard();
    UniqueCharacterSuitSlot_EnforceMissionGuard();

    if (g_broadcastPending.exchange(false, std::memory_order_relaxed))
        V_FrameWork::EmitMessage("Mission", "MissionStateReset", code);

    if (g_finalizerFired.load(std::memory_order_relaxed))
    {
        g_epochMissionCode.store(code, std::memory_order_relaxed);
        return;
    }

    std::uint32_t prev = g_epochMissionCode.load(std::memory_order_relaxed);
    if (code == prev)
        return;
    if (!g_epochMissionCode.compare_exchange_strong(prev, code, std::memory_order_relaxed))
        return;

    RunAllClears();

    if (prev != 0xFFFFFFFFu)
        LogDebug("[MissionStateReset] mission %u -> %u: cleared all per-mission "
                 "overrides via the mission-code poll (the finalizer has not fired "
                 "yet)\n",
            prev, code);
}
