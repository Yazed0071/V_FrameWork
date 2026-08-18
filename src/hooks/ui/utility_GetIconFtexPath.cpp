#include "pch.h"
#include "utility_GetIconFtexPath.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "AddressSet.h"

namespace
{
    using GetIconFtexPath_t = std::int64_t* (__fastcall*)(std::int64_t* outPathId, std::uint32_t equipId, int mode);

    static GetIconFtexPath_t g_OrigGetIconFtexPath = nullptr;
    static std::unordered_map<int, std::uint64_t> g_PerEquipIconPaths;
    static std::mutex g_PerEquipIconPathsMutex;
    static bool g_EquipIconFtexPathHookInstalled = false;
}


void EquipIcon_SetEquipIdIconFtexPath(int equipId, uint64_t texturePathHash)
{
    std::lock_guard<std::mutex> lock(g_PerEquipIconPathsMutex);
    g_PerEquipIconPaths[equipId] = texturePathHash;
}


void EquipIcon_ClearIconFtexPath(int equipId)
{
    std::lock_guard<std::mutex> lock(g_PerEquipIconPathsMutex);
    g_PerEquipIconPaths.erase(equipId);
}


void EquipIcon_ClearAllIconFtexPaths()
{
    std::lock_guard<std::mutex> lock(g_PerEquipIconPathsMutex);
    g_PerEquipIconPaths.clear();
}


#ifdef _DEBUG
static std::atomic<int> g_IconProbeRemaining{0};
#endif

void EquipIcon_DebugArmProbe(int calls)
{
#ifdef _DEBUG
    g_IconProbeRemaining.store(calls, std::memory_order_relaxed);
    LogDebug("[IconProbe] armed for next %d GetIconFtexPath calls\n", calls);
#else
    (void)calls;
#endif
}

static std::int64_t* __fastcall hkGetIconFtexPath(std::int64_t* outPathId, std::uint32_t equipId, int mode)
{
    {
        std::lock_guard<std::mutex> lock(g_PerEquipIconPathsMutex);
        const auto it = g_PerEquipIconPaths.find(static_cast<int>(equipId));
        if (it != g_PerEquipIconPaths.end())
        {
            *outPathId = static_cast<std::int64_t>(it->second);
#ifdef _DEBUG
            if (g_IconProbeRemaining.fetch_sub(1, std::memory_order_relaxed) > 0)
                LogDebug("[IconProbe] equipId=%u mode=%d -> OVERRIDE hash=0x%016llX\n",
                    equipId, mode,
                    static_cast<unsigned long long>(it->second));
            else
                g_IconProbeRemaining.store(0, std::memory_order_relaxed);
#endif
            return outPathId;
        }
    }

    std::int64_t* r = g_OrigGetIconFtexPath(outPathId, equipId, mode);
#ifdef _DEBUG
    if (g_IconProbeRemaining.fetch_sub(1, std::memory_order_relaxed) > 0)
        LogDebug("[IconProbe] equipId=%u mode=%d -> orig hash=0x%016llX\n",
            equipId, mode,
            r ? static_cast<unsigned long long>(*r) : 0ull);
    else
        g_IconProbeRemaining.store(0, std::memory_order_relaxed);
#endif
    return r;
}


bool Install_EquipIconFtexPath_Hook()
{
    if (g_EquipIconFtexPathHookInstalled)
    {
        return true;
    }

    void* target = ResolveGameAddress(gAddr.GetIconFtexPath);
    if (!target)
    {
        Log("[EquipIcon] target resolve failed\n");
        return false;
    }

    if (!CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGetIconFtexPath),
        reinterpret_cast<void**>(&g_OrigGetIconFtexPath)))
    {
        Log("[EquipIcon] hook install failed\n");
        return false;
    }

    g_EquipIconFtexPathHookInstalled = true;
#ifdef _DEBUG
    LogDebug("[EquipIcon] hook installed\n");
#endif
    return true;
}


bool Uninstall_EquipIconFtexPath_Hook()
{
    if (!g_EquipIconFtexPathHookInstalled)
        return true;

    void* target = ResolveGameAddress(gAddr.GetIconFtexPath);
    if (target)
        DisableAndRemoveHook(target);

    g_OrigGetIconFtexPath = nullptr;
    g_EquipIconFtexPathHookInstalled = false;

#ifdef _DEBUG
    LogDebug("[EquipIcon] hook removed\n");
#endif
    return true;
}
