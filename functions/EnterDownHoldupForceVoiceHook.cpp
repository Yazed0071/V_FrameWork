#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <atomic>

#include "HookUtils.h"
#include "log.h"

namespace
{
    // Hook target:
    // tpp::gm::soldier::impl::HoldupActionImpl::State_EnterDownHoldup
    // Params:
    //   self    = HoldupActionImpl*
    //   actorId = acting soldier slot/id
    //   proc    = state proc
    using State_EnterDownHoldup_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, std::uint32_t proc);

    // Absolute address of HoldupActionImpl::State_EnterDownHoldup.
    static constexpr std::uintptr_t ABS_State_EnterDownHoldup = 0x14A140940ull;

    static State_EnterDownHoldup_t g_OrigState_EnterDownHoldup = nullptr;

    // Small RNG state for random voice selection.
    static std::atomic<std::uint64_t> g_RngState{ 0 };
}

// Safely reads a qword from memory.
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

// Safely reads a dword from memory.
// Params: addr (uintptr_t), outValue (uint32_t&)
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

// Safely reads a byte from memory.
// Params: addr (uintptr_t), outValue (uint8_t&)
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

// Resolves the per-action entry used by HoldupActionImpl::State_EnterDownHoldup.
// Formula from decomp:
//   entry = ((actorId - *(int*)(self+0x90)) * 0x40) + *(qword*)(self+0x88)
// Params: self (void*), actorId (uint32_t)
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

// Dispatches the holdup-down reaction through the same downstream manager used by the game.
// This matches the original call shape:
//   manager->Dispatch(actorId, 0x95EA16B0, 4, reactionHash, 0)
// Params: self (void*), actorId (uint32_t), reactionHash (uint32_t)
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
        Log("[HoldupDownForce] Dispatch exception\n");
    }
}

// Returns a random bit used to choose between the two unused holdup-down lines.
// Params: none
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

// Chooses one of the two unused holdup-down reaction hashes at random.
// Params: none
static std::uint32_t ChooseRandomHoldupDownHash()
{
    return (NextRandomBit() != 0) ? 0x16CD9714u : 0x16CD9715u;
}

// Hooked EnterDownHoldup.
// If the original code would skip the reaction because entry+0x3F has bit 0x2 set,
// force one of the two unused lines randomly, then continue with the original function.
// Params: self (void*), actorId (uint32_t), proc (uint32_t)
static void __fastcall hkState_EnterDownHoldup(
    void* self,
    std::uint32_t actorId,
    std::uint32_t proc)
{
    if (proc == 1)
    {
        const std::uintptr_t entry = GetHoldupEntry(self, actorId);
        if (entry)
        {
            std::uint8_t flags3F = 0;
            if (SafeReadByte(entry + 0x3Full, flags3F))
            {
                // Original code only dispatches the 0x16CD9714 / 0x16CD9715 reaction when bit 0x2 is NOT set.
                // We do the opposite: if bit 0x2 IS set, force a random one ourselves so the skipped lines can play.
                if ((flags3F & 0x2u) != 0)
                {
                    const std::uint32_t reactionHash = ChooseRandomHoldupDownHash();

                    Log("[HoldupDownForce] actor=%u flags3F=0x%02X forcedHash=0x%08X\n",
                        actorId,
                        static_cast<unsigned>(flags3F),
                        reactionHash);

                    DispatchHoldupDownReaction(self, actorId, reactionHash);
                }
            }
        }
    }

    g_OrigState_EnterDownHoldup(self, actorId, proc);
}

// Installs the EnterDownHoldup forced-random-voice hook.
// Params: none
bool Install_State_EnterDownHoldupForceVoice_Hook()
{
    void* target = ResolveGameAddress(ABS_State_EnterDownHoldup);
    if (!target)
    {
        Log("[HoldupDownForce] target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkState_EnterDownHoldup),
        reinterpret_cast<void**>(&g_OrigState_EnterDownHoldup));

    Log("[HoldupDownForce] Install: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes the EnterDownHoldup forced-random-voice hook.
// Params: none
bool Uninstall_State_EnterDownHoldupForceVoice_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_EnterDownHoldup));
    g_OrigState_EnterDownHoldup = nullptr;

    Log("[HoldupDownForce] Removed\n");
    return true;
}