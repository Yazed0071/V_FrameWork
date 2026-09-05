#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "HookUtils.h"
#include "VehicleDemoStopExempt.h"
#include "log.h"

namespace
{
    constexpr std::uint8_t kSitePattern[] =
    {
        0x48, 0x8B, 0x06, 0xB2, 0x20, 0x48, 0x8B, 0xCE, 0xFF, 0x10, 0x84, 0xC0, 0x74, 0x07,
        0x66, 0x83, 0x4B, 0x58, 0x08, 0xEB, 0x09, 0xB8, 0xF7, 0xFF, 0x00, 0x00, 0x66, 0x21,
        0x43, 0x58
    };
    constexpr std::size_t   kSiteOffsetInPattern = 3;
    constexpr std::size_t   kPatchSize           = 7;
    constexpr std::size_t   kThunkSize           = 64;
    constexpr std::size_t   kEntriesOffset       = 0x30;
    constexpr std::size_t   kRealizedSetOffset   = 0x1C8;
    constexpr std::size_t   kEntryCountOffset    = 0x1E0;
    constexpr std::size_t   kEntryStride         = 0x80;
    constexpr std::size_t   kEntryIgnoreFlags    = 0x7C;
    constexpr std::uint16_t kEntryIgnoreBit      = 0x1000;
    constexpr unsigned      kMaxEntries          = 256;
    constexpr std::uint64_t kReportIntervalMs    = 10000;

    std::uint8_t* g_Site  = nullptr;
    std::uint8_t* g_Thunk = nullptr;
    std::uint8_t  g_OrigBytes[kPatchSize] = {};
    bool          g_Patched = false;

    std::atomic<std::uint64_t> g_LastExemptReportMs{ 0 };
    std::atomic<std::uint64_t> g_LastStoppedReportMs{ 0 };
    std::atomic<std::uint64_t> g_ExemptFrames{ 0 };
    std::atomic<std::uint64_t> g_StoppedFrames{ 0 };

    bool FindIgnoringVehicle(std::uint8_t* type, unsigned* outIndex, unsigned* outRealized)
    {
        __try
        {
            const unsigned count = *reinterpret_cast<std::uint16_t*>(type + kEntryCountOffset);
            auto* entries = *reinterpret_cast<std::uint8_t**>(type + kEntriesOffset);
            auto* realizedSet = *reinterpret_cast<std::uint8_t**>(type + kRealizedSetOffset);
            if (!entries || !realizedSet)
                return false;
            const std::uint32_t* bits = *reinterpret_cast<const std::uint32_t**>(realizedSet + 8);
            if (!bits)
                return false;
            unsigned realized = 0;
            bool found = false;
            for (unsigned i = 0; i < count && i < kMaxEntries; ++i)
            {
                if (!((bits[i >> 5] >> (i & 31)) & 1u))
                    continue;
                ++realized;
                if (!found
                    && (*reinterpret_cast<std::uint16_t*>(entries + i * kEntryStride + kEntryIgnoreFlags)
                        & kEntryIgnoreBit))
                {
                    *outIndex = i;
                    found = true;
                }
            }
            *outRealized = realized;
            return found;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* AllocateThunkNear(std::uintptr_t nearAddr, std::size_t size)
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
                void* p = VirtualAlloc(reinterpret_cast<LPVOID>(roundedNear - offset), size,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (p)
                    return p;
            }
            void* p2 = VirtualAlloc(reinterpret_cast<LPVOID>(roundedNear + offset), size,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p2)
                return p2;
        }
        return nullptr;
    }

    bool RegionMatches(const std::uint8_t* at)
    {
        __try
        {
            return std::memcmp(at, kSitePattern, sizeof(kSitePattern)) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::size_t ScanRegion(std::uint8_t* begin, std::size_t size, std::uint8_t** outSite,
                           std::size_t found)
    {
        if (size < sizeof(kSitePattern))
            return found;
        __try
        {
            const std::uint8_t first = kSitePattern[0];
            const std::size_t last = size - sizeof(kSitePattern);
            for (std::size_t i = 0; i <= last; ++i)
            {
                if (begin[i] != first)
                    continue;
                if (std::memcmp(begin + i, kSitePattern, sizeof(kSitePattern)) == 0)
                {
                    if (found == 0)
                        *outSite = begin + i + kSiteOffsetInPattern;
                    ++found;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return found;
    }

    std::size_t FindSite(std::uint8_t** outSite)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base)
            return 0;
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        std::size_t found = 0;
        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s)
        {
            if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            std::uint8_t* cur = base + sec[s].VirtualAddress;
            std::uint8_t* end = cur + sec[s].Misc.VirtualSize;
            while (cur < end)
            {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(cur, &mbi, sizeof(mbi)))
                    break;
                auto* regionEnd = static_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
                if (regionEnd > end)
                    regionEnd = end;
                const bool executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                                                        | PAGE_EXECUTE_READWRITE
                                                        | PAGE_EXECUTE_WRITECOPY)) != 0;
                if (mbi.State == MEM_COMMIT && executable && !(mbi.Protect & PAGE_GUARD))
                    found = ScanRegion(cur, static_cast<std::size_t>(regionEnd - cur), outSite, found);
                cur = regionEnd;
            }
        }
        return found;
    }
}

bool VehicleDemoStopExemptDecision(std::uint8_t* type)
{
    unsigned index = 0;
    unsigned realized = 0;
    const bool exempt = FindIgnoringVehicle(type, &index, &realized);
    const std::uint64_t now = GetTickCount64();
    if (exempt)
    {
        g_ExemptFrames.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t last = g_LastExemptReportMs.load(std::memory_order_relaxed);
        if (now - last >= kReportIntervalMs)
        {
            g_LastExemptReportMs.store(now, std::memory_order_relaxed);
            Log("[VehicleDemoStop] S_STOP_FOR_DEMO is raised but vehicle instance %u carries "
                "SetIgnoreDisableNpc, so the vehicle type is not flagged stopped this frame - %u "
                "realized vehicle(s) keep driving (the stop flag lives on the type object, so every "
                "realized vehicle is released together while an exempt one exists)\n",
                index, realized);
        }
    }
    else
    {
        g_StoppedFrames.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t last = g_LastStoppedReportMs.load(std::memory_order_relaxed);
        if (now - last >= kReportIntervalMs)
        {
            g_LastStoppedReportMs.store(now, std::memory_order_relaxed);
            Log("[VehicleDemoStop] S_STOP_FOR_DEMO applied to the vehicle type: %u realized "
                "vehicle(s), none carry SetIgnoreDisableNpc\n", realized);
        }
    }
    return exempt;
}

namespace
{
    void BuildThunk(std::uint8_t* t)
    {
        std::memset(t, 0xCC, kThunkSize);
        std::size_t o = 0;
        const std::uint8_t head[] =
        {
            0x48, 0x83, 0xEC, 0x28,
            0xB2, 0x20,
            0x48, 0x8B, 0xCE,
            0xFF, 0x10,
            0x84, 0xC0,
            0x74, 0x24,
            0x48, 0x8B, 0xCB,
            0x48, 0xB8
        };
        std::memcpy(t + o, head, sizeof(head));
        o += sizeof(head);
        const std::uint64_t fn = reinterpret_cast<std::uint64_t>(&VehicleDemoStopExemptDecision);
        std::memcpy(t + o, &fn, sizeof(fn));
        o += sizeof(fn);
        const std::uint8_t tail[] =
        {
            0xFF, 0xD0,
            0x84, 0xC0,
            0x74, 0x07,
            0x33, 0xC0,
            0x48, 0x83, 0xC4, 0x28,
            0xC3,
            0xB8, 0x01, 0x00, 0x00, 0x00,
            0x48, 0x83, 0xC4, 0x28,
            0xC3,
            0x48, 0x83, 0xC4, 0x28,
            0xC3
        };
        std::memcpy(t + o, tail, sizeof(tail));
    }
}

bool Install_VehicleDemoStopExempt()
{
    if (g_Patched)
        return true;

    std::uint8_t* site = nullptr;
    const std::size_t matches = FindSite(&site);
    if (matches != 1 || !site)
    {
        Log("[VehicleDemoStop] the vehicle type's S_STOP_FOR_DEMO check was not found uniquely "
            "(%zu match(es)), so vehicles still freeze for every demo regardless of "
            "SetIgnoreDisableNpc\n", matches);
        return false;
    }

    g_Thunk = static_cast<std::uint8_t*>(
        AllocateThunkNear(reinterpret_cast<std::uintptr_t>(site), 0x1000));
    if (!g_Thunk)
    {
        Log("[VehicleDemoStop] no executable page could be placed near 0x%llX, so the check is "
            "left untouched\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(site)));
        return false;
    }
    BuildThunk(g_Thunk);

    const std::int64_t rel = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(g_Thunk))
                           - (static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site)) + 5);
    if (rel < INT32_MIN || rel > INT32_MAX)
    {
        VirtualFree(g_Thunk, 0, MEM_RELEASE);
        g_Thunk = nullptr;
        Log("[VehicleDemoStop] the thunk landed out of rel32 range, so the check is left untouched\n");
        return false;
    }

    std::uint8_t patch[kPatchSize] = { 0xE8, 0, 0, 0, 0, 0x66, 0x90 };
    const std::int32_t rel32 = static_cast<std::int32_t>(rel);
    std::memcpy(patch + 1, &rel32, sizeof(rel32));

    DWORD old = 0;
    if (!VirtualProtect(site, kPatchSize, PAGE_EXECUTE_READWRITE, &old))
    {
        VirtualFree(g_Thunk, 0, MEM_RELEASE);
        g_Thunk = nullptr;
        Log("[VehicleDemoStop] the check site could not be unprotected, so it is left untouched\n");
        return false;
    }
    std::memcpy(g_OrigBytes, site, kPatchSize);
    std::memcpy(site, patch, kPatchSize);
    VirtualProtect(site, kPatchSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, kPatchSize);

    g_Site = site;
    g_Patched = true;
    Log("[VehicleDemoStop] armed at 0x%llX (thunk %p): the vehicle type's S_STOP_FOR_DEMO check now "
        "yields to SetIgnoreDisableNpc - while a realized vehicle carries that command the vehicle "
        "type is not frozen for a demo (the flag is type-wide, so it cannot be released per "
        "vehicle at this site; a demo with no exempt vehicle behaves exactly as before)\n",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(site)), g_Thunk);
    return true;
}

void Uninstall_VehicleDemoStopExempt()
{
    if (g_Patched && g_Site)
    {
        DWORD old = 0;
        if (VirtualProtect(g_Site, kPatchSize, PAGE_EXECUTE_READWRITE, &old))
        {
            std::memcpy(g_Site, g_OrigBytes, kPatchSize);
            VirtualProtect(g_Site, kPatchSize, old, &old);
            FlushInstructionCache(GetCurrentProcess(), g_Site, kPatchSize);
        }
    }
    g_Patched = false;
    g_Site = nullptr;
    if (g_Thunk)
    {
        VirtualFree(g_Thunk, 0, MEM_RELEASE);
        g_Thunk = nullptr;
    }
}
