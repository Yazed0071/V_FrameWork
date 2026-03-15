// HoldupCancelLookToPlayerHook.cpp
// tpp::gm::soldier::impl::HoldupActionImpl::State_StandHoldupCancelLookToPlayer
#include "pch.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#include "MinHook.h"

extern void Log(const char* fmt, ...);

// -----------------------------------------------------------------------------
// Target address
// VA: 0x14A141910
// Image base:    0x140000000
// RVA = VA - ImageBase = 0x0A141910
// -----------------------------------------------------------------------------
static constexpr uintptr_t kState_StandHoldupCancelLookToPlayer_RVA = 0x0A141910ULL;

static uintptr_t gBase = 0;

using StateFn_t = void(__fastcall*)(void* holdupThis, uint64_t id, int phase);
static StateFn_t gOrig_State = nullptr;


using SpeakVfunc20_t = void(__fastcall*)(void* self,
    uint32_t id32,
    uint32_t a3,
    int a4,
    uint32_t lineId,
    float a6);


static __forceinline uintptr_t ReadQwordNoThrow(uintptr_t addr)
{
    __try { return *reinterpret_cast<uintptr_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __forceinline uint32_t ReadDwordNoThrow(uintptr_t addr)
{
    __try { return *reinterpret_cast<uint32_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __forceinline uint8_t ReadByteNoThrow(uintptr_t addr)
{
    __try { return *reinterpret_cast<uint8_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static __forceinline void WriteByteNoThrow(uintptr_t addr, uint8_t v)
{
    __try { *reinterpret_cast<uint8_t*>(addr) = v; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// slot = *(this+0x88) + ( (id32 - *(this+0x90)) * 0x40 )
static uintptr_t GetHoldupSlot(void* holdupThis, uint32_t id32)
{
    const auto p1 = reinterpret_cast<uintptr_t>(holdupThis);

    const uint32_t baseId = ReadDwordNoThrow(p1 + 0x90);
    if (id32 < baseId) return 0;

    const uintptr_t slotsBase = ReadQwordNoThrow(p1 + 0x88);
    if (!slotsBase) return 0;

    const uint32_t idx = id32 - baseId;
    return slotsBase + (static_cast<uintptr_t>(idx) * 0x40);
}

static uint32_t ComputeLineIdFromSlot(uintptr_t slot)
{
    uint32_t lineId = 0x17CD9886u; // EVR071:Player has their gun on the enemy - more reaction variations. Bold reactions.

    return lineId;
}

// plVar2 = *( *(this + 0x70) + 0xA8 )
static void* ResolveSpeakObject(void* holdupThis)
{
    const auto p1 = reinterpret_cast<uintptr_t>(holdupThis);

    const uintptr_t mgr = ReadQwordNoThrow(p1 + 0x70);
    if (!mgr) return nullptr;

    void* obj = reinterpret_cast<void*>(ReadQwordNoThrow(mgr + 0xA8));
    return obj;
}

struct FireEntry
{
    uint64_t key;
    uint32_t tick;
};

static FireEntry gFireRing[64]{};
static std::atomic<uint32_t> gFireIdx{ 0 };

static bool ShouldFire(uint64_t key, uint32_t nowTick, uint32_t cooldownMs = 800)
{
    for (auto& e : gFireRing)
    {
        if (e.key == key)
        {
            const uint32_t dt = nowTick - e.tick;
            if (dt < cooldownMs)
                return false;

            e.tick = nowTick;
            return true;
        }
    }

    const uint32_t i = gFireIdx.fetch_add(1) & 63u;
    gFireRing[i].key = key;
    gFireRing[i].tick = nowTick;
    return true;
}

static void TrySpeak_EnterDownHoldupStyle(void* holdupThis, uint32_t id32, uint32_t lineId)
{
    void* obj = ResolveSpeakObject(holdupThis);
    if (!obj) return;

    const uintptr_t vtbl = ReadQwordNoThrow(reinterpret_cast<uintptr_t>(obj));
    if (!vtbl) return;

    const uintptr_t fnp = ReadQwordNoThrow(vtbl + 0x20);
    if (!fnp) return;

    auto fn = reinterpret_cast<SpeakVfunc20_t>(fnp);

    __try
    {
        fn(obj,
            id32,
            0x95EA16B0u,
            4,              // IMPORTANT: matches State_EnterDownHoldup
			lineId,         // IMPORTANT: FNV132 of line name, e.g. "EVR071:Player has their gun on the enemy - more reaction variations. Bold reactions."
            0.0f);          // IMPORTANT: delay??
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[Holdup] Speak vfunc EXCEPTION. this=%p id=%u line=0x%08X\n",
            holdupThis, id32, lineId);
    }
}

static std::atomic<uint64_t> gDetourHits{ 0 };

static void __fastcall Hook_State(void* holdupThis, uint64_t id, int phase)
{
    if (gOrig_State)
        gOrig_State(holdupThis, id, phase);

    if (phase != 1)
        return;

    const uint32_t id32 = static_cast<uint32_t>(id & 0xFFFFFFFFu);
    const uintptr_t slot = GetHoldupSlot(holdupThis, id32);
    if (!slot) return;

    const uint8_t b3f = ReadByteNoThrow(slot + 0x3F);
    const bool alreadyPlayed = ((b3f & 0x02u) != 0);

    const uint32_t lineId = ComputeLineIdFromSlot(slot);

    const uint32_t now = GetTickCount();
    const uint64_t key =
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(holdupThis)) >> 4) ^
        (static_cast<uint64_t>(id32) << 32) ^
        static_cast<uint64_t>(lineId);

    if (!ShouldFire(key, now, 900))
        return;

    TrySpeak_EnterDownHoldupStyle(holdupThis, id32, lineId);

    if (!alreadyPlayed)
        WriteByteNoThrow(slot + 0x3F, static_cast<uint8_t>(b3f | 0x02u));

    const uint64_t n = ++gDetourHits;
    if (n == 1 || (n % 50) == 0)
    {
        Log("[Holdup] SPEAK attempt #%llu phase=1 id=%u line=0x%08X this=%p slot=%p\n",
            (unsigned long long)n, id32, lineId, holdupThis, (void*)slot);
    }
}


bool Install_State_StandHoldupCancelLookToPlayer_Hook(HMODULE hGame)
{
    if (!hGame) return false;

    gBase = reinterpret_cast<uintptr_t>(hGame);
    const uintptr_t target = gBase + kState_StandHoldupCancelLookToPlayer_RVA;

    Log("[Holdup] base=%p target=%p (rva=0x%llX)\n",
        (void*)gBase, (void*)target, (unsigned long long)kState_StandHoldupCancelLookToPlayer_RVA);

    uint8_t pro[16]{};
    __try { std::memcpy(pro, (void*)target, sizeof(pro)); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[Holdup] ERROR: cannot read target bytes.\n");
        return false;
    }

    Log("[Holdup] prologue: ");
    for (int i = 0; i < 16; i++) Log("%02X ", pro[i]);
    Log("\n");

    MH_STATUS s1 = MH_CreateHook((LPVOID)target, (LPVOID)&Hook_State, (LPVOID*)&gOrig_State);
    Log("[Holdup] MH_CreateHook -> %d orig=%p\n", (int)s1, (void*)gOrig_State);
    if (s1 != MH_OK) return false;

    MH_STATUS s2 = MH_EnableHook((LPVOID)target);
    Log("[Holdup] MH_EnableHook -> %d\n", (int)s2);
    if (s2 != MH_OK) return false;

    Log("[Holdup] Hook installed OK.\n");
    return true;
}

bool Uninstall_State_StandHoldupCancelLookToPlayer_Hook()
{
    if (!gBase) return true;

    const uintptr_t target = gBase + kState_StandHoldupCancelLookToPlayer_RVA;

    MH_DisableHook((LPVOID)target);
    MH_RemoveHook((LPVOID)target);

    gOrig_State = nullptr;
    gBase = 0;
    gDetourHits.store(0);
    gFireIdx.store(0);
    std::memset(gFireRing, 0, sizeof(gFireRing));

    Log("[Holdup] Hook removed.\n");
    return true;
}