#include "pch.h"

#include <Windows.h>
#include <array>
#include <cstdint>
#include <mutex>

#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "NoticeControllerImpl_CheckSightNoticePlayer.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kSoldierIndexMask = 0x1FFu;
    constexpr std::size_t   kSoldierSlotCount = 0x200u;

    using SightNotice4_t = void(__fastcall*)(void* self, std::uint32_t soldierIndex,
                                             void* a3, void* a4);

    std::mutex                                    g_mtx;
    std::array<std::uint8_t, kSoldierSlotCount>   g_ignoreMask{};

    SightNotice4_t g_OrigPlayer = nullptr;

    void* g_PlayerTarget = nullptr;

    std::uintptr_t Addr_Player() { return gAddr.NoticeControllerImpl_CheckSightNoticePlayer; }

    thread_local int t_suppressPlayerSight = 0;

    void __fastcall hk_Player(void* self, std::uint32_t soldierIndex, void* a3, void* a4)
    {
        if (!g_OrigPlayer)
            return;

        if (!Soldier_IgnoresNotice(soldierIndex, SoldierNoticeIgnore::kPlayer))
        {
            g_OrigPlayer(self, soldierIndex, a3, a4);
            return;
        }

        ++t_suppressPlayerSight;
        g_OrigPlayer(self, soldierIndex, a3, a4);
        --t_suppressPlayerSight;
    }

    bool InstallOne(std::uintptr_t addr, void* detour, void** orig, void** targetOut,
                    const char* what)
    {
        if (!addr)
            return false;

        void* target = ResolveGameAddress(addr);
        if (!target || !CreateAndEnableHook(target, detour, orig))
        {
            Log("[SoldierNoticeIgnore] ERROR: %s hook failed - the matching SetRestrictNotice "
                "ignore field will be accepted but do nothing for every soldier.\n", what);
            return false;
        }

        *targetOut = target;
        return true;
    }
}

void Set_SoldierNoticeIgnoreMask(std::uint32_t gameObjectId, std::uint8_t mask)
{
    const std::uint32_t slot = gameObjectId & kSoldierIndexMask;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_ignoreMask[slot] = mask;
}

bool SoldierNotice_ShouldDropNotice(std::uint32_t soldierIndex, std::uint8_t noticeType,
                                    const void* returnAddress)
{
    UNREFERENCED_PARAMETER(noticeType);
    UNREFERENCED_PARAMETER(returnAddress);

    if (t_suppressPlayerSight > 0)
        return true;

    return Soldier_IgnoresNotice(soldierIndex, SoldierNoticeIgnore::kPlayer);
}

bool Soldier_IgnoresNotice(std::uint32_t soldierIndex, std::uint8_t flag)
{
    const std::uint32_t slot = soldierIndex & kSoldierIndexMask;
    std::lock_guard<std::mutex> lk(g_mtx);
    return (g_ignoreMask[slot] & flag) != 0;
}

void Clear_AllSoldierIgnorePlayer()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_ignoreMask.fill(0);
}

bool Install_CheckSightNoticePlayer_Hook()
{
    if (!Addr_Player())
    {
        Log("[SoldierNoticeIgnore] ERROR: CheckSightNoticePlayer is not ported for this "
            "build - SetRestrictNotice ignorePlayer is accepted but does nothing, so "
            "those soldiers still spot the player\n");
    }

    InstallOne(Addr_Player(), reinterpret_cast<void*>(&hk_Player),
               reinterpret_cast<void**>(&g_OrigPlayer), &g_PlayerTarget,
               "CheckSightNoticePlayer (ignorePlayer)");

    return true;
}

bool Uninstall_CheckSightNoticePlayer_Hook()
{
    if (g_PlayerTarget)
        DisableAndRemoveHook(g_PlayerTarget);

    g_PlayerTarget = nullptr;
    g_OrigPlayer = nullptr;

    Clear_AllSoldierIgnorePlayer();
    return true;
}
