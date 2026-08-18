#pragma once

#include <cstdint>

namespace SoldierNoticeIgnore
{
    constexpr std::uint8_t kPlayer       = 1u << 0;
    constexpr std::uint8_t kHostage      = 1u << 2;
    constexpr std::uint8_t kNoticeObject = 1u << 3;
    constexpr std::uint8_t kCBox         = 1u << 4;
    constexpr std::uint8_t kNoise        = 1u << 5;
}

bool Install_CheckSightNoticePlayer_Hook();
bool Uninstall_CheckSightNoticePlayer_Hook();

void Set_SoldierNoticeIgnoreMask(std::uint32_t gameObjectId, std::uint8_t mask);
bool Soldier_IgnoresNotice(std::uint32_t soldierIndex, std::uint8_t flag);
bool SoldierNotice_ShouldDropNotice(std::uint32_t soldierIndex, std::uint8_t noticeType,
                                    const void* returnAddress);
void Clear_AllSoldierIgnorePlayer();
