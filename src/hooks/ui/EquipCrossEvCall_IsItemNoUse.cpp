#include "pch.h"
#include "EquipCrossEvCall_IsItemNoUse.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    struct ReturnFalsePatchSite
    {
        std::uintptr_t address;   
        std::uint8_t   expect[12];
        std::size_t    expectLen;
        std::uint8_t   write[8];  
        std::size_t    writeLen;
    };

    // tpp::ui::hud::EquipCrossEvCall::IsItemNoUse
    static const ReturnFalsePatchSite* GetPatchSiteForCurrentBuild()
    {
        if (!gAddr.EquipCrossEvCall_IsItemNoUse)
            return nullptr;

        static ReturnFalsePatchSite site =
        {
            0, // holder
            { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20 }, 10,
            { 0x31,0xC0,0xC3 }, 3,     // xor eax,eax ; ret  -> return false
        };
        site.address = gAddr.EquipCrossEvCall_IsItemNoUse;
        return &site;
    }

    static bool        g_Applied      = false;
    static void*       g_PatchTarget  = nullptr;
    static std::uint8_t g_OriginalBytes[8] = {};
    static std::size_t g_OriginalLen  = 0;

    static bool WriteBytes(void* target, const std::uint8_t* src, std::size_t size)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("[IsItemNoUse] VirtualProtect failed (err=%lu)\n", GetLastError());
            return false;
        }
        std::memcpy(target, src, size);
        DWORD restored = 0;
        VirtualProtect(target, size, oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, size);
        return true;
    }
}

bool Install_IsItemNoUsePatch()
{
    if (g_Applied)
        return true;

    const ReturnFalsePatchSite* site = GetPatchSiteForCurrentBuild();
    if (!site)
    {
        LogDebug("[IsItemNoUse] no patch address for current build\n");
        return false;
    }

    void* target = ResolveGameAddress(site->address);
    if (!target)
    {
        LogDebug("[IsItemNoUse] ResolveGameAddress returned null\n");
        return false;
    }

    const auto* cur = static_cast<const std::uint8_t*>(target);
    if (std::memcmp(cur, site->expect, site->expectLen) != 0)
    {
        LogDebug("[IsItemNoUse] unexpected bytes at %p (have %02X %02X %02X %02X %02X, "
            "expected %02X %02X %02X %02X %02X) - not patching\n",
            target, cur[0], cur[1], cur[2], cur[3], cur[4],
            site->expect[0], site->expect[1], site->expect[2], site->expect[3], site->expect[4]);
        return false;
    }

    g_OriginalLen = site->writeLen;
    std::memcpy(g_OriginalBytes, cur, g_OriginalLen);
    g_PatchTarget = target;

    if (!WriteBytes(target, site->write, site->writeLen))
        return false;

#ifdef _DEBUG
    LogDebug("[IsItemNoUse] patched %p (%zu bytes) -> EquipCrossEvCall::IsItemNoUse always returns false\n",
        target, site->writeLen);
#endif

    g_Applied = true;
    return true;
}

void Uninstall_IsItemNoUsePatch()
{
    if (!g_Applied)
        return;

    if (g_PatchTarget && g_OriginalLen)
        WriteBytes(g_PatchTarget, g_OriginalBytes, g_OriginalLen);

    g_Applied = false;
    g_PatchTarget = nullptr;
    g_OriginalLen = 0;
}
