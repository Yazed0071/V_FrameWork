#include "pch.h"
#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <mutex>
#include "MinHook.h"
#include "log.h"
#include "TppPickableRuntime.h"
#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"

namespace
{
    struct PickableOverride
    {
        std::uint16_t mask = 0;
        std::uint16_t values[kTppPickableFieldCount] = {};
    };

    static std::unordered_map<std::uint16_t, PickableOverride> g_PickableOverrides;
    static std::mutex g_PickableOverridesMutex;

    static bool g_TppPickableHooksInstalled = false;
    static void* g_LastPickableSystem = nullptr;

    using CopyAndAdjustInfo_t =
        void(__fastcall*)(void* thisPtr, std::uint16_t* outInfo, void* statusPtr, std::uint8_t* locatorParam);

    static CopyAndAdjustInfo_t g_OrigCopyAndAdjustInfo = nullptr;
}


static std::uint16_t ClampPickableCount(std::uint32_t value)
{
    if (value > 0x0FFFu)
        return 0x0FFFu;

    return static_cast<std::uint16_t>(value);
}


static bool TryGetLivePickableInfoByIndex(void* thisPtr, std::uint32_t locatorIndex, std::uint16_t*& outInfo)
{
    outInfo = nullptr;

    if (!thisPtr)
        return false;

    std::uint8_t* thisBytes = static_cast<std::uint8_t*>(thisPtr);

    __try
    {
        const std::uint32_t locatorCount = *reinterpret_cast<std::uint32_t*>(thisBytes + 0x74);
        if (locatorIndex >= locatorCount)
            return false;

        std::uint8_t* infoBase = *reinterpret_cast<std::uint8_t**>(thisBytes + 0x38);
        if (!infoBase)
            return false;

        outInfo = reinterpret_cast<std::uint16_t*>(infoBase + (static_cast<std::size_t>(locatorIndex) * 0x10));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        outInfo = nullptr;
        return false;
    }
}


static bool TryGetLocatorIndexFromOutInfo(void* thisPtr, std::uint16_t* outInfo, std::uint16_t& outIndex)
{
    outIndex = 0;

    if (!thisPtr || !outInfo)
        return false;

    std::uint8_t* thisBytes = static_cast<std::uint8_t*>(thisPtr);

    std::uint8_t* infoBase = *reinterpret_cast<std::uint8_t**>(thisBytes + 0x38);
    if (!infoBase)
        return false;

    const std::uint32_t locatorCount = *reinterpret_cast<std::uint32_t*>(thisBytes + 0x74);

    std::uint8_t* outBytes = reinterpret_cast<std::uint8_t*>(outInfo);
    const std::ptrdiff_t diff = outBytes - infoBase;

    if (diff < 0)
        return false;

    if ((diff % 0x10) != 0)
        return false;

    const std::uint32_t locatorIndex = static_cast<std::uint32_t>(diff / 0x10);
    if (locatorIndex >= locatorCount)
        return false;

    outIndex = static_cast<std::uint16_t>(locatorIndex);
    return true;
}


static std::uint16_t ClampPickableFieldValue(std::uint32_t fieldId, std::uint32_t value)
{
    switch (fieldId)
    {
    case kTppPickableFieldEquipId:
        return static_cast<std::uint16_t>(value & 0x7FFu);
    case kTppPickableFieldCountRaw:
    case kTppPickableFieldSecondCountRaw:
    case kTppPickableFieldCountMax:
    case kTppPickableFieldSecondCountMax:
        return ClampPickableCount(value);
    case kTppPickableFieldInfoType:
        return static_cast<std::uint16_t>(value & 0xFFu);
    default:
        return static_cast<std::uint16_t>(value & 0xFFFFu);
    }
}


static void ApplyFieldToInfo(std::uint16_t* info, std::uint32_t fieldId, std::uint16_t value)
{
    if (!info)
        return;

    std::uint8_t* bytes = reinterpret_cast<std::uint8_t*>(info);

    switch (fieldId)
    {
    case kTppPickableFieldEquipId:
        info[0] = static_cast<std::uint16_t>((info[0] & ~0x7FFu) | (value & 0x7FFu));
        break;
    case kTppPickableFieldCountRaw:
        info[2] = value;
        break;
    case kTppPickableFieldSecondCountRaw:
        info[3] = value;
        break;
    case kTppPickableFieldCountMax:
        info[4] = value;
        break;
    case kTppPickableFieldSecondCountMax:
        info[5] = value;
        break;
    case kTppPickableFieldInfoType:
        bytes[12] = static_cast<std::uint8_t>(value);
        info[7] = static_cast<std::uint16_t>((info[7] & ~0x1u) | ((value & 0xFFu) ? 0x1u : 0x0u));
        break;
    case kTppPickableFieldFlags:
        info[7] = static_cast<std::uint16_t>((value & ~0x8u) | (info[7] & 0x8u));
        break;
    default:
        break;
    }
}


static bool GuardedApplyFieldToInfo(std::uint16_t* info, std::uint32_t fieldId, std::uint16_t value)
{
    __try
    {
        ApplyFieldToInfo(info, fieldId, value);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool GuardedCopyInfoWords(const std::uint16_t* info, std::uint16_t* outWords8)
{
    __try
    {
        for (int i = 0; i < 8; ++i)
            outWords8[i] = info[i];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool GuardedReadCountRaw(const std::uint16_t* info, std::uint16_t& outCountRaw)
{
    __try
    {
        outCountRaw = info[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static void ApplyOverrideToInfo(std::uint16_t* info, const PickableOverride& override_)
{
    static const std::uint32_t kApplyOrder[kTppPickableFieldCount] =
    {
        kTppPickableFieldEquipId,
        kTppPickableFieldCountRaw,
        kTppPickableFieldSecondCountRaw,
        kTppPickableFieldCountMax,
        kTppPickableFieldSecondCountMax,
        kTppPickableFieldFlags,
        kTppPickableFieldInfoType,
    };

    for (std::uint32_t fieldId : kApplyOrder)
    {
        if (override_.mask & (1u << fieldId))
            ApplyFieldToInfo(info, fieldId, override_.values[fieldId]);
    }
}


static void __fastcall hkCopyAndAdjustInfo(void* thisPtr, std::uint16_t* outInfo, void* statusPtr, std::uint8_t* locatorParam)
{
    g_LastPickableSystem = thisPtr;

    MISSION_GUARD_ORIGINAL_VOID(g_OrigCopyAndAdjustInfo, thisPtr, outInfo, statusPtr, locatorParam);

    g_OrigCopyAndAdjustInfo(thisPtr, outInfo, statusPtr, locatorParam);

    std::uint16_t locatorIndex = 0;
    if (!TryGetLocatorIndexFromOutInfo(thisPtr, outInfo, locatorIndex))
        return;

    std::lock_guard<std::mutex> lock(g_PickableOverridesMutex);
    const auto it = g_PickableOverrides.find(locatorIndex);
    if (it == g_PickableOverrides.end())
        return;

    ApplyOverrideToInfo(outInfo, it->second);
}


bool Set_TppPickableFieldByIndex(std::uint32_t locatorIndex, std::uint32_t fieldId, std::uint32_t value)
{
    if (locatorIndex > 0xFFFFu || fieldId >= kTppPickableFieldCount)
        return false;

    const std::uint16_t index16 = static_cast<std::uint16_t>(locatorIndex);
    const std::uint16_t value16 = ClampPickableFieldValue(fieldId, value);

    {
        std::lock_guard<std::mutex> lock(g_PickableOverridesMutex);
        PickableOverride& override_ = g_PickableOverrides[index16];
        override_.mask |= static_cast<std::uint16_t>(1u << fieldId);
        override_.values[fieldId] = value16;
    }

    if (!MissionCodeGuard::ShouldBypassHooks())
    {
        std::uint16_t* liveInfo = nullptr;
        if (TryGetLivePickableInfoByIndex(g_LastPickableSystem, locatorIndex, liveInfo))
        {
            GuardedApplyFieldToInfo(liveInfo, fieldId, value16);
        }
    }

    return true;
}


bool Get_TppPickableInfoWordsByIndex(std::uint32_t locatorIndex, std::uint16_t* outWords8)
{
    if (locatorIndex > 0xFFFFu || !outWords8)
        return false;

    std::uint16_t* liveInfo = nullptr;
    if (!TryGetLivePickableInfoByIndex(g_LastPickableSystem, locatorIndex, liveInfo))
        return false;

    return GuardedCopyInfoWords(liveInfo, outWords8);
}


bool Set_TppPickableCountRawByIndex(std::uint32_t locatorIndex, std::uint32_t countRaw)
{
    return Set_TppPickableFieldByIndex(locatorIndex, kTppPickableFieldCountRaw, countRaw);
}


bool Get_TppPickableCountRawByIndex(std::uint32_t locatorIndex, std::uint16_t& outCountRaw)
{
    if (locatorIndex > 0xFFFFu)
        return false;

    std::uint16_t* liveInfo = nullptr;
    if (TryGetLivePickableInfoByIndex(g_LastPickableSystem, locatorIndex, liveInfo) &&
        GuardedReadCountRaw(liveInfo, outCountRaw))
    {
        return true;
    }

    const std::uint16_t index16 = static_cast<std::uint16_t>(locatorIndex);
    {
        std::lock_guard<std::mutex> lock(g_PickableOverridesMutex);
        const auto it = g_PickableOverrides.find(index16);
        if (it != g_PickableOverrides.end() &&
            (it->second.mask & (1u << kTppPickableFieldCountRaw)))
        {
            outCountRaw = it->second.values[kTppPickableFieldCountRaw];
            return true;
        }
    }

    Log("[TppPickable] GetCountRawByIndex failed index=%u\n",
        static_cast<unsigned>(locatorIndex));
    return false;
}


void Clear_TppPickableOverrides()
{
    std::lock_guard<std::mutex> lock(g_PickableOverridesMutex);
    g_PickableOverrides.clear();
}


namespace
{
    static void* g_ItemWindowThunk = nullptr;

    static void* AllocThunkNear(uintptr_t anchor, std::size_t size)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const uintptr_t gran = si.dwAllocationGranularity
                             ? si.dwAllocationGranularity : 0x10000;
        const uintptr_t lo = (anchor > 0x40000000ull) ? anchor - 0x40000000ull : 0x10000ull;
        const uintptr_t hi = anchor + 0x40000000ull;
        for (uintptr_t p = (anchor & ~(gran - 1)) + gran; p < hi; p += gran)
        {
            void* m = VirtualAlloc(reinterpret_cast<void*>(p), size,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (m)
                return m;
        }
        for (uintptr_t p = (anchor & ~(gran - 1)); p > lo; p -= gran)
        {
            void* m = VirtualAlloc(reinterpret_cast<void*>(p), size,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (m)
                return m;
        }
        return nullptr;
    }

    static bool InstallItemWindowBoundPatch()
    {
        const uintptr_t site = gAddr.TppPickable_ItemWindowBoundSite;
        if (!site)
            return true;

        auto* at = reinterpret_cast<std::uint8_t*>(site);
        static const std::uint8_t kOrig[6] = { 0x81, 0xFB, 0x09, 0x02, 0x00, 0x00 };
        for (int i = 0; i < 6; ++i)
        {
            if (at[i] != kOrig[i])
            {
                LogDebug("[TppPickable] item-window bound patch REFUSED at 0x%llX: expected "
                    "CMP EBX,0x209 (81 FB 09 02 00 00) but found %02X %02X %02X %02X "
                    "%02X %02X - another module detoured these bytes or the address is "
                    "wrong for this build; item equipIds below 0x1C2 keep crashing on "
                    "drop/pickup\n",
                    static_cast<unsigned long long>(site),
                    at[0], at[1], at[2], at[3], at[4], at[5]);
                return false;
            }
        }

        const uintptr_t backTo   = site + 6;
        const uintptr_t rejectTo = site + 0xA9;

        std::uint8_t* t = static_cast<std::uint8_t*>(AllocThunkNear(site, 64));
        if (!t)
        {
            LogDebug("[TppPickable] item-window bound patch REFUSED: no executable page "
                "within rel32 range of 0x%llX - item equipIds below 0x1C2 keep crashing "
                "on drop/pickup\n", static_cast<unsigned long long>(site));
            return false;
        }

        std::size_t n = 0;
        t[n++] = 0x81; t[n++] = 0xFB;
        t[n++] = 0xC2; t[n++] = 0x01; t[n++] = 0x00; t[n++] = 0x00;
        t[n++] = 0x0F; t[n++] = 0x8C;
        const std::int32_t relReject =
            static_cast<std::int32_t>(rejectTo - (reinterpret_cast<uintptr_t>(t) + n + 4));
        std::memcpy(t + n, &relReject, 4); n += 4;
        std::memcpy(t + n, kOrig, 6); n += 6;
        t[n++] = 0xE9;
        const std::int32_t relBack =
            static_cast<std::int32_t>(backTo - (reinterpret_cast<uintptr_t>(t) + n + 4));
        std::memcpy(t + n, &relBack, 4); n += 4;

        const std::int64_t relIn =
            static_cast<std::int64_t>(reinterpret_cast<uintptr_t>(t)) -
            static_cast<std::int64_t>(site + 5);
        if (relIn > INT32_MAX || relIn < INT32_MIN)
        {
            VirtualFree(t, 0, MEM_RELEASE);
            LogDebug("[TppPickable] item-window bound patch REFUSED: thunk is out of rel32 "
                "range of 0x%llX - item equipIds below 0x1C2 keep crashing on "
                "drop/pickup\n", static_cast<unsigned long long>(site));
            return false;
        }

        DWORD old = 0;
        if (!VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old))
        {
            VirtualFree(t, 0, MEM_RELEASE);
            Log("[TppPickable] item-window bound patch REFUSED: VirtualProtect failed at "
                "0x%llX - item equipIds below 0x1C2 keep crashing on drop/pickup\n",
                static_cast<unsigned long long>(site));
            return false;
        }
        const std::int32_t relIn32 = static_cast<std::int32_t>(relIn);
        at[0] = 0xE9;
        std::memcpy(at + 1, &relIn32, 4);
        at[5] = 0x90;
        VirtualProtect(at, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 6);

        g_ItemWindowThunk = t;
        return true;
    }
}

bool Install_TppPickableHooks()
{
    if (g_TppPickableHooksInstalled)
        return true;

    InstallItemWindowBoundPatch();

    void* target = reinterpret_cast<void*>(gAddr.CopyAndAdjustInfo);

    if (MH_CreateHook(
        target,
        reinterpret_cast<void*>(&hkCopyAndAdjustInfo),
        reinterpret_cast<void**>(&g_OrigCopyAndAdjustInfo)) != MH_OK)
    {
        Log("[TppPickable] Install hooks: FAIL create hook\n");
        return false;
    }

    if (EnableOrQueueHook(target) != MH_OK)
    {
        MH_RemoveHook(target);
        g_OrigCopyAndAdjustInfo = nullptr;
        Log("[TppPickable] Install hooks: FAIL enable hook\n");
        return false;
    }

    g_TppPickableHooksInstalled = true;
#ifdef _DEBUG
    LogDebug("[TppPickable] Install hooks: OK\n");
#endif
    return true;
}


bool Uninstall_TppPickableHooks()
{
    if (!g_TppPickableHooksInstalled)
        return true;

    void* target = reinterpret_cast<void*>(gAddr.CopyAndAdjustInfo);

    MH_DisableHook(target);
    MH_RemoveHook(target);

    g_OrigCopyAndAdjustInfo = nullptr;
    g_TppPickableHooksInstalled = false;
    g_LastPickableSystem = nullptr;

    Clear_TppPickableOverrides();

#ifdef _DEBUG
    LogDebug("[TppPickable] Uninstall hooks: OK\n");
#endif
    return true;
}