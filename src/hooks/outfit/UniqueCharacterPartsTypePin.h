#pragma once

#include <cstdint>

namespace uniquecharpin
{
    bool Install();
    void Uninstall();
    void SyncSupport(std::uint8_t livePartsType, std::uint8_t liveSelector,
                     bool bypass);

    void RememberCustom(std::uint8_t playerType, std::uint8_t partsType,
                        std::uint8_t selectorCode);
    void ForgetCustom(std::uint8_t playerType);
    bool TryGetRemembered(std::uint8_t playerType,
                          std::uint8_t* outPartsType,
                          std::uint8_t* outSelectorCode);
    void ReassertAfterRestore();

    bool IsOwnSuitPartsType(std::uint8_t playerType,
                            std::uint8_t partsType);

}
