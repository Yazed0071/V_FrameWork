#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "HookUtils.h"
#include "log.h"
#include "VIPSoundRecoveryHook.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"

namespace
{


    using State_SoundRecovery_t =
        void(__fastcall*)(void* self, std::uint32_t actorId, int proc, void* evt);


    static constexpr std::uint32_t HASH_EVENT_VOICE_NOTICE = 0x1077DB8Du;


    static constexpr std::uint32_t HASH_REACTION_CATEGORY_NOTICE = 0x95EA16B0u;


    static constexpr std::uint32_t HASH_SLEEP_WAKE_OFFICER = 0x9CD0A89Cu;


    static constexpr std::uint32_t HASH_HOLDUP_RECOVERY_VIP = 0x92D098DEu;


    static State_SoundRecovery_t g_OrigState_SoundRecovery = nullptr;
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


static std::uintptr_t GetNoticeActionEntry(void* self, std::uint32_t actorId)
{
    if (!self)
        return 0;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    __try
    {
        const std::uint32_t baseIndex =
            *reinterpret_cast<const std::uint32_t*>(selfAddr + 0x98ull);

        const std::uint64_t tableBase =
            *reinterpret_cast<const std::uint64_t*>(selfAddr + 0x90ull);

        const std::uint32_t slot = actorId - baseIndex;
        return static_cast<std::uintptr_t>(
            tableBase + (static_cast<std::uint64_t>(slot) * 0x68ull));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}


static std::uint32_t GetEventHash(void* evt)
{
    if (!evt)
        return 0;

    __try
    {
        const auto objectAddr = reinterpret_cast<std::uintptr_t>(evt);
        const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(objectAddr);
        if (!vtbl)
            return 0;

        const auto fnAddr = *reinterpret_cast<const std::uintptr_t*>(vtbl + 0x0ull);
        if (!fnAddr)
            return 0;

        using GetHashFn_t = std::uint32_t(__fastcall*)(void*);
        auto fn = reinterpret_cast<GetHashFn_t>(fnAddr);
        return fn(evt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}


static void DispatchNoticeReaction(void* noticeSelf, std::uint32_t actorId, std::uint32_t reactionHash)
{
    if (!noticeSelf)
        return;

    __try
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(noticeSelf);

        const std::uint64_t soldierActionRoot =
            *reinterpret_cast<const std::uint64_t*>(selfAddr + 0x78ull);
        if (!soldierActionRoot)
            return;

        const std::uint64_t reactionMgr =
            *reinterpret_cast<const std::uint64_t*>(
                static_cast<std::uintptr_t>(soldierActionRoot) + 0xA8ull);
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
            1,
            reactionHash,
            1.0f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogDebug("[SoundRecovery] DispatchNoticeReaction exception\n");
    }
}


static void __fastcall hkState_SoundRecovery(
    void* self,
    std::uint32_t actorId,
    int proc,
    void* evt)
{
    MISSION_GUARD_ORIGINAL_VOID(g_OrigState_SoundRecovery, self, actorId, proc, evt);

    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);

        if (eventHash == HASH_EVENT_VOICE_NOTICE)
        {
            const std::uintptr_t entry = GetNoticeActionEntry(self, actorId);

            if (entry)
            {
                std::uint8_t b57 = 0;
                if (SafeReadByte(entry + 0x57ull, b57))
                {
                    const std::uint32_t replacementHash =
                        (b57 == 0x0Au)
                            ? HASH_HOLDUP_RECOVERY_VIP
                            : HASH_SLEEP_WAKE_OFFICER;

                    DispatchNoticeReaction(self, actorId, replacementHash);
                    return;
                }
            }

            Log("[SoundRecovery] actor=%u voice-notice: entry/byte read failed, vanilla fallback\n",
                actorId);
        }
    }

    if (g_OrigState_SoundRecovery)
        g_OrigState_SoundRecovery(self, actorId, proc, evt);
}


bool Install_VIPSoundRecovery_Hook()
{
    void* target = ResolveGameAddress(
        gAddr.State_StandEnterRecoverySleepFaintHoldupComradeBySound);

    if (!target)
    {
        Log("[SoundRecovery] Install failed: address not resolved\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkState_SoundRecovery),
        reinterpret_cast<void**>(&g_OrigState_SoundRecovery));

#ifdef _DEBUG
    Log("[SoundRecovery] Install State_StandEnterRecoverySleepFaintHoldupComradeBySound: %s\n",
        ok ? "OK" : "FAIL");
#endif

    return ok;
}


bool Uninstall_VIPSoundRecovery_Hook()
{
    DisableAndRemoveHook(
        ResolveGameAddress(gAddr.State_StandEnterRecoverySleepFaintHoldupComradeBySound));

    g_OrigState_SoundRecovery = nullptr;

#ifdef _DEBUG
    LogDebug("[SoundRecovery] Hook removed\n");
#endif
    return true;
}
