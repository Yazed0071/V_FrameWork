#pragma once

#include <Windows.h>
#include <cstdint>
#include "MinHook.h"
#include "log.h"
#include "HookArena.h"


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

inline const char* MhStatusName(MH_STATUS st)
{
    switch (st)
    {
        case MH_UNKNOWN:                    return "MH_UNKNOWN";
        case MH_OK:                         return "MH_OK";
        case MH_ERROR_ALREADY_INITIALIZED:  return "MH_ERROR_ALREADY_INITIALIZED";
        case MH_ERROR_NOT_INITIALIZED:      return "MH_ERROR_NOT_INITIALIZED";
        case MH_ERROR_ALREADY_CREATED:      return "MH_ERROR_ALREADY_CREATED";
        case MH_ERROR_NOT_CREATED:          return "MH_ERROR_NOT_CREATED";
        case MH_ERROR_ENABLED:              return "MH_ERROR_ENABLED";
        case MH_ERROR_DISABLED:             return "MH_ERROR_DISABLED";
        case MH_ERROR_NOT_EXECUTABLE:       return "MH_ERROR_NOT_EXECUTABLE";
        case MH_ERROR_UNSUPPORTED_FUNCTION: return "MH_ERROR_UNSUPPORTED_FUNCTION";
        case MH_ERROR_MEMORY_ALLOC:         return "MH_ERROR_MEMORY_ALLOC";
        case MH_ERROR_MEMORY_PROTECT:       return "MH_ERROR_MEMORY_PROTECT";
        case MH_ERROR_MODULE_NOT_FOUND:     return "MH_ERROR_MODULE_NOT_FOUND";
        case MH_ERROR_FUNCTION_NOT_FOUND:   return "MH_ERROR_FUNCTION_NOT_FOUND";
        default:                            return "MH_ERROR_?";
    }
}


inline bool CreateAndEnableHook(void* target, void* detour, void** original)
{
    if (!target || !detour || !original)
        return false;

    MH_STATUS st = MH_CreateHook(target, detour, original);
    while (st == MH_ERROR_MEMORY_ALLOC && HookArena::ReleaseOne())
        st = MH_CreateHook(target, detour, original);

    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
    {
        const DWORD lastErr = GetLastError();
        const uintptr_t base = GetExeBase();
        const uintptr_t abs = reinterpret_cast<uintptr_t>(target);
        Log("[Hook] MH_CreateHook refused %p (game+0x%llX): %s (last error %lu) - this "
            "hook is not installed, so the feature behind it stays vanilla\n",
            target,
            static_cast<unsigned long long>(base ? abs - base : 0ull),
            MhStatusName(st),
            lastErr);
        return false;
    }
    if (st == MH_ERROR_ALREADY_CREATED && !*original)
    {
        Log("[Hook] %p is already hooked by another module and no trampoline was "
            "handed back - this hook is not installed\n", target);
        return false;
    }

    st = EnableOrQueueHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED)
    {
        Log("[Hook] enable refused %p: %s - the hook was created but never armed\n",
            target, MhStatusName(st));
        return false;
    }

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