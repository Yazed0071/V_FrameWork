#include "pch.h"
#include "MissionEmergencyHook.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "HookUtils.h"
#include "log.h"
#include "AddressSet.h"

namespace
{
    using GetMissionCodeCategory_t = std::uint8_t (__fastcall*)(std::uint16_t missionCode);

    static GetMissionCodeCategory_t g_OrigGetMissionCodeCategory = nullptr;

    static std::mutex                       g_AllowlistMutex;
    static std::unordered_set<std::uint16_t> g_EmergencyMissionAllowlist;

    static bool g_MissionEmergencyHookInstalled = false;

    static constexpr std::uint8_t kEmergencyCategory = 4;
}


static std::uint8_t __fastcall hkGetMissionCodeCategory(std::uint16_t missionCode)
{
    bool allowlistHit = false;
    {
        std::lock_guard<std::mutex> lock(g_AllowlistMutex);
        allowlistHit = (g_EmergencyMissionAllowlist.find(missionCode) !=
                        g_EmergencyMissionAllowlist.end());
    }

    if (allowlistHit)
        return kEmergencyCategory;

    if (!g_OrigGetMissionCodeCategory)
        return 0;

    return g_OrigGetMissionCodeCategory(missionCode);
}


void MissionEmergency_SetEnabled(std::uint16_t missionCode, bool enabled)
{
    std::lock_guard<std::mutex> lock(g_AllowlistMutex);

    if (enabled)
    {
        const auto inserted = g_EmergencyMissionAllowlist.insert(missionCode);
    }
    else
    {
        const auto erased = g_EmergencyMissionAllowlist.erase(missionCode);
    }
}


bool MissionEmergency_IsEnabled(std::uint16_t missionCode)
{
    std::lock_guard<std::mutex> lock(g_AllowlistMutex);
    return g_EmergencyMissionAllowlist.find(missionCode) != g_EmergencyMissionAllowlist.end();
}


void MissionEmergency_ClearAll()
{
    std::lock_guard<std::mutex> lock(g_AllowlistMutex);
    if (!g_EmergencyMissionAllowlist.empty())
    {
        g_EmergencyMissionAllowlist.clear();
    }
}


bool Install_MissionEmergency_Hook()
{
    if (g_MissionEmergencyHookInstalled)
    {
        return true;
    }

    if (!gAddr.GetMissionCodeCategory)
    {
        LogDebug("[MissionEmergency] GetMissionCodeCategory address is 0 for this build; skipping\n");
        return false;
    }

    void* target = ResolveGameAddress(gAddr.GetMissionCodeCategory);
    if (!target)
    {
        Log("[MissionEmergency] target resolve failed\n");
        return false;
    }

    if (!CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGetMissionCodeCategory),
        reinterpret_cast<void**>(&g_OrigGetMissionCodeCategory)))
    {
        Log("[MissionEmergency] hook install failed\n");
        return false;
    }

    g_MissionEmergencyHookInstalled = true;
#ifdef _DEBUG
    LogDebug("[MissionEmergency] hook installed at 0x%llX\n",
        static_cast<unsigned long long>(gAddr.GetMissionCodeCategory));
#endif
    return true;
}


bool Uninstall_MissionEmergency_Hook()
{
    if (!g_MissionEmergencyHookInstalled)
        return true;

    if (gAddr.GetMissionCodeCategory)
    {
        void* target = ResolveGameAddress(gAddr.GetMissionCodeCategory);
        if (target)
            DisableAndRemoveHook(target);
    }

    g_OrigGetMissionCodeCategory = nullptr;
    g_MissionEmergencyHookInstalled = false;

    MissionEmergency_ClearAll();

#ifdef _DEBUG
    LogDebug("[MissionEmergency] hook removed\n");
#endif
    return true;
}
