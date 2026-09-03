#pragma once


#include <cstdint>
#include <string>
#include <vector>


namespace SoldierAkObjIdMap
{
    bool Install();
    bool Uninstall();
    std::string GetEmitterNameForAkObjId(std::uint32_t akObjId);
    std::vector<std::uint32_t> GetAkObjIdsForObject(void* object);
    std::vector<std::uint32_t> GetAkObjIdsForControl(void* control);
    std::vector<std::uint32_t> GetAllSoldierVoiceAkObjIds();
    void SetActiveSoldierVoiceCents(float cents);
    void ClearActiveSoldierVoiceCents();
    void SetPitchForControl(void* control, float cents);
    void ClearPitchForControl(void* control);
    void SetDesiredPitchForGoId(std::uint32_t goId, float cents);
    void ClearDesiredPitchForGoId(std::uint32_t goId);
    void ClearAllDesiredPitches();
    std::vector<std::uint32_t> GetAkObjIdsForGoId(std::uint32_t goId);
    void SetCommandPostVoiceCents(std::uint32_t cpIndex, float cents);
    void ClearCommandPostVoiceCents();
    std::vector<std::uint32_t> GetCommandPostAkObjIds();
}