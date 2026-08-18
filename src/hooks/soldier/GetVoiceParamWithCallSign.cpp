#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_set>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "GetVoiceParamWithCallSign.h"
#include "AddressSet.h"

namespace
{


    using GetVoiceParamWithCallSign_t = std::uint64_t(__fastcall*)(void* self, std::uint32_t ownerIndex);


    using GetBaseVoiceParam_t = std::uint64_t(__fastcall*)(void* obj28, std::uint32_t ownerIndexLow8);


    static GetVoiceParamWithCallSign_t g_OrigGetVoiceParamWithCallSign = nullptr;


    static std::unordered_set<std::uint16_t> g_ExtraSoldierIndices;

    static std::mutex g_CallSignMutex;


    struct HardcodedCallSignExtra
    {
        std::uint32_t baseVoiceParam = 0;
        std::uint32_t extraStateId = 0;
        const char* label = nullptr;
    };


    struct CallSignOwnerEntry58
    {
        std::uint32_t dword00 = 0;
        std::uint32_t dword04 = 0;
        std::uint16_t word08 = 0xFFFFu;
        std::uint16_t rawCallSign0A = 0xFFFFu;
        std::uint16_t soldierIndex0C = 0xFFFFu;
        std::uint16_t word0E = 0xFFFFu;
        std::uint8_t byte10 = 0;
        std::uint8_t byte11 = 0;
        std::uint8_t byte12 = 0;
        std::uint8_t byte13 = 0;
    };


    static constexpr HardcodedCallSignExtra kHardcodedCallSignExtras[] =
    {
        { 0x69C268FEu, 0xBF58FDC6u, "DAT_142345928_EXTRA" },
        { 0x29E1F784u, 0x24324540u, "SIGNS_11_EXTRA" },
        { 0x15E302E6u, 0xEC26AC4Eu, "SIGNS_43_EXTRA" },
        { 0x005E7532u, 0xBF58FDC6u, "DAT_142345AD8_EXTRA" },
        { 0x04B10EF0u, 0xAA35573Bu, "DAT_142345AA8_EXTRA" },
        { 0x55d358ADu, 0x5353D6F4u, "DAT_142345A48_EXTRA" },
        { 0x55d358ADu, 0xADDF8BF6u, "DAT_142345A78_EXTRA" },
        { 0x60C307FEu, 0x2FFAA7E7u, "SIGNS_12_EXTRA" },
        { 0x96470469u, 0xA7DD43FFu, "SIGNS_16_EXTRA" },
        { 0xAB3DA1D5u, 0x41DA64A4u, "DAT_142345B08_EXTRA" },
        { 0x8E45B284u, 0xAA35573Bu, "DAT_1423458F8_EXTRA" },
        { 0xD0553D69u, 0x41DA64A4u, "DAT_142345958_EXTRA" },
        { 0xE3166019u, 0x1E2FFD49u, "SIGNS_14_EXTRA" },
    };
}


static bool SafeReadByte(std::uintptr_t addr, std::uint8_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint8_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool SafeReadWord(std::uintptr_t addr, std::uint16_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint16_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool SafeReadDword(std::uintptr_t addr, std::uint32_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint32_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static bool SafeReadQword(std::uintptr_t addr, std::uint64_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint64_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}


static std::uint16_t NormalizeSoldierIndexFromGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t raw = static_cast<std::uint16_t>(gameObjectId);

    if (raw == 0xFFFFu)
        return 0xFFFFu;

    if ((raw & 0xFE00u) != 0x0400u)
        return 0xFFFFu;

    return static_cast<std::uint16_t>(raw & 0x01FFu);
}


static std::uint16_t NormalizeSoldierIndexFromOwnerEntry(std::uint16_t rawSoldierIndex)
{
    if (rawSoldierIndex == 0xFFFFu)
        return 0xFFFFu;

    if (rawSoldierIndex == 0x01FFu)
        return 0xFFFFu;

    if (rawSoldierIndex > 0x01FFu)
        return 0xFFFFu;

    return rawSoldierIndex;
}


static bool TryReadCallSignOwnerEntry58(
    void* self,
    std::uint32_t ownerIndex,
    CallSignOwnerEntry58& outEntry)
{
    outEntry = {};

    if (!self)
        return false;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint64_t tableBase = 0;
    if (!SafeReadQword(selfAddr + 0x58ull, tableBase) || tableBase == 0)
        return false;

    const std::uintptr_t entry =
        static_cast<std::uintptr_t>(tableBase) + static_cast<std::uintptr_t>(ownerIndex) * 0x14ull;

    if (!SafeReadDword(entry + 0x00ull, outEntry.dword00))
        return false;
    if (!SafeReadDword(entry + 0x04ull, outEntry.dword04))
        return false;
    if (!SafeReadWord(entry + 0x08ull, outEntry.word08))
        return false;
    if (!SafeReadWord(entry + 0x0Aull, outEntry.rawCallSign0A))
        return false;
    if (!SafeReadWord(entry + 0x0Cull, outEntry.soldierIndex0C))
        return false;
    if (!SafeReadWord(entry + 0x0Eull, outEntry.word0E))
        return false;
    if (!SafeReadByte(entry + 0x10ull, outEntry.byte10))
        return false;
    if (!SafeReadByte(entry + 0x11ull, outEntry.byte11))
        return false;
    if (!SafeReadByte(entry + 0x12ull, outEntry.byte12))
        return false;
    if (!SafeReadByte(entry + 0x13ull, outEntry.byte13))
        return false;

    return true;
}


static bool TryReadBaseVoiceParam(void* self, std::uint32_t ownerIndex, std::uint64_t& outBaseVoiceParam)
{
    outBaseVoiceParam = 0;

    if (!self)
        return false;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint64_t obj28 = 0;
    if (!SafeReadQword(selfAddr + 0x28ull, obj28) || obj28 == 0)
        return false;

    std::uint64_t vtbl = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(obj28), vtbl) || vtbl == 0)
        return false;

    std::uint64_t fnAddr = 0;
    if (!SafeReadQword(static_cast<std::uintptr_t>(vtbl) + 0xA0ull, fnAddr) || fnAddr == 0)
        return false;

    const auto fn = reinterpret_cast<GetBaseVoiceParam_t>(fnAddr);
    outBaseVoiceParam = fn(reinterpret_cast<void*>(obj28), ownerIndex & 0xFFu);
    return true;
}


static const HardcodedCallSignExtra* FindHardcodedCallSignExtra(std::uint32_t baseVoiceParam)
{
    for (const auto& entry : kHardcodedCallSignExtras)
    {
        if (entry.baseVoiceParam == baseVoiceParam)
            return &entry;
    }

    return nullptr;
}


static std::uint64_t __fastcall hkGetVoiceParamWithCallSign(void* self, std::uint32_t ownerIndex)
{
    if (MissionCodeGuard::ShouldBypassHooks())
    {
        if (g_OrigGetVoiceParamWithCallSign)
            return g_OrigGetVoiceParamWithCallSign(self, ownerIndex);

        return 0;
    }

    std::uint64_t baseVoiceParam64 = 0;
    const bool hasBaseVoiceParam = TryReadBaseVoiceParam(self, ownerIndex, baseVoiceParam64);
    const std::uint32_t baseVoiceParam = static_cast<std::uint32_t>(baseVoiceParam64 & 0xFFFFFFFFu);

    CallSignOwnerEntry58 entry58{};
    const bool hasEntry58 = TryReadCallSignOwnerEntry58(self, ownerIndex, entry58);

    const std::uint16_t resolvedSoldierIndex =
        hasEntry58 ? NormalizeSoldierIndexFromOwnerEntry(entry58.soldierIndex0C) : 0xFFFFu;

    if (hasBaseVoiceParam)
    {
        const HardcodedCallSignExtra* extra = FindHardcodedCallSignExtra(baseVoiceParam);
        if (extra)
        {
            bool isRegisteredSoldier = false;

            {
                std::lock_guard<std::mutex> lock(g_CallSignMutex);
                if (resolvedSoldierIndex != 0xFFFFu)
                {
                    isRegisteredSoldier =
                        g_ExtraSoldierIndices.find(resolvedSoldierIndex) != g_ExtraSoldierIndices.end();
                }
            }

            if (isRegisteredSoldier)
            {
                return static_cast<std::uint64_t>(extra->extraStateId);
            }
        }
    }

    const std::uint64_t finalVoiceParam =
        g_OrigGetVoiceParamWithCallSign
        ? g_OrigGetVoiceParamWithCallSign(self, ownerIndex)
        : 0;

    return finalVoiceParam;
}


void Add_CallSignExtraSoldier(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[CallSignExtra] Add soldier ignored: invalid GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.insert(soldierIndex);
    }
}


void Remove_CallSignExtraSoldier(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[CallSignExtra] Remove soldier ignored: invalid GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.erase(soldierIndex);
    }
}


void Clear_CallSignExtraSoldiers()
{
    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.clear();
    }
}


bool Install_CallSignExtra_Hook()
{
    void* target = ResolveGameAddress(gAddr.GetVoiceParamWithCallSign);
    if (!target)
    {
        Log("[Hook] CallSignExtra: target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGetVoiceParamWithCallSign),
        reinterpret_cast<void**>(&g_OrigGetVoiceParamWithCallSign));

#ifdef _DEBUG
    Log("[Hook] CallSignExtra: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[Hook] CallSignExtra: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_CallSignExtra_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.GetVoiceParamWithCallSign));
    g_OrigGetVoiceParamWithCallSign = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.clear();
    }

#ifdef _DEBUG
    LogDebug("[Hook] CallSignExtra: removed\n");
#endif
    return true;
}