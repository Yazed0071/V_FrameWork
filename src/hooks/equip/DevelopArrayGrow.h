#pragma once

#include <cstdint>

namespace equip
{
    void PreApplyDevelopArrayGrowPatches();
    bool Install_DevelopArrayGrow();
    void Uninstall_DevelopArrayGrow();

    bool DevelopArrayGrowActive();

    std::uint32_t NativeFlowBound();

    constexpr std::uint16_t kFlowSentinel   = 0x400;
    constexpr std::uint32_t kMaxFlowSlots   = 4096;

    std::size_t DevFlagsPtrOffsetBase20();

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
        return v < NativeFlowBound() && v != kFlowSentinel;
    }

    void DevFlagsWriteByte(void* controller, std::uint32_t index,
                           std::uint8_t value);

    bool DevFlagsTryReadByte(void* controller, std::uint32_t index,
                             std::uint8_t& out);

    void EnsureDevelopBlockArmed(void* base20);

    void SyncDevelopFlagsWithSave();

}
