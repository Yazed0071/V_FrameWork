#include "pch.h"
#include "SearchLightActionPluginImpl_StateDoor.h"

#include <Windows.h>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "LuaBroadcaster.h"
#include "log.h"
#include "MissionCodeGuard.h"

namespace
{
    using StateDoor_t = void(__fastcall*)(void*, std::uint32_t, std::uint32_t, void*);

    static StateDoor_t g_OrigStart = nullptr;
    static StateDoor_t g_OrigEnd   = nullptr;

    static bool ReadDoor(void* self, std::uint32_t playerIndex,
        bool& started, std::uint16_t& gimmickId, std::uint32_t& doorSide)
    {
        started = false; gimmickId = 0; doorSide = 0;
        if (!self) return false;

        __try
        {
            const auto base   = reinterpret_cast<std::uintptr_t>(self);
            const auto info   = *reinterpret_cast<std::uintptr_t*>(base + 0x38);
            const auto slots  = *reinterpret_cast<std::uintptr_t*>(base + 0x78);
            if (!info || !slots) return false;

            const auto origin = *reinterpret_cast<std::uint32_t*>(info + 0x24);
            if (playerIndex < origin) return false;

            const auto work = slots + static_cast<std::uintptr_t>(playerIndex - origin) * 0x480;
            gimmickId = *reinterpret_cast<std::uint16_t*>(work + 0x16);
            doorSide  = (*reinterpret_cast<std::uint8_t*>(work + 0x478) >> 2) & 1u;
            started   = (*reinterpret_cast<std::uint8_t*>(work + 0x46c) & 1u) != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Started bit (work+0x46c bit0) goes 0 -> 1 when the lockpick begins.
    static void __fastcall hkStateDoorStart(void* self, std::uint32_t playerIndex, std::uint32_t proc, void* param3)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigStart, self, playerIndex, proc, param3);

        bool was = false; std::uint16_t gid; std::uint32_t side;
        const bool ok0 = ReadDoor(self, playerIndex, was, gid, side);

        if (g_OrigStart) g_OrigStart(self, playerIndex, proc, param3);

        bool now = false;
        const bool ok1 = ReadDoor(self, playerIndex, now, gid, side);
        if (ok0 && ok1 && !was && now)
            V_FrameWork::EmitMessage("Player", "OnPlayerLockPickStart", playerIndex, gid, side);
    }

    // Started bit goes 1 -> 0 when the lockpick ends.
    static void __fastcall hkStateDoorEnd(void* self, std::uint32_t playerIndex, std::uint32_t proc, void* param3)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigEnd, self, playerIndex, proc, param3);

        bool was = false; std::uint16_t gid; std::uint32_t side;
        const bool ok0 = ReadDoor(self, playerIndex, was, gid, side);

        if (g_OrigEnd) g_OrigEnd(self, playerIndex, proc, param3);

        bool now = false; std::uint16_t gid1; std::uint32_t side1;
        const bool ok1 = ReadDoor(self, playerIndex, now, gid1, side1);
        if (ok0 && ok1 && was && !now)
            V_FrameWork::EmitMessage("Player", "OnPlayerLockPickEnd", playerIndex, gid, side);
    }
}

bool Install_SearchLightActionPluginImpl_StateDoor_Hook()
{
    if (!gAddr.SearchLightActionPluginImpl_StateDoorStart ||
        !gAddr.SearchLightActionPluginImpl_StateDoorEnd)
    {
        LogDebug("[LockPick] addresses not set for this build -- skipped\n");
        return false;
    }

    void* startTarget = ResolveGameAddress(gAddr.SearchLightActionPluginImpl_StateDoorStart);
    void* endTarget   = ResolveGameAddress(gAddr.SearchLightActionPluginImpl_StateDoorEnd);
    if (!startTarget || !endTarget)
    {
        Log("[LockPick] resolve failed (start=%p end=%p)\n", startTarget, endTarget);
        return false;
    }

    const bool startOk = CreateAndEnableHook(
        startTarget, reinterpret_cast<void*>(&hkStateDoorStart), reinterpret_cast<void**>(&g_OrigStart));
    const bool endOk = CreateAndEnableHook(
        endTarget, reinterpret_cast<void*>(&hkStateDoorEnd), reinterpret_cast<void**>(&g_OrigEnd));

#ifdef _DEBUG
    Log("[LockPick] StateDoorStart:%s StateDoorEnd:%s\n", startOk ? "OK" : "FAIL", endOk ? "OK" : "FAIL");
#else
    if (!startOk || !endOk)
        Log("[LockPick] StateDoorStart:%s StateDoorEnd:%s\n", startOk ? "OK" : "FAIL", endOk ? "OK" : "FAIL");
#endif
    return startOk && endOk;
}

bool Uninstall_SearchLightActionPluginImpl_StateDoor_Hook()
{
    if (gAddr.SearchLightActionPluginImpl_StateDoorStart)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.SearchLightActionPluginImpl_StateDoorStart));
    if (gAddr.SearchLightActionPluginImpl_StateDoorEnd)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.SearchLightActionPluginImpl_StateDoorEnd));

    g_OrigStart = nullptr;
    g_OrigEnd   = nullptr;
    return true;
}
