#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>

#include "HookUtils.h"
#include "log.h"

namespace
{
    using DecrementPhaseCounter_t = void(__fastcall*)(void* self, std::uint32_t phaseIndex, void* knowledge);

    // Absolute address of tpp::gm::impl::cp::CautionAiImpl::DecrementPhaseCounter.
    // Params: self (void*), phaseIndex (uint32_t), knowledge (void*)
    static constexpr std::uintptr_t ABS_DecrementPhaseCounter = 0x140D6EAA0ull;

    static DecrementPhaseCounter_t g_OrigDecrementPhaseCounter = nullptr;

    // Edit only this startup value.
    // Examples:
    //  4.0f   = about 4 seconds
    // 10.0f   = about 10 seconds
    // 40.0f   = about 40 seconds
    // 60.0f   = about 60 seconds
    // 90.0f   = about 90 seconds
    // 100.0f  = about 100 seconds
    static float g_DurationSeconds = 60.0f;

    // Derived normalized drain rate.
    // Effective custom drain per update = deltaScale * g_NormalizedDrainRate
    static float g_NormalizedDrainRate = 0.0f;

    static bool g_EnableOverride = true;
    static bool g_LogEveryAppliedDrain = true;

    // Last observed snapshot from the hook.
    static bool g_HaveLastObservedSnapshot = false;
    static std::uint32_t g_LastObservedPhaseIndex = 0xFFFFFFFFu;
    static std::uint8_t g_LastObservedStateId = 0xFFu;
    static float g_LastObservedNormalizedTimer = -1.0f;
    static float g_LastObservedRemainingSeconds = -1.0f;
    static bool g_LastObservedUsedOverride = false;

    // Writes a timestamped line into the existing log sink.
    // Params: fmt (const char*), ...
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

    // Converts desired seconds into normalized drain.
    // Params: seconds (float)
    static float DurationSecondsToNormalizedDrain(float seconds)
    {
        if (seconds <= 0.0f)
            return 1.0f;

        return 1.0f / seconds;
    }

    // Syncs the normalized drain rate from the configured duration in seconds.
    // Params: none
    static void SyncCautionStepNormalDrainFromDuration()
    {
        if (g_DurationSeconds <= 0.0f)
            g_DurationSeconds = 1.0f;

        g_NormalizedDrainRate = DurationSecondsToNormalizedDrain(g_DurationSeconds);
    }

    // Returns the caution manager pointer from CautionAiImpl.
    // Params: self (void*)
    static std::uintptr_t* GetCautionManager(void* self)
    {
        if (!self)
            return nullptr;

        return *reinterpret_cast<std::uintptr_t**>(
            reinterpret_cast<std::uintptr_t>(self) + 0x70ull
            );
    }

    // Returns the caution delta/time-scale value used by the game.
    // Params: self (void*)
    static float GetCautionDeltaScale(void* self)
    {
        std::uintptr_t* manager = GetCautionManager(self);
        if (!manager)
            return 0.0f;

        return *reinterpret_cast<float*>(
            reinterpret_cast<std::uintptr_t>(manager) + 0x158ull
            );
    }

    // Returns the vanilla phase rate from the phase config table.
    // Params: self (void*), phaseIndex (uint32_t)
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

    // Returns a pointer to the main phase timer at phaseBase + 0x50 + phaseIndex * 0x60.
    // Params: self (void*), phaseIndex (uint32_t)
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

    // Returns a pointer to the main phase flags at phaseBase + 0x54 + phaseIndex * 0x60.
    // Params: self (void*), phaseIndex (uint32_t)
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

    // Returns the local knowledge state id at knowledge + 6.
    // Params: knowledge (void*)
    static std::uint8_t GetKnowledgeStateId(void* knowledge)
    {
        if (!knowledge)
            return 0xFFu;

        return *reinterpret_cast<std::uint8_t*>(
            reinterpret_cast<std::uintptr_t>(knowledge) + 6ull
            );
    }

    // Returns the local knowledge flags byte at knowledge + 7.
    // Params: knowledge (void*)
    static std::uint8_t GetKnowledgeFlags(void* knowledge)
    {
        if (!knowledge)
            return 0xFFu;

        return *reinterpret_cast<std::uint8_t*>(
            reinterpret_cast<std::uintptr_t>(knowledge) + 7ull
            );
    }

    // Returns the local knowledge timer at knowledge + 0.
    // Params: knowledge (void*)
    static float GetKnowledgeLocalTimer(void* knowledge)
    {
        if (!knowledge)
            return -1.0f;

        return *reinterpret_cast<float*>(knowledge);
    }

    // Updates the last observed snapshot used by the public getter.
    // Params: self (void*), phaseIndex (uint32_t), knowledge (void*), currentTimer (float)
    static void UpdateLastObservedSnapshot(void* self, std::uint32_t phaseIndex, void* knowledge, float currentTimer)
    {
        float remainingSeconds = -1.0f;
        const float vanillaPhaseRate = GetVanillaPhaseRate(self, phaseIndex);

        if (currentTimer <= 0.0f)
        {
            remainingSeconds = 0.0f;
        }
        else if (g_EnableOverride && g_NormalizedDrainRate > 0.0f)
        {
            // Under the override, normalized time-to-zero is simply timer / normalizedDrainRate.
            remainingSeconds = currentTimer / g_NormalizedDrainRate;
        }
        else if (vanillaPhaseRate > 0.0f)
        {
            // Approximate vanilla remaining seconds.
            remainingSeconds = currentTimer / vanillaPhaseRate;
        }

        g_HaveLastObservedSnapshot = true;
        g_LastObservedPhaseIndex = phaseIndex;
        g_LastObservedStateId = GetKnowledgeStateId(knowledge);
        g_LastObservedNormalizedTimer = currentTimer;
        g_LastObservedRemainingSeconds = remainingSeconds;
        g_LastObservedUsedOverride = g_EnableOverride;
    }

    // Replaces the vanilla subtraction result with a custom seconds-based subtraction,
    // but only when vanilla actually took the normal decrement path.
    // Params: self (void*), phaseIndex (uint32_t), knowledge (void*)
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

        // Let vanilla run first so all branch logic still happens.
        g_OrigDecrementPhaseCounter(self, phaseIndex, knowledge);

        const float vanillaAfterTimer = *phaseTimer;
        const std::uint8_t knowledgeFlagsAfter = GetKnowledgeFlags(knowledge);

        // This is the actual drain vanilla applied this call.
        const float vanillaDrain = beforeTimer - vanillaAfterTimer;

        // If override is off, preserve vanilla and just snapshot/log.
        if (!g_EnableOverride || g_NormalizedDrainRate <= 0.0f)
        {
            UpdateLastObservedSnapshot(self, phaseIndex, knowledge, vanillaAfterTimer);

            if (g_LogEveryAppliedDrain)
            {
                //LogCautionPhaseTimer(
                //    "[CautionPhaseTimer] phase=%u state=%u flagsBefore=0x%02X flagsAfter=0x%02X timer %.3f -> vanilla %.3f localTimer=%.3f override=OFF delta=%.6f phaseRate=%.6f predictedVanilla=%.6f actualVanilla=%.6f\n",
                //    phaseIndex,
                //    static_cast<unsigned>(stateId),
                //    static_cast<unsigned>(knowledgeFlagsBefore),
                //    static_cast<unsigned>(knowledgeFlagsAfter),
                //    beforeTimer,
                //    vanillaAfterTimer,
                //    localKnowledgeTimer,
                //    deltaScale,
                //    vanillaPhaseRate,
                //    predictedVanillaDrain,
                //    vanillaDrain
                //);
            }

            return;
        }

        // If bit 0x80 was not set, keep vanilla behavior.
        if ((knowledgeFlagsBefore & 0x80u) == 0u)
        {
            UpdateLastObservedSnapshot(self, phaseIndex, knowledge, vanillaAfterTimer);
            return;
        }

        // If vanilla did not actually subtract, preserve skip/defer behavior.
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
                //LogCautionPhaseTimer(
                //    "[CautionPhaseTimer] phase=%u state=%u flagsBefore=0x%02X flagsAfter=0x%02X timer %.3f -> vanilla %.3f localTimer=%.3f seconds=%.3f normalized=%.6f delta=%.6f phaseRate=%.6f predictedVanilla=%.6f actualVanilla=%.6f -> keeping vanilla\n",
                //    phaseIndex,
                //    static_cast<unsigned>(stateId),
                //    static_cast<unsigned>(knowledgeFlagsBefore),
                //    static_cast<unsigned>(knowledgeFlagsAfter),
                //    beforeTimer,
                //    vanillaAfterTimer,
                //    localKnowledgeTimer,
                //    g_DurationSeconds,
                //    g_NormalizedDrainRate,
                //    deltaScale,
                //    vanillaPhaseRate,
                //    predictedVanillaDrain,
                //    vanillaDrain
                //);
            }

            return;
        }

        const float customDrain = deltaScale * g_NormalizedDrainRate;

        float customAfterTimer = beforeTimer - customDrain;
        if (customAfterTimer < 0.0f)
            customAfterTimer = 0.0f;

        // Replace the timer result with the custom one.
        *phaseTimer = customAfterTimer;

        // Re-apply bit 22 threshold behavior based on the custom timer.
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

        //if (g_LogEveryAppliedDrain)
        //{
        //    LogCautionPhaseTimer(
        //        "[CautionPhaseTimer] phase=%u state=%u flagsBefore=0x%02X flagsAfter=0x%02X timer %.3f -> vanilla %.3f -> custom %.3f localTimer=%.3f seconds=%.3f normalized=%.6f delta=%.6f phaseRate=%.6f predictedVanilla=%.6f actualVanilla=%.6f customDrain=%.6f remaining=%.3f\n",
        //        phaseIndex,
        //        static_cast<unsigned>(stateId),
        //        static_cast<unsigned>(knowledgeFlagsBefore),
        //        static_cast<unsigned>(knowledgeFlagsAfter),
        //        beforeTimer,
        //        vanillaAfterTimer,
        //        customAfterTimer,
        //        localKnowledgeTimer,
        //        g_DurationSeconds,
        //        g_NormalizedDrainRate,
        //        deltaScale,
        //        vanillaPhaseRate,
        //        predictedVanillaDrain,
        //        vanillaDrain,
        //        customDrain,
        //        g_LastObservedRemainingSeconds
        //    );
        //}
    }

    // Hooked DecrementPhaseCounter.
    // Params: self (void*), phaseIndex (uint32_t), knowledge (void*)
    static void __fastcall hkDecrementPhaseCounter(void* self, std::uint32_t phaseIndex, void* knowledge)
    {
        ReplaceVanillaDrainIfNeeded(self, phaseIndex, knowledge);
    }
}

// Installs the caution phase timer hook.
// Params: none
bool Install_CautionStepNormalTimerHook()
{
    SyncCautionStepNormalDrainFromDuration();

    void* target = ResolveGameAddress(ABS_DecrementPhaseCounter);
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

    LogCautionPhaseTimer(
        "[Hook] CautionPhaseTimer: %s (target=%p, seconds=%.3f, normalized=%.6f, override=%s)\n",
        ok ? "OK" : "FAIL",
        target,
        g_DurationSeconds,
        g_NormalizedDrainRate,
        g_EnableOverride ? "ON" : "OFF"
    );

    return ok;
}

// Removes the caution phase timer hook.
// Params: none
bool Uninstall_CautionStepNormalTimerHook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_DecrementPhaseCounter));
    g_OrigDecrementPhaseCounter = nullptr;

    LogCautionPhaseTimer("[Hook] CautionPhaseTimer: removed\n");
    return true;
}

// Sets the desired caution phase duration in seconds.
// Params: seconds (float)
void Set_CautionStepNormalDurationSeconds(float seconds)
{
    g_DurationSeconds = seconds;
    SyncCautionStepNormalDrainFromDuration();
    g_EnableOverride = true;

    LogCautionPhaseTimer(
        "[CautionPhaseTimer] duration set to %.3f seconds -> normalized drain %.6f (override enabled)\n",
        g_DurationSeconds,
        g_NormalizedDrainRate
    );
}

// Returns the configured caution phase duration in seconds.
// Params: none
float Get_CautionStepNormalDurationSeconds()
{
    return g_DurationSeconds;
}

// Disables the custom caution duration override and restores vanilla behavior.
// Params: none
void Unset_CautionStepNormalDurationSeconds()
{
    g_EnableOverride = false;

    LogCautionPhaseTimer(
        "[CautionPhaseTimer] custom duration override disabled -> vanilla behavior restored\n"
    );
}

// Returns the last observed remaining time before the current hooked phase timer reaches zero.
// This is based on the last phase processed by the hook.
// Params: none
float Get_CautionStepNormalRemainingSeconds()
{
    if (!g_HaveLastObservedSnapshot)
    {
        LogCautionPhaseTimer(
            "[CautionPhaseTimer] GetRemainingSeconds -> no snapshot available yet\n"
        );
        return -1.0f;
    }

    LogCautionPhaseTimer(
        "[CautionPhaseTimer] GetRemainingSeconds -> phase=%u state=%u timer=%.3f remainingSeconds=%.3f mode=%s\n",
        g_LastObservedPhaseIndex,
        static_cast<unsigned>(g_LastObservedStateId),
        g_LastObservedNormalizedTimer,
        g_LastObservedRemainingSeconds,
        g_LastObservedUsedOverride ? "custom" : "vanilla"
    );

    return g_LastObservedRemainingSeconds;
}