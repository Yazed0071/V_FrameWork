#pragma once

#include <cstddef>

namespace HookArena
{
    void        ReserveEarly();
    bool        ReleaseOne();
    std::size_t Remaining();
}
