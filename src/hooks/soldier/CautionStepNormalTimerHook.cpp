#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"
#include "FoxHashes.h"
#include "CautionStepNormalTimerHook.h"

namespace
{
    using DecrementPhaseCounter_t = void(__fastcall*)(void* self, std::uint32_t phaseIndex, void* knowledge);


    static DecrementPhaseCounter_t g_OrigDecrementPhaseCounter = nullptr;


    static float g_DurationSeconds = 60.0f;


    static float g_NormalizedDrainRate = 0.0f;

    static bool g_EnableOverride = false;
    static bool g_LogEveryAppliedDrain = false;

    static constexpr unsigned kMaxCommandPosts = 256;
    static bool  g_CpEnable[kMaxCommandPosts]          = {};
    static float g_CpDurationSeconds[kMaxCommandPosts] = {};
    static float g_CpNormalizedDrain[kMaxCommandPosts] = {};
    static float g_CpRemainingSeconds[kMaxCommandPosts] = {};

    static float EffectiveNormalizedDrain(std::uint32_t cpIndex)
    {
        if (cpIndex < kMaxCommandPosts && g_CpEnable[cpIndex] && g_CpNormalizedDrain[cpIndex] > 0.0f)
            return g_CpNormalizedDrain[cpIndex];
        if (g_EnableOverride && g_NormalizedDrainRate > 0.0f)
            return g_NormalizedDrainRate;
        return 0.0f;
    }


    static bool g_HaveLastObservedSnapshot = false;
    static std::uint32_t g_LastObservedPhaseIndex = 0xFFFFFFFFu;
    static std::uint8_t g_LastObservedStateId = 0xFFu;
    static float g_LastObservedNormalizedTimer = -1.0f;
    static float g_LastObservedRemainingSeconds = -1.0f;
    static bool g_LastObservedUsedOverride = false;


    static void LogCautionPhaseTimer(const char* fmt, ...)
    {
        char message[1024] = {};

        va_list args;
        va_start(args, fmt);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
        va_end(args);

        SYSTEMTIME st = {};
        GetLocalTime(&st);

        char finalMessage[1200] = {};
        _snprintf_s(
            finalMessage,
            sizeof(finalMessage),
            _TRUNCATE,
            "[%02u:%02u:%02u.%03u] %s",
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond),
            static_cast<unsigned>(st.wMilliseconds),
            message
        );

        Log("%s", finalMessage);
    }

    static float DurationSecondsToNormalizedDrain(float seconds)
    {
        if (seconds <= 0.0f)
            return 1.0f;

        return 1.0f / seconds;
    }

    static void SyncCautionStepNormalDrainFromDuration()
    {
        if (g_DurationSeconds <= 0.0f)
            g_DurationSeconds = 1.0f;

        g_NormalizedDrainRate = DurationSecondsToNormalizedDrain(g_DurationSeconds);
    }

    static std::uintptr_t* GetCautionManager(void* self)
    {
        if (!self)
            return nullptr;

        return *reinterpret_cast<std::uintptr_t**>(
            reinterpret_cast<std::uintptr_t>(self) + 0x70ull
            );
    }

    static float GetCautionDeltaScale(void* self)
    {
        std::uintptr_t* manager = GetCautionManager(self);
        if (!manager)
            return 0.0f;

        return *reinterpret_cast<float*>(
            reinterpret_cast<std::uintptr_t>(manager) + 0x158ull
            );
    }

    static float GetVanillaPhaseRate(void* self, std::uint32_t phaseIndex)
    {
        std::uintptr_t* manager = GetCautionManager(self);
        if (!manager)
            return 0.0f;

        const std::uintptr_t phaseConfigBase = manager[5];
        if (!phaseConfigBase)
            return 0.0f;

        return *reinterpret_cast<float*>(
            phaseConfigBase + 4ull + static_cast<std::uintptr_t>(phaseIndex) * 0x14ull
            );
    }


    static float* GetPhaseTimerPtr(void* self, std::uint32_t phaseIndex)
    {
        std::uintptr_t* manager = GetCautionManager(self);
        if (!manager)
            return nullptr;

        const std::uintptr_t phaseBase = manager[6];
        if (!phaseBase)
            return nullptr;

        const std::uintptr_t phaseOffset = static_cast<std::uintptr_t>(phaseIndex) * 0x60ull;
        return reinterpret_cast<float*>(phaseBase + 0x50ull + phaseOffset);
    }


    static std::uint32_t* GetPhaseFlagsPtr(void* self, std::uint32_t phaseIndex)
    {
        std::uintptr_t* manager = GetCautionManager(self);
        if (!manager)
            return nullptr;

        const std::uintptr_t phaseBase = manager[6];
        if (!phaseBase)
            return nullptr;

        const std::uintptr_t phaseOffset = static_cast<std::uintptr_t>(phaseIndex) * 0x60ull;
        return reinterpret_cast<std::uint32_t*>(phaseBase + 0x54ull + phaseOffset);
    }


    static std::uint8_t GetKnowledgeStateId(void* knowledge)
    {
        if (!knowledge)
            return 0xFFu;

        return *reinterpret_cast<std::uint8_t*>(
            reinterpret_cast<std::uintptr_t>(knowledge) + 6ull
            );
    }


    static std::uint8_t GetKnowledgeFlags(void* knowledge)
    {
        if (!knowledge)
            return 0xFFu;

        return *reinterpret_cast<std::uint8_t*>(
            reinterpret_cast<std::uintptr_t>(knowledge) + 7ull
            );
    }


    static float GetKnowledgeLocalTimer(void* knowledge)
    {
        if (!knowledge)
            return -1.0f;

        return *reinterpret_cast<float*>(knowledge);
    }


    static void UpdateLastObservedSnapshot(void* self, std::uint32_t phaseIndex, void* knowledge, float currentTimer)
    {
        float remainingSeconds = -1.0f;
        const float vanillaPhaseRate = GetVanillaPhaseRate(self, phaseIndex);

        if (currentTimer <= 0.0f)
        {
            remainingSeconds = 0.0f;
        }
        else if (EffectiveNormalizedDrain(phaseIndex) > 0.0f)
        {
            remainingSeconds = currentTimer / EffectiveNormalizedDrain(phaseIndex);
        }
        else if (vanillaPhaseRate > 0.0f)
        {

            remainingSeconds = currentTimer / vanillaPhaseRate;
        }

        g_HaveLastObservedSnapshot = true;
        g_LastObservedPhaseIndex = phaseIndex;
        g_LastObservedStateId = GetKnowledgeStateId(knowledge);
        g_LastObservedNormalizedTimer = currentTimer;
        g_LastObservedRemainingSeconds = remainingSeconds;
        g_LastObservedUsedOverride = g_EnableOverride;

        if (phaseIndex < kMaxCommandPosts)
            g_CpRemainingSeconds[phaseIndex] = remainingSeconds;
    }


    static void ReplaceVanillaDrainIfNeeded(void* self, std::uint32_t phaseIndex, void* knowledge)
    {
        float* const phaseTimer = GetPhaseTimerPtr(self, phaseIndex);
        std::uint32_t* const phaseFlags = GetPhaseFlagsPtr(self, phaseIndex);

        if (!phaseTimer || !knowledge)
        {
            g_OrigDecrementPhaseCounter(self, phaseIndex, knowledge);
            if (phaseTimer)
                UpdateLastObservedSnapshot(self, phaseIndex, knowledge, *phaseTimer);
            return;
        }

        const float beforeTimer = *phaseTimer;
        const std::uint32_t beforeFlags = phaseFlags ? *phaseFlags : 0u;
        const std::uint8_t knowledgeFlagsBefore = GetKnowledgeFlags(knowledge);
        const std::uint8_t stateId = GetKnowledgeStateId(knowledge);
        const float localKnowledgeTimer = GetKnowledgeLocalTimer(knowledge);

        const float deltaScale = GetCautionDeltaScale(self);
        const float vanillaPhaseRate = GetVanillaPhaseRate(self, phaseIndex);
        const float predictedVanillaDrain = vanillaPhaseRate * deltaScale;


        g_OrigDecrementPhaseCounter(self, phaseIndex, knowledge);

        const float vanillaAfterTimer = *phaseTimer;
        const std::uint8_t knowledgeFlagsAfter = GetKnowledgeFlags(knowledge);


        const float vanillaDrain = beforeTimer - vanillaAfterTimer;


        const float overrideDrain = EffectiveNormalizedDrain(phaseIndex);
        if (overrideDrain <= 0.0f)
        {
            UpdateLastObservedSnapshot(self, phaseIndex, knowledge, vanillaAfterTimer);

            if (g_LogEveryAppliedDrain)
            {
#ifdef _DEBUG
                LogCautionPhaseTimer(
                    "[CautionPhaseTimer] phase=%u state=%u flagsBefore=0x%02X flagsAfter=0x%02X timer %.3f -> vanilla %.3f localTimer=%.3f override=OFF delta=%.6f phaseRate=%.6f predictedVanilla=%.6f actualVanilla=%.6f\n",
                    phaseIndex,
                    static_cast<unsigned>(stateId),
                    static_cast<unsigned>(knowledgeFlagsBefore),
                    static_cast<unsigned>(knowledgeFlagsAfter),
                    beforeTimer,
                    vanillaAfterTimer,
                    localKnowledgeTimer,
                    deltaScale,
                    vanillaPhaseRate,
                    predictedVanillaDrain,
                    vanillaDrain
                );
#endif
            }

            return;
        }


        if ((knowledgeFlagsBefore & 0x80u) == 0u)
        {
            UpdateLastObservedSnapshot(self, phaseIndex, knowledge, vanillaAfterTimer);
            return;
        }


        if (beforeTimer <= 0.0f || vanillaDrain <= 0.000001f)
        {
            UpdateLastObservedSnapshot(self, phaseIndex, knowledge, vanillaAfterTimer);
            return;
        }

        if (deltaScale <= 0.0f)
        {
            UpdateLastObservedSnapshot(self, phaseIndex, knowledge, vanillaAfterTimer);

            if (g_LogEveryAppliedDrain)
            {
#ifdef _DEBUG
                LogCautionPhaseTimer(
                    "[CautionPhaseTimer] phase=%u state=%u flagsBefore=0x%02X flagsAfter=0x%02X timer %.3f -> vanilla %.3f localTimer=%.3f seconds=%.3f normalized=%.6f delta=%.6f phaseRate=%.6f predictedVanilla=%.6f actualVanilla=%.6f -> keeping vanilla\n",
                    phaseIndex,
                    static_cast<unsigned>(stateId),
                    static_cast<unsigned>(knowledgeFlagsBefore),
                    static_cast<unsigned>(knowledgeFlagsAfter),
                    beforeTimer,
                    vanillaAfterTimer,
                    localKnowledgeTimer,
                    g_DurationSeconds,
                    g_NormalizedDrainRate,
                    deltaScale,
                    vanillaPhaseRate,
                    predictedVanillaDrain,
                    vanillaDrain
                );
#endif
            }

            return;
        }

        const float customDrain = deltaScale * overrideDrain;

        float customAfterTimer = beforeTimer - customDrain;
        if (customAfterTimer < 0.0f)
            customAfterTimer = 0.0f;


        *phaseTimer = customAfterTimer;


        if (phaseFlags)
        {
            constexpr std::uint32_t kBit22 = (1u << 22);

            if (customAfterTimer < 0.95f)
            {
                *phaseFlags &= ~kBit22;
            }
            else
            {
                if ((beforeFlags & kBit22) != 0u)
                    *phaseFlags |= kBit22;
                else
                    *phaseFlags &= ~kBit22;
            }
        }

        UpdateLastObservedSnapshot(self, phaseIndex, knowledge, customAfterTimer);

        if (g_LogEveryAppliedDrain)
        {
#ifdef _DEBUG
            LogCautionPhaseTimer(
                "[CautionPhaseTimer] phase=%u state=%u flagsBefore=0x%02X flagsAfter=0x%02X timer %.3f -> vanilla %.3f -> custom %.3f localTimer=%.3f seconds=%.3f normalized=%.6f delta=%.6f phaseRate=%.6f predictedVanilla=%.6f actualVanilla=%.6f customDrain=%.6f remaining=%.3f\n",
                phaseIndex,
                static_cast<unsigned>(stateId),
                static_cast<unsigned>(knowledgeFlagsBefore),
                static_cast<unsigned>(knowledgeFlagsAfter),
                beforeTimer,
                vanillaAfterTimer,
                customAfterTimer,
                localKnowledgeTimer,
                g_DurationSeconds,
                g_NormalizedDrainRate,
                deltaScale,
                vanillaPhaseRate,
                predictedVanillaDrain,
                vanillaDrain,
                customDrain,
                g_LastObservedRemainingSeconds
            );
#endif
        }
    }


    static bool  g_PendingCpActive   = false;
    static float g_PendingCpDuration = 0.0f;

    static bool          g_PendingCpCapture = false;
    static std::uint32_t g_CapturedCpIndex  = 0xFFFFFFFFu;

    using TppCp2LuaCommands_t = void(__fastcall*)(unsigned long long, unsigned long long, unsigned int, unsigned int);
    static TppCp2LuaCommands_t   g_OrigTppCp2LuaCommands       = nullptr;
    static std::uint32_t         g_HashSetCautionPhaseDuration = 0;
    static std::uintptr_t Addr_TppCp2LuaCommands()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x140D5DF30ull;
        case GB::En_1_0_15_3:  return 0x140D5E070ull;
        case GB::Jp_1_0_15_4:
        case GB::Jp_1_0_15_4a: return 0x140D5DEF0ull;
        case GB::Jp_1_0_15_3:  return 0x140D5DDE0ull;
        default:               return 0;
        }
    }

    static void __fastcall hkTppCp2LuaCommands(unsigned long long p1, unsigned long long p2,
                                               unsigned int p3, unsigned int cmdHash)
    {
        if (g_OrigTppCp2LuaCommands)
            g_OrigTppCp2LuaCommands(p1, p2, p3, cmdHash);

        if (cmdHash == g_HashSetCautionPhaseDuration && g_PendingCpActive)
        {
            g_PendingCpActive = false;
            Set_CautionStepNormalDurationSecondsForCp(p3, g_PendingCpDuration);
        }

        if (g_PendingCpCapture)
        {
            g_PendingCpCapture = false;
            g_CapturedCpIndex = p3;
        }
    }

    static void __fastcall hkDecrementPhaseCounter(void* self, std::uint32_t phaseIndex, void* knowledge)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigDecrementPhaseCounter, self, phaseIndex, knowledge);
        ReplaceVanillaDrainIfNeeded(self, phaseIndex, knowledge);
    }
}


bool Install_CautionStepNormalTimerHook()
{
    SyncCautionStepNormalDrainFromDuration();

    for (unsigned i = 0; i < kMaxCommandPosts; ++i)
        g_CpRemainingSeconds[i] = -1.0f;

    void* target = ResolveGameAddress(gAddr.DecrementPhaseCounter);
    if (!target)
    {
        LogCautionPhaseTimer("[Hook] CautionPhaseTimer: target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkDecrementPhaseCounter),
        reinterpret_cast<void**>(&g_OrigDecrementPhaseCounter)
    );

#ifdef _DEBUG
    LogCautionPhaseTimer(
        "[Hook] CautionPhaseTimer: %s (target=%p, seconds=%.3f, normalized=%.6f, override=%s)\n",
        ok ? "OK" : "FAIL",
        target,
        g_DurationSeconds,
        g_NormalizedDrainRate,
        g_EnableOverride ? "ON" : "OFF"
    );
#endif

    g_HashSetCautionPhaseDuration = FoxHashes::StrCode32("SetCautionPhaseDuration");
    if (Addr_TppCp2LuaCommands() != 0)
    {
        void* cp2 = ResolveGameAddress(Addr_TppCp2LuaCommands());
        const bool ok2 = cp2 && CreateAndEnableHook(
            cp2,
            reinterpret_cast<void*>(&hkTppCp2LuaCommands),
            reinterpret_cast<void**>(&g_OrigTppCp2LuaCommands));
        if (!ok2)
        {
            Log("[CautionPhaseTimer] ERROR: TppCp2LuaCommands hook failed @ %p - per-CP caution "
                "duration and the AssignInterrogationWithVoice CP capture will not work.\n", cp2);
        }
#ifdef _DEBUG
        LogCautionPhaseTimer("[Hook] TppCp2LuaCommands (per-cp caution): %s @ %p\n", ok2 ? "OK" : "FAIL", cp2);
#endif
    }
    else
    {
        Log("[CautionPhaseTimer] ERROR: TppCp2LuaCommands address is missing for this build - "
            "per-CP caution duration and the AssignInterrogationWithVoice CP capture are disabled.\n");
    }

    return ok;
}


bool Uninstall_CautionStepNormalTimerHook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.DecrementPhaseCounter));
    g_OrigDecrementPhaseCounter = nullptr;
    if (g_OrigTppCp2LuaCommands)
    {
        DisableAndRemoveHook(ResolveGameAddress(Addr_TppCp2LuaCommands()));
        g_OrigTppCp2LuaCommands = nullptr;
    }

#ifdef _DEBUG
    LogCautionPhaseTimer("[Hook] CautionPhaseTimer: removed\n");
#endif
    return true;
}


void Set_CautionStepNormalDurationSeconds(float seconds)
{
    g_DurationSeconds = seconds;
    SyncCautionStepNormalDrainFromDuration();
    g_EnableOverride = true;

#ifdef _DEBUG
    LogCautionPhaseTimer(
        "[CautionPhaseTimer] duration set to %.3f seconds -> normalized drain %.6f (override enabled)\n",
        g_DurationSeconds,
        g_NormalizedDrainRate
    );
#endif
}


float Get_CautionStepNormalDurationSeconds()
{
    return g_DurationSeconds;
}


void Unset_CautionStepNormalDurationSeconds()
{
    g_EnableOverride = false;

#ifdef _DEBUG
    LogCautionPhaseTimer(
        "[CautionPhaseTimer] custom duration override disabled -> vanilla behavior restored\n"
    );
#endif
}


void Set_CautionStepNormalDurationSecondsForCp(std::uint32_t cpIndex, float seconds)
{
    if (cpIndex >= kMaxCommandPosts)
        return;
    if (seconds <= 0.0f)
        seconds = 1.0f;
    g_CpDurationSeconds[cpIndex] = seconds;
    g_CpNormalizedDrain[cpIndex] = 1.0f / seconds;
    g_CpEnable[cpIndex] = true;

#ifdef _DEBUG
    LogCautionPhaseTimer(
        "[CautionPhaseTimer] cp=%u duration set to %.3f seconds -> normalized drain %.6f (override enabled)\n",
        cpIndex, seconds, g_CpNormalizedDrain[cpIndex]
    );
#endif
}


void Unset_CautionStepNormalDurationSecondsForCp(std::uint32_t cpIndex)
{
    if (cpIndex < kMaxCommandPosts)
        g_CpEnable[cpIndex] = false;
}


void Clear_AllCautionStepNormalDurationOverrides()
{
    g_EnableOverride = false;
    for (unsigned i = 0; i < kMaxCommandPosts; ++i)
        g_CpEnable[i] = false;
}


void Set_PendingCautionDurationForCp(float seconds)
{
    g_PendingCpDuration = seconds;
    g_PendingCpActive = true;
}


float Get_CautionStepNormalRemainingSeconds()
{
    if (!g_HaveLastObservedSnapshot)
        return -1.0f;

    return g_LastObservedRemainingSeconds;
}


void Arm_CautionCpCapture()
{
    g_CapturedCpIndex = 0xFFFFFFFFu;
    g_PendingCpCapture = true;
}


std::uint32_t Take_CautionCpIndex()
{
    g_PendingCpCapture = false;
    return g_CapturedCpIndex;
}


float Get_CautionStepNormalDurationSecondsForCp(std::uint32_t cpIndex)
{
    if (cpIndex < kMaxCommandPosts && g_CpEnable[cpIndex])
        return g_CpDurationSeconds[cpIndex];
    return g_DurationSeconds;
}


float Get_CautionStepNormalRemainingSecondsForCp(std::uint32_t cpIndex)
{
    if (cpIndex < kMaxCommandPosts)
        return g_CpRemainingSeconds[cpIndex];
    return g_LastObservedRemainingSeconds;
}