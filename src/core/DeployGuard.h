#pragma once

#include <cstdint>

namespace DeployGuard
{
    void Init();
    void NoteLoadoutSlot(std::uint32_t slot, const std::int32_t* ids);
    void OnMissionCode(std::uint32_t code);
    void OnPlayerLive();
    void OnCleanExit();
    void ForceDropExtendedIds();
    bool ShouldDropExtendedIds();
}
