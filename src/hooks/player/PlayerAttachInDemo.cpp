#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
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
    using DemoActionOnSignal_t = void(__fastcall*)(void* self, std::uint32_t gameObjectIndex,
                                                  void* err, std::uint64_t* signal);
    using UpdateStreamMotion_t = void(__fastcall*)(void* animController, std::uint32_t index,
                                                   const void* transformRT);
    using IsPlaying_t = bool(__fastcall*)(void* demoAnimController, std::uint32_t index);
    using Warp_t = void(__fastcall*)(void* self, std::uint32_t instanceIndex,
                                     const float* pos, const float* rot, bool waitForBlock);
    using GetSharedInstance_t = void* (__fastcall*)();
    using GetTypeObject_t = void* (__fastcall*)(void* manager, std::uint16_t gameObjectId);
    using TypeCnpWorld_t = bool(__fastcall*)(void* controller, std::uint32_t instanceIndex,
                                            std::uint32_t cnpHash, float* outMatrix);

    enum class OwnerKind : std::uint8_t
    {
        Vehicle,
        Horse,
        Heli,
        WalkerGear,
        Other
    };

    constexpr std::uint32_t kInvalidId         = 0xFFFFu;
    constexpr std::uint32_t kInstanceMask      = 0x1FFu;
    constexpr std::size_t   kQuarkAppSystem    = 0x98;
    constexpr std::size_t   kAppVehicleSystem  = 0x2A8;
    constexpr std::size_t   kGetCommunicationSlot = 0x18;
    bool IsLocalPlayerActor(void* self, std::uint32_t gameObjectIndex);

    constexpr std::size_t   kPlayerSubObject   = 0x20;
    constexpr std::size_t   kPlayerWarpSlot    = 0x1B0;
    constexpr std::uint32_t kStatusUnconscious = 0x67;
    constexpr std::uint32_t kTypeIdShift        = 9;
    constexpr std::size_t   kManagerGetTypeSlot = 0x20;
    constexpr std::size_t   kTypeWrapperOffset  = 0x08;
    constexpr std::size_t   kTypeIdOffset       = 0x1C;
    constexpr std::size_t   kWrapperTypeOffset  = 0x20;
    constexpr std::size_t   kMaxRealizedChunks  = 64;
    constexpr std::size_t   kSharedConnectPointSlot = 0x370;
    constexpr std::size_t   kSharedControllerBase   = 0x0C;
    constexpr std::size_t   kSharedControllerCount  = 0x08;
    constexpr std::size_t   kHeliConnectPointSlot   = 0x130;
    constexpr std::size_t   kHeliControllerBase     = 0x6C;
    constexpr std::size_t   kHeliControllerCount    = 0x68;

    constexpr std::uint32_t kDemoPhaseAnimation      = 2;
    constexpr std::ptrdiff_t kExecOwner              = -0x28;
    constexpr std::size_t   kExecDemoAnimController  = 0x18;
    constexpr std::size_t   kOwnerAnimHolder         = 0x78;
    constexpr std::size_t   kAnimHolderController    = 0x238;
    constexpr std::size_t   kOwnerPlayerSub          = 0x138;
    constexpr std::size_t   kPlayerSubLocalIndex     = 0x218;
    constexpr std::size_t   kDemoAnimIsPlayingSlot   = 0x18;
    constexpr std::size_t   kUpdateStreamMotionSlot  = 0x330;

    constexpr std::uint64_t kSignalIdMask         = 0xFFFFFFFFFFFFull;
    constexpr std::uint64_t kSignalDemoFinish     = 0x650a0778cc01ull;
    constexpr std::uint64_t kLastStateRideHorse   = 0x78f0576b4ae7ull;
    constexpr std::uint64_t kLastStateRideHeli    = 0x9c2c350c1ad2ull;
    constexpr std::uint64_t kLastStateRideWalker  = 0x212c94e8edb6ull;
    constexpr std::size_t   kSignalLastStateSlot  = 8;
    constexpr std::size_t   kSignalOpponentOffset = 0x50;
    constexpr std::uint64_t kHandoffWindowMs      = 3000;

    OnPlayingAfterUpdateStream_t g_OrigOnPlayingAfterUpdateStream = nullptr;
    DemoState_t                  g_OrigDoFinish    = nullptr;
    DemoState_t                  g_OrigDoInterrupt = nullptr;
    void*                        g_HookTarget      = nullptr;
    void*                        g_HookFinish      = nullptr;
    void*                        g_HookInterrupt   = nullptr;
    DemoActionExecute_t          g_OrigDemoActionExecute = nullptr;
    void*                        g_HookDemoExecute = nullptr;
    DemoActionOnSignal_t         g_OrigDemoActionOnSignal = nullptr;
    void*                        g_HookDemoOnSignal = nullptr;

    std::mutex        g_Mutex;
    std::string       g_OwnerName;
    std::uint32_t     g_OwnerObjectId   = kInvalidId;
    std::uint32_t     g_ConnectPointId  = 0;
    bool              g_UnattachOnSleep = false;
    OwnerKind         g_OwnerKind       = OwnerKind::Vehicle;
    std::atomic<bool> g_Active{ false };
    std::atomic<std::uint64_t> g_HandoffDeadlineMs{ 0 };
    std::atomic<std::uint64_t> g_SleepReleasedAtMs{ 0 };
    std::atomic<std::uint64_t> g_StreamPlacedAtMs{ 0 };
    std::atomic<std::int64_t>  g_PerFrameHookQpc{ 0 };
    bool         g_ReportedWarpRetired = false;

    constexpr std::uint64_t kStreamLiveWindowMs = 40;
    constexpr unsigned      kSummaryWrites = 30;

    unsigned     g_SummaryWrites = 0;
    std::int64_t g_SummaryFirstQpc = 0;
    float        g_SummaryLastPos[3] = {};
    double       g_SummaryDistance = 0.0;
    bool         g_ReportedSummary = false;

    std::atomic<std::int64_t> g_DemoEndQpc{ 0 };
    std::atomic<bool>         g_InOwnWarp{ false };
    void**                    g_WarpSlot = nullptr;
    Warp_t                    g_OrigWarp = nullptr;
    const char*               g_DemoEndHow = "";
    constexpr double          kDemoEndWatchMs   = 3000.0;

    std::int64_t QpcNow();
    double       QpcToMs(std::int64_t ticks);

    alignas(16) float         g_MissionWarpPos[4] = {};
    alignas(16) float         g_MissionWarpRot[4] = {};
    std::atomic<bool>         g_MissionWarpPending{ false };

    constexpr unsigned kOrderCalibrationFrames = 12;

    float Distance3(const float* a, const float* b);

    bool  g_TuneCamFix = true;

    void ReadAttachTuning()
    {
        g_TuneCamFix = true;
        char path[MAX_PATH]{};
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
            return;
        char* lastSlash = std::strrchr(path, '\\');
        if (!lastSlash)
            return;
        *(lastSlash + 1) = '\0';
        if (strcat_s(path, "mod\\V_FrameWork\\attach_tuning.txt") != 0)
            return;
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f)
        {
            Log("[AttachInDemo] no tuning file at %s - defaults (camera fix on); keys: "
                "camfix=0|1\n", path);
            return;
        }
        char line[128];
        while (std::fgets(line, sizeof(line), f))
        {
            char* eq = std::strchr(line, '=');
            if (!eq)
                continue;
            *eq = '\0';
            const char* key = line;
            const float value = static_cast<float>(std::atof(eq + 1));
            if (std::strncmp(key, "camfix", 6) == 0)
                g_TuneCamFix = value != 0.0f;
        }
        std::fclose(f);
        Log("[AttachInDemo] tuning file %s: camfix=%d\n", path, g_TuneCamFix ? 1 : 0);
    }

    constexpr std::uintptr_t kMainViewportPtr   = 0x142BE1280;
    constexpr std::size_t    kViewportCamera    = 0x570;
    constexpr std::size_t    kCameraWorldMatrix = 0x30;

    constexpr std::uintptr_t kCameraCommitFn      = 0x140437B60;
    constexpr std::size_t    kCameraDaemonCount   = 0x88;
    constexpr std::size_t    kCameraDaemonCameras = 0x90;
    constexpr std::size_t    kGkCameraEnabled     = 0xE0;
    constexpr std::size_t    kGkCameraQuat        = 0x120;
    constexpr std::size_t    kGkCameraPos         = 0x130;
    constexpr std::size_t    kGrCameraViewMatrix  = 0x70;
    constexpr std::size_t    kPlaybackAnchorScale = 0x190;
    constexpr std::size_t    kPlaybackAnchorQuat  = 0x1A0;
    constexpr std::size_t    kPlaybackAnchorPos   = 0x1B0;
    constexpr float          kAnchorMatchMeters   = 0.02f;
    constexpr float          kAnchorMatchQuatDot  = 0.999f;
    constexpr unsigned char  kCameraCommitPrologue[] = { 0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54,
                                                         0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };

    using CameraCommit_t = void(__fastcall*)(void* daemon, void* a2, void* a3, void* a4);
    CameraCommit_t     g_OrigCameraCommit = nullptr;
    void*              g_HookCameraCommit = nullptr;
    std::atomic<void*> g_Playback{ nullptr };

    void MatrixToQuaternion(const float* m16, float* outXYZW);

    enum class AnchorMode : std::uint8_t
    {
        Unknown,
        Full,
        YawOnly,
        TranslationOnly
    };

    struct CamFix
    {
        bool         valid = false;
        float        dq[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float        dt[3] = {};
        std::int64_t writeQpc = 0;
    };

    std::mutex   g_CamFixMutex;
    CamFix       g_CamFix;
    AnchorMode   g_AnchorMode = AnchorMode::Unknown;
    float        g_PrevWriteQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float        g_PrevWritePos[3] = {};
    float        g_PrevPrevWriteQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float        g_PrevPrevWritePos[3] = {};
    unsigned     g_CamFixWriteCount = 0;
    std::atomic<std::int64_t> g_LastCommitQpc{ 0 };
    float        g_LastPatchedQuat[4] = {};
    float        g_LastPatchedPos[3] = {};
    bool         g_PatchedOnce = false;
    unsigned     g_CommitAfterWriteCount = 0;
    unsigned     g_CommitBeforeWriteCount = 0;
    unsigned     g_CommitStaticCount = 0;
    unsigned     g_CommitSeen = 0;
    unsigned     g_AnchorLagOne = 0;
    unsigned     g_AnchorLagTwo = 0;
    unsigned     g_AnchorLagNone = 0;
    bool         g_ReportedAnchorMismatch = false;
    bool         g_ReportedCameraStatic = false;
    bool         g_ReportedGrPatchFailed = false;
    float        g_LastFixLen = 0.0f;
    float        g_LastFixYawDeg = 0.0f;
    double       g_LastCommitAfterWriteMs = 0.0;

    void ResetCamFixState()
    {
        {
            std::lock_guard<std::mutex> lock(g_CamFixMutex);
            g_CamFix = CamFix{};
        }
        g_AnchorMode = AnchorMode::Unknown;
        g_CamFixWriteCount = 0;
        g_PatchedOnce = false;
        g_CommitAfterWriteCount = 0;
        g_CommitBeforeWriteCount = 0;
        g_CommitStaticCount = 0;
        g_CommitSeen = 0;
        g_AnchorLagOne = 0;
        g_AnchorLagTwo = 0;
        g_AnchorLagNone = 0;
        g_ReportedAnchorMismatch = false;
        g_ReportedCameraStatic = false;
        g_ReportedGrPatchFailed = false;
        g_LastFixLen = 0.0f;
        g_LastFixYawDeg = 0.0f;
        g_LastCommitAfterWriteMs = 0.0;
    }

    void QuatMul(const float* a, const float* b, float* out)
    {
        out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    }

    void QuatNormalize(float* q)
    {
        const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (len < 1e-6f)
        {
            q[0] = 0.0f;
            q[1] = 0.0f;
            q[2] = 0.0f;
            q[3] = 1.0f;
            return;
        }
        for (int i = 0; i < 4; ++i)
            q[i] /= len;
    }

    void QuatRotate(const float* q, const float* v, float* out)
    {
        const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
        const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
        const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
        out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
        out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
        out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
    }

    void QuatToMatrix3(const float* q, float* r9)
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        r9[0] = 1.0f - 2.0f * (y * y + z * z);
        r9[1] = 2.0f * (x * y + z * w);
        r9[2] = 2.0f * (x * z - y * w);
        r9[3] = 2.0f * (x * y - z * w);
        r9[4] = 1.0f - 2.0f * (x * x + z * z);
        r9[5] = 2.0f * (y * z + x * w);
        r9[6] = 2.0f * (x * z + y * w);
        r9[7] = 2.0f * (y * z - x * w);
        r9[8] = 1.0f - 2.0f * (x * x + y * y);
    }

    void YawOnlyQuat(const float* q, float* out)
    {
        const float fx = 2.0f * (q[0] * q[2] + q[1] * q[3]);
        const float fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
        const float yaw = std::atan2(fx, fz);
        out[0] = 0.0f;
        out[1] = std::sin(yaw * 0.5f);
        out[2] = 0.0f;
        out[3] = std::cos(yaw * 0.5f);
    }

    float QuatDotAbs(const float* a, const float* b)
    {
        return std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    }

    bool ReadPlaybackAnchor(void* playback, float* outQuat, float* outPos, float* outScale)
    {
        __try
        {
            auto* base = reinterpret_cast<const std::uint8_t*>(playback);
            const float* scale = reinterpret_cast<const float*>(base + kPlaybackAnchorScale);
            const float* quat  = reinterpret_cast<const float*>(base + kPlaybackAnchorQuat);
            const float* pos   = reinterpret_cast<const float*>(base + kPlaybackAnchorPos);
            for (int i = 0; i < 4; ++i)
            {
                if (!std::isfinite(scale[i]) || !std::isfinite(quat[i]) || !std::isfinite(pos[i]))
                    return false;
            }
            const float qLen = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2]
                               + quat[3] * quat[3];
            if (qLen < 0.9f || qLen > 1.1f)
                return false;
            for (int i = 0; i < 3; ++i)
            {
                if (scale[i] < 0.5f || scale[i] > 2.0f)
                    return false;
                outScale[i] = scale[i];
                outPos[i] = pos[i];
            }
            for (int i = 0; i < 4; ++i)
                outQuat[i] = quat[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ApplyFixToGrCamera(const float* dq, const float* dt)
    {
        __try
        {
            auto* viewport = *reinterpret_cast<std::uint8_t**>(kMainViewportPtr);
            if (!viewport)
                return false;
            auto* camera = *reinterpret_cast<std::uint8_t**>(viewport + kViewportCamera);
            if (!camera)
                return false;
            float* world = reinterpret_cast<float*>(camera + kCameraWorldMatrix);
            float* view  = reinterpret_cast<float*>(camera + kGrCameraViewMatrix);
            for (int i = 0; i < 16; ++i)
            {
                if (!std::isfinite(world[i]))
                    return false;
            }
            float r[9];
            QuatToMatrix3(dq, r);
            float fresh[16];
            for (int c = 0; c < 4; ++c)
            {
                const float vx = world[c * 4 + 0];
                const float vy = world[c * 4 + 1];
                const float vz = world[c * 4 + 2];
                fresh[c * 4 + 0] = r[0] * vx + r[3] * vy + r[6] * vz;
                fresh[c * 4 + 1] = r[1] * vx + r[4] * vy + r[7] * vz;
                fresh[c * 4 + 2] = r[2] * vx + r[5] * vy + r[8] * vz;
                fresh[c * 4 + 3] = world[c * 4 + 3];
            }
            fresh[12] += dt[0];
            fresh[13] += dt[1];
            fresh[14] += dt[2];
            float inv[16];
            for (int c = 0; c < 3; ++c)
            {
                for (int rr = 0; rr < 3; ++rr)
                    inv[c * 4 + rr] = fresh[rr * 4 + c];
                inv[c * 4 + 3] = 0.0f;
            }
            for (int rr = 0; rr < 3; ++rr)
            {
                inv[12 + rr] = -(fresh[rr * 4 + 0] * fresh[12] + fresh[rr * 4 + 1] * fresh[13]
                                 + fresh[rr * 4 + 2] * fresh[14]);
            }
            inv[15] = 1.0f;
            for (int i = 0; i < 16; ++i)
            {
                world[i] = fresh[i];
                view[i] = inv[i];
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void NoteCamFixAtWrite(const float* matrix16)
    {
        float q[4];
        MatrixToQuaternion(matrix16, q);
        QuatNormalize(q);
        const float p[3] = { matrix16[12], matrix16[13], matrix16[14] };

        float prevQ[4], prevPrevQ[4];
        float prevP[3], prevPrevP[3];
        for (int i = 0; i < 4; ++i)
        {
            prevQ[i] = g_PrevWriteQuat[i];
            prevPrevQ[i] = g_PrevPrevWriteQuat[i];
            g_PrevPrevWriteQuat[i] = g_PrevWriteQuat[i];
            g_PrevWriteQuat[i] = q[i];
        }
        for (int i = 0; i < 3; ++i)
        {
            prevP[i] = g_PrevWritePos[i];
            prevPrevP[i] = g_PrevPrevWritePos[i];
            g_PrevPrevWritePos[i] = g_PrevWritePos[i];
            g_PrevWritePos[i] = p[i];
        }
        const unsigned written = g_CamFixWriteCount;
        if (g_CamFixWriteCount < 1000000u)
            ++g_CamFixWriteCount;

        if (!g_TuneCamFix || written < 2)
            return;

        void* playback = g_Playback.load(std::memory_order_relaxed);
        float aq[4], ap[3], ascale[3];
        if (!playback || !ReadPlaybackAnchor(playback, aq, ap, ascale))
        {
            std::lock_guard<std::mutex> lock(g_CamFixMutex);
            g_CamFix.valid = false;
            return;
        }

        const float* matchQ = nullptr;
        if (Distance3(ap, prevP) < kAnchorMatchMeters)
        {
            ++g_AnchorLagOne;
            matchQ = prevQ;
        }
        else if (Distance3(ap, prevPrevP) < kAnchorMatchMeters)
        {
            ++g_AnchorLagTwo;
            matchQ = prevPrevQ;
        }
        else
        {
            ++g_AnchorLagNone;
            if (!g_ReportedAnchorMismatch)
            {
                g_ReportedAnchorMismatch = true;
                Log("[AttachInDemo] the playback's relative-play anchor (%.2f %.2f %.2f, scale %.2f "
                    "%.2f %.2f) matches neither the pose written one frame earlier (%.2f %.2f %.2f) "
                    "nor two frames earlier (%.2f %.2f %.2f), so the demo is not re-anchored to the "
                    "player each frame and the camera fix stays idle\n",
                    ap[0], ap[1], ap[2], ascale[0], ascale[1], ascale[2], prevP[0], prevP[1], prevP[2],
                    prevPrevP[0], prevPrevP[1], prevPrevP[2]);
            }
            std::lock_guard<std::mutex> lock(g_CamFixMutex);
            g_CamFix.valid = false;
            return;
        }

        if (g_AnchorMode == AnchorMode::Unknown)
        {
            float yawQ[4];
            YawOnlyQuat(matchQ, yawQ);
            if (QuatDotAbs(aq, matchQ) > kAnchorMatchQuatDot)
                g_AnchorMode = AnchorMode::Full;
            else if (QuatDotAbs(aq, yawQ) > kAnchorMatchQuatDot)
                g_AnchorMode = AnchorMode::YawOnly;
            else
                g_AnchorMode = AnchorMode::TranslationOnly;
        }

        float tq[4] = { q[0], q[1], q[2], q[3] };
        if (g_AnchorMode == AnchorMode::YawOnly)
            YawOnlyQuat(q, tq);
        else if (g_AnchorMode == AnchorMode::TranslationOnly)
        {
            for (int i = 0; i < 4; ++i)
                tq[i] = aq[i];
        }
        const float aqConj[4] = { -aq[0], -aq[1], -aq[2], aq[3] };
        float dq[4];
        QuatMul(tq, aqConj, dq);
        QuatNormalize(dq);
        float rotatedAnchor[3];
        QuatRotate(dq, ap, rotatedAnchor);
        float dt[3];
        for (int i = 0; i < 3; ++i)
            dt[i] = p[i] - rotatedAnchor[i];

        const std::int64_t now = QpcNow();
        {
            std::lock_guard<std::mutex> lock(g_CamFixMutex);
            g_CamFix.valid = true;
            for (int i = 0; i < 4; ++i)
                g_CamFix.dq[i] = dq[i];
            for (int i = 0; i < 3; ++i)
                g_CamFix.dt[i] = dt[i];
            g_CamFix.writeQpc = now;
        }
        g_LastFixLen = Distance3(p, ap);
        const float wAbs = std::fabs(dq[3]) > 1.0f ? 1.0f : std::fabs(dq[3]);
        g_LastFixYawDeg = 2.0f * std::acos(wAbs) * 57.29578f;

        const std::int64_t tickQpc = g_PerFrameHookQpc.load(std::memory_order_relaxed);
        const std::int64_t commitQpc = g_LastCommitQpc.load(std::memory_order_relaxed);
        if (!g_HookCameraCommit || commitQpc > tickQpc)
        {
            if (ApplyFixToGrCamera(dq, dt))
            {
                ++g_CommitBeforeWriteCount;
                std::lock_guard<std::mutex> lock(g_CamFixMutex);
                g_CamFix.valid = false;
            }
            else if (!g_ReportedGrPatchFailed)
            {
                g_ReportedGrPatchFailed = true;
                Log("[AttachInDemo] the committed viewport camera could not be patched at the "
                    "stream write, so the camera fix has no effect on frames where the camera "
                    "commit runs before the write\n");
            }
        }
    }

    int PatchActiveGkCamera(void* daemon, const CamFix* fix)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(daemon);
            const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(base + kCameraDaemonCount);
            auto** cameras = *reinterpret_cast<std::uint8_t***>(base + kCameraDaemonCameras);
            std::uint8_t* active = nullptr;
            for (std::uint32_t i = 0; cameras && i < count && i < 64; ++i)
            {
                if (cameras[i] && cameras[i][kGkCameraEnabled] != 0)
                {
                    active = cameras[i];
                    break;
                }
            }
            if (!active)
                return -1;
            float* quat = reinterpret_cast<float*>(active + kGkCameraQuat);
            float* pos  = reinterpret_cast<float*>(active + kGkCameraPos);
            bool finite = true;
            for (int i = 0; i < 4; ++i)
                finite = finite && std::isfinite(quat[i]) && std::isfinite(pos[i]);
            const float qLen = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2]
                               + quat[3] * quat[3];
            if (!finite || qLen < 0.9f || qLen > 1.1f)
                return -1;
            bool untouched = g_PatchedOnce;
            for (int i = 0; i < 4 && untouched; ++i)
                untouched = quat[i] == g_LastPatchedQuat[i];
            for (int i = 0; i < 3 && untouched; ++i)
                untouched = pos[i] == g_LastPatchedPos[i];
            if (untouched)
                return 0;
            float nq[4];
            QuatMul(fix->dq, quat, nq);
            QuatNormalize(nq);
            float np[3];
            QuatRotate(fix->dq, pos, np);
            for (int i = 0; i < 4; ++i)
            {
                quat[i] = nq[i];
                g_LastPatchedQuat[i] = nq[i];
            }
            for (int i = 0; i < 3; ++i)
            {
                pos[i] = np[i] + fix->dt[i];
                g_LastPatchedPos[i] = pos[i];
            }
            g_PatchedOnce = true;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    constexpr std::uintptr_t kGetControlRootFn     = 0x140421740;
    constexpr std::uintptr_t kAnchorCaptureBegin   = 0x1404334B0;
    constexpr std::uintptr_t kAnchorCaptureEnd     = 0x140433720;
    constexpr double         kDemoTickRecentMs     = 200.0;
    constexpr float          kGeneralFixMinMeters  = 0.002f;
    constexpr float          kRootIdMatchMeters    = 1.0f;
    constexpr unsigned       kControlRootRing      = 4;
    constexpr unsigned char  kGetControlRootPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57,
                                                           0x48, 0x83, 0xEC, 0x30 };

    using GetControlRoot_t = int* (__fastcall*)(int* err, float* outTransform, std::uint64_t id);
    GetControlRoot_t          g_OrigGetControlRoot = nullptr;
    void*                     g_HookGetControlRoot = nullptr;
    std::atomic<std::int64_t> g_DemoTickQpc{ 0 };

    struct ControlRootSample
    {
        std::uint64_t id = 0;
        float         pos[3] = {};
        bool          ok = false;
    };

    std::mutex        g_ControlRootMutex;
    ControlRootSample g_ControlRootRing[kControlRootRing];
    unsigned          g_ControlRootRingCount = 0;
    float             g_GenPrevFresh[2][3] = {};
    bool              g_GenPrevValid[2] = { false, false };
    std::uint64_t     g_LastRootId = 0;
    unsigned          g_GenSeen = 0;
    unsigned          g_GenApplied = 0;
    float             g_GenMaxLen = 0.0f;
    bool              g_ReportedGenEngaged = false;
    bool              g_ReportedRootFreshness = false;

    void ResetGeneralFixState()
    {
        g_GenPrevValid[0] = false;
        g_GenPrevValid[1] = false;
        g_GenSeen = 0;
        g_GenApplied = 0;
        g_GenMaxLen = 0.0f;
        g_ReportedGenEngaged = false;
        g_ReportedRootFreshness = false;
    }

    int* __fastcall hkGetCharacterControlRoot(int* err, float* outTransform, std::uint64_t id)
    {
        int* result = g_OrigGetControlRoot ? g_OrigGetControlRoot(err, outTransform, id) : err;
        const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const std::uintptr_t base = GetExeBase();
        if (base && caller >= base + ToRva(kAnchorCaptureBegin)
            && caller < base + ToRva(kAnchorCaptureEnd) && outTransform)
        {
            ControlRootSample sample;
            sample.id = id;
            sample.ok = result && *result >= 0;
            sample.pos[0] = outTransform[8];
            sample.pos[1] = outTransform[9];
            sample.pos[2] = outTransform[10];
            std::lock_guard<std::mutex> lock(g_ControlRootMutex);
            g_ControlRootRing[g_ControlRootRingCount % kControlRootRing] = sample;
            ++g_ControlRootRingCount;
        }
        return result;
    }

    bool ReadFreshControlRoot(std::uint64_t id, float* outQuat, float* outPos)
    {
        if (!g_OrigGetControlRoot)
            return false;
        alignas(16) float transform[12] = {};
        int err = -1;
        int* result = g_OrigGetControlRoot(&err, transform, id);
        if (!result || *result < 0)
            return false;
        for (int i = 0; i < 12; ++i)
        {
            if (!std::isfinite(transform[i]))
                return false;
        }
        for (int i = 0; i < 4; ++i)
            outQuat[i] = transform[4 + i];
        for (int i = 0; i < 3; ++i)
            outPos[i] = transform[8 + i];
        return true;
    }

    std::uint64_t PickAnchoredRootId(const float* anchorPos)
    {
        std::lock_guard<std::mutex> lock(g_ControlRootMutex);
        const unsigned n = g_ControlRootRingCount < kControlRootRing ? g_ControlRootRingCount
                                                                     : kControlRootRing;
        for (unsigned back = 1; back <= n; ++back)
        {
            const ControlRootSample& s =
                g_ControlRootRing[(g_ControlRootRingCount - back) % kControlRootRing];
            if (s.ok && Distance3(s.pos, anchorPos) < kRootIdMatchMeters)
                return s.id;
        }
        return 0;
    }

    void NoteControlRootFreshness(std::uint64_t id)
    {
        if (g_ReportedRootFreshness || g_CamFixWriteCount < kOrderCalibrationFrames)
            return;
        float fq[4], fp[3];
        if (!ReadFreshControlRoot(id, fq, fp))
            return;
        g_ReportedRootFreshness = true;
        Log("[AttachInDemo] at the camera commit the player's control root sits %.3f m from the "
            "pose written this frame (0 = the control root follows the stream write at once, one "
            "frame of motion = it only catches up after the commit)\n",
            Distance3(fp, g_PrevWritePos));
    }

    void ApplyGeneralCameraFix(void* daemon, std::int64_t now)
    {
        void* playback = g_Playback.load(std::memory_order_relaxed);
        const std::int64_t tick = g_DemoTickQpc.load(std::memory_order_relaxed);
        if (!playback || !tick || QpcToMs(now - tick) > kDemoTickRecentMs)
            return;
        float aq[4], ap[3], ascale[3];
        if (!ReadPlaybackAnchor(playback, aq, ap, ascale))
            return;
        const std::uint64_t id = PickAnchoredRootId(ap);
        if (!id)
            return;
        g_LastRootId = id;
        float fq[4], fp[3];
        if (!ReadFreshControlRoot(id, fq, fp))
            return;
        if (g_GenSeen < 1000000u)
            ++g_GenSeen;
        const bool anchored =
            (g_GenPrevValid[0] && Distance3(ap, g_GenPrevFresh[0]) < kAnchorMatchMeters)
            || (g_GenPrevValid[1] && Distance3(ap, g_GenPrevFresh[1]) < kAnchorMatchMeters);
        for (int i = 0; i < 3; ++i)
        {
            g_GenPrevFresh[1][i] = g_GenPrevFresh[0][i];
            g_GenPrevFresh[0][i] = fp[i];
        }
        g_GenPrevValid[1] = g_GenPrevValid[0];
        g_GenPrevValid[0] = true;
        if (!anchored)
            return;
        const float len = Distance3(fp, ap);
        const float dot = QuatDotAbs(fq, aq);
        if (len < kGeneralFixMinMeters && dot > 0.99999f)
            return;
        const float aqConj[4] = { -aq[0], -aq[1], -aq[2], aq[3] };
        CamFix fix;
        QuatMul(fq, aqConj, fix.dq);
        QuatNormalize(fix.dq);
        float rotatedAnchor[3];
        QuatRotate(fix.dq, ap, rotatedAnchor);
        for (int i = 0; i < 3; ++i)
            fix.dt[i] = fp[i] - rotatedAnchor[i];
        fix.valid = true;
        fix.writeQpc = now;
        if (PatchActiveGkCamera(daemon, &fix) != 1)
            return;
        ++g_GenApplied;
        if (len > g_GenMaxLen)
            g_GenMaxLen = len;
        if (!g_ReportedGenEngaged)
        {
            g_ReportedGenEngaged = true;
            Log("[AttachInDemo] demo camera lag fix engaged on a frame with no stream write: this "
                "demo re-anchors itself every frame to a character that moved %.3f m after the "
                "anchor was captured, so the camera is re-aligned to that character's fresh pose "
                "at every camera commit\n",
                len);
        }
    }

    void ReportGeneralFixAtDemoEnd()
    {
        if (g_GenApplied)
        {
            Log("[AttachInDemo] demo camera lag fix corrected %u of the %u demo frames that had no "
                "stream write, largest correction %.3f m\n",
                g_GenApplied, g_GenSeen, g_GenMaxLen);
        }
        ResetGeneralFixState();
    }

    void __fastcall hkCameraCommit(void* daemon, void* a2, void* a3, void* a4)
    {
        if (!MissionCodeGuard::ShouldBypassHooks())
        {
            const std::int64_t now = QpcNow();
            bool patched = false;
            if (g_Active.load(std::memory_order_relaxed) && g_TuneCamFix)
            {
                const std::int64_t tickQpc = g_PerFrameHookQpc.load(std::memory_order_relaxed);
                CamFix fix;
                {
                    std::lock_guard<std::mutex> lock(g_CamFixMutex);
                    fix = g_CamFix;
                    if (fix.valid && fix.writeQpc > tickQpc)
                        g_CamFix.valid = false;
                }
                if (g_CommitSeen < 1000000u)
                    ++g_CommitSeen;
                if (fix.valid && fix.writeQpc > tickQpc)
                {
                    const int result = PatchActiveGkCamera(daemon, &fix);
                    if (result == 1)
                    {
                        patched = true;
                        ++g_CommitAfterWriteCount;
                        g_LastCommitAfterWriteMs = QpcToMs(now - fix.writeQpc);
                        float ap[3], aq[4], ascale[3];
                        void* playback = g_Playback.load(std::memory_order_relaxed);
                        if (playback && ReadPlaybackAnchor(playback, aq, ap, ascale))
                        {
                            const std::uint64_t id = PickAnchoredRootId(ap);
                            if (id)
                            {
                                g_LastRootId = id;
                                NoteControlRootFreshness(id);
                            }
                        }
                    }
                    else if (result == 0)
                    {
                        ++g_CommitStaticCount;
                        if (!g_ReportedCameraStatic)
                        {
                            g_ReportedCameraStatic = true;
                            Log("[AttachInDemo] the active gamekit camera still holds the transform "
                                "patched on the previous frame, so the demo did not rewrite it this "
                                "frame and the camera fix skipped it\n");
                        }
                    }
                }
                g_LastCommitQpc.store(now, std::memory_order_relaxed);
            }
            if (g_TuneCamFix && !patched)
                ApplyGeneralCameraFix(daemon, now);
        }
        if (g_OrigCameraCommit)
            g_OrigCameraCommit(daemon, a2, a3, a4);
    }

    bool InstallGetControlRootHook()
    {
        if (g_HookGetControlRoot)
            return true;
        void* target = ResolveGameAddress(kGetControlRootFn);
        if (!target)
            return false;
        if (std::memcmp(target, kGetControlRootPrologue, sizeof(kGetControlRootPrologue)) != 0)
        {
            Log("[AttachInDemo] the character control-root reader at %p does not start with the "
                "expected prologue, so the demo camera lag fix only works with an attach on this "
                "build\n", target);
            return false;
        }
        if (!CreateAndEnableHook(target, &hkGetCharacterControlRoot,
                                 reinterpret_cast<void**>(&g_OrigGetControlRoot)))
        {
            Log("[AttachInDemo] the character control-root hook did not install, so the demo "
                "camera lag fix only works with an attach\n");
            return false;
        }
        g_HookGetControlRoot = target;
        return true;
    }

    bool InstallCameraCommitHook()
    {
        if (g_HookCameraCommit)
            return true;
        void* target = ResolveGameAddress(kCameraCommitFn);
        if (!target)
            return false;
        if (std::memcmp(target, kCameraCommitPrologue, sizeof(kCameraCommitPrologue)) != 0)
        {
            Log("[AttachInDemo] the camera commit routine at %p does not start with the expected "
                "prologue, so the demo camera fix is not installed on this build\n", target);
            return false;
        }
        if (!CreateAndEnableHook(target, &hkCameraCommit,
                                 reinterpret_cast<void**>(&g_OrigCameraCommit)))
        {
            Log("[AttachInDemo] the camera commit hook did not install, so the demo camera fix "
                "can only patch the committed viewport matrices at the stream write\n");
            return false;
        }
        g_HookCameraCommit = target;
        return true;
    }

    float Distance3(const float* a, const float* b)
    {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    std::int64_t QpcNow()
    {
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        return li.QuadPart;
    }

    double QpcToMs(std::int64_t ticks)
    {
        static const double perMs = []
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart) / 1000.0;
        }();
        return static_cast<double>(ticks) / perMs;
    }

    std::atomic<void*> g_PlayerImpl{ nullptr };

    bool g_ReportedNoMatrix = false;
    bool g_ReportedHookLive = false;
    bool g_ReportedAttached = false;
    bool g_ReportedPlaced = false;
    bool g_ReportedNoPlacementKind = false;
    bool g_ReportedHandoff = false;
    bool g_ReportedTypePath = false;

    const char* KindName(OwnerKind kind)
    {
        switch (kind)
        {
            case OwnerKind::Vehicle:    return "vehicle";
            case OwnerKind::Horse:      return "horse";
            case OwnerKind::Heli:       return "heli";
            case OwnerKind::WalkerGear: return "walkergear";
            default:                    return "other";
        }
    }

    struct OwnerProbe
    {
        const char* typeName;
        OwnerKind   kind;
    };

    constexpr OwnerProbe kOwnerProbes[] =
    {
        { "TppVehicle2",    OwnerKind::Vehicle },
        { "TppHorse2",      OwnerKind::Horse },
        { "TppHeli2",       OwnerKind::Heli },
        { "TppWalkerGear2", OwnerKind::WalkerGear },
    };

    bool ResolveOwnerByName(const char* ownerName, std::uint32_t& idOut, OwnerKind& kindOut)
    {
        for (const OwnerProbe& probe : kOwnerProbes)
        {
            std::uint32_t id = kInvalidId;
            if (GetGameObjectIdByName(probe.typeName, ownerName, id) && id != kInvalidId)
            {
                idOut   = id;
                kindOut = probe.kind;
                return true;
            }
        }
        return false;
    }

    bool ResolveOwnerKindById(std::uint32_t ownerObjectId, OwnerKind& kindOut)
    {
        for (const OwnerProbe& probe : kOwnerProbes)
        {
            std::uint32_t id = kInvalidId;
            if (GetGameObjectIdWithIndex(probe.typeName, ownerObjectId & kInstanceMask, id)
                && id == ownerObjectId)
            {
                kindOut = probe.kind;
                return true;
            }
        }
        return false;
    }

    std::uint64_t HandoffStateForKind(OwnerKind kind)
    {
        switch (kind)
        {
            case OwnerKind::Horse:      return kLastStateRideHorse;
            case OwnerKind::Heli:       return kLastStateRideHeli;
            case OwnerKind::WalkerGear: return kLastStateRideWalker;
            default:                    return 0;
        }
    }

    void NormalizeAxis(float* v)
    {
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f)
        {
            v[0] /= len;
            v[1] /= len;
            v[2] /= len;
        }
        else
        {
            v[0] = 0.0f;
            v[1] = 0.0f;
            v[2] = 0.0f;
        }
    }

    void MatrixToQuaternion(const float* m16, float* outXYZW)
    {
        float x[3] = { m16[0], m16[1], m16[2] };
        float y[3] = { m16[4], m16[5], m16[6] };
        float z[3] = { m16[8], m16[9], m16[10] };
        NormalizeAxis(x);
        NormalizeAxis(y);
        NormalizeAxis(z);

        const float r00 = x[0], r10 = x[1], r20 = x[2];
        const float r01 = y[0], r11 = y[1], r21 = y[2];
        const float r02 = z[0], r12 = z[1], r22 = z[2];

        float qx, qy, qz, qw;
        const float trace = r00 + r11 + r22;
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            qw = 0.25f * s;
            qx = (r21 - r12) / s;
            qy = (r02 - r20) / s;
            qz = (r10 - r01) / s;
        }
        else if (r00 > r11 && r00 > r22)
        {
            const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
            qw = (r21 - r12) / s;
            qx = 0.25f * s;
            qy = (r01 + r10) / s;
            qz = (r02 + r20) / s;
        }
        else if (r11 > r22)
        {
            const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
            qw = (r02 - r20) / s;
            qx = (r01 + r10) / s;
            qy = 0.25f * s;
            qz = (r12 + r21) / s;
        }
        else
        {
            const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
            qw = (r10 - r01) / s;
            qx = (r02 + r20) / s;
            qy = (r12 + r21) / s;
            qz = 0.25f * s;
        }

        const float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (len > 1e-6f)
        {
            qx /= len;
            qy /= len;
            qz /= len;
            qw /= len;
        }
        else
        {
            qx = 0.0f;
            qy = 0.0f;
            qz = 0.0f;
            qw = 1.0f;
        }

        outXYZW[0] = qx;
        outXYZW[1] = qy;
        outXYZW[2] = qz;
        outXYZW[3] = qw;
    }

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

    void ReleaseAttach(const char* how)
    {
        const bool wasActive = g_Active.exchange(false, std::memory_order_relaxed);
        const std::uint64_t sleepReleasedAt =
            g_SleepReleasedAtMs.exchange(0, std::memory_order_relaxed);
        if (!wasActive && sleepReleasedAt == 0)
            return;

        std::string owner;
        OwnerKind kind;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            owner = g_OwnerName;
            kind  = g_OwnerKind;
        }

        char handoff[160];
        if (HandoffStateForKind(kind) == 0)
        {
            std::snprintf(handoff, sizeof(handoff),
                          "a %s owner has no boarding handoff", KindName(kind));
        }
        else if (g_ReportedHandoff)
        {
            std::snprintf(handoff, sizeof(handoff),
                          "the boarding handoff already patched the finish payload");
        }
        else
        {
            const std::uint64_t now      = GetTickCount64();
            const std::uint64_t deadline = g_HandoffDeadlineMs.load(std::memory_order_relaxed);
            if (deadline > now)
                std::snprintf(handoff, sizeof(handoff),
                              "the boarding handoff window stays open for %llu ms in case the "
                              "finish signal arrives late",
                              static_cast<unsigned long long>(deadline - now));
            else
                std::snprintf(handoff, sizeof(handoff), "the boarding handoff never fired");
        }

        if (sleepReleasedAt != 0)
        {
            const std::uint64_t now = GetTickCount64();
            Log("[AttachInDemo] %s - '%s' (%s) had already been released %llu ms earlier when "
                "the player went unconscious with unattachOnSleep set: the per-frame placement "
                "stopped then, the next demo will not re-apply it, and %s\n",
                how, owner.c_str(), KindName(kind),
                static_cast<unsigned long long>(now > sleepReleasedAt ? now - sleepReleasedAt : 0),
                handoff);
            return;
        }

        Log("[AttachInDemo] %s - '%s' (%s) released: the per-frame placement has stopped, the "
            "next demo will not re-apply it, and %s\n",
            how, owner.c_str(), KindName(kind), handoff);
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

    bool ReadVehicleConnectPointMatrix(std::uint32_t ownerObjectId, std::uint32_t cnpHash,
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

    bool ReadFloat3Safe(const float* src, float* out)
    {
        __try
        {
            out[0] = src[0];
            out[1] = src[1];
            out[2] = src[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadFloat4Safe(const float* src, float* out)
    {
        __try
        {
            out[0] = src[0];
            out[1] = src[1];
            out[2] = src[2];
            out[3] = src[3];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void __fastcall hkPlayerWarp(void* self, std::uint32_t instanceIndex, const float* pos,
                                 const float* rot, bool waitForBlock)
    {
        const bool own = g_InOwnWarp.exchange(false, std::memory_order_relaxed);
        if (!own)
        {
            float p[3] = {};
            const bool havePos = pos && ReadFloat3Safe(pos, p);
            const std::int64_t endQpc = g_DemoEndQpc.load(std::memory_order_relaxed);
            const double sinceEnd = endQpc ? QpcToMs(QpcNow() - endQpc) : -1.0;
            const bool inWindow = sinceEnd >= 0.0 && sinceEnd <= kDemoEndWatchMs;
            if (inWindow)
                Log("[AttachInDemo] the game or Lua warped the player to (%.2f %.2f %.2f) %.1f ms "
                    "after the demo %s (instance %u, waitForBlock=%d)\n",
                    havePos ? p[0] : 0.0f, havePos ? p[1] : 0.0f, havePos ? p[2] : 0.0f,
                    sinceEnd, g_DemoEndHow, instanceIndex, waitForBlock ? 1 : 0);

            if (inWindow && !waitForBlock && havePos)
            {
                g_MissionWarpPos[0] = p[0];
                g_MissionWarpPos[1] = p[1];
                g_MissionWarpPos[2] = p[2];
                g_MissionWarpPos[3] = 1.0f;
                float r[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (rot)
                    ReadFloat4Safe(rot, r);
                g_MissionWarpRot[0] = r[0];
                g_MissionWarpRot[1] = r[1];
                g_MissionWarpRot[2] = r[2];
                g_MissionWarpRot[3] = r[3];
                g_MissionWarpPending.store(true, std::memory_order_relaxed);
                Log("[AttachInDemo] that warp is the mission's own post-demo placement; if the demo's "
                    "end-of-skip restore warp follows it, the restore is redirected there\n");
            }
            else if (inWindow && waitForBlock
                     && g_MissionWarpPending.exchange(false, std::memory_order_relaxed))
            {
                Log("[AttachInDemo] the demo's restore warp to (%.2f %.2f %.2f) was redirected to the "
                    "mission's target (%.2f %.2f %.2f), block loading kept - without this the restore "
                    "put the player back where the demo was skipped\n",
                    havePos ? p[0] : 0.0f, havePos ? p[1] : 0.0f, havePos ? p[2] : 0.0f,
                    g_MissionWarpPos[0], g_MissionWarpPos[1], g_MissionWarpPos[2]);
                if (g_OrigWarp)
                    g_OrigWarp(self, instanceIndex, g_MissionWarpPos, g_MissionWarpRot, waitForBlock);
                return;
            }
            else if (!inWindow)
            {
                g_MissionWarpPending.store(false, std::memory_order_relaxed);
            }
        }
        if (g_OrigWarp)
            g_OrigWarp(self, instanceIndex, pos, rot, waitForBlock);
    }

    void EnsurePlayerWarpHook()
    {
        if (g_WarpSlot)
            return;
        void* impl = g_PlayerImpl.load(std::memory_order_relaxed);
        if (!impl)
            return;
        __try
        {
            auto* sub = reinterpret_cast<std::uint8_t*>(impl) + kPlayerSubObject;
            auto** vtbl = *reinterpret_cast<void***>(sub);
            if (!vtbl)
                return;
            void** slot = vtbl + kPlayerWarpSlot / sizeof(void*);
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                return;
            g_OrigWarp = reinterpret_cast<Warp_t>(*slot);
            *slot = reinterpret_cast<void*>(&hkPlayerWarp);
            VirtualProtect(slot, sizeof(void*), old, &old);
            g_WarpSlot = slot;
            Log("[AttachInDemo] player Warp vtable slot 0x%zX hooked (orig=%p) - every warp the game "
                "or Lua sends around a demo end is logged with its timing\n",
                kPlayerWarpSlot, reinterpret_cast<void*>(g_OrigWarp));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void RestorePlayerWarpHook()
    {
        if (!g_WarpSlot)
            return;
        __try
        {
            DWORD old = 0;
            if (VirtualProtect(g_WarpSlot, sizeof(void*), PAGE_READWRITE, &old))
            {
                *g_WarpSlot = reinterpret_cast<void*>(g_OrigWarp);
                VirtualProtect(g_WarpSlot, sizeof(void*), old, &old);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        g_WarpSlot = nullptr;
        g_OrigWarp = nullptr;
    }

    void NoteDemoEnd(const char* how)
    {
        g_DemoEndHow = how;
        g_DemoEndQpc.store(QpcNow(), std::memory_order_relaxed);
        g_MissionWarpPending.store(false, std::memory_order_relaxed);
        Log("[AttachInDemo] demo playback %s%s\n", how,
            g_Active.load(std::memory_order_relaxed) ? " with the attach still armed" : "");
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
            alignas(16) float rot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            MatrixToQuaternion(matrix16, rot);

            g_InOwnWarp.store(true, std::memory_order_relaxed);
            warp(sub, 0, pos, rot, false);
            g_InOwnWarp.store(false, std::memory_order_relaxed);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    struct TypeLayout
    {
        OwnerKind   kind;
        const char* label;
        uintptr_t   typeVtable;
        std::size_t instanceCount;
        std::size_t realizedBegin;
        std::size_t realizedEnd;
        std::size_t controller;
        uintptr_t   controllerVtable;
        std::size_t connectPointSlot;
        std::size_t controllerBase;
        std::size_t controllerCount;
    };

    std::size_t BuildTypeLayouts(TypeLayout* out, std::size_t capacity)
    {
        const TypeLayout all[] =
        {
            { OwnerKind::Horse, "horse", gAddr.GameObjectType_Horse2_Vtable,
              0x40, 0x58, 0x60, 0x10, gAddr.AnimationControllerImpl_Vtable,
              kSharedConnectPointSlot, kSharedControllerBase, kSharedControllerCount },
            { OwnerKind::Heli, "heli with its own animation controller", gAddr.GameObjectType_Heli2_Vtable,
              0x60, 0x48, 0x50, 0x08, gAddr.HeliAnimationControllerImpl_Vtable,
              kHeliConnectPointSlot, kHeliControllerBase, kHeliControllerCount },
            { OwnerKind::Heli, "heli with the shared animation controller", gAddr.GameObjectType_HeliAlt_Vtable,
              0x68, 0x80, 0x88, 0x10, gAddr.AnimationControllerImpl_Vtable,
              kSharedConnectPointSlot, kSharedControllerBase, kSharedControllerCount },
            { OwnerKind::WalkerGear, "walkergear", gAddr.GameObjectType_WalkerGear2_Vtable,
              0x70, 0x88, 0x90, 0x10, gAddr.AnimationControllerImpl_Vtable,
              kSharedConnectPointSlot, kSharedControllerBase, kSharedControllerCount },
        };

        std::size_t n = 0;
        for (const TypeLayout& layout : all)
        {
            if (n >= capacity)
                break;
            if (layout.typeVtable != 0 && layout.controllerVtable != 0)
                out[n++] = layout;
        }
        return n;
    }

    bool TypePlacementAvailable(OwnerKind kind)
    {
        if (!gAddr.Fox_GameObjectManager_GetSharedInstance)
            return false;

        TypeLayout layouts[4];
        const std::size_t n = BuildTypeLayouts(layouts, 4);
        for (std::size_t i = 0; i < n; ++i)
        {
            if (layouts[i].kind == kind)
                return true;
        }
        return false;
    }

    bool PlacementSupported(OwnerKind kind)
    {
        if (kind == OwnerKind::Vehicle)
            return gAddr.Vehicle_GetConnectPointWorldMatrix != 0;
        return TypePlacementAvailable(kind);
    }

    bool ReadTypeConnectPointMatrix(OwnerKind kind, std::uint32_t ownerObjectId,
                                    std::uint32_t cnpHash, float* outMatrix16,
                                    const char** reason)
    {
        auto getShared = reinterpret_cast<GetSharedInstance_t>(
            ResolveGameAddress(gAddr.Fox_GameObjectManager_GetSharedInstance));
        if (!getShared)
        {
            *reason = "the game object manager accessor is not ported for this build";
            return false;
        }

        TypeLayout layouts[4];
        const std::size_t layoutCount = BuildTypeLayouts(layouts, 4);

        __try
        {
            std::uint8_t* manager = reinterpret_cast<std::uint8_t*>(getShared());
            if (!manager)
            {
                *reason = "the game object manager is not up";
                return false;
            }

            void** managerVtbl = *reinterpret_cast<void***>(manager);
            auto getType = reinterpret_cast<GetTypeObject_t>(
                managerVtbl[kManagerGetTypeSlot / sizeof(void*)]);
            if (!getType)
            {
                *reason = "the game object manager has no type lookup";
                return false;
            }

            std::uint8_t* type = reinterpret_cast<std::uint8_t*>(
                getType(manager, static_cast<std::uint16_t>(ownerObjectId)));
            if (!type)
            {
                *reason = "no game object type is registered for the owner id";
                return false;
            }

            void* typeVtbl = *reinterpret_cast<void**>(type);
            const TypeLayout* layout = nullptr;
            for (std::size_t i = 0; i < layoutCount; ++i)
            {
                if (ResolveGameAddress(layouts[i].typeVtable) == typeVtbl)
                {
                    layout = &layouts[i];
                    break;
                }
            }
            if (!layout)
            {
                *reason = "the owner's type object has a layout this build does not know";
                return false;
            }
            if (layout->kind != kind)
            {
                *reason = "the owner's type object does not match the owner kind";
                return false;
            }

            const std::uint16_t typeId = *reinterpret_cast<std::uint16_t*>(type + kTypeIdOffset);
            if (typeId != static_cast<std::uint16_t>(ownerObjectId >> kTypeIdShift))
            {
                *reason = "the type object's id does not match the owner id";
                return false;
            }

            std::uint8_t* wrapper = *reinterpret_cast<std::uint8_t**>(type + kTypeWrapperOffset);
            if (!wrapper || *reinterpret_cast<std::uint8_t**>(wrapper + kWrapperTypeOffset) != type)
            {
                *reason = "the type object's back-link is inconsistent";
                return false;
            }

            const std::uint32_t index = ownerObjectId & kInstanceMask;
            const std::uint32_t instanceCount =
                *reinterpret_cast<std::uint32_t*>(type + layout->instanceCount);
            if (index >= instanceCount)
            {
                *reason = "the owner index is past the type's instance count";
                return false;
            }

            std::uint8_t** begin = *reinterpret_cast<std::uint8_t***>(type + layout->realizedBegin);
            std::uint8_t** end   = *reinterpret_cast<std::uint8_t***>(type + layout->realizedEnd);
            if (!begin || !end || end < begin
                || static_cast<std::size_t>(end - begin) > kMaxRealizedChunks)
            {
                *reason = "the owner's type has no realized instances";
                return false;
            }

            void* controllerVtbl = ResolveGameAddress(layout->controllerVtable);
            std::uint32_t chunk = 0;
            for (std::uint8_t** it = begin; it != end; ++it, ++chunk)
            {
                std::uint8_t* realized = *it;
                if (!realized)
                    continue;

                std::uint8_t* controller =
                    *reinterpret_cast<std::uint8_t**>(realized + layout->controller);
                if (!controller || *reinterpret_cast<void**>(controller) != controllerVtbl)
                    continue;

                const std::int32_t base =
                    *reinterpret_cast<std::int32_t*>(controller + layout->controllerBase);
                const std::uint32_t count =
                    *reinterpret_cast<std::uint32_t*>(controller + layout->controllerCount);
                if (base < 0 || index < static_cast<std::uint32_t>(base)
                    || index >= static_cast<std::uint32_t>(base) + count)
                    continue;

                auto query = reinterpret_cast<TypeCnpWorld_t>(
                    (*reinterpret_cast<void***>(controller))[layout->connectPointSlot / sizeof(void*)]);
                if (!query)
                {
                    *reason = "the owner's animation controller has no connect point query";
                    return false;
                }

                if (!query(controller, index, cnpHash, outMatrix16))
                {
                    *reason = "the connect point name is not on the owner's model";
                    return false;
                }

                if (!g_ReportedTypePath)
                {
                    g_ReportedTypePath = true;
                    Log("[AttachInDemo] '%s' resolved through the %s type object (type id 0x%02X, "
                        "realized chunk %u of %u, controller base %d count %u) - the connect point "
                        "query is the owner's own animation controller, not the vehicle system\n",
                        g_OwnerName.c_str(), layout->label, typeId, chunk + 1,
                        static_cast<unsigned>(end - begin), base, count);
                }
                return true;
            }

            *reason = "the owner instance is not realized right now";
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *reason = "a read faulted while walking the owner's type object";
            return false;
        }
    }

    bool ReadConnectPointMatrix(OwnerKind kind, std::uint32_t ownerObjectId, std::uint32_t cnpHash,
                                float* outMatrix16, const char** reason)
    {
        if (kind == OwnerKind::Vehicle)
        {
            if (ReadVehicleConnectPointMatrix(ownerObjectId, cnpHash, outMatrix16))
                return true;
            *reason = "the vehicle may be unrealized, or the connect-point name may not exist on "
                      "its model";
            return false;
        }
        return ReadTypeConnectPointMatrix(kind, ownerObjectId, cnpHash, outMatrix16, reason);
    }

    void __fastcall hkOnPlayingAfterUpdateStream(void* playback)
    {
        if (g_OrigOnPlayingAfterUpdateStream)
            g_OrigOnPlayingAfterUpdateStream(playback);
        if (g_Playback.exchange(playback, std::memory_order_relaxed) != playback)
            ResetGeneralFixState();
        g_DemoTickQpc.store(QpcNow(), std::memory_order_relaxed);

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
        OwnerKind     kind;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ownerObjectId   = g_OwnerObjectId;
            cnpHash         = g_ConnectPointId;
            unattachOnSleep = g_UnattachOnSleep;
            kind            = g_OwnerKind;
        }

        if (ownerObjectId == kInvalidId)
            return;

        if (unattachOnSleep && PlayerIsUnconscious())
        {
            if (g_Active.exchange(false, std::memory_order_relaxed))
                g_SleepReleasedAtMs.store(GetTickCount64(), std::memory_order_relaxed);
            g_HandoffDeadlineMs.store(0, std::memory_order_relaxed);
            return;
        }

        if (!PlacementSupported(kind))
        {
            if (!g_ReportedNoPlacementKind)
            {
                g_ReportedNoPlacementKind = true;
                Log("[AttachInDemo] '%s' is a %s owner and this build has no connect point path "
                    "for that type, so this attach only arms the boarding handoff at demo end\n",
                    g_OwnerName.c_str(), KindName(kind));
            }
            return;
        }

        alignas(16) float matrix[16] = {};
        const char* reason = "";
        if (!ReadConnectPointMatrix(kind, ownerObjectId, cnpHash, matrix, &reason))
        {
            if (!g_ReportedNoMatrix)
            {
                g_ReportedNoMatrix = true;
                Log("[AttachInDemo] '%s' (%s) has no reachable connect point this frame, so the "
                    "player is not being attached: %s\n",
                    g_OwnerName.c_str(), KindName(kind), reason);
            }
            return;
        }

        g_PerFrameHookQpc.store(QpcNow(), std::memory_order_relaxed);

        const std::uint64_t placedAt = g_StreamPlacedAtMs.load(std::memory_order_relaxed);
        const bool streamLive = placedAt && GetTickCount64() - placedAt < kStreamLiveWindowMs;
        if (streamLive)
        {
            if (!g_ReportedWarpRetired)
            {
                g_ReportedWarpRetired = true;
                Log("[AttachInDemo] '%s' stream-motion placement is live, so the per-frame warp "
                    "is retired - a warp is a teleport with no interpolation and it was fighting "
                    "the stream write every frame; it only returns if the stream write stops for "
                    "%llu ms\n",
                    g_OwnerName.c_str(), static_cast<unsigned long long>(kStreamLiveWindowMs));
            }
            return;
        }

        const bool warped = WarpPlayerTo(matrix);

        if (warped && !g_ReportedAttached)
        {
            g_ReportedAttached = true;
            Log("[AttachInDemo] '%s' connect point reached and the player was moved to it with the "
                "full connect-point rotation - if the player only appears there when the demo "
                "ends, the demo is re-driving the player transform after this write\n",
                g_OwnerName.c_str());
        }
    }

    bool PlaceDemoActor(void* self, std::uint32_t index, const float* matrix16)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(self);

            void* owner = *reinterpret_cast<void**>(base + kExecOwner);
            void* demoAnimCtl = *reinterpret_cast<void**>(base + kExecDemoAnimController);
            if (!owner || !demoAnimCtl)
                return false;

            auto* ownerBytes = reinterpret_cast<std::uint8_t*>(owner);

            void* animHolder = *reinterpret_cast<void**>(ownerBytes + kOwnerAnimHolder);
            void* playerSub  = *reinterpret_cast<void**>(ownerBytes + kOwnerPlayerSub);
            if (!animHolder || !playerSub)
                return false;

            void* animCtl = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(animHolder) + kAnimHolderController);
            if (!animCtl)
                return false;

            const std::uint32_t localIndex = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uint8_t*>(playerSub) + kPlayerSubLocalIndex);
            if (index != localIndex)
                return false;

            auto** demoVtbl = *reinterpret_cast<void***>(demoAnimCtl);
            if (!demoVtbl)
                return false;

            auto isPlaying = reinterpret_cast<IsPlaying_t>(
                demoVtbl[kDemoAnimIsPlayingSlot / sizeof(void*)]);
            if (!isPlaying || !isPlaying(demoAnimCtl, index))
                return false;

            auto** animVtbl = *reinterpret_cast<void***>(animCtl);
            if (!animVtbl)
                return false;

            auto updateStreamMotion = reinterpret_cast<UpdateStreamMotion_t>(
                animVtbl[kUpdateStreamMotionSlot / sizeof(void*)]);
            if (!updateStreamMotion)
                return false;

            alignas(16) float transformRT[12] = {};
            transformRT[0]  = 1.0f;
            transformRT[1]  = 1.0f;
            transformRT[2]  = 1.0f;
            transformRT[3]  = 1.0f;
            MatrixToQuaternion(matrix16, transformRT + 4);
            transformRT[8]  = matrix16[12];
            transformRT[9]  = matrix16[13];
            transformRT[10] = matrix16[14];
            transformRT[11] = 1.0f;

            updateStreamMotion(animCtl, index, transformRT);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ResetSummaryState()
    {
        g_SummaryWrites = 0;
        g_SummaryFirstQpc = 0;
        g_SummaryDistance = 0.0;
        g_ReportedSummary = false;
    }

    void NoteSummarySample(std::int64_t nowQpc, const float* matrix16)
    {
        if (g_SummaryWrites == 0)
            g_SummaryFirstQpc = nowQpc;
        else
            g_SummaryDistance += Distance3(matrix16 + 12, g_SummaryLastPos);
        g_SummaryLastPos[0] = matrix16[12];
        g_SummaryLastPos[1] = matrix16[13];
        g_SummaryLastPos[2] = matrix16[14];
        if (g_SummaryWrites < 1000000u)
            ++g_SummaryWrites;
        if (g_ReportedSummary || g_SummaryWrites < kSummaryWrites)
            return;
        g_ReportedSummary = true;
        const double spanMs = QpcToMs(nowQpc - g_SummaryFirstQpc);
        const double perWrite = spanMs / static_cast<double>(g_SummaryWrites - 1);
        Log("[AttachInDemo] placement summary after %u stream writes: %.1f ms per write, connect "
            "point %.2f m/s, demo anchor = the pose written one frame earlier on %u frame(s) / "
            "two on %u / unmatched on %u (rotation %s), camera fix at the commit %u, at the "
            "write %u, skipped %u, last correction %.3f m %.2f deg\n",
            g_SummaryWrites, perWrite, spanMs > 0.0 ? g_SummaryDistance * 1000.0 / spanMs : 0.0,
            g_AnchorLagOne, g_AnchorLagTwo, g_AnchorLagNone,
            g_AnchorMode == AnchorMode::Full ? "matches the written pose"
            : (g_AnchorMode == AnchorMode::YawOnly ? "matches the written yaw only"
               : (g_AnchorMode == AnchorMode::TranslationOnly ? "differs, translation only"
                                                               : "not verified")),
            g_CommitAfterWriteCount, g_CommitBeforeWriteCount, g_CommitStaticCount, g_LastFixLen,
            g_LastFixYawDeg);
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
        OwnerKind     kind;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ownerObjectId = g_OwnerObjectId;
            cnpHash       = g_ConnectPointId;
            kind          = g_OwnerKind;
        }

        if (ownerObjectId == kInvalidId || !PlacementSupported(kind))
            return;

        alignas(16) float matrix[16] = {};
        const char* reason = "";
        if (!ReadConnectPointMatrix(kind, ownerObjectId, cnpHash, matrix, &reason))
            return;

        if (!PlaceDemoActor(self, gameObjectIndex, matrix))
            return;

        NoteCamFixAtWrite(matrix);

        const std::int64_t nowQpc = QpcNow();
        g_StreamPlacedAtMs.store(GetTickCount64(), std::memory_order_relaxed);

        if (!g_ReportedPlaced)
        {
            g_ReportedPlaced = true;
            Log("[AttachInDemo] '%s' is being placed through the demo's own stream-motion slot - "
                "the player should now sit on the connect point during the cutscene, not only "
                "after it\n", g_OwnerName.c_str());
        }

        NoteSummarySample(nowQpc, matrix);
    }

    bool IsLocalPlayerActor(void* self, std::uint32_t gameObjectIndex)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(self);
            auto* owner = *reinterpret_cast<std::uint8_t**>(base + kExecOwner);
            if (!owner)
                return false;

            auto* playerSub = *reinterpret_cast<std::uint8_t**>(owner + kOwnerPlayerSub);
            if (!playerSub)
                return false;

            return *reinterpret_cast<std::uint32_t*>(playerSub + kPlayerSubLocalIndex)
                   == gameObjectIndex;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::uint64_t ReadSignalId(std::uint64_t* signal)
    {
        __try
        {
            return signal[0] & kSignalIdMask;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool PatchFinishPayload(std::uint64_t* signal, std::uint64_t lastState,
                            std::uint32_t ownerObjectId)
    {
        __try
        {
            signal[kSignalLastStateSlot] = lastState;
            *reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uint8_t*>(signal) + kSignalOpponentOffset)
                = static_cast<std::uint16_t>(ownerObjectId);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void TryPatchDemoFinish(void* self, std::uint32_t gameObjectIndex, std::uint64_t* signal)
    {
        if (!signal)
            return;

        const bool armed = g_Active.load(std::memory_order_relaxed)
                        || GetTickCount64() < g_HandoffDeadlineMs.load(std::memory_order_relaxed);
        if (!armed)
            return;

        if (MissionCodeGuard::ShouldBypassHooks())
            return;

        if (ReadSignalId(signal) != kSignalDemoFinish)
            return;

        std::uint32_t ownerObjectId;
        OwnerKind     kind;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            ownerObjectId = g_OwnerObjectId;
            kind          = g_OwnerKind;
        }

        const std::uint64_t lastState = HandoffStateForKind(kind);
        if (ownerObjectId == kInvalidId || lastState == 0)
            return;

        if (!IsLocalPlayerActor(self, gameObjectIndex))
            return;

        if (!PatchFinishPayload(signal, lastState, ownerObjectId))
            return;

        g_HandoffDeadlineMs.store(0, std::memory_order_relaxed);

        if (!g_ReportedHandoff)
        {
            g_ReportedHandoff = true;
            Log("[AttachInDemo] demo finish reached the local player with '%s' armed - the "
                "finish payload now names the %s ride state and opponent 0x%04X, so the "
                "engine's own boarding handoff seats the player on it\n",
                g_OwnerName.c_str(), KindName(kind), ownerObjectId);
        }
    }

    void __fastcall hkSequentialDemoActionOnSignal(void* self, std::uint32_t gameObjectIndex,
                                                   void* err, std::uint64_t* signal)
    {
        TryPatchDemoFinish(self, gameObjectIndex, signal);

        if (g_OrigDemoActionOnSignal)
            g_OrigDemoActionOnSignal(self, gameObjectIndex, err, signal);
    }

    void OpenHandoffWindow()
    {
        if (!g_Active.load(std::memory_order_relaxed))
            return;

        OwnerKind kind;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            kind = g_OwnerKind;
        }

        if (HandoffStateForKind(kind) != 0)
            g_HandoffDeadlineMs.store(GetTickCount64() + kHandoffWindowMs,
                                      std::memory_order_relaxed);
    }

    std::uint32_t __fastcall hkDoFinish(void* playback)
    {
        NoteDemoEnd("finished");
        OpenHandoffWindow();
        const std::uint32_t result = g_OrigDoFinish ? g_OrigDoFinish(playback) : 0;
        ReleaseAttach("demo finished");
        g_Playback.store(nullptr, std::memory_order_relaxed);
        ReportGeneralFixAtDemoEnd();
        return result;
    }

    std::uint32_t __fastcall hkDoInterrupt(void* playback)
    {
        NoteDemoEnd("was interrupted");
        OpenHandoffWindow();
        const std::uint32_t result = g_OrigDoInterrupt ? g_OrigDoInterrupt(playback) : 0;
        ReleaseAttach("demo interrupted");
        g_Playback.store(nullptr, std::memory_order_relaxed);
        ReportGeneralFixAtDemoEnd();
        return result;
    }

    bool ArmAttach(const char* label, std::uint32_t ownerObjectId, const char* connectPointName,
                   bool unattachOnSleep, OwnerKind kind)
    {
        if (ownerObjectId == kInvalidId || !connectPointName || !*connectPointName)
            return false;

        std::string previousOwner;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            previousOwner     = g_OwnerName;
            g_OwnerName       = label;
            g_OwnerObjectId   = ownerObjectId;
            g_ConnectPointId  = FoxHashes::StrCode32(connectPointName);
            g_UnattachOnSleep = unattachOnSleep;
            g_OwnerKind       = kind;
        }

        g_ReportedNoMatrix        = false;
        g_ReportedHookLive        = false;
        g_ReportedAttached        = false;
        g_ReportedPlaced          = false;
        g_ReportedNoPlacementKind = false;
        g_ReportedHandoff         = false;
        g_ReportedTypePath        = false;
        g_ReportedWarpRetired     = false;
        g_StreamPlacedAtMs.store(0, std::memory_order_relaxed);
        ReadAttachTuning();
        ResetCamFixState();
        ResetSummaryState();
        g_PerFrameHookQpc.store(0, std::memory_order_relaxed);
        g_HandoffDeadlineMs.store(0, std::memory_order_relaxed);
        const std::uint64_t sleepReleasedAt =
            g_SleepReleasedAtMs.exchange(0, std::memory_order_relaxed);
        g_Active.store(true, std::memory_order_relaxed);

        char previous[256] = "";
        if (sleepReleasedAt != 0)
        {
            const std::uint64_t now = GetTickCount64();
            std::snprintf(previous, sizeof(previous),
                          " - the previous attach '%s' had been released %llu ms ago when the "
                          "player went unconscious with unattachOnSleep set and no demo end "
                          "reported it",
                          previousOwner.c_str(),
                          static_cast<unsigned long long>(now > sleepReleasedAt ? now - sleepReleasedAt : 0));
        }

        Log("[AttachInDemo] armed: owner=%s kind=%s connectPoint hash=0x%08X unattachOnSleep=%d "
            "boardingHandoff=%s placement=%s%s\n",
            label, KindName(kind), g_ConnectPointId, unattachOnSleep ? 1 : 0,
            HandoffStateForKind(kind) != 0 ? "yes" : "no",
            PlacementSupported(kind) ? "yes" : "no", previous);
        EnsurePlayerWarpHook();
        return true;
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
    char label[32] = {};
    std::snprintf(label, sizeof(label), "0x%04X", ownerObjectId);

    OwnerKind kind = OwnerKind::Other;
    if (ownerObjectId == kInvalidId || !ResolveOwnerKindById(ownerObjectId, kind))
    {
        Log("[AttachInDemo] %s is not a live TppVehicle2, TppHorse2, TppHeli2 or TppWalkerGear2 "
            "instance, so nothing was attached\n", label);
        return false;
    }

    return ArmAttach(label, ownerObjectId, connectPointName, unattachOnSleep, kind);
}

bool RequestToAttachInDemo(const char* ownerName, const char* connectPointName,
                           bool unattachOnSleep)
{
    if (!ownerName || !*ownerName || !connectPointName || !*connectPointName)
        return false;

    std::uint32_t ownerObjectId = kInvalidId;
    OwnerKind     kind          = OwnerKind::Other;
    if (!ResolveOwnerByName(ownerName, ownerObjectId, kind))
    {
        Log("[AttachInDemo] '%s' did not resolve as a TppVehicle2, TppHorse2, TppHeli2 or "
            "TppWalkerGear2 game object, so nothing was attached - pass the game object id "
            "instead if it has no locator name\n", ownerName);
        return false;
    }

    return ArmAttach(ownerName, ownerObjectId, connectPointName, unattachOnSleep, kind);
}

void ClearAttachInDemo()
{
    ReleaseAttach("V_Player.ClearAttachInDemo called");
    g_HandoffDeadlineMs.store(0, std::memory_order_relaxed);
}

bool Install_PlayerAttachInDemo_Hook()
{
    if (g_HookTarget)
        return true;

    if (!gAddr.DemoPlayback_OnPlayingAfterUpdateStream
        || !gAddr.Vehicle_GetConnectPointWorldMatrix)
    {
        LogDebug("[AttachInDemo] the demo and vehicle addresses are not ported for this build, so "
                 "V_Player.RequestToAttachInDemo is accepted but the player is never attached\n");
        return true;
    }

    Log("[AttachInDemo] type-object connect point path: horse=%s heli=%s walkergear=%s - those "
        "owners can hold the player on a connect point during the demo, not only board him "
        "after it\n",
        TypePlacementAvailable(OwnerKind::Horse) ? "armed" : "unported",
        TypePlacementAvailable(OwnerKind::Heli) ? "armed" : "unported",
        TypePlacementAvailable(OwnerKind::WalkerGear) ? "armed" : "unported");

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

    if (gAddr.Player_SequentialDemoActionOnSignal)
    {
        void* onSignal = ResolveGameAddress(gAddr.Player_SequentialDemoActionOnSignal);
        if (onSignal && CreateAndEnableHook(onSignal, &hkSequentialDemoActionOnSignal,
                                            reinterpret_cast<void**>(&g_OrigDemoActionOnSignal)))
            g_HookDemoOnSignal = onSignal;
    }

    InstallCameraCommitHook();
    InstallGetControlRootHook();

    if (!g_HookDemoExecute)
    {
        Log("[AttachInDemo] the demo player-placement hook did not install, so the player will "
            "only appear at the connect point once the demo ends - the cutscene's own stream "
            "motion owns the model until then\n");
    }

    if (!g_HookDemoOnSignal)
    {
        Log("[AttachInDemo] the demo finish-signal hook did not install, so a horse, heli or "
            "walkergear owner is never boarded when its demo ends\n");
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
    g_Active.store(false, std::memory_order_relaxed);
    g_HandoffDeadlineMs.store(0, std::memory_order_relaxed);
    g_SleepReleasedAtMs.store(0, std::memory_order_relaxed);
    RestorePlayerWarpHook();

    if (g_HookTarget)
        DisableAndRemoveHook(g_HookTarget);
    if (g_HookFinish)
        DisableAndRemoveHook(g_HookFinish);
    if (g_HookInterrupt)
        DisableAndRemoveHook(g_HookInterrupt);

    if (g_HookDemoExecute)
        DisableAndRemoveHook(g_HookDemoExecute);
    if (g_HookDemoOnSignal)
        DisableAndRemoveHook(g_HookDemoOnSignal);
    if (g_HookCameraCommit)
        DisableAndRemoveHook(g_HookCameraCommit);
    if (g_HookGetControlRoot)
        DisableAndRemoveHook(g_HookGetControlRoot);

    g_HookTarget       = nullptr;
    g_HookFinish       = nullptr;
    g_HookInterrupt    = nullptr;
    g_HookDemoExecute  = nullptr;
    g_HookDemoOnSignal = nullptr;
    g_HookCameraCommit = nullptr;
    g_OrigCameraCommit = nullptr;
    g_HookGetControlRoot = nullptr;
    g_OrigGetControlRoot = nullptr;
    g_Playback.store(nullptr, std::memory_order_relaxed);
    g_OrigDemoActionExecute  = nullptr;
    g_OrigDemoActionOnSignal = nullptr;
    g_OrigOnPlayingAfterUpdateStream = nullptr;
    g_OrigDoFinish    = nullptr;
    g_OrigDoInterrupt = nullptr;
    g_PlayerImpl.store(nullptr, std::memory_order_relaxed);
    return true;
}
