#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "HostageGender.h"

namespace
{
    using GetQuarkSystemTable_t = std::uint8_t* (__fastcall*)();

    constexpr std::size_t kQuarkTableApplicationSystem = 0x98;
    constexpr std::size_t kAppSystemHostageServices    = 0x390;
    constexpr std::size_t kInfoServiceRecords          = 0x08;
    constexpr std::size_t kRecordStride                = 0x10;

    constexpr std::uint32_t kObjectIndexMask = 0x1FFu;
    constexpr int           kObjectTypeShift = 9;

    constexpr int kHostageTypeFirst = 3;
    constexpr int kHostageTypeLast  = 0xB;
    constexpr int kHostageKindCount = 9;

    constexpr int kBitFemale = 34;
    constexpr int kBitChild  = 55;

    int KindFromObjectType(int objectType)
    {
        if (objectType < kHostageTypeFirst || objectType > kHostageTypeLast)
            return -1;
        return objectType - kHostageTypeFirst;
    }
}

int HostageGender::Read(std::uint32_t gameObjectId)
{
    const int kind = KindFromObjectType(static_cast<int>(gameObjectId >> kObjectTypeShift));
    if (kind < 0 || kind >= kHostageKindCount)
        return kUnknown;

    auto qst = reinterpret_cast<GetQuarkSystemTable_t>(
        ResolveGameAddress(gAddr.Fox_GetQuarkSystemTable));
    if (!qst)
        return kUnknown;

    __try
    {
        std::uint8_t* table = qst();
        if (!table)
            return kUnknown;

        auto* appSystem = *reinterpret_cast<std::uint8_t**>(table + kQuarkTableApplicationSystem);
        if (!appSystem)
            return kUnknown;

        auto* service = *reinterpret_cast<std::uint8_t**>(
            appSystem + kAppSystemHostageServices + static_cast<std::size_t>(kind) * 8);
        if (!service)
            return kUnknown;

        auto* records = *reinterpret_cast<std::uint8_t**>(service + kInfoServiceRecords);
        if (!records)
            return kUnknown;

        const std::size_t index = gameObjectId & kObjectIndexMask;
        const std::uint64_t record =
            *reinterpret_cast<std::uint64_t*>(records + index * kRecordStride);

        if ((record >> kBitChild) & 1ull)
            return kChild;
        if ((record >> kBitFemale) & 1ull)
            return kFemale;
        return kMale;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return kUnknown;
    }
}
