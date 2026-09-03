#include "pch.h"

#include <Windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "PlayerAlertPhase.h"
#include "HostageGender.h"
#include "MissionCodeGuard.h"
#include "LostHostageHook.h"
#include "NoticeControllerImpl_CheckSightNoticePlayer.h"
#include "StepRadioDiscovery.h"
#include "AddressSet.h"
#include "FoxHashes.h"


using ConvertRadioTypeToSpeechLabel_t = std::uint32_t(__fastcall*)(std::uint8_t radioType);
using AddNoticeInfo_t = bool(__fastcall*)(void* self, std::uint32_t soldierIndex, const void* noticeBlob);
using StateRadioRequest_t = void(__fastcall*)(void* self, int actionIndex, int stateProc);
using GetNameIdWithGameObjectId_t = std::uint32_t(__fastcall*)(std::uint16_t gameObjectId);
using GetQuarkSystemTable_t = void* (__fastcall*)();


static constexpr std::uint8_t  RADIO_PRISONER_GONE = 0x1Au;
static constexpr std::uint8_t  NOTICE_ESCAPE_OBJECT = 0x21u;
static constexpr std::uint16_t ESCAPE_OBJECT_ID_BASE = 0x6200u;
static constexpr std::uint8_t  NOTICE_OBJECT_TYPE_LOST = 0x01u;
static constexpr int           kNoticeObjectSlotCount = 24;
static constexpr std::size_t   kNoticeObjectSlotStride = 0x20;
static constexpr std::size_t   kNoticeObjectSlotsBaseOffset = 0x24;
static constexpr std::size_t   kQuarkSystemTable_Offset0 = 0x98;
static constexpr std::size_t   kQuarkSystemTable_Offset1 = 0x1D8;

static constexpr int HOSTAGE_MALE = 0;
static constexpr int HOSTAGE_FEMALE = 1;
static constexpr int HOSTAGE_CHILD = 2;

static constexpr int SOURCE_NONE = 0;
static constexpr int SOURCE_NOTICE = 1;
static constexpr int SOURCE_RADIO = 2;


static constexpr const char* LANGID_MALE_NOT_TAKEN   = "CPR0230";
static constexpr const char* LANGID_FEMALE_NOT_TAKEN = "V_CPR0250";
static constexpr const char* LANGID_CHILD_NOT_TAKEN  = "V_CPR0271";
static constexpr const char* LANGID_MALE_TAKEN       = "CPR0061";
static constexpr const char* LANGID_FEMALE_TAKEN     = "CPR0062";
static constexpr const char* LANGID_CHILD_TAKEN      = "CPR0063";


struct TrackedHostage
{
    std::uint16_t objectId = 0xFFFFu;
    int           type = HOSTAGE_MALE;
    int           nameId = -1;
    std::uint32_t customLostLabel = 0;
    std::uint32_t customLostLabelTaken = 0;
};

struct PendingReport
{
    bool           active = false;
    std::uint32_t  soldierIndex = 0xFFFFFFFFu;
    std::uint16_t  hostageObjId = 0xFFFFu;
    int            hostageType = -1;
    int            source = SOURCE_NONE;
    int            slotIndex = -1;
    std::uint16_t  noticeObjId = 0xFFFFu;
    std::uint32_t  customLostLabel = 0;
    std::uint32_t  customLostLabelTaken = 0;
};


static ConvertRadioTypeToSpeechLabel_t g_OrigConvertLabel = nullptr;
static AddNoticeInfo_t                 g_OrigAddNoticeInfo = nullptr;
static StateRadioRequest_t             g_OrigRadioRequest = nullptr;
static GetNameIdWithGameObjectId_t     g_GetNameId = nullptr;
static GetQuarkSystemTable_t           g_GetQuarkSystemTable = nullptr;

static std::unordered_map<std::uint16_t, TrackedHostage> g_HostagesByObjectId;
static std::unordered_map<int, TrackedHostage>           g_HostagesByNameId;
static std::unordered_map<std::uint32_t, PendingReport>  g_PendingBySoldier;
static PendingReport                                     g_SelectedReport;
static std::mutex                                        g_Mutex;


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
    static std::uint32_t male_not_taken   = 0;
    static std::uint32_t female_not_taken = 0;
    static std::uint32_t child_not_taken  = 0;
    static std::uint32_t male_taken       = 0;
    static std::uint32_t female_taken     = 0;
    static std::uint32_t child_taken      = 0;

    if (male_not_taken == 0)
    {
        male_not_taken   = FoxHashes::StrCode32(LANGID_MALE_NOT_TAKEN);
        female_not_taken = FoxHashes::StrCode32(LANGID_FEMALE_NOT_TAKEN);
        child_not_taken  = FoxHashes::StrCode32(LANGID_CHILD_NOT_TAKEN);
        male_taken       = FoxHashes::StrCode32(LANGID_MALE_TAKEN);
        female_taken     = FoxHashes::StrCode32(LANGID_FEMALE_TAKEN);
        child_taken      = FoxHashes::StrCode32(LANGID_CHILD_TAKEN);
    }

    if (playerTookIt)
    {
        if (hostageType == HOSTAGE_FEMALE) return female_taken;
        if (hostageType == HOSTAGE_CHILD)  return child_taken;
        return male_taken;
    }
    if (hostageType == HOSTAGE_FEMALE) return female_not_taken;
    if (hostageType == HOSTAGE_CHILD)  return child_not_taken;
    return male_not_taken;
}


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

static bool LoadGetNameIdFunction()
{
    if (!g_GetNameId)
        g_GetNameId = reinterpret_cast<GetNameIdWithGameObjectId_t>(ResolveGameAddress(gAddr.GetNameIdWithGameObjectId));
    return g_GetNameId != nullptr;
}

static bool TryGetNameId(std::uint16_t objectId, int& outNameId)
{
    outNameId = -1;
    if (!LoadGetNameIdFunction()) return false;
    __try { outNameId = static_cast<int>(g_GetNameId(objectId)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { outNameId = -1; return false; }
}

static bool LoadGetQuarkSystemTable()
{
    if (!g_GetQuarkSystemTable)
        g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
            ResolveGameAddress(gAddr.GetQuarkSystemTable));
    return g_GetQuarkSystemTable != nullptr;
}

static bool ReadNoticeObjectOwner(int slotIndex,
                                  std::uint16_t& outOwnerId,
                                  std::uint8_t& outType,
                                  std::uint8_t& outFlags)
{
    outOwnerId = 0xFFFFu;
    outType = 0xFFu;
    outFlags = 0u;

    if (slotIndex < 0 || slotIndex >= kNoticeObjectSlotCount) return false;
    if (!LoadGetQuarkSystemTable()) return false;

    void* qst = nullptr;
    __try { qst = g_GetQuarkSystemTable(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!qst) return false;

    const std::uintptr_t qstAddr = reinterpret_cast<std::uintptr_t>(qst);

    std::uint64_t intermediate = 0;
    if (!ReadU64(qstAddr + kQuarkSystemTable_Offset0, intermediate) || !intermediate)
        return false;

    std::uint64_t nos = 0;
    if (!ReadU64(static_cast<std::uintptr_t>(intermediate) + kQuarkSystemTable_Offset1, nos)
        || !nos)
        return false;

    const std::uintptr_t slotBase = static_cast<std::uintptr_t>(nos)
        + kNoticeObjectSlotsBaseOffset
        + static_cast<std::uintptr_t>(slotIndex) * kNoticeObjectSlotStride;

    if (!ReadU16(slotBase + 0x00, outOwnerId)) return false;
    if (!ReadU8(slotBase + 0x04, outType))     return false;
    if (!ReadU8(slotBase + 0x05, outFlags))    return false;
    return true;
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


static bool LookupTrackedByObjectId_NoLock(std::uint16_t objectId, TrackedHostage& out)
{
    auto it = g_HostagesByObjectId.find(objectId);
    if (it == g_HostagesByObjectId.end()) return false;
    out = it->second;
    return true;
}

static PendingReport BuildPendingReport(std::uint32_t soldierIndex,
                                        const TrackedHostage& hostage,
                                        int slotIndex,
                                        std::uint16_t noticeObjId)
{
    PendingReport r{};
    r.active = true;
    r.soldierIndex = soldierIndex;
    r.hostageObjId = hostage.objectId;
    r.hostageType = hostage.type;
    r.source = SOURCE_NOTICE;
    r.slotIndex = slotIndex;
    r.noticeObjId = noticeObjId;
    r.customLostLabel = hostage.customLostLabel;
    r.customLostLabelTaken = hostage.customLostLabelTaken;
    return r;
}

static void ClearSelectedReport(const char* reason)
{
    if (!g_SelectedReport.active) return;
    g_SelectedReport = {};
}


static bool __fastcall hkAddNoticeInfo(void* self, std::uint32_t soldierIndex, const void* noticeBlob)
{
    if (!g_OrigAddNoticeInfo) return false;
    if (MissionCodeGuard::ShouldBypassHooks()) return g_OrigAddNoticeInfo(self, soldierIndex, noticeBlob);

    std::uint8_t  noticeType = 0xFFu;
    std::uint16_t noticeObjId = 0xFFFFu;
    int           slot = -1;

    ParseNoticeBlob(noticeBlob, noticeType, noticeObjId, slot);

    if (SoldierNotice_ShouldDropNotice(soldierIndex, noticeType, _ReturnAddress()))
        return false;

    const bool accepted = g_OrigAddNoticeInfo(self, soldierIndex, noticeBlob);
    if (!accepted) return false;

    if (noticeType != NOTICE_ESCAPE_OBJECT) return true;

    if (slot < 0)
    {
        return true;
    }

    std::uint16_t ownerId = 0xFFFFu;
    std::uint8_t  slotType = 0xFFu;
    std::uint8_t  slotFlags = 0u;
    if (!ReadNoticeObjectOwner(slot, ownerId, slotType, slotFlags))
    {
        return true;
    }

    if ((slotFlags & 0x1u) == 0u || slotType != NOTICE_OBJECT_TYPE_LOST)
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_Mutex);

    TrackedHostage hostage{};
    if (!LookupTrackedByObjectId_NoLock(ownerId, hostage))
    {
        return true;
    }

    const PendingReport report = BuildPendingReport(soldierIndex, hostage, slot, noticeObjId);
    g_PendingBySoldier[soldierIndex] = report;

    return true;
}

static void __fastcall hkStateRadioRequest(void* self, int actionIndex, int stateProc)
{
    if (!g_OrigRadioRequest) return;
    if (MissionCodeGuard::ShouldBypassHooks()) { g_OrigRadioRequest(self, actionIndex, stateProc); return; }

    g_OrigRadioRequest(self, actionIndex, stateProc);


    if (stateProc != 0) return;


    LostHostageDiscovery_OnRadioRequest(self, actionIndex, stateProc);

    std::uint32_t soldierIndex = 0xFFFFFFFFu;
    std::uint8_t  radioType = 0xFFu;
    if (!ReadRadioEntry(self, actionIndex, soldierIndex, radioType)) return;
    if (radioType != RADIO_PRISONER_GONE) return;

    std::lock_guard<std::mutex> lock(g_Mutex);

    auto it = g_PendingBySoldier.find(soldierIndex);
    if (it == g_PendingBySoldier.end())
    {
        return;
    }

    g_SelectedReport = it->second;
    g_SelectedReport.source = SOURCE_RADIO;
    g_PendingBySoldier.erase(it);
}

static std::uint32_t __fastcall hkConvertRadioTypeToSpeechLabel(std::uint8_t radioType)
{
    if (!g_OrigConvertLabel) return 0;
    if (MissionCodeGuard::ShouldBypassHooks()) return g_OrigConvertLabel(radioType);

    const std::uint32_t defaultLabel = g_OrigConvertLabel(radioType);

    {
        std::uint32_t discoveryLabel = 0u;
        if (LostHostageDiscovery_TryConsumeConvertOverride(radioType, discoveryLabel)
            && discoveryLabel != 0u)
        {
            return discoveryLabel;
        }
    }

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
            const bool playerTookIt =
                PlayerAlertPhase::IsAtOrAbove(PlayerAlertPhase::kCaution);

            const int nativeGender = HostageGender::Read(report.hostageObjId);
            const int hostageType  = (nativeGender != HostageGender::kUnknown)
                ? nativeGender
                : report.hostageType;

            const std::uint32_t customLabel = playerTookIt
                ? report.customLostLabelTaken
                : report.customLostLabel;

            const std::uint32_t overrideLabel = (customLabel != 0)
                ? customLabel
                : PickSpeechLabel(hostageType, playerTookIt);

            if (overrideLabel)
            {
                return overrideLabel;
            }
        }
    }

    return defaultLabel;
}


void Add_LostHostageTrap(std::uint32_t gameObjectId, int hostageType,
                         std::uint32_t customLostLabel,
                         std::uint32_t customLostLabelTaken)
{
    if (hostageType < HOSTAGE_MALE || hostageType > HOSTAGE_CHILD)
    {
        LogDebug("[LostHostage] Add_LostHostage: invalid type=%d for objectId=0x%08X\n", hostageType, gameObjectId);
        return;
    }

    const std::uint16_t rawId = static_cast<std::uint16_t>(gameObjectId);
    int nameId = -1;
    TryGetNameId(rawId, nameId);

    TrackedHostage h{};
    h.objectId = rawId;
    h.type = hostageType;
    h.nameId = nameId;
    h.customLostLabel = customLostLabel;
    h.customLostLabelTaken =
        (customLostLabelTaken != 0) ? customLostLabelTaken : customLostLabel;

    std::lock_guard<std::mutex> lock(g_Mutex);

    const auto existing = g_HostagesByObjectId.find(rawId);
    if (existing != g_HostagesByObjectId.end())

    g_HostagesByObjectId[rawId] = h;
    if (nameId != -1)
        g_HostagesByNameId[nameId] = h;
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

    for (auto it2 = g_PendingBySoldier.begin(); it2 != g_PendingBySoldier.end();)
        it2 = (it2->second.hostageObjId == rawId) ? g_PendingBySoldier.erase(it2) : std::next(it2);

    if (g_SelectedReport.active && g_SelectedReport.hostageObjId == rawId)
        ClearSelectedReport("hostage removed");
}

void Clear_LostHostagesTrap()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_HostagesByObjectId.clear();
    g_HostagesByNameId.clear();
    g_PendingBySoldier.clear();
    g_SelectedReport = {};
}


bool Install_LostHostage_Hooks()
{
    void* addrConvert = ResolveGameAddress(gAddr.ConvertRadioTypeToLabel);
    void* addrNotice = ResolveGameAddress(gAddr.AddNoticeInfo);
    void* addrRadio = ResolveGameAddress(gAddr.StateRadioRequest);

#ifdef _DEBUG
    Log("======== LOSTHOSTAGE BUILD MARKER ========\n");
#endif

    if (!addrConvert || !addrNotice || !addrRadio)
    {
        Log("[LostHostage] Install failed: convert=%p notice=%p radio=%p\n",
            addrConvert, addrNotice, addrRadio);
        return false;
    }

    LoadGetNameIdFunction();
    LoadGetQuarkSystemTable();

    const bool okConvert = CreateAndEnableHook(addrConvert, reinterpret_cast<void*>(&hkConvertRadioTypeToSpeechLabel), reinterpret_cast<void**>(&g_OrigConvertLabel));
    const bool okNotice = CreateAndEnableHook(addrNotice, reinterpret_cast<void*>(&hkAddNoticeInfo), reinterpret_cast<void**>(&g_OrigAddNoticeInfo));

    const bool okRadio = CreateAndEnableHook(addrRadio, reinterpret_cast<void*>(&hkStateRadioRequest), reinterpret_cast<void**>(&g_OrigRadioRequest));

#ifdef _DEBUG
    Log("[LostHostage] Hook ConvertLabel:   %s\n", okConvert ? "OK" : "FAIL");
    Log("[LostHostage] Hook AddNoticeInfo:  %s\n", okNotice ? "OK" : "FAIL");
    Log("[LostHostage] Hook RadioRequest:   %s target=%p orig=%p\n",
        okRadio ? "OK" : "FAIL", addrRadio, reinterpret_cast<void*>(g_OrigRadioRequest));
#else
    if (!okConvert)
        Log("[LostHostage] Hook ConvertLabel:   %s\n", okConvert ? "OK" : "FAIL");
    if (!okNotice)
        Log("[LostHostage] Hook AddNoticeInfo:  %s\n", okNotice ? "OK" : "FAIL");
    if (!okRadio)
        Log("[LostHostage] Hook RadioRequest:   %s target=%p orig=%p\n",
            okRadio ? "OK" : "FAIL", addrRadio, reinterpret_cast<void*>(g_OrigRadioRequest));
#endif

    return okConvert && okNotice && okRadio;
}

bool Uninstall_LostHostage_Hooks()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.ConvertRadioTypeToLabel));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.AddNoticeInfo));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.StateRadioRequest));

    g_OrigConvertLabel = nullptr;
    g_OrigAddNoticeInfo = nullptr;
    g_OrigRadioRequest = nullptr;

    std::lock_guard<std::mutex> lock(g_Mutex);
    g_HostagesByObjectId.clear();
    g_HostagesByNameId.clear();
    g_PendingBySoldier.clear();
    g_SelectedReport = {};

#ifdef _DEBUG
    LogDebug("[LostHostage] Uninstall_LostHostage_Hooks\n");
#endif
    return true;
}
