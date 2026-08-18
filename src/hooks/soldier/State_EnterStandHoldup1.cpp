#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <random>
#include <unordered_map>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "State_EnterStandHoldup1.h"
#include "AddressSet.h"

namespace
{


    using HoldupState_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, int proc);


    using AddNoise_t =
        void(__fastcall*)(void* self, std::uint32_t actorId);


    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;


    static constexpr std::uint32_t HASH_HOLDUP_REACTION_COWARDLY = 0x16CD9714u;

    static HoldupState_t g_OrigState_EnterStandHoldup1 = nullptr;
    static HoldupState_t g_OrigState_EnterStandHoldupUnarmed = nullptr;
    static AddNoise_t    g_AddNoise = nullptr;


    enum class CowardlyChoice : std::uint8_t
    {
        Unknown = 0,
        Custom  = 1,
        Vanilla = 2,
    };


    static std::unordered_map<std::uint32_t, CowardlyChoice> g_CowardlyChoiceByActor;
    static std::mutex                                        g_CowardlyChoiceMutex;


    static CowardlyChoice GetOrRollCowardlyChoice(std::uint32_t actorId)
    {
        std::lock_guard<std::mutex> lock(g_CowardlyChoiceMutex);
        auto& slot = g_CowardlyChoiceByActor[actorId];
        if (slot == CowardlyChoice::Unknown)
        {
            static thread_local std::mt19937 rng{ std::random_device{}() };
            std::uniform_int_distribution<int> dist(0, 1);
            slot = (dist(rng) == 0) ? CowardlyChoice::Custom : CowardlyChoice::Vanilla;
        }
        return slot;
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
        LogDebug("[HoldUpReactionCowardly] DispatchHoldupReaction exception\n");
    }
}


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
        LogDebug("[HoldUpReactionCowardly] AddNoise exception\n");
    }
}


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

    if (proc != 1)
    {
        origFn(self, actorId, proc);
        return;
    }

    if (GetOrRollCowardlyChoice(actorId) != CowardlyChoice::Custom)
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


    origFn(self, actorId, proc);


    SafeWriteByte(entry + 0x3Full, flags3F);


    DispatchHoldupReaction(self, actorId, HASH_HOLDUP_REACTION_COWARDLY);
    CallHoldupAddNoise(self, actorId);
}


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


bool Install_HoldUpReactionCowardlyReactions_Hook()
{
    g_AddNoise = reinterpret_cast<AddNoise_t>(ResolveGameAddress(gAddr.AddNoise));

    void* targetStandHoldup1 = ResolveGameAddress(gAddr.State_EnterStandHoldup1);
    void* targetStandHoldupUnarmed = ResolveGameAddress(gAddr.State_EnterStandHoldupUnarmed);

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

#ifdef _DEBUG
    Log("[HoldUpReactionCowardly] Install: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[HoldUpReactionCowardly] Install: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_HoldUpReactionCowardlyReactions_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_EnterStandHoldup1));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.State_EnterStandHoldupUnarmed));

    g_OrigState_EnterStandHoldup1 = nullptr;
    g_OrigState_EnterStandHoldupUnarmed = nullptr;
    g_AddNoise = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_CowardlyChoiceMutex);
        g_CowardlyChoiceByActor.clear();
    }

#ifdef _DEBUG
    LogDebug("[HoldUpReactionCowardly] removed\n");
#endif
    return true;
}