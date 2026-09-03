#pragma once

#include <cstddef>
#include <cstdint>

bool Install_HeadMarkMarkerEvCall_SetIconSubType_Patch();
void Uninstall_HeadMarkMarkerEvCall_SetIconSubType_Patch();

struct HeadMarkColourStop
{
    bool          isRgb;
    std::uint32_t paletteId;
    float         r;
    float         g;
    float         b;
};

bool SetHeadMarkEntityColour(std::uint32_t gameObjectId, int state,
                             const HeadMarkColourStop* stops, unsigned count,
                             float speed, bool blend, float fade);
void Clear_HeadMarkEntityColours();
void DescribeHeadMarkOverrides(std::uint16_t systemId, char* out, std::size_t cap);

bool GetHeadMarkColourForSlot(std::uint16_t systemId, unsigned slot, float outRgb[3],
                              int* outReason = nullptr);
std::uint32_t ResolveHeadMarkColourId(const char* name);
