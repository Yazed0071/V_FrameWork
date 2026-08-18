#include "pch.h"

#include "ShadowState.h"
#include "AdditionalMotionTable_GetMtarPathId.h"

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "log.h"

namespace
{
    std::mutex                                    g_Mutex;
    outfit::shadow::Slot                          g_Slots[outfit::shadow::kMaxSlots] = {};

    std::uint8_t                                  g_ArmTier[outfit::shadow::kPlayerTypeMax]      = {0,0,0,0};
    bool                                          g_ArmTierCaptured[outfit::shadow::kPlayerTypeMax] = {false,false,false,false};

    thread_local std::size_t                      tl_CurrentSlot = SIZE_MAX;
}

namespace outfit::shadow
{
    void Set(std::size_t slot, const Slot& s)
    {
        if (slot >= kMaxSlots) return;
        std::lock_guard<std::mutex> lock(g_Mutex);

        const Slot& prev = g_Slots[slot];
        const bool changed =
               !prev.used
            ||  prev.realPartsType  != s.realPartsType
            ||  prev.realCamoType   != s.realCamoType
            ||  prev.realArmType    != s.realArmType
            ||  prev.realPlayerType != s.realPlayerType
            ||  prev.developId      != s.developId
            ||  prev.variantIdx     != s.variantIdx;

        g_Slots[slot] = s;
        g_Slots[slot].used = true;

        if (changed)
        {
#ifdef _DEBUG
            LogDebug("[ShadowState] slot=%zu SET realPartsType=0x%02X realCamo=0x%02X "
                "realArmType=%u realPlayerType=%u developId=%u variantIdx=%u\n",
                slot,
                static_cast<unsigned>(s.realPartsType),
                static_cast<unsigned>(s.realCamoType),
                static_cast<unsigned>(s.realArmType),
                static_cast<unsigned>(s.realPlayerType),
                static_cast<unsigned>(s.developId),
                static_cast<unsigned>(s.variantIdx));
#endif
            outfit::RequestAdditionalMotionReresolve();
        }
    }

    void Clear(std::size_t slot)
    {
        if (slot >= kMaxSlots) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_Slots[slot].used)
        {
#ifdef _DEBUG
            LogDebug("[ShadowState] slot=%zu CLEAR (was developId=%u)\n",
                slot, static_cast<unsigned>(g_Slots[slot].developId));
#endif
        }
        g_Slots[slot] = Slot{};
    }

    void ResetAll(const char* reason)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        bool anyUsed = false;
        for (auto& s : g_Slots) { if (s.used) { anyUsed = true; break; } }
        for (auto& s : g_Slots) s = Slot{};
        if (anyUsed)
        {
#ifdef _DEBUG
            LogDebug("[ShadowState] RESET-ALL reason=%s\n", reason ? reason : "(unspecified)");
#endif
        }
    }

    bool Get(std::size_t slot, Slot* out)
    {
        if (slot >= kMaxSlots) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (!g_Slots[slot].used) return false;
        if (out) *out = g_Slots[slot];
        return true;
    }

    bool IsActive(std::size_t slot)
    {
        if (slot >= kMaxSlots) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_Slots[slot].used;
    }

    void SetArmTier(std::uint32_t playerType, std::uint8_t tier)
    {
        const std::uint32_t pt = playerType & 0xFFu;
        if (pt >= kPlayerTypeMax) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_ArmTier[pt]         = tier;
        g_ArmTierCaptured[pt] = true;
    }

    std::uint8_t GetArmTier(std::uint32_t playerType, bool* outCaptured)
    {
        const std::uint32_t pt = playerType & 0xFFu;
        if (pt >= kPlayerTypeMax)
        {
            if (outCaptured) *outCaptured = false;
            return 0;
        }
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (outCaptured) *outCaptured = g_ArmTierCaptured[pt];
        return g_ArmTier[pt];
    }

    void ResetArmTierCache()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& v : g_ArmTier)         v = 0;
        for (auto& v : g_ArmTierCaptured) v = false;
    }

    void         SetCurrentSlot(std::size_t slot) { tl_CurrentSlot = slot; }
    void         ClearCurrentSlot()               { tl_CurrentSlot = SIZE_MAX; }
    std::size_t  GetCurrentSlot()                 { return tl_CurrentSlot; }
    bool         HasCurrentSlot()                 { return tl_CurrentSlot != SIZE_MAX; }
}
