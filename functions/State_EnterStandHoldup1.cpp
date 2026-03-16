#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "State_EnterStandHoldup1.h"

namespace
{
    // Shared signature for both HoldupActionImpl state functions.
    // Params: self, actorId, proc
    using HoldupState_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, int proc);

    // HoldupActionImpl::AddNoise.
    // Params: self, actorId
    using AddNoise_t =
        void(__fastcall*)(void* self, std::uint32_t actorId);

    // Absolute address of HoldupActionImpl::State_EnterStandHoldup1.
    static constexpr std::uintptr_t ABS_State_EnterStandHoldup1 = 0x14A140C00ull;

    // Absolute address of HoldupActionImpl::State_EnterStandHoldupUnarmed.
    static constexpr std::uintptr_t ABS_State_EnterStandHoldupUnarmed = 0x14A141500ull;

    // Absolute address of HoldupActionImpl::AddNoise.
    static constexpr std::uintptr_t ABS_AddNoise = 0x14147F240ull;

    // Reaction category used by the game's notice reaction manager.
    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;

    // Your custom cowardly reaction hash.
    static constexpr std::uint32_t HASH_HOLDUP_REACTION_COWARDLY = 0x16CD9714u;

    static HoldupState_t g_OrigState_EnterStandHoldup1 = nullptr;
    static HoldupState_t g_OrigState_EnterStandHoldupUnarmed = nullptr;
    static AddNoise_t    g_AddNoise = nullptr;

    // Global toggle controlled from Lua.
    static bool g_UseHoldUpReactionCowardlyReactions = false;
}

// Safely reads one byte from memory.
// Params: addr, outValue
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

// Safely writes one byte to memory.
// Params: addr, value
static bool SafeWriteByte(std::uintptr_t addr, std::uint8_t value)
{
    if (!addr)
        return false;

    __try
    {
        *reinterpret_cast<std::uint8_t*>(addr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Resolves one HoldupAction entry using the game's table math.
// Params: self, actorId
static std::uintptr_t GetHoldupEntry(void* self, std::uint32_t actorId)
{
    if (!self)
        return 0;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    __try
    {
        const std::uint32_t baseIndex =
            *reinterpret_cast<const std::uint32_t*>(selfAddr + 0x90ull);

        const std::uint64_t tableBase =
            *reinterpret_cast<const std::uint64_t*>(selfAddr + 0x88ull);

        const std::uint32_t slot = actorId - baseIndex;
        return static_cast<std::uintptr_t>(
            tableBase + (static_cast<std::uint64_t>(slot) * 0x40ull));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Dispatches one custom notice reaction through the reaction manager used by HoldupActionImpl.
// Matches the original call shape at vfunc +0x20 with arg4 = 4 and delay = 0.0f.
// Params: holdupSelf, actorId, reactionHash
static void DispatchHoldupReaction(void* holdupSelf, std::uint32_t actorId, std::uint32_t reactionHash)
{
    if (!holdupSelf)
        return;

    __try
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(holdupSelf);

        const std::uint64_t root70 =
            *reinterpret_cast<const std::uint64_t*>(selfAddr + 0x70ull);
        if (!root70)
            return;

        const std::uint64_t reactionMgr =
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(root70) + 0xA8ull);
        if (!reactionMgr)
            return;

        const std::uint64_t vtbl =
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(reactionMgr));
        if (!vtbl)
            return;

        const std::uint64_t fnAddr =
            *reinterpret_cast<const std::uint64_t*>(static_cast<std::uintptr_t>(vtbl) + 0x20ull);
        if (!fnAddr)
            return;

        using DispatchFn_t =
            void(__fastcall*)(void* mgr,
                std::uint32_t actorId,
                std::uint32_t categoryHash,
                int arg4,
                std::uint32_t reactionHash,
                float delaySeconds);

        auto fn = reinterpret_cast<DispatchFn_t>(fnAddr);
        fn(
            reinterpret_cast<void*>(reactionMgr),
            actorId,
            HASH_REACTION_CATEGORY_NOTICE,
            4,
            reactionHash,
            0.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[HoldUpReactionCowardly] DispatchHoldupReaction exception\n");
    }
}

// Calls HoldupActionImpl::AddNoise if resolved.
// Params: self, actorId
static void CallHoldupAddNoise(void* self, std::uint32_t actorId)
{
    if (!g_AddNoise || !self)
        return;

    __try
    {
        g_AddNoise(self, actorId);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[HoldUpReactionCowardly] AddNoise exception\n");
    }
}

// Shared replacement logic for both stand-holdup states.
// If enabled, suppresses the vanilla concerned reaction by temporarily setting bit 0x2,
// then dispatches the cowardly reaction instead.
// Params: stateTag, origFn, self, actorId, proc
static void RunCowardlyReactionOverride(
    const char* stateTag,
    HoldupState_t origFn,
    void* self,
    std::uint32_t actorId,
    int proc)
{
    if (MissionCodeGuard::ShouldBypassHooks())
    {
        if (origFn)
            origFn(self, actorId, proc);
        return;
    }

    if (!origFn)
        return;

    if (!g_UseHoldUpReactionCowardlyReactions || proc != 1)
    {
        origFn(self, actorId, proc);
        return;
    }

    const std::uintptr_t entry = GetHoldupEntry(self, actorId);
    if (!entry)
    {
        origFn(self, actorId, proc);
        return;
    }

    std::uint8_t flags3F = 0;
    if (!SafeReadByte(entry + 0x3Full, flags3F))
    {
        origFn(self, actorId, proc);
        return;
    }

    // Vanilla only dispatches the concerned reaction when bit 0x2 is NOT set.
    // If it is already set, keep vanilla behavior.
    if ((flags3F & 0x2u) != 0u)
    {
        origFn(self, actorId, proc);
        return;
    }

    const std::uint8_t patchedFlags = static_cast<std::uint8_t>(flags3F | 0x2u);
    if (!SafeWriteByte(entry + 0x3Full, patchedFlags))
    {
        origFn(self, actorId, proc);
        return;
    }

    // Let vanilla do everything except the concerned reaction branch.
    origFn(self, actorId, proc);

    // Restore original flags.
    SafeWriteByte(entry + 0x3Full, flags3F);

    // Dispatch your custom reaction in place of the vanilla one.
    Log("[HoldUpReactionCowardly][%s] actor=%u flags3F=0x%02X customHash=0x%08X\n",
        stateTag,
        actorId,
        static_cast<unsigned>(flags3F),
        static_cast<unsigned>(HASH_HOLDUP_REACTION_COWARDLY));

    DispatchHoldupReaction(self, actorId, HASH_HOLDUP_REACTION_COWARDLY);
    CallHoldupAddNoise(self, actorId);
}

// Enables or disables the cowardly holdup reaction replacement.
// Params: enabled
void Set_HoldUpReactionCowardlyReactions(bool enabled)
{
    g_UseHoldUpReactionCowardlyReactions = enabled;

    Log("[HoldUpReactionCowardly] %s (hash=0x%08X)\n",
        enabled ? "ON" : "OFF",
        static_cast<unsigned>(HASH_HOLDUP_REACTION_COWARDLY));
}

// Returns whether the cowardly holdup reaction replacement is enabled.
// Params: none
bool Get_HoldUpReactionCowardlyReactions()
{
    return g_UseHoldUpReactionCowardlyReactions;
}

// Hooked version of HoldupActionImpl::State_EnterStandHoldup1.
// Params: self, actorId, proc
static void __fastcall hkState_EnterStandHoldup1(
    void* self,
    std::uint32_t actorId,
    int proc)
{
    RunCowardlyReactionOverride(
        "StandHoldup1",
        g_OrigState_EnterStandHoldup1,
        self,
        actorId,
        proc);
}

// Hooked version of HoldupActionImpl::State_EnterStandHoldupUnarmed.
// Params: self, actorId, proc
static void __fastcall hkState_EnterStandHoldupUnarmed(
    void* self,
    std::uint32_t actorId,
    int proc)
{
    RunCowardlyReactionOverride(
        "StandHoldupUnarmed",
        g_OrigState_EnterStandHoldupUnarmed,
        self,
        actorId,
        proc);
}

// Installs both cowardly-reaction hooks.
// Params: none
bool Install_HoldUpReactionCowardlyReactions_Hook()
{
    g_AddNoise = reinterpret_cast<AddNoise_t>(ResolveGameAddress(ABS_AddNoise));

    void* targetStandHoldup1 = ResolveGameAddress(ABS_State_EnterStandHoldup1);
    void* targetStandHoldupUnarmed = ResolveGameAddress(ABS_State_EnterStandHoldupUnarmed);

    if (!targetStandHoldup1 || !targetStandHoldupUnarmed)
    {
        Log("[HoldUpReactionCowardly] Install: target resolve failed\n");
        return false;
    }

    const bool okA = CreateAndEnableHook(
        targetStandHoldup1,
        reinterpret_cast<void*>(&hkState_EnterStandHoldup1),
        reinterpret_cast<void**>(&g_OrigState_EnterStandHoldup1));

    const bool okB = CreateAndEnableHook(
        targetStandHoldupUnarmed,
        reinterpret_cast<void*>(&hkState_EnterStandHoldupUnarmed),
        reinterpret_cast<void**>(&g_OrigState_EnterStandHoldupUnarmed));

    const bool ok = okA && okB;

    Log("[HoldUpReactionCowardly] Install: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes both cowardly-reaction hooks.
// Params: none
bool Uninstall_HoldUpReactionCowardlyReactions_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_EnterStandHoldup1));
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_EnterStandHoldupUnarmed));

    g_OrigState_EnterStandHoldup1 = nullptr;
    g_OrigState_EnterStandHoldupUnarmed = nullptr;
    g_AddNoise = nullptr;

    Log("[HoldUpReactionCowardly] removed\n");
    return true;
}