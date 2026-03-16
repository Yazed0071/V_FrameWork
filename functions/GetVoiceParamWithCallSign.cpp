#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <unordered_set>
#include <mutex>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "GetVoiceParamWithCallSign.h"

namespace
{
    // Hook type for RadioSpeechHandlerImpl::GetVoiceParamWithCallSign.
    // Params: self (RadioSpeechHandlerImpl*), ownerIndex (uint32_t)
    using GetVoiceParamWithCallSign_t = std::uint64_t(__fastcall*)(void* self, std::uint32_t ownerIndex);

    // Virtual function type used by *(self + 0x28)->vfunc[0xA0] to get the base voice param.
    // Params: obj28 (void*), ownerIndexLow8 (uint32_t)
    using GetBaseVoiceParam_t = std::uint64_t(__fastcall*)(void* obj28, std::uint32_t ownerIndexLow8);

    // Absolute address of RadioSpeechHandlerImpl::GetVoiceParamWithCallSign.
    static constexpr std::uintptr_t ABS_GetVoiceParamWithCallSign = 0x140DA3170ull;

    // Turn this on only if you want every matching-base call logged.
    static constexpr bool kLogBaseMatchMisses = true;

    // Turn this on only if you want every vanilla call logged too.
    static constexpr bool kLogEveryCall = false;

    static GetVoiceParamWithCallSign_t g_OrigGetVoiceParamWithCallSign = nullptr;

    // Soldiers that should use the hardcoded extra call-sign mapping.
    // Stored by normalized soldier index.
    static std::unordered_set<std::uint16_t> g_ExtraSoldierIndices;

    static std::mutex g_CallSignMutex;

    // One hardcoded "base list -> extra StateId" mapping.
    struct HardcodedCallSignExtra
    {
        std::uint32_t baseVoiceParam = 0;
        std::uint32_t extraStateId = 0;
        const char* label = nullptr;
    };

    // Parsed owner entry from self + 0x58, indexed by ownerIndex with stride 0x14.
    // This is the same entry family used by CallPart and GetVoiceParamWithCallSign.
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

    // Hardcoded per-list extra StateIds.
    // Add the rest of your lists here later.
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

// Returns YES or NO for logging.
// Params: value
static const char* YesNo(bool value)
{
    return value ? "YES" : "NO";
}

// Safely reads a byte from memory.
// Params: addr, outValue
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

// Safely reads a word from memory.
// Params: addr, outValue
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

// Safely reads a dword from memory.
// Params: addr, outValue
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

// Safely reads a qword from memory.
// Params: addr, outValue
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

// Converts a soldier GameObjectId like 0x040B into soldier index 0x000B.
// Params: gameObjectId
static std::uint16_t NormalizeSoldierIndexFromGameObjectId(std::uint32_t gameObjectId)
{
    const std::uint16_t raw = static_cast<std::uint16_t>(gameObjectId);

    if (raw == 0xFFFFu)
        return 0xFFFFu;

    if ((raw & 0xFE00u) != 0x0400u)
        return 0xFFFFu;

    return static_cast<std::uint16_t>(raw & 0x01FFu);
}

// Normalizes the soldier index read from the owner entry.
// CallPart checks 0x01FF as the invalid sentinel.
// Params: rawSoldierIndex
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

// Reads the owner entry from self + 0x58 for one ownerIndex.
// Entry layout is 0x14 bytes wide.
// Params: self, ownerIndex, outEntry
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

// Computes rawCallSign % 12 for logging/debug.
// Params: rawCallSign, outModuloIndex
static bool TryComputeCallSignModulo12(std::uint16_t rawCallSign, std::uint32_t& outModuloIndex)
{
    outModuloIndex = 0;

    if (rawCallSign == 0xFFFFu)
        return false;

    outModuloIndex = static_cast<std::uint32_t>(rawCallSign % 12u);
    return true;
}

// Calls the internal base voice-param getter used by GetVoiceParamWithCallSign.
// Params: self, ownerIndex, outBaseVoiceParam
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

// Returns the hardcoded extra StateId for one base voice-param/list hash.
// Params: baseVoiceParam
static const HardcodedCallSignExtra* FindHardcodedCallSignExtra(std::uint32_t baseVoiceParam)
{
    for (const auto& entry : kHardcodedCallSignExtras)
    {
        if (entry.baseVoiceParam == baseVoiceParam)
            return &entry;
    }

    return nullptr;
}

// Logs one call-sign owner entry in a compact form.
// Params: ownerIndex, entry, normalizedSoldierIndex, hasModulo12, modulo12, baseVoiceParam
static void LogOwnerEntry58(
    std::uint32_t ownerIndex,
    const CallSignOwnerEntry58& entry,
    std::uint16_t normalizedSoldierIndex,
    bool hasModulo12,
    std::uint32_t modulo12,
    std::uint32_t baseVoiceParam)
{
    Log(
        "[CallSignExtra][ENTRY58] ownerIndex=%u word08=0x%04X rawCallSign=0x%04X modulo12=%s%u soldierIndex0C=0x%04X normalizedSoldierIndex=%u word0E=0x%04X bytes10_13=%02X %02X %02X %02X base=0x%08X\n",
        static_cast<unsigned>(ownerIndex),
        static_cast<unsigned>(entry.word08),
        static_cast<unsigned>(entry.rawCallSign0A),
        hasModulo12 ? "" : "INVALID:",
        static_cast<unsigned>(modulo12),
        static_cast<unsigned>(entry.soldierIndex0C),
        static_cast<unsigned>(normalizedSoldierIndex),
        static_cast<unsigned>(entry.word0E),
        static_cast<unsigned>(entry.byte10),
        static_cast<unsigned>(entry.byte11),
        static_cast<unsigned>(entry.byte12),
        static_cast<unsigned>(entry.byte13),
        static_cast<unsigned>(baseVoiceParam));
}

// Hook for RadioSpeechHandlerImpl::GetVoiceParamWithCallSign.
// Resolves the speaker from the owner entry at self + 0x58 and matches the registered soldier set
// against entry + 0x0C instead of ownerIndex.
// Params: self, ownerIndex
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

    const std::uint16_t rawCallSign =
        hasEntry58 ? entry58.rawCallSign0A : 0xFFFFu;

    std::uint32_t modulo12 = 0;
    const bool hasModulo12 = TryComputeCallSignModulo12(rawCallSign, modulo12);

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

            if (hasEntry58)
            {
                if (isRegisteredSoldier)
                {
                    Log(
                        "[CallSignExtra][CUSTOM] ownerIndex=%u rawCallSign=0x%04X modulo12=%s%u resolvedSoldierIndex=%u base=0x%08X -> extraStateId=0x%08X (%s)\n",
                        static_cast<unsigned>(ownerIndex),
                        static_cast<unsigned>(rawCallSign),
                        hasModulo12 ? "" : "INVALID:",
                        static_cast<unsigned>(modulo12),
                        static_cast<unsigned>(resolvedSoldierIndex),
                        static_cast<unsigned>(baseVoiceParam),
                        static_cast<unsigned>(extra->extraStateId),
                        extra->label ? extra->label : "unnamed");

                    return static_cast<std::uint64_t>(extra->extraStateId);
                }

                if (kLogBaseMatchMisses)
                {
                    LogOwnerEntry58(
                        ownerIndex,
                        entry58,
                        resolvedSoldierIndex,
                        hasModulo12,
                        modulo12,
                        baseVoiceParam);

                    Log(
                        "[CallSignExtra][MISS] ownerIndex=%u resolvedSoldierIndex=%s%u registered=%s base=0x%08X extraStateId=0x%08X\n",
                        static_cast<unsigned>(ownerIndex),
                        resolvedSoldierIndex == 0xFFFFu ? "INVALID:" : "",
                        static_cast<unsigned>(resolvedSoldierIndex),
                        YesNo(isRegisteredSoldier),
                        static_cast<unsigned>(baseVoiceParam),
                        static_cast<unsigned>(extra->extraStateId));
                }
            }
            else if (kLogBaseMatchMisses)
            {
                Log(
                    "[CallSignExtra][MISS] ownerIndex=%u entry58=READ_FAIL base=0x%08X extraStateId=0x%08X\n",
                    static_cast<unsigned>(ownerIndex),
                    static_cast<unsigned>(baseVoiceParam),
                    static_cast<unsigned>(extra->extraStateId));
            }
        }
    }

    const std::uint64_t finalVoiceParam =
        g_OrigGetVoiceParamWithCallSign
        ? g_OrigGetVoiceParamWithCallSign(self, ownerIndex)
        : 0;

    if (kLogEveryCall)
    {
        if (hasEntry58)
        {
            Log(
                "[CallSignExtra][VANILLA] ownerIndex=%u rawCallSign=0x%04X modulo12=%s%u resolvedSoldierIndex=%s%u base=0x%08X final=0x%08X\n",
                static_cast<unsigned>(ownerIndex),
                static_cast<unsigned>(rawCallSign),
                hasModulo12 ? "" : "INVALID:",
                static_cast<unsigned>(modulo12),
                resolvedSoldierIndex == 0xFFFFu ? "INVALID:" : "",
                static_cast<unsigned>(resolvedSoldierIndex),
                static_cast<unsigned>(baseVoiceParam),
                static_cast<unsigned>(finalVoiceParam & 0xFFFFFFFFu));
        }
        else
        {
            Log(
                "[CallSignExtra][VANILLA] ownerIndex=%u entry58=READ_FAIL base=0x%08X final=0x%08X\n",
                static_cast<unsigned>(ownerIndex),
                static_cast<unsigned>(baseVoiceParam),
                static_cast<unsigned>(finalVoiceParam & 0xFFFFFFFFu));
        }
    }

    return finalVoiceParam;
}

// Marks one soldier to use hardcoded extra call-sign overrides.
// Params: gameObjectId
void Add_CallSignExtraSoldier(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        Log("[CallSignExtra] Add soldier ignored: invalid GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.insert(soldierIndex);
    }

    Log("[CallSignExtra] Added soldier: gameObjectId=0x%08X soldierIndex=0x%04X\n",
        gameObjectId,
        static_cast<unsigned>(soldierIndex));
}

// Removes one soldier from hardcoded extra call-sign overrides.
// Params: gameObjectId
void Remove_CallSignExtraSoldier(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        Log("[CallSignExtra] Remove soldier ignored: invalid GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.erase(soldierIndex);
    }

    Log("[CallSignExtra] Removed soldier: gameObjectId=0x%08X soldierIndex=0x%04X\n",
        gameObjectId,
        static_cast<unsigned>(soldierIndex));
}

// Clears all soldiers from hardcoded extra call-sign overrides.
// Params: none
void Clear_CallSignExtraSoldiers()
{
    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.clear();
    }

    Log("[CallSignExtra] Cleared all soldiers\n");
}

// Installs the hardcoded GetVoiceParamWithCallSign hook.
// Params: none
bool Install_CallSignExtra_Hook()
{
    void* target = ResolveGameAddress(ABS_GetVoiceParamWithCallSign);
    if (!target)
    {
        Log("[Hook] CallSignExtra: target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGetVoiceParamWithCallSign),
        reinterpret_cast<void**>(&g_OrigGetVoiceParamWithCallSign));

    Log("[Hook] CallSignExtra: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes the hardcoded GetVoiceParamWithCallSign hook.
// Params: none
bool Uninstall_CallSignExtra_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_GetVoiceParamWithCallSign));
    g_OrigGetVoiceParamWithCallSign = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_ExtraSoldierIndices.clear();
    }

    Log("[Hook] CallSignExtra: removed\n");
    return true;
}