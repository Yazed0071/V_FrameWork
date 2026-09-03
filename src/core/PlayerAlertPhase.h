#pragma once

namespace PlayerAlertPhase
{
    constexpr int kSneak   = 0;
    constexpr int kCaution = 1;
    constexpr int kEvasion = 2;
    constexpr int kAlert   = 3;

    int  Read();
    bool IsAtOrAbove(int phase);
}
