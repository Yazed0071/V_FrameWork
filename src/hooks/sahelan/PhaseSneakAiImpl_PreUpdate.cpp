#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "PhaseSneakAiImpl_PreUpdate.h"

namespace
{
    using PreUpdate_t = void(__fastcall*)(void* self,
                                          std::uint32_t slot,
                                          std::int64_t* aiContext);

    using StepFunc_t = void(__fastcall*)(void* self,
                                         std::uint32_t slot,
                                         std::uint32_t stepProc,
                                         void* knowledge,
                                         void* unused);

    static PreUpdate_t g_OrigPreUpdate = nullptr;

    static constexpr std::uint32_t kStepProcEnter = 0;
    static constexpr std::uint32_t kStepProcExit  = 1;
    static constexpr std::size_t   kStepEntrySize = 16;
    static constexpr std::size_t   kStepFuncOffset   = 0;
    static constexpr std::size_t   kStepClassOffset  = 8;
    static constexpr std::int64_t  kBaseAdjustment   = -0x20;

    static bool DispatchStepProc(void* selfPreUpdateArg,
                                 std::uint32_t slot,
                                 std::uint32_t stepProc,
                                 void* knowledge,
                                 std::uint8_t phase,
                                 const char* label)
    {
        auto tableBase = reinterpret_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.Sahelan_PhaseSneakAiImpl_StepFuncsTable));
        if (!tableBase)
        {
            LogDebug("[Sahelan] %s phase=%u: step-funcs table unresolved\n", label, phase);
            return false;
        }

        const std::size_t entryOffset = static_cast<std::size_t>(phase) * kStepEntrySize;
        auto stepFunc = *reinterpret_cast<StepFunc_t*>(tableBase + entryOffset + kStepFuncOffset);
        const std::int32_t classOffset = *reinterpret_cast<std::int32_t*>(
            tableBase + entryOffset + kStepClassOffset);

        if (!stepFunc)
        {
            LogDebug("[Sahelan] %s phase=%u: null step func\n", label, phase);
            return false;
        }

        auto baseThis = reinterpret_cast<std::uint8_t*>(selfPreUpdateArg) + kBaseAdjustment;
        auto stepThis = baseThis + classOffset;

        __try
        {
            stepFunc(stepThis, slot, stepProc, knowledge, nullptr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[Sahelan] %s phase=%u: SEH in step proc=%u\n", label, phase, stepProc);
            return false;
        }
    }

    static void __fastcall hk_PreUpdate(void* self,
                                        std::uint32_t slot,
                                        std::int64_t* aiContext)
    {
        if (g_OrigPreUpdate)
            g_OrigPreUpdate(self, slot, aiContext);

        if (!aiContext || !*aiContext)
            return;

        auto knowledge = reinterpret_cast<std::uint8_t*>(*aiContext);

        __try
        {
            SahelanPhaseForce::g_CurrentPhase.store(
                static_cast<std::int32_t>(knowledge[4]),
                std::memory_order_relaxed);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        const std::int32_t forced =
            SahelanPhaseForce::g_ForcedPhase.load(std::memory_order_relaxed);

        if (forced < 0 || forced > 0xFF)
            return;

        const std::uint8_t forcedByte = static_cast<std::uint8_t>(forced);
        const std::uint8_t currentByte = knowledge[4];

        if (currentByte == forcedByte)
        {
            SahelanPhaseForce::g_ForcedPhase.store(-1, std::memory_order_relaxed);
            return;
        }

        const bool okExit = DispatchStepProc(self, slot, kStepProcExit,
                                             knowledge, currentByte, "Phase Exit");

        __try
        {
            knowledge[4] = forcedByte;
            knowledge[5] = currentByte;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[Sahelan] Phase change: SEH writing knowledge\n");
            SahelanPhaseForce::g_ForcedPhase.store(-1, std::memory_order_relaxed);
            return;
        }

        const bool okEnter = DispatchStepProc(self, slot, kStepProcEnter,
                                              knowledge, forcedByte, "Phase Enter");

        SahelanPhaseForce::g_ForcedPhase.store(-1, std::memory_order_relaxed);

#ifdef _DEBUG
        LogDebug("[Sahelan] Phase: %u -> %u (Exit=%s Enter=%s)\n",
            currentByte, forcedByte,
            okExit  ? "ok" : "fail",
            okEnter ? "ok" : "fail");
#endif
    }
}

bool Install_PhaseSneakAiImpl_PreUpdate_Hook()
{
    void* target = ResolveGameAddress(gAddr.Sahelan_PhaseSneakAiImpl_PreUpdate);
    const bool ok = target && CreateAndEnableHook(target,
        reinterpret_cast<void*>(&hk_PreUpdate),
        reinterpret_cast<void**>(&g_OrigPreUpdate));

#ifdef _DEBUG
    Log("[Hook] PhaseSneakAiImpl::PreUpdate: %s target=%p\n",
        ok ? "OK" : "FAIL", target);
#endif
    return ok;
}

bool Uninstall_PhaseSneakAiImpl_PreUpdate_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.Sahelan_PhaseSneakAiImpl_PreUpdate));
    g_OrigPreUpdate = nullptr;
    SahelanPhaseForce::g_ForcedPhase.store(-1, std::memory_order_relaxed);
    SahelanPhaseForce::g_CurrentPhase.store(-1, std::memory_order_relaxed);
    return true;
}

void Set_SahelanForcePhase(std::int32_t phase)
{
    if (phase < 0 || phase > 0xFF)
    {
        LogDebug("[Sahelan] ForcePhase: ignoring out-of-range phase=%d\n", phase);
        return;
    }
    SahelanPhaseForce::g_ForcedPhase.store(phase, std::memory_order_relaxed);
#ifdef _DEBUG
    LogDebug("[Sahelan] ForcePhase: queued phase=%d (single-shot Exit/Enter on next PreUpdate)\n", phase);
#endif
}

std::int32_t Get_SahelanCurrentPhase()
{
    return SahelanPhaseForce::g_CurrentPhase.load(std::memory_order_relaxed);
}
