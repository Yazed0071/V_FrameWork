#include "pch.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "AddressSet.h"
#include "HookUtils.h"
#include "SoldierVehicleAvoid.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kSoldierIndexMask          = 0x1FFu;
    constexpr std::size_t   kSoldierSlotCount          = 0x200u;
    constexpr std::uint8_t  kNoticeTypeVehicleApproach = 3u;
    constexpr std::uint64_t kDropLogIntervalMs         = 5000u;

    std::mutex                                   g_mtx;
    std::array<std::uint8_t, kSoldierSlotCount>  g_ignore{};
    std::array<std::uint64_t, kSoldierSlotCount> g_lastDropLogMs{};
    std::atomic<bool>                            g_installed{ false };

    bool ReadNoticeVehicleId(const void* blob, std::uint16_t* outId)
    {
        __try
        {
            *outId = *reinterpret_cast<const std::uint16_t*>(
                reinterpret_cast<const std::uint8_t*>(blob) + 2u);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ShouldLogDrop(std::uint32_t slot)
    {
        const std::uint64_t now = GetTickCount64();
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_lastDropLogMs[slot] != 0 && now - g_lastDropLogMs[slot] < kDropLogIntervalMs)
            return false;
        g_lastDropLogMs[slot] = now;
        return true;
    }
}

void Set_SoldierIgnoreVehicle(std::uint32_t gameObjectId, bool enabled)
{
    const std::uint32_t slot = gameObjectId & kSoldierIndexMask;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_ignore[slot] = enabled ? 1u : 0u;
    }
    LogDebug("[SoldierIgnoreVehicle] soldier 0x%04X (slot %u) ignoreVehicle=%s\n",
             gameObjectId, slot, enabled ? "on" : "off");
}

bool Soldier_IgnoresVehicle(std::uint32_t soldierIndex)
{
    const std::uint32_t slot = soldierIndex & kSoldierIndexMask;
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_ignore[slot] != 0u;
}

bool SoldierVehicleAvoid_ShouldDropNotice(std::uint32_t soldierIndex, std::uint8_t noticeType,
                                          const void* noticeBlob)
{
    if (noticeType != kNoticeTypeVehicleApproach || !noticeBlob)
        return false;
    if (!g_installed.load(std::memory_order_relaxed))
        return false;
    if (!Soldier_IgnoresVehicle(soldierIndex))
        return false;

    const std::uint32_t slot = soldierIndex & kSoldierIndexMask;
    if (ShouldLogDrop(slot))
    {
        std::uint16_t vehicleId = 0xFFFFu;
        if (!ReadNoticeVehicleId(noticeBlob, &vehicleId))
            vehicleId = 0xFFFFu;
        LogDebug("[SoldierIgnoreVehicle] soldier slot %u ignores vehicle 0x%04X approaching\n",
                 slot, vehicleId);
    }
    return true;
}

void Clear_AllSoldierIgnoreVehicle()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_ignore.fill(0u);
    g_lastDropLogMs.fill(0u);
}

bool Install_SoldierVehicleAvoid_Hook()
{
    if (!ResolveGameAddress(gAddr.AddNoticeInfo))
    {
        Log("[SoldierIgnoreVehicle] ERROR: AddNoticeInfo is missing for this build - the shared "
            "notice hook that SetIgnoreVehicle relies on cannot exist, so flagged soldiers still "
            "step off the road for vehicles\n");
        return false;
    }

    g_installed.store(true, std::memory_order_relaxed);
    LogDebug("[SoldierIgnoreVehicle] ready\n");
    return true;
}

bool Uninstall_SoldierVehicleAvoid_Hook()
{
    g_installed.store(false, std::memory_order_relaxed);
    Clear_AllSoldierIgnoreVehicle();
    return true;
}
