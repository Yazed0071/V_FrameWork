#include "pch.h"
#include "PlacedSystemImpl_Insert.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    using InsertFn          = void (__fastcall*)(void* self, void* desc, void* outIdx);
    using WarpFn            = void (__fastcall*)(void* playerSub, std::uint32_t id, float* pos, float* quat, std::uint8_t controlFlag);
    using DeathFamilyTickFn = void (__fastcall*)(void* outer, std::uint32_t id);
    using PrePollingFn      = void (__fastcall*)(void* core, std::uint32_t idx);

    static InsertFn g_OrigInsert = nullptr;
    static void*    g_InsertAddr = nullptr;

    static WarpFn g_OrigWarp = nullptr;
    static void*  g_WarpAddr = nullptr;

    static DeathFamilyTickFn g_OrigFamilyTick = nullptr;
    static void*             g_FamilyTickAddr = nullptr;

    static PrePollingFn g_OrigPrePolling = nullptr;
    static void*        g_PrePollingAddr = nullptr;

    static void*                 g_System   = nullptr;
    static std::uint32_t         g_Slots[8] = {};
    static std::atomic<unsigned> g_SlotCount{ 0 };

    static std::uint64_t g_LastWarpFire = 0;

    static bool          g_DeathHang    = false;
    static std::uint64_t g_DeathStart   = 0;
    static std::uint64_t g_DeathLastTry = 0;

    static bool g_SleepWokeEpisode = false;
    static bool g_TeleportArmed    = false;

    static uintptr_t PlacedSystemInsertAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140FC8020ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x140FC8070ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x140FC88A0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x140FC8960ull;
            default: return 0;
        }
    }
    static uintptr_t SetWarpToPositionAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x1409CEAE0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x1409CE9E0ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x14627C6F0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x147F4CCE0ull;
            default: return 0;
        }
    }
    static uintptr_t DeathFamilyTickAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x14118C1C0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x14118C240ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x14118C970ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x14118CA10ull;
            default: return 0;
        }
    }
    static uintptr_t DamageFallStateAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x14118E1B0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x14118E230ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x14118E9E0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x14118EA90ull;
            default: return 0;
        }
    }
    static uintptr_t PrePollingAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140FF28D0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x140FF28F0ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x140FF3000ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x140FF30C0ull;
            default: return 0;
        }
    }

    static bool IsPortalEquipId(std::uint16_t id)
    {
        return id == 0x186u || id == 0x16Au || id == 0x16Bu || id == 0x19Cu || id == 0x192u;
    }

    static bool ReadPortalDesc(void* self, void* desc)
    {
        __try
        {
            if (self != g_System)
            {
                g_System = self;
                g_SlotCount.store(0, std::memory_order_relaxed);
            }
            if (!desc)
                return false;
            return IsPortalEquipId(*reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uint8_t*>(desc) + 0x30));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void __fastcall hk_Insert(void* self, void* desc, void* outIdx)
    {
        if (!ReadPortalDesc(self, desc))
        {
            g_OrigInsert(self, desc, outIdx);
            return;
        }

        std::uint32_t localSlot = 0xFFFFFFFFu;
        g_OrigInsert(self, desc, &localSlot);
        if (outIdx)
            *reinterpret_cast<std::uint32_t*>(outIdx) = localSlot;
        if (localSlot != 0xFFFFFFFFu)
        {
            const unsigned n = g_SlotCount.load(std::memory_order_relaxed);
            if (n < 8)
            {
                g_Slots[n] = localSlot;
                g_SlotCount.store(n + 1, std::memory_order_relaxed);
            }
        }
    }

    static void __fastcall hk_Warp(void* playerSub, std::uint32_t id, float* pos, float* quat, std::uint8_t controlFlag)
    {
        g_OrigWarp(playerSub, id, pos, quat, controlFlag);
        if (!playerSub)
            return;
        __try
        {
            std::uint8_t* sub = reinterpret_cast<std::uint8_t*>(playerSub);
            const std::uint32_t localId = *reinterpret_cast<std::uint32_t*>(sub + 0x218);
            if (pos && id == localId)
                g_LastWarpFire = GetTickCount64();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall hk_PrePolling(void* core, std::uint32_t idx)
    {
        __try
        {
            std::uint8_t* pc      = reinterpret_cast<std::uint8_t*>(core);
            std::uint8_t* pplayer = *reinterpret_cast<std::uint8_t**>(pc + 0x408);
            std::uint8_t* psub    = pplayer ? *reinterpret_cast<std::uint8_t**>(pplayer + 0x138) : nullptr;
            std::uint8_t* pactArr = *reinterpret_cast<std::uint8_t**>(pc + 0x2D8);

            const int preAct = (psub && pactArr && idx == *reinterpret_cast<std::uint32_t*>(psub + 0x218))
                               ? pactArr[idx] : -1;

            if (preAct == 0x19 && WormholePortal_AnyDeployed())
                g_TeleportArmed = true;

            if (g_TeleportArmed && (preAct == 0x19 || preAct == 0x07) && psub)
                *reinterpret_cast<std::uint32_t*>(psub + 0x204) |= 0x40000u;

            std::uint8_t* sub30 = pplayer ? *reinterpret_cast<std::uint8_t**>(pplayer + 0x3F0) : nullptr;
            std::uint8_t* base  = sub30 ? sub30 - 0x30 : nullptr;

            std::uint8_t* pidxO = base ? *reinterpret_cast<std::uint8_t**>(base + 0x38) : nullptr;
            std::uint8_t* pwork = base ? *reinterpret_cast<std::uint8_t**>(base + 0x60) : nullptr;
            std::uint8_t* rec   = nullptr;
            if (pidxO && pwork)
                rec = pwork + static_cast<std::size_t>(idx - *reinterpret_cast<std::int32_t*>(pidxO + 0x24)) * 0x20;

            if (preAct == 0x07 && g_TeleportArmed && rec)
            {
                rec[0x11] = 3u;

                std::uint8_t* frz = *reinterpret_cast<std::uint8_t**>(pc + 0x240);
                std::uint8_t* fb  = frz ? *reinterpret_cast<std::uint8_t**>(frz + 8) : nullptr;
                if (fb)
                {
                    std::uint8_t* fr = fb + static_cast<std::size_t>(
                        idx - *reinterpret_cast<std::int32_t*>(frz + 0x14)) * 0x80;
                    *reinterpret_cast<std::uint16_t*>(fr + 0x72) &= 0xFFBFu;
                    for (int i = 0; i < 11; ++i)
                    {
                        *reinterpret_cast<std::uint16_t*>(fr + 0x24 + i * 2) &= 0xFFBFu;
                        *reinterpret_cast<std::uint16_t*>(fr + 0x3A + i * 2) &= 0xFFBFu;
                    }
                    *reinterpret_cast<std::uint16_t*>(fr + 0x50) &= 0xFFBFu;
                    *reinterpret_cast<std::uint16_t*>(fr + 0x52) &= 0xFFBFu;
                    *reinterpret_cast<std::uint16_t*>(fr + 0x5A) &= 0xFFBFu;
                    *reinterpret_cast<std::uint16_t*>(fr + 0x5C) &= 0xFFBFu;
                    *reinterpret_cast<std::uint16_t*>(fr + 0x5E) &= 0xFFBFu;
                    *reinterpret_cast<std::uint16_t*>(fr + 0x66) &= 0xFFBFu;
                    fr[0x79] |= 2u;
                }

                g_SleepWokeEpisode = true;
            }

            if (g_SleepWokeEpisode && (preAct == 0x19 || preAct == 0x07) && psub &&
                (*reinterpret_cast<std::uint32_t*>(psub + 0x204) & 0x100u) == 0)
            {
                std::uint8_t* p60u = *reinterpret_cast<std::uint8_t**>(psub + 0x60);
                if (p60u)
                {
                    std::uint8_t* abu = *reinterpret_cast<std::uint8_t**>(p60u + 8);
                    if (abu)
                        *reinterpret_cast<std::int32_t*>(abu + static_cast<std::size_t>(idx) * 0x4C + 0x34) = 0;

                    std::uint8_t* fw = *reinterpret_cast<std::uint8_t**>(p60u + 0x168);
                    if (fw)
                        *reinterpret_cast<std::uint32_t*>(fw + static_cast<std::size_t>(idx) * 4) &= ~0x200u;
                }

                std::uint8_t* c280 = *reinterpret_cast<std::uint8_t**>(pc + 0x280);
                if (c280)
                {
                    std::uint8_t* urec = c280 + static_cast<std::size_t>(
                        idx - *reinterpret_cast<std::int32_t*>(pc + 8)) * 0x20;
                    *reinterpret_cast<std::uint32_t*>(urec + 8) &= ~0x4000u;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        g_OrigPrePolling(core, idx);
        __try
        {
            std::uint8_t* c      = reinterpret_cast<std::uint8_t*>(core);
            std::uint8_t* player = *reinterpret_cast<std::uint8_t**>(c + 0x408);
            if (!player)
                return;
            std::uint8_t* sub = *reinterpret_cast<std::uint8_t**>(player + 0x138);
            if (!sub)
                return;
            if (idx != *reinterpret_cast<std::uint32_t*>(sub + 0x218))
                return;
            std::uint8_t* actArr = *reinterpret_cast<std::uint8_t**>(c + 0x2D8);
            const int actByte = actArr ? actArr[idx] : -1;

            const bool normalAct = (actByte == 0x44 || actByte == 0x22 || actByte == 0x31);
            if (normalAct && (g_SleepWokeEpisode || g_TeleportArmed))
            {
                g_SleepWokeEpisode = false;
                g_TeleportArmed    = false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall hk_DeathFamilyTick(void* outer, std::uint32_t id)
    {
        g_OrigFamilyTick(outer, id);
        __try
        {
            std::uint8_t* o     = reinterpret_cast<std::uint8_t*>(outer);
            std::uint8_t* owner = *reinterpret_cast<std::uint8_t**>(o + 8);
            std::uint8_t* idxO  = *reinterpret_cast<std::uint8_t**>(o + 0x38);
            std::uint8_t* wbase = *reinterpret_cast<std::uint8_t**>(o + 0x68);
            if (!owner || !idxO || !wbase)
                return;
            std::uint8_t* sub = *reinterpret_cast<std::uint8_t**>(owner + 0x138);
            if (!sub)
                return;
            if (id != *reinterpret_cast<std::uint32_t*>(sub + 0x218))
                return;
            if (*reinterpret_cast<std::uint32_t*>(sub + 0x204) & 0x100u)
                return;
            const std::int32_t idxBase = *reinterpret_cast<std::int32_t*>(idxO + 0x24);
            std::uint8_t* work  = wbase + static_cast<std::size_t>(id - idxBase) * 0xB0;
            void*         subFn = *reinterpret_cast<void**>(work);

            const std::int32_t dmgSub = *reinterpret_cast<std::int32_t*>(work + 0x8C);
            const bool hang =
                subFn == reinterpret_cast<void*>(DamageFallStateAddr()) &&
                (dmgSub == 0x35 || dmgSub == 0x67) &&
                *reinterpret_cast<std::uint8_t*>(work + 0x10) == 1;

            if (!hang)
            {
                g_DeathHang = false;
                return;
            }

            if (!WormholePortal_AnyDeployed())
                return;

            const std::uint64_t now = GetTickCount64();
            if (!g_DeathHang)
            {
                g_DeathHang    = true;
                g_DeathStart   = now;
                g_DeathLastTry = 0;
            }

            const bool fire =
                (g_LastWarpFire >= g_DeathStart && now - g_LastWarpFire >= 250) ||
                now - g_DeathStart > 6000;
            if (!fire || now - g_DeathLastTry < 300)
                return;
            g_DeathLastTry = now;

            *reinterpret_cast<float*>(work + 0x58)        = 0.0f;
            *reinterpret_cast<std::uint8_t*>(work + 0xA0) &= 0xFDu;
            *reinterpret_cast<std::uint8_t*>(work + 0x11)  = 2u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

bool Install_PlacedSystemImpl_Insert_Hook()
{
    void* ins = reinterpret_cast<void*>(PlacedSystemInsertAddr());
    if (!ins)
    {
        Log("[Wormhole] WARNING: no placed-object insert address for %s; the near-death portal warp stays FOB-only.\n",
            GetGameBuildName(gGameBuild));
        return false;
    }
    if (!CreateAndEnableHook(ins, reinterpret_cast<void*>(&hk_Insert), reinterpret_cast<void**>(&g_OrigInsert)))
    {
        Log("[Wormhole] ERROR: could not hook the placed-object insert for %s; the near-death portal warp stays FOB-only.\n",
            GetGameBuildName(gGameBuild));
        return false;
    }
    g_InsertAddr = ins;

    void* warp = reinterpret_cast<void*>(SetWarpToPositionAddr());
    if (warp && CreateAndEnableHook(warp, reinterpret_cast<void*>(&hk_Warp), reinterpret_cast<void**>(&g_OrigWarp)))
        g_WarpAddr = warp;
    else
        Log("[Wormhole] ERROR: could not hook SetWarpToPosition for %s; the rescue destination falls back to the live transform.\n",
            GetGameBuildName(gGameBuild));

    void* famTick = reinterpret_cast<void*>(DeathFamilyTickAddr());
    if (famTick && CreateAndEnableHook(famTick, reinterpret_cast<void*>(&hk_DeathFamilyTick),
                                       reinterpret_cast<void**>(&g_OrigFamilyTick)))
        g_FamilyTickAddr = famTick;
    else
        Log("[Wormhole] ERROR: could not hook the damage family tick for %s; players will not recover after the portal warp.\n",
            GetGameBuildName(gGameBuild));

    void* prePoll = reinterpret_cast<void*>(PrePollingAddr());
    if (prePoll && CreateAndEnableHook(prePoll, reinterpret_cast<void*>(&hk_PrePolling),
                                       reinterpret_cast<void**>(&g_OrigPrePolling)))
        g_PrePollingAddr = prePoll;
    else
        Log("[Wormhole] ERROR: could not hook ExecPrePolling for %s; the player will not stand up after the portal warp.\n",
            GetGameBuildName(gGameBuild));

    return true;
}

void Uninstall_PlacedSystemImpl_Insert_Hook()
{
    if (g_InsertAddr)     DisableAndRemoveHook(g_InsertAddr);
    if (g_WarpAddr)       DisableAndRemoveHook(g_WarpAddr);
    if (g_FamilyTickAddr) DisableAndRemoveHook(g_FamilyTickAddr);
    if (g_PrePollingAddr) DisableAndRemoveHook(g_PrePollingAddr);
    g_InsertAddr     = nullptr;
    g_OrigInsert     = nullptr;
    g_WarpAddr       = nullptr;
    g_OrigWarp       = nullptr;
    g_FamilyTickAddr = nullptr;
    g_OrigFamilyTick = nullptr;
    g_PrePollingAddr = nullptr;
    g_OrigPrePolling = nullptr;
    g_System         = nullptr;
    g_SleepWokeEpisode = false;
    g_TeleportArmed    = false;
    g_SlotCount.store(0, std::memory_order_relaxed);
}

bool WormholePortal_AnyDeployed()
{
    __try
    {
        const unsigned n = g_SlotCount.load(std::memory_order_relaxed);
        if (!g_System || n == 0)
            return false;
        std::uint8_t* base  = reinterpret_cast<std::uint8_t*>(g_System);
        std::uint8_t* flags = *reinterpret_cast<std::uint8_t**>(base + 0xB0);
        std::uint8_t* equip = *reinterpret_cast<std::uint8_t**>(base + 0x38);
        if (!flags || !equip)
            return false;
        unsigned live = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            const std::uint32_t slot = g_Slots[i];
            const std::uint8_t* f = flags + static_cast<std::size_t>(slot) * 3;
            if ((f[0] & 1) && !(f[1] & 0x80) &&
                IsPortalEquipId(*reinterpret_cast<const std::uint16_t*>(
                    equip + static_cast<std::size_t>(slot) * 4)))
                g_Slots[live++] = slot;
        }
        if (live != n)
            g_SlotCount.store(live, std::memory_order_relaxed);
        return live != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_SlotCount.store(0, std::memory_order_relaxed);
        return false;
    }
}
