#pragma once

#include <Windows.h>
#include <cstdint>
#include "MinHook.h"

// Returns the current game executable base address.
// Params: none
inline uintptr_t GetExeBase()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

constexpr uintptr_t EXE_PREFERRED_BASE = 0x140000000ull;

// Converts an absolute preferred-base address into an RVA.
// Params: absAddr (uintptr_t)
inline constexpr uintptr_t ToRva(uintptr_t absAddr)
{
    return absAddr - EXE_PREFERRED_BASE;
}

// Resolves a preferred-base absolute game address against the current module base.
// Params: absAddr (uintptr_t)
inline void* ResolveGameAddress(uintptr_t absAddr)
{
    const uintptr_t base = GetExeBase();
    if (!base)
        return nullptr;

    return reinterpret_cast<void*>(base + ToRva(absAddr));
}

// Creates and enables a MinHook hook.
// Params: target (void*), detour (void*), original (void**)
inline bool CreateAndEnableHook(void* target, void* detour, void** original)
{
    if (!target || !detour || !original)
        return false;

    MH_STATUS st = MH_CreateHook(target, detour, original);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
        return false;

    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED)
        return false;

    return true;
}

// Disables and removes a MinHook hook.
// Params: target (void*)
inline bool DisableAndRemoveHook(void* target)
{
    if (!target)
        return false;

    MH_DisableHook(target);
    MH_RemoveHook(target);
    return true;
}