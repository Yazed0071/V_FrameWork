#include "pch.h"
#include "ShowMissionIcon.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    static bool          g_Installed       = false;
    static std::uint8_t* g_Thunk           = nullptr;
    static std::uint8_t  g_OrigCallBytes[6]{};
    static void*         g_PatchedCallSite = nullptr;
    static std::mutex    g_OverrideMutex;

    static constexpr std::size_t kThunkSize          = 64;
    static constexpr std::size_t kFlagOffsetInThunk  = 36;
    static constexpr std::size_t kHashOffsetInThunk  = 44;
    static constexpr std::size_t kContOffsetInThunk  = 28;

    static void* AllocateThunkNear(std::uintptr_t nearAddr, std::size_t size)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::uintptr_t granularity = si.dwAllocationGranularity;
        const std::uintptr_t roundedNear = nearAddr & ~(granularity - 1);

        const std::uintptr_t maxDistance = 0x60000000ull;

        for (std::uintptr_t offset = granularity; offset < maxDistance; offset += granularity)
        {
            if (roundedNear >= offset)
            {
                const std::uintptr_t tryAddr = roundedNear - offset;
                void* p = VirtualAlloc(reinterpret_cast<LPVOID>(tryAddr),
                    size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (p) return p;
            }

            const std::uintptr_t tryAddr2 = roundedNear + offset;
            void* p2 = VirtualAlloc(reinterpret_cast<LPVOID>(tryAddr2),
                size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p2) return p2;
        }

        return nullptr;
    }

    static void BuildThunk(std::uint8_t* thunk, std::uintptr_t continuationAddr)
    {
        std::memset(thunk, 0xCC, kThunkSize);

        std::size_t o = 0;

        thunk[o++] = 0x80;
        thunk[o++] = 0x3D;
        std::int32_t flagRel = static_cast<std::int32_t>(kFlagOffsetInThunk) - 7;
        std::memcpy(thunk + o, &flagRel, 4); o += 4;
        thunk[o++] = 0x00;

        thunk[o++] = 0x74;
        thunk[o++] = 0x07;

        thunk[o++] = 0x48;
        thunk[o++] = 0x8B;
        thunk[o++] = 0x15;
        std::int32_t hashRel = static_cast<std::int32_t>(kHashOffsetInThunk) - 16;
        std::memcpy(thunk + o, &hashRel, 4); o += 4;

        thunk[o++] = 0xFF;
        thunk[o++] = 0x93;
        std::uint32_t vtblOff = 0x750;
        std::memcpy(thunk + o, &vtblOff, 4); o += 4;

        thunk[o++] = 0xFF;
        thunk[o++] = 0x25;
        std::int32_t contRel = 0;
        std::memcpy(thunk + o, &contRel, 4); o += 4;

        std::uint64_t contAddr = static_cast<std::uint64_t>(continuationAddr);
        std::memcpy(thunk + kContOffsetInThunk, &contAddr, 8);

        thunk[kFlagOffsetInThunk] = 0x00;

        std::uint64_t initialHash = 0;
        std::memcpy(thunk + kHashOffsetInThunk, &initialHash, 8);
    }
}

bool ShowMissionIcon_SetTitleHash(std::uint64_t hash48)
{
    std::lock_guard<std::mutex> lock(g_OverrideMutex);

    if (!g_Thunk) return false;

    if (hash48 == 0)
    {
        g_Thunk[kFlagOffsetInThunk] = 0x00;
        std::uint64_t zero = 0;
        std::memcpy(g_Thunk + kHashOffsetInThunk, &zero, 8);
        return true;
    }

    const std::uint64_t masked = hash48 & 0x0000FFFFFFFFFFFFull;
    std::memcpy(g_Thunk + kHashOffsetInThunk, &masked, 8);
    g_Thunk[kFlagOffsetInThunk] = 0x01;
    return true;
}

bool ShowMissionIcon_SetTitleOverride(const char* text)
{
    (void)text;
    return false;
}

bool ShowMissionIcon_PatchTitleHash(std::uint64_t value)
{
    return ShowMissionIcon_SetTitleHash(value);
}

bool Install_ShowMissionIcon_Hook()
{
    if (g_Installed) return true;

    if (!gAddr.IconTitleGetLangTextCall)
    {
        LogDebug("[ShowMissionIcon] IconTitleGetLangTextCall address is 0; title detour disabled\n");
        return false;
    }

    auto* callSite = reinterpret_cast<std::uint8_t*>(
        ResolveGameAddress(gAddr.IconTitleGetLangTextCall));
    if (!callSite)
    {
        Log("[ShowMissionIcon] call site resolve failed\n");
        return false;
    }

    g_Thunk = reinterpret_cast<std::uint8_t*>(
        AllocateThunkNear(reinterpret_cast<std::uintptr_t>(callSite), 0x1000));
    if (!g_Thunk)
    {
        Log("[ShowMissionIcon] thunk allocation failed\n");
        return false;
    }

    const std::uintptr_t continuationAddr =
        reinterpret_cast<std::uintptr_t>(callSite) + 6;
    BuildThunk(g_Thunk, continuationAddr);

    const std::int64_t rel = static_cast<std::int64_t>(
        reinterpret_cast<std::uintptr_t>(g_Thunk)) -
        (static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(callSite)) + 5);

    if (rel < INT32_MIN || rel > INT32_MAX)
    {
        LogDebug("[ShowMissionIcon] thunk too far for rel32 JMP (rel=0x%llX)\n",
            static_cast<long long>(rel));
        VirtualFree(g_Thunk, 0, MEM_RELEASE);
        g_Thunk = nullptr;
        return false;
    }

    std::uint8_t patch[6];
    patch[0] = 0xE9;
    std::int32_t rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(patch + 1, &rel32, 4);
    patch[5] = 0x90;

    DWORD oldProtect = 0;
    if (!VirtualProtect(callSite, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        Log("[ShowMissionIcon] VirtualProtect on call site failed\n");
        VirtualFree(g_Thunk, 0, MEM_RELEASE);
        g_Thunk = nullptr;
        return false;
    }

    std::memcpy(g_OrigCallBytes, callSite, 6);
    std::memcpy(callSite, patch, 6);

    DWORD tmp = 0;
    VirtualProtect(callSite, 6, oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), callSite, 6);

    g_PatchedCallSite = callSite;
    g_Installed = true;

#ifdef _DEBUG
    LogDebug("[ShowMissionIcon] title detour installed: callsite=%p thunk=%p continuation=0x%llX\n",
        static_cast<void*>(callSite),
        static_cast<void*>(g_Thunk),
        static_cast<unsigned long long>(continuationAddr));
#endif
    return true;
}

bool Uninstall_ShowMissionIcon_Hook()
{
    if (!g_Installed) return true;

    if (g_PatchedCallSite)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_PatchedCallSite, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            std::memcpy(g_PatchedCallSite, g_OrigCallBytes, 6);
            DWORD tmp = 0;
            VirtualProtect(g_PatchedCallSite, 6, oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), g_PatchedCallSite, 6);
        }
        g_PatchedCallSite = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_OverrideMutex);
        if (g_Thunk)
        {
            VirtualFree(g_Thunk, 0, MEM_RELEASE);
            g_Thunk = nullptr;
        }
    }

    g_Installed = false;
#ifdef _DEBUG
    LogDebug("[ShowMissionIcon] title detour uninstalled\n");
#endif
    return true;
}
