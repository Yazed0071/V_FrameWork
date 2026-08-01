#include "pch.h"
#include "WeaponPartsDevelopControllerImpl_GetLangEnhanceInfoMessageIdArray.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kCustomMax   = 16384;
    constexpr std::uint32_t kVanillaBound = 0x13a;

    static std::uint64_t             g_customIds[kCustomMax] = {};
    static std::atomic<std::uint32_t> g_customCount{ 0 };
    static bool                      g_ready  = false;
    static std::uint64_t             g_slot0  = 0;

    static const std::uint8_t kPartCount[12] = {
        0xea, 0x6a, 0xa1, 0x2b, 0x1d, 0x28, 0x19, 0x19, 0x0a, 0x0a, 0x17, 0xa1
    };

    using IndexForRegist_t = std::uint64_t(__fastcall*)(void* self, std::uint64_t langId);
    using MessageIdArray_t = void(__fastcall*)(void* self, int partsIndex, char partId,
                                               unsigned char maxCount, std::uint64_t* out);
    using GetById_t        = std::uint64_t*(__fastcall*)(void* self, std::uint64_t* out,
                                                         std::uint64_t index);
    using GetBound_t       = std::uint64_t(__fastcall*)(void* self);

    static IndexForRegist_t g_origIndex   = nullptr;
    static MessageIdArray_t g_origMsg     = nullptr;
    static GetById_t        g_origGetById = nullptr;
    static GetBound_t       g_origBound   = nullptr;

    static bool SafeReadU64(const std::uint64_t* p, std::uint64_t& out)
    {
        __try { out = *p; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static std::uint64_t __fastcall hkIndexForRegist(void* self, std::uint64_t langId)
    {
        const std::uint64_t idx = g_origIndex ? g_origIndex(self, langId) : 0;
        if (idx != 0 || !g_ready)
            return idx;

        const std::uint64_t m = langId & 0xffffffffffffull;
        if (m == 0 || m == (g_slot0 & 0xffffffffffffull))
            return 0;

        const std::uint32_t n = g_customCount.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < n; ++i)
            if ((g_customIds[i] & 0xffffffffffffull) == m)
                return kVanillaBound + i;

        if (n < kCustomMax)
        {
            g_customIds[n] = langId;
            g_customCount.store(n + 1, std::memory_order_release);
            Log("[EnhanceLangId] registered custom enhance lang id 0x%llx -> index %u "
                "(langPowerUpInfo / weapon-parts upgrade info)\n",
                static_cast<unsigned long long>(langId & 0xffffffffffffull),
                kVanillaBound + n);
            return kVanillaBound + n;
        }

        static bool s_fullLogged = false;
        if (!s_fullLogged)
        {
            s_fullLogged = true;
            Log("[EnhanceLangId] custom enhance lang-ID buffer full (%u) - further "
                "IDs will not register\n", kCustomMax);
        }
        return 0;
    }

    static std::uint64_t* __fastcall hkGetById(void* self, std::uint64_t* out,
                                               std::uint64_t index)
    {
        const std::uint64_t i = index & 0xffffull;
        if (g_ready && out && i >= kVanillaBound)
        {
            const std::uint32_t nc = g_customCount.load(std::memory_order_acquire);
            const std::uint64_t ci = i - kVanillaBound;
            if (ci < nc)
            {
                *out = g_customIds[ci];

                static std::atomic<int> s_served{ 0 };
                const int seen = s_served.fetch_add(1, std::memory_order_relaxed);
                if (seen < 16)
                    Log("[EnhanceLangId] develop menu served custom upgrade-info text: "
                        "index %llu -> lang id 0x%llx\n",
                        static_cast<unsigned long long>(i),
                        static_cast<unsigned long long>(g_customIds[ci] & 0xffffffffffffull));
                return out;
            }
        }

        if (g_origGetById)
            return g_origGetById(self, out, index);
        if (out)
            *out = 0;
        return out;
    }

    static std::uint64_t __fastcall hkGetBound(void* self)
    {
        const std::uint64_t base = g_origBound ? g_origBound(self) : kVanillaBound;
        if (!g_ready)
            return base;
        return base + g_customCount.load(std::memory_order_acquire);
    }

    static void FillCustomSEH(void* self, int partsIndex, char partId,
                              unsigned char maxCount, std::uint64_t* out,
                              std::uint32_t count)
    {
        __try
        {
            std::uint8_t* perPart =
                *reinterpret_cast<std::uint8_t**>(
                    reinterpret_cast<std::uint8_t*>(self) + 0x6f30
                    + static_cast<std::size_t>(partsIndex) * 8);
            if (!perPart)
                return;

            const std::uint32_t nc = g_customCount.load(std::memory_order_acquire);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (perPart[i * 0x28] != partId)
                    continue;
                for (std::uint32_t b = 0; b < 6; ++b)
                {
                    const std::uint16_t idx = *reinterpret_cast<std::uint16_t*>(
                        perPart + i * 0x28 + 0x18 + b * 2);
                    if (idx == 0)
                        break;
                    if (idx >= kVanillaBound && b < maxCount)
                    {
                        const std::uint32_t ci =
                            static_cast<std::uint32_t>(idx) - kVanillaBound;
                        if (ci < nc)
                            out[b] = g_customIds[ci];
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void __fastcall hkMessageIdArray(void* self, int partsIndex, char partId,
                                            unsigned char maxCount, std::uint64_t* out)
    {
        if (g_origMsg)
            g_origMsg(self, partsIndex, partId, maxCount, out);

        if (!g_ready || !out || partsIndex < 0 || partsIndex > 0xb)
            return;
        const std::uint32_t count = kPartCount[partsIndex];
        if (count == 0)
            return;
        FillCustomSEH(self, partsIndex, partId, maxCount, out, count);
    }
}

bool Install_EnhanceLangIdUnlimited()
{
    void* idxFn    = ResolveGameAddress(gAddr.WeaponEnhance_GetIndexForRegist);
    void* byIdFn   = ResolveGameAddress(gAddr.WeaponEnhance_GetLangIdById);
    void* boundFn  = ResolveGameAddress(gAddr.WeaponEnhance_GetLangIdBound);
    if (!idxFn || !byIdFn || !boundFn)
    {
        Log("[EnhanceLangId] addresses not set for this build - custom enhance "
            "lang IDs unavailable (feature skipped)\n");
        return true;
    }

    if (auto* arr = static_cast<std::uint64_t*>(
            ResolveGameAddress(gAddr.WeaponEnhanceLangIdArray)))
    {
        if (!SafeReadU64(arr, g_slot0))
            g_slot0 = 0;
    }

    const bool okIdx   = CreateAndEnableHook(
        idxFn, &hkIndexForRegist, reinterpret_cast<void**>(&g_origIndex));
    const bool okById  = CreateAndEnableHook(
        byIdFn, &hkGetById, reinterpret_cast<void**>(&g_origGetById));
    const bool okBound = CreateAndEnableHook(
        boundFn, &hkGetBound, reinterpret_cast<void**>(&g_origBound));
    if (!okIdx || !okById || !okBound)
    {
        Log("[EnhanceLangId] hook install FAILED (index=%d byId=%d bound=%d)\n",
            okIdx, okById, okBound);
        return false;
    }

    if (void* msgFn = ResolveGameAddress(gAddr.WeaponEnhance_GetMessageIdArray))
        CreateAndEnableHook(msgFn, &hkMessageIdArray,
                            reinterpret_cast<void**>(&g_origMsg));

    g_ready = true;
    Log("[EnhanceLangId] unlimited enhance lang IDs armed (cap %u custom; "
        "custom langPowerUpInfo IDs auto-register on lookup, served to the R&D "
        "develop menu and weapon-parts screen)\n", kCustomMax);
    return true;
}

bool Uninstall_EnhanceLangIdUnlimited()
{
    g_ready = false;
    if (gAddr.WeaponEnhance_GetIndexForRegist)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.WeaponEnhance_GetIndexForRegist));
    if (gAddr.WeaponEnhance_GetLangIdById)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.WeaponEnhance_GetLangIdById));
    if (gAddr.WeaponEnhance_GetLangIdBound)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.WeaponEnhance_GetLangIdBound));
    if (gAddr.WeaponEnhance_GetMessageIdArray)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.WeaponEnhance_GetMessageIdArray));
    g_origIndex   = nullptr;
    g_origMsg     = nullptr;
    g_origGetById = nullptr;
    g_origBound   = nullptr;
    return true;
}
