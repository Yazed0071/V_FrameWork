#include "pch.h"
#include "PartIdWiden.h"

#include <Windows.h>
#include <cstring>

#include "AddressSet.h"
#include "log.h"

namespace
{
    struct WidenSite
    {
        std::uint64_t addr;
        const char*   slot;
        std::uint8_t  expect[5];
        std::uint8_t  patch[5];
        int           len;
    };

    static const WidenSite kSitesEn154[] = {
        { 0x140DC38FFull, "receiver",     { 0x44,0x0F,0xB6,0x45,0x00 }, { 0x44,0x0F,0xB7,0x45,0x00 }, 5 },
        { 0x140DC3AF4ull, "barrel",       { 0x0F,0xB6,0x55,0x01,0x00 }, { 0x0F,0xB7,0x55,0x02,0x00 }, 4 },
        { 0x140DC3CCAull, "magazine",     { 0x0F,0xB6,0x45,0x02,0x00 }, { 0x0F,0xB7,0x45,0x04,0x00 }, 4 },
        { 0x140DC3C71ull, "stock",        { 0x0F,0xB6,0x45,0x03,0x00 }, { 0x0F,0xB7,0x45,0x06,0x00 }, 4 },
        { 0x140DC3D0Full, "muzzleOption", { 0x0F,0xB6,0x45,0x05,0x00 }, { 0x0F,0xB7,0x45,0x0A,0x00 }, 4 },
        { 0x140DC3E6Cull, "sight",        { 0x0F,0xB6,0x45,0x06,0x00 }, { 0x0F,0xB7,0x45,0x0C,0x00 }, 4 },
        { 0x140DC3EF0ull, "sight2",       { 0x0F,0xB6,0x45,0x07,0x00 }, { 0x0F,0xB7,0x45,0x0E,0x00 }, 4 },
        { 0x140DC3D81ull, "laserFlash1",  { 0x0F,0xB6,0x45,0x08,0x00 }, { 0x0F,0xB7,0x45,0x10,0x00 }, 4 },
        { 0x140DC3DDFull, "laserFlash2",  { 0x0F,0xB6,0x45,0x09,0x00 }, { 0x0F,0xB7,0x45,0x12,0x00 }, 4 },
        { 0x140DC3F40ull, "underBarrel",  { 0x0F,0xB6,0x45,0x0A,0x00 }, { 0x0F,0xB7,0x45,0x14,0x00 }, 4 },
        { 0x140DC3FC6ull, "underBarrelCmp", { 0x80,0x7D,0x0A,0x06,0x00 }, { 0x80,0x7D,0x14,0x06,0x00 }, 4 },
    };

    static const int kDescWordToRowSlot[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 8 };

    static bool g_Active = false;
    static bool g_Applied[sizeof(kSitesEn154) / sizeof(kSitesEn154[0])] = {};

    static bool ReadBytesSEH(const void* p, std::uint8_t* out, int len)
    {
        __try
        {
            std::memcpy(out, p, static_cast<size_t>(len));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool WriteBytes(void* addr, const std::uint8_t* bytes, int len)
    {
        DWORD oldProt = 0;
        if (!VirtualProtect(addr, static_cast<SIZE_T>(len), PAGE_EXECUTE_READWRITE, &oldProt))
            return false;
        std::memcpy(addr, bytes, static_cast<size_t>(len));
        DWORD tmp = 0;
        VirtualProtect(addr, static_cast<SIZE_T>(len), oldProt, &tmp);
        FlushInstructionCache(GetCurrentProcess(), addr, static_cast<SIZE_T>(len));
        return true;
    }
}

bool PartIdWiden_IsActive()
{
    return g_Active;
}

int PartIdWiden_MaxPartId()
{
    return g_Active ? 0xFFFF : 0xFF;
}

bool PartIdWiden_BuildWideDesc(const unsigned char* narrowDesc,
                               const int* wideIds12,
                               std::uint8_t* out24)
{
    if (!out24)
        return false;
    std::memset(out24, 0, 24);
    for (int word = 0; word < 11; ++word)
    {
        const int slot = kDescWordToRowSlot[word];
        int id = 0;
        if (wideIds12 && wideIds12[slot] > 0)
            id = wideIds12[slot];
        else if (narrowDesc)
            id = narrowDesc[word];
        if (id < 0)
            id = 0;
        if (id > 0xFFFF)
            id = 0xFFFF;
        out24[word * 2 + 0] = static_cast<std::uint8_t>(id & 0xFF);
        out24[word * 2 + 1] = static_cast<std::uint8_t>((id >> 8) & 0xFF);
    }
    return true;
}

bool PartIdWiden_Install()
{
    if (g_Active)
        return true;

    const int n = static_cast<int>(sizeof(kSitesEn154) / sizeof(kSitesEn154[0]));

    if (gGameBuild != ::AddressSetRuntime::GameBuild::En_1_0_15_4a)
    {
        Log("[PartIdWiden] ERROR: the part-id widening patch table is authored for EN 1.0.15.4 "
            "(day3900) only - every custom part id stays capped at the 255-lane byte on this "
            "build until the table is ported\n");
        return false;
    }

    for (int i = 0; i < n; ++i)
    {
        void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(kSitesEn154[i].addr));
        std::uint8_t cur[5] = {};
        if (!ReadBytesSEH(addr, cur, kSitesEn154[i].len))
        {
            Log("[PartIdWiden] ERROR: site %s @%p is unreadable - NOTHING was patched, part ids "
                "stay byte-capped (a half-applied table would load weapons with garbage parts)\n",
                kSitesEn154[i].slot, addr);
            return false;
        }
        if (std::memcmp(cur, kSitesEn154[i].expect, static_cast<size_t>(kSitesEn154[i].len)) != 0)
        {
            Log("[PartIdWiden] ERROR: site %s @%p holds unexpected bytes "
                "(%02X %02X %02X %02X %02X) - another mod owns this instruction or the build "
                "differs. NOTHING was patched; part ids stay byte-capped\n",
                kSitesEn154[i].slot, addr, cur[0], cur[1], cur[2], cur[3], cur[4]);
            return false;
        }
    }

    for (int i = 0; i < n; ++i)
    {
        void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(kSitesEn154[i].addr));
        if (!WriteBytes(addr, kSitesEn154[i].patch, kSitesEn154[i].len))
        {
            Log("[PartIdWiden] ERROR: site %s @%p failed to write - rolling back the %d site(s) "
                "already applied so the desc layout stays consistent\n",
                kSitesEn154[i].slot, addr, i);
            for (int j = 0; j < i; ++j)
            {
                void* back = reinterpret_cast<void*>(static_cast<uintptr_t>(kSitesEn154[j].addr));
                WriteBytes(back, kSitesEn154[j].expect, kSitesEn154[j].len);
                g_Applied[j] = false;
            }
            return false;
        }
        g_Applied[i] = true;
    }

    g_Active = true;
    return true;
}

void PartIdWiden_Uninstall()
{
    const int n = static_cast<int>(sizeof(kSitesEn154) / sizeof(kSitesEn154[0]));
    for (int i = 0; i < n; ++i)
    {
        if (!g_Applied[i])
            continue;
        void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(kSitesEn154[i].addr));
        WriteBytes(addr, kSitesEn154[i].expect, kSitesEn154[i].len);
        g_Applied[i] = false;
    }
    g_Active = false;
}
