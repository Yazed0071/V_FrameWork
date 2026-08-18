

#include "pch.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#include "MinHook.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaBroadcaster.h"
#include "AddressSet.h"
#include "GetGameObjectIdWithIndex.h"
#include "MissionCodeGuard.h"

extern void Log(const char* fmt, ...);


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
    uint32_t lineId = 0x17CD9886u;

    return lineId;
}


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
    ULONGLONG tick;
};

static FireEntry gFireRing[64]{};
static std::atomic<uint32_t> gFireIdx{ 0 };

static bool ShouldFire(uint64_t key, ULONGLONG nowTick, uint32_t cooldownMs = 800)
{
    for (auto& e : gFireRing)
    {
        if (e.key == key)
        {
            const ULONGLONG dt = nowTick - e.tick;
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
            4,
			lineId,
            0.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogDebug("[Holdup] Speak vfunc EXCEPTION. this=%p id=%u line=0x%08X\n",
            holdupThis, id32, lineId);
    }
}


static constexpr uintptr_t kSlotSoldierIndexOffset = 0x38;

static uint32_t ReadSoldierIndexFromSlot(uintptr_t slot)
{
    return ReadDwordNoThrow(slot + kSlotSoldierIndexOffset);
}

static void EmitHoldupCancelLookToPlayerMessage(uint32_t soldierIndex)
{
    const std::uint32_t gameObjectId = GetGameObjectIdByIndex("TppSoldier2", soldierIndex);
    if (!gameObjectId)
    {
        Log("[Holdup] failed to resolve soldier index %u\n", soldierIndex);
        return;
    }

    V_FrameWork::EmitMessage("GameObject", "HoldupCancelLookToPlayer", gameObjectId);
}

static std::atomic<uint64_t> gDetourHits{ 0 };

static void __fastcall Hook_State(void* holdupThis, uint64_t id, int phase)
{
    MISSION_GUARD_ORIGINAL_VOID(gOrig_State, holdupThis, id, phase);

    if (gOrig_State)
        gOrig_State(holdupThis, id, phase);

    if (phase != 1)
        return;

    const uint32_t holdupId = static_cast<uint32_t>(id & 0xFFFFFFFFu);
    const ULONGLONG now = GetTickCount64();

    const uintptr_t slot = GetHoldupSlot(holdupThis, holdupId);
    if (!slot) return;

    const uint32_t soldierIndex = ReadSoldierIndexFromSlot(slot);

    {
        const uint64_t emitKey =
            ((static_cast<uint64_t>(reinterpret_cast<uintptr_t>(holdupThis)) >> 4) ^
             (static_cast<uint64_t>(soldierIndex) << 32)) ^ 0xE17E17E17E17ULL;

        if (ShouldFire(emitKey, now, 500))
            EmitHoldupCancelLookToPlayerMessage(soldierIndex);
    }

    const uint32_t lineId = ComputeLineIdFromSlot(slot);

    const uint64_t key =
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(holdupThis)) >> 4) ^
        (static_cast<uint64_t>(holdupId) << 32) ^
        static_cast<uint64_t>(lineId);

    TrySpeak_EnterDownHoldupStyle(holdupThis, holdupId, lineId);

    const uint64_t n = ++gDetourHits;
    if (n == 1 || (n % 50) == 0)
    {
    }
}


bool Install_State_StandHoldupCancelLookToPlayer_Hook(HMODULE hGame)
{
    if (!hGame) return false;

    gBase = reinterpret_cast<uintptr_t>(hGame);
    const uintptr_t target = reinterpret_cast<uintptr_t>(
        ResolveGameAddress(gAddr.State_StandHoldupCancelLookToPlayer));

    uint8_t pro[16]{};
    __try { std::memcpy(pro, (void*)target, sizeof(pro)); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[Holdup] ERROR: cannot read target bytes.\n");
        return false;
    }

    for (int i = 0; i < 16; i++) ;

    MH_STATUS s1 = MH_CreateHook((LPVOID)target, (LPVOID)&Hook_State, (LPVOID*)&gOrig_State);
    if (s1 != MH_OK) return false;

    MH_STATUS s2 = EnableOrQueueHook((LPVOID)target);
    if (s2 != MH_OK) return false;

#ifdef _DEBUG
    LogDebug("[Holdup] Hook installed OK.\n");
#endif
    return true;
}

bool Uninstall_State_StandHoldupCancelLookToPlayer_Hook()
{
    if (!gBase) return true;

    const uintptr_t target = reinterpret_cast<uintptr_t>(
        ResolveGameAddress(gAddr.State_StandHoldupCancelLookToPlayer));

    MH_DisableHook((LPVOID)target);
    MH_RemoveHook((LPVOID)target);

    gOrig_State = nullptr;
    gBase = 0;
    gDetourHits.store(0);
    gFireIdx.store(0);
    std::memset(gFireRing, 0, sizeof(gFireRing));

#ifdef _DEBUG
    LogDebug("[Holdup] Hook removed.\n");
#endif
    return true;
}