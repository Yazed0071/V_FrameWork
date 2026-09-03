#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "ActionCoreImpl_UpdateOptCamo.h"
#include "AddressSet.h"

namespace
{


    using UpdateOptCamo_t = void(__fastcall*)(void* self, std::uint32_t actorIndex);


    enum class OptCamoForceMode : std::uint32_t
    {
        None = 0,
        ForceOn = 1,
        ForceOff = 2
    };


    enum class ResolveOptCamoStatus : std::uint32_t
    {
        Ok = 0,
        NullSelf,
        NullRoot,
        NullMapOwner,
        NullMapTable,
        InvalidMappedIndex,
        NullConfigList,
        NullConfigBase,
        ExceptionThrown
    };

    static constexpr std::uint32_t kDesiredOptCamoBit = 0x20000000u;

    static constexpr std::uint32_t kSoldierIndexMask = 0x01FFu;

    static UpdateOptCamo_t g_OrigUpdateOptCamo = nullptr;

    static std::mutex g_UpdateOptCamoMutex;
    static std::unordered_map<std::uint32_t, OptCamoForceMode> g_MappedIndexModes;

    static std::uint64_t g_SoldierActorMap   = 0;
    static std::uint64_t g_SoldierStatusBase  = 0;
    static std::uint32_t g_SoldierStatusCount = 0;
}


static bool TryReadUInt32SEH(const std::uint32_t* ptr, std::uint32_t& outValue)
{
    outValue = 0;

    if (!ptr)
        return false;

    __try
    {
        outValue = *ptr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outValue = 0;
        return false;
    }
}


static bool TryWriteUInt32SEH(std::uint32_t* ptr, std::uint32_t value)
{
    if (!ptr)
        return false;

    __try
    {
        *ptr = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool ResolveOptCamoFlagsField(
    void* self,
    std::uint32_t actorIndex,
    std::uint32_t*& outFlagsPtr,
    std::uint16_t& outMappedIndex,
    ResolveOptCamoStatus& outStatus)
{
    outFlagsPtr = nullptr;
    outMappedIndex = 0xFFFFu;
    outStatus = ResolveOptCamoStatus::NullSelf;

    if (!self)
        return false;

    __try
    {
        const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

        const std::uint64_t root =
            *reinterpret_cast<const std::uint64_t*>(selfAddr + 0x1E8ull);
        if (!root)
        {
            outStatus = ResolveOptCamoStatus::NullRoot;
            return false;
        }

        const std::uint64_t mapOwner =
            *reinterpret_cast<const std::uint64_t*>(root + 0x18ull);
        if (!mapOwner)
        {
            outStatus = ResolveOptCamoStatus::NullMapOwner;
            return false;
        }

        const std::uint64_t mapTable =
            *reinterpret_cast<const std::uint64_t*>(mapOwner + 0x28ull);
        if (!mapTable)
        {
            outStatus = ResolveOptCamoStatus::NullMapTable;
            return false;
        }

        const std::uint16_t mappedIndex =
            *reinterpret_cast<const std::uint16_t*>(
                mapTable + (static_cast<std::uint64_t>(actorIndex) * 2ull));

        outMappedIndex = mappedIndex;

        if (mappedIndex == 0x01FFu)
        {
            outStatus = ResolveOptCamoStatus::InvalidMappedIndex;
            return false;
        }

        const std::uint64_t configList =
            *reinterpret_cast<const std::uint64_t*>(root + 0x110ull);
        if (!configList)
        {
            outStatus = ResolveOptCamoStatus::NullConfigList;
            return false;
        }

        const std::uint32_t configCount =
            *reinterpret_cast<const std::uint32_t*>(configList + 0x10ull);

        const std::uint64_t configBase =
            *reinterpret_cast<const std::uint64_t*>(configList + 0x8ull);
        if (!configBase)
        {
            outStatus = ResolveOptCamoStatus::NullConfigBase;
            return false;
        }

        std::uint64_t configEntry = configBase;
        if (static_cast<std::uint32_t>(mappedIndex) < configCount)
        {
            configEntry = configBase + (static_cast<std::uint64_t>(mappedIndex) * 0x28ull);
        }

        outFlagsPtr = reinterpret_cast<std::uint32_t*>(configEntry + 0x10ull);
        g_SoldierActorMap    = mapTable;
        g_SoldierStatusBase  = configBase;
        g_SoldierStatusCount = configCount;
        outStatus = ResolveOptCamoStatus::Ok;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outFlagsPtr = nullptr;
        outMappedIndex = 0xFFFFu;
        outStatus = ResolveOptCamoStatus::ExceptionThrown;
        return false;
    }
}


static OptCamoForceMode GetModeForMappedIndex(std::uint32_t mappedIndex)
{
    std::lock_guard<std::mutex> lock(g_UpdateOptCamoMutex);

    const auto it = g_MappedIndexModes.find(mappedIndex);
    if (it != g_MappedIndexModes.end())
        return it->second;

    return OptCamoForceMode::None;
}


static void SetMappedIndexMode_NoLock(std::uint32_t mappedIndex, OptCamoForceMode mode)
{
    if (mode == OptCamoForceMode::None)
    {
        g_MappedIndexModes.erase(mappedIndex);
    }
    else
    {
        g_MappedIndexModes[mappedIndex] = mode;
    }
}


static void __fastcall hkUpdateOptCamo(void* self, std::uint32_t actorIndex)
{
    if (!g_OrigUpdateOptCamo)
        return;

    if (MissionCodeGuard::ShouldBypassHooks())
    {
        g_OrigUpdateOptCamo(self, actorIndex);
        return;
    }

    std::uint32_t* flagsPtr = nullptr;
    std::uint16_t mappedIndex = 0xFFFFu;
    ResolveOptCamoStatus resolveStatus = ResolveOptCamoStatus::NullSelf;

    const bool resolved = ResolveOptCamoFlagsField(
        self,
        actorIndex,
        flagsPtr,
        mappedIndex,
        resolveStatus);

    if (!resolved || !flagsPtr)
    {
        g_OrigUpdateOptCamo(self, actorIndex);
        return;
    }

    const OptCamoForceMode mode = GetModeForMappedIndex(static_cast<std::uint32_t>(mappedIndex));
    if (mode == OptCamoForceMode::None)
    {
        g_OrigUpdateOptCamo(self, actorIndex);
        return;
    }

    std::uint32_t savedFlags = 0;
    if (!TryReadUInt32SEH(flagsPtr, savedFlags))
    {
        g_OrigUpdateOptCamo(self, actorIndex);
        return;
    }

    std::uint32_t forcedFlags = savedFlags;
    if (mode == OptCamoForceMode::ForceOn)
    {
        forcedFlags = savedFlags | kDesiredOptCamoBit;
    }
    else if (mode == OptCamoForceMode::ForceOff)
    {
        forcedFlags = savedFlags & ~kDesiredOptCamoBit;
    }

    bool patchedFlags = false;
    if (forcedFlags != savedFlags)
    {
        patchedFlags = TryWriteUInt32SEH(flagsPtr, forcedFlags);
    }

    g_OrigUpdateOptCamo(self, actorIndex);

    if (patchedFlags)
    {
        TryWriteUInt32SEH(flagsPtr, savedFlags);
    }
}


bool Install_UpdateOptCamo_Hook()
{
    void* target = ResolveGameAddress(gAddr.UpdateOptCamo);
    if (!target)
    {
        Log("[Hook] UpdateOptCamo: address resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkUpdateOptCamo),
        reinterpret_cast<void**>(&g_OrigUpdateOptCamo));

#ifdef _DEBUG
    Log("[Hook] UpdateOptCamo: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[Hook] UpdateOptCamo: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_UpdateOptCamo_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.UpdateOptCamo));
    g_OrigUpdateOptCamo = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_UpdateOptCamoMutex);
        g_MappedIndexModes.clear();
    }

    return true;
}


void Set_UpdateOptCamoEnableMappedIndex(std::uint32_t mappedIndex, bool enabled)
{
    const std::uint32_t soldierIndex = mappedIndex & kSoldierIndexMask;

    std::lock_guard<std::mutex> lock(g_UpdateOptCamoMutex);

    if (enabled)
    {
        SetMappedIndexMode_NoLock(soldierIndex, OptCamoForceMode::ForceOn);
    }
    else
    {
        SetMappedIndexMode_NoLock(soldierIndex, OptCamoForceMode::ForceOff);
    }
}


void Clear_UpdateOptCamoMappedIndexOverrides()
{
    std::lock_guard<std::mutex> lock(g_UpdateOptCamoMutex);
    g_MappedIndexModes.clear();
}


bool GetSoldierStatusArray(std::uint64_t& outBase, std::uint32_t& outCount)
{
    outBase  = g_SoldierStatusBase;
    outCount = g_SoldierStatusCount;
    return outBase != 0 && outCount != 0;
}


bool GetSoldierActorMap(std::uint64_t& outMapTable)
{
    outMapTable = g_SoldierActorMap;
    return outMapTable != 0;
}
