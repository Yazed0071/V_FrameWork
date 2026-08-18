#include "pch.h"
#include "EquipCrossSetEquipItemPatch.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    struct BytePatchSite
    {
        std::uintptr_t address;
        std::uint8_t   original[2];
        std::uint8_t   patched[2];
    };

    static const BytePatchSite* GetPatchSitesForCurrentBuild(std::size_t& outCount)
    {
        if (!gAddr.EquipCrossSetEquipItem_Site1 ||
            !gAddr.EquipCrossSetEquipItem_Site2 ||
            !gAddr.EquipCrossSetEquipItem_Site3)
        {
            outCount = 0;
            return nullptr;
        }

        static BytePatchSite sites[3] =
        {
            { 0, { 0x74, 0x41 }, { 0xEB, 0x41 } },
            { 0, { 0x74, 0xC3 }, { 0x90, 0x90 } },
            { 0, { 0x75, 0x81 }, { 0x90, 0x90 } },
        };
        sites[0].address = gAddr.EquipCrossSetEquipItem_Site1;
        sites[1].address = gAddr.EquipCrossSetEquipItem_Site2;
        sites[2].address = gAddr.EquipCrossSetEquipItem_Site3;
        outCount = 3;
        return sites;
    }

    static bool g_Applied = false;

    static bool WriteBytes(void* target, const std::uint8_t* src, std::size_t size)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("[EquipCrossSetEquipItem] VirtualProtect failed (err=%lu)\n", GetLastError());
            return false;
        }
        std::memcpy(target, src, size);
        DWORD restored = 0;
        VirtualProtect(target, size, oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, size);
        return true;
    }
}

bool Install_EquipCrossSetEquipItemPatch()
{
    if (g_Applied)
        return true;

    std::size_t count = 0;
    const BytePatchSite* sites = GetPatchSitesForCurrentBuild(count);
    if (!sites || count == 0)
    {
        LogDebug("[EquipCrossSetEquipItem] no patch addresses for current build\n");
        return false;
    }

    constexpr std::size_t kMaxSites = 16;
    if (count > kMaxSites)
    {
        LogDebug("[EquipCrossSetEquipItem] too many patch sites (%zu)\n", count);
        return false;
    }

    void* targets[kMaxSites] = {};
    for (std::size_t i = 0; i < count; ++i)
    {
        void* target = ResolveGameAddress(sites[i].address);
        if (!target)
        {
            LogDebug("[EquipCrossSetEquipItem] ResolveGameAddress returned null for site %zu\n", i);
            return false;
        }

        const auto* cur = static_cast<const std::uint8_t*>(target);
        if (cur[0] != sites[i].original[0] || cur[1] != sites[i].original[1])
        {
            LogDebug("[EquipCrossSetEquipItem] unexpected bytes at %p (%02X %02X, expected %02X %02X) - not patching\n",
                target, cur[0], cur[1], sites[i].original[0], sites[i].original[1]);
            return false;
        }

        targets[i] = target;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        if (!WriteBytes(targets[i], sites[i].patched, sizeof(sites[i].patched)))
            return false;

#ifdef _DEBUG
        LogDebug("[EquipCrossSetEquipItem] patched %p: %02X %02X -> %02X %02X\n",
            targets[i],
            sites[i].original[0], sites[i].original[1],
            sites[i].patched[0], sites[i].patched[1]);
#endif
    }

    g_Applied = true;
    return true;
}

void Uninstall_EquipCrossSetEquipItemPatch()
{
    if (!g_Applied)
        return;

    std::size_t count = 0;
    const BytePatchSite* sites = GetPatchSitesForCurrentBuild(count);
    if (sites)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            void* target = ResolveGameAddress(sites[i].address);
            if (target)
                WriteBytes(target, sites[i].original, sizeof(sites[i].original));
        }
    }

    g_Applied = false;
}
