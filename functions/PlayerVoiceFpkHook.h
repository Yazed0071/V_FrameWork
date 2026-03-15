#pragma once

#include <cstdint>

bool Install_PlayerVoiceFpk_Hook();
bool Uninstall_PlayerVoiceFpk_Hook();

//playerType:
//   1 = male DD
//   2 = female DD
//   anything else = fallback/non-DD
void Set_PlayerVoiceFpkPathForType(std::uint32_t playerType, const char* rawPath);

//playerType:
//   1 = male DD
//   2 = female DD
//   anything else = fallback/non-DD
void Clear_PlayerVoiceFpkPathForType(std::uint32_t playerType);

void Clear_AllPlayerVoiceFpkOverrides();