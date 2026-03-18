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
#include "StepRadioDiscovery.h"


// ============================================================
// Function pointer types for the game functions we hook
// ============================================================

using ExecCallback_t = void(__fastcall*)(void* self, void* trapInfo);
using ConvertRadioTypeToSpeechLabel_t = std::uint32_t(__fastcall*)(std::uint8_t radioType);
using AddNoticeInfo_t = bool(__fastcall*)(void* self, std::uint32_t soldierIndex, const void* noticeBlob);
using StateRadioRequest_t = void(__fastcall*)(void* self, int actionIndex, int stateProc);
using GetNameIdWithGameObjectId_t = std::uint32_t(__fastcall*)(std::uint16_t gameObjectId);


// ============================================================
// Game addresses
// ============================================================

static constexpr std::uintptr_t ADDR_ExecCallback = 0x140A19030ull;
static constexpr std::uintptr_t ADDR_ConvertRadioTypeToLabel = 0x140D685C0ull;
static constexpr std::uintptr_t ADDR_AddNoticeInfo = 0x1414DCB60ull;
static constexpr std::uintptr_t ADDR_StateRadioRequest = 0x14A2ACC00ull;
static constexpr std::uintptr_t ADDR_GetNameIdWithGameObjectId = 0x146C98180ull;


// ============================================================
// Game constants
// ============================================================

static constexpr std::uint8_t  RADIO_PRISONER_GONE = 0x1Au;
static constexpr std::uint8_t  NOTICE_ESCAPE_OBJECT = 0x21u;
static constexpr std::uint8_t  NOTICE_ESCAPE_PRELUDE = 0x32u;
static constexpr std::uint16_t ESCAPE_OBJECT_ID_BASE = 0x6200u;

static constexpr int HOSTAGE_MALE = 0;
static constexpr int HOSTAGE_FEMALE = 1;
static constexpr int HOSTAGE_CHILD = 2;

static constexpr int SOURCE_NONE = 0;
static constexpr int SOURCE_NOTICE = 1;
static constexpr int SOURCE_RADIO = 2;

static constexpr std::uint32_t LABEL_MALE_NOT_TAKEN = 0xFA42F4E9u;
static constexpr std::uint32_t LABEL_MALE_TAKEN = 0x43ED2D08u;
static constexpr std::uint32_t LABEL_FEMALE_NOT_TAKEN = 0xBAE03A98u;
static constexpr std::uint32_t LABEL_FEMALE_TAKEN = 0xD586CA7Bu;
static constexpr std::uint32_t LABEL_CHILD_NOT_TAKEN = 0x2A2B54E0u;
static constexpr std::uint32_t LABEL_CHILD_TAKEN = 0x96902568u;


// ============================================================
// Data structures
// ============================================================

struct TrackedHostage
{
    std::uint16_t objectId = 0xFFFFu;
    int           type = HOSTAGE_MALE;
    int           nameId = -1;
    bool          playerTookIt = false;
};

struct EscapedHostage
{
    bool           active = false;
    std::uint16_t  objectId = 0xFFFFu;
    int            type = -1;
    int            moverNameId = -1;
    int            expectedNameId = -1;
    bool           playerTookIt = false;
    std::uintptr_t trapSelf = 0;
    std::uintptr_t cbData = 0;
    std::uintptr_t cbData40 = 0;
};

struct PendingReport
{
    bool           active = false;
    std::uint32_t  soldierIndex = 0xFFFFFFFFu;
    std::uint16_t  hostageObjId = 0xFFFFu;
    int            hostageType = -1;
    int            requestType = -1;
    int            moverNameId = -1;
    int            expectedNameId = -1;
    bool           playerTookIt = false;
    int            source = SOURCE_NONE;
    int            escapeSlot = -1;
    std::uint16_t  noticeObjId = 0xFFFFu;
    std::uintptr_t trapSelf = 0;
    std::uintptr_t cbData = 0;
    std::uintptr_t cbData40 = 0;
};


// ============================================================
// Global state
// ============================================================

static ExecCallback_t                  g_OrigExecCallback = nullptr;
static ConvertRadioTypeToSpeechLabel_t g_OrigConvertLabel = nullptr;
static AddNoticeInfo_t                 g_OrigAddNoticeInfo = nullptr;
static StateRadioRequest_t             g_OrigRadioRequest = nullptr;
static GetNameIdWithGameObjectId_t     g_GetNameId = nullptr;

static std::unordered_map<std::uint16_t, TrackedHostage> g_HostagesByObjectId;
static std::unordered_map<int, TrackedHostage> g_HostagesByNameId;
static std::vector<EscapedHostage>                       g_EscapedHostages;
static std::unordered_map<std::uint32_t, PendingReport>  g_PendingBySoldier;
static PendingReport                                     g_SelectedReport;
static std::mutex                                        g_Mutex;


// ============================================================
// Small utilities
// ============================================================

static const char* HostageTypeName(int type)
{
    if (type == HOSTAGE_MALE)   return "MALE";
    if (type == HOSTAGE_FEMALE) return "FEMALE";
    if (type == HOSTAGE_CHILD)  return "CHILD";
    return "UNKNOWN";
}

static const char* YesNo(bool value) { return value ? "YES" : "NO"; }

static const char* SourceName(int source)
{
    if (source == SOURCE_NOTICE) return "NOTICE";
    if (source == SOURCE_RADIO)  return "RADIO";
    return "NONE";
}

static std::uint32_t PickSpeechLabel(int hostageType, bool playerTookIt)
{
    if (hostageType == HOSTAGE_MALE)   return playerTookIt ? LABEL_MALE_TAKEN : LABEL_MALE_NOT_TAKEN;
    if (hostageType == HOSTAGE_FEMALE) return playerTookIt ? LABEL_FEMALE_TAKEN : LABEL_FEMALE_NOT_TAKEN;
    if (hostageType == HOSTAGE_CHILD)  return playerTookIt ? LABEL_CHILD_TAKEN : LABEL_CHILD_NOT_TAKEN;
    return 0;
}


// ============================================================
// Safe memory reads
// Each one returns false if the address is null or causes
// an access violation, instead of crashing the game.
// ============================================================

static bool ReadU8(std::uintptr_t addr, std::uint8_t& out)
{
    if (!addr) return false;
    __try { out = *reinterpret_cast<const std::uint8_t*>(addr);  return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadU16(std::uintptr_t addr, std::uint16_t& out)
{
    if (!addr) return false;
    __try { out = *reinterpret_cast<const std::uint16_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadInt(std::uintptr_t addr, int& out)
{
    if (!addr) return false;
    __try { out = *reinterpret_cast<const int*>(addr);           return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadU64(std::uintptr_t addr, std::uint64_t& out)
{
    if (!addr) return false;
    __try { out = *reinterpret_cast<const std::uint64_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadBytes(std::uintptr_t addr, std::uint8_t* buf, std::size_t size)
{
    if (!addr || !buf || !size) return false;
    __try { std::memcpy(buf, reinterpret_cast<const void*>(addr), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}


// ============================================================
// Helpers that read structured data from game objects
// ============================================================

static bool LoadGetNameIdFunction()
{
    if (!g_GetNameId)
        g_GetNameId = reinterpret_cast<GetNameIdWithGameObjectId_t>(ResolveGameAddress(ADDR_GetNameIdWithGameObjectId));
    return g_GetNameId != nullptr;
}

static bool TryGetNameId(std::uint16_t objectId, int& outNameId)
{
    outNameId = -1;
    if (!LoadGetNameIdFunction()) return false;
    __try { outNameId = static_cast<int>(g_GetNameId(objectId)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { outNameId = -1; return false; }
}

static bool ReadTrapPointers(void* self, std::uintptr_t& outCbData, std::uintptr_t& outCbData40)
{
    outCbData = outCbData40 = 0;
    if (!self) return false;

    std::uint64_t cbData = 0;
    if (!ReadU64(reinterpret_cast<std::uintptr_t>(self) + 0x10ull, cbData) || !cbData) return false;

    std::uint64_t cbData40 = 0;
    if (!ReadU64(static_cast<std::uintptr_t>(cbData) + 0x40ull, cbData40) || !cbData40) return false;

    outCbData = static_cast<std::uintptr_t>(cbData);
    outCbData40 = static_cast<std::uintptr_t>(cbData40);
    return true;
}

static bool ReadExpectedNameId(void* self, int& outNameId)
{
    outNameId = -1;
    std::uintptr_t cbData = 0, cbData40 = 0;
    if (!ReadTrapPointers(self, cbData, cbData40)) return false;
    return ReadInt(cbData40 + 0x10ull, outNameId);
}

static bool ReadCurrentNoticeType(void* self, std::uint32_t soldierIndex, std::uint32_t& outType)
{
    outType = 0;
    if (!self) return false;

    std::uint64_t slotsBase = 0;
    if (!ReadU64(reinterpret_cast<std::uintptr_t>(self) + 0x40ull, slotsBase) || !slotsBase) return false;

    std::uintptr_t slot = static_cast<std::uintptr_t>(slotsBase) + soldierIndex * 0x80ull;

    std::uint8_t compactType = 0;
    if (!ReadU8(slot, compactType)) return false;

    if (compactType == 0x3Eu)
    {
        int fullType = 0;
        if (!ReadInt(slot + 0x60ull, fullType)) return false;
        outType = static_cast<std::uint32_t>(fullType);
    }
    else
    {
        outType = compactType;
    }
    return true;
}

static void BuildHexDump(std::uintptr_t addr, char* buf, std::size_t bufSize)
{
    if (!buf || !bufSize) return;
    buf[0] = '\0';
    std::uint8_t bytes[16] = {};
    if (!ReadBytes(addr, bytes, 16)) { std::snprintf(buf, bufSize, "<read failed>"); return; }
    std::snprintf(buf, bufSize,
        "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

static bool ParseNoticeBlob(const void* blob, std::uint8_t& outType, std::uint16_t& outObjId, int& outSlot)
{
    outType = 0xFFu; outObjId = 0xFFFFu; outSlot = -1;
    if (!blob) return false;

    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(blob);
    if (!ReadU8(addr, outType)) return false;
    if (outType != NOTICE_ESCAPE_OBJECT) return true;
    if (!ReadU16(addr + 0x2ull, outObjId)) return false;
    if (outObjId >= ESCAPE_OBJECT_ID_BASE)
        outSlot = static_cast<int>(outObjId - ESCAPE_OBJECT_ID_BASE);
    return true;
}

static bool ReadRadioEntry(void* self, int actionIndex, std::uint32_t& outSoldierIndex, std::uint8_t& outRadioType)
{
    outSoldierIndex = 0xFFFFFFFFu; outRadioType = 0xFFu;
    if (!self) return false;

    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(self);
    std::uint64_t listBase = 0;
    int baseIndex = 0;
    if (!ReadU64(base + 0x88ull, listBase) || !listBase) return false;
    if (!ReadInt(base + 0x90ull, baseIndex)) return false;

    int relativeIndex = actionIndex - baseIndex;
    if (relativeIndex < 0) return false;

    std::uintptr_t entry = static_cast<std::uintptr_t>(listBase) + static_cast<std::uintptr_t>(relativeIndex) * 0x18ull;

    int soldierIndex = -1;
    ReadInt(entry + 0x08ull, soldierIndex);
    ReadU8(entry + 0x10ull, outRadioType);

    outSoldierIndex = static_cast<std::uint32_t>(soldierIndex);
    return true;
}


// ============================================================
// State helpers
// ============================================================

static bool FindTrackedHostage(std::uint16_t objectId, int moverNameId, int expectedNameId, TrackedHostage& out)
{
    std::lock_guard<std::mutex> lock(g_Mutex);

    auto it = g_HostagesByObjectId.find(objectId);
    if (it != g_HostagesByObjectId.end())
    {
        out = it->second;
        Log("[LostHostage] Matched hostage by objectId=0x%04X\n", static_cast<unsigned>(objectId));
        return true;
    }

    if (moverNameId != -1)
    {
        auto it2 = g_HostagesByNameId.find(moverNameId);
        if (it2 != g_HostagesByNameId.end())
        {
            out = it2->second;
            Log("[LostHostage] Matched hostage by moverNameId=%d\n", moverNameId);
            return true;
        }
    }

    if (expectedNameId != -1)
    {
        auto it3 = g_HostagesByNameId.find(expectedNameId);
        if (it3 != g_HostagesByNameId.end())
        {
            out = it3->second;
            Log("[LostHostage] Matched hostage by expectedNameId=%d\n", expectedNameId);
            return true;
        }
    }

    Log("[LostHostage] No match. Tracked: byId=%u byName=%u\n",
        static_cast<unsigned>(g_HostagesByObjectId.size()),
        static_cast<unsigned>(g_HostagesByNameId.size()));
    for (const auto& kv : g_HostagesByObjectId)
        Log("[LostHostage]   objectId=0x%04X type=%s nameId=%d playerTook=%s\n",
            static_cast<unsigned>(kv.second.objectId), HostageTypeName(kv.second.type),
            kv.second.nameId, YesNo(kv.second.playerTookIt));
    return false;
}

static int RecordEscapedHostage(const TrackedHostage& h, int moverNameId, int expectedNameId,
    void* trapSelf, std::uintptr_t cbData, std::uintptr_t cbData40)
{
    for (std::size_t i = 0; i < g_EscapedHostages.size(); ++i)
    {
        EscapedHostage& e = g_EscapedHostages[i];
        if (!e.active || e.objectId != h.objectId) continue;
        e.type = h.type;
        e.moverNameId = moverNameId;
        e.expectedNameId = expectedNameId;
        e.playerTookIt = h.playerTookIt;
        e.trapSelf = reinterpret_cast<std::uintptr_t>(trapSelf);
        e.cbData = cbData;
        e.cbData40 = cbData40;
        return static_cast<int>(i);
    }

    EscapedHostage e{};
    e.active = true;
    e.objectId = h.objectId;
    e.type = h.type;
    e.moverNameId = moverNameId;
    e.expectedNameId = expectedNameId;
    e.playerTookIt = h.playerTookIt;
    e.trapSelf = reinterpret_cast<std::uintptr_t>(trapSelf);
    e.cbData = cbData;
    e.cbData40 = cbData40;
    g_EscapedHostages.push_back(e);
    return static_cast<int>(g_EscapedHostages.size() - 1);
}

static PendingReport BuildPendingReport(std::uint32_t soldierIndex, const EscapedHostage& e,
    int requestType, int source, int slot, std::uint16_t noticeObjId)
{
    PendingReport r{};
    r.active = true;
    r.soldierIndex = soldierIndex;
    r.hostageObjId = e.objectId;
    r.hostageType = e.type;
    r.requestType = requestType;
    r.moverNameId = e.moverNameId;
    r.expectedNameId = e.expectedNameId;
    r.playerTookIt = e.playerTookIt;
    r.source = source;
    r.escapeSlot = slot;
    r.noticeObjId = noticeObjId;
    r.trapSelf = e.trapSelf;
    r.cbData = e.cbData;
    r.cbData40 = e.cbData40;
    return r;
}

static void ClearSelectedReport(const char* reason)
{
    if (!g_SelectedReport.active) return;
    Log("[LostHostage] ClearSelectedReport reason=%s soldierIndex=%u hostageObjId=0x%04X type=%s source=%s slot=%d\n",
        reason, static_cast<unsigned>(g_SelectedReport.soldierIndex),
        static_cast<unsigned>(g_SelectedReport.hostageObjId),
        HostageTypeName(g_SelectedReport.hostageType),
        SourceName(g_SelectedReport.source),
        g_SelectedReport.escapeSlot);
    g_SelectedReport = {};
}


// ============================================================
// Hooks
// ============================================================

static void __fastcall hkExecCallback(void* self, void* trapInfo)
{
    if (!g_OrigExecCallback) return;
    if (MissionCodeGuard::ShouldBypassHooks()) { g_OrigExecCallback(self, trapInfo); return; }

    int            mapRequest = -1;
    std::uint16_t  moverObjectId = 0xFFFFu;
    int            moverNameId = -1;
    int            expectedNameId = -1;
    std::uintptr_t cbData = 0;
    std::uintptr_t cbData40 = 0;

    const bool readOk =
        ReadInt(reinterpret_cast<std::uintptr_t>(trapInfo) + 0xD0ull, mapRequest) &&
        ReadU16(reinterpret_cast<std::uintptr_t>(trapInfo) + 0x68ull, moverObjectId);

    if (readOk)
    {
        ReadTrapPointers(self, cbData, cbData40);
        ReadExpectedNameId(self, expectedNameId);
        TryGetNameId(moverObjectId, moverNameId);

        const bool nameMatch = (moverNameId != -1 && moverNameId == expectedNameId);

        if (mapRequest != 4)
            Log("[LostHostageTrap] request=%d moverObjId=0x%04X moverNameId=%d expectedNameId=%d nameMatch=%s\n",
                mapRequest, static_cast<unsigned>(moverObjectId), moverNameId, expectedNameId, YesNo(nameMatch));

        if (mapRequest == 2 && moverObjectId != 0xFFFFu)
        {
            TrackedHostage hostage{};
            const bool found = FindTrackedHostage(moverObjectId, moverNameId, expectedNameId, hostage);

            if (found)
            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                const int slot = RecordEscapedHostage(hostage, moverNameId, expectedNameId, self, cbData, cbData40);
                Log("[LostHostage] Escape recorded slot=%d objectId=0x%04X type=%s playerTook=%s\n",
                    slot, static_cast<unsigned>(hostage.objectId), HostageTypeName(hostage.type), YesNo(hostage.playerTookIt));
            }
            else
            {
                Log("[LostHostage] Escape ignored (untracked) moverObjId=0x%04X\n",
                    static_cast<unsigned>(moverObjectId));
            }
        }
    }

    g_OrigExecCallback(self, trapInfo);
}

static bool __fastcall hkAddNoticeInfo(void* self, std::uint32_t soldierIndex, const void* noticeBlob)
{
    if (!g_OrigAddNoticeInfo) return false;
    if (MissionCodeGuard::ShouldBypassHooks()) return g_OrigAddNoticeInfo(self, soldierIndex, noticeBlob);

    std::uint8_t  noticeType = 0xFFu;
    std::uint16_t noticeObjId = 0xFFFFu;
    int           slot = -1;
    char          hexDump[16 * 3] = {};

    ParseNoticeBlob(noticeBlob, noticeType, noticeObjId, slot);
    if (noticeBlob)
        BuildHexDump(reinterpret_cast<std::uintptr_t>(noticeBlob), hexDump, sizeof(hexDump));

    std::uint32_t noticeBefore = 0;
    const bool hadBefore = ReadCurrentNoticeType(self, soldierIndex, noticeBefore);

    const bool accepted = g_OrigAddNoticeInfo(self, soldierIndex, noticeBlob);
    if (!accepted) return false;

    if (noticeType != NOTICE_ESCAPE_OBJECT && noticeType != NOTICE_ESCAPE_PRELUDE)
        return true;

    std::uint32_t noticeAfter = 0;
    const bool hasAfter = ReadCurrentNoticeType(self, soldierIndex, noticeAfter);

    Log("[LostHostageNotice] soldierIndex=%u noticeBefore=%s0x%X noticeAfter=%s0x%X newType=0x%02X blob=[%s]\n",
        static_cast<unsigned>(soldierIndex),
        hadBefore ? "" : "?", static_cast<unsigned>(noticeBefore),
        hasAfter ? "" : "?", static_cast<unsigned>(noticeAfter),
        static_cast<unsigned>(noticeType), hexDump);

    if (noticeType != NOTICE_ESCAPE_OBJECT) return true;

    std::lock_guard<std::mutex> lock(g_Mutex);

    if (slot < 0)
    {
        Log("[LostHostage] AddNoticeInfo: bad slot soldierIndex=%u noticeObjId=0x%04X\n",
            static_cast<unsigned>(soldierIndex), static_cast<unsigned>(noticeObjId));
        return true;
    }

    if (static_cast<std::size_t>(slot) >= g_EscapedHostages.size() || !g_EscapedHostages[slot].active)
    {
        Log("[LostHostage] AddNoticeInfo: no escaped entry for slot=%d soldierIndex=%u\n",
            slot, static_cast<unsigned>(soldierIndex));
        return true;
    }

    const EscapedHostage& escaped = g_EscapedHostages[slot];
    const PendingReport report = BuildPendingReport(soldierIndex, escaped, NOTICE_ESCAPE_OBJECT, SOURCE_NOTICE, slot, noticeObjId);
    g_PendingBySoldier[soldierIndex] = report;

    Log("[LostHostage] Pending stored soldierIndex=%u slot=%d noticeObjId=0x%04X hostageObjId=0x%04X type=%s playerTook=%s\n",
        static_cast<unsigned>(soldierIndex), slot, static_cast<unsigned>(noticeObjId),
        static_cast<unsigned>(escaped.objectId), HostageTypeName(escaped.type), YesNo(escaped.playerTookIt));

    return true;
}

static void __fastcall hkStateRadioRequest(void* self, int actionIndex, int stateProc)
{
    if (!g_OrigRadioRequest) return;
    if (MissionCodeGuard::ShouldBypassHooks()) { g_OrigRadioRequest(self, actionIndex, stateProc); return; }

    g_OrigRadioRequest(self, actionIndex, stateProc);

    // From the logs: stateProc=0 is when byte12 flips to 0x02, meaning the
    // radio is actually about to transmit. All other stateProc values fire too
    // early or too late relative to ConvertRadioTypeToSpeechLabel being called.
    if (stateProc != 0) return;

    // Let the discovery helper inspect the same shared RadioRequest event.
    LostHostageDiscovery_OnRadioRequest(self, actionIndex, stateProc);

    std::uint32_t soldierIndex = 0xFFFFFFFFu;
    std::uint8_t  radioType = 0xFFu;
    if (!ReadRadioEntry(self, actionIndex, soldierIndex, radioType)) return;
    if (radioType != RADIO_PRISONER_GONE) return;

    std::lock_guard<std::mutex> lock(g_Mutex);

    auto it = g_PendingBySoldier.find(soldierIndex);
    if (it == g_PendingBySoldier.end())
    {
        Log("[LostHostage] RadioRequest: no pending for soldierIndex=%u\n", static_cast<unsigned>(soldierIndex));
        return;
    }

    g_SelectedReport = it->second;
    g_SelectedReport.source = SOURCE_RADIO;
    g_PendingBySoldier.erase(it);

    Log("[LostHostage] RadioRequest: selected report soldierIndex=%u hostageObjId=0x%04X type=%s slot=%d playerTook=%s\n",
        static_cast<unsigned>(g_SelectedReport.soldierIndex),
        static_cast<unsigned>(g_SelectedReport.hostageObjId),
        HostageTypeName(g_SelectedReport.hostageType),
        g_SelectedReport.escapeSlot,
        YesNo(g_SelectedReport.playerTookIt));
}

static std::uint32_t __fastcall hkConvertRadioTypeToSpeechLabel(std::uint8_t radioType)
{
    if (!g_OrigConvertLabel) return 0;
    if (MissionCodeGuard::ShouldBypassHooks()) return g_OrigConvertLabel(radioType);

    const std::uint32_t defaultLabel = g_OrigConvertLabel(radioType);

    if (radioType == RADIO_PRISONER_GONE)
    {
        PendingReport report{};
        bool hasReport = false;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (g_SelectedReport.active)
            {
                report = g_SelectedReport;
                hasReport = true;
                ClearSelectedReport("label consumed");
            }
        }

        if (hasReport)
        {
            const std::uint32_t overrideLabel = PickSpeechLabel(report.hostageType, report.playerTookIt);
            if (overrideLabel)
            {
                Log("[LostHostageRadio] Override radioType=0x%02X default=0x%08X override=0x%08X soldierIndex=%u source=%s hostageObjId=0x%04X type=%s slot=%d playerTook=%s\n",
                    static_cast<unsigned>(radioType), static_cast<unsigned>(defaultLabel), static_cast<unsigned>(overrideLabel),
                    static_cast<unsigned>(report.soldierIndex), SourceName(report.source),
                    static_cast<unsigned>(report.hostageObjId), HostageTypeName(report.hostageType),
                    report.escapeSlot, YesNo(report.playerTookIt));

                return overrideLabel;
            }
        }
    }

    return LostHostageDiscovery_OnConvertRadioTypeToSpeechLabel(radioType, defaultLabel);
}


// ============================================================
// Public API
// ============================================================

void Add_LostHostageTrap(std::uint32_t gameObjectId, int hostageType)
{
    if (hostageType < HOSTAGE_MALE || hostageType > HOSTAGE_CHILD)
    {
        Log("[LostHostage] Add_LostHostage: invalid type=%d for objectId=0x%08X\n", hostageType, gameObjectId);
        return;
    }

    const std::uint16_t rawId = static_cast<std::uint16_t>(gameObjectId);
    int nameId = -1;
    TryGetNameId(rawId, nameId);

    TrackedHostage h{};
    h.objectId = rawId;
    h.type = hostageType;
    h.nameId = nameId;
    h.playerTookIt = false;

    std::lock_guard<std::mutex> lock(g_Mutex);
    g_HostagesByObjectId[rawId] = h;
    if (nameId != -1)
        g_HostagesByNameId[nameId] = h;

    Log("[LostHostage] Add_LostHostage: tracking objectId=0x%04X type=%s nameId=%d\n",
        static_cast<unsigned>(rawId), HostageTypeName(hostageType), nameId);
}

void Remove_LostHostageTrap(std::uint32_t gameObjectId)
{
    const std::uint16_t rawId = static_cast<std::uint16_t>(gameObjectId);

    std::lock_guard<std::mutex> lock(g_Mutex);

    auto it = g_HostagesByObjectId.find(rawId);
    if (it != g_HostagesByObjectId.end())
    {
        const int nameId = it->second.nameId;
        g_HostagesByObjectId.erase(it);
        if (nameId != -1) g_HostagesByNameId.erase(nameId);
    }

    for (EscapedHostage& e : g_EscapedHostages)
        if (e.active && e.objectId == rawId) e.active = false;

    for (auto it2 = g_PendingBySoldier.begin(); it2 != g_PendingBySoldier.end();)
        it2 = (it2->second.hostageObjId == rawId) ? g_PendingBySoldier.erase(it2) : std::next(it2);

    if (g_SelectedReport.active && g_SelectedReport.hostageObjId == rawId)
        ClearSelectedReport("hostage removed");

    Log("[LostHostage] Remove_LostHostage: objectId=0x%04X\n", static_cast<unsigned>(rawId));
}

void Clear_LostHostagesTrap()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_HostagesByObjectId.clear();
    g_HostagesByNameId.clear();
    g_EscapedHostages.clear();
    g_PendingBySoldier.clear();
    g_SelectedReport = {};
    Log("[LostHostage] Clear_LostHostages\n");
}

void PlayerTookHostage(std::uint32_t gameObjectId, bool playerTookIt)
{
    const std::uint16_t rawId = static_cast<std::uint16_t>(gameObjectId);

    std::lock_guard<std::mutex> lock(g_Mutex);

    auto it = g_HostagesByObjectId.find(rawId);
    if (it == g_HostagesByObjectId.end())
    {
        Log("[LostHostage] PlayerTookHostage: objectId=0x%08X not tracked\n", gameObjectId);
        return;
    }

    it->second.playerTookIt = playerTookIt;

    const int nameId = it->second.nameId;
    if (nameId != -1)
    {
        auto itName = g_HostagesByNameId.find(nameId);
        if (itName != g_HostagesByNameId.end())
            itName->second.playerTookIt = playerTookIt;
    }

    for (EscapedHostage& e : g_EscapedHostages)
        if (e.active && e.objectId == rawId) e.playerTookIt = playerTookIt;

    for (auto& kv : g_PendingBySoldier)
        if (kv.second.hostageObjId == rawId) kv.second.playerTookIt = playerTookIt;

    if (g_SelectedReport.active && g_SelectedReport.hostageObjId == rawId)
        g_SelectedReport.playerTookIt = playerTookIt;

    Log("[LostHostage] PlayerTookHostage: objectId=0x%08X playerTook=%s\n", gameObjectId, YesNo(playerTookIt));
}

bool Install_LostHostage_Hooks()
{
    void* addrExec = ResolveGameAddress(ADDR_ExecCallback);
    void* addrConvert = ResolveGameAddress(ADDR_ConvertRadioTypeToLabel);
    void* addrNotice = ResolveGameAddress(ADDR_AddNoticeInfo);
    void* addrRadio = ResolveGameAddress(ADDR_StateRadioRequest);

    Log("======== LOSTHOSTAGE BUILD MARKER ========\n");

    if (!addrExec || !addrConvert || !addrNotice || !addrRadio)
    {
        Log("[LostHostage] Install failed: exec=%p convert=%p notice=%p radio=%p\n",
            addrExec, addrConvert, addrNotice, addrRadio);
        return false;
    }

    LoadGetNameIdFunction();

    const bool okExec = CreateAndEnableHook(addrExec, reinterpret_cast<void*>(&hkExecCallback), reinterpret_cast<void**>(&g_OrigExecCallback));
    const bool okConvert = CreateAndEnableHook(addrConvert, reinterpret_cast<void*>(&hkConvertRadioTypeToSpeechLabel), reinterpret_cast<void**>(&g_OrigConvertLabel));
    const bool okNotice = CreateAndEnableHook(addrNotice, reinterpret_cast<void*>(&hkAddNoticeInfo), reinterpret_cast<void**>(&g_OrigAddNoticeInfo));
    const bool okRadio = CreateAndEnableHook(addrRadio, reinterpret_cast<void*>(&hkStateRadioRequest), reinterpret_cast<void**>(&g_OrigRadioRequest));

    Log("[LostHostage] Hook ExecCallback:   %s\n", okExec ? "OK" : "FAIL");
    Log("[LostHostage] Hook ConvertLabel:   %s\n", okConvert ? "OK" : "FAIL");
    Log("[LostHostage] Hook AddNoticeInfo:  %s\n", okNotice ? "OK" : "FAIL");
    Log("[LostHostage] Hook RadioRequest:   %s target=%p orig=%p\n",
        okRadio ? "OK" : "FAIL", addrRadio, reinterpret_cast<void*>(g_OrigRadioRequest));

    return okExec && okConvert && okNotice && okRadio;
}

bool Uninstall_LostHostage_Hooks()
{
    DisableAndRemoveHook(ResolveGameAddress(ADDR_ExecCallback));
    DisableAndRemoveHook(ResolveGameAddress(ADDR_ConvertRadioTypeToLabel));
    DisableAndRemoveHook(ResolveGameAddress(ADDR_AddNoticeInfo));
    DisableAndRemoveHook(ResolveGameAddress(ADDR_StateRadioRequest));

    g_OrigExecCallback = nullptr;
    g_OrigConvertLabel = nullptr;
    g_OrigAddNoticeInfo = nullptr;
    g_OrigRadioRequest = nullptr;

    std::lock_guard<std::mutex> lock(g_Mutex);
    g_HostagesByObjectId.clear();
    g_HostagesByNameId.clear();
    g_EscapedHostages.clear();
    g_PendingBySoldier.clear();
    g_SelectedReport = {};

    Log("[LostHostage] Uninstall_LostHostage_Hooks\n");
    return true;
}
