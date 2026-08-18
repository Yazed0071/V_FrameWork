#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <intrin.h>

#include "AddressSet.h"
#include "CqcActionPluginImpl_StateHoldMove.h"
#include "HookUtils.h"
#include "LuaApi.h"
#include "MissionCodeGuard.h"
#include "log.h"

#pragma intrinsic(_ReturnAddress)

namespace
{
    constexpr std::size_t   kWorkStride            = 0x610;
    constexpr std::size_t   kWorkDesiredStance     = 0x604;
    constexpr std::uint8_t  kDesiredStanceStandBit = 0x01;
    constexpr std::uint32_t kStanceChangeInputBit  = 0x02;
    constexpr std::uint32_t kStanceGateSignature   = 0x01A8E8D1u;

    constexpr int           kPlayerStanceStand     = 1;
    constexpr int           kPlayerStanceSquat     = 2;
    constexpr std::uint64_t kPendingHoldLifetimeMs = 3000;
    constexpr std::uint64_t kHoldLiveWindowMs      = 250;

    using StateHoldMove_t = void(__fastcall*)(void* self, std::uint32_t charIndex,
                                              std::uint32_t stateProcess, void* param);
    using InputFlags_t    = std::uint32_t(__fastcall*)(void* self);
    using LuaStanceReq_t  = int(__fastcall*)(lua_State* L);

    std::atomic<int>           g_stance{ kCqcStanceNone };
    std::atomic<std::uint64_t> g_stanceTick{ 0 };
    std::atomic<std::uint64_t> g_holdTick{ 0 };

    StateHoldMove_t g_OrigState      = nullptr;
    void*           g_StateTarget    = nullptr;
    StateHoldMove_t g_OrigGunState   = nullptr;
    void*           g_GunStateTarget = nullptr;

    InputFlags_t      g_OrigInput   = nullptr;
    void*             g_InputTarget = nullptr;
    std::atomic<bool> g_inputHookTried{ false };

    thread_local int t_inputOverride = 0;

    std::uintptr_t Addr_StateHoldMove()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x1411666A0ull;
        case GB::Jp_1_0_15_4:
        case GB::Jp_1_0_15_4a: return 0x141166730ull;
        case GB::En_1_0_15_3:  return 0x141166D30ull;
        case GB::Jp_1_0_15_3:  return 0x141166DB0ull;
        default:               return 0;
        }
    }

    std::uintptr_t Addr_StateGunHoldMove()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x1411644E0ull;
        case GB::Jp_1_0_15_4:
        case GB::Jp_1_0_15_4a: return 0x141164570ull;
        case GB::En_1_0_15_3:  return 0x141164B70ull;
        case GB::Jp_1_0_15_3:  return 0x141164BF0ull;
        default:               return 0;
        }
    }

    std::uintptr_t Addr_RequestToSetTargetStance()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x1409DB170ull;
        case GB::Jp_1_0_15_4:
        case GB::Jp_1_0_15_4a: return 0x1409DAFE0ull;
        case GB::En_1_0_15_3:  return 0x1409DA3E0ull;
        case GB::Jp_1_0_15_3:  return 0x1409D9ED0ull;
        default:               return 0;
        }
    }

    bool CallStanceRequestSeh(LuaStanceReq_t fn, lua_State* L)
    {
        __try
        {
            fn(L);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct HoldContext
    {
        std::uint8_t* work;
        void*         inputFlagsFn;
        int           currentStance;
    };

    bool ResolveHoldContextSeh(void* self, std::uint32_t charIndex, HoldContext* out)
    {
        __try
        {
            std::uint8_t* const self8 = static_cast<std::uint8_t*>(self);
            std::uint8_t* const owner = *reinterpret_cast<std::uint8_t**>(self8 + 0x08);
            std::uint8_t* const sys   = *reinterpret_cast<std::uint8_t**>(owner + 0x138);

            std::uint8_t* const  stanceTable = *reinterpret_cast<std::uint8_t**>(sys + 0x60);
            const std::uint8_t*  stanceRow   = *reinterpret_cast<std::uint8_t* const*>(stanceTable + 0x30);

            std::uint8_t* const pool  = *reinterpret_cast<std::uint8_t**>(self8 + 0x68);
            std::uint8_t* const owner2 = *reinterpret_cast<std::uint8_t**>(self8 + 0x38);
            const std::uint32_t first = *reinterpret_cast<const std::uint32_t*>(owner2 + 0x24);

            void* const  input = *reinterpret_cast<void**>(sys + 0xB0);
            void** const vtbl  = *reinterpret_cast<void***>(input);

            out->work          = pool + static_cast<std::size_t>(charIndex - first) * kWorkStride;
            out->inputFlagsFn  = vtbl[8];
            out->currentStance = (stanceRow[charIndex] == 1) ? kCqcStanceStanding
                                                             : kCqcStanceCrouching;

            const volatile std::uint8_t probe = out->work[kWorkDesiredStance];
            (void)probe;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void WriteDesiredStanceSeh(std::uint8_t* work, int stance)
    {
        __try
        {
            std::uint8_t& desired = work[kWorkDesiredStance];
            if (stance == kCqcStanceStanding)
                desired = static_cast<std::uint8_t>(desired | kDesiredStanceStandBit);
            else
                desired = static_cast<std::uint8_t>(desired & ~kDesiredStanceStandBit);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    std::uint32_t __fastcall hk_InputFlags(void* self)
    {
        const std::uint32_t flags = g_OrigInput ? g_OrigInput(self) : 0;

        if (t_inputOverride == 0)
            return flags;

        if (*static_cast<const std::uint32_t*>(_ReturnAddress()) != kStanceGateSignature)
            return flags;

        return flags | kStanceChangeInputBit;
    }

    void EnsureInputHook(void* fn)
    {
        if (fn == nullptr || g_OrigInput != nullptr)
            return;
        if (g_inputHookTried.exchange(true))
            return;

        if (!CreateAndEnableHook(fn, reinterpret_cast<void*>(&hk_InputFlags),
                                 reinterpret_cast<void**>(&g_OrigInput)))
        {
            Log("[TargetCqcStance] ERROR: the CQC hold input-flag reader at %p could not be hooked - "
                "V_Player.RequestToSetTargetCqcStance can still park the desired-stance flag, but the "
                "hold keeps whatever stance the player pressed.\n", fn);
            return;
        }

        g_InputTarget = fn;
    }

    void ArmPendingStance(void* self, std::uint32_t charIndex)
    {
        g_holdTick.store(GetTickCount64(), std::memory_order_relaxed);

        int stance = g_stance.load(std::memory_order_relaxed);

        if (stance != kCqcStanceNone)
        {
            const std::uint64_t issued = g_stanceTick.load(std::memory_order_relaxed);
            if (issued != 0 && GetTickCount64() - issued > kPendingHoldLifetimeMs)
            {
                g_stance.store(kCqcStanceNone, std::memory_order_relaxed);
                stance = kCqcStanceNone;
            }
        }

        if (stance == kCqcStanceNone || self == nullptr)
            return;

        HoldContext ctx{};
        if (!ResolveHoldContextSeh(self, charIndex, &ctx))
            return;

        if (ctx.currentStance == stance)
        {
            g_stance.compare_exchange_strong(stance, kCqcStanceNone, std::memory_order_relaxed);
            return;
        }

        WriteDesiredStanceSeh(ctx.work, stance);
        EnsureInputHook(ctx.inputFlagsFn);

        if (g_OrigInput != nullptr)
            t_inputOverride = 1;
    }

    void __fastcall hk_StateHoldMove(void* self, std::uint32_t charIndex,
                                     std::uint32_t stateProcess, void* param)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigState, self, charIndex, stateProcess, param);

        ArmPendingStance(self, charIndex);

        if (g_OrigState)
            g_OrigState(self, charIndex, stateProcess, param);

        t_inputOverride = 0;
    }

    void __fastcall hk_StateGunHoldMove(void* self, std::uint32_t charIndex,
                                        std::uint32_t stateProcess, void* param)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigGunState, self, charIndex, stateProcess, param);

        ArmPendingStance(self, charIndex);

        if (g_OrigGunState)
            g_OrigGunState(self, charIndex, stateProcess, param);

        t_inputOverride = 0;
    }
}

void Set_TargetCqcStance(int stance)
{
    g_stanceTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_stance.store(stance, std::memory_order_relaxed);
}

bool Request_PlayerTargetStance(lua_State* L, int stance)
{
    MISSION_GUARD_RETURN_FALSE();

    if (L == nullptr)
        return false;

    const std::uintptr_t addr = Addr_RequestToSetTargetStance();
    if (addr == 0)
    {
        Log("[TargetCqcStance] ERROR: Player::RequestToSetTargetStance is unmapped for this build - "
            "V_Player.RequestToSetTargetCqcStance still works during a CQC hold, but it cannot change "
            "the stance while the player is aiming or moving normally.\n");
        return false;
    }

    void* const target = ResolveGameAddress(addr);
    if (target == nullptr || !ResolveLuaApi())
        return false;

    PushLuaNumber(L, static_cast<float>(stance == kCqcStanceStanding ? kPlayerStanceStand
                                                                    : kPlayerStanceSquat));

    const bool ok = CallStanceRequestSeh(reinterpret_cast<LuaStanceReq_t>(target), L);

    LuaPop(L, 1);

    if (!ok)
    {
        Log("[TargetCqcStance] ERROR: Player::RequestToSetTargetStance at %p faulted - the stance "
            "request outside a CQC hold was dropped; the player keeps the stance he is in.\n", target);
        return false;
    }

    return true;
}

int Get_TargetCqcStance()
{
    return g_stance.load(std::memory_order_relaxed);
}

bool IsInCqcHold()
{
    const std::uint64_t tick = g_holdTick.load(std::memory_order_relaxed);
    return tick != 0 && GetTickCount64() - tick <= kHoldLiveWindowMs;
}

void Clear_TargetCqcStance()
{
    g_stance.store(kCqcStanceNone, std::memory_order_relaxed);
    g_holdTick.store(0, std::memory_order_relaxed);
}

bool Install_TargetCqcStance_Hook()
{
    const std::uintptr_t addr = Addr_StateHoldMove();
    if (!addr)
    {
        Log("[TargetCqcStance] ERROR: CqcActionPluginImpl::StateHoldMove address is missing for this "
            "build - V_Player.RequestToSetTargetCqcStance will be accepted but the CQC hold keeps its "
            "vanilla stance.\n");
        return true;
    }

    void* target = ResolveGameAddress(addr);
    if (!target || !CreateAndEnableHook(target, reinterpret_cast<void*>(&hk_StateHoldMove),
                                        reinterpret_cast<void**>(&g_OrigState)))
    {
        Log("[TargetCqcStance] ERROR: CqcActionPluginImpl::StateHoldMove hook failed - "
            "V_Player.RequestToSetTargetCqcStance will do nothing.\n");
        return false;
    }

    g_StateTarget = target;

    const std::uintptr_t gunAddr = Addr_StateGunHoldMove();
    if (!gunAddr)
    {
        Log("[TargetCqcStance] ERROR: CqcActionPluginImpl::StateGunHoldMove address is missing for "
            "this build - V_Player.RequestToSetTargetCqcStance works during a plain CQC hold but does "
            "nothing while the player aims a gun during that hold, because the aiming hold runs a "
            "different state handler.\n");
        return true;
    }

    void* gunTarget = ResolveGameAddress(gunAddr);
    if (!gunTarget || !CreateAndEnableHook(gunTarget, reinterpret_cast<void*>(&hk_StateGunHoldMove),
                                           reinterpret_cast<void**>(&g_OrigGunState)))
    {
        Log("[TargetCqcStance] ERROR: CqcActionPluginImpl::StateGunHoldMove hook at %p failed - "
            "V_Player.RequestToSetTargetCqcStance is ignored while the player aims a gun during a CQC "
            "hold; the plain hold still works.\n", gunTarget);
        return true;
    }

    g_GunStateTarget = gunTarget;
    return true;
}

bool Uninstall_TargetCqcStance_Hook()
{
    if (g_InputTarget)
        DisableAndRemoveHook(g_InputTarget);
    g_InputTarget = nullptr;
    g_OrigInput   = nullptr;
    g_inputHookTried.store(false);

    if (g_GunStateTarget)
        DisableAndRemoveHook(g_GunStateTarget);
    g_GunStateTarget = nullptr;
    g_OrigGunState   = nullptr;

    if (g_StateTarget)
        DisableAndRemoveHook(g_StateTarget);
    g_StateTarget = nullptr;
    g_OrigState   = nullptr;
    Clear_TargetCqcStance();
    return true;
}
