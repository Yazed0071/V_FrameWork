#include "pch.h"
#include "HookArena.h"

#include <Windows.h>
#include <cstdint>

#include "log.h"

namespace HookArena
{
    namespace
    {
        static constexpr std::size_t   kGranuleCount = 16;
        static constexpr std::uintptr_t kSearchSpan  = 0x30000000ull;

        static void*       g_Granules[kGranuleCount] = {};
        static std::size_t g_Count    = 0;
        static bool        g_Reserved = false;
        static bool        g_SaidHandOver = false;
        static bool        g_SaidEmpty    = false;

        static std::uintptr_t ImageEnd(std::uintptr_t base)
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return 0;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + static_cast<std::uintptr_t>(dos->e_lfanew));
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return 0;
            return base + nt->OptionalHeader.SizeOfImage;
        }
    }

    void ReserveEarly()
    {
        if (g_Reserved)
            return;
        g_Reserved = true;

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::uintptr_t gran = si.dwAllocationGranularity;
        if (gran == 0)
            return;

        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!base)
            return;

        const std::uintptr_t end = ImageEnd(base);
        if (end == 0)
            return;

        const std::uintptr_t first = (end + gran - 1) & ~(gran - 1);
        const std::uintptr_t last  = base + kSearchSpan;

        for (std::uintptr_t addr = first;
             addr < last && g_Count < kGranuleCount;
             addr += gran)
        {
            if (void* p = VirtualAlloc(reinterpret_cast<LPVOID>(addr), gran,
                                       MEM_RESERVE, PAGE_NOACCESS))
                g_Granules[g_Count++] = p;
        }

        if (g_Count == 0)
            Log("[HookArena] no address space could be reserved above the game image, "
                "so MinHook has no fallback if the region fills before the hooks are "
                "installed and any hook that misses stays vanilla\n");
    }

    bool ReleaseOne()
    {
        if (g_Count == 0)
        {
            if (!g_SaidEmpty)
            {
                g_SaidEmpty = true;
                Log("[HookArena] the reserved trampoline pool is empty - every hook "
                    "from here on stays vanilla because no 64 KB granule is free "
                    "near the game image\n");
            }
            return false;
        }

        void* p = g_Granules[--g_Count];
        g_Granules[g_Count] = nullptr;
        if (!VirtualFree(p, 0, MEM_RELEASE))
            return false;

        if (!g_SaidHandOver)
        {
            g_SaidHandOver = true;
            Log("[HookArena] MinHook ran out of trampoline space near the game image, "
                "so the reserve is being handed over one granule at a time (%zu of %zu "
                "left); without it the remaining hooks would not install\n",
                g_Count, kGranuleCount);
        }
        return true;
    }

    std::size_t Remaining()
    {
        return g_Count;
    }
}
