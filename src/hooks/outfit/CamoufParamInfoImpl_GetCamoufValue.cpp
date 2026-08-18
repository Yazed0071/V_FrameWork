#include "pch.h"

#include "CamoufParamInfoImpl_GetCamoufValue.h"
#include "OutfitRegistry.h"

#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"


namespace
{
    using GetCamoufValue_t =
        int (__fastcall*)(void* self,
                          std::uint32_t camoType,
                          std::uint32_t materialType);

    static GetCamoufValue_t g_OrigGetCamoufValue = nullptr;
    static bool             g_Installed          = false;

#ifdef _DEBUG
    static std::atomic<bool> g_FirstVirtualHitLogged{ false };
#endif

    static int __fastcall hkGetCamoufValue(
        void* self, std::uint32_t camoType, std::uint32_t materialType)
    {
        if (camoType <= outfit::kVanillaCamoTypeMax)
        {
            return g_OrigGetCamoufValue
                ? g_OrigGetCamoufValue(self, camoType, materialType)
                : 0;
        }


        const outfit::OutfitEntry* entry = nullptr;
        std::uint8_t                ownerPT = 0;
        if (!outfit::TryGetOutfitByCamoVirtualId(
                static_cast<std::uint8_t>(camoType), &entry, &ownerPT)
            || !entry)
        {
            return 0;
        }

        if (materialType >= outfit::kCamoMaterialCount)
            return 0;

        const std::int32_t* row = entry->GetCamoBonusValuesFor(ownerPT);
        if (!row) return 0;

        const std::int32_t v = row[materialType];

#ifdef _DEBUG
        if (!g_FirstVirtualHitLogged.exchange(true))
        {
            LogDebug("[OutfitGetCamoufValue] FIRST VIRTUAL HIT: "
                "virtualId=%u materialType=%u value=%d ownerPT=%u "
                "(developId=%u flowIndex=%u)\n",
                static_cast<unsigned>(camoType),
                static_cast<unsigned>(materialType),
                static_cast<int>(v),
                static_cast<unsigned>(ownerPT),
                static_cast<unsigned>(entry->developId),
                static_cast<unsigned>(entry->flowIndex));
        }
#endif

        return v;
    }
}

namespace outfit
{
    bool Install_OutfitGetCamoufValue_Hook()
    {
        if (g_Installed) return true;

        void* target = ResolveGameAddress(gAddr.CamoufParamInfo_GetCamoufValue);
        if (!target)
        {
            LogDebug("[OutfitGetCamoufValue] target unresolved; module disabled "
                "(camoBonusValues unique-row feature will be inactive)\n");
            return false;
        }

        g_Installed = CreateAndEnableHook(
            target,
            reinterpret_cast<void*>(&hkGetCamoufValue),
            reinterpret_cast<void**>(&g_OrigGetCamoufValue));

#ifdef _DEBUG
        Log("[OutfitGetCamoufValue] installed: %s (target=%p)\n",
            g_Installed ? "OK" : "FAIL", target);
#endif
        return g_Installed;
    }

    void Uninstall_OutfitGetCamoufValue_Hook()
    {
        if (!g_Installed) return;
        if (void* t = ResolveGameAddress(gAddr.CamoufParamInfo_GetCamoufValue))
            DisableAndRemoveHook(t);
        g_OrigGetCamoufValue = nullptr;
        g_Installed          = false;
#ifdef _DEBUG
        LogDebug("[OutfitGetCamoufValue] removed\n");
#endif
    }
}
