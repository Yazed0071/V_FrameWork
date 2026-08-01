#pragma once

#include <Windows.h>

namespace equip
{
    enum MenuPerfSlotId
    {
        kPerf_FillGrid = 0,
        kPerf_CopyGrid,
        kPerf_CountBadge,
        kPerf_FillFlat,
        kPerf_CopyFlat,
        kPerf_IsEquipVisile,
        kPerf_GetSuitIdx,
        kPerf_IsEquipSuit,
        kPerf_DrainHeads,
        kPerf_UpdateRecords,
        kPerf_AddListSuit,
        kPerf_SlotCount
    };

    void MenuPerfAdd(int slot, long long ticks);

    struct MenuPerfScope
    {
        int slot;
        LARGE_INTEGER t0;
        explicit MenuPerfScope(int s) : slot(s) { QueryPerformanceCounter(&t0); }
        ~MenuPerfScope()
        {
            LARGE_INTEGER t1;
            QueryPerformanceCounter(&t1);
            MenuPerfAdd(slot, t1.QuadPart - t0.QuadPart);
        }
    };
}
