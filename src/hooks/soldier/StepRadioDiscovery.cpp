
#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <array>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "NoticeControllerImpl_CheckSightNoticePlayer.h"
#include "AddressSet.h"

namespace
{
    using CheckSightNoticeHostage_t = void(__fastcall*)(void* self, std::uint32_t soldierIndex, std::uint64_t param3, void* tracker);
    using StepRadioDiscovery_t = void(__fastcall*)(void* self, std::uint32_t soldierIndex, int step, void* info);

    static constexpr std::uint16_t LHD_INVALID_TARGET_ID = 0xFFFFu;
    static constexpr std::uint32_t LHD_INVALID_SOLDIER_ID = 0xFFFFFFFFu;
    static constexpr std::size_t   LHD_TRACKED_SLOT_COUNT = 3u;

    static constexpr std::uint8_t LHD_RADIO_TYPE_HOSTAGE_FOUND_A = 0x12u;
    static constexpr std::uint8_t LHD_RADIO_TYPE_HOSTAGE_FOUND_B = 0x0Bu;
    static constexpr std::uint8_t LHD_RADIO_TYPE_HOSTAGE_FOUND_C = 0x25u;
    static constexpr std::uint8_t LHD_RADIO_TYPE_HOSTAGE_FOUND_D = 0x0Cu;

    static constexpr std::uint32_t LHD_LABEL_MALE = 0x247b3defu;
    static constexpr std::uint32_t LHD_LABEL_FEMALE = 0xa2c4a6ecu;
    static constexpr std::uint32_t LHD_LABEL_CHILD = 0x100eb512u;

    static CheckSightNoticeHostage_t g_LHD_OrigCheckSightNoticeHostage = nullptr;
    static StepRadioDiscovery_t      g_LHD_OrigStepRadioDiscovery = nullptr;

    struct LostHostageDiscoveryInfo
    {
        std::uint16_t rawGameObjectId = LHD_INVALID_TARGET_ID;
        int           hostageType = -1;
        std::uint32_t speechLabel = 0u;
    };

    struct LostHostageDiscoveryRadioRequestEntryView
    {
        std::uintptr_t objectPtr = 0;
        std::uint32_t  speakerSoldierIndex = LHD_INVALID_SOLDIER_ID;
        std::uint16_t  word0C = 0xFFFFu;
        std::uint16_t  word0E = 0xFFFFu;
        std::uint8_t   byte10 = 0xFFu;
        std::uint8_t   byte12 = 0xFFu;
        std::uint8_t   byte13 = 0xFFu;
        std::uint8_t   byte15 = 0xFFu;
    };

    struct LostHostageDiscoverySelectedRadio
    {
        bool           active = false;
        std::uint32_t  soldierIndex = LHD_INVALID_SOLDIER_ID;
        std::uint16_t  targetId = LHD_INVALID_TARGET_ID;
        int            hostageType = -1;
        std::uint8_t   radioType = 0xFFu;
        std::uint32_t  overrideLabel = 0u;
    };

    static std::unordered_map<std::uint16_t, LostHostageDiscoveryInfo> g_LHD_TrackedHostagesByRawId;
    std::unordered_map<std::uint32_t, std::uint16_t> g_LHD_LastCandidateTargetBySoldier;
    std::unordered_map<std::uint32_t, std::uint16_t> g_LHD_LastConfirmedTargetBySoldier;
    static std::mutex g_LHD_Mutex;
    static std::unordered_map<std::uint32_t, LostHostageDiscoverySelectedRadio> g_LHD_PendingBySoldier;
    static LostHostageDiscoverySelectedRadio g_LHD_NextConvertOverride{};
}

static const char* LostHostageDiscovery_HostageTypeName(int hostageType)
{
    switch (hostageType)
    {
    case 0:  return "MALE";
    case 1:  return "FEMALE";
    case 2:  return "CHILD";
    default: return "UNKNOWN";
    }
}

static bool LostHostageDiscovery_SafeReadQword(std::uintptr_t addr, std::uint64_t& outValue)
{
    if (!addr) return false;
    __try
    {
        outValue = *reinterpret_cast<const std::uint64_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool LostHostageDiscovery_SafeReadInt(std::uintptr_t addr, int& outValue)
{
    if (!addr) return false;
    __try
    {
        outValue = *reinterpret_cast<const int*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool LostHostageDiscovery_SafeReadU16(std::uintptr_t addr, std::uint16_t& outValue)
{
    if (!addr) return false;
    __try
    {
        outValue = *reinterpret_cast<const std::uint16_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool LostHostageDiscovery_SafeReadU8(std::uintptr_t addr, std::uint8_t& outValue)
{
    if (!addr) return false;
    __try
    {
        outValue = *reinterpret_cast<const std::uint8_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static std::uint32_t LostHostageDiscovery_GetLabelForHostageType(int hostageType)
{
    switch (hostageType)
    {
    case 0:  return LHD_LABEL_MALE;
    case 1:  return LHD_LABEL_FEMALE;
    case 2:  return LHD_LABEL_CHILD;
    default: return 0u;
    }
}

static bool LostHostageDiscovery_IsHostageDiscoveryRadioType(std::uint8_t radioType)
{
    return radioType == LHD_RADIO_TYPE_HOSTAGE_FOUND_A ||
        radioType == LHD_RADIO_TYPE_HOSTAGE_FOUND_B ||
        radioType == LHD_RADIO_TYPE_HOSTAGE_FOUND_C ||
        radioType == LHD_RADIO_TYPE_HOSTAGE_FOUND_D;
}

static void LostHostageDiscovery_ResetRuntimeState_NoLock()
{
    g_LHD_LastCandidateTargetBySoldier.clear();
    g_LHD_LastConfirmedTargetBySoldier.clear();
    g_LHD_PendingBySoldier.clear();
    g_LHD_NextConvertOverride = {};
}

static bool LostHostageDiscovery_TryReadTrackedTargetIds(
    void* tracker,
    std::uint16_t(&outIds)[LHD_TRACKED_SLOT_COUNT])
{
    if (!tracker) return false;

    for (std::size_t i = 0; i < LHD_TRACKED_SLOT_COUNT; ++i)
    {
        if (!LostHostageDiscovery_SafeReadU16(
            reinterpret_cast<std::uintptr_t>(tracker) + 0x88ull + (i * 2ull),
            outIds[i]))
        {
            return false;
        }
    }

    return true;
}

static std::uint16_t LostHostageDiscovery_ChooseCandidateIdFromTracker(
    const std::uint16_t(&beforeIds)[LHD_TRACKED_SLOT_COUNT],
    const std::uint16_t(&afterIds)[LHD_TRACKED_SLOT_COUNT])
{
    for (std::size_t i = 0; i < LHD_TRACKED_SLOT_COUNT; ++i)
    {
        const std::uint16_t afterId = afterIds[i];
        if (afterId == LHD_INVALID_TARGET_ID) continue;

        bool existedBefore = false;
        for (std::size_t j = 0; j < LHD_TRACKED_SLOT_COUNT; ++j)
        {
            if (beforeIds[j] == afterId)
            {
                existedBefore = true;
                break;
            }
        }

        if (!existedBefore)
            return afterId;
    }

    for (std::size_t i = 0; i < LHD_TRACKED_SLOT_COUNT; ++i)
    {
        if (afterIds[i] != LHD_INVALID_TARGET_ID)
            return afterIds[i];
    }

    return LHD_INVALID_TARGET_ID;
}

static std::uint16_t LostHostageDiscovery_GetCurrentTargetForSoldier_NoLock(std::uint32_t soldierIndex)
{
    const auto itConfirmed = g_LHD_LastConfirmedTargetBySoldier.find(soldierIndex);
    if (itConfirmed != g_LHD_LastConfirmedTargetBySoldier.end() &&
        itConfirmed->second != LHD_INVALID_TARGET_ID)
    {
        return itConfirmed->second;
    }

    const auto itCandidate = g_LHD_LastCandidateTargetBySoldier.find(soldierIndex);
    if (itCandidate != g_LHD_LastCandidateTargetBySoldier.end() &&
        itCandidate->second != LHD_INVALID_TARGET_ID)
    {
        return itCandidate->second;
    }

    return LHD_INVALID_TARGET_ID;
}

static bool LostHostageDiscovery_TryGetTrackedHostageInfo_NoLock(
    std::uint16_t targetId,
    LostHostageDiscoveryInfo& outInfo)
{
    const auto it = g_LHD_TrackedHostagesByRawId.find(targetId);
    if (it == g_LHD_TrackedHostagesByRawId.end())
        return false;

    outInfo = it->second;
    return true;
}

static bool LostHostageDiscovery_TryReadRadioRequestEntry(
    void* self,
    int actionIndex,
    LostHostageDiscoveryRadioRequestEntryView& outView)
{
    outView = {};
    if (!self) return false;

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(self);

    std::uint64_t listBase = 0;
    int baseActionIndex = 0;

    if (!LostHostageDiscovery_SafeReadQword(base + 0x88ull, listBase) || listBase == 0)
        return false;

    if (!LostHostageDiscovery_SafeReadInt(base + 0x90ull, baseActionIndex))
        return false;

    const int relativeIndex = actionIndex - baseActionIndex;
    if (relativeIndex < 0)
        return false;

    const std::uintptr_t entry =
        static_cast<std::uintptr_t>(listBase) +
        (static_cast<std::uintptr_t>(relativeIndex) * 0x18ull);

    std::uint64_t objectPtr = 0;
    int speakerSoldierIndex = -1;

    LostHostageDiscovery_SafeReadQword(entry + 0x0ull, objectPtr);
    LostHostageDiscovery_SafeReadInt(entry + 0x8ull, speakerSoldierIndex);
    LostHostageDiscovery_SafeReadU16(entry + 0xCull, outView.word0C);
    LostHostageDiscovery_SafeReadU16(entry + 0xEull, outView.word0E);
    LostHostageDiscovery_SafeReadU8(entry + 0x10ull, outView.byte10);
    LostHostageDiscovery_SafeReadU8(entry + 0x12ull, outView.byte12);
    LostHostageDiscovery_SafeReadU8(entry + 0x13ull, outView.byte13);
    LostHostageDiscovery_SafeReadU8(entry + 0x15ull, outView.byte15);

    outView.objectPtr = static_cast<std::uintptr_t>(objectPtr);
    outView.speakerSoldierIndex =
        (speakerSoldierIndex < 0)
        ? LHD_INVALID_SOLDIER_ID
        : static_cast<std::uint32_t>(speakerSoldierIndex);

    return true;
}

static void __fastcall hkLostHostageDiscovery_CheckSightNoticeHostage(
    void* self,
    std::uint32_t soldierIndex,
    std::uint64_t param3,
    void* tracker)
{
    if (!g_LHD_OrigCheckSightNoticeHostage)
        return;

    if (MissionCodeGuard::ShouldBypassHooks())
    {
        g_LHD_OrigCheckSightNoticeHostage(self, soldierIndex, param3, tracker);
        return;
    }

    if (Soldier_IgnoresNotice(soldierIndex, SoldierNoticeIgnore::kHostage))
        return;

    std::uint16_t beforeIds[LHD_TRACKED_SLOT_COUNT] =
    {
        LHD_INVALID_TARGET_ID, LHD_INVALID_TARGET_ID, LHD_INVALID_TARGET_ID
    };

    std::uint16_t afterIds[LHD_TRACKED_SLOT_COUNT] =
    {
        LHD_INVALID_TARGET_ID, LHD_INVALID_TARGET_ID, LHD_INVALID_TARGET_ID
    };

    LostHostageDiscovery_TryReadTrackedTargetIds(tracker, beforeIds);

    g_LHD_OrigCheckSightNoticeHostage(self, soldierIndex, param3, tracker);

    if (!LostHostageDiscovery_TryReadTrackedTargetIds(tracker, afterIds))
        return;

    const std::uint16_t chosenCandidate =
        LostHostageDiscovery_ChooseCandidateIdFromTracker(beforeIds, afterIds);

    if (chosenCandidate == LHD_INVALID_TARGET_ID)
        return;

    std::lock_guard<std::mutex> lock(g_LHD_Mutex);

    g_LHD_LastCandidateTargetBySoldier[soldierIndex] = chosenCandidate;
}

static void __fastcall hkLostHostageDiscovery_StepRadioDiscovery(
    void* self,
    std::uint32_t soldierIndex,
    int step,
    void* info)
{
    if (!g_LHD_OrigStepRadioDiscovery)
        return;

    if (MissionCodeGuard::ShouldBypassHooks())
    {
        g_LHD_OrigStepRadioDiscovery(self, soldierIndex, step, info);
        return;
    }

    if ((step == 0 || step == 4) && info != nullptr)
    {
        std::uint16_t targetId = LHD_INVALID_TARGET_ID;

        LostHostageDiscovery_SafeReadU16(reinterpret_cast<std::uintptr_t>(info) + 0x26ull, targetId);

        if (targetId != LHD_INVALID_TARGET_ID)
        {
            std::lock_guard<std::mutex> lock(g_LHD_Mutex);

            g_LHD_LastConfirmedTargetBySoldier[soldierIndex] = targetId;
        }
    }

    g_LHD_OrigStepRadioDiscovery(self, soldierIndex, step, info);
}

void LostHostageDiscovery_OnRadioRequest(void* self, int actionIndex, int stateProc)
{
    if (stateProc != 0)
        return;

    LostHostageDiscoveryRadioRequestEntryView after{};

    if (!LostHostageDiscovery_TryReadRadioRequestEntry(self, actionIndex, after))
        return;

    if (!LostHostageDiscovery_IsHostageDiscoveryRadioType(after.byte10))
        return;

    if (after.speakerSoldierIndex == LHD_INVALID_SOLDIER_ID)
        return;

    std::lock_guard<std::mutex> lock(g_LHD_Mutex);

    const std::uint16_t targetId =
        LostHostageDiscovery_GetCurrentTargetForSoldier_NoLock(after.speakerSoldierIndex);

    LostHostageDiscoveryInfo hostageInfo{};
    if (!LostHostageDiscovery_TryGetTrackedHostageInfo_NoLock(targetId, hostageInfo))
    {
        return;
    }

    if (hostageInfo.speechLabel == 0u)
        return;

    LostHostageDiscoverySelectedRadio pending{};
    pending.active = true;
    pending.soldierIndex = after.speakerSoldierIndex;
    pending.targetId = targetId;
    pending.hostageType = hostageInfo.hostageType;
    pending.radioType = after.byte10;
    pending.overrideLabel = hostageInfo.speechLabel;

    g_LHD_PendingBySoldier[pending.soldierIndex] = pending;
    g_LHD_NextConvertOverride = pending;
}

bool LostHostageDiscovery_TryConsumeConvertOverride(
    std::uint8_t radioType,
    std::uint32_t& outOverrideLabel)
{
    outOverrideLabel = 0u;

    if (!LostHostageDiscovery_IsHostageDiscoveryRadioType(radioType))
        return false;

    std::lock_guard<std::mutex> lock(g_LHD_Mutex);

    if (!g_LHD_NextConvertOverride.active)
        return false;

    if (g_LHD_NextConvertOverride.radioType != radioType)
        return false;

    if (g_LHD_NextConvertOverride.overrideLabel == 0u)
    {
        g_LHD_NextConvertOverride = {};
        return false;
    }

    outOverrideLabel = g_LHD_NextConvertOverride.overrideLabel;

    g_LHD_PendingBySoldier.erase(g_LHD_NextConvertOverride.soldierIndex);
    g_LHD_NextConvertOverride = {};
    return true;
}
bool LostHostageDiscovery_TryOverrideForCallWithRadioType(
    std::uint32_t ownerIndex,
    std::uint8_t radioType,
    std::uint32_t& outOverrideLabel)
{
    outOverrideLabel = 0u;

    if (!LostHostageDiscovery_IsHostageDiscoveryRadioType(radioType))
        return false;

    std::lock_guard<std::mutex> lock(g_LHD_Mutex);

    const auto it = g_LHD_PendingBySoldier.find(ownerIndex);
    if (it == g_LHD_PendingBySoldier.end())
        return false;

    const LostHostageDiscoverySelectedRadio selected = it->second;
    g_LHD_PendingBySoldier.erase(it);

    if (!selected.active || selected.overrideLabel == 0u)
        return false;

    outOverrideLabel = selected.overrideLabel;
    return true;
}

void Add_LostHostageDiscovery(std::uint32_t gameObjectId, int hostageType)
{
    if (hostageType < 0 || hostageType > 2)
    {
        Log(
            "[LostHostageDiscovery] Add ignored: invalid hostageType=%d gameObjectId=0x%08X\n",
            hostageType,
            gameObjectId);
        return;
    }

    const std::uint16_t rawGameObjectId = static_cast<std::uint16_t>(gameObjectId);
    const std::uint32_t speechLabel = LostHostageDiscovery_GetLabelForHostageType(hostageType);

    if (rawGameObjectId == LHD_INVALID_TARGET_ID || speechLabel == 0u)
    {
        Log(
            "[LostHostageDiscovery] Add ignored: rawGameObjectId=0x%04X speechLabel=0x%08X\n",
            static_cast<unsigned>(rawGameObjectId),
            static_cast<unsigned>(speechLabel));
        return;
    }

    LostHostageDiscoveryInfo info{};
    info.rawGameObjectId = rawGameObjectId;
    info.hostageType = hostageType;
    info.speechLabel = speechLabel;

    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        g_LHD_TrackedHostagesByRawId[rawGameObjectId] = info;
    }
}

void Remove_LostHostageDiscovery(std::uint32_t gameObjectId)
{
    const std::uint16_t rawGameObjectId = static_cast<std::uint16_t>(gameObjectId);

    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        g_LHD_TrackedHostagesByRawId.erase(rawGameObjectId);

        for (auto it = g_LHD_PendingBySoldier.begin();
            it != g_LHD_PendingBySoldier.end();)
        {
            if (it->second.targetId == rawGameObjectId)
                it = g_LHD_PendingBySoldier.erase(it);
            else
                ++it;
        }
    }
}

void Clear_LostHostageDiscovery()
{
    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        g_LHD_TrackedHostagesByRawId.clear();
        LostHostageDiscovery_ResetRuntimeState_NoLock();
    }
}

void Dump_LostHostageDiscovery()
{
    std::lock_guard<std::mutex> lock(g_LHD_Mutex);

    for (const auto& kv : g_LHD_TrackedHostagesByRawId)
    {
        const LostHostageDiscoveryInfo& info = kv.second;
    }
}

bool Install_LostHostageDiscovery_Hooks()
{
    void* checkSightTarget = ResolveGameAddress(gAddr.CheckSightNoticeHostage);
    void* stepTarget = ResolveGameAddress(gAddr.StepRadioDiscovery);

    if (!checkSightTarget || !stepTarget)
    {
        Log(
            "[LostHostageDiscovery] Install failed: checkSight=%p step=%p\n",
            checkSightTarget,
            stepTarget);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        LostHostageDiscovery_ResetRuntimeState_NoLock();
    }

    const bool okCheckSight = CreateAndEnableHook(
        checkSightTarget,
        reinterpret_cast<void*>(&hkLostHostageDiscovery_CheckSightNoticeHostage),
        reinterpret_cast<void**>(&g_LHD_OrigCheckSightNoticeHostage));

    const bool okStep = CreateAndEnableHook(
        stepTarget,
        reinterpret_cast<void*>(&hkLostHostageDiscovery_StepRadioDiscovery),
        reinterpret_cast<void**>(&g_LHD_OrigStepRadioDiscovery));

#ifdef _DEBUG
    Log("[LostHostageDiscovery] Install CheckSightNoticeHostage: %s\n", okCheckSight ? "OK" : "FAIL");
    Log("[LostHostageDiscovery] Install StepRadioDiscovery: %s\n", okStep ? "OK" : "FAIL");
#else
    if (!okCheckSight)
        Log("[LostHostageDiscovery] Install CheckSightNoticeHostage: %s\n", okCheckSight ? "OK" : "FAIL");
    if (!okStep)
        Log("[LostHostageDiscovery] Install StepRadioDiscovery: %s\n", okStep ? "OK" : "FAIL");
#endif

    return okCheckSight && okStep;
}

bool Uninstall_LostHostageDiscovery_Hooks()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.CheckSightNoticeHostage));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.StepRadioDiscovery));

    g_LHD_OrigCheckSightNoticeHostage = nullptr;
    g_LHD_OrigStepRadioDiscovery = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        g_LHD_TrackedHostagesByRawId.clear();
        LostHostageDiscovery_ResetRuntimeState_NoLock();
    }

#ifdef _DEBUG
    LogDebug("[LostHostageDiscovery] removed\n");
#endif
    return true;
}
