#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "AddressSet.h"
#include "FoxHashes.h"
#include "HookUtils.h"
#include "GetGameObjectIdWithIndex.h"
#include "MissionCodeGuard.h"
#include "PlayerAttachInDemo.h"
#include "log.h"

namespace
{
    using OnPlayingAfterUpdateStream_t = void(__fastcall*)(void* playback);
    using DemoState_t  = std::uint32_t(__fastcall*)(void* playback);
    using QuarkTable_t = std::uint8_t* (__fastcall*)();
    using GetCommunication_t = void* (__fastcall*)(void* self, std::int32_t* err, void** out,
                                                   std::uint16_t instanceIndex);
    using GetVehicleCnpWorld_t = void* (__fastcall*)(void* info, std::int32_t* err,
                                                     float* outMatrix, std::uint32_t cnpHash);
    using CheckStatusBit_t = bool(__fastcall*)(std::uint32_t statusId);
    using DemoActionExecute_t = void(__fastcall*)(void* self, std::uint32_t phase,
                                                 std::uint32_t gameObjectIndex);
    using UpdateStreamMotion_t = void(__fastcall*)(void* animController, std::uint32_t index,
                                                   const void* transformRT);
    using IsPlaying_t = bool(__fastcall*)(void* demoAnimController, std::uint32_t index);
    using Warp_t = void(__fastcall*)(void* self, std::uint32_t instanceIndex,
                                     const float* pos, const float* rot, bool waitForBlock);

    constexpr std::uint32_t kInvalidId         = 0xFFFFu;
    constexpr std::uint32_t kInstanceMask      = 0x1FFu;
    constexpr std::size_t   kQuarkAppSystem    = 0x98;
    constexpr std::size_t   kAppVehicleSystem  = 0x2A8;
    constexpr std::size_t   kGetCommunicationSlot = 0x18;
    constexpr std::size_t   kPlayerSubObject   = 0x20;
    constexpr std::size_t   kPlayerWarpSlot    = 0x1B0;
    constexpr std::uint32_t kStatusUnconscious = 0x67;
    const char* const       kVehicleTypeName   = "TppVehicle2";

    constexpr std::uint32_t kDemoPhaseAnimation      = 2;
    constexpr std::ptrdiff_t kExecOwner              = -0x28;
    constexpr std::size_t   kExecDemoAnimController  = 0x18;
    constexpr std::size_t   kOwnerAnimHolder         = 0x78;
    constexpr std::size_t   kAnimHolderController    = 0x238;
    constexpr std::size_t   kOwnerPlayerSub          = 0x138;
    constexpr std::size_t   kPlayerSubLocalIndex     = 0x218;
    constexpr std::size_t   kDemoAnimIsPlayingSlot   = 0x18;
    constexpr std::size_t   kUpdateStreamMotionSlot  = 0x330;

    OnPlayingAfterUpdateStream_t g_OrigOnPlayingAfterUpdateStream = nullptr;
    DemoState_t                  g_OrigDoFinish    = nullptr;
    DemoState_t                  g_OrigDoInterrupt = nullptr;
    void*                        g_HookTarget      = nullptr;
    void*                        g_HookFinish      = nullptr;
    void*                        g_HookInterrupt   = nullptr;
    DemoActionExecute_t          g_OrigDemoActionExecute = nullptr;
    void*                        g_HookDemoExecute = nullptr;

    std::mutex        g_Mutex;
    std::string       g_OwnerName;
    std::uint32_t     g_OwnerObjectId   = kInvalidId;
    std::uint32_t     g_ConnectPointId  = 0;
    bool              g_UnattachOnSleep = false;
    std::atomic<bool> g_Active{ false };

    std::atomic<void*> g_PlayerImpl{ nullptr };

    bool g_ReportedNoMatrix = false;
    bool g_ReportedHookLive = false;
    bool g_ReportedAttached = false;
    bool g_ReportedPlaced = false;

    bool PlayerIsUnconscious()
    {
        if (!gAddr.PlayerInfo_CheckStatusBit)
            return false;

        auto fn = reinterpret_cast<CheckStatusBit_t>(
            ResolveGameAddress(gAddr.PlayerInfo_CheckStatusBit));
        if (!fn)
            return false;

        __try
        {
            return fn(kStatusUnconscious);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void* GetVehicleSystem()
    {
        if (!gAddr.Fox_GetQuarkSystemTable)
            return nullptr;

        auto qst = reinterpret_cast<QuarkTable_t>(
            ResolveGameAddress(gAddr.Fox_GetQuarkSystemTable));
        if (!qst)
            return nullptr;

        __try
        {
            std::uint8_t* table = qst();
            if (!table)
                return nullptr;

            std::uint8_t* app = *reinterpret_cast<std::uint8_t**>(table + kQuarkAppSystem);
            if (!app)
                return nullptr;

            return *reinterpret_cast<void**>(app + kAppVehicleSystem);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ReadConnectPointMatrix(std::uint32_t ownerObjectId, std::uint32_t cnpHash,
                                float* outMatrix16)
    {
        void* sys = GetVehicleSystem();
        if (!sys)
            return false;

        auto cnpWorld = reinterpret_cast<GetVehicleCnpWorld_t>(
            ResolveGameAddress(gAddr.Vehicle_GetConnectPointWorldMatrix));
        if (!cnpWorld)
            return false;

        __try
        {
            void** sysVtbl = *reinterpret_cast<void***>(sys);
            if (!sysVtbl)
                return false;

            auto getCommunication = reinterpret_cast<GetCommunication_t>(
                sysVtbl[kGetCommunicationSlot / sizeof(void*)]);
            if (!getCommunication)
                return false;

            std::int32_t err  = -1;
            void*        info = nullptr;
            getCommunication(sys, &err, &info,
                             static_cast<std::uint16_t>(ownerObjectId & kInstanceMask));
            if (err < 0 || !info)
                return false;

            std::int32_t cnpErr = -1;
            cnpWorld(info, &cnpErr, outMatrix16, cnpHash);
            return cnpErr >= 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WarpPlayerTo(const float* matrix16)
    {
        void* impl = g_PlayerImpl.load(std::memory_order_relaxed);
        if (!impl)
            return false;

        __try
        {
            auto* sub = reinterpret_cast<std::uint8_t*>(impl) + kPlayerSubObject;
            auto** vtbl = *reinterpret_cast<void***>(sub);
            if (!vtbl)
                return false;

            auto warp = reinterpret_cast<Warp_t>(vtbl[kPlayerWarpSlot / sizeof(void*)]);
            if (!warp)
                return false;

            alignas(16) float pos[4] = { matrix16[12], matrix16[13], matrix16[14], 1.0f };

            const float yaw  = std::atan2(matrix16[8], matrix16[10]);
            const float half = yaw * 0.5f;
            alignas(16) float rot[4] = { 0.0f, std::sin(half), 0.0f, std::cos(half) };

            warp(sub, 0, pos, rot, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void __fastcall hkOnPlayingAfterUpdateStream(void* playback)
    {
        if (g_OrigOnPlayingAfterUpdateStream)
            g_OrigOnPlayingAfterUpdateStream(playback);

        if (!g_Active.load(std::memory_order_relaxed))
            return;

        if (MissionCodeGuard::ShouldBypassHooks())
            return;

        if (!g_ReportedHookLive)
        {
            g_ReportedHookLive = true;
            Log("[AttachInDemo] the demo per-frame hook is live with an attach armed\n");
        }

        std::uint32_t ownerObjectId;
        std::uint32_t cnpHash;
        bool          unattachOnSleep;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ownerObjectId   = g_OwnerObjectId;
            cnpHash         = g_ConnectPointId;
            unattachOnSleep = g_UnattachOnSleep;
        }

        if (ownerObjectId == kInvalidId)
            return;

        if (unattachOnSleep && PlayerIsUnconscious())
        {
            ClearAttachInDemo();
            return;
        }

        alignas(16) float matrix[16] = {};
        if (!ReadConnectPointMatrix(ownerObjectId, cnpHash, matrix))
        {
            if (!g_ReportedNoMatrix)
            {
                g_ReportedNoMatrix = true;
                Log("[AttachInDemo] '%s' has no reachable connect point this frame, so the player "
                    "is not being attached - the vehicle may be unrealized, or the connect-point "
                    "name may not exist on its model\n", g_OwnerName.c_str());
            }
            return;
        }

        const bool warped = WarpPlayerTo(matrix);

        if (warped && !g_ReportedAttached)
        {
            g_ReportedAttached = true;
            Log("[AttachInDemo] '%s' connect point reached and the player was moved to it - if the "
                "player only appears there when the demo ends, the demo is re-driving the player "
                "transform after this write\n", g_OwnerName.c_str());
        }
    }

    void PlaceDemoActor(void* self, std::uint32_t index, const float* matrix16)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(self);

            void* owner = *reinterpret_cast<void**>(base + kExecOwner);
            void* demoAnimCtl = *reinterpret_cast<void**>(base + kExecDemoAnimController);
            if (!owner || !demoAnimCtl)
                return;

            auto* ownerBytes = reinterpret_cast<std::uint8_t*>(owner);

            void* animHolder = *reinterpret_cast<void**>(ownerBytes + kOwnerAnimHolder);
            void* playerSub  = *reinterpret_cast<void**>(ownerBytes + kOwnerPlayerSub);
            if (!animHolder || !playerSub)
                return;

            void* animCtl = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(animHolder) + kAnimHolderController);
            if (!animCtl)
                return;

            const std::uint32_t localIndex = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(playerSub) + kPlayerSubLocalIndex);
            if (index != localIndex)
                return;

            auto** demoVtbl = *reinterpret_cast<void***>(demoAnimCtl);
            if (!demoVtbl)
                return;

            auto isPlaying = reinterpret_cast<IsPlaying_t>(
                demoVtbl[kDemoAnimIsPlayingSlot / sizeof(void*)]);
            if (!isPlaying || !isPlaying(demoAnimCtl, index))
                return;

            auto** animVtbl = *reinterpret_cast<void***>(animCtl);
            if (!animVtbl)
                return;

            auto updateStreamMotion = reinterpret_cast<UpdateStreamMotion_t>(
                animVtbl[kUpdateStreamMotionSlot / sizeof(void*)]);
            if (!updateStreamMotion)
                return;

            const float yaw  = std::atan2(matrix16[8], matrix16[10]);
            const float half = yaw * 0.5f;

            alignas(16) float transformRT[12] = {};
            transformRT[0]  = 1.0f;
            transformRT[1]  = 1.0f;
            transformRT[2]  = 1.0f;
            transformRT[3]  = 1.0f;
            transformRT[4]  = 0.0f;
            transformRT[5]  = std::sin(half);
            transformRT[6]  = 0.0f;
            transformRT[7]  = std::cos(half);
            transformRT[8]  = matrix16[12];
            transformRT[9]  = matrix16[13];
            transformRT[10] = matrix16[14];
            transformRT[11] = 1.0f;

            updateStreamMotion(animCtl, index, transformRT);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void __fastcall hkSequentialDemoActionExecute(void* self, std::uint32_t phase,
                                                  std::uint32_t gameObjectIndex)
    {
        if (g_OrigDemoActionExecute)
            g_OrigDemoActionExecute(self, phase, gameObjectIndex);

        if (phase != kDemoPhaseAnimation || !g_Active.load(std::memory_order_relaxed))
            return;

        if (MissionCodeGuard::ShouldBypassHooks())
            return;

        std::uint32_t ownerObjectId;
        std::uint32_t cnpHash;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ownerObjectId = g_OwnerObjectId;
            cnpHash       = g_ConnectPointId;
        }

        if (ownerObjectId == kInvalidId)
            return;

        alignas(16) float matrix[16] = {};
        if (!ReadConnectPointMatrix(ownerObjectId, cnpHash, matrix))
            return;

        PlaceDemoActor(self, gameObjectIndex, matrix);

        if (!g_ReportedPlaced)
        {
            g_ReportedPlaced = true;
            Log("[AttachInDemo] '%s' is being placed through the demo's own stream-motion slot - "
                "the player should now sit on the connect point during the cutscene, not only "
                "after it\n", g_OwnerName.c_str());
        }
    }

    std::uint32_t __fastcall hkDoFinish(void* playback)
    {
        ClearAttachInDemo();
        return g_OrigDoFinish ? g_OrigDoFinish(playback) : 0;
    }

    std::uint32_t __fastcall hkDoInterrupt(void* playback)
    {
        ClearAttachInDemo();
        return g_OrigDoInterrupt ? g_OrigDoInterrupt(playback) : 0;
    }
}

void Note_PlayerGameObjectImpl(void* impl)
{
    if (impl)
        g_PlayerImpl.store(impl, std::memory_order_relaxed);
}

bool RequestToAttachInDemoById(std::uint32_t ownerObjectId, const char* connectPointName,
                               bool unattachOnSleep)
{
    if (ownerObjectId == kInvalidId || !connectPointName || !*connectPointName)
        return false;

    char label[32] = {};
    std::snprintf(label, sizeof(label), "0x%04X", ownerObjectId);

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_OwnerName       = label;
        g_OwnerObjectId   = ownerObjectId;
        g_ConnectPointId  = FoxHashes::StrCode32(connectPointName);
        g_UnattachOnSleep = unattachOnSleep;
    }

    g_ReportedNoMatrix  = false;
    g_ReportedHookLive  = false;
    g_ReportedAttached  = false;
    g_ReportedPlaced    = false;
    g_Active.store(true, std::memory_order_relaxed);

    Log("[AttachInDemo] armed: owner=%s connectPoint hash=0x%08X unattachOnSleep=%d\n",
        label, g_ConnectPointId, unattachOnSleep ? 1 : 0);
    return true;
}

bool RequestToAttachInDemo(const char* ownerName, const char* connectPointName,
                           bool unattachOnSleep)
{
    if (!ownerName || !*ownerName || !connectPointName || !*connectPointName)
        return false;

    std::uint32_t ownerObjectId = kInvalidId;
    if (!GetGameObjectIdByName(kVehicleTypeName, ownerName, ownerObjectId))
    {
        Log("[AttachInDemo] '%s' did not resolve to a %s game object, so nothing was attached - "
            "pass the vehicle's game object id instead if it has no locator name\n",
            ownerName, kVehicleTypeName);
        return false;
    }

    if (!RequestToAttachInDemoById(ownerObjectId, connectPointName, unattachOnSleep))
        return false;

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_OwnerName = ownerName;
    }
    return true;
}

void ClearAttachInDemo()
{
    g_Active.store(false, std::memory_order_relaxed);
}

bool Install_PlayerAttachInDemo_Hook()
{
    if (g_HookTarget)
        return true;

    if (!gAddr.DemoPlayback_OnPlayingAfterUpdateStream
        || !gAddr.Vehicle_GetConnectPointWorldMatrix)
    {
        LogDebug("[AttachInDemo] the demo and vehicle addresses are not ported for this build, so "
                 "V_Player.RequestToAttachInDemo is refused and the player is never attached\n");
        return true;
    }

    void* target = ResolveGameAddress(gAddr.DemoPlayback_OnPlayingAfterUpdateStream);
    if (!target)
        return false;

    if (!CreateAndEnableHook(target, &hkOnPlayingAfterUpdateStream,
                             reinterpret_cast<void**>(&g_OrigOnPlayingAfterUpdateStream)))
    {
        Log("[AttachInDemo] the demo per-frame hook did not install, so "
            "V_Player.RequestToAttachInDemo is accepted but the player is never attached\n");
        return false;
    }

    g_HookTarget = target;

    if (gAddr.DemoPlayback_DoFinish)
    {
        void* finish = ResolveGameAddress(gAddr.DemoPlayback_DoFinish);
        if (finish && CreateAndEnableHook(finish, &hkDoFinish,
                                          reinterpret_cast<void**>(&g_OrigDoFinish)))
            g_HookFinish = finish;
    }

    if (gAddr.DemoPlayback_DoInterrupt)
    {
        void* interrupt = ResolveGameAddress(gAddr.DemoPlayback_DoInterrupt);
        if (interrupt && CreateAndEnableHook(interrupt, &hkDoInterrupt,
                                             reinterpret_cast<void**>(&g_OrigDoInterrupt)))
            g_HookInterrupt = interrupt;
    }

    if (gAddr.Player_SequentialDemoActionExecute)
    {
        void* exec = ResolveGameAddress(gAddr.Player_SequentialDemoActionExecute);
        if (exec && CreateAndEnableHook(exec, &hkSequentialDemoActionExecute,
                                        reinterpret_cast<void**>(&g_OrigDemoActionExecute)))
            g_HookDemoExecute = exec;
    }

    if (!g_HookDemoExecute)
    {
        Log("[AttachInDemo] the demo player-placement hook did not install, so the player will "
            "only appear at the connect point once the demo ends - the cutscene's own stream "
            "motion owns the model until then\n");
    }

    if (!g_HookFinish || !g_HookInterrupt)
    {
        Log("[AttachInDemo] the demo end hooks did not install, so an attach stays armed after "
            "its demo ends and would re-apply on the next demo - call "
            "V_Player.ClearAttachInDemo() yourself at demo end\n");
    }

    return true;
}

bool Uninstall_PlayerAttachInDemo_Hook()
{
    ClearAttachInDemo();

    if (g_HookTarget)
        DisableAndRemoveHook(g_HookTarget);
    if (g_HookFinish)
        DisableAndRemoveHook(g_HookFinish);
    if (g_HookInterrupt)
        DisableAndRemoveHook(g_HookInterrupt);

    if (g_HookDemoExecute)
        DisableAndRemoveHook(g_HookDemoExecute);

    g_HookTarget      = nullptr;
    g_HookFinish      = nullptr;
    g_HookInterrupt   = nullptr;
    g_HookDemoExecute = nullptr;
    g_OrigDemoActionExecute = nullptr;
    g_OrigOnPlayingAfterUpdateStream = nullptr;
    g_OrigDoFinish    = nullptr;
    g_OrigDoInterrupt = nullptr;
    g_PlayerImpl.store(nullptr, std::memory_order_relaxed);
    return true;
}
