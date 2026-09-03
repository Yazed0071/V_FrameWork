#pragma once

bool QuietCqc_SetHoldCqc(bool enable);
bool QuietCqc_SetInterrogate(bool enable);

void QuietCqc_EnforceMissionGuard();

bool Install_QuietCqcPatches();
void Uninstall_QuietCqcPatches();
