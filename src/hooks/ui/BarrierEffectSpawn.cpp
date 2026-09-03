#include "pch.h"
#include "BarrierEffectSpawn.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaBroadcaster.h"

bool WormholePortal_AnyDeployed();

namespace
{
    using UpdateFn    = void* (__fastcall*)(void* r15, std::uint64_t index, void* r8, void* r9);
    using GetItemIdFn = int   (__fastcall*)(void*, unsigned int, std::uint64_t, std::uint64_t);
    using BoolFn      = int   (__fastcall*)();

    using DtorFn       = void  (__fastcall*)(void* self);
    using BlowCbFn     = std::uint32_t (__fastcall*)(void* callable, void* offense,
                                                     void* defense, void* result, void* core);
    using RegisterCbFn = void  (__fastcall*)(void* core, std::uint32_t idx);
    using PredicateFn  = std::uint32_t (__fastcall*)(void* a1, void* a2, void* a3, void* a4,
                                                     void* a5, void* a6);
    using IsSkillActiveFn = std::uint8_t (__fastcall*)(void* iface, std::uint32_t idx,
                                                       std::uint32_t typeId);
    using SkillUpdateFn   = void (__fastcall*)(void* iface, std::uint32_t idx,
                                               std::uint32_t typeId, float amount);

    static UpdateFn    g_Orig          = nullptr;
    static GetItemIdFn g_OrigGetItemId = nullptr;
    static BoolFn      g_IsFobMode     = nullptr;
    static DtorFn      g_OrigDtor      = nullptr;
    static void*       g_DtorAddr      = nullptr;
    static BlowCbFn     g_OrigBlowCb        = nullptr;
    static void*        g_BlowCbAddr        = nullptr;
    static RegisterCbFn g_RegisterDefenseCb = nullptr;
    static void*        g_DefenseFunctor    = nullptr;
    static PredicateFn  g_OrigPredicate     = nullptr;
    static void*        g_PredicateAddr     = nullptr;

    struct DedupEntry { std::uint32_t key; std::uint32_t tick; };
    constexpr std::uint32_t kRehitTicks = 60;
    static DedupEntry                 g_DedupRing[4] = {};
    static std::atomic<unsigned>      g_DedupPos{ 0 };
    static std::atomic<std::uint32_t> g_Tick{ 0 };

    struct DmgEvent { int playerIndex; float before; float after; };
    static DmgEvent              g_DmgQueue[16];
    static std::atomic<unsigned> g_DmgWr{ 0 };
    static std::atomic<unsigned> g_DmgRd{ 0 };

    static std::uint8_t* g_FobAbsorbByte   = nullptr;
    static bool          g_FobAbsorbForced = false;
    static float         g_HpFloor         = 0.0f;
    static float         g_StamFloor       = 0.0f;
    static bool          g_HpFloorValid    = false;
    static std::uint32_t g_LeakTick        = 0;

    static uintptr_t BarrierBlowCbAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140FF94D0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x140FF9560ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x140FF9C70ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x140FF9CC0ull;
            default: return 0;
        }
    }

    struct SoundMuteSite
    {
        uintptr_t    addr;
        std::uint8_t expect[5];
        std::uint8_t mute[5];
        const char*  name;
    };
    static constexpr std::size_t kMaxMuteSites = 4;
    static bool  g_SoundMuted = false;
    static void* g_MuteAddr[kMaxMuteSites] = {};

    static std::size_t BarrierSoundMuteSites(SoundMuteSite (&out)[kMaxMuteSites])
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4:
                out[0] = { 0x140D2DDB7ull, { 0x41, 0x0F, 0xB7, 0x47, 0x2A },
                                            { 0xE9, 0x15, 0x02, 0x00, 0x00 }, "bullet wall-hit SE" };
                out[1] = { 0x1417525CAull, { 0xE8, 0xD1, 0xF0, 0xFF, 0xFF },
                                            { 0x90, 0x90, 0x90, 0x90, 0x90 }, "wall-object hit SE" };
                out[2] = { 0x141751648ull, { 0xC6, 0x44, 0x24, 0x28, 0x01 },
                                            { 0xC6, 0x44, 0x24, 0x28, 0x00 }, "ricochet-spawn sound flag" };
                return 3;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4:
                out[0] = { 0x140D2DD67ull, { 0x41, 0x0F, 0xB7, 0x47, 0x2A },
                                            { 0xE9, 0x15, 0x02, 0x00, 0x00 }, "bullet wall-hit SE" };
                out[1] = { 0x14175258Aull, { 0xE8, 0xD1, 0xF0, 0xFF, 0xFF },
                                            { 0x90, 0x90, 0x90, 0x90, 0x90 }, "wall-object hit SE" };
                out[2] = { 0x141751608ull, { 0xC6, 0x44, 0x24, 0x28, 0x01 },
                                            { 0xC6, 0x44, 0x24, 0x28, 0x00 }, "ricochet-spawn sound flag" };
                return 3;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3:
                out[0] = { 0x140D2DC77ull, { 0x41, 0x0F, 0xB7, 0x47, 0x2A },
                                            { 0xE9, 0x15, 0x02, 0x00, 0x00 }, "bullet wall-hit SE" };
                out[1] = { 0x14175310Aull, { 0xE8, 0xC1, 0xF0, 0xFF, 0xFF },
                                            { 0x90, 0x90, 0x90, 0x90, 0x90 }, "wall-object hit SE" };
                out[2] = { 0x14194C55Eull, { 0xC6, 0x44, 0x24, 0x28, 0x01 },
                                            { 0xC6, 0x44, 0x24, 0x28, 0x00 }, "ricochet-spawn sound flag" };
                out[3] = { 0x14194C5A3ull, { 0xC6, 0x44, 0x24, 0x28, 0x01 },
                                            { 0xC6, 0x44, 0x24, 0x28, 0x00 }, "ricochet-spawn sound flag (2)" };
                return 4;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3:
                out[0] = { 0x140D2D8D7ull, { 0x41, 0x0F, 0xB7, 0x47, 0x2A },
                                            { 0xE9, 0x15, 0x02, 0x00, 0x00 }, "bullet wall-hit SE" };
                out[1] = { 0x1417531DAull, { 0xE8, 0xD1, 0xF0, 0xFF, 0xFF },
                                            { 0x90, 0x90, 0x90, 0x90, 0x90 }, "wall-object hit SE" };
                out[2] = { 0x14194C68Eull, { 0xC6, 0x44, 0x24, 0x28, 0x01 },
                                            { 0xC6, 0x44, 0x24, 0x28, 0x00 }, "ricochet-spawn sound flag" };
                out[3] = { 0x14194C6D3ull, { 0xC6, 0x44, 0x24, 0x28, 0x01 },
                                            { 0xC6, 0x44, 0x24, 0x28, 0x00 }, "ricochet-spawn sound flag (2)" };
                return 4;
            default:
                return 0;
        }
    }

    static bool WritePatchBytes(void* target, const std::uint8_t* src, std::size_t size)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        std::memcpy(target, src, size);
        DWORD restored = 0;
        VirtualProtect(target, size, oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, size);
        return true;
    }

    static void ApplyBarrierSoundMute()
    {
        if (g_SoundMuted) return;
        SoundMuteSite sites[kMaxMuteSites];
        const std::size_t n = BarrierSoundMuteSites(sites);
        if (n == 0) return;
        for (std::size_t i = 0; i < n; ++i)
        {
            std::uint8_t* p = reinterpret_cast<std::uint8_t*>(sites[i].addr);
            if (std::memcmp(p, sites[i].expect, 5) == 0)
            {
                if (WritePatchBytes(p, sites[i].mute, 5))
                    g_MuteAddr[i] = p;
            }
            else
            {
                static std::atomic<bool> s_MuteWarned{ false };
                bool w = false;
                if (s_MuteWarned.compare_exchange_strong(w, true))
                    Log("[Barrier] WARNING: wall-hit SE site '%s' @ 0x%llX has unexpected bytes; that per-hit sound stays audible.\n",
                        sites[i].name, static_cast<unsigned long long>(sites[i].addr));
            }
        }
        g_SoundMuted = true;
    }

    static void RestoreBarrierSoundMute()
    {
        if (!g_SoundMuted) return;
        SoundMuteSite sites[kMaxMuteSites];
        const std::size_t n = BarrierSoundMuteSites(sites);
        for (std::size_t i = 0; i < n; ++i)
        {
            if (g_MuteAddr[i])
            {
                WritePatchBytes(g_MuteAddr[i], sites[i].expect, 5);
                g_MuteAddr[i] = nullptr;
            }
        }
        g_SoundMuted = false;
    }

    static uintptr_t BarrierRegisterDefenseCbAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140FF5BB0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x140FF5C40ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x1496DA350ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x14A13FBB0ull;
            default: return 0;
        }
    }
    static uintptr_t BarrierDefenseFunctorAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x142ACEB58ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x142ACEB58ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x142ACEB58ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x142ACEB58ull;
            default: return 0;
        }
    }

    static uintptr_t BarrierPredicateAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140FFAAA0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x140FFAB30ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x1496DD430ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x14A142E80ull;
            default: return 0;
        }
    }

    static uintptr_t BarrierPlayerDtorAddr()
    {
        switch (gGameBuild)
        {
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140FF4AE0ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x140FF4B70ull;
            case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x1496D5120ull;
            case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x14A137900ull;
            default: return 0;
        }
    }

    static std::atomic<bool> g_DesiredUp{ false };
    static std::atomic<bool> g_PrevBit18Want{ false };
    static std::atomic<bool> g_Failed{ false };
    static void* g_SetEquipItemCallRet = nullptr;
    static std::atomic<bool> g_ShieldUpCache{ false };
    static std::atomic<bool> g_ShieldDeployed{ false };
    static std::atomic<bool> g_PrevShieldActive{ false };

    static void* g_GitAddr = nullptr;
    static void* g_UpdAddr = nullptr;
    static void* g_SpnAddr = nullptr;

    constexpr std::uint64_t kPlayerIndex    = 0;
    constexpr int           kBarrierEquipId = 0x1E9;
    constexpr std::uint32_t kBit14          = 0x00004000u;
    constexpr std::uint32_t kBit18          = 0x00040000u;
    constexpr std::size_t   kSlots          = 256;
    constexpr int           kShieldEffectId = 0;

    static std::int32_t g_Arr808[kSlots];
    static std::int32_t g_Arr810[kSlots];
    static std::uint8_t g_Arr818[kSlots];
    static void*        g_Arr828[kSlots];
    static void*        g_Arr830[1024];
    alignas(16) static std::uint8_t g_SlotObj[0x100];
    static void* g_SlotVtbl[64];
    alignas(16) static std::uint8_t g_XformBuf[0x40];

    static void* __fastcall SlotGetXform(void*) { return g_XformBuf; }

    static void InitFakeStorage()
    {
        for (std::size_t i = 0; i < kSlots; ++i)
        {
            g_Arr808[i] = -1;
            g_Arr810[i] = -1;
            g_Arr818[i] = 0;
            g_Arr828[i] = &g_SlotObj;
        }
        std::memset(g_Arr830, 0, sizeof(g_Arr830));
        std::memset(g_SlotObj, 0, sizeof(g_SlotObj));
        std::memset(g_XformBuf, 0, sizeof(g_XformBuf));
        for (std::size_t i = 0; i < 64; ++i)
            g_SlotVtbl[i] = reinterpret_cast<void*>(&SlotGetXform);
        *reinterpret_cast<void**>(g_SlotObj) = g_SlotVtbl;
    }

    static bool InFob()
    {
        if (!g_IsFobMode) return false;
        __try { return g_IsFobMode() != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static int __fastcall hk_GetItemId(void* lmenu, unsigned int index, std::uint64_t a3, std::uint64_t a4)
    {
        int id = g_OrigGetItemId(lmenu, index, a3, a4);
        if (_ReturnAddress() == g_SetEquipItemCallRet)
            g_DesiredUp.store(id == kBarrierEquipId, std::memory_order_relaxed);
        return id;
    }

    static void DrainBarrierDamageEmits()
    {
        unsigned rd = g_DmgRd.load(std::memory_order_relaxed);
        const unsigned wr = g_DmgWr.load(std::memory_order_acquire);
        while (rd != wr)
        {
            const DmgEvent& e = g_DmgQueue[rd & 15];
            V_FrameWork::EmitMessage("Player", "BarrierDamage", e.playerIndex, e.before, e.after);
            ++rd;
        }
        g_DmgRd.store(rd, std::memory_order_relaxed);
    }

    static void* __fastcall hk_Update(void* r15, std::uint64_t index, void* r8, void* r9)
    {
        if (g_Failed.load(std::memory_order_relaxed) || index != kPlayerIndex || !r15)
            return g_Orig ? g_Orig(r15, index, r8, r9) : nullptr;

        if (InFob())
            return g_Orig ? g_Orig(r15, index, r8, r9) : nullptr;

        g_Tick.fetch_add(1, std::memory_order_relaxed);

        const bool desired  = g_DesiredUp.load(std::memory_order_relaxed);
        const bool portalUp = WormholePortal_AnyDeployed();
        std::uint32_t* sub204     = nullptr;
        std::uint32_t  oldSub204  = 0;
        bool           forced     = false;
        bool           wantBit18  = false;

        __try
        {
            const std::int32_t base = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(r15) + 8);
            const std::int64_t idx  = static_cast<std::int64_t>(index) - base;
            std::uint8_t* recBase = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(r15) + 0x280);
            std::uint8_t* player  = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(r15) + 0x408);
            if (idx >= 0 && idx < static_cast<std::int64_t>(kSlots) && recBase && player)
            {
                std::uint8_t* sub = *reinterpret_cast<std::uint8_t**>(player + 0x138);
                if (sub)
                {
                    std::uint32_t* field10 = reinterpret_cast<std::uint32_t*>(recBase + idx * 0x20 + 0x10);
                    sub204 = reinterpret_cast<std::uint32_t*>(sub + 0x204);
                    void** p808 = reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(r15) + 0x808);

                    bool shieldActive = false;
                    std::uint8_t* p60 = *reinterpret_cast<std::uint8_t**>(sub + 0x60);
                    std::uint8_t* ab  = p60 ? *reinterpret_cast<std::uint8_t**>(p60 + 0x8) : nullptr;
                    if (ab)
                        shieldActive = *reinterpret_cast<float*>(ab + static_cast<std::size_t>(kShieldEffectId) * 0x4c + 0x3c) > 0.0f;

                    bool domeAlive = false;
                    {
                        void** arr828pre = *reinterpret_cast<void***>(reinterpret_cast<std::uint8_t*>(r15) + 0x828);
                        std::int32_t* h808 = *reinterpret_cast<std::int32_t**>(reinterpret_cast<std::uint8_t*>(r15) + 0x808);
                        std::uint8_t* b818 = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(r15) + 0x818);
                        if (arr828pre && arr828pre != g_Arr828 &&
                            h808 && h808 != g_Arr808 && b818 && b818 != g_Arr818)
                            domeAlive = b818[idx] == 0 && h808[idx] != -1;
                    }
                    const bool active = desired || shieldActive || domeAlive;
                    wantBit18 = active || portalUp;

                    g_ShieldUpCache.store(desired || shieldActive, std::memory_order_relaxed);
                    g_ShieldDeployed.store(shieldActive, std::memory_order_relaxed);

                    const bool prevWant = g_PrevBit18Want.exchange(wantBit18,
                                                                   std::memory_order_relaxed);
                    if (prevWant && !wantBit18)
                        *sub204 &= ~kBit18;

                    const bool prevActive = g_PrevShieldActive.exchange(active,
                                                                        std::memory_order_relaxed);
                    if (prevActive && !active)
                    {
                        __try
                        {
                            *reinterpret_cast<std::uint8_t*>(sub + 0x3C0) &= 0xFEu;
                            if (g_FobAbsorbForced && g_FobAbsorbByte)
                            {
                                std::uint8_t* lifeIf = *reinterpret_cast<std::uint8_t**>(sub + 0xC0);
                                if (lifeIf && lifeIf - 0x20 + 0x8E == g_FobAbsorbByte)
                                    *g_FobAbsorbByte = 0;
                            }
                            g_FobAbsorbForced = false;
                            g_FobAbsorbByte   = nullptr;
                            g_HpFloorValid    = false;
                            g_LeakTick        = 0;
                            for (unsigned i = 0; i < 4; ++i)
                                g_DedupRing[i] = { 0, 0 };
                            RestoreBarrierSoundMute();
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }

                    if (active)
                    {
                        if (*p808 == nullptr)
                        {
                            *p808 = g_Arr808;
                            *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(r15) + 0x810) = g_Arr810;
                            *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(r15) + 0x818) = g_Arr818;
                            *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(r15) + 0x828) = g_Arr828;
                            *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(r15) + 0x830) = g_Arr830;
                        }
                        *field10 |= kBit14;
                        oldSub204 = *sub204;
                        *sub204 |= kBit18;
                        forced = true;

                        void** arr828 = *reinterpret_cast<void***>(reinterpret_cast<std::uint8_t*>(r15) + 0x828);
                        if (arr828 && arr828 != g_Arr828)
                        {
                            std::uint8_t* node = reinterpret_cast<std::uint8_t*>(arr828[idx]);
                            if (node)
                            {
                                if (g_RegisterDefenseCb && g_DefenseFunctor &&
                                    *reinterpret_cast<void**>(node + 0x18) != g_DefenseFunctor)
                                {
                                    g_RegisterDefenseCb(r15, static_cast<std::uint32_t>(idx));
                                    if (*reinterpret_cast<void**>(node + 0x18) != g_DefenseFunctor)
                                    {
                                        static std::atomic<bool> s_RegWarned{ false };
                                        bool re = false;
                                        if (s_RegWarned.compare_exchange_strong(re, true))
                                            Log("[Barrier] WARNING: defense-callback registration did not take (node=%p); dome hits will not register.\n",
                                                static_cast<void*>(node));
                                    }
                                }

                                const std::uint16_t pid = *reinterpret_cast<std::uint16_t*>(sub + 0x218);
                                const std::uint64_t expectedOwner = 0x8800000000000000ull | pid;
                                if (pid != 0xFFFFu &&
                                    *reinterpret_cast<std::uint64_t*>(node + 0x50) != expectedOwner)
                                    *reinterpret_cast<std::uint64_t*>(node + 0x50) = expectedOwner;
                            }
                        }

                        if (shieldActive)
                        {
                            std::uint8_t* lifeIf  = *reinterpret_cast<std::uint8_t**>(sub + 0xC0);
                            std::uint8_t* lifeCtl = lifeIf ? lifeIf - 0x20 : nullptr;
                            if (lifeCtl && !g_FobAbsorbForced)
                            {
                                if (lifeCtl[0x8E] == 0)
                                {
                                    lifeCtl[0x8E]     = 1;
                                    g_FobAbsorbByte   = lifeCtl + 0x8E;
                                    g_FobAbsorbForced = true;
                                }
                                else
                                {
                                    static std::atomic<bool> s_AbsorbWarned{ false };
                                    bool aw = false;
                                    if (s_AbsorbWarned.compare_exchange_strong(aw, true))
                                        Log("[Barrier] WARNING: LifeController FOB-absorb byte @ %p already 0x%02X; not forcing (dome hits may damage the player).\n",
                                            static_cast<void*>(lifeCtl + 0x8E), lifeCtl[0x8E]);
                                }
                            }

                            ApplyBarrierSoundMute();

                            if (ab)
                            {
                                std::uint8_t* rec  = ab + (std::size_t)idx * 0x4C;
                                float* hp   = reinterpret_cast<float*>(rec);
                                float* stam = reinterpret_cast<float*>(rec + 0x2C);
                                std::int32_t* sleepState = reinterpret_cast<std::int32_t*>(rec + 0x34);
                                if (!g_HpFloorValid)
                                {
                                    g_HpFloor      = *hp;
                                    g_StamFloor    = *stam;
                                    g_HpFloorValid = *hp > 0.0f;
                                }
                                else
                                {
                                    std::uint8_t* pcore   = *reinterpret_cast<std::uint8_t**>(player + 0x78);
                                    std::uint8_t* pactArr = pcore ? *reinterpret_cast<std::uint8_t**>(pcore + 0x2D8) : nullptr;
                                    const bool unconscious = pactArr && pactArr[idx] == 0x19u;

                                    bool leaked = false;
                                    if (*hp < g_HpFloor) { *hp = g_HpFloor; leaked = true; }
                                    else                 g_HpFloor = *hp;
                                    if (*stam < g_StamFloor) { *stam = g_StamFloor; leaked = true; }
                                    else                     g_StamFloor = *stam;
                                    if (!unconscious && *sleepState != 0) { *sleepState = 0; leaked = true; }

                                    const std::uint32_t now = g_Tick.load(std::memory_order_relaxed);
                                    if (leaked && now - g_LeakTick >= kRehitTicks)
                                    {
                                        g_LeakTick = now;
                                        float* gauge = reinterpret_cast<float*>(rec + 0x3C);
                                        if (*gauge > 0.0f)
                                        {
                                            const float before = *gauge;
                                            float nv = before - 1.0f;
                                            if (nv < -1.0f) nv = -1.0f;
                                            *gauge = nv;

                                            const unsigned wr = g_DmgWr.load(std::memory_order_relaxed);
                                            if (wr - g_DmgRd.load(std::memory_order_relaxed) < 16)
                                            {
                                                g_DmgQueue[wr & 15] = { static_cast<int>(idx), before, nv };
                                                g_DmgWr.store(wr + 1, std::memory_order_release);
                                            }

                                            void* skillIf = *reinterpret_cast<void**>(sub + 0xF8);
                                            if (skillIf)
                                            {
                                                void** vt = *reinterpret_cast<void***>(skillIf);
                                                auto isActive     = reinterpret_cast<IsSkillActiveFn>(vt[2]);
                                                auto updateCommon = reinterpret_cast<SkillUpdateFn>(vt[4]);
                                                if (isActive && updateCommon &&
                                                    isActive(skillIf, static_cast<std::uint32_t>(idx), 0xFu))
                                                    updateCommon(skillIf, static_cast<std::uint32_t>(idx), 0x10u, 1.0f);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_Failed.store(true, std::memory_order_relaxed);
            Log("[Barrier] ERROR: Energy Wall render faulted reading player state; render disabled to stay safe.\n");
            return g_Orig ? g_Orig(r15, index, r8, r9) : nullptr;
        }

        DrainBarrierDamageEmits();

        void* ret = nullptr;
        __try
        {
            ret = g_Orig ? g_Orig(r15, index, r8, r9) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (forced && sub204) *sub204 = oldSub204;
            g_Failed.store(true, std::memory_order_relaxed);
            Log("[Barrier] ERROR: engine effect update crashed while the Energy Wall was forced; reverted + render disabled.\n");
            return ret;
        }

        if (wantBit18 && sub204) *sub204 |= kBit18;
        return ret;
    }

    static std::uint32_t __fastcall hk_BlowCb(void* callable, void* offense,
                                              void* defense, void* result, void* core)
    {
        bool rehitDrain = false;
        __try
        {
            if (core && offense && defense && result)
            {
                std::uint8_t* c = reinterpret_cast<std::uint8_t*>(core);
                std::uint8_t* player = *reinterpret_cast<std::uint8_t**>(c + 0x408);
                std::uint8_t* sub = player ? *reinterpret_cast<std::uint8_t**>(player + 0x138) : nullptr;
                std::uint8_t* defRec = *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(defense) + 0x28);
                if (sub && defRec)
                {
                    const unsigned int defObjId = *reinterpret_cast<std::uint16_t*>(defRec);
                    std::uint8_t* mapTbl = *reinterpret_cast<std::uint8_t**>(sub + 0x70);
                    const std::uint16_t mappedIdx = mapTbl
                        ? *reinterpret_cast<std::uint16_t*>(mapTbl + (std::size_t)(defObjId & 0x1FF) * 2)
                        : 0xFFFFu;
                    const int base  = *reinterpret_cast<std::int32_t*>(c + 8);
                    const int count = *reinterpret_cast<std::int32_t*>(c + 0xC);
                    const int slot  = static_cast<int>(mappedIdx) - base;

                    if (mappedIdx != 0xFFFFu && slot >= 0 && slot < count && count <= 256)
                    {
                        std::uint8_t* atk = *reinterpret_cast<std::uint8_t**>(
                            reinterpret_cast<std::uint8_t*>(offense) + 0x28);
                        std::uint16_t attackId  = 1;
                        std::uint16_t shooter   = 0xFFFF;
                        std::uint32_t attackKey = 0;
                        if (atk)
                        {
                            attackId  = *reinterpret_cast<std::uint16_t*>(atk - 0x20 + 0x28);
                            shooter   = *reinterpret_cast<std::uint16_t*>(atk - 0x20 + 0x26);
                            attackKey = *reinterpret_cast<std::uint32_t*>(atk - 0x20 + 0x24);
                            if (attackId == 0xFFFFu) attackId = 1;
                        }

                        const std::uint32_t now = g_Tick.load(std::memory_order_relaxed);
                        bool duplicate = false;
                        bool rehit     = false;
                        if (atk)
                        {
                            int found = -1;
                            for (unsigned i = 0; i < 4; ++i)
                                if (g_DedupRing[i].key == attackKey) { found = static_cast<int>(i); break; }
                            if (found >= 0)
                            {
                                const std::uint32_t gap = now - g_DedupRing[found].tick;
                                g_DedupRing[found].tick = now;
                                if (gap <= 1)
                                    duplicate = true;
                                else if (gap >= kRehitTicks)
                                    rehit = true;
                            }
                            else
                                g_DedupRing[g_DedupPos.fetch_add(1, std::memory_order_relaxed) & 3] = { attackKey, now };
                        }

                        if (!duplicate)
                        {
                            std::uint8_t* p60 = *reinterpret_cast<std::uint8_t**>(sub + 0x60);
                            std::uint8_t* ab  = p60 ? *reinterpret_cast<std::uint8_t**>(p60 + 8) : nullptr;
                            float* gauge = ab
                                ? reinterpret_cast<float*>(ab + (std::size_t)slot * 0x4C + 0x3C)
                                : nullptr;
                            if (gauge && *gauge > 0.0f)
                            {
                                const float before = *gauge;
                                float nv = before - 1.0f;
                                if (nv < -1.0f) nv = -1.0f;
                                *gauge = nv;

                                const unsigned wr = g_DmgWr.load(std::memory_order_relaxed);
                                if (wr - g_DmgRd.load(std::memory_order_relaxed) < 16)
                                {
                                    g_DmgQueue[wr & 15] = { slot, before, nv };
                                    g_DmgWr.store(wr + 1, std::memory_order_release);
                                }

                                if (rehit)
                                {
                                    void* skillIf = *reinterpret_cast<void**>(sub + 0xF8);
                                    if (skillIf)
                                    {
                                        void** vt = *reinterpret_cast<void***>(skillIf);
                                        auto isActive     = reinterpret_cast<IsSkillActiveFn>(vt[2]);
                                        auto updateCommon = reinterpret_cast<SkillUpdateFn>(vt[4]);
                                        if (isActive && updateCommon &&
                                            isActive(skillIf, static_cast<std::uint32_t>(slot), 0xFu))
                                        {
                                            updateCommon(skillIf, static_cast<std::uint32_t>(slot), 0x10u, 1.0f);
                                            rehitDrain = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (rehitDrain && !InFob())
            return 1;

        return g_OrigBlowCb(callable, offense, defense, result, core);
    }

    static std::uint32_t __fastcall hk_Predicate(void* a1, void* a2, void* a3, void* a4,
                                                 void* a5, void* a6)
    {
        const std::uint32_t r = g_OrigPredicate(a1, a2, a3, a4, a5, a6);
        if (r == 0 && g_ShieldDeployed.load(std::memory_order_relaxed) && !InFob())
        {
            static std::atomic<bool> s_PredWarned{ false };
            bool e = false;
            if (s_PredWarned.compare_exchange_strong(e, true))
                Log("[Barrier] WARNING: wall pair predicate rejected in SP -- forcing accept so the dome keeps stopping bullets.\n");
            return 1;
        }
        return r;
    }

    static void __fastcall hk_PlayerDtor(void* self)
    {
        if (self)
        {
            __try
            {
                auto* p = reinterpret_cast<std::uint8_t*>(self);
                if (*reinterpret_cast<void**>(p + 0x808) == g_Arr808)
                {
                    *reinterpret_cast<void**>(p + 0x808) = nullptr;
                    *reinterpret_cast<void**>(p + 0x810) = nullptr;
                    *reinterpret_cast<void**>(p + 0x818) = nullptr;
                    *reinterpret_cast<void**>(p + 0x828) = nullptr;
                    *reinterpret_cast<void**>(p + 0x830) = nullptr;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        g_OrigDtor(self);
    }

}

bool Install_BarrierEffectSpawn()
{
    InitFakeStorage();

    g_IsFobMode = reinterpret_cast<BoolFn>(
        ResolveGameAddress(gAddr.Barrier_IsFobMode));
    if (!g_IsFobMode)
        Log("[Barrier] WARNING: no IsFobMode address for %s; the dome cannot stand down inside FOB missions (may render there too).\n", GetGameBuildName(gGameBuild));

    g_SetEquipItemCallRet = ResolveGameAddress(gAddr.Barrier_EquipItemCallRet);
    if (!g_SetEquipItemCallRet)
        Log("[Barrier] WARNING: no equip return-address for %s; the dome may not trigger when the Energy Wall is equipped.\n", GetGameBuildName(gGameBuild));

    void* git = ResolveGameAddress(gAddr.Barrier_GetItemId);
    const bool gitOk = git && CreateAndEnableHook(git, reinterpret_cast<void*>(&hk_GetItemId), reinterpret_cast<void**>(&g_OrigGetItemId));
    g_GitAddr = gitOk ? git : nullptr;
    if (!gitOk)
        Log("[Barrier] ERROR: could not hook equip detection (addr 0x%llX) for %s; the Energy Wall dome will never appear on this build.\n",
            static_cast<unsigned long long>(gAddr.Barrier_GetItemId), GetGameBuildName(gGameBuild));

    void* upd = ResolveGameAddress(gAddr.Barrier_Updater);
    const bool updOk = upd && CreateAndEnableHook(upd, reinterpret_cast<void*>(&hk_Update), reinterpret_cast<void**>(&g_Orig));
    g_UpdAddr = updOk ? upd : nullptr;
    if (!updOk)
        Log("[Barrier] ERROR: could not hook the effect updater (addr 0x%llX) for %s; the Energy Wall dome will not render on this build.\n",
            static_cast<unsigned long long>(gAddr.Barrier_Updater), GetGameBuildName(gGameBuild));

    void* dtor = ResolveGameAddress(BarrierPlayerDtorAddr());
    if (dtor && CreateAndEnableHook(dtor, reinterpret_cast<void*>(&hk_PlayerDtor), reinterpret_cast<void**>(&g_OrigDtor)))
        g_DtorAddr = dtor;
    else if (!dtor)
        Log("[Barrier] WARNING: no teardown-cleanup address for %s; reloading a checkpoint with the dome up may crash.\n", GetGameBuildName(gGameBuild));
    else
        Log("[Barrier] ERROR: could not hook the teardown-cleanup site for %s; reloading a checkpoint with the dome up may crash.\n", GetGameBuildName(gGameBuild));

    void* blow = reinterpret_cast<void*>(BarrierBlowCbAddr());
    if (blow && CreateAndEnableHook(blow, reinterpret_cast<void*>(&hk_BlowCb), reinterpret_cast<void**>(&g_OrigBlowCb)))
        g_BlowCbAddr = blow;
    else
        Log("[Barrier] WARNING: dome hit hook unavailable for %s; self pulses will drain the wall and no BarrierDamage messages will be sent.\n",
            GetGameBuildName(gGameBuild));

    g_RegisterDefenseCb = reinterpret_cast<RegisterCbFn>(BarrierRegisterDefenseCbAddr());
    g_DefenseFunctor    = reinterpret_cast<void*>(BarrierDefenseFunctorAddr());
    if (!g_RegisterDefenseCb || !g_DefenseFunctor)
        Log("[Barrier] WARNING: no defense-callback registration address for %s; dome hits will not register (no HP loss or hit effects).\n",
            GetGameBuildName(gGameBuild));

    void* pred = reinterpret_cast<void*>(BarrierPredicateAddr());
    if (pred && CreateAndEnableHook(pred, reinterpret_cast<void*>(&hk_Predicate), reinterpret_cast<void**>(&g_OrigPredicate)))
        g_PredicateAddr = pred;
    else if (g_RegisterDefenseCb)
        Log("[Barrier] WARNING: wall pair-predicate hook unavailable for %s; if its FOB state is absent the dome may stop blocking bullets.\n",
            GetGameBuildName(gGameBuild));

    return gitOk && updOk;
}

void Uninstall_BarrierEffectSpawn()
{
    if (g_GitAddr) DisableAndRemoveHook(g_GitAddr);
    if (g_UpdAddr) DisableAndRemoveHook(g_UpdAddr);
    if (g_DtorAddr) DisableAndRemoveHook(g_DtorAddr);
    if (g_BlowCbAddr) DisableAndRemoveHook(g_BlowCbAddr);
    if (g_PredicateAddr) DisableAndRemoveHook(g_PredicateAddr);
    RestoreBarrierSoundMute();
    g_Orig          = nullptr;
    g_OrigGetItemId = nullptr;
    g_OrigDtor      = nullptr;
    g_OrigBlowCb    = nullptr;
    g_OrigPredicate = nullptr;
    g_RegisterDefenseCb = nullptr;
    g_DefenseFunctor    = nullptr;
    g_GitAddr = g_UpdAddr = g_DtorAddr = g_BlowCbAddr = g_PredicateAddr = nullptr;
}

bool BarrierEffect_IsShieldActive()
{
    return g_ShieldUpCache.load(std::memory_order_relaxed);
}

bool BarrierEffect_IsShieldDeployed()
{
    return g_ShieldDeployed.load(std::memory_order_relaxed) && !InFob();
}
