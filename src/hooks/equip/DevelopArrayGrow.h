#pragma once

#include <cstddef>
#include <cstdint>

namespace equip
{
    void PreApplyDevelopArrayGrowPatches();
    bool Install_DevelopArrayGrow();
    void Uninstall_DevelopArrayGrow();

    bool DevelopArrayGrowActive();

    std::uint32_t NativeFlowBound();

    constexpr std::uint16_t kFlowSentinel      = 0x400;
    constexpr std::uint16_t kGunsmithFlowFirst = 0x3FD;
    constexpr std::uint16_t kGunsmithFlowLast  = 0x3FF;
    constexpr std::uint32_t kMaxFlowSlots      = 65535;

    bool GunsmithFlowRowClaimed(std::uint32_t row);

    inline bool IsReservedFlowRow(std::uint32_t v)
    {
        if (v == kFlowSentinel)
            return true;
        if (v < kGunsmithFlowFirst || v > kGunsmithFlowLast)
            return false;
        return GunsmithFlowRowClaimed(v);
    }

    std::size_t DevFlagsPtrOffsetBase20();

    void SilenceDevelopRowAnnounce(std::int32_t developId);

    void AssertDevelopRowAnnounced(std::int32_t developId,
                                   std::int32_t flowIndex);

    void InvalidateDevelopVisibilityCache();
    void InvalidateDevelopLookupIndex();
    void DevelopLookupTakeCounters(unsigned long long& calls,
                                   unsigned long long& indexed,
                                   unsigned long long& builds,
                                   unsigned long long& stale);
    void BeginDevelopVisibilityCache();
    void EndDevelopVisibilityCache();
    void LogDevelopScanCounters(const char* phase);

    struct DevelopVisibilityScope
    {
        DevelopVisibilityScope();
        ~DevelopVisibilityScope();
        bool owned;
    };

    void DevelopVisibilityTakeCounters(unsigned long long& calls,
                                       unsigned long long& hits);

    inline bool IsValidFlowIndex(std::uint32_t v)
    {
        return v < NativeFlowBound() && !IsReservedFlowRow(v);
    }

    void DevFlagsWriteByte(void* controller, std::uint32_t index,
                           std::uint8_t value);

    bool DevFlagsTryReadByte(void* controller, std::uint32_t index,
                             std::uint8_t& out);

    std::uint32_t FirstCustomFlowIndex();

    bool TryReadRowDevelopId(std::uint32_t index, std::uint32_t& out);

    void EnsureDevelopBlockArmed(void* base20);

    std::size_t CollectVanillaIdentityEquipIds(std::int32_t* out,
                                               std::size_t capacity);

    void SyncDevelopFlagsWithSave();

}
