#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "AddressSet.h"
#include "HookUtils.h"
#include "InterrogationVoiceEvent.h"
#include "MissionStateReset.h"
#include "SoldierVoiceTypeQuery.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kDdVoxEne             = 0x95ea16b0u;
    constexpr std::uint32_t kDdVoxEneState        = 0x68b311e1u;
    constexpr std::uint32_t kInvalidSoldierIndex  = 0xFFFFFFFFu;
    constexpr std::uint32_t kInterrogateVoiceType = 2u;
    constexpr std::uint32_t kMarkerVoiceTypeLo    = 3u;
    constexpr std::uint32_t kMarkerVoiceTypeHi    = 4u;
    constexpr std::uint32_t kMaxLabelBases        = 16u;
    constexpr std::uint32_t kLabelFamilyRange     = 8u;

    using UpdateInterrogation_t = void (__fastcall*)(void* soldier2, std::uint32_t index,
                                                     void* knowledge, std::uint32_t hung);
    using CallVoice_t = char (__fastcall*)(void* self, std::uint32_t slot,
                                           std::uint32_t voiceType, std::uint32_t event,
                                           std::uint64_t a5, std::uint64_t a6, std::uint64_t a7);

    struct CpVoiceEvents
    {
        std::uint32_t mainEvent;
        std::uint32_t markerEvent;
        std::uint32_t labelBaseCount;
        std::uint32_t labelBases[kMaxLabelBases];
    };

    std::mutex g_mtx;
    std::unordered_map<std::uint32_t, CpVoiceEvents> g_eventsByCp;

    thread_local std::uint32_t t_mainEvent    = 0;
    thread_local std::uint32_t t_markerEvent  = 0;
    thread_local std::uint32_t t_soldierIndex = kInvalidSoldierIndex;
    thread_local std::uint32_t t_labelBaseCount = 0;
    thread_local std::uint32_t t_labelBases[kMaxLabelBases] = {};

    std::atomic<std::uint32_t> g_pendingMainEvent{ 0 };
    std::atomic<std::uint32_t> g_pendingMarkerEvent{ 0 };
    std::atomic<std::uint32_t> g_pendingSoldierIndex{ kInvalidSoldierIndex };
    std::atomic<std::uint64_t> g_pendingUntilTick{ 0 };
    std::atomic<std::uint32_t> g_pendingLabelBaseCount{ 0 };
    std::uint32_t              g_pendingLabelBases[kMaxLabelBases] = {};

    UpdateInterrogation_t g_OrigUpdate       = nullptr;
    UpdateInterrogation_t g_OrigUpdateMarker = nullptr;
    CallVoice_t           g_OrigCallVoice    = nullptr;

    void* g_UpdateTarget    = nullptr;
    void* g_MarkerTarget    = nullptr;
    void* g_CallVoiceTarget = nullptr;

    bool TryReadCpId(void* soldier2, std::uint32_t& out)
    {
        __try
        {
            const auto base = reinterpret_cast<const std::uint8_t*>(soldier2);
            void* p = *reinterpret_cast<void* const*>(base + 0xD0);
            if (!p)
                return false;
            out = *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(p) + 0x30);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    CpVoiceEvents LookupEvents(std::uint32_t cpId)
    {
        MissionStateReset::PollMissionChange();

        std::lock_guard<std::mutex> lk(g_mtx);
        const auto it = g_eventsByCp.find(cpId);
        if (it == g_eventsByCp.end())
            return CpVoiceEvents{};
        return it->second;
    }

    void RunUpdateGuarded(UpdateInterrogation_t orig, void* soldier2, std::uint32_t index,
                          void* knowledge, std::uint32_t hung, CpVoiceEvents ev)
    {
        const std::uint32_t soldierIndex = index & 0x1FFu;

        if (ev.mainEvent || ev.markerEvent)
        {
            const std::uint64_t now = GetTickCount64();
            g_pendingLabelBaseCount.store(0, std::memory_order_release);
            for (std::uint32_t i = 0; i < ev.labelBaseCount && i < kMaxLabelBases; ++i)
                g_pendingLabelBases[i] = ev.labelBases[i];
            g_pendingLabelBaseCount.store(ev.labelBaseCount, std::memory_order_release);
            g_pendingMainEvent.store(ev.mainEvent, std::memory_order_relaxed);
            g_pendingMarkerEvent.store(ev.markerEvent, std::memory_order_relaxed);
            g_pendingSoldierIndex.store(soldierIndex, std::memory_order_relaxed);
            g_pendingUntilTick.store(now + 2000, std::memory_order_relaxed);
        }

        const std::uint32_t prevMain   = t_mainEvent;
        const std::uint32_t prevMarker = t_markerEvent;
        const std::uint32_t prevIndex  = t_soldierIndex;
        const std::uint32_t prevCount  = t_labelBaseCount;
        std::uint32_t prevBases[kMaxLabelBases];
        for (std::uint32_t i = 0; i < kMaxLabelBases; ++i)
            prevBases[i] = t_labelBases[i];
        t_mainEvent    = ev.mainEvent;
        t_markerEvent  = ev.markerEvent;
        t_soldierIndex = soldierIndex;
        t_labelBaseCount = (ev.labelBaseCount < kMaxLabelBases) ? ev.labelBaseCount
                                                                : kMaxLabelBases;
        for (std::uint32_t i = 0; i < kMaxLabelBases; ++i)
            t_labelBases[i] = ev.labelBases[i];
        __try   { orig(soldier2, index, knowledge, hung); }
        __finally
        {
            t_mainEvent    = prevMain;
            t_markerEvent  = prevMarker;
            t_soldierIndex = prevIndex;
            t_labelBaseCount = prevCount;
            for (std::uint32_t i = 0; i < kMaxLabelBases; ++i)
                t_labelBases[i] = prevBases[i];
        }
    }

    void __fastcall hk_UpdateInterrogation(void* soldier2, std::uint32_t index,
                                           void* knowledge, std::uint32_t hung)
    {
        if (!g_OrigUpdate)
            return;
        std::uint32_t cp = 0;
        CpVoiceEvents ev{ 0u, 0u };
        if (TryReadCpId(soldier2, cp))
            ev = LookupEvents(cp);
        RunUpdateGuarded(g_OrigUpdate, soldier2, index, knowledge, hung, ev);
    }

    void __fastcall hk_UpdateInterrogationMarker(void* soldier2, std::uint32_t index,
                                                 void* knowledge, std::uint32_t hung)
    {
        if (!g_OrigUpdateMarker)
            return;
        std::uint32_t cp = 0;
        CpVoiceEvents ev{ 0u, 0u };
        if (TryReadCpId(soldier2, cp))
            ev = LookupEvents(cp);
        RunUpdateGuarded(g_OrigUpdateMarker, soldier2, index, knowledge, hung, ev);
    }

    struct SlotSnapshot
    {
        int           valid;
        std::uint32_t curVoiceType;
        std::uint64_t playingId;
        std::uint32_t queuedEvent;
        std::uint32_t label0;
        std::uint32_t label1;
        std::uint16_t queuedVoiceType;
        std::uint16_t flags;
        std::uint32_t status;
    };

    void ReadEntrySnapshot(const void* entry, SlotSnapshot& out)
    {
        out.valid = 0;
        __try
        {
            const auto e = reinterpret_cast<const std::uint8_t*>(entry);
            out.curVoiceType    = *reinterpret_cast<const std::uint32_t*>(e + 0x00);
            out.playingId       = *reinterpret_cast<const std::uint64_t*>(e + 0x08);
            out.queuedEvent     = *reinterpret_cast<const std::uint32_t*>(e + 0x20);
            out.label0          = *reinterpret_cast<const std::uint32_t*>(e + 0x24);
            out.label1          = *reinterpret_cast<const std::uint32_t*>(e + 0x28);
            out.queuedVoiceType = *reinterpret_cast<const std::uint16_t*>(e + 0x2c);
            out.flags           = *reinterpret_cast<const std::uint16_t*>(e + 0x2e);
            out.status          = *reinterpret_cast<const std::uint32_t*>(e + 0x50);
            out.valid = 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { out.valid = 0; }
    }

    void ReadSlotSnapshot(const void* self, std::uint32_t slot, SlotSnapshot& out)
    {
        out.valid = 0;
        const std::uint8_t* entry = nullptr;
        __try
        {
            const auto base = reinterpret_cast<const std::uint8_t*>(self);
            const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(base + 0x48);
            const auto table = *reinterpret_cast<const std::uint8_t* const*>(base + 0x30);
            if (table && slot < count)
                entry = table + static_cast<std::size_t>(slot) * 0x58;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { entry = nullptr; }
        if (entry)
            ReadEntrySnapshot(entry, out);
    }

    bool IsInterestingVoxEvent(std::uint32_t evId)
    {
        if (evId == kDdVoxEne || evId == kDdVoxEneState)
            return true;
        std::lock_guard<std::mutex> lk(g_mtx);
        for (const auto& kv : g_eventsByCp)
            if (kv.second.mainEvent == evId || kv.second.markerEvent == evId)
                return true;
        return false;
    }

    bool LabelInFamily(std::uint32_t label, const std::uint32_t* bases, std::uint32_t count)
    {
        for (std::uint32_t i = 0; i < count; ++i)
            if (label - bases[i] + kLabelFamilyRange < 2u * kLabelFamilyRange)
                return true;
        return false;
    }

    std::uint32_t ChooseSwapEvent(std::uint32_t voiceType, std::uint32_t label,
                                  std::uint32_t mainEvent, std::uint32_t markerEvent,
                                  const std::uint32_t* bases, std::uint32_t count)
    {
        if (voiceType == kMarkerVoiceTypeLo || voiceType == kMarkerVoiceTypeHi)
            return markerEvent;
        if (voiceType != kInterrogateVoiceType)
            return 0;
        if (mainEvent == 0 || count == 0)
            return mainEvent;
        return LabelInFamily(label, bases, count) ? mainEvent : 0;
    }

    std::uint32_t TakePendingEventForSlot(std::uint32_t slot, std::uint32_t voiceType,
                                          std::uint32_t label)
    {
        const std::uint32_t rawSpeaker = GetSoldierIndexFromSoundSlot(slot);
        const std::uint32_t speaker =
            (rawSpeaker == 0xFFFFu) ? kInvalidSoldierIndex : (rawSpeaker & 0x1FFu);

        if (t_mainEvent != 0 || t_markerEvent != 0)
        {
            if (speaker != kInvalidSoldierIndex && speaker != t_soldierIndex)
                return 0;
            return ChooseSwapEvent(voiceType, label, t_mainEvent, t_markerEvent,
                                   t_labelBases, t_labelBaseCount);
        }

        if (GetTickCount64() >= g_pendingUntilTick.load(std::memory_order_relaxed))
            return 0;

        if (speaker == kInvalidSoldierIndex)
        {
            static std::atomic<bool> s_warnedUnmappedSlot{ false };
            if (!s_warnedUnmappedSlot.exchange(true, std::memory_order_relaxed))
                Log("[InterrogationVoice] WARN: a soldier vox call on sound slot %u "
                    "arrived with an interrogation voice pending, but that slot is "
                    "not mapped to a soldier index yet - keeping the vanilla "
                    "voice\n", slot);
            return 0;
        }

        if (speaker != g_pendingSoldierIndex.load(std::memory_order_relaxed))
            return 0;

        std::uint32_t bases[kMaxLabelBases];
        std::uint32_t count = g_pendingLabelBaseCount.load(std::memory_order_acquire);
        if (count > kMaxLabelBases)
            count = kMaxLabelBases;
        for (std::uint32_t i = 0; i < count; ++i)
            bases[i] = g_pendingLabelBases[i];

        return ChooseSwapEvent(voiceType, label,
                               g_pendingMainEvent.load(std::memory_order_relaxed),
                               g_pendingMarkerEvent.load(std::memory_order_relaxed),
                               bases, count);
    }

    char __fastcall hk_CallVoice(void* self, std::uint32_t slot, std::uint32_t voiceType,
                                 std::uint32_t event, std::uint64_t a5,
                                 std::uint64_t a6, std::uint64_t a7)
    {
        std::uint32_t ev = event;
        const bool generic = (event == kDdVoxEne || event == kDdVoxEneState);
        if (generic)
        {
            const std::uint32_t pending =
                TakePendingEventForSlot(slot, voiceType, static_cast<std::uint32_t>(a6));
            if (pending != 0)
                ev = pending;
        }

#ifdef _DEBUG
        if (generic)
        {
            const std::uint32_t rawSpeaker = GetSoldierIndexFromSoundSlot(slot);
            SlotSnapshot pre{};
            SlotSnapshot post{};
            ReadSlotSnapshot(self, slot, pre);
            const char ret = g_OrigCallVoice
                ? g_OrigCallVoice(self, slot, voiceType, ev, a5, a6, a7) : 0;
            ReadSlotSnapshot(self, slot, post);
            LogDebug("[InterrogationVoice] %s slot=%u spk=0x%X vt=%u ev=0x%X->0x%X "
                     "a5=0x%llX a6=0x%llX a7=0x%llX ret=%d | pre v=%d cur=%u pid=0x%llX "
                     "qev=0x%X qvt=%u fl=0x%X st=0x%X | post cur=%u pid=0x%llX qev=0x%X "
                     "qvt=%u fl=0x%X st=0x%X\n",
                     (ev != event) ? "SWAP" : "pass",
                     slot, rawSpeaker, voiceType, event, ev,
                     static_cast<unsigned long long>(a5),
                     static_cast<unsigned long long>(a6),
                     static_cast<unsigned long long>(a7),
                     static_cast<int>(ret),
                     pre.valid, pre.curVoiceType,
                     static_cast<unsigned long long>(pre.playingId),
                     pre.queuedEvent, pre.queuedVoiceType, pre.flags, pre.status,
                     post.curVoiceType,
                     static_cast<unsigned long long>(post.playingId),
                     post.queuedEvent, post.queuedVoiceType, post.flags, post.status);
            return ret;
        }
#endif
        return g_OrigCallVoice ? g_OrigCallVoice(self, slot, voiceType, ev, a5, a6, a7) : 0;
    }
}

void Debug_InterrogationVoice_NoteDequeue(void* entry, std::uint32_t slot)
{
#ifdef _DEBUG
    if (!entry)
        return;
    SlotSnapshot s{};
    ReadEntrySnapshot(entry, s);
    if (!s.valid)
        return;
    if (!IsInterestingVoxEvent(s.queuedEvent))
        return;
    const std::uint32_t rawSpeaker = GetSoldierIndexFromSoundSlot(slot);
    LogDebug("[InterrogationVoice] deq slot=%u spk=0x%X qev=0x%X qvt=%u lb0=0x%X lb1=0x%X "
             "cur=%u pid=0x%llX fl=0x%X st=0x%X\n",
             slot, rawSpeaker, s.queuedEvent, s.queuedVoiceType, s.label0, s.label1,
             s.curVoiceType, static_cast<unsigned long long>(s.playingId),
             s.flags, s.status);
#else
    (void)entry;
    (void)slot;
#endif
}

void Register_InterrogationVoiceEvent(std::uint32_t cpIndex, std::uint32_t eventId,
                                      std::uint32_t markerEventId,
                                      const std::uint32_t* labelBases,
                                      std::uint32_t labelBaseCount)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (eventId || markerEventId)
    {
        CpVoiceEvents ev{};
        ev.mainEvent   = eventId;
        ev.markerEvent = markerEventId;
        if (labelBases)
        {
            ev.labelBaseCount = (labelBaseCount < kMaxLabelBases) ? labelBaseCount
                                                                  : kMaxLabelBases;
            for (std::uint32_t i = 0; i < ev.labelBaseCount; ++i)
                ev.labelBases[i] = labelBases[i];
        }
        g_eventsByCp[cpIndex] = ev;
    }
    else
        g_eventsByCp.erase(cpIndex);
}

void Clear_InterrogationVoiceEvents()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_eventsByCp.clear();
    g_pendingMainEvent.store(0, std::memory_order_relaxed);
    g_pendingMarkerEvent.store(0, std::memory_order_relaxed);
    g_pendingSoldierIndex.store(kInvalidSoldierIndex, std::memory_order_relaxed);
    g_pendingUntilTick.store(0, std::memory_order_relaxed);
    g_pendingLabelBaseCount.store(0, std::memory_order_relaxed);
}

bool Install_InterrogationVoiceEvent_Hook()
{
    void* cv = ResolveGameAddress(gAddr.SoundControllerImpl_CallVoice);
    if (!cv)
    {
        Log("[InterrogationVoice] ERROR: CallVoice address 0 (unsupported build) - custom "
            "interrogation voice disabled.\n");
        return true;
    }
    if (!CreateAndEnableHook(cv, reinterpret_cast<void*>(&hk_CallVoice),
                             reinterpret_cast<void**>(&g_OrigCallVoice)))
    {
        Log("[InterrogationVoice] ERROR: CallVoice hook failed - custom interrogation voice "
            "will not apply.\n");
        return false;
    }
    g_CallVoiceTarget = cv;

    void* up = ResolveGameAddress(gAddr.Soldier2InterrogateUtil_UpdateInterrogation);
    if (up && CreateAndEnableHook(up, reinterpret_cast<void*>(&hk_UpdateInterrogation),
                                  reinterpret_cast<void**>(&g_OrigUpdate)))
        g_UpdateTarget = up;
    else
        Log("[InterrogationVoice] WARN: UpdateInterrogation hook failed - main interrogation "
            "line will keep the vanilla voice.\n");

    void* mk = ResolveGameAddress(gAddr.Soldier2InterrogateUtil_UpdateInterrogationMarker);
    if (mk && CreateAndEnableHook(mk, reinterpret_cast<void*>(&hk_UpdateInterrogationMarker),
                                  reinterpret_cast<void**>(&g_OrigUpdateMarker)))
        g_MarkerTarget = mk;
    else
        Log("[InterrogationVoice] WARN: UpdateInterrogationMarker hook failed - marker "
            "interrogation lines will keep the vanilla voice.\n");

    return true;
}

bool Uninstall_InterrogationVoiceEvent_Hook()
{
    if (g_CallVoiceTarget) DisableAndRemoveHook(g_CallVoiceTarget);
    if (g_UpdateTarget)    DisableAndRemoveHook(g_UpdateTarget);
    if (g_MarkerTarget)    DisableAndRemoveHook(g_MarkerTarget);

    g_OrigCallVoice = nullptr;
    g_OrigUpdate = nullptr;
    g_OrigUpdateMarker = nullptr;
    g_CallVoiceTarget = g_UpdateTarget = g_MarkerTarget = nullptr;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_eventsByCp.clear();
    }
    g_pendingMainEvent.store(0, std::memory_order_relaxed);
    g_pendingMarkerEvent.store(0, std::memory_order_relaxed);
    g_pendingSoldierIndex.store(kInvalidSoldierIndex, std::memory_order_relaxed);
    g_pendingUntilTick.store(0, std::memory_order_relaxed);
    g_pendingLabelBaseCount.store(0, std::memory_order_relaxed);
    return true;
}
