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
    constexpr std::uint64_t kDescWord0      = 0xFFFFFFFFFFFFFFFFull;
    constexpr std::uint64_t kDescWord1      = 0ull;
    constexpr std::uint64_t kDescWord2      = 0ull;
    constexpr std::uint64_t kDescWord3      = 0x2ull;
    constexpr int           kDescRecords    = 128;
    constexpr std::size_t   kDescRecordSize = 0x60;
    constexpr std::size_t   kDescBytes      = 0x70 + kDescRecords * kDescRecordSize;
    constexpr std::uint64_t kDescCapacity   = 0x7f0000ull;

    constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

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

    std::uint8_t* ResolveGeoQuerySeh()
    {
        auto qst = reinterpret_cast<QuarkTable_t>(ResolveGameAddress(gAddr.Fox_GetQuarkSystemTable));
        if (!qst)
            return nullptr;

        __try
        {
            std::uint8_t* table = qst();
            if (!table)
                return nullptr;

            std::uint8_t* holder = *reinterpret_cast<std::uint8_t**>(table + 0x80);
            if (!holder)
                return nullptr;

            return *reinterpret_cast<std::uint8_t**>(holder + 0x08);
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

    bool SphereOverlapSeh(std::uint8_t* geo, std::uint8_t* desc, const Vec4* center, float radius,
                          int* outHits)
    {
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(geo);
            if (!vtbl || !vtbl[0])
            {
                return false;
            }

            *outHits = reinterpret_cast<GeoSphere_t>(vtbl[0])(geo, desc, kGeoQueryMask,
                                                              center, radius);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
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

    std::uint8_t* geo = ResolveGeoQuerySeh();
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

    const float yaw  = rotY * kDegreesToRadians;
    const float sinY = std::sin(yaw);
    const float cosY = std::cos(yaw);

    const int nx = StepCount(maxX - minX);
    const int nz = StepCount(maxZ - minZ);

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
                return false;
            }

            if (hits > 0)
            {
                return false;
            }
        }
    }

    return true;
}
