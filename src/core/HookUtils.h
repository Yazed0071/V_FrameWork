#pragma once

#include <Windows.h>
#include <cstdint>
#include "MinHook.h"


inline uintptr_t GetExeBase()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

constexpr uintptr_t EXE_PREFERRED_BASE = 0x140000000ull;


inline constexpr uintptr_t ToRva(uintptr_t absAddr)
{
    return absAddr - EXE_PREFERRED_BASE;
}


inline void* ResolveGameAddress(uintptr_t absAddr)
{
    if (absAddr == 0)
        return nullptr;

    const uintptr_t base = GetExeBase();
    if (!base)
        return nullptr;

    return reinterpret_cast<void*>(base + ToRva(absAddr));
}


extern bool g_HookBatchMode;

inline MH_STATUS EnableOrQueueHook(void* target)
{
    return g_HookBatchMode ? MH_QueueEnableHook(target) : MH_EnableHook(target);
}

inline bool CreateAndEnableHook(void* target, void* detour, void** original)
{
    if (!target || !detour || !original)
        return false;

    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
        return false;
    if (st == MH_ERROR_ALREADY_CREATED && !*original)
        return false;

    st = EnableOrQueueHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED)
        return false;

    return true;
}


inline bool DisableAndRemoveHook(void* target)
{
    if (!target)
        return false;

    MH_DisableHook(target);
    MH_RemoveHook(target);
    return true;
}