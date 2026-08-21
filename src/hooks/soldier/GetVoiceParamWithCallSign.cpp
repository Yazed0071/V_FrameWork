#include "pch.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
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


    static std::unordered_map<std::uint16_t, std::uint8_t> g_SoldierCallSigns;

    static std::mutex g_CallSignMutex;


    static constexpr std::uint8_t kMinCallSign = 1;

    static constexpr std::uint8_t kMaxCallSign = 13;


    struct CallSignFamily
    {
        std::uint32_t wildcard = 0;
        const char* clipPrefix = nullptr;
        bool skipBroadcastClip = false;
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


    static constexpr CallSignFamily kCallSignFamilies[] =
    {
        { 0x8E45B284u, "csn01", false },
        { 0x69C268FEu, "csc01", false },
        { 0xD0553D69u, "csa01", false },
        { 0x29E1F784u, "csn02", false },
        { 0x60C307FEu, "csc02", false },
        { 0x96470469u, "csa02", false },
        { 0xE3166019u, "csn03", false },
        { 0x55D358ADu, "csc03", false },
        { 0x5FF58302u, "csa03", false },
        { 0x04B10EF0u, "csn01", true  },
        { 0x005E7532u, "csc01", true  },
        { 0xAB3DA1D5u, "csa01", true  },
        { 0x15E302E6u, "cpi02", false },
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


static const CallSignFamily* FindCallSignFamily(std::uint32_t baseVoiceParam)
{
    for (const auto& family : kCallSignFamilies)
    {
        if (family.wildcard == baseVoiceParam)
            return &family;
    }

    return nullptr;
}


static std::uint32_t Fnv1Lower32(const char* text)
{
    std::uint32_t hash = 0x811C9DC5u;

    for (; text && *text; ++text)
    {
        char c = *text;
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + 0x20);

        hash = (hash * 0x01000193u) ^ static_cast<std::uint8_t>(c);
    }

    return hash;
}


static bool BuildCallSignClipName(const CallSignFamily& family, std::uint8_t callSign,
    char* out, std::size_t cap)
{
    if (callSign < kMinCallSign || callSign > kMaxCallSign)
        return false;

    if (!out || cap < 8)
        return false;

    int clipIndex = static_cast<int>(callSign) - static_cast<int>(kMinCallSign);
    if (family.skipBroadcastClip && clipIndex >= 10)
        clipIndex += 1;

    std::size_t n = 0;
    for (const char* p = family.clipPrefix; p && *p && n + 3 < cap; ++p)
        out[n++] = *p;

    out[n++] = static_cast<char>('0' + (clipIndex / 10));
    out[n++] = static_cast<char>('0' + (clipIndex % 10));
    out[n] = '\0';
    return true;
}


static std::uint32_t ResolveCallSignVoiceParam(const CallSignFamily& family, std::uint8_t callSign)
{
    char clipName[16];
    if (!BuildCallSignClipName(family, callSign, clipName, sizeof(clipName)))
        return 0;

    return Fnv1Lower32(clipName);
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
    if (TryReadBaseVoiceParam(self, ownerIndex, baseVoiceParam64))
    {
        const std::uint32_t baseVoiceParam = static_cast<std::uint32_t>(baseVoiceParam64 & 0xFFFFFFFFu);

        const CallSignFamily* family = FindCallSignFamily(baseVoiceParam);
        if (family)
        {
            CallSignOwnerEntry58 entry58{};
            if (TryReadCallSignOwnerEntry58(self, ownerIndex, entry58))
            {
                const std::uint16_t resolvedSoldierIndex =
                    NormalizeSoldierIndexFromOwnerEntry(entry58.soldierIndex0C);

                std::uint8_t callSign = 0;

                if (resolvedSoldierIndex != 0xFFFFu)
                {
                    std::lock_guard<std::mutex> lock(g_CallSignMutex);
                    const auto it = g_SoldierCallSigns.find(resolvedSoldierIndex);
                    if (it != g_SoldierCallSigns.end())
                        callSign = it->second;
                }

                if (callSign != 0)
                {
                    const std::uint32_t voiceParam = ResolveCallSignVoiceParam(*family, callSign);
                    if (voiceParam != 0)
                    {
#ifdef _DEBUG
                        char clipName[16] = {};
                        BuildCallSignClipName(*family, callSign, clipName, sizeof(clipName));

                        std::uint32_t vanillaVoiceParam = 0;
                        if (g_OrigGetVoiceParamWithCallSign)
                            vanillaVoiceParam = static_cast<std::uint32_t>(
                                g_OrigGetVoiceParamWithCallSign(self, ownerIndex) & 0xFFFFFFFFu);

                        LogDebug("[SoldierCallSign] APPLIED callSign %u -> clip %s (0x%08X) on soldier "
                            "0x%04X, line wildcard 0x%08X, rawCallSign %u; vanilla would have played "
                            "0x%08X\n",
                            static_cast<unsigned>(callSign),
                            clipName,
                            voiceParam,
                            static_cast<unsigned>(0x0400u | resolvedSoldierIndex),
                            baseVoiceParam,
                            static_cast<unsigned>(entry58.rawCallSign0A),
                            vanillaVoiceParam);
#endif
                        return static_cast<std::uint64_t>(voiceParam);
                    }
                }
            }
        }
#ifdef _DEBUG
        else
        {
            CallSignOwnerEntry58 skipped{};
            if (TryReadCallSignOwnerEntry58(self, ownerIndex, skipped))
            {
                const std::uint16_t skippedIndex =
                    NormalizeSoldierIndexFromOwnerEntry(skipped.soldierIndex0C);

                bool isRegistered = false;
                if (skippedIndex != 0xFFFFu)
                {
                    std::lock_guard<std::mutex> lock(g_CallSignMutex);
                    isRegistered = g_SoldierCallSigns.find(skippedIndex) != g_SoldierCallSigns.end();
                }

                if (isRegistered)
                    LogDebug("[SoldierCallSign] SKIPPED soldier 0x%04X: line wildcard 0x%08X carries no "
                        "call sign clips, so it keeps its vanilla voice\n",
                        static_cast<unsigned>(0x0400u | skippedIndex), baseVoiceParam);
            }
        }
#endif
    }

    const std::uint64_t finalVoiceParam =
        g_OrigGetVoiceParamWithCallSign
        ? g_OrigGetVoiceParamWithCallSign(self, ownerIndex)
        : 0;

    return finalVoiceParam;
}


void Set_SoldierCallSign(std::uint32_t gameObjectId, std::uint8_t callSign)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[SoldierCallSign] Set ignored: invalid GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    if (callSign < kMinCallSign || callSign > kMaxCallSign)
    {
        Log("[SoldierCallSign] ERROR: SetRadioCallSign could not apply callSign %u to game object 0x%08X "
            "- valid call signs are %u to %u, so that soldier keeps its vanilla call sign.\n",
            static_cast<unsigned>(callSign), gameObjectId,
            static_cast<unsigned>(kMinCallSign), static_cast<unsigned>(kMaxCallSign));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_SoldierCallSigns[soldierIndex] = callSign;
    }

    LogDebug("[SoldierCallSign] REGISTERED callSign %u for game object 0x%08X (soldier index %u)\n",
        static_cast<unsigned>(callSign), gameObjectId, static_cast<unsigned>(soldierIndex));
}


void Remove_SoldierCallSign(std::uint32_t gameObjectId)
{
    const std::uint16_t soldierIndex =
        NormalizeSoldierIndexFromGameObjectId(gameObjectId);

    if (soldierIndex == 0xFFFFu)
    {
        LogDebug("[SoldierCallSign] Remove ignored: invalid GameObjectId=0x%08X\n", gameObjectId);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_SoldierCallSigns.erase(soldierIndex);
    }
}


void Clear_SoldierCallSigns()
{
    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_SoldierCallSigns.clear();
    }
}


bool Install_SoldierCallSign_Hook()
{
    void* target = ResolveGameAddress(gAddr.GetVoiceParamWithCallSign);
    if (!target)
    {
        Log("[Hook] SoldierCallSign: target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGetVoiceParamWithCallSign),
        reinterpret_cast<void**>(&g_OrigGetVoiceParamWithCallSign));

#ifdef _DEBUG
    Log("[Hook] SoldierCallSign: %s\n", ok ? "OK" : "FAIL");
#else
    if (!ok)
        Log("[Hook] SoldierCallSign: %s\n", ok ? "OK" : "FAIL");
#endif
    return ok;
}


bool Uninstall_SoldierCallSign_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.GetVoiceParamWithCallSign));
    g_OrigGetVoiceParamWithCallSign = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_CallSignMutex);
        g_SoldierCallSigns.clear();
    }

#ifdef _DEBUG
    LogDebug("[Hook] SoldierCallSign: removed\n");
#endif
    return true;
}