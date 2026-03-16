#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "LostHostageHook.h"

namespace
{
    // Hook type for tpp::gm::TrapExecLostHostageCallback::ExecCallback.
    // Params: self (void*), trapInfo (void*)
    using ExecCallback_t = void(__fastcall*)(void* self, void* trapInfo);

    // Hook type for RadioSpeechHandlerImpl::CallPart.
    // Params: self (void*), ownerIndex (uint8_t)
    using CallPart_t = bool(__fastcall*)(void* self, std::uint8_t ownerIndex);

    // Game function type for MbDvcMapCallbackImpl::GetMapRequest.
    // Params: trapInfo (void*)
    using GetMapRequest_t = int(__fastcall*)(void* trapInfo);

    // Game function type for fox::geo::TrapInfo::GetMoverGameObjectId.
    // Params: trapInfo (void*), outGameObjectIdBuffer (uint16_t*)
    using GetMoverGameObjectId_t = void(__fastcall*)(void* trapInfo, std::uint16_t* outGameObjectIdBuffer);

    // Game function type for fox::geo::TrapInfo::GetTrapPosition.
    // Params: trapInfo (void*)
    using GetTrapPosition_t = const float* (__fastcall*)(void* trapInfo);

    // Game function type for fox::gm::GetNameIdWithGameObjectId.
    // Params: gameObjectId (uint32_t)
    using GetNameIdWithGameObjectId_t = int(__fastcall*)(std::uint32_t gameObjectId);

    // Game function type for fox::GetQuarkSystemTable.
    // Params: none
    using GetQuarkSystemTable_t = void* (__fastcall*)();

    // Virtual function type for the object-system lookup under QuarkSystemTable + 0x98.
    // Params: objectSystem98 (void*), partitionIndex (uint16_t)
    using GetPartitionObjectSet_t = void* (__fastcall*)(void* objectSystem98, std::uint16_t partitionIndex);

    // Virtual function type for objectSet + 0x30.
    // Params: objectSet (void*), localIndex (uint16_t), mask (uint64_t)
    using ObjectCheck30_t = char(__fastcall*)(void* objectSet, std::uint16_t localIndex, std::uint64_t mask);

    // Virtual function type for objectSet + 0x38.
    // Params: objectSet (void*), localIndex (uint16_t)
    using ObjectCheck38_t = char(__fastcall*)(void* objectSet, std::uint16_t localIndex);

    // Internal game function type for RadioSpeechHandlerImpl::GetVoiceParamWithCallSign.
    // Params: selfMinus20 (void*), ownerIndex (uint32_t)
    using GetVoiceParamWithCallSign_t = std::uint64_t(__fastcall*)(void* selfMinus20, std::uint32_t ownerIndex);

    // Virtual function type for (self + 8)->vfunc[0x98].
    // Params: callPartContext8 (void*), ownerIndex (uint32_t)
    using GetSpeechLabel98_t = int(__fastcall*)(void* callPartContext8, std::uint32_t ownerIndex);

    // Virtual function type for (self + 8)->vfunc[0xB0].
    // Params: callPartContext8 (void*), ownerIndex (uint32_t)
    using GetPriorityB0_t = int(__fastcall*)(void* callPartContext8, std::uint32_t ownerIndex);

    // Virtual function type for (self + 8)->vfunc[0xB8].
    // Params: callPartContext8 (void*), ownerIndex (uint32_t)
    using GetVoiceTypeB8_t = std::uint32_t(__fastcall*)(void* callPartContext8, std::uint32_t ownerIndex);

    // Absolute address of TrapExecLostHostageCallback::ExecCallback.
    static constexpr std::uintptr_t ABS_ExecCallback = 0x140A19030ull;

    // Absolute address of RadioSpeechHandlerImpl::CallPart.
    static constexpr std::uintptr_t ABS_CallPart = 0x140DA2AA0ull;

    // Absolute address of MbDvcMapCallbackImpl::GetMapRequest.
    static constexpr std::uintptr_t ABS_GetMapRequest = 0x140F29180ull;

    // Absolute address of fox::geo::TrapInfo::GetMoverGameObjectId.
    static constexpr std::uintptr_t ABS_GetMoverGameObjectId = 0x141BA0200ull;

    // Absolute address of fox::geo::TrapInfo::GetTrapPosition.
    static constexpr std::uintptr_t ABS_GetTrapPosition = 0x141BA01F0ull;

    // Absolute address of fox::gm::GetNameIdWithGameObjectId.
    static constexpr std::uintptr_t ABS_GetNameIdWithGameObjectId = 0x140BF4570ull;

    // Absolute address of fox::GetQuarkSystemTable.
    static constexpr std::uintptr_t ABS_GetQuarkSystemTable = 0x140BFF3F0ull;

    // Absolute address of RadioSpeechHandlerImpl::GetVoiceParamWithCallSign.
    static constexpr std::uintptr_t ABS_GetVoiceParamWithCallSign = 0x140DA3170ull;

    static constexpr std::uint32_t DEFAULT_SPEECH_LABEL = 0x6B52E59Fu;
    static constexpr std::uint32_t SPECIAL_VOICE_PARAM = 0xC63B015Fu;
    static constexpr std::uint32_t SPECIAL_SPEECH_LABEL = 0xF0DFCC07u;

    static constexpr ULONGLONG LOST_HOSTAGE_PENDING_TIMEOUT_MS = 10000ull;

    static ExecCallback_t g_OrigExecCallback = nullptr;
    static CallPart_t g_OrigCallPart = nullptr;

    static GetMapRequest_t g_GetMapRequest = nullptr;
    static GetMoverGameObjectId_t g_GetMoverGameObjectId = nullptr;
    static GetTrapPosition_t g_GetTrapPosition = nullptr;
    static GetNameIdWithGameObjectId_t g_GetNameIdWithGameObjectId = nullptr;
    static GetQuarkSystemTable_t g_GetQuarkSystemTable = nullptr;
    static GetVoiceParamWithCallSign_t g_GetVoiceParamWithCallSign = nullptr;

    // Hostage types:
    // 0 = male
    // 1 = female
    // 2 = child
    struct LostHostageInfo
    {
        int hostageType = 0;
    };

    struct PendingLostHostageReport
    {
        bool active = false;
        std::uint16_t hostageGameObjectId = 0xFFFFu;
        int hostageNameId = -1;
        int hostageType = -1;
        int requestType = -1;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        ULONGLONG armedAtTickMs = 0;
    };

    struct LostHostageTrapEval
    {
        bool valid = false;

        int mapRequest = -1;

        std::uint16_t moverGameObjectId = 0xFFFFu;
        int moverNameId = -1;

        int expectedNameId = -1;
        bool nameMatches = false;

        std::uint16_t partitionIndex = 0xFFFFu;
        std::uint16_t localIndex = 0xFFFFu;

        void* objectSet = nullptr;

        bool hasCheck30 = false;
        bool check30 = false;

        bool hasCheck38 = false;
        bool check38 = false;

        bool hasTrapPosition = false;
        float trapX = 0.0f;
        float trapY = 0.0f;
        float trapZ = 0.0f;

        bool request1WouldRun = false;
        bool request2WouldRun = false;
    };

    struct CallPartSnapshot
    {
        bool valid = false;

        std::uint8_t ownerIndex = 0;
        std::uint16_t speakerSlot = 0xFFFFu;

        std::uint8_t stateByte12 = 0;
        bool alreadyProcessed = false;
        std::uint8_t stage = 0;

        int rawSpeechLabel98 = 0;
        int effectiveSpeechLabel = 0;
        int priorityB0 = 0;
        std::uint32_t voiceTypeB8 = 0;
        std::uint32_t voiceParam = 0;
    };

    static std::unordered_map<std::uint16_t, LostHostageInfo> g_TrackedLostHostages;
    static PendingLostHostageReport g_PendingLostHostageReport;

    static std::uint32_t g_LostHostageSpeechLabels[3] = { 0, 0, 0 };

    static std::mutex g_LostHostageMutex;

    static std::uint64_t g_LastRadioLogKey = 0;
    static ULONGLONG g_LastRadioLogTickMs = 0;
}

// Returns YES or NO for logs.
// Params: value (bool)
static const char* YesNo(bool value)
{
    return value ? "YES" : "NO";
}

// Returns a readable hostage type name.
// Params: hostageType (int)
static const char* HostageTypeName(int hostageType)
{
    switch (hostageType)
    {
    case 0: return "MALE";
    case 1: return "FEMALE";
    case 2: return "CHILD";
    default: return "UNKNOWN";
    }
}

// Safely reads one qword from memory.
// Params: addr (uintptr_t), outValue (uint64_t&)
static bool SafeReadQword(std::uintptr_t addr, std::uint64_t& outValue)
{
    if (!addr)
        return false;

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

// Safely reads one int from memory.
// Params: addr (uintptr_t), outValue (int&)
static bool SafeReadInt(std::uintptr_t addr, int& outValue)
{
    if (!addr)
        return false;

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

// Safely reads one byte from memory.
// Params: addr (uintptr_t), outValue (uint8_t&)
static bool SafeReadByte(std::uintptr_t addr, std::uint8_t& outValue)
{
    if (!addr)
        return false;

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

// Safely reads one word from memory.
// Params: addr (uintptr_t), outValue (uint16_t&)
static bool SafeReadWord(std::uintptr_t addr, std::uint16_t& outValue)
{
    if (!addr)
        return false;

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

// Resolves helper functions used by this module.
// Params: none
static bool ResolveLostHostageHelpers()
{
    if (!g_GetMapRequest)
    {
        g_GetMapRequest = reinterpret_cast<GetMapRequest_t>(
            ResolveGameAddress(ABS_GetMapRequest));
    }

    if (!g_GetMoverGameObjectId)
    {
        g_GetMoverGameObjectId = reinterpret_cast<GetMoverGameObjectId_t>(
            ResolveGameAddress(ABS_GetMoverGameObjectId));
    }

    if (!g_GetTrapPosition)
    {
        g_GetTrapPosition = reinterpret_cast<GetTrapPosition_t>(
            ResolveGameAddress(ABS_GetTrapPosition));
    }

    if (!g_GetNameIdWithGameObjectId)
    {
        g_GetNameIdWithGameObjectId = reinterpret_cast<GetNameIdWithGameObjectId_t>(
            ResolveGameAddress(ABS_GetNameIdWithGameObjectId));
    }

    if (!g_GetQuarkSystemTable)
    {
        g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
            ResolveGameAddress(ABS_GetQuarkSystemTable));
    }

    if (!g_GetVoiceParamWithCallSign)
    {
        g_GetVoiceParamWithCallSign = reinterpret_cast<GetVoiceParamWithCallSign_t>(
            ResolveGameAddress(ABS_GetVoiceParamWithCallSign));
    }

    return
        g_GetMapRequest &&
        g_GetMoverGameObjectId &&
        g_GetTrapPosition &&
        g_GetNameIdWithGameObjectId &&
        g_GetQuarkSystemTable &&
        g_GetVoiceParamWithCallSign;
}

// Reads the expected hostage NameId from the callback object.
// Params: self (void*), outExpectedNameId (int&)
static bool TryGetExpectedNameId(void* self, int& outExpectedNameId)
{
    outExpectedNameId = -1;

    if (!self)
        return false;

    std::uint64_t callbackData = 0;
    if (!SafeReadQword(reinterpret_cast<std::uintptr_t>(self) + 0x10ull, callbackData) || callbackData == 0)
        return false;

    std::uint64_t callbackData40 = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(callbackData) + 0x40ull, callbackData40) || callbackData40 == 0)
        return false;

    return SafeReadInt(static_cast<std::uintptr_t>(callbackData40) + 0x10ull, outExpectedNameId);
}

// Resolves the object-set used by the trap callback for the mover GameObjectId.
// Params: moverGameObjectId (uint16_t), outObjectSet (void*&)
static bool TryGetPartitionObjectSet(std::uint16_t moverGameObjectId, void*& outObjectSet)
{
    outObjectSet = nullptr;

    if (!ResolveLostHostageHelpers())
        return false;

    void* quarkSystemTable = g_GetQuarkSystemTable();
    if (!quarkSystemTable)
        return false;

    std::uint64_t objectSystem98 = 0;
    if (!SafeReadQword(reinterpret_cast<std::uintptr_t>(quarkSystemTable) + 0x98ull, objectSystem98) || objectSystem98 == 0)
        return false;

    std::uint64_t vtbl = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(objectSystem98), vtbl) || vtbl == 0)
        return false;

    std::uint64_t fnAddr = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(vtbl) + 0x10ull, fnAddr) || fnAddr == 0)
        return false;

    const auto fn = reinterpret_cast<GetPartitionObjectSet_t>(fnAddr);

    __try
    {
        outObjectSet = fn(
            reinterpret_cast<void*>(objectSystem98),
            static_cast<std::uint16_t>(moverGameObjectId >> 9));

        return outObjectSet != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outObjectSet = nullptr;
        return false;
    }
}

// Calls objectSet virtual +0x30 safely.
// Params: objectSet (void*), localIndex (uint16_t), mask (uint64_t), outValue (bool&)
static bool TryCallObjectCheck30(void* objectSet, std::uint16_t localIndex, std::uint64_t mask, bool& outValue)
{
    outValue = false;

    if (!objectSet)
        return false;

    std::uint64_t vtbl = 0;
    if (!SafeReadQword(reinterpret_cast<std::uintptr_t>(objectSet), vtbl) || vtbl == 0)
        return false;

    std::uint64_t fnAddr = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(vtbl) + 0x30ull, fnAddr) || fnAddr == 0)
        return false;

    const auto fn = reinterpret_cast<ObjectCheck30_t>(fnAddr);

    __try
    {
        outValue = fn(objectSet, localIndex, mask) != 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outValue = false;
        return false;
    }
}

// Calls objectSet virtual +0x38 safely.
// Params: objectSet (void*), localIndex (uint16_t), outValue (bool&)
static bool TryCallObjectCheck38(void* objectSet, std::uint16_t localIndex, bool& outValue)
{
    outValue = false;

    if (!objectSet)
        return false;

    std::uint64_t vtbl = 0;
    if (!SafeReadQword(reinterpret_cast<std::uintptr_t>(objectSet), vtbl) || vtbl == 0)
        return false;

    std::uint64_t fnAddr = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(vtbl) + 0x38ull, fnAddr) || fnAddr == 0)
        return false;

    const auto fn = reinterpret_cast<ObjectCheck38_t>(fnAddr);

    __try
    {
        outValue = fn(objectSet, localIndex) != 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outValue = false;
        return false;
    }
}

// Evaluates whether vanilla would take the lost-hostage trap logic.
// Params: self (void*), trapInfo (void*), outEval (LostHostageTrapEval&)
static bool EvaluateLostHostageTrap(void* self, void* trapInfo, LostHostageTrapEval& outEval)
{
    outEval = {};

    if (!self || !trapInfo)
        return false;

    if (!ResolveLostHostageHelpers())
        return false;

    outEval.valid = true;
    outEval.mapRequest = g_GetMapRequest(trapInfo);

    std::uint16_t moverBuffer[8] = {};
    g_GetMoverGameObjectId(trapInfo, moverBuffer);

    outEval.moverGameObjectId = moverBuffer[0];
    outEval.partitionIndex = static_cast<std::uint16_t>(outEval.moverGameObjectId >> 9);
    outEval.localIndex = static_cast<std::uint16_t>(outEval.moverGameObjectId & 0x01FFu);

    outEval.moverNameId =
        g_GetNameIdWithGameObjectId(static_cast<std::uint32_t>(outEval.moverGameObjectId));

    TryGetExpectedNameId(self, outEval.expectedNameId);
    outEval.nameMatches = (outEval.expectedNameId == outEval.moverNameId);

    TryGetPartitionObjectSet(outEval.moverGameObjectId, outEval.objectSet);

    if (outEval.objectSet)
    {
        outEval.hasCheck30 =
            TryCallObjectCheck30(
                outEval.objectSet,
                outEval.localIndex,
                0x8000000000000000ull,
                outEval.check30);

        outEval.hasCheck38 =
            TryCallObjectCheck38(
                outEval.objectSet,
                outEval.localIndex,
                outEval.check38);
    }

    const float* trapPos = g_GetTrapPosition ? g_GetTrapPosition(trapInfo) : nullptr;
    if (trapPos)
    {
        outEval.hasTrapPosition = true;
        outEval.trapX = trapPos[0];
        outEval.trapY = trapPos[1];
        outEval.trapZ = trapPos[2];
    }

    outEval.request1WouldRun =
        outEval.mapRequest == 1 &&
        outEval.nameMatches &&
        outEval.objectSet != nullptr &&
        outEval.hasCheck30 &&
        !outEval.check30;

    outEval.request2WouldRun =
        outEval.mapRequest == 2 &&
        outEval.nameMatches &&
        outEval.objectSet != nullptr &&
        outEval.hasCheck30 &&
        !outEval.check30 &&
        outEval.hasCheck38 &&
        outEval.check38;

    return true;
}

// Returns whether one hostage is tracked.
// Params: hostageGameObjectId (uint16_t), outInfo (LostHostageInfo&)
static bool TryGetTrackedLostHostage(std::uint16_t hostageGameObjectId, LostHostageInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_LostHostageMutex);

    const auto it = g_TrackedLostHostages.find(hostageGameObjectId);
    if (it == g_TrackedLostHostages.end())
        return false;

    outInfo = it->second;
    return true;
}

// Arms the pending lost-hostage report.
// Params: hostageGameObjectId (uint16_t), hostageNameId (int), hostageType (int), requestType (int), x (float), y (float), z (float)
static void ArmPendingLostHostageReport(
    std::uint16_t hostageGameObjectId,
    int hostageNameId,
    int hostageType,
    int requestType,
    float x,
    float y,
    float z)
{
    std::lock_guard<std::mutex> lock(g_LostHostageMutex);

    g_PendingLostHostageReport.active = true;
    g_PendingLostHostageReport.hostageGameObjectId = hostageGameObjectId;
    g_PendingLostHostageReport.hostageNameId = hostageNameId;
    g_PendingLostHostageReport.hostageType = hostageType;
    g_PendingLostHostageReport.requestType = requestType;
    g_PendingLostHostageReport.x = x;
    g_PendingLostHostageReport.y = y;
    g_PendingLostHostageReport.z = z;
    g_PendingLostHostageReport.armedAtTickMs = GetTickCount64();

    Log(
        "[LostHostage] Armed pending report: hostageGameObjectId=0x%04X hostageNameId=%d hostageType=%s request=%d pos=(%.3f, %.3f, %.3f)\n",
        static_cast<unsigned>(hostageGameObjectId),
        hostageNameId,
        HostageTypeName(hostageType),
        requestType,
        x,
        y,
        z);
}

// Clears the pending lost-hostage report.
// Params: reason (const char*)
static void ClearPendingLostHostageReport_NoLock(const char* reason)
{
    if (g_PendingLostHostageReport.active)
    {
        Log(
            "[LostHostage] Cleared pending report: reason=%s hostageGameObjectId=0x%04X hostageType=%s request=%d\n",
            reason ? reason : "unknown",
            static_cast<unsigned>(g_PendingLostHostageReport.hostageGameObjectId),
            HostageTypeName(g_PendingLostHostageReport.hostageType),
            g_PendingLostHostageReport.requestType);
    }

    g_PendingLostHostageReport = {};
}

// Expires the pending lost-hostage report if it is too old.
// Params: none
static void ExpirePendingLostHostageReport_NoLock()
{
    if (!g_PendingLostHostageReport.active)
        return;

    const ULONGLONG nowMs = GetTickCount64();
    const ULONGLONG ageMs = nowMs - g_PendingLostHostageReport.armedAtTickMs;

    if (ageMs > LOST_HOSTAGE_PENDING_TIMEOUT_MS)
    {
        ClearPendingLostHostageReport_NoLock("timeout");
    }
}

// Reads CallPart dispatch-related values without changing behavior.
// Params: self (void*), ownerIndex (uint8_t), outSnapshot (CallPartSnapshot&)
static bool CaptureCallPartSnapshot(void* self, std::uint8_t ownerIndex, CallPartSnapshot& outSnapshot)
{
    outSnapshot = {};

    if (!self)
        return false;

    if (!ResolveLostHostageHelpers() || !g_GetVoiceParamWithCallSign)
        return false;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint64_t entryTable = 0;
    if (!SafeReadQword(selfAddr + 0x38ull, entryTable) || entryTable == 0)
        return false;

    const std::uintptr_t entryAddr =
        static_cast<std::uintptr_t>(entryTable) + static_cast<std::uintptr_t>(ownerIndex) * 0x14ull;

    std::uint8_t stateByte12 = 0;
    std::uint16_t speakerSlot = 0xFFFFu;

    if (!SafeReadByte(entryAddr + 0x12ull, stateByte12))
        return false;

    if (!SafeReadWord(entryAddr + 0x08ull, speakerSlot))
        return false;

    std::uint64_t context8 = 0;
    if (!SafeReadQword(selfAddr + 0x08ull, context8) || context8 == 0)
        return false;

    std::uint64_t context8Vtbl = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(context8), context8Vtbl) || context8Vtbl == 0)
        return false;

    std::uint64_t fn98Addr = 0;
    std::uint64_t fnB0Addr = 0;
    std::uint64_t fnB8Addr = 0;

    if (!SafeReadQword(static_cast<std::uintptr_t>(context8Vtbl) + 0x98ull, fn98Addr) || fn98Addr == 0)
        return false;
    if (!SafeReadQword(static_cast<std::uintptr_t>(context8Vtbl) + 0xB0ull, fnB0Addr) || fnB0Addr == 0)
        return false;
    if (!SafeReadQword(static_cast<std::uintptr_t>(context8Vtbl) + 0xB8ull, fnB8Addr) || fnB8Addr == 0)
        return false;

    const auto fn98 = reinterpret_cast<GetSpeechLabel98_t>(fn98Addr);
    const auto fnB0 = reinterpret_cast<GetPriorityB0_t>(fnB0Addr);
    const auto fnB8 = reinterpret_cast<GetVoiceTypeB8_t>(fnB8Addr);

    int rawSpeechLabel98 = 0;
    int priorityB0 = 0;
    std::uint32_t voiceTypeB8 = 0;
    std::uint32_t voiceParam = 0;

    __try
    {
        rawSpeechLabel98 = fn98(reinterpret_cast<void*>(context8), ownerIndex);
        priorityB0 = fnB0(reinterpret_cast<void*>(context8), ownerIndex);
        voiceTypeB8 = fnB8(reinterpret_cast<void*>(context8), ownerIndex);

        const std::uintptr_t speechHandlerBase = selfAddr - 0x20ull;
        voiceParam = static_cast<std::uint32_t>(
            g_GetVoiceParamWithCallSign(reinterpret_cast<void*>(speechHandlerBase), ownerIndex));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    outSnapshot.valid = true;
    outSnapshot.ownerIndex = ownerIndex;
    outSnapshot.speakerSlot = speakerSlot;
    outSnapshot.stateByte12 = stateByte12;
    outSnapshot.alreadyProcessed = (stateByte12 & 0x2u) != 0u;
    outSnapshot.stage = static_cast<std::uint8_t>((stateByte12 >> 2) & 0x7u);
    outSnapshot.rawSpeechLabel98 = rawSpeechLabel98;
    outSnapshot.priorityB0 = priorityB0;
    outSnapshot.voiceTypeB8 = voiceTypeB8;
    outSnapshot.voiceParam = voiceParam;

    int effectiveSpeechLabel = DEFAULT_SPEECH_LABEL;
    if (rawSpeechLabel98 != 0)
        effectiveSpeechLabel = rawSpeechLabel98;

    if (voiceParam == SPECIAL_VOICE_PARAM)
        effectiveSpeechLabel = static_cast<int>(SPECIAL_SPEECH_LABEL);

    outSnapshot.effectiveSpeechLabel = effectiveSpeechLabel;

    return true;
}

// Logs one CallPart snapshot while an escaped-hostage report is pending.
// Params: snapshot (const CallPartSnapshot&), pending (const PendingLostHostageReport&), customSpeechLabel (uint32_t)
static void LogLostHostageCallPart(
    const CallPartSnapshot& snapshot,
    const PendingLostHostageReport& pending,
    std::uint32_t customSpeechLabel)
{
    Log(
        "[LostHostageRadio] owner=%u speakerSlot=%u stateByte12=0x%02X processed=%s stage=%u rawSpeech98=0x%08X effectiveSpeech=0x%08X priority=0x%08X voiceType=0x%08X voiceParam=0x%08X pendingHostageGameObjectId=0x%04X pendingHostageType=%s pendingRequest=%d customSpeech=0x%08X\n",
        static_cast<unsigned>(snapshot.ownerIndex),
        static_cast<unsigned>(snapshot.speakerSlot),
        static_cast<unsigned>(snapshot.stateByte12),
        YesNo(snapshot.alreadyProcessed),
        static_cast<unsigned>(snapshot.stage),
        static_cast<unsigned>(snapshot.rawSpeechLabel98),
        static_cast<unsigned>(snapshot.effectiveSpeechLabel),
        static_cast<unsigned>(snapshot.priorityB0),
        static_cast<unsigned>(snapshot.voiceTypeB8),
        static_cast<unsigned>(snapshot.voiceParam),
        static_cast<unsigned>(pending.hostageGameObjectId),
        HostageTypeName(pending.hostageType),
        pending.requestType,
        static_cast<unsigned>(customSpeechLabel));
}

// Hooked version of TrapExecLostHostageCallback::ExecCallback.
// Params: self (void*), trapInfo (void*)
static void __fastcall hkExecCallback(void* self, void* trapInfo)
{
    if (!g_OrigExecCallback)
        return;

    if (MissionCodeGuard::ShouldBypassHooks())
    {
        g_OrigExecCallback(self, trapInfo);
        return;
    }

    LostHostageTrapEval eval{};
    if (EvaluateLostHostageTrap(self, trapInfo, eval))
    {
        Log(
            "[LostHostageTrap] request=%d moverGameObjectId=0x%04X moverNameId=%d expectedNameId=%d nameMatches=%s req1WouldRun=%s req2WouldRun=%s pos=(%.3f, %.3f, %.3f)\n",
            eval.mapRequest,
            static_cast<unsigned>(eval.moverGameObjectId),
            eval.moverNameId,
            eval.expectedNameId,
            YesNo(eval.nameMatches),
            YesNo(eval.request1WouldRun),
            YesNo(eval.request2WouldRun),
            eval.trapX,
            eval.trapY,
            eval.trapZ);

        if (eval.request1WouldRun || eval.request2WouldRun)
        {
            LostHostageInfo hostageInfo{};
            if (TryGetTrackedLostHostage(eval.moverGameObjectId, hostageInfo))
            {
                Log(
                    "[LostHostageTrap] MATCH tracked hostage: gameObjectId=0x%04X hostageType=%s\n",
                    static_cast<unsigned>(eval.moverGameObjectId),
                    HostageTypeName(hostageInfo.hostageType));

                ArmPendingLostHostageReport(
                    eval.moverGameObjectId,
                    eval.moverNameId,
                    hostageInfo.hostageType,
                    eval.mapRequest,
                    eval.trapX,
                    eval.trapY,
                    eval.trapZ);
            }
            else
            {
                Log(
                    "[LostHostageTrap] Ignored untracked hostage: gameObjectId=0x%04X moverNameId=%d\n",
                    static_cast<unsigned>(eval.moverGameObjectId),
                    eval.moverNameId);
            }
        }
    }

    g_OrigExecCallback(self, trapInfo);
}

// Hooked version of RadioSpeechHandlerImpl::CallPart.
// This leaves vanilla behavior untouched and only logs the report path while a pending
// lost-hostage report is armed.
// Params: self (void*), ownerIndex (uint8_t)
static bool __fastcall hkCallPart(void* self, std::uint8_t ownerIndex)
{
    if (!g_OrigCallPart)
        return false;

    if (MissionCodeGuard::ShouldBypassHooks())
    {
        return g_OrigCallPart(self, ownerIndex);
    }

    PendingLostHostageReport pending{};
    std::uint32_t customSpeechLabel = 0;
    bool shouldLog = false;

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);

        ExpirePendingLostHostageReport_NoLock();

        if (g_PendingLostHostageReport.active)
        {
            pending = g_PendingLostHostageReport;

            if (pending.hostageType >= 0 && pending.hostageType <= 2)
                customSpeechLabel = g_LostHostageSpeechLabels[pending.hostageType];

            shouldLog = true;
        }
    }

    if (shouldLog)
    {
        CallPartSnapshot snapshot{};
        if (CaptureCallPartSnapshot(self, ownerIndex, snapshot))
        {
            // Only log the main dispatch path. This cuts most of the noise.
            if (!snapshot.alreadyProcessed && snapshot.stage > 3)
            {
                const std::uint64_t logKey =
                    (static_cast<std::uint64_t>(ownerIndex) << 56) ^
                    (static_cast<std::uint64_t>(snapshot.effectiveSpeechLabel) << 24) ^
                    (static_cast<std::uint64_t>(snapshot.voiceParam) << 1) ^
                    static_cast<std::uint64_t>(snapshot.voiceTypeB8);

                const ULONGLONG nowMs = GetTickCount64();
                bool shouldEmit = true;

                if (g_LastRadioLogKey == logKey && (nowMs - g_LastRadioLogTickMs) < 750ull)
                    shouldEmit = false;

                if (shouldEmit)
                {
                    g_LastRadioLogKey = logKey;
                    g_LastRadioLogTickMs = nowMs;

                    LogLostHostageCallPart(snapshot, pending, customSpeechLabel);
                }
            }
        }
    }

    return g_OrigCallPart(self, ownerIndex);
}

// Registers one hostage to track for escape reports.
// Params: gameObjectId (uint32_t), hostageType (0=male, 1=female, 2=child)
void Add_LostHostage(std::uint32_t gameObjectId, int hostageType)
{
    if (hostageType < 0 || hostageType > 2)
    {
        Log("[LostHostage] Add ignored: invalid hostageType=%d for gameObjectId=0x%08X\n",
            hostageType,
            gameObjectId);
        return;
    }

    const std::uint16_t rawGameObjectId = static_cast<std::uint16_t>(gameObjectId);

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);

        LostHostageInfo info{};
        info.hostageType = hostageType;
        g_TrackedLostHostages[rawGameObjectId] = info;
    }

    Log(
        "[LostHostage] Added tracked hostage: gameObjectId=0x%08X hostageType=%s\n",
        gameObjectId,
        HostageTypeName(hostageType));
}

// Removes one tracked hostage.
// Params: gameObjectId (uint32_t)
void Remove_LostHostage(std::uint32_t gameObjectId)
{
    const std::uint16_t rawGameObjectId = static_cast<std::uint16_t>(gameObjectId);

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        g_TrackedLostHostages.erase(rawGameObjectId);

        if (g_PendingLostHostageReport.active &&
            g_PendingLostHostageReport.hostageGameObjectId == rawGameObjectId)
        {
            ClearPendingLostHostageReport_NoLock("removed hostage");
        }
    }

    Log("[LostHostage] Removed tracked hostage: gameObjectId=0x%08X\n", gameObjectId);
}

// Clears all tracked hostages and pending escape state.
// Params: none
void Clear_LostHostages()
{
    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        g_TrackedLostHostages.clear();
        ClearPendingLostHostageReport_NoLock("clear all");
    }

    Log("[LostHostage] Cleared all tracked hostages\n");
}

// Sets the custom speech label for one hostage type.
// Params: hostageType (0=male, 1=female, 2=child), speechLabel (uint32_t)
void Set_LostHostageSpeechLabel(int hostageType, std::uint32_t speechLabel)
{
    if (hostageType < 0 || hostageType > 2)
    {
        Log("[LostHostage] Speech label ignored: invalid hostageType=%d\n", hostageType);
        return;
    }

    g_LostHostageSpeechLabels[hostageType] = speechLabel;

    Log(
        "[LostHostage] Speech label set: hostageType=%s speechLabel=0x%08X\n",
        HostageTypeName(hostageType),
        static_cast<unsigned>(speechLabel));
}

// Installs the lost-hostage hooks.
// Params: none
bool Install_LostHostage_Hooks()
{
    ResolveLostHostageHelpers();

    void* trapTarget = ResolveGameAddress(ABS_ExecCallback);
    void* callPartTarget = ResolveGameAddress(ABS_CallPart);

    if (!trapTarget || !callPartTarget)
    {
        Log("[LostHostage] Install: target resolve failed\n");
        return false;
    }

    const bool okTrap = CreateAndEnableHook(
        trapTarget,
        reinterpret_cast<void*>(&hkExecCallback),
        reinterpret_cast<void**>(&g_OrigExecCallback));

    const bool okRadio = CreateAndEnableHook(
        callPartTarget,
        reinterpret_cast<void*>(&hkCallPart),
        reinterpret_cast<void**>(&g_OrigCallPart));

    const bool ok = okTrap && okRadio;

    Log("[LostHostage] Install TrapExecLostHostageCallback: %s\n", okTrap ? "OK" : "FAIL");
    Log("[LostHostage] Install RadioSpeechHandlerImpl::CallPart: %s\n", okRadio ? "OK" : "FAIL");

    return ok;
}

// Removes the lost-hostage hooks.
// Params: none
bool Uninstall_LostHostage_Hooks()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_ExecCallback));
    DisableAndRemoveHook(ResolveGameAddress(ABS_CallPart));

    g_OrigExecCallback = nullptr;
    g_OrigCallPart = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        g_TrackedLostHostages.clear();
        ClearPendingLostHostageReport_NoLock("uninstall");
    }

    Log("[LostHostage] removed\n");
    return true;
}