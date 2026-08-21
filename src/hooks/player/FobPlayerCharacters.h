#pragma once

#include <cstdint>

namespace fobchars
{
    constexpr std::uint8_t kPlayerType_Avatar = 3;
    constexpr std::uint8_t kPlayerType_Ocelot = 5;
    constexpr std::uint8_t kPlayerType_Quiet  = 6;

    void NoteLoadoutPlayerType(std::uint8_t playerType, std::uint32_t flags);
    void ReassertSelectedCharacter();
}

bool Install_FobPlayerCharacters_Patches();
void Uninstall_FobPlayerCharacters_Patches();
