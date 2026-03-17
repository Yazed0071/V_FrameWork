#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <vector>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "LostHostageHook.h"

namespace
{
    // Hook type for tpp::gm::TrapExecLostHostageCallback::ExecCallback.
    // Params: self (void*), trapInfo (void*)
    using ExecCallback_t = void(__fastcall*)(void* self, void* trapInfo);

    // Hook type for tpp::gm::CpRadioService::ConvertRadioTypeToSpeechLabel.
    // Params: radioType (uint8_t)
    using ConvertRadioTypeToSpeechLabel_t = std::uint32_t(__fastcall*)(std::uint8_t radioType);

    // Hook type for tpp::gm::soldier::impl::NoticeControllerImpl::AddNoticeInfo.
    // Params: self (void*), soldierIndex (uint32_t), noticeInfoBlob (const void*)
    using AddNoticeInfo_t = bool(__fastcall*)(void* self, std::uint32_t soldierIndex, const void* noticeInfoBlob);

    // Hook type for tpp::gm::soldier::impl::RadioActionImpl::State_RadioRequest.
    // Params: self (void*), actionIndex (int), stateProc (int)
    using StateRadioRequest_t = void(__fastcall*)(void* self, int actionIndex, int stateProc);

    // Game function type for fox::gm::GetNameIdWithGameObjectId.
    // Params: gameObjectId (uint16_t)
    using GetNameIdWithGameObjectId_t = std::uint32_t(__fastcall*)(std::uint16_t gameObjectId);

    // Absolute addresses.
    static constexpr std::uintptr_t ABS_ExecCallback = 0x140A19030ull;
    static constexpr std::uintptr_t ABS_ConvertRadioTypeToSpeechLabel = 0x140D685C0ull;
    static constexpr std::uintptr_t ABS_AddNoticeInfo = 0x1414DCB60ull;
    static constexpr std::uintptr_t ABS_StateRadioRequest = 0x14A2ACC00ull;
    static constexpr std::uintptr_t ABS_GetNameIdWithGameObjectId_BODY = 0x146C98180ull;

    static constexpr std::uint8_t RADIO_TYPE_PRISONER_GONE = 0x1Au;
    static constexpr std::uint8_t NOTICE_TYPE_ESCAPE_OBJECT = 0x21u;
    static constexpr std::uint8_t NOTICE_TYPE_ESCAPE_PRELUDE = 0x32u;
    static constexpr std::uint16_t NOTICE_OBJECT_ESCAPE_BASE = 0x6200u;

    // Hardcoded labels by hostage type and whether player took the hostage.
    static constexpr std::uint32_t LABEL_MALE_NOT_TAKEN = 0xFA42F4E9u;
    static constexpr std::uint32_t LABEL_MALE_TAKEN = 0x43ED2D08u;
    static constexpr std::uint32_t LABEL_FEMALE_NOT_TAKEN = 0xBAE03A98u;
    static constexpr std::uint32_t LABEL_FEMALE_TAKEN = 0xD586CA7Bu;
    static constexpr std::uint32_t LABEL_CHILD_NOT_TAKEN = 0x2A2B54E0u;
    static constexpr std::uint32_t LABEL_CHILD_TAKEN = 0x96902568u;

    // Pending report source.
    enum PendingSource
    {
        PENDING_SOURCE_NONE = 0,
        PENDING_SOURCE_NOTICE21 = 1,
        PENDING_SOURCE_RADIOREQUEST = 2
    };

    static ExecCallback_t g_OrigExecCallback = nullptr;
    static ConvertRadioTypeToSpeechLabel_t g_OrigConvertRadioTypeToSpeechLabel = nullptr;
    static AddNoticeInfo_t g_OrigAddNoticeInfo = nullptr;
    static StateRadioRequest_t g_OrigStateRadioRequest = nullptr;
    static GetNameIdWithGameObjectId_t g_GetNameIdWithGameObjectId = nullptr;

    // Hostage types are radio variants only:
    // 0 = male radio
    // 1 = female radio
    // 2 = child radio
    struct LostHostageInfo
    {
        std::uint16_t rawGameObjectId = 0xFFFFu;
        int hostageType = 0;
        int nameId = -1;
    };

    struct LostHostageTrapEval
    {
        bool valid = false;
        int mapRequest = -1;
        std::uint16_t moverGameObjectId = 0xFFFFu;
        int moverNameId = -1;
        int expectedNameId = -1;
        bool moverNameMatchesExpected = false;
        std::uintptr_t callbackData = 0;
        std::uintptr_t callbackData40 = 0;
    };

    struct EscapedHostageEntry
    {
        bool valid = false;
        std::uint16_t hostageGameObjectId = 0xFFFFu;
        int hostageType = -1;
        int moverNameId = -1;
        int expectedNameId = -1;
        bool playerTookHostage = false;
        std::uintptr_t trapSelf = 0;
        std::uintptr_t callbackData = 0;
        std::uintptr_t callbackData40 = 0;
    };

    struct PendingLostHostageReport
    {
        bool active = false;
        std::uint32_t soldierIndex = 0xFFFFFFFFu;
        std::uint16_t hostageGameObjectId = 0xFFFFu;
        int hostageType = -1;
        int requestType = -1;
        int moverNameId = -1;
        int expectedNameId = -1;
        bool playerTookHostage = false;
        ULONGLONG armedAtTickMs = 0;
        int source = PENDING_SOURCE_NONE;
        int escapeOrderSlot = -1;
        std::uint16_t noticeObjectId = 0xFFFFu;
        std::uintptr_t trapSelf = 0;
        std::uintptr_t callbackData = 0;
        std::uintptr_t callbackData40 = 0;
    };

    struct RadioRequestEntryView
    {
        std::uintptr_t objectPtr = 0;
        std::uint32_t gameObjectId = 0xFFFFFFFFu; // actually looks like speaker soldier index in logs
        std::uint16_t word0C = 0xFFFFu;
        std::uint16_t word0E = 0xFFFFu;
        std::uint8_t byte10 = 0xFFu; // radio type
        std::uint8_t byte12 = 0xFFu;
        std::uint8_t byte13 = 0xFFu;
        std::uint8_t byte15 = 0xFFu;
    };

    static std::unordered_map<std::uint16_t, LostHostageInfo> g_TrackedLostHostagesByRawId;
    static std::unordered_map<int, LostHostageInfo> g_TrackedLostHostagesByNameId;

    // Escape order: first escaped hostage -> slot 0 -> 0x6200, second -> slot 1 -> 0x6201, etc.
    static std::vector<EscapedHostageEntry> g_EscapedHostagesInOrder;

    // Pending notice21 result per noticing soldier.
    static std::unordered_map<std::uint32_t, PendingLostHostageReport> g_PendingReportsBySoldierIndex;

    // Final report chosen by RadioRequest speaker soldier, consumed by ConvertRadioTypeToSpeechLabel.
    static PendingLostHostageReport g_SelectedRadioPending;

    static bool g_LostHostagePlayerTookHostage = false;
    static std::mutex g_LostHostageMutex;
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

// Returns YES or NO for logs.
// Params: value (bool)
static const char* YesNo(bool value)
{
    return value ? "YES" : "NO";
}

// Returns a readable pending source name.
// Params: source (int)
static const char* PendingSourceName(int source)
{
    switch (source)
    {
    case PENDING_SOURCE_NOTICE21:    return "NOTICE21";
    case PENDING_SOURCE_RADIOREQUEST:return "RADIOREQUEST";
    default:                         return "NONE";
    }
}

// Returns the hardcoded override speech label for one hostage type and player-took mode.
// Params: hostageType (int), playerTookHostage (bool)
static std::uint32_t GetLostHostageOverrideLabel(int hostageType, bool playerTookHostage)
{
    switch (hostageType)
    {
    case 0: return playerTookHostage ? LABEL_MALE_TAKEN : LABEL_MALE_NOT_TAKEN;
    case 1: return playerTookHostage ? LABEL_FEMALE_TAKEN : LABEL_FEMALE_NOT_TAKEN;
    case 2: return playerTookHostage ? LABEL_CHILD_TAKEN : LABEL_CHILD_NOT_TAKEN;
    default: return 0;
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

// Safely reads one uint16 from memory.
// Params: addr (uintptr_t), outValue (uint16_t&)
static bool SafeReadU16(std::uintptr_t addr, std::uint16_t& outValue)
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

// Safely reads one uint8 from memory.
// Params: addr (uintptr_t), outValue (uint8_t&)
static bool SafeReadU8(std::uintptr_t addr, std::uint8_t& outValue)
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

// Safely copies raw bytes from memory.
// Params: addr (uintptr_t), outBuffer (uint8_t*), sizeBytes (size_t)
static bool SafeReadBytes(std::uintptr_t addr, std::uint8_t* outBuffer, std::size_t sizeBytes)
{
    if (!addr || !outBuffer || sizeBytes == 0)
        return false;

    __try
    {
        std::memcpy(outBuffer, reinterpret_cast<const void*>(addr), sizeBytes);
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
    if (!g_GetNameIdWithGameObjectId)
    {
        g_GetNameIdWithGameObjectId = reinterpret_cast<GetNameIdWithGameObjectId_t>(
            ResolveGameAddress(ABS_GetNameIdWithGameObjectId_BODY));
    }

    return g_GetNameIdWithGameObjectId != nullptr;
}

// Safely calls fox::gm::GetNameIdWithGameObjectId using the real body address.
// Params: gameObjectId (uint16_t), outNameId (int&)
static bool TryGetNameIdWithGameObjectId(std::uint16_t gameObjectId, int& outNameId)
{
    outNameId = -1;

    if (!ResolveLostHostageHelpers() || !g_GetNameIdWithGameObjectId)
        return false;

    __try
    {
        outNameId = static_cast<int>(g_GetNameIdWithGameObjectId(gameObjectId));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outNameId = -1;
        return false;
    }
}

// Reads trap-side pointers from the callback object.
// Params: self (void*), outCallbackData (uintptr_t&), outCallbackData40 (uintptr_t&)
static bool TryGetTrapPointers(
    void* self,
    std::uintptr_t& outCallbackData,
    std::uintptr_t& outCallbackData40)
{
    outCallbackData = 0;
    outCallbackData40 = 0;

    if (!self)
        return false;

    std::uint64_t callbackData = 0;
    if (!SafeReadQword(reinterpret_cast<std::uintptr_t>(self) + 0x10ull, callbackData) || callbackData == 0)
        return false;

    std::uint64_t callbackData40 = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(callbackData) + 0x40ull, callbackData40) || callbackData40 == 0)
        return false;

    outCallbackData = static_cast<std::uintptr_t>(callbackData);
    outCallbackData40 = static_cast<std::uintptr_t>(callbackData40);
    return true;
}

// Reads the expected hostage NameId from the callback object.
// Params: self (void*), outExpectedNameId (int&)
static bool TryGetExpectedNameId(void* self, int& outExpectedNameId)
{
    outExpectedNameId = -1;

    std::uintptr_t callbackData = 0;
    std::uintptr_t callbackData40 = 0;
    if (!TryGetTrapPointers(self, callbackData, callbackData40))
        return false;

    return SafeReadInt(callbackData40 + 0x10ull, outExpectedNameId);
}

// Evaluates the top-level trap values.
// Params: self (void*), trapInfo (void*), outEval (LostHostageTrapEval&)
static bool EvaluateLostHostageTrap(void* self, void* trapInfo, LostHostageTrapEval& outEval)
{
    outEval = {};

    if (!self || !trapInfo)
        return false;

    outEval.valid = true;

    if (!SafeReadInt(reinterpret_cast<std::uintptr_t>(trapInfo) + 0xD0ull, outEval.mapRequest))
        return false;

    if (!SafeReadU16(reinterpret_cast<std::uintptr_t>(trapInfo) + 0x68ull, outEval.moverGameObjectId))
        return false;

    TryGetTrapPointers(self, outEval.callbackData, outEval.callbackData40);

    if (!TryGetExpectedNameId(self, outEval.expectedNameId))
        return false;

    TryGetNameIdWithGameObjectId(outEval.moverGameObjectId, outEval.moverNameId);

    outEval.moverNameMatchesExpected =
        (outEval.moverNameId != -1 && outEval.moverNameId == outEval.expectedNameId);

    return true;
}

// Reads the currently stored notice type for one soldier.
// Params: self (void*), soldierIndex (uint32_t), outCurrentNoticeType (uint32_t&)
static bool TryGetCurrentNoticeType(void* self, std::uint32_t soldierIndex, std::uint32_t& outCurrentNoticeType)
{
    outCurrentNoticeType = 0;

    if (!self)
        return false;

    std::uint64_t noticeSlotsBase = 0;
    if (!SafeReadQword(reinterpret_cast<std::uintptr_t>(self) + 0x40ull, noticeSlotsBase) || noticeSlotsBase == 0)
        return false;

    const std::uintptr_t slotBase =
        static_cast<std::uintptr_t>(noticeSlotsBase) + (static_cast<std::uintptr_t>(soldierIndex) * 0x80ull);

    std::uint8_t currentType8 = 0;
    if (!SafeReadU8(slotBase + 0x0ull, currentType8))
        return false;

    if (currentType8 == 0x3Eu)
    {
        int currentType32 = 0;
        if (!SafeReadInt(slotBase + 0x60ull, currentType32))
            return false;

        outCurrentNoticeType = static_cast<std::uint32_t>(currentType32);
        return true;
    }

    outCurrentNoticeType = static_cast<std::uint32_t>(currentType8);
    return true;
}

// Builds a short hex string for the first 16 bytes of a notice blob.
// Params: blobAddr (uintptr_t), outText (char*), outTextSize (size_t)
static void BuildNoticeBlobHex16(std::uintptr_t blobAddr, char* outText, std::size_t outTextSize)
{
    if (!outText || outTextSize == 0)
        return;

    outText[0] = '\0';

    std::uint8_t bytes[16] = {};
    if (!SafeReadBytes(blobAddr, bytes, sizeof(bytes)))
    {
        std::snprintf(outText, outTextSize, "<read failed>");
        return;
    }

    std::snprintf(
        outText,
        outTextSize,
        "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        static_cast<unsigned>(bytes[0]),
        static_cast<unsigned>(bytes[1]),
        static_cast<unsigned>(bytes[2]),
        static_cast<unsigned>(bytes[3]),
        static_cast<unsigned>(bytes[4]),
        static_cast<unsigned>(bytes[5]),
        static_cast<unsigned>(bytes[6]),
        static_cast<unsigned>(bytes[7]),
        static_cast<unsigned>(bytes[8]),
        static_cast<unsigned>(bytes[9]),
        static_cast<unsigned>(bytes[10]),
        static_cast<unsigned>(bytes[11]),
        static_cast<unsigned>(bytes[12]),
        static_cast<unsigned>(bytes[13]),
        static_cast<unsigned>(bytes[14]),
        static_cast<unsigned>(bytes[15]));
}

// Extracts notice type and 0x62xx escape-order slot from a notice blob.
// Params: noticeInfoBlob (const void*), outNoticeType (uint8_t&), outNoticeObjectId (uint16_t&), outEscapeSlot (int&)
static bool TryParseEscapeNoticeBlob(
    const void* noticeInfoBlob,
    std::uint8_t& outNoticeType,
    std::uint16_t& outNoticeObjectId,
    int& outEscapeSlot)
{
    outNoticeType = 0xFFu;
    outNoticeObjectId = 0xFFFFu;
    outEscapeSlot = -1;

    if (!noticeInfoBlob)
        return false;

    const std::uintptr_t blob = reinterpret_cast<std::uintptr_t>(noticeInfoBlob);

    if (!SafeReadU8(blob + 0x0ull, outNoticeType))
        return false;

    if (outNoticeType != NOTICE_TYPE_ESCAPE_OBJECT)
        return true;

    if (!SafeReadU16(blob + 0x2ull, outNoticeObjectId))
        return false;

    if (outNoticeObjectId >= NOTICE_OBJECT_ESCAPE_BASE)
        outEscapeSlot = static_cast<int>(outNoticeObjectId - NOTICE_OBJECT_ESCAPE_BASE);

    return true;
}

// Reads one RadioActionImpl::State_RadioRequest entry.
// Params: self (void*), actionIndex (int), outView (RadioRequestEntryView&)
static bool TryReadRadioRequestEntry(void* self, int actionIndex, RadioRequestEntryView& outView)
{
    outView = {};

    if (!self)
        return false;

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(self);

    std::uint64_t listBase = 0;
    int baseActionIndex = 0;

    if (!SafeReadQword(base + 0x88ull, listBase) || listBase == 0)
        return false;

    if (!SafeReadInt(base + 0x90ull, baseActionIndex))
        return false;

    const int relativeIndex = actionIndex - baseActionIndex;
    if (relativeIndex < 0)
        return false;

    const std::uintptr_t entry = static_cast<std::uintptr_t>(listBase) + (static_cast<std::uintptr_t>(relativeIndex) * 0x18ull);

    std::uint64_t objectPtr = 0;
    int gameObjectId = -1;

    SafeReadQword(entry + 0x0ull, objectPtr);
    SafeReadInt(entry + 0x8ull, gameObjectId);
    SafeReadU16(entry + 0xCull, outView.word0C);
    SafeReadU16(entry + 0xEull, outView.word0E);
    SafeReadU8(entry + 0x10ull, outView.byte10);
    SafeReadU8(entry + 0x12ull, outView.byte12);
    SafeReadU8(entry + 0x13ull, outView.byte13);
    SafeReadU8(entry + 0x15ull, outView.byte15);

    outView.objectPtr = static_cast<std::uintptr_t>(objectPtr);
    outView.gameObjectId = static_cast<std::uint32_t>(gameObjectId);
    return true;
}

// Tries to match a tracked hostage by raw mover id first, then moverNameId, then expectedNameId.
// Params: moverGameObjectId (uint16_t), moverNameId (int), expectedNameId (int), outInfo (LostHostageInfo&)
static bool TryMatchTrackedLostHostage(
    std::uint16_t moverGameObjectId,
    int moverNameId,
    int expectedNameId,
    LostHostageInfo& outInfo)
{
    std::lock_guard<std::mutex> lock(g_LostHostageMutex);

    const auto itRaw = g_TrackedLostHostagesByRawId.find(moverGameObjectId);
    if (itRaw != g_TrackedLostHostagesByRawId.end())
    {
        outInfo = itRaw->second;
        Log("[LostHostage] Match by raw GameObjectId: 0x%04X\n",
            static_cast<unsigned>(moverGameObjectId));
        return true;
    }

    if (moverNameId != -1)
    {
        const auto itMoverName = g_TrackedLostHostagesByNameId.find(moverNameId);
        if (itMoverName != g_TrackedLostHostagesByNameId.end())
        {
            outInfo = itMoverName->second;
            Log("[LostHostage] Match by moverNameId: %d\n", moverNameId);
            return true;
        }
    }

    if (expectedNameId != -1)
    {
        const auto itExpectedName = g_TrackedLostHostagesByNameId.find(expectedNameId);
        if (itExpectedName != g_TrackedLostHostagesByNameId.end())
        {
            outInfo = itExpectedName->second;
            Log("[LostHostage] Match by expectedNameId: %d\n", expectedNameId);
            return true;
        }
    }

    Log("[LostHostage] Dump tracked hostages: rawCount=%u nameCount=%u\n",
        static_cast<unsigned>(g_TrackedLostHostagesByRawId.size()),
        static_cast<unsigned>(g_TrackedLostHostagesByNameId.size()));

    for (const auto& kv : g_TrackedLostHostagesByRawId)
    {
        const LostHostageInfo& info = kv.second;
        Log("[LostHostage]   raw=0x%04X type=%s nameId=%d\n",
            static_cast<unsigned>(info.rawGameObjectId),
            HostageTypeName(info.hostageType),
            info.nameId);
    }

    return false;
}

// Registers one escaped hostage in current-run escape order.
// Params: hostageInfo (const LostHostageInfo&), eval (const LostHostageTrapEval&), trapSelf (void*)
static int RegisterEscapedHostage_NoLock(
    const LostHostageInfo& hostageInfo,
    const LostHostageTrapEval& eval,
    void* trapSelf)
{
    for (std::size_t i = 0; i < g_EscapedHostagesInOrder.size(); ++i)
    {
        EscapedHostageEntry& entry = g_EscapedHostagesInOrder[i];
        if (entry.valid && entry.hostageGameObjectId == hostageInfo.rawGameObjectId)
        {
            entry.hostageType = hostageInfo.hostageType;
            entry.moverNameId = eval.moverNameId;
            entry.expectedNameId = eval.expectedNameId;
            entry.playerTookHostage = g_LostHostagePlayerTookHostage;
            entry.trapSelf = reinterpret_cast<std::uintptr_t>(trapSelf);
            entry.callbackData = eval.callbackData;
            entry.callbackData40 = eval.callbackData40;
            return static_cast<int>(i);
        }
    }

    EscapedHostageEntry entry{};
    entry.valid = true;
    entry.hostageGameObjectId = hostageInfo.rawGameObjectId;
    entry.hostageType = hostageInfo.hostageType;
    entry.moverNameId = eval.moverNameId;
    entry.expectedNameId = eval.expectedNameId;
    entry.playerTookHostage = g_LostHostagePlayerTookHostage;
    entry.trapSelf = reinterpret_cast<std::uintptr_t>(trapSelf);
    entry.callbackData = eval.callbackData;
    entry.callbackData40 = eval.callbackData40;

    g_EscapedHostagesInOrder.push_back(entry);
    return static_cast<int>(g_EscapedHostagesInOrder.size() - 1);
}

// Stores one soldier-specific pending report from notice21.
// Params: report (const PendingLostHostageReport&)
static void StorePendingReportForSoldier_NoLock(const PendingLostHostageReport& report)
{
    g_PendingReportsBySoldierIndex[report.soldierIndex] = report;

    Log(
        "[LostHostage] Stored soldier pending: soldierIndex=%u source=%s hostageGameObjectId=0x%04X hostageType=%s slot=%d noticeObjectId=0x%04X\n",
        static_cast<unsigned>(report.soldierIndex),
        PendingSourceName(report.source),
        static_cast<unsigned>(report.hostageGameObjectId),
        HostageTypeName(report.hostageType),
        report.escapeOrderSlot,
        static_cast<unsigned>(report.noticeObjectId));
}

// Builds one soldier-specific pending report from one escaped-hostage entry.
// Params: soldierIndex (uint32_t), entry (const EscapedHostageEntry&), requestType (int), source (int), escapeOrderSlot (int), noticeObjectId (uint16_t)
static PendingLostHostageReport MakePendingReportFromEntry(
    std::uint32_t soldierIndex,
    const EscapedHostageEntry& entry,
    int requestType,
    int source,
    int escapeOrderSlot,
    std::uint16_t noticeObjectId)
{
    PendingLostHostageReport report{};
    report.active = true;
    report.soldierIndex = soldierIndex;
    report.hostageGameObjectId = entry.hostageGameObjectId;
    report.hostageType = entry.hostageType;
    report.requestType = requestType;
    report.moverNameId = entry.moverNameId;
    report.expectedNameId = entry.expectedNameId;
    report.playerTookHostage = entry.playerTookHostage;
    report.armedAtTickMs = GetTickCount64();
    report.source = source;
    report.escapeOrderSlot = escapeOrderSlot;
    report.noticeObjectId = noticeObjectId;
    report.trapSelf = entry.trapSelf;
    report.callbackData = entry.callbackData;
    report.callbackData40 = entry.callbackData40;
    return report;
}

// Clears the selected radio pending record.
// Params: reason (const char*)
static void ClearSelectedRadioPending_NoLock(const char* reason)
{
    if (g_SelectedRadioPending.active)
    {
        Log(
            "[LostHostage] Cleared selected radio pending: reason=%s soldierIndex=%u hostageGameObjectId=0x%04X hostageType=%s source=%s slot=%d noticeObjectId=0x%04X\n",
            reason ? reason : "unknown",
            static_cast<unsigned>(g_SelectedRadioPending.soldierIndex),
            static_cast<unsigned>(g_SelectedRadioPending.hostageGameObjectId),
            HostageTypeName(g_SelectedRadioPending.hostageType),
            PendingSourceName(g_SelectedRadioPending.source),
            g_SelectedRadioPending.escapeOrderSlot,
            static_cast<unsigned>(g_SelectedRadioPending.noticeObjectId));
    }

    g_SelectedRadioPending = {};
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
        if (eval.mapRequest != 4)
        {
            Log(
                "[LostHostageTrap] self=%p callbackData=%p callbackData40=%p request=%d moverGameObjectId=0x%04X moverNameId=%d expectedNameId=%d nameMatch=%s\n",
                self,
                reinterpret_cast<void*>(eval.callbackData),
                reinterpret_cast<void*>(eval.callbackData40),
                eval.mapRequest,
                static_cast<unsigned>(eval.moverGameObjectId),
                eval.moverNameId,
                eval.expectedNameId,
                YesNo(eval.moverNameMatchesExpected));
        }

        if (eval.mapRequest == 2 && eval.moverGameObjectId != 0xFFFFu)
        {
            LostHostageInfo hostageInfo{};
            if (TryMatchTrackedLostHostage(
                eval.moverGameObjectId,
                eval.moverNameId,
                eval.expectedNameId,
                hostageInfo))
            {
                Log(
                    "[LostHostageTrap] MATCH tracked hostage: moverGameObjectId=0x%04X hostageType=%s trackedRaw=0x%04X trackedNameId=%d nameMatch=%s\n",
                    static_cast<unsigned>(eval.moverGameObjectId),
                    HostageTypeName(hostageInfo.hostageType),
                    static_cast<unsigned>(hostageInfo.rawGameObjectId),
                    hostageInfo.nameId,
                    YesNo(eval.moverNameMatchesExpected));

                std::lock_guard<std::mutex> lock(g_LostHostageMutex);

                const int slot = RegisterEscapedHostage_NoLock(hostageInfo, eval, self);

                Log(
                    "[LostHostage] Registered escaped hostage slot=%d hostageGameObjectId=0x%04X hostageType=%s callbackData40=%p\n",
                    slot,
                    static_cast<unsigned>(hostageInfo.rawGameObjectId),
                    HostageTypeName(hostageInfo.hostageType),
                    reinterpret_cast<void*>(eval.callbackData40));
            }
            else
            {
                Log(
                    "[LostHostageTrap] Ignored untracked hostage: gameObjectId=0x%04X moverNameId=%d expectedNameId=%d nameMatch=%s\n",
                    static_cast<unsigned>(eval.moverGameObjectId),
                    eval.moverNameId,
                    eval.expectedNameId,
                    YesNo(eval.moverNameMatchesExpected));
            }
        }
    }

    g_OrigExecCallback(self, trapInfo);
}

// Hooked version of NoticeControllerImpl::AddNoticeInfo.
// Params: self (void*), soldierIndex (uint32_t), noticeInfoBlob (const void*)
static bool __fastcall hkAddNoticeInfo(
    void* self,
    std::uint32_t soldierIndex,
    const void* noticeInfoBlob)
{
    if (!g_OrigAddNoticeInfo)
        return false;

    if (MissionCodeGuard::ShouldBypassHooks())
        return g_OrigAddNoticeInfo(self, soldierIndex, noticeInfoBlob);

    std::uint8_t newNoticeType = 0xFFu;
    std::uint16_t noticeObjectId = 0xFFFFu;
    int escapeSlot = -1;
    char blobHex16[16 * 3] = {};
    std::uint32_t currentNoticeTypeBefore = 0;
    const bool hasCurrentBefore = TryGetCurrentNoticeType(self, soldierIndex, currentNoticeTypeBefore);

    TryParseEscapeNoticeBlob(noticeInfoBlob, newNoticeType, noticeObjectId, escapeSlot);
    if (noticeInfoBlob)
        BuildNoticeBlobHex16(reinterpret_cast<std::uintptr_t>(noticeInfoBlob), blobHex16, sizeof(blobHex16));
    else
        std::snprintf(blobHex16, sizeof(blobHex16), "<null>");

    const bool accepted = g_OrigAddNoticeInfo(self, soldierIndex, noticeInfoBlob);
    if (!accepted)
        return false;

    if (newNoticeType != NOTICE_TYPE_ESCAPE_OBJECT &&
        newNoticeType != NOTICE_TYPE_ESCAPE_PRELUDE)
    {
        return true;
    }

    std::uint32_t currentNoticeTypeAfter = 0;
    const bool hasCurrentAfter = TryGetCurrentNoticeType(self, soldierIndex, currentNoticeTypeAfter);

    Log(
        "[LostHostageNotice] soldierIndex=%u accepted=YES currentBefore=%s0x%X currentAfter=%s0x%X newNotice=0x%02X blob16=[%s]\n",
        static_cast<unsigned>(soldierIndex),
        hasCurrentBefore ? "" : "?",
        static_cast<unsigned>(currentNoticeTypeBefore),
        hasCurrentAfter ? "" : "?",
        static_cast<unsigned>(currentNoticeTypeAfter),
        static_cast<unsigned>(newNoticeType),
        blobHex16);

    if (newNoticeType == NOTICE_TYPE_ESCAPE_OBJECT)
    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);

        if (escapeSlot < 0)
        {
            Log(
                "[LostHostage] NOTICE21 ignored: soldierIndex=%u noticeObjectId=0x%04X did not decode to valid slot\n",
                static_cast<unsigned>(soldierIndex),
                static_cast<unsigned>(noticeObjectId));
            return true;
        }

        const std::size_t slotIndex = static_cast<std::size_t>(escapeSlot);
        if (slotIndex >= g_EscapedHostagesInOrder.size())
        {
            Log(
                "[LostHostage] NOTICE21 ignored: soldierIndex=%u slot=%d noticeObjectId=0x%04X has no escaped entry yet\n",
                static_cast<unsigned>(soldierIndex),
                escapeSlot,
                static_cast<unsigned>(noticeObjectId));
            return true;
        }

        const EscapedHostageEntry& entry = g_EscapedHostagesInOrder[slotIndex];
        if (!entry.valid)
        {
            Log(
                "[LostHostage] NOTICE21 ignored: soldierIndex=%u slot=%d noticeObjectId=0x%04X entry invalid\n",
                static_cast<unsigned>(soldierIndex),
                escapeSlot,
                static_cast<unsigned>(noticeObjectId));
            return true;
        }

        const PendingLostHostageReport report = MakePendingReportFromEntry(
            soldierIndex,
            entry,
            NOTICE_TYPE_ESCAPE_OBJECT,
            PENDING_SOURCE_NOTICE21,
            escapeSlot,
            noticeObjectId);

        StorePendingReportForSoldier_NoLock(report);

        Log(
            "[LostHostage] NOTICE21 selected soldierIndex=%u slot=%d noticeObjectId=0x%04X hostageGameObjectId=0x%04X hostageType=%s\n",
            static_cast<unsigned>(soldierIndex),
            escapeSlot,
            static_cast<unsigned>(noticeObjectId),
            static_cast<unsigned>(entry.hostageGameObjectId),
            HostageTypeName(entry.hostageType));
    }

    return true;
}

// Hooked version of RadioActionImpl::State_RadioRequest.
// This is where we map the actual speaker soldier to the notice21 result.
// Params: self (void*), actionIndex (int), stateProc (int)
static void __fastcall hkStateRadioRequest(void* self, int actionIndex, int stateProc)
{
    if (!g_OrigStateRadioRequest)
        return;

    if (MissionCodeGuard::ShouldBypassHooks())
    {
        g_OrigStateRadioRequest(self, actionIndex, stateProc);
        return;
    }

    RadioRequestEntryView before{};
    const bool hasBefore = TryReadRadioRequestEntry(self, actionIndex, before);

    Log(
        "[RadioRequest] ENTER self=%p actionIndex=%d stateProc=%d\n",
        self,
        actionIndex,
        stateProc);

    if (hasBefore)
    {
        Log(
            "[RadioRequest] BEFORE self=%p actionIndex=%d obj=%p speakerSoldierIndex=%u word0C=0x%04X word0E=0x%04X byte10=0x%02X byte12=0x%02X byte13=0x%02X byte15=0x%02X\n",
            self,
            actionIndex,
            reinterpret_cast<void*>(before.objectPtr),
            static_cast<unsigned>(before.gameObjectId),
            static_cast<unsigned>(before.word0C),
            static_cast<unsigned>(before.word0E),
            static_cast<unsigned>(before.byte10),
            static_cast<unsigned>(before.byte12),
            static_cast<unsigned>(before.byte13),
            static_cast<unsigned>(before.byte15));
    }

    g_OrigStateRadioRequest(self, actionIndex, stateProc);

    RadioRequestEntryView after{};
    const bool hasAfter = TryReadRadioRequestEntry(self, actionIndex, after);

    if (hasAfter)
    {
        Log(
            "[RadioRequest] AFTER self=%p actionIndex=%d obj=%p speakerSoldierIndex=%u word0C=0x%04X word0E=0x%04X byte10=0x%02X byte12=0x%02X byte13=0x%02X byte15=0x%02X\n",
            self,
            actionIndex,
            reinterpret_cast<void*>(after.objectPtr),
            static_cast<unsigned>(after.gameObjectId),
            static_cast<unsigned>(after.word0C),
            static_cast<unsigned>(after.word0E),
            static_cast<unsigned>(after.byte10),
            static_cast<unsigned>(after.byte12),
            static_cast<unsigned>(after.byte13),
            static_cast<unsigned>(after.byte15));
    }

    Log(
        "[RadioRequest] LEAVE self=%p actionIndex=%d stateProc=%d\n",
        self,
        actionIndex,
        stateProc);

    // We only care when this request is a prisoner-gone radio.
    // The logs you posted showed byte10 == 0x1A for the correct request.
    if (!hasAfter || after.byte10 != RADIO_TYPE_PRISONER_GONE)
        return;

    // Based on your logs:
    // gameObjectId=0x00000006  -> soldierIndex 6
    // gameObjectId=0x0000002C  -> soldierIndex 44
    const std::uint32_t speakerSoldierIndex = after.gameObjectId;

    std::lock_guard<std::mutex> lock(g_LostHostageMutex);

    const auto it = g_PendingReportsBySoldierIndex.find(speakerSoldierIndex);
    if (it == g_PendingReportsBySoldierIndex.end())
    {
        Log(
            "[LostHostage] RadioRequest no soldier pending: speakerSoldierIndex=%u radioType=0x%02X stateProc=%d\n",
            static_cast<unsigned>(speakerSoldierIndex),
            static_cast<unsigned>(after.byte10),
            stateProc);
        return;
    }

    g_SelectedRadioPending = it->second;
    g_SelectedRadioPending.source = PENDING_SOURCE_RADIOREQUEST;
    g_PendingReportsBySoldierIndex.erase(it);

    Log(
        "[LostHostage] RadioRequest selected pending: speakerSoldierIndex=%u hostageGameObjectId=0x%04X hostageType=%s slot=%d noticeObjectId=0x%04X stateProc=%d\n",
        static_cast<unsigned>(g_SelectedRadioPending.soldierIndex),
        static_cast<unsigned>(g_SelectedRadioPending.hostageGameObjectId),
        HostageTypeName(g_SelectedRadioPending.hostageType),
        g_SelectedRadioPending.escapeOrderSlot,
        static_cast<unsigned>(g_SelectedRadioPending.noticeObjectId),
        stateProc);
}

// Hooked version of CpRadioService::ConvertRadioTypeToSpeechLabel.
// Params: radioType (uint8_t)
static std::uint32_t __fastcall hkConvertRadioTypeToSpeechLabel(std::uint8_t radioType)
{
    if (!g_OrigConvertRadioTypeToSpeechLabel)
        return 0;

    if (MissionCodeGuard::ShouldBypassHooks())
        return g_OrigConvertRadioTypeToSpeechLabel(radioType);

    const std::uint32_t baseLabel = g_OrigConvertRadioTypeToSpeechLabel(radioType);

    if (radioType != RADIO_TYPE_PRISONER_GONE)
        return baseLabel;

    PendingLostHostageReport pending{};
    bool hasPending = false;

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        if (g_SelectedRadioPending.active)
        {
            pending = g_SelectedRadioPending;
            hasPending = true;
        }
    }

    if (!hasPending)
        return baseLabel;

    const std::uint32_t overrideLabel =
        GetLostHostageOverrideLabel(
            pending.hostageType,
            pending.playerTookHostage);

    if (overrideLabel == 0)
        return baseLabel;

    Log(
        "[LostHostageRadio] Override ConvertRadioTypeToSpeechLabel: radioType=0x%02X baseLabel=0x%08X overrideLabel=0x%08X soldierIndex=%u source=%s hostageGameObjectId=0x%04X hostageType=%s slot=%d noticeObjectId=0x%04X playerTook=%s\n",
        static_cast<unsigned>(radioType),
        static_cast<unsigned>(baseLabel),
        static_cast<unsigned>(overrideLabel),
        static_cast<unsigned>(pending.soldierIndex),
        PendingSourceName(pending.source),
        static_cast<unsigned>(pending.hostageGameObjectId),
        HostageTypeName(pending.hostageType),
        pending.escapeOrderSlot,
        static_cast<unsigned>(pending.noticeObjectId),
        YesNo(pending.playerTookHostage));

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        ClearSelectedRadioPending_NoLock("radio override consumed");
    }

    return overrideLabel;
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

    int nameId = -1;
    TryGetNameIdWithGameObjectId(rawGameObjectId, nameId);

    LostHostageInfo info{};
    info.rawGameObjectId = rawGameObjectId;
    info.hostageType = hostageType;
    info.nameId = nameId;

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        g_TrackedLostHostagesByRawId[rawGameObjectId] = info;

        if (nameId != -1)
            g_TrackedLostHostagesByNameId[nameId] = info;
    }

    Log(
        "[LostHostage] Added tracked hostage: inputGameObjectId=0x%08X rawGameObjectId=0x%04X hostageType=%s nameId=%d\n",
        gameObjectId,
        static_cast<unsigned>(rawGameObjectId),
        HostageTypeName(hostageType),
        nameId);
}

// Removes one tracked hostage.
// Params: gameObjectId (uint32_t)
void Remove_LostHostage(std::uint32_t gameObjectId)
{
    const std::uint16_t rawGameObjectId = static_cast<std::uint16_t>(gameObjectId);

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);

        const auto it = g_TrackedLostHostagesByRawId.find(rawGameObjectId);
        if (it != g_TrackedLostHostagesByRawId.end())
        {
            const int nameId = it->second.nameId;
            g_TrackedLostHostagesByRawId.erase(it);

            if (nameId != -1)
                g_TrackedLostHostagesByNameId.erase(nameId);
        }

        for (EscapedHostageEntry& entry : g_EscapedHostagesInOrder)
        {
            if (entry.valid && entry.hostageGameObjectId == rawGameObjectId)
                entry.valid = false;
        }

        for (auto it = g_PendingReportsBySoldierIndex.begin(); it != g_PendingReportsBySoldierIndex.end();)
        {
            if (it->second.hostageGameObjectId == rawGameObjectId)
                it = g_PendingReportsBySoldierIndex.erase(it);
            else
                ++it;
        }

        if (g_SelectedRadioPending.active &&
            g_SelectedRadioPending.hostageGameObjectId == rawGameObjectId)
        {
            ClearSelectedRadioPending_NoLock("removed hostage");
        }
    }

    Log(
        "[LostHostage] Removed tracked hostage: inputGameObjectId=0x%08X rawGameObjectId=0x%04X\n",
        gameObjectId,
        static_cast<unsigned>(rawGameObjectId));
}

// Clears all tracked hostages.
// Params: none
void Clear_LostHostages()
{
    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        g_TrackedLostHostagesByRawId.clear();
        g_TrackedLostHostagesByNameId.clear();
        g_EscapedHostagesInOrder.clear();
        g_PendingReportsBySoldierIndex.clear();
        g_SelectedRadioPending = {};
    }

    Log("[LostHostage] Cleared all tracked hostages\n");
}

// Sets whether the hostage was taken by the player.
// Params: playerTookHostage (bool)
void SetLostHostageFromPlayer(bool playerTookHostage)
{
    g_LostHostagePlayerTookHostage = playerTookHostage;

    Log(
        "[LostHostage] Player-took-hostage mode set: %s\n",
        YesNo(playerTookHostage));
}

// Installs the lost-hostage hooks.
// Params: none
bool Install_LostHostage_Hooks()
{
    void* trapTarget = ResolveGameAddress(ABS_ExecCallback);
    void* convertTarget = ResolveGameAddress(ABS_ConvertRadioTypeToSpeechLabel);
    void* addNoticeInfoTarget = ResolveGameAddress(ABS_AddNoticeInfo);
    void* stateRadioRequestTarget = ResolveGameAddress(ABS_StateRadioRequest);

    Log("======== LOSTHOSTAGE BUILD MARKER: RadioActionImpl::State_RadioRequest mapping build loaded ========\n");

    if (!trapTarget || !convertTarget || !addNoticeInfoTarget || !stateRadioRequestTarget)
    {
        Log("[LostHostage] Install: target resolve failed trap=%p convert=%p addNoticeInfo=%p stateRadioRequest=%p\n",
            trapTarget,
            convertTarget,
            addNoticeInfoTarget,
            stateRadioRequestTarget);
        return false;
    }

    ResolveLostHostageHelpers();

    const bool okTrap = CreateAndEnableHook(
        trapTarget,
        reinterpret_cast<void*>(&hkExecCallback),
        reinterpret_cast<void**>(&g_OrigExecCallback));

    const bool okConvert = CreateAndEnableHook(
        convertTarget,
        reinterpret_cast<void*>(&hkConvertRadioTypeToSpeechLabel),
        reinterpret_cast<void**>(&g_OrigConvertRadioTypeToSpeechLabel));

    const bool okAddNoticeInfo = CreateAndEnableHook(
        addNoticeInfoTarget,
        reinterpret_cast<void*>(&hkAddNoticeInfo),
        reinterpret_cast<void**>(&g_OrigAddNoticeInfo));

    const bool okStateRadioRequest = CreateAndEnableHook(
        stateRadioRequestTarget,
        reinterpret_cast<void*>(&hkStateRadioRequest),
        reinterpret_cast<void**>(&g_OrigStateRadioRequest));

    Log("[LostHostage] Install TrapExecLostHostageCallback: %s\n", okTrap ? "OK" : "FAIL");
    Log("[LostHostage] Install CpRadioService::ConvertRadioTypeToSpeechLabel: %s\n", okConvert ? "OK" : "FAIL");
    Log("[LostHostage] Install NoticeControllerImpl::AddNoticeInfo: %s\n", okAddNoticeInfo ? "OK" : "FAIL");
    Log("[LostHostage] Install RadioActionImpl::State_RadioRequest: %s target=%p orig=%p\n",
        okStateRadioRequest ? "OK" : "FAIL",
        stateRadioRequestTarget,
        reinterpret_cast<void*>(g_OrigStateRadioRequest));

    return okTrap && okConvert && okAddNoticeInfo && okStateRadioRequest;
}

// Removes the lost-hostage hooks and clears current-run state.
// Params: none
bool Uninstall_LostHostage_Hooks()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_ExecCallback));
    DisableAndRemoveHook(ResolveGameAddress(ABS_ConvertRadioTypeToSpeechLabel));
    DisableAndRemoveHook(ResolveGameAddress(ABS_AddNoticeInfo));
    DisableAndRemoveHook(ResolveGameAddress(ABS_StateRadioRequest));

    g_OrigExecCallback = nullptr;
    g_OrigConvertRadioTypeToSpeechLabel = nullptr;
    g_OrigAddNoticeInfo = nullptr;
    g_OrigStateRadioRequest = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_LostHostageMutex);
        g_TrackedLostHostagesByRawId.clear();
        g_TrackedLostHostagesByNameId.clear();
        g_EscapedHostagesInOrder.clear();
        g_PendingReportsBySoldierIndex.clear();
        g_SelectedRadioPending = {};
    }

    Log("[LostHostage] removed\n");
    return true;
}