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
    using SightNotice2_t = void(__fastcall*)(void* self, std::uint32_t soldierIndex);
    using FilterNoise_t  = void(__fastcall*)(void* self, std::uint32_t soldierIndex, void* a3);

    std::mutex                                    g_mtx;
    std::array<std::uint8_t, kSoldierSlotCount>   g_ignoreMask{};

    SightNotice4_t g_OrigPlayer       = nullptr;
    SightNotice2_t g_OrigNoticeObject = nullptr;
    SightNotice2_t g_OrigCBox         = nullptr;
    FilterNoise_t  g_OrigNoise        = nullptr;

    void* g_PlayerTarget       = nullptr;
    void* g_NoticeObjectTarget = nullptr;
    void* g_CBoxTarget         = nullptr;
    void* g_NoiseTarget        = nullptr;

    std::uintptr_t Addr_Player()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x1414E0FC0ull;
        default:               return 0;
        }
    }

    std::uintptr_t Addr_NoticeObject()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x1414E08F0ull;
        default:               return 0;
        }
    }

    std::uintptr_t Addr_CBox()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x1414DF100ull;
        default:               return 0;
        }
    }

    std::uintptr_t Addr_Noise()
    {
        using GB = AddressSetRuntime::GameBuild;
        switch (gGameBuild)
        {
        case GB::En_1_0_15_4:
        case GB::En_1_0_15_4a: return 0x1414E4B90ull;
        default:               return 0;
        }
    }

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

    void __fastcall hk_NoticeObject(void* self, std::uint32_t soldierIndex)
    {
        if (!g_OrigNoticeObject)
            return;
        if (Soldier_IgnoresNotice(soldierIndex, SoldierNoticeIgnore::kNoticeObject))
            return;
        g_OrigNoticeObject(self, soldierIndex);
    }

    void __fastcall hk_CBox(void* self, std::uint32_t soldierIndex)
    {
        if (!g_OrigCBox)
            return;
        if (Soldier_IgnoresNotice(soldierIndex, SoldierNoticeIgnore::kCBox))
            return;
        g_OrigCBox(self, soldierIndex);
    }

    void __fastcall hk_Noise(void* self, std::uint32_t soldierIndex, void* a3)
    {
        if (!g_OrigNoise)
            return;
        if (Soldier_IgnoresNotice(soldierIndex, SoldierNoticeIgnore::kNoise))
            return;
        g_OrigNoise(self, soldierIndex, a3);
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
        Log("[SoldierNoticeIgnore] ERROR: the soldier sight-notice addresses are missing for this "
            "build - SetRestrictNotice ignorePlayer/ignoreHostage/ignoreNoticeObject/ignoreCBox/"
            "ignoreNoise will be accepted but do nothing, so those soldiers keep noticing "
            "normally.\n");
    }

    InstallOne(Addr_Player(), reinterpret_cast<void*>(&hk_Player),
               reinterpret_cast<void**>(&g_OrigPlayer), &g_PlayerTarget,
               "CheckSightNoticePlayer (ignorePlayer)");
    InstallOne(Addr_NoticeObject(), reinterpret_cast<void*>(&hk_NoticeObject),
               reinterpret_cast<void**>(&g_OrigNoticeObject), &g_NoticeObjectTarget,
               "CheckSightNoticeNoticeObject (ignoreNoticeObject)");
    InstallOne(Addr_CBox(), reinterpret_cast<void*>(&hk_CBox),
               reinterpret_cast<void**>(&g_OrigCBox), &g_CBoxTarget,
               "CheckSightNoticeCBox (ignoreCBox)");
    InstallOne(Addr_Noise(), reinterpret_cast<void*>(&hk_Noise),
               reinterpret_cast<void**>(&g_OrigNoise), &g_NoiseTarget,
               "FilterSpecificNoise (ignoreNoise)");

    return true;
}

bool Uninstall_CheckSightNoticePlayer_Hook()
{
    if (g_PlayerTarget)       DisableAndRemoveHook(g_PlayerTarget);
    if (g_NoticeObjectTarget) DisableAndRemoveHook(g_NoticeObjectTarget);
    if (g_CBoxTarget)         DisableAndRemoveHook(g_CBoxTarget);
    if (g_NoiseTarget)        DisableAndRemoveHook(g_NoiseTarget);

    g_PlayerTarget = g_NoticeObjectTarget = g_CBoxTarget = g_NoiseTarget = nullptr;
    g_OrigPlayer = nullptr;
    g_OrigNoticeObject = nullptr;
    g_OrigCBox = nullptr;
    g_OrigNoise = nullptr;

    Clear_AllSoldierIgnorePlayer();
    return true;
}
