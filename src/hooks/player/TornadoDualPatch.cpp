#include "pch.h"
#include "TornadoDualPatch.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    static constexpr std::uint8_t kOriginalBytes[2] = { 0x74, 0x10 };
    static constexpr std::uint8_t kEnabledBytes[2]  = { 0x90, 0x90 };
    static bool g_Applied = false;

    static bool WriteBytes(void* target, const std::uint8_t* src)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(kOriginalBytes), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("[TornadoDual] VirtualProtect failed (err=%lu)\n", GetLastError());
            return false;
        }
        std::memcpy(target, src, sizeof(kOriginalBytes));
        DWORD restored = 0;
        VirtualProtect(target, sizeof(kOriginalBytes), oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(kOriginalBytes));
        return true;
    }
}

bool Install_TornadoDualPatch()
{
    if (g_Applied)
        return true;

    if (!gAddr.TornadoDualPatch)
    {
        LogDebug("[TornadoDual] patch address not set for current build\n");
        return false;
    }

    void* target = ResolveGameAddress(gAddr.TornadoDualPatch);
    if (!target)
    {
        LogDebug("[TornadoDual] ResolveGameAddress returned null\n");
        return false;
    }

    const auto* cur = static_cast<const std::uint8_t*>(target);
    if (cur[0] != kOriginalBytes[0] || cur[1] != kOriginalBytes[1])
    {
        LogDebug("[TornadoDual] unexpected bytes at %p (%02X %02X) - not patching\n", target, cur[0], cur[1]);
        return false;
    }

    if (!WriteBytes(target, kEnabledBytes))
        return false;

    g_Applied = true;
#ifdef _DEBUG
    LogDebug("[TornadoDual] enabled by default (wrote 90 90 at %p)\n", target);
#endif
    return true;
}

void Uninstall_TornadoDualPatch()
{
    if (!g_Applied)
        return;

    if (gAddr.TornadoDualPatch)
    {
        void* target = ResolveGameAddress(gAddr.TornadoDualPatch);
        if (target)
            WriteBytes(target, kOriginalBytes);
    }
    g_Applied = false;
}
