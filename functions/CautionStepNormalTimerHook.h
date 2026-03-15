#pragma once

// Installs the caution phase timer hook.
// Params: none
bool Install_CautionStepNormalTimerHook();

// Removes the caution phase timer hook.
// Params: none
bool Uninstall_CautionStepNormalTimerHook();

// Sets the desired caution phase duration in seconds.
// Params: seconds (float)
void Set_CautionStepNormalDurationSeconds(float seconds);

// Returns the configured caution phase duration in seconds.
// Params: none
float Get_CautionStepNormalDurationSeconds();

// Disables the custom caution duration override and restores vanilla behavior.
// Params: none
void Unset_CautionStepNormalDurationSeconds();

// Returns the last observed remaining time before the current hooked phase timer reaches zero.
// This is based on the last phase processed by the hook.
// Params: none
float Get_CautionStepNormalRemainingSeconds();