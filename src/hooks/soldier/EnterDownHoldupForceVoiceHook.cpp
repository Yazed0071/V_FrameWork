#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <atomic>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"

namespace
{


    using State_EnterDownHoldup_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc);


    static State_EnterDownHoldup_t g_OrigState_EnterDownHoldup = nullptr;


    static std::atomic<std::uint64_t> g_RngState{ 0 };
}


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


static bool SafeReadDword(std::uintptr_t addr, std::uint32_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint32_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


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


static std::uintptr_t GetHoldupEntry(void* self, std::uint32_t actorId)
{
    if (!self)
        return 0;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint32_t baseIndex = 0;
    std::uint64_t tableBase = 0;

    if (!SafeReadDword(selfAddr + 0x90ull, baseIndex))
        return 0;

    if (!SafeReadQword(selfAddr + 0x88ull, tableBase))
        return 0;

    const std::uint32_t slot = actorId - baseIndex;
    return static_cast<std::uintptr_t>(tableBase + (static_cast<std::uint64_t>(slot) * 0x40ull));
}


static void DispatchHoldupDownReaction(void* self, std::uint32_t actorId, std::uint32_t reactionHash)
{
    if (!self)
        return;

    __try
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

        std::uint64_t managerRoot = 0;
        if (!SafeReadQword(selfAddr + 0x70ull, managerRoot) || !managerRoot)
            return;

        std::uint64_t reactionMgr = 0;
        if (!SafeReadQword(static_cast<std::uintptr_t>(managerRoot) + 0xA8ull, reactionMgr) || !reactionMgr)
            return;

        std::uint64_t vtbl = 0;
        if (!SafeReadQword(static_cast<std::uintptr_t>(reactionMgr), vtbl) || !vtbl)
            return;

        std::uint64_t fnAddr = 0;
        if (!SafeReadQword(static_cast<std::uintptr_t>(vtbl) + 0x20ull, fnAddr) || !fnAddr)
            return;

        using DispatchFn_t =
            void(__fastcall*)(void* mgr, std::uint32_t actorId, std::uint32_t categoryHash, int arg4, std::uint32_t reactionHash, float delay);

        auto fn = reinterpret_cast<DispatchFn_t>(fnAddr);

        fn(reinterpret_cast<void*>(reactionMgr), actorId, 0x95EA16B0u, 4, reactionHash, 0.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogDebug("[HoldupDownForce] Dispatch exception\n");
    }
}


static std::uint32_t NextRandomBit()
{
    std::uint64_t state = g_RngState.load(std::memory_order_relaxed);
    if (state == 0)
    {
        state = (static_cast<std::uint64_t>(GetTickCount64()) << 1) ^ 0x9E3779B97F4A7C15ull;
    }

    state ^= (state << 13);
    state ^= (state >> 7);
    state ^= (state << 17);

    g_RngState.store(state, std::memory_order_relaxed);
    return static_cast<std::uint32_t>(state & 1ull);
}


static std::uint32_t ChooseRandomHoldupDownHash()
{
    return (NextRandomBit() != 0) ? 0x16CD9714u : 0x16CD9715u;
}


static void __fastcall hkState_EnterDownHoldup(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_EnterDownHoldup, self, actorId, proc);

    if (proc == 1)
    {
        const std::uintptr_t entry = GetHoldupEntry(self, actorId);
        if (entry)
        {
            std::uint8_t flags3F = 0;
            if (SafeReadByte(entry + 0x3Full, flags3F))
            {


                if ((flags3F & 0x2u) != 0)
                {
                    const std::uint32_t reactionHash = ChooseRandomHoldupDownHash();

                    DispatchHoldupDownReaction(self, actorId, reactionHash);
                }
            }
        }
    }

    g_OrigState_EnterDownHoldup(self, actorId, proc);
}


bool Install_State_EnterDownHoldupForceVoice_Hook()
{
    void* target = ResolveGameAddress(gAddr.State_EnterDownHoldup);
    if (!target)
    {
        Log("[HoldupDownForce] target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkState_EnterDownHoldup),
        reinterpret_cast<void**>(&g_OrigState_EnterDownHoldup));

#ifdef _DEBUG
    Log("[HoldupDownForce] Install: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[HoldupDownForce] Install: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_State_EnterDownHoldupForceVoice_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_EnterDownHoldup));
    g_OrigState_EnterDownHoldup = nullptr;

#ifdef _DEBUG
    LogDebug("[HoldupDownForce] Removed\n");
#endif
    return true;
}