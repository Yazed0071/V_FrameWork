
#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <array>
#include <deque>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"

namespace
{
    using CheckSightNoticeHostage_t = void(__fastcall*)(void* self, std::uint32_t soldierIndex, std::uint64_t param3, void* tracker);
    using StepRadioDiscovery_t = void(__fastcall*)(void* self, std::uint32_t soldierIndex, int step, void* info);

    static constexpr std::uintptr_t ABS_CheckSightNoticeHostage = 0x1414E1090ull;
    static constexpr std::uintptr_t ABS_StepRadioDiscovery = 0x14150F2C0ull;

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
    static thread_local std::deque<LostHostageDiscoverySelectedRadio> g_LHD_SelectedRadioQueue;
}

#ifdef _DEBUG
#define LHD_LOG(...) do { std::printf("[LostHostageDiscovery] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)
#else
#define LHD_LOG(...) do {} while (0)
#endif

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
    g_LHD_SelectedRadioQueue.clear();
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

    const auto it = g_LHD_LastCandidateTargetBySoldier.find(soldierIndex);
    const std::uint16_t previous =
        (it != g_LHD_LastCandidateTargetBySoldier.end())
        ? it->second
        : LHD_INVALID_TARGET_ID;

    if (previous != chosenCandidate)
    {
        LHD_LOG(
            "CheckSightNoticeHostage soldier=%u candidate=0x%04X before=[0x%04X,0x%04X,0x%04X] after=[0x%04X,0x%04X,0x%04X]",
            static_cast<unsigned>(soldierIndex),
            static_cast<unsigned>(chosenCandidate),
            static_cast<unsigned>(beforeIds[0]),
            static_cast<unsigned>(beforeIds[1]),
            static_cast<unsigned>(beforeIds[2]),
            static_cast<unsigned>(afterIds[0]),
            static_cast<unsigned>(afterIds[1]),
            static_cast<unsigned>(afterIds[2]));
    }

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
        std::uint16_t auxId = LHD_INVALID_TARGET_ID;
        std::uint8_t flags = 0xFFu;

        LostHostageDiscovery_SafeReadU16(reinterpret_cast<std::uintptr_t>(info) + 0x26ull, targetId);
        LostHostageDiscovery_SafeReadU16(reinterpret_cast<std::uintptr_t>(info) + 0x28ull, auxId);
        LostHostageDiscovery_SafeReadU8(reinterpret_cast<std::uintptr_t>(info) + 0x32ull, flags);

        if (targetId != LHD_INVALID_TARGET_ID)
        {
            std::lock_guard<std::mutex> lock(g_LHD_Mutex);

            const auto it = g_LHD_LastConfirmedTargetBySoldier.find(soldierIndex);
            const std::uint16_t previous =
                (it != g_LHD_LastConfirmedTargetBySoldier.end())
                ? it->second
                : LHD_INVALID_TARGET_ID;

            if (previous != targetId)
            {
                LHD_LOG(
                    "StepRadioDiscovery step=%d soldier=%u target=0x%04X aux=0x%04X flags=0x%02X",
                    step,
                    static_cast<unsigned>(soldierIndex),
                    static_cast<unsigned>(targetId),
                    static_cast<unsigned>(auxId),
                    static_cast<unsigned>(flags));
            }

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
        LHD_LOG(
            "OnRadioRequest no tracked hostage: soldier=%u radioType=0x%02X target=0x%04X stateProc=%d",
            static_cast<unsigned>(after.speakerSoldierIndex),
            static_cast<unsigned>(after.byte10),
            static_cast<unsigned>(targetId),
            stateProc);
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

    if (!g_LHD_SelectedRadioQueue.empty())
    {
        const auto& back = g_LHD_SelectedRadioQueue.back();
        if (back.soldierIndex == pending.soldierIndex &&
            back.targetId == pending.targetId &&
            back.radioType == pending.radioType &&
            back.overrideLabel == pending.overrideLabel)
        {
            LHD_LOG(
                "OnRadioRequest duplicate ignored: soldier=%u radioType=0x%02X target=0x%04X hostageType=%s label=0x%08X",
                static_cast<unsigned>(pending.soldierIndex),
                static_cast<unsigned>(pending.radioType),
                static_cast<unsigned>(pending.targetId),
                LostHostageDiscovery_HostageTypeName(pending.hostageType),
                static_cast<unsigned>(pending.overrideLabel));
            return;
        }
    }

    g_LHD_SelectedRadioQueue.push_back(pending);

    LHD_LOG(
        "OnRadioRequest queued override: soldier=%u radioType=0x%02X target=0x%04X hostageType=%s label=0x%08X stateProc=%d queueSize=%u",
        static_cast<unsigned>(pending.soldierIndex),
        static_cast<unsigned>(pending.radioType),
        static_cast<unsigned>(pending.targetId),
        LostHostageDiscovery_HostageTypeName(pending.hostageType),
        static_cast<unsigned>(pending.overrideLabel),
        stateProc,
        static_cast<unsigned>(g_LHD_SelectedRadioQueue.size()));
}
std::uint32_t LostHostageDiscovery_OnConvertRadioTypeToSpeechLabel(
    std::uint8_t radioType,
    std::uint32_t baseLabel)
{
    if (!LostHostageDiscovery_IsHostageDiscoveryRadioType(radioType))
        return baseLabel;

    if (g_LHD_SelectedRadioQueue.empty())
        return baseLabel;

    // FIFO consume so back-to-back hostage discoveries from different soldiers
    // do not overwrite each other before conversion happens.
    const LostHostageDiscoverySelectedRadio selected = g_LHD_SelectedRadioQueue.front();
    g_LHD_SelectedRadioQueue.pop_front();

    if (!selected.active || selected.overrideLabel == 0u)
        return baseLabel;

    LHD_LOG(
        "OnConvert override: radioType=0x%02X baseLabel=0x%08X overrideLabel=0x%08X soldier=%u target=0x%04X hostageType=%s remainingQueue=%u",
        static_cast<unsigned>(radioType),
        static_cast<unsigned>(baseLabel),
        static_cast<unsigned>(selected.overrideLabel),
        static_cast<unsigned>(selected.soldierIndex),
        static_cast<unsigned>(selected.targetId),
        LostHostageDiscovery_HostageTypeName(selected.hostageType),
        static_cast<unsigned>(g_LHD_SelectedRadioQueue.size()));

    return selected.overrideLabel;
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

    Log(
        "[LostHostageDiscovery] Added tracked hostage: inputGameObjectId=0x%08X rawGameObjectId=0x%04X hostageType=%s speechLabel=0x%08X\n",
        gameObjectId,
        static_cast<unsigned>(rawGameObjectId),
        LostHostageDiscovery_HostageTypeName(hostageType),
        static_cast<unsigned>(speechLabel));
}

void Remove_LostHostageDiscovery(std::uint32_t gameObjectId)
{
    const std::uint16_t rawGameObjectId = static_cast<std::uint16_t>(gameObjectId);

    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        g_LHD_TrackedHostagesByRawId.erase(rawGameObjectId);

        for (auto it = g_LHD_SelectedRadioQueue.begin();
            it != g_LHD_SelectedRadioQueue.end();)
        {
            if (it->targetId == rawGameObjectId)
                it = g_LHD_SelectedRadioQueue.erase(it);
            else
                ++it;
        }
    }

    Log(
        "[LostHostageDiscovery] Removed tracked hostage: inputGameObjectId=0x%08X rawGameObjectId=0x%04X\n",
        gameObjectId,
        static_cast<unsigned>(rawGameObjectId));
}

void Clear_LostHostageDiscovery()
{
    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        g_LHD_TrackedHostagesByRawId.clear();
        LostHostageDiscovery_ResetRuntimeState_NoLock();
    }

    Log("[LostHostageDiscovery] Cleared all tracked hostages\n");
}

void Dump_LostHostageDiscovery()
{
    std::lock_guard<std::mutex> lock(g_LHD_Mutex);

    Log(
        "[LostHostageDiscovery] Dump tracked hostages: count=%u\n",
        static_cast<unsigned>(g_LHD_TrackedHostagesByRawId.size()));

    for (const auto& kv : g_LHD_TrackedHostagesByRawId)
    {
        const LostHostageDiscoveryInfo& info = kv.second;
        Log(
            "[LostHostageDiscovery]   rawGameObjectId=0x%04X hostageType=%s speechLabel=0x%08X\n",
            static_cast<unsigned>(info.rawGameObjectId),
            LostHostageDiscovery_HostageTypeName(info.hostageType),
            static_cast<unsigned>(info.speechLabel));
    }
}

bool Install_LostHostageDiscovery_Hooks()
{
    void* checkSightTarget = ResolveGameAddress(ABS_CheckSightNoticeHostage);
    void* stepTarget = ResolveGameAddress(ABS_StepRadioDiscovery);

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

    Log("[LostHostageDiscovery] Install CheckSightNoticeHostage: %s\n", okCheckSight ? "OK" : "FAIL");
    Log("[LostHostageDiscovery] Install StepRadioDiscovery: %s\n", okStep ? "OK" : "FAIL");

    return okCheckSight && okStep;
}

bool Uninstall_LostHostageDiscovery_Hooks()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_CheckSightNoticeHostage));
    DisableAndRemoveHook(ResolveGameAddress(ABS_StepRadioDiscovery));

    g_LHD_OrigCheckSightNoticeHostage = nullptr;
    g_LHD_OrigStepRadioDiscovery = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_LHD_Mutex);
        g_LHD_TrackedHostagesByRawId.clear();
        LostHostageDiscovery_ResetRuntimeState_NoLock();
    }

    Log("[LostHostageDiscovery] removed\n");
    return true;
}
