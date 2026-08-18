#include "pch.h"

#include "EquipDevelopControllerImpl_GetSuitDevelopInfoIndex.h"
#include "OutfitRegistry.h"
#include "CustomHeadRegistry.h"
#include "../equip/DevelopArrayGrow.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "../equip/EquipDevelop_SetEquipUndeveloped.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"
#include "../equip/MenuPerf.h"

namespace
{
    using EdcGetSuitIndex_t =
        std::uint32_t (__fastcall*)(void* self, std::uint32_t outfitByte,
                                    std::uint32_t camoByte);
    using EdcGetFaceIndex_t =
        std::uint32_t (__fastcall*)(void* self, std::uint32_t faceVal);

    using IsEquipVisile_t =
        std::uint8_t (__fastcall*)(void* self, std::uint16_t idx);

    using EdcGetSuitCamoType_t =
        std::uint32_t (__fastcall*)(void* self, std::uint32_t devIndex);
    using EdcGetSuitLevel_t =
        std::uint8_t  (__fastcall*)(void* self, std::uint32_t devIndex);

    using EdcIsEquipSuit_t =
        std::uint8_t (__fastcall*)(void* self, std::uint32_t playerType,
                                   std::uint32_t devIndex);


    static EdcGetSuitIndex_t  g_OrigEdcGetSuitIndex     = nullptr;
    static EdcGetFaceIndex_t  g_OrigEdcGetFaceIndex     = nullptr;
    static IsEquipVisile_t    g_OrigIsEquipVisile       = nullptr;
    static EdcGetSuitCamoType_t g_OrigEdcGetSuitCamoType = nullptr;
    static EdcGetSuitLevel_t    g_OrigEdcGetSuitLevel    = nullptr;
    static EdcIsEquipSuit_t     g_OrigEdcIsEquipSuit     = nullptr;
    static bool g_InstalledEdcGetSuitIndex = false;
    static bool g_InstalledEdcGetFaceIndex = false;
    static bool g_InstalledIsEquipVisile   = false;
    static bool g_InstalledGetSuitCamoType = false;
    static bool g_InstalledGetSuitLevel    = false;
    static bool g_InstalledIsEquipSuit     = false;

    constexpr std::size_t kDevelopHiddenCap = 0x10000;
    static std::uint8_t g_DevelopHiddenBits[kDevelopHiddenCap] = {};

    static void* g_CachedEDC = nullptr;

    constexpr std::uint32_t kDevelopIndexSentinel = 0x400;

    static std::uint32_t kEdcRowCapacityLive()
    {
        return equip::NativeFlowBound();
    }
    #define kEdcRowCapacity kEdcRowCapacityLive()

#define kDevelopedBitsOffset (equip::DevFlagsPtrOffsetBase20())

    std::mutex                  g_PendingDevelopedMutex;
    std::vector<std::uint16_t>  g_PendingDeveloped;

    std::unordered_set<std::uint16_t>  g_SuppressedDeveloped;

    static bool IsEnBuild()
    {
        return gAddr.EquipDevCtrl_GetSuitDevelopInfoIndex != 0
            || gAddr.EquipDevCtrl_GetFaceEquipDevelopInfoIndex != 0;
    }

    static void MarkDevelopedNow(void* edc, std::uint16_t flowIndex)
    {
        if (!edc || flowIndex >= kEdcRowCapacity)
            return;
        if (!IsEnBuild())
            return;

        __try
        {
            std::uint8_t** bitsPtr = reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(edc) + kDevelopedBitsOffset);
            std::uint8_t* bits = *bitsPtr;
            if (!bits)
                return;
            bits[flowIndex] |= 1u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void ClearDevelopedNow(void* edc, std::uint16_t flowIndex)
    {
        if (!edc || flowIndex >= kEdcRowCapacity)
            return;
        if (!IsEnBuild())
            return;

        __try
        {
            std::uint8_t** bitsPtr = reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(edc) + kDevelopedBitsOffset);
            std::uint8_t* bits = *bitsPtr;
            if (!bits)
                return;
            bits[flowIndex] &= ~static_cast<std::uint8_t>(1u);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void DrainPendingDeveloped(void* edc)
    {
        if (!edc)
            return;

        std::vector<std::uint16_t> drain;
        std::vector<std::uint16_t> suppressed;
        {
            std::lock_guard<std::mutex> lock(g_PendingDevelopedMutex);
            drain.swap(g_PendingDeveloped);
            suppressed.assign(g_SuppressedDeveloped.begin(),
                              g_SuppressedDeveloped.end());
        }

        for (std::uint16_t idx : drain)
            MarkDevelopedNow(edc, idx);

        for (std::uint16_t idx : suppressed)
            ClearDevelopedNow(edc, idx);

        EquipDevelop_DrainPendingUndevelops();
    }

    static std::uint32_t __fastcall hkEdcGetSuitIndex(
        void* self, std::uint32_t camoType, std::uint32_t level)
    {
        equip::MenuPerfScope _perf(equip::kPerf_GetSuitIdx);
        if (!g_CachedEDC && self)
        {
            g_CachedEDC = self;
            DrainPendingDeveloped(self);
        }

        const std::uint32_t ret = g_OrigEdcGetSuitIndex
            ? g_OrigEdcGetSuitIndex(self, camoType, level)
            : kDevelopIndexSentinel;

        std::uint8_t partsType = 0;
        std::uint8_t selector  = 0;
        const bool haveLive =
            outfit::GetCurrentEquippedSuitBytes(&partsType, &selector);

        if (ret != kDevelopIndexSentinel)
        {
            if (haveLive
                && partsType >= outfit::kCustomPartsTypeStart
                && partsType <= outfit::kCustomPartsTypeEnd
                && selector == static_cast<std::uint8_t>(camoType))
            {
                const outfit::OutfitEntry* worn = nullptr;
                std::uint8_t wvi = 0;
                if (!outfit::TryGetOutfitByVariantSelector(selector, &worn, &wvi))
                    outfit::TryGetOutfitByPartsType(partsType, &worn);
                if (worn && worn->flowIndex != 0
                    && worn->flowIndex < kEdcRowCapacity)
                {
                    return worn->flowIndex;
                }
            }
            return ret;
        }

        if (!haveLive)
            return ret;

        const outfit::OutfitEntry* suit = nullptr;
        std::uint8_t vi = 0;

        if (selector >= outfit::kCustomSelectorStart &&
            selector <= outfit::kCustomSelectorEnd)
        {
            outfit::TryGetOutfitByVariantSelector(selector, &suit, &vi);
        }
        if (!suit &&
            partsType >= outfit::kCustomPartsTypeStart &&
            partsType <= outfit::kCustomPartsTypeEnd)
        {
            outfit::TryGetOutfitByPartsType(partsType, &suit);
        }

        if (suit && suit->flowIndex != 0 && suit->flowIndex < kEdcRowCapacity)
            return suit->flowIndex;

        if (selector >= outfit::kCustomSelectorStart &&
            selector <= outfit::kCustomSelectorEnd &&
            partsType < outfit::kCustomPartsTypeStart &&
            g_OrigEdcGetSuitIndex)
        {
            std::uint8_t vpt  = 0;
            std::uint8_t vidx = 0;
            if (outfit::TryGetVanillaExtByVariantSelector(selector, &vpt, &vidx))
            {
                const std::uint8_t baseCamo =
                    outfit::VanillaExtGetVariantSourceCamo(vpt, vidx);
                if (baseCamo != 0xFF)
                {
                    const std::uint32_t baseRet =
                        g_OrigEdcGetSuitIndex(self, baseCamo, level);
                    if (baseRet != kDevelopIndexSentinel)
                        return baseRet;
                }
            }
        }

        return ret;
    }

    static std::uint32_t __fastcall hkEdcGetFaceIndex(void* self, std::uint32_t faceVal)
    {
        if (!g_CachedEDC && self)
        {
            g_CachedEDC = self;
            DrainPendingDeveloped(self);
        }

        const std::uint32_t ret = g_OrigEdcGetFaceIndex
            ? g_OrigEdcGetFaceIndex(self, faceVal)
            : kDevelopIndexSentinel;

        if (ret != kDevelopIndexSentinel)
        {
            if (faceVal >= 1 && faceVal <= 5)
            {
                const std::uint8_t livePT = outfit::ReadLivePartsType();
                if (livePT >= outfit::kCustomPartsTypeStart
                    && livePT <= outfit::kCustomPartsTypeEnd)
                {
                    const outfit::OutfitEntry* oe = nullptr;
                    const std::uint16_t vanEquipId =
                        static_cast<std::uint16_t>(faceVal + 0x20D);
                    if (outfit::TryGetOutfitByPartsType(livePT, &oe) && oe
                        && !oe->HasHeadOptionAnyVariant(
                               vanEquipId, outfit::ReadLivePlayerType()))
                        return kDevelopIndexSentinel;
                }
            }
            return ret;
        }

        std::uint32_t out = ret;
        const outfit::CustomHeadEntry* bySlot =
            outfit::TryGetCustomHeadBySlot(static_cast<std::uint8_t>(faceVal));
        const outfit::CustomHeadEntry* head = bySlot
            ? bySlot
            : outfit::TryGetCustomHeadByEquipId(
                  static_cast<std::uint16_t>(faceVal));
        if (head && head->equipId && head->equipId < kEdcRowCapacity)
        {
            bool resolve = true;
            if (bySlot)
            {
                const std::uint8_t livePT  = outfit::ReadLivePartsType();
                const std::uint8_t livePly = outfit::ReadLivePlayerType();
                const outfit::OutfitEntry* oe = nullptr;
                resolve =
                    (livePT >= outfit::kCustomPartsTypeStart
                     && livePT <= outfit::kCustomPartsTypeEnd
                     && outfit::TryGetOutfitByPartsType(livePT, &oe) && oe
                     && oe->HasHeadOptionAnyVariant(head->equipId, livePly))
                    || (livePT < outfit::kCustomPartsTypeStart
                        && outfit::VanillaExtHasHeadOption(
                               livePT, head->equipId, livePly,
                               outfit::ReadLiveSelectorCode()));
            }
            if (resolve)
                out = head->equipId;
#ifdef _DEBUG
            {
                static int s_probe = 0;
                if (s_probe < 12)
                {
                    ++s_probe;
                    LogDebug("[HeadSummary] GetFaceEquipDevelopInfoIndex: faceVal=0x%X "
                        "bySlot=%d livePT=0x%02X resolve=%d -> out=0x%X\n",
                        faceVal, bySlot ? 1 : 0,
                        static_cast<unsigned>(outfit::ReadLivePartsType()),
                        resolve ? 1 : 0, out);
                }
            }
#endif
        }

        return out;
    }

    static std::uint16_t ResolveWornCustomFlowIndex(std::uint8_t* outLiveSelector)
    {
        std::uint8_t partsType = 0;
        std::uint8_t selector  = 0;
        if (!outfit::GetCurrentEquippedSuitBytes(&partsType, &selector))
            return 0;

        const outfit::OutfitEntry* suit = nullptr;
        std::uint8_t vi = 0;
        if (selector >= outfit::kCustomSelectorStart &&
            selector <= outfit::kCustomSelectorEnd)
        {
            outfit::TryGetOutfitByVariantSelector(selector, &suit, &vi);
        }
        if (!suit &&
            partsType >= outfit::kCustomPartsTypeStart &&
            partsType <= outfit::kCustomPartsTypeEnd)
        {
            outfit::TryGetOutfitByPartsType(partsType, &suit);
        }

        if (!suit || suit->flowIndex == 0 || suit->flowIndex >= kEdcRowCapacity)
            return 0;
        if (outLiveSelector)
            *outLiveSelector = selector;
        return suit->flowIndex;
    }

    static std::uint8_t ReadLiveSuitLevelClamped(void* edc)
    {
        std::uint8_t lvl = 1;
        __try
        {
            auto* svm = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(edc) + 0x1e010);
            if (svm)
            {
                const std::uint8_t v = svm[0xBA0];
                lvl = v ? v : std::uint8_t{1};
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { lvl = 1; }
        return lvl;
    }

    static std::uint32_t __fastcall hkEdcGetSuitCamoType(
        void* self, std::uint32_t devIndex)
    {
        const std::uint32_t r = g_OrigEdcGetSuitCamoType
            ? g_OrigEdcGetSuitCamoType(self, devIndex)
            : 0xFFu;

        {
            std::uint8_t pt = 0, sel = 0;
            if (outfit::GetCurrentEquippedSuitBytes(&pt, &sel)
                && pt < outfit::kCustomPartsTypeStart
                && sel >= outfit::kCustomSelectorStart
                && sel <= outfit::kCustomSelectorEnd)
            {
                std::uint8_t vpt = 0, vidx = 0;
                if (outfit::TryGetVanillaExtByVariantSelector(sel, &vpt, &vidx))
                {
                    const std::uint8_t baseCamo =
                        outfit::VanillaExtGetVariantSourceCamo(vpt, vidx);
                    if (baseCamo != 0xFF
                        && static_cast<std::uint32_t>(baseCamo) == r)
                        return sel;
                }
            }
        }

        if (r != 0xFFu)
            return r;

        const outfit::OutfitEntry* rowOutfit = nullptr;
        if (!outfit::TryGetOutfitByFlowIndex(
                static_cast<std::uint16_t>(devIndex), &rowOutfit) || !rowOutfit)
            return r;

        std::uint8_t liveSel = 0;
        if (ResolveWornCustomFlowIndex(&liveSel) ==
                static_cast<std::uint16_t>(devIndex))
            return liveSel;
        return r;
    }

    static std::uint8_t __fastcall hkEdcGetSuitLevel(
        void* self, std::uint32_t devIndex)
    {
        const std::uint8_t r = g_OrigEdcGetSuitLevel
            ? g_OrigEdcGetSuitLevel(self, devIndex)
            : std::uint8_t{0};
        if (r != 0)
            return r;

        const outfit::OutfitEntry* rowOutfit = nullptr;
        if (!outfit::TryGetOutfitByFlowIndex(
                static_cast<std::uint16_t>(devIndex), &rowOutfit) || !rowOutfit)
            return r;

        if (ResolveWornCustomFlowIndex(nullptr) !=
                static_cast<std::uint16_t>(devIndex))
            return r;
        return IsEnBuild() ? ReadLiveSuitLevelClamped(self) : std::uint8_t{1};
    }

    static std::uint8_t __fastcall hkEdcIsEquipSuit(
        void* self, std::uint32_t playerType, std::uint32_t devIndex)
    {
        equip::MenuPerfScope _perf(equip::kPerf_IsEquipSuit);
        outfit::DrainPendingHeads();
        if (outfit::IsCustomHeadEquipId(static_cast<std::uint16_t>(devIndex)))
            return 0;

        const outfit::OutfitEntry* e = nullptr;
        if (playerType <= 3
            && outfit::TryGetOutfitByFlowIndex(
                   static_cast<std::uint16_t>(devIndex), &e) && e
            && !e->IsPlayerTypeSupported(static_cast<std::uint8_t>(playerType)))
        {
            return 0;
        }
        return g_OrigEdcIsEquipSuit
            ? g_OrigEdcIsEquipSuit(self, playerType, devIndex)
            : std::uint8_t{0};
    }

    using EdcGetBaseDevelopId_t =
        std::uint16_t (__fastcall*)(void* self, std::uint16_t idx);
    static EdcGetBaseDevelopId_t g_OrigEdcGetBaseDevelopId = nullptr;
    static bool g_InstalledGetBaseDevelopId = false;


    static std::uint16_t __fastcall hkEdcGetBaseDevelopId(
        void* self, std::uint16_t idx)
    {
        if (idx >= equip::NativeFlowBound())
        {
            static std::atomic<int> s_logged{ 0 };
            if (s_logged.fetch_add(1) < 4)
                LogDebug("[EquipDevelop] GetBaseDevelopId guarded: caller passed "
                    "index %u (record array holds %u) - returned 0.\n",
                    static_cast<unsigned>(idx),
                    equip::NativeFlowBound());
            return 0;
        }
        return g_OrigEdcGetBaseDevelopId
            ? g_OrigEdcGetBaseDevelopId(self, idx)
            : std::uint16_t{0};
    }

    constexpr DWORD kVisCacheTtlMs = 50;
    constexpr std::size_t kVisCacheSize = 0x10000;
    struct VisCacheEntry
    {
        DWORD        tick;
        std::uint8_t valid;
        std::uint8_t result;
    };
    static VisCacheEntry g_VisCache[kVisCacheSize] = {};

    static std::uint8_t __fastcall hkIsEquipVisile(void* self, std::uint16_t idx)
    {
        if (!g_CachedEDC && self)
        {
            g_CachedEDC = self;
            DrainPendingDeveloped(self);
        }

        if (g_DevelopHiddenBits[idx] != 0)
            return 0;

        const DWORD now = GetTickCount();
        VisCacheEntry& e = g_VisCache[idx];
        if (e.valid && (now - e.tick) < kVisCacheTtlMs)
            return e.result;

        const std::uint8_t r =
            g_OrigIsEquipVisile ? g_OrigIsEquipVisile(self, idx) : 0;
        e.tick   = now;
        e.result = r;
        e.valid  = 1;
        return r;
    }
}

namespace outfit
{
    bool Install_OutfitEquippedState_Hooks()
    {
        void* tEdcSuit   = ResolveGameAddress(gAddr.EquipDevCtrl_GetSuitDevelopInfoIndex);
        void* tEdcFace   = ResolveGameAddress(gAddr.EquipDevCtrl_GetFaceEquipDevelopInfoIndex);

        if (gAddr.EquipDevCtrl_GetSuitDevelopInfoIndex && tEdcSuit)
        {
            g_InstalledEdcGetSuitIndex = CreateAndEnableHook(
                tEdcSuit,
                reinterpret_cast<void*>(&hkEdcGetSuitIndex),
                reinterpret_cast<void**>(&g_OrigEdcGetSuitIndex));
        }

        if (gAddr.EquipDevCtrl_GetFaceEquipDevelopInfoIndex && tEdcFace)
        {
            g_InstalledEdcGetFaceIndex = CreateAndEnableHook(
                tEdcFace,
                reinterpret_cast<void*>(&hkEdcGetFaceIndex),
                reinterpret_cast<void**>(&g_OrigEdcGetFaceIndex));
        }

        if (void* tVisile = ResolveGameAddress(gAddr.EquipDevCtrl_IsEquipVisile);
            gAddr.EquipDevCtrl_IsEquipVisile && tVisile)
        {
            g_InstalledIsEquipVisile = CreateAndEnableHook(
                tVisile,
                reinterpret_cast<void*>(&hkIsEquipVisile),
                reinterpret_cast<void**>(&g_OrigIsEquipVisile));
            if (!g_InstalledIsEquipVisile)
                Log("[OutfitDevelopVisibility] ERROR: IsEquipVisile hook at %p "
                    "was refused - another module (DevelopArrayGrow claims the "
                    "same address) already owns it. The develop-menu hide filter "
                    "now runs from that owner instead; if it does not, "
                    "showInDevelopMenu=false is ignored and hidden rows appear "
                    "in R&D.\n", tVisile);
        }

        if (void* tCamo = ResolveGameAddress(gAddr.EquipDevCtrl_GetSuitCamoType);
            gAddr.EquipDevCtrl_GetSuitCamoType && tCamo)
        {
            g_InstalledGetSuitCamoType = CreateAndEnableHook(
                tCamo,
                reinterpret_cast<void*>(&hkEdcGetSuitCamoType),
                reinterpret_cast<void**>(&g_OrigEdcGetSuitCamoType));
        }

        if (void* tLevel = ResolveGameAddress(gAddr.EquipDevCtrl_GetSuitLevel);
            gAddr.EquipDevCtrl_GetSuitLevel && tLevel)
        {
            g_InstalledGetSuitLevel = CreateAndEnableHook(
                tLevel,
                reinterpret_cast<void*>(&hkEdcGetSuitLevel),
                reinterpret_cast<void**>(&g_OrigEdcGetSuitLevel));
        }

        if (void* tSuitGate = ResolveGameAddress(gAddr.EquipDevCtrl_IsEquipSuit);
            gAddr.EquipDevCtrl_IsEquipSuit && tSuitGate)
        {
            g_InstalledIsEquipSuit = CreateAndEnableHook(
                tSuitGate,
                reinterpret_cast<void*>(&hkEdcIsEquipSuit),
                reinterpret_cast<void**>(&g_OrigEdcIsEquipSuit));
        }

        if (void* tBase = ResolveGameAddress(gAddr.EquipDevCtrl_GetBaseDevelopId);
            gAddr.EquipDevCtrl_GetBaseDevelopId && tBase)
        {
            g_InstalledGetBaseDevelopId = CreateAndEnableHook(
                tBase,
                reinterpret_cast<void*>(&hkEdcGetBaseDevelopId),
                reinterpret_cast<void**>(&g_OrigEdcGetBaseDevelopId));
        }

        return true;
    }

    void Uninstall_OutfitEquippedState_Hooks()
    {
        if (g_InstalledEdcGetSuitIndex)
            DisableAndRemoveHook(
                ResolveGameAddress(gAddr.EquipDevCtrl_GetSuitDevelopInfoIndex));
        if (g_InstalledEdcGetFaceIndex)
            DisableAndRemoveHook(
                ResolveGameAddress(gAddr.EquipDevCtrl_GetFaceEquipDevelopInfoIndex));
        if (g_InstalledIsEquipVisile)
            DisableAndRemoveHook(
                ResolveGameAddress(gAddr.EquipDevCtrl_IsEquipVisile));
        if (g_InstalledGetSuitCamoType)
            DisableAndRemoveHook(
                ResolveGameAddress(gAddr.EquipDevCtrl_GetSuitCamoType));
        if (g_InstalledGetSuitLevel)
            DisableAndRemoveHook(
                ResolveGameAddress(gAddr.EquipDevCtrl_GetSuitLevel));
        if (g_InstalledIsEquipSuit)
            DisableAndRemoveHook(
                ResolveGameAddress(gAddr.EquipDevCtrl_IsEquipSuit));
        if (g_InstalledGetBaseDevelopId)
            DisableAndRemoveHook(
                ResolveGameAddress(gAddr.EquipDevCtrl_GetBaseDevelopId));
        g_OrigEdcGetBaseDevelopId = nullptr;
        g_InstalledGetBaseDevelopId = false;
        g_OrigEdcGetSuitIndex  = nullptr;
        g_OrigEdcGetFaceIndex  = nullptr;
        g_OrigIsEquipVisile    = nullptr;
        g_OrigEdcGetSuitCamoType = nullptr;
        g_OrigEdcGetSuitLevel    = nullptr;
        g_OrigEdcIsEquipSuit     = nullptr;
        g_CachedEDC            = nullptr;
        g_InstalledEdcGetSuitIndex = false;
        g_InstalledEdcGetFaceIndex = false;
        g_InstalledIsEquipVisile   = false;
        g_InstalledGetSuitCamoType = false;
        g_InstalledGetSuitLevel    = false;
        g_InstalledIsEquipSuit     = false;
    }

    void SetDevelopHidden(unsigned short index, bool hidden)
    {
        if (index < kDevelopHiddenCap)
            g_DevelopHiddenBits[index] = hidden ? 1 : 0;
        equip::InvalidateDevelopVisibilityCache();
    }

    bool IsDevelopHidden(unsigned short index)
    {
        return index < kDevelopHiddenCap && g_DevelopHiddenBits[index] != 0;
    }

    void* GetCachedEquipDevelopController()
    {
        return g_CachedEDC;
    }

    bool IsFlowIndexDevelopedByOrig(unsigned short)
    {
        return false;
    }

    void MarkDeveloped(unsigned short flowIndex)
    {
        if (flowIndex == 0 || flowIndex >= kEdcRowCapacity)
            return;
        if (!IsEnBuild())
            return;

        {
            std::lock_guard<std::mutex> lock(g_PendingDevelopedMutex);
            if (g_SuppressedDeveloped.count(flowIndex) != 0)
                return;
        }

        void* edc = g_CachedEDC;
        if (edc)
        {
            MarkDevelopedNow(edc, flowIndex);
            return;
        }

        std::lock_guard<std::mutex> lock(g_PendingDevelopedMutex);
        g_PendingDeveloped.push_back(flowIndex);
    }

    void SuppressDeveloped(unsigned short flowIndex)
    {
        if (flowIndex == 0 || flowIndex >= kEdcRowCapacity)
            return;
        if (!IsEnBuild())
            return;

        {
            std::lock_guard<std::mutex> lock(g_PendingDevelopedMutex);
            g_SuppressedDeveloped.insert(flowIndex);
            g_PendingDeveloped.erase(
                std::remove(g_PendingDeveloped.begin(), g_PendingDeveloped.end(), flowIndex),
                g_PendingDeveloped.end());
        }

        if (g_CachedEDC)
            ClearDevelopedNow(g_CachedEDC, flowIndex);
    }

    void ClearDevelopedInController(void* controller, unsigned short index)
    {
        if (!controller)
            return;
        ClearDevelopedNow(controller, index);
    }

    void InvalidateEquipVisibilityCache()
    {
        for (std::size_t i = 0; i < kVisCacheSize; ++i)
            g_VisCache[i].valid = 0;
    }
}
