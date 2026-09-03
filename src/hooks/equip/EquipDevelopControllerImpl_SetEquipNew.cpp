#include "pch.h"
#include "EquipDevelopControllerImpl_SetEquipNew.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

#include "DevelopArrayGrow.h"
#include "EquipDevelop_SetEquipUndeveloped.h"
#include "HookUtils.h"
#include "V_FrameWorkState.h"
#include "log.h"

namespace
{
    constexpr std::size_t kSetEquipNewSlot          = 0x258 / sizeof(void*);
    constexpr std::size_t kSetEquipNewDevelopedSlot = 0x280 / sizeof(void*);
    constexpr std::size_t kEnableDevelopSlot        = 0x1B8 / sizeof(void*);

    using SetEquipNew_t = void(__fastcall*)(void* controller, std::uint16_t index,
                                            std::uint8_t isNew);
    using RowPredicate_t = char(__fastcall*)(void* controller, std::uint16_t index);

    SetEquipNew_t  g_OrigUndeveloped   = nullptr;
    SetEquipNew_t  g_OrigDeveloped     = nullptr;
    RowPredicate_t g_OrigEnableDevelop = nullptr;

    void* g_TargetUndeveloped   = nullptr;
    void* g_TargetDeveloped     = nullptr;
    void* g_TargetEnableDevelop = nullptr;

    std::atomic<bool> g_Installed{ false };

    bool BytesPresent(const std::uint8_t* code, std::size_t span,
                      const std::uint8_t* want, std::size_t wantLen)
    {
        __try
        {
            for (std::size_t i = 0; i + wantLen <= span; ++i)
            {
                std::size_t j = 0;
                while (j < wantLen && code[i + j] == want[j])
                    ++j;
                if (j == wantLen)
                    return true;
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool LooksLikeEnableDevelop(void* target)
    {
        static const std::uint8_t kRecordBit[] =
            { 0x0F, 0xB6, 0x44, 0x08, 0x15, 0xC1, 0xE8, 0x04, 0x83, 0xE0, 0x01 };
        return BytesPresent(static_cast<const std::uint8_t*>(target), 0x20,
                            kRecordBit, sizeof(kRecordBit));
    }

    bool ReadVtableSlot(void* controller, std::size_t slot, void*& out)
    {
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(controller);
            if (!vtbl)
                return false;
            out = vtbl[slot];
            return out != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ResolveManagedRow(std::uint16_t index, std::int32_t& developId)
    {
        if (index < equip::FirstCustomFlowIndex())
            return false;
        std::uint32_t rowDevelopId = 0;
        if (!equip::TryReadRowDevelopId(index, rowDevelopId) || rowDevelopId == 0)
            return false;
        developId = static_cast<std::int32_t>(rowDevelopId);
        return V_FrameWorkState::IsManagedDevelopId(developId);
    }

    void GuardedSet(SetEquipNew_t orig, void* controller, std::uint16_t index,
                    std::uint8_t isNew)
    {
        if (!orig)
            return;

        std::int32_t developId = 0;
        if (!ResolveManagedRow(index, developId))
        {
            orig(controller, index, isNew);
            return;
        }

        if (!isNew)
        {
            orig(controller, index, 0);
            V_FrameWorkState::SetNewByDevelopId(developId, false);
            return;
        }

        orig(controller, index,
             V_FrameWorkState::GetNewByDevelopId(developId) ? 1 : 0);
    }

    void __fastcall hkSetEquipNew(void* controller, std::uint16_t index,
                                  std::uint8_t isNew)
    {
        GuardedSet(g_OrigUndeveloped, controller, index, isNew);
    }

    void __fastcall hkSetEquipNewDeveloped(void* controller, std::uint16_t index,
                                           std::uint8_t isNew)
    {
        GuardedSet(g_OrigDeveloped, controller, index, isNew);
    }

    char __fastcall hkIsEnableEquipDevelop(void* controller, std::uint16_t index)
    {
        const char answer = g_OrigEnableDevelop
                            ? g_OrigEnableDevelop(controller, index) : 0;
        if (answer == 0)
            return answer;

        std::int32_t developId = 0;
        if (!ResolveManagedRow(index, developId))
            return answer;

        return EquipDevelop_IsDevelopedByDevelopId(
                   static_cast<std::uint32_t>(developId)) ? 0 : answer;
    }

    void InstallDevelopAvailableGate(void* controller)
    {
        void* target = nullptr;
        if (!ReadVtableSlot(controller, kEnableDevelopSlot, target)
            || !LooksLikeEnableDevelop(target))
        {
            Log("[EquipDevelop] ERROR: the develop-available gate was not installed: "
                "the controller vtable slot does not hold IsEnableEquipDevelop on "
                "this build, so a developed custom row keeps its NEW ribbon in R&D\n");
            return;
        }
        if (!CreateAndEnableHook(target, &hkIsEnableEquipDevelop,
                                 reinterpret_cast<void**>(&g_OrigEnableDevelop)))
        {
            g_OrigEnableDevelop = nullptr;
            Log("[EquipDevelop] ERROR: the develop-available gate was refused at %p "
                "- a developed custom row keeps its NEW ribbon in R&D\n", target);
            return;
        }
        g_TargetEnableDevelop = target;
    }

    void InstallNewBadgeGuard(void* undeveloped, void* developed)
    {
        const bool a = CreateAndEnableHook(
            undeveloped, &hkSetEquipNew,
            reinterpret_cast<void**>(&g_OrigUndeveloped));
        const bool b = CreateAndEnableHook(
            developed, &hkSetEquipNewDeveloped,
            reinterpret_cast<void**>(&g_OrigDeveloped));

        if (a)
            g_TargetUndeveloped = undeveloped;
        else
            g_OrigUndeveloped = nullptr;
        if (b)
            g_TargetDeveloped = developed;
        else
            g_OrigDeveloped = nullptr;

        if (!a || !b)
            Log("[EquipDevelop] ERROR: the NEW-badge guard was refused (undeveloped "
                "%p=%d developed %p=%d) - the engine re-raises the badge on every "
                "record write and every develop, so seen custom rows badge again\n",
                undeveloped, a ? 1 : 0, developed, b ? 1 : 0);
    }
}

void EquipDevelop_ArmSetEquipNewGuard(void* controller)
{
    if (!controller || g_Installed.load(std::memory_order_relaxed))
        return;

    void* undeveloped = nullptr;
    void* developed   = nullptr;
    if (!ReadVtableSlot(controller, kSetEquipNewSlot, undeveloped)
        || !ReadVtableSlot(controller, kSetEquipNewDevelopedSlot, developed))
        return;

    bool expected = false;
    if (!g_Installed.compare_exchange_strong(expected, true,
                                             std::memory_order_relaxed))
        return;

    InstallDevelopAvailableGate(controller);
    InstallNewBadgeGuard(undeveloped, developed);
}

void EquipDevelop_ReleaseSetEquipNewGuard()
{
    if (!g_Installed.load(std::memory_order_relaxed))
        return;

    if (g_TargetUndeveloped)
        DisableAndRemoveHook(g_TargetUndeveloped);
    if (g_TargetDeveloped)
        DisableAndRemoveHook(g_TargetDeveloped);
    if (g_TargetEnableDevelop)
        DisableAndRemoveHook(g_TargetEnableDevelop);

    g_TargetUndeveloped   = nullptr;
    g_TargetDeveloped     = nullptr;
    g_TargetEnableDevelop = nullptr;

    g_OrigUndeveloped   = nullptr;
    g_OrigDeveloped     = nullptr;
    g_OrigEnableDevelop = nullptr;

    g_Installed.store(false, std::memory_order_relaxed);
}
