#pragma once

#include <cstdint>

bool Install_LostHostageDiscovery_Hooks();
bool Uninstall_LostHostageDiscovery_Hooks();

void Add_LostHostageDiscovery(std::uint32_t gameObjectId, int hostageType);
void Remove_LostHostageDiscovery(std::uint32_t gameObjectId);
void Clear_LostHostageDiscovery();
void Dump_LostHostageDiscovery();

// Called from LostHostageHook's shared RadioRequest hook.
// Params: self (void*), actionIndex (int), stateProc (int)
void LostHostageDiscovery_OnRadioRequest(void* self, int actionIndex, int stateProc);

// Called from LostHostageHook's shared ConvertRadioTypeToSpeechLabel hook.
// Params: radioType (uint8_t), baseLabel (uint32_t)
std::uint32_t LostHostageDiscovery_OnConvertRadioTypeToSpeechLabel(
    std::uint8_t radioType,
    std::uint32_t baseLabel);
