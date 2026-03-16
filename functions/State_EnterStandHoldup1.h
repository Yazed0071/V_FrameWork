#pragma once

#include <cstdint>

// Enables or disables the cowardly holdup reaction replacement.
// Params: enabled
void Set_HoldUpReactionCowardlyReactions(bool enabled);

// Returns whether the cowardly holdup reaction replacement is enabled.
// Params: none
bool Get_HoldUpReactionCowardlyReactions();

// Installs both stand-holdup cowardly-reaction hooks.
// Params: none
bool Install_HoldUpReactionCowardlyReactions_Hook();

// Removes both stand-holdup cowardly-reaction hooks.
// Params: none
bool Uninstall_HoldUpReactionCowardlyReactions_Hook();