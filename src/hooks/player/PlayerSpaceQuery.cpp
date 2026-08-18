#include "pch.h"

#include <Windows.h>
#include <cmath>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "PlayerSpaceQuery.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kGeoQueryMask   = 0x00060700u;
    constexpr std::uint64_t kDescWord0      = 0x20ull;
    constexpr std::uint64_t kDescWord1      = 0x10000000010ull;
    constexpr std::uint64_t kDescWord2      = 0ull;
    constexpr std::uint64_t kDescWord3      = 0x80000006ull;
    constexpr int           kDescRecords    = 128;
    constexpr std::size_t   kDescRecordSize = 0x60;
    constexpr std::size_t   kDescBytes      = 0x70 + kDescRecords * kDescRecordSize;
    constexpr std::uint64_t kDescCapacity   = 0x7f0000ull;

    constexpr float kSampleHeight = 1.0f;
    constexpr float kSampleRadius = 0.65f;
    constexpr float kSampleStep   = 0.9f;
    constexpr int   kMaxSteps     = 12;

    struct alignas(16) Vec4
    {
        float x;
        float y;
        float z;
        float w;
    };

    using QuarkTable_t = std::uint8_t* (__fastcall*)();
    using GeoSphere_t  = int(__fastcall*)(void* self, void* desc, std::uint32_t mask,
                                          const Vec4* center, float radius);

    struct GeoDiag
    {
        void* table;
        void* holder;
        void* geo;
        void* vtbl0;
    };

    std::uint8_t* ResolveGeoQuerySeh(GeoDiag* diag)
    {
        diag->table  = nullptr;
        diag->holder = nullptr;
        diag->geo    = nullptr;
        diag->vtbl0  = nullptr;

        auto qst = reinterpret_cast<QuarkTable_t>(ResolveGameAddress(gAddr.Fox_GetQuarkSystemTable));
        if (!qst)
            return nullptr;

        __try
        {
            std::uint8_t* table = qst();
            diag->table = table;
            if (!table)
                return nullptr;

            std::uint8_t* holder = *reinterpret_cast<std::uint8_t**>(table + 0x80);
            diag->holder = holder;
            if (!holder)
                return nullptr;

            std::uint8_t* geo = *reinterpret_cast<std::uint8_t**>(holder + 0x08);
            diag->geo = geo;
            if (!geo)
                return nullptr;

            void** vtbl = *reinterpret_cast<void***>(geo);
            if (vtbl)
                diag->vtbl0 = vtbl[0];
            return geo;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void InitQueryDesc(std::uint8_t* d)
    {
        for (std::size_t i = 0; i < kDescBytes; ++i)
            d[i] = 0;

        *reinterpret_cast<std::int32_t*>(d + 0x3c)  = -1;
        *reinterpret_cast<std::int32_t*>(d + 0x64)  = -1;
        *reinterpret_cast<std::uint64_t*>(d + 0x68) = kDescCapacity;

        std::uint8_t* r = d + 0x70;
        for (int i = 0; i < kDescRecords; ++i)
        {
            *reinterpret_cast<std::uint32_t*>(r + 0x0c) = 0xFFFF0000u;
            *reinterpret_cast<std::uint16_t*>(r + 0x1c) = 0xFFFFu;
            *reinterpret_cast<std::uint16_t*>(r + 0x1e) = 0x00FFu;
            *reinterpret_cast<std::uint32_t*>(r + 0x2c) = 0xFF7FFFFFu;
            r += kDescRecordSize;
        }

        *reinterpret_cast<std::uint64_t*>(d + 0x00) = kDescWord0;
        *reinterpret_cast<std::uint64_t*>(d + 0x08) = kDescWord1;
        *reinterpret_cast<std::uint64_t*>(d + 0x10) = kDescWord2;
        *reinterpret_cast<std::uint64_t*>(d + 0x18) = kDescWord3;
    }

    using SphereEntry_t = int(__fastcall*)(void* desc, std::uint32_t mask, const Vec4* center,
                                           float radius);

    SphereEntry_t     g_OrigSphereEntry = nullptr;
    thread_local int  t_ourQuery        = 0;
    long long         g_engineCalls     = 0;
    long long         g_engineHits      = 0;
    int               g_engineLogged    = 0;

    void DumpDescSeh(const char* tag, const void* descPtr)
    {
        __try
        {
            const std::uint8_t* d = static_cast<const std::uint8_t*>(descPtr);
            const auto q = [d](std::size_t o) {
                return *reinterpret_cast<const std::uint64_t*>(d + o);
            };
            const auto w = [d](std::size_t o) {
                return *reinterpret_cast<const std::uint32_t*>(d + o);
            };
            Log("[PlayerSpaceDiag] %s desc=%p +00=%016llX +08=%016llX +10=%016llX +18=%016llX\n",
                tag, descPtr, q(0x00), q(0x08), q(0x10), q(0x18));
            Log("[PlayerSpaceDiag] %s      +20=%016llX +28=%016llX +30=%08X +34=%08X +38=%08X "
                "+3c=%08X\n",
                tag, q(0x20), q(0x28), w(0x30), w(0x34), w(0x38), w(0x3c));
            Log("[PlayerSpaceDiag] %s      +40=%016llX +48=%016llX +50=%016llX +58=%016llX "
                "+60=%08X +64=%08X +68=%016llX\n",
                tag, q(0x40), q(0x48), q(0x50), q(0x58), w(0x60), w(0x64), q(0x68));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[PlayerSpaceDiag] %s desc=%p UNREADABLE\n", tag, descPtr);
        }
    }

    int __fastcall hk_SphereEntry(void* desc, std::uint32_t mask, const Vec4* center, float radius)
    {
        const int r = g_OrigSphereEntry(desc, mask, center, radius);
        if (t_ourQuery == 0)
        {
            ++g_engineCalls;
            if (r > 0)
            {
                ++g_engineHits;
                if (g_engineLogged < 3)
                {
                    ++g_engineLogged;
                    Log("[PlayerSpaceDiag] ENGINE sphere query HIT: mask=0x%08X r=%.3f "
                        "center=(%.3f,%.3f,%.3f) -> hits=%d\n",
                        mask, radius, center ? center->x : 0.0f, center ? center->y : 0.0f,
                        center ? center->z : 0.0f, r);
                    DumpDescSeh("ENGINE", desc);
                }
            }
        }
        return r;
    }

    void* ResolveSphereEntrySeh(std::uint8_t* geo)
    {
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(geo);
            if (!vtbl || !vtbl[0])
                return nullptr;

            std::uint8_t* thunk = static_cast<std::uint8_t*>(vtbl[0]);
            if (thunk[0x11] != 0xE9)
                return nullptr;

            const std::int32_t rel = *reinterpret_cast<const std::int32_t*>(thunk + 0x12);
            return thunk + 0x16 + rel;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void EnsureEngineProbe(std::uint8_t* geo)
    {
        static bool tried = false;
        if (tried)
            return;
        tried = true;

        void* target = ResolveSphereEntrySeh(geo);
        if (!target)
        {
            Log("[PlayerSpaceDiag] engine-probe: could not follow the vtbl[0] thunk to the shared "
                "sphere-query entry - engine-vs-mine comparison unavailable\n");
            return;
        }

        const bool ok = CreateAndEnableHook(target, &hk_SphereEntry,
                                            reinterpret_cast<void**>(&g_OrigSphereEntry));
        Log("[PlayerSpaceDiag] engine-probe: shared sphere entry=%p hook=%d\n", target, ok ? 1 : 0);
    }

    bool SphereOverlapSeh(std::uint8_t* geo, std::uint8_t* desc, const Vec4* center, float radius,
                          int* outHits)
    {
        ++t_ourQuery;
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(geo);
            if (!vtbl || !vtbl[0])
            {
                --t_ourQuery;
                return false;
            }

            *outHits = reinterpret_cast<GeoSphere_t>(vtbl[0])(geo, desc, kGeoQueryMask,
                                                              center, radius);
            --t_ourQuery;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            --t_ourQuery;
            return false;
        }
    }

    int StepCount(float span)
    {
        if (span <= 0.0f)
            return 0;

        int n = static_cast<int>(span / kSampleStep);
        if (static_cast<float>(n) * kSampleStep < span)
            ++n;
        if (n < 1)
            n = 1;
        if (n > kMaxSteps)
            n = kMaxSteps;
        return n;
    }
}

bool PlayerSpace_IsClearAround(float minX, float maxX, float minZ, float maxZ,
                               float posX, float posY, float posZ, float rotY)
{
    MISSION_GUARD_RETURN_FALSE();

    if (maxX < minX)
    {
        const float t = minX;
        minX = maxX;
        maxX = t;
    }
    if (maxZ < minZ)
    {
        const float t = minZ;
        minZ = maxZ;
        maxZ = t;
    }

    GeoDiag       diag{};
    std::uint8_t* geo = ResolveGeoQuerySeh(&diag);

    static std::uint64_t s_lastDiagTick = 0;
    const std::uint64_t  diagNow        = GetTickCount64();
    const bool           diagOn         = (s_lastDiagTick == 0 || diagNow - s_lastDiagTick >= 1000);
    if (diagOn)
        s_lastDiagTick = diagNow;

    if (geo)
        EnsureEngineProbe(geo);

    if (diagOn)
        Log("[PlayerSpaceDiag] table=%p holder(+0x80)=%p geo(+0x8)=%p vtbl[0]=%p | engine sphere "
            "queries so far: calls=%lld hits=%lld\n",
            diag.table, diag.holder, diag.geo, diag.vtbl0, g_engineCalls, g_engineHits);

    if (!geo)
    {
        static std::uint64_t s_lastTick = 0;
        const std::uint64_t  now        = GetTickCount64();
        if (s_lastTick == 0 || now - s_lastTick >= 5000)
        {
            s_lastTick = now;
            Log("[PlayerSpace] ERROR: the fox collision query object is unavailable, so "
                "V_Player.IsThereEnoughSpaceAroundPlayer cannot test the world and answers "
                "'not enough space' - callers gated on it will take their blocked branch.\n");
        }
        return false;
    }

    alignas(16) std::uint8_t desc[kDescBytes];
    InitQueryDesc(desc);

    const float sinY = std::sin(rotY);
    const float cosY = std::cos(rotY);

    const int nx = StepCount(maxX - minX);
    const int nz = StepCount(maxZ - minZ);

    if (diagOn)
    {
        Log("[PlayerSpaceDiag] pos=(%.3f,%.3f,%.3f) rotY=%.4f box=[%.2f..%.2f]x[%.2f..%.2f] "
            "nx=%d nz=%d samples=%d\n",
            posX, posY, posZ, rotY, minX, maxX, minZ, maxZ, nx, nz, (nx + 1) * (nz + 1));

        Vec4 ctrl;
        ctrl.x = posX;
        ctrl.y = posY + kSampleHeight;
        ctrl.z = posZ;
        ctrl.w = 1.0f;

        DumpDescSeh("MINE  ", desc);

        const float radii[4] = { 1.0f, 10.0f, 50.0f, 5000.0f };
        for (int i = 0; i < 4; ++i)
        {
            InitQueryDesc(desc);
            int        ctrlHits = -1;
            const bool ctrlOk   = SphereOverlapSeh(geo, desc, &ctrl, radii[i], &ctrlHits);
            Log("[PlayerSpaceDiag] control probe at player r=%.1f -> ok=%d hits=%d count=%d "
                "index=%d cap=%u\n",
                radii[i], ctrlOk ? 1 : 0, ctrlHits,
                *reinterpret_cast<const std::int32_t*>(desc + 0x60),
                *reinterpret_cast<const std::int32_t*>(desc + 0x64),
                *reinterpret_cast<const std::uint16_t*>(desc + 0x6a));
        }

        InitQueryDesc(desc);
    }

    int diagMaxHits    = 0;
    int diagFailedCall = 0;

    for (int iz = 0; iz <= nz; ++iz)
    {
        const float tz     = (nz == 0) ? 0.0f : static_cast<float>(iz) / static_cast<float>(nz);
        const float localZ = minZ + (maxZ - minZ) * tz;

        for (int ix = 0; ix <= nx; ++ix)
        {
            const float tx     = (nx == 0) ? 0.0f : static_cast<float>(ix) / static_cast<float>(nx);
            const float localX = minX + (maxX - minX) * tx;

            Vec4 center;
            center.x = posX + localX * cosY + localZ * sinY;
            center.y = posY + kSampleHeight;
            center.z = posZ - localX * sinY + localZ * cosY;
            center.w = 1.0f;

            int hits = 0;
            if (!SphereOverlapSeh(geo, desc, &center, kSampleRadius, &hits))
            {
                if (diagOn)
                    Log("[PlayerSpaceDiag] sample(%d,%d) center=(%.3f,%.3f,%.3f) -> CALL FAILED\n",
                        ix, iz, center.x, center.y, center.z);
                return false;
            }

            if (diagOn && ix == 0 && iz == 0)
                Log("[PlayerSpaceDiag] sample(0,0) center=(%.3f,%.3f,%.3f) r=%.2f -> hits=%d\n",
                    center.x, center.y, center.z, kSampleRadius, hits);

            if (hits > diagMaxHits)
                diagMaxHits = hits;

            if (hits > 0)
            {
                if (diagOn)
                    Log("[PlayerSpaceDiag] BLOCKED at sample(%d,%d) center=(%.3f,%.3f,%.3f) "
                        "hits=%d -> returning false\n",
                        ix, iz, center.x, center.y, center.z, hits);
                return false;
            }
        }
    }

    if (diagOn)
        Log("[PlayerSpaceDiag] all %d samples clear (maxHits=%d, failedCalls=%d) -> returning true\n",
            (nx + 1) * (nz + 1), diagMaxHits, diagFailedCall);

    return true;
}
