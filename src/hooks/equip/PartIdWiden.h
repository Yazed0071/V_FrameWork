#pragma once

#include <cstdint>

bool PartIdWiden_Install();
void PartIdWiden_Uninstall();
bool PartIdWiden_IsActive();

int  PartIdWiden_MaxPartId();
bool PartIdWiden_BuildWideDesc(const unsigned char* narrowDesc,
                               const int* wideIds12,
                               std::uint8_t* out24);
