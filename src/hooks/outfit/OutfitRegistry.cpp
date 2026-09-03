#include "pch.h"

#include "OutfitRegistry.h"
#include "ShadowState.h"
#include "FoxHashes.h"
#include "AdditionalMotionTable_GetMtarPathId.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <unordered_map>

#include "AddressSet.h"
#include "log.h"
#include <HookUtils.h>
#include "../../core/V_FrameWorkState.h"
#include "UniqueCharacterDefaultOutfit.h"

namespace
{
    using outfit::kCustomPartsTypeStart;
    using outfit::kCustomPartsTypeEnd;
    using outfit::kCustomSelectorStart;
    using outfit::kCustomSelectorEnd;
    using outfit::kPlayerTypeMax;
    using outfit::OutfitEntry;
    using outfit::OutfitDefinition;
    using outfit::OutfitPlayerTypeData;

    using GetQuarkSystemTable_t = void* (__fastcall*)();

    static std::deque<OutfitEntry> g_Entries;
    static std::mutex                            g_Mutex;
    static GetQuarkSystemTable_t                 g_GetQuarkSystemTable = nullptr;


    static std::atomic<std::uint8_t> g_ActiveVariant[256] = {};


    static std::uint16_t g_PendingDevelopId            = 0;
    static std::uint32_t g_PendingDevelopIdStamp       = 0;
    static std::uint16_t g_PendingHeadOptionEquipId    = 0;
    static std::uint8_t  g_WornCustomHeadSlot          = 0;
    static bool          g_SupplyDropClickLatch        = false;
    static std::uint16_t g_PendingSupplyDropDevelopId  = 0;
    static std::uint8_t  g_PendingSupplyDropVariantIdx = 0;
    static std::uint16_t g_CrateDeliveredDevelopId     = 0;
    static std::uint8_t  g_CrateDeliveredVariantIdx    = 0;

    static constexpr std::size_t kMaxVanillaExtVariants = 29;
    static_assert(V_FrameWorkState::kPersistedVariantSelectorSlots
                      == outfit::kMaxVariantsPerOutfit - 1,
                  "persisted variant-selector slots must track kMaxVariantsPerOutfit-1");

    struct VanillaSuitExtension
    {
        bool          used             = false;
        std::uint8_t  vanillaPartsType = 0xFF;
        outfit::VanillaSuitHeadExt perPlayerType[kPlayerTypeMax] = {};

        std::uint8_t  variantCount[kPlayerTypeMax] = {};
        outfit::VanillaSuitVariantAsset
            variants[kPlayerTypeMax][kMaxVanillaExtVariants] = {};

        std::uint8_t  variantSelectorCodes[kMaxVanillaExtVariants] = {};
        std::uint8_t  variantSourceCamo[kMaxVanillaExtVariants] = {};
        std::uint8_t  variantSelectorCount = 0;

        std::uint64_t suitVoiceFpk[kPlayerTypeMax] = {};
        std::uint8_t  suitVoiceCamo[kPlayerTypeMax] = {};

        bool          suitArmOverride[kPlayerTypeMax]  = {};
        bool          suitArmEnable[kPlayerTypeMax]    = {};
        std::uint8_t  suitArmCamo[kPlayerTypeMax]      = {};
        bool          suitHeadOverride[kPlayerTypeMax] = {};
        bool          suitHeadEnable[kPlayerTypeMax]   = {};
        std::uint8_t  suitHeadCamo[kPlayerTypeMax]     = {};
        bool          suitAbilityOverride[kPlayerTypeMax]  = {};
        std::uint8_t  suitAbilityCamo[kPlayerTypeMax]      = {};
        bool          suitAbilitySilent[kPlayerTypeMax]    = {};
        std::uint8_t  suitAbilityDefense[kPlayerTypeMax]   = {};
        std::uint8_t  suitAbilityRegen[kPlayerTypeMax]     = {};
        std::uint32_t suitAbilityRattle[kPlayerTypeMax]    = {};
        std::uint32_t suitAbilityDamageSe[kPlayerTypeMax]  = {};
        std::uint8_t  suitAbilityParamKind[kPlayerTypeMax] = {};
    };
    static std::deque<VanillaSuitExtension> g_VanillaExts;

    static VanillaSuitExtension* FindVanillaExt_NoLock(std::uint8_t partsType)
    {
        for (auto& x : g_VanillaExts)
            if (x.used && x.vanillaPartsType == partsType) return &x;
        return nullptr;
    }

    static VanillaSuitExtension* FindOrCreateVanillaExt_NoLock(std::uint8_t partsType)
    {
        VanillaSuitExtension* x = FindVanillaExt_NoLock(partsType);
        if (x) return x;
        g_VanillaExts.emplace_back();
        x = &g_VanillaExts.back();
        x->used             = true;
        x->vanillaPartsType = partsType;
        for (std::uint8_t i = 0; i < kMaxVanillaExtVariants; ++i)
            x->variantSourceCamo[i] = 0xFF;
        return x;
    }

    struct PendingSummaryDisplay
    {
        std::uint64_t nameHash = 0;
        std::uint64_t iconHash = 0;
    };
    static std::unordered_map<std::uint16_t, PendingSummaryDisplay>
        g_PendingSummaryDisplay;

    static bool ResolveQuarkApi()
    {
        if (!g_GetQuarkSystemTable)
        {
            g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
                ResolveGameAddress(gAddr.GetQuarkSystemTable));
        }
        return g_GetQuarkSystemTable != nullptr;
    }

    static std::uint8_t* GetQuarkLiveState()
    {
        if (!ResolveQuarkApi()) return nullptr;
        auto* qt = reinterpret_cast<std::uint8_t*>(g_GetQuarkSystemTable());
        if (!qt) return nullptr;
        auto* q98 = *reinterpret_cast<std::uint8_t**>(qt + 0x98);
        if (!q98) return nullptr;
        return *reinterpret_cast<std::uint8_t**>(q98 + 0x10);
    }

    static bool IsCustomPartsType(std::uint8_t v)
    {
        return v >= kCustomPartsTypeStart && v <= kCustomPartsTypeEnd;
    }

    static bool IsCustomSelector(std::uint8_t v)
    {
        return v >= kCustomSelectorStart && v <= kCustomSelectorEnd;
    }

    static bool IsAllocatableSelector(std::uint8_t v)
    {
        return IsCustomSelector(v)
            && !(v >= outfit::kVanillaEventCamoStart
              && v <= outfit::kVanillaEventCamoEnd);
    }

    static bool          g_ReservationsBuilt = false;
    static std::uint64_t g_SelectorReservedBy[256]  = {};
    static std::uint64_t g_PartsTypeReservedBy[256] = {};

    static std::uint64_t HashOutfitKey(const char* key)
    {
        std::uint64_t h = 1469598103934665603ull;
        if (key)
            for (const unsigned char* p =
                     reinterpret_cast<const unsigned char*>(key); *p; ++p)
            { h ^= *p; h *= 1099511628211ull; }
        return h ? h : 1ull;
    }

    static std::uint16_t ResolvePersistedFlowIndex(const char* key)
    {
        if (!key || !key[0]) return 0;
        const std::int32_t fi = V_FrameWorkState::GetPersistedFlowIndex(key);
        if (fi <= 0 || fi > 0xFFFF) return 0;
        return static_cast<std::uint16_t>(fi);
    }

    static void BuildReservations_NoLock()
    {
        if (g_ReservationsBuilt) return;
        g_ReservationsBuilt = true;

        std::size_t reservedSel = 0, reservedPt = 0, conflicts = 0;
        V_FrameWorkState::ForEachPersistedOutfit(
            [&](const std::string& key, std::uint8_t pt, std::uint8_t sel,
                const std::uint8_t* variants)
            {
                const std::uint64_t h = HashOutfitKey(key.c_str());
                if (IsCustomPartsType(pt))
                {
                    if (g_PartsTypeReservedBy[pt] == 0)
                    { g_PartsTypeReservedBy[pt] = h; ++reservedPt; }
                    else if (g_PartsTypeReservedBy[pt] != h) ++conflicts;
                }
                auto reserveSel = [&](std::uint8_t s)
                {
                    if (!IsAllocatableSelector(s)) return;
                    if (g_SelectorReservedBy[s] == 0)
                    { g_SelectorReservedBy[s] = h; ++reservedSel; }
                    else if (g_SelectorReservedBy[s] != h) ++conflicts;
                };
                reserveSel(sel);
                for (std::size_t i = 0;
                     i < V_FrameWorkState::kPersistedVariantSelectorSlots; ++i)
                    reserveSel(variants[i]);
            });

        if (reservedSel || reservedPt || conflicts)
        {
#ifdef _DEBUG
            LogDebug("[OutfitRegistry] reservation pass: %zu selector(s) + %zu "
                "partsType(s) reserved for persisted outfit keys%s\n",
                reservedSel, reservedPt,
                conflicts ? " (persisted-value CONFLICTS detected - "
                            "first-loaded key wins, later keys re-allocate)"
                          : "");
#endif
        }
    }

    static bool IsKeyHashRegistered_NoLock(std::uint64_t h)
    {
        if (h == 0) return false;
        for (const auto& e : g_Entries)
            if (e.used && e.keyHash == h) return true;
        return false;
    }

    static std::uint8_t AllocatePartsType_NoLock(std::uint8_t hint,
                                                 std::uint64_t keyHash)
    {
        auto takenBy = [&](std::uint8_t candidate)
        {
            for (const auto& e : g_Entries)
                if (e.used && e.partsType == candidate) return true;
            return false;
        };

        if (IsCustomPartsType(hint) && !takenBy(hint)
            && (g_PartsTypeReservedBy[hint] == 0
             || g_PartsTypeReservedBy[hint] == keyHash))
        {
            return hint;
        }

        for (std::uint16_t v = kCustomPartsTypeStart; v <= kCustomPartsTypeEnd; ++v)
        {
            const auto candidate = static_cast<std::uint8_t>(v);
            if (takenBy(candidate)) continue;
            if (g_PartsTypeReservedBy[candidate] != 0
             && g_PartsTypeReservedBy[candidate] != keyHash) continue;
            return candidate;
        }
        for (std::uint16_t v = kCustomPartsTypeStart; v <= kCustomPartsTypeEnd; ++v)
        {
            const auto candidate = static_cast<std::uint8_t>(v);
            if (takenBy(candidate)) continue;
            if (IsKeyHashRegistered_NoLock(g_PartsTypeReservedBy[candidate]))
                continue;
            LogDebug("[OutfitRegistry] partsType pool exhausted - consuming "
                "reserved value 0x%02X from an absent key\n", candidate);
            return candidate;
        }
        return 0xFF;
    }


    static bool IsSelectorTaken_NoLock(std::uint8_t code)
    {
        for (const auto& e : g_Entries)
        {
            if (!e.used) continue;
            if (e.selectorCode == code) return true;


            for (std::uint8_t k = 1; k < e.variantCount; ++k)
            {
                if (e.variantSelectorCodes[k] == code) return true;
            }
        }
        for (const auto& x : g_VanillaExts)
        {
            if (!x.used) continue;
            for (std::uint8_t k = 0; k < x.variantSelectorCount; ++k)
                if (x.variantSelectorCodes[k] == code) return true;
        }
        return false;
    }

    static bool IsSelectorReservedForOther_NoLock(std::uint8_t code,
                                                  std::uint64_t keyHash)
    {
        return g_SelectorReservedBy[code] != 0
            && g_SelectorReservedBy[code] != keyHash;
    }

    static bool IsVirtualIdTaken_NoLock(std::uint8_t virtualId);

    static std::uint8_t AllocateSelector_NoLock(std::uint8_t hint,
                                                std::uint64_t keyHash)
    {
        if (IsCustomSelector(hint) && !IsAllocatableSelector(hint))
            LogDebug("[OutfitRegistry] selector hint 0x%02X is inside the vanilla "
                     "event-camo band 0x83-0x88 (legacy) - migrating to a clean "
                     "selector\n", hint);

        if (IsAllocatableSelector(hint) && !IsSelectorTaken_NoLock(hint)
            && !IsSelectorReservedForOther_NoLock(hint, keyHash)
            && !IsVirtualIdTaken_NoLock(hint))
            return hint;

        for (std::uint16_t v = kCustomSelectorStart; v <= kCustomSelectorEnd; ++v)
        {
            const auto candidate = static_cast<std::uint8_t>(v);
            if (!IsAllocatableSelector(candidate)) continue;
            if (IsSelectorTaken_NoLock(candidate)) continue;
            if (IsSelectorReservedForOther_NoLock(candidate, keyHash)) continue;
            if (IsVirtualIdTaken_NoLock(candidate)) continue;
            return candidate;
        }
        for (std::uint16_t v = kCustomSelectorStart; v <= kCustomSelectorEnd; ++v)
        {
            const auto candidate = static_cast<std::uint8_t>(v);
            if (!IsAllocatableSelector(candidate)) continue;
            if (IsSelectorTaken_NoLock(candidate)) continue;
            if (IsVirtualIdTaken_NoLock(candidate)) continue;
            if (IsKeyHashRegistered_NoLock(g_SelectorReservedBy[candidate]))
                continue;
            LogDebug("[OutfitRegistry] selector pool exhausted - consuming reserved "
                "value 0x%02X from an absent key\n", candidate);
            return candidate;
        }
        return 0xFF;
    }

    static bool IsVirtualIdTaken_NoLock(std::uint8_t virtualId)
    {
        for (const auto& e : g_Entries)
        {
            if (!e.used) continue;
            for (const auto& b : e.perPlayerType)
            {
                if (b.used && b.hasCamoBonusValues
                    && b.camoBonusType == virtualId)
                    return true;
            }
        }
        return false;
    }


    static std::uint8_t AllocateVirtualId_NoLock()
    {
        for (std::uint16_t v = outfit::kCamoVirtualIdStart;
             v <= outfit::kCamoVirtualIdEnd; ++v)
        {
            const auto cand = static_cast<std::uint8_t>(v);
            if (cand >= outfit::kVanillaEventCamoStart
                && cand <= outfit::kVanillaEventCamoEnd)
                continue;
            if (IsVirtualIdTaken_NoLock(cand)) continue;
            if (IsCustomSelector(cand)
                && (IsSelectorTaken_NoLock(cand)
                    || g_SelectorReservedBy[cand] != 0))
                continue;
            return cand;
        }
        return 0xFF;
    }


    static bool TryEvictOne_NoLock(const char* forKey);

    static bool ReclaimByte_NoLock(std::uint8_t wanted, std::uint64_t forKeyHash,
                                   const char* forKey, bool probeOnly);

    static bool ShouldLogBindRefusal_NoLock(std::uint64_t keyHash,
                                            const char* reason)
    {
        if (!reason
            || (std::strcmp(reason, "menu-stamp") != 0
                && std::strcmp(reason, "row-window") != 0))
            return true;
        static std::unordered_set<std::uint64_t> s_menuStampLogged;
        return s_menuStampLogged.insert(keyHash).second;
    }

    static void PublishSuitParamDonors_NoLock(const OutfitEntry& e);
    static void ClearSuitParamDonors_NoLock(const OutfitEntry& e);

    static bool BindOutfit_NoLock(OutfitEntry& e, bool allowEvict,
                                  const char* reason, bool requireHintExact = false)
    {
        if (e.bound)
            return true;

        if (requireHintExact)
        {
            const bool needPt =
                AllocatePartsType_NoLock(e.partsTypeHint, e.keyHash)
                    != e.partsTypeHint;
            const bool needSel =
                AllocateSelector_NoLock(e.selectorCodeHint, e.keyHash)
                    != e.selectorCodeHint;
            if (needPt || needSel)
            {
                const bool canPt = !needPt
                    || ReclaimByte_NoLock(e.partsTypeHint, e.keyHash, e.key, true);
                const bool canSel = !needSel
                    || ReclaimByte_NoLock(e.selectorCodeHint, e.keyHash, e.key, true);
                if (canPt && canSel)
                {
                    if (needPt)
                        ReclaimByte_NoLock(e.partsTypeHint, e.keyHash, e.key, false);
                    if (needSel)
                        ReclaimByte_NoLock(e.selectorCodeHint, e.keyHash, e.key, false);
                }
            }
        }

        std::uint8_t partsType =
            AllocatePartsType_NoLock(e.partsTypeHint, e.keyHash);
        if (partsType == 0xFF && allowEvict && TryEvictOne_NoLock(e.key))
            partsType = AllocatePartsType_NoLock(e.partsTypeHint, e.keyHash);
        if (partsType == 0xFF)
        {
            if (ShouldLogBindRefusal_NoLock(e.keyHash, reason))
                LogDebug("[OutfitRegistry] BIND REFUSED key=%s reason=%s: partsType "
                         "pool exhausted (%d live) - unequip or clear loadout slots "
                         "to free live bytes\n", e.key, reason ? reason : "?",
                    kCustomPartsTypeEnd - kCustomPartsTypeStart + 1);
            return false;
        }
        if (requireHintExact && partsType != e.partsTypeHint)
        {
            LogDebug("[OutfitRegistry] recognition bind key=%s: reserved partsType "
                "0x%02X unavailable (got 0x%02X) - no bind, degrades to "
                "vanilla\n", e.key, e.partsTypeHint, partsType);
            return false;
        }
        std::uint8_t selector =
            AllocateSelector_NoLock(e.selectorCodeHint, e.keyHash);
        if (selector == 0xFF && allowEvict && TryEvictOne_NoLock(e.key))
            selector = AllocateSelector_NoLock(e.selectorCodeHint, e.keyHash);
        if (selector == 0xFF)
        {
            if (ShouldLogBindRefusal_NoLock(e.keyHash, reason))
                LogDebug("[OutfitRegistry] BIND REFUSED key=%s reason=%s: selector "
                    "pool exhausted (%d live)\n",
                    e.key, reason ? reason : "?",
                    kCustomSelectorEnd - kCustomSelectorStart + 1
                  - (outfit::kVanillaEventCamoEnd
                   - outfit::kVanillaEventCamoStart + 1));
            return false;
        }
        if (requireHintExact && selector != e.selectorCodeHint)
        {
            LogDebug("[OutfitRegistry] recognition bind key=%s: reserved selector "
                "0x%02X unavailable (got 0x%02X) - no bind, degrades to "
                "vanilla\n", e.key, e.selectorCodeHint, selector);
            return false;
        }

        std::uint8_t variantSelectors[outfit::kMaxVariantsPerOutfit];
        for (auto& v : variantSelectors) v = 0xFF;
        variantSelectors[0] = selector;
        for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
        {
            auto usedEarlier = [&](std::uint8_t c)
            {
                for (std::uint8_t k = 0; k < vi; ++k)
                    if (variantSelectors[k] == c) return true;
                return false;
            };

            std::uint8_t alloc = 0xFF;
            const std::uint8_t hint = e.variantSelectorHints[vi];
            if (IsAllocatableSelector(hint) && !IsSelectorTaken_NoLock(hint)
                && !usedEarlier(hint)
                && !IsVirtualIdTaken_NoLock(hint)
                && !IsSelectorReservedForOther_NoLock(hint, e.keyHash))
                alloc = hint;
            if (alloc == 0xFF)
            {
                for (std::uint16_t cand = kCustomSelectorStart;
                     cand <= kCustomSelectorEnd; ++cand)
                {
                    const auto c = static_cast<std::uint8_t>(cand);
                    if (!IsAllocatableSelector(c)) continue;
                    if (IsSelectorTaken_NoLock(c)) continue;
                    if (usedEarlier(c)) continue;
                    if (IsVirtualIdTaken_NoLock(c)) continue;
                    if (IsSelectorReservedForOther_NoLock(c, e.keyHash)) continue;
                    alloc = c; break;
                }
            }
            if (alloc == 0xFF)
            {
                for (std::uint16_t cand = kCustomSelectorStart;
                     cand <= kCustomSelectorEnd; ++cand)
                {
                    const auto c = static_cast<std::uint8_t>(cand);
                    if (!IsAllocatableSelector(c)) continue;
                    if (IsSelectorTaken_NoLock(c)) continue;
                    if (usedEarlier(c)) continue;
                    if (IsVirtualIdTaken_NoLock(c)) continue;
                    LogDebug("[OutfitRegistry] selector pool exhausted - variant %u "
                        "consumes reserved value 0x%02X from an absent key\n",
                        static_cast<unsigned>(vi), c);
                    alloc = c; break;
                }
            }
            if (alloc == 0xFF)
            {
                if (ShouldLogBindRefusal_NoLock(e.keyHash, reason))
                    LogDebug("[OutfitRegistry] BIND REFUSED key=%s reason=%s: no "
                        "free selector for variant %u of %u\n",
                        e.key, reason ? reason : "?",
                        static_cast<unsigned>(vi),
                        static_cast<unsigned>(e.variantCount));
                return false;
            }
            variantSelectors[vi] = alloc;
        }

        e.partsType    = partsType;
        e.selectorCode = selector;
        for (std::size_t i = 0; i < outfit::kMaxVariantsPerOutfit; ++i)
            e.variantSelectorCodes[i] = variantSelectors[i];

        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            auto& b = e.perPlayerType[pt];
            if (!b.used || !b.hasCamoBonusValues) continue;
            if (b.camoBonusType != outfit::kCamoBonusTypeUnset) continue;
            const std::uint8_t vid = AllocateVirtualId_NoLock();
            if (vid == 0xFF)
            {
                LogDebug("[OutfitRegistry] camo virtual-id pool exhausted while "
                    "binding PT=%u of '%s' - branch runs without a unique "
                    "bonus row\n", static_cast<unsigned>(pt), e.key);
                b.hasCamoBonusValues = false;
            }
            else
            {
                b.camoBonusType = vid;
            }
        }

        e.bound = true;
        e.lastBindTouch = GetTickCount64();
        PublishSuitParamDonors_NoLock(e);
        g_ActiveVariant[e.partsType] = e.defaultVariant;
        V_FrameWorkState::SetPersistedOutfitIds(e.key, e.partsType,
                                                e.selectorCode);
        if (e.variantCount > 1)
            V_FrameWorkState::SetPersistedOutfitVariantSelectors(
                e.key, e.variantSelectorCodes + 1,
                static_cast<std::size_t>(e.variantCount) - 1);
#ifdef _DEBUG
        LogDebug("[OutfitRegistry] bound key=%s partsType=0x%02X selector=0x%02X "
            "(%s)\n", e.key, e.partsType, e.selectorCode,
            reason ? reason : "?");
#endif
        return true;
    }

    static std::atomic<std::uint8_t>
        g_SuitParamDonor[256][kPlayerTypeMax] = {};
    static std::atomic<std::uint8_t>
        g_AbilityDefense[256][kPlayerTypeMax] = {};
    static std::atomic<std::uint8_t>
        g_AbilityRegen[256][kPlayerTypeMax] = {};

    static constexpr std::size_t kAbilityVariantSlots = 8;
    static std::atomic<std::uint8_t>
        g_VarDefense[256][kPlayerTypeMax][kAbilityVariantSlots] = {};
    static std::atomic<std::uint8_t>
        g_VarRegen[256][kPlayerTypeMax][kAbilityVariantSlots] = {};

    static void PublishSuitParamDonors_NoLock(const OutfitEntry& e)
    {
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            const OutfitPlayerTypeData* pd = e.GetPTData(pt);
            const std::uint8_t kind = pd ? pd->suitParamKind : std::uint8_t{ 0 };

            const std::uint8_t def = pd ? pd->abilityDefense      : std::uint8_t{ 0 };
            const std::uint8_t reg = pd ? pd->abilityLifeRecovery : std::uint8_t{ 0 };

            if (e.selectorCode != 0)
            {
                g_SuitParamDonor[e.selectorCode][pt].store(
                    kind, std::memory_order_relaxed);
                g_AbilityDefense[e.selectorCode][pt].store(
                    def, std::memory_order_relaxed);
                g_AbilityRegen[e.selectorCode][pt].store(
                    reg, std::memory_order_relaxed);
            }

            for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
            {
                const std::uint8_t sel = e.variantSelectorCodes[vi];
                if (sel == 0) continue;
                g_SuitParamDonor[sel][pt].store(kind, std::memory_order_relaxed);
                g_AbilityDefense[sel][pt].store(def, std::memory_order_relaxed);
                g_AbilityRegen[sel][pt].store(reg, std::memory_order_relaxed);
            }

            const std::uint8_t branchVars =
                pd ? pd->variantCount : std::uint8_t{ 0 };
            for (std::uint8_t vi = 0;
                 vi < branchVars && vi < kAbilityVariantSlots; ++vi)
            {
                const outfit::OutfitVariant* v = pd->Var(vi);
                const bool own = v && v->abilities.declared;
                const std::uint8_t d =
                    own ? static_cast<std::uint8_t>(v->abilities.defense + 1)
                        : std::uint8_t{ 0 };
                const std::uint8_t r =
                    own ? static_cast<std::uint8_t>(v->abilities.lifeRecovery + 1)
                        : std::uint8_t{ 0 };
                if (e.selectorCode != 0)
                {
                    g_VarDefense[e.selectorCode][pt][vi].store(
                        d, std::memory_order_relaxed);
                    g_VarRegen[e.selectorCode][pt][vi].store(
                        r, std::memory_order_relaxed);
                }
                const std::uint8_t vsel = e.variantSelectorCodes[vi];
                if (vsel != 0)
                {
                    g_VarDefense[vsel][pt][vi].store(d, std::memory_order_relaxed);
                    g_VarRegen[vsel][pt][vi].store(r, std::memory_order_relaxed);
                }
            }

            if (branchVars > kAbilityVariantSlots)
            {
                static std::atomic<int> s_varCap{ 0 };
                if (s_varCap.fetch_add(1, std::memory_order_relaxed) < 4)
                    Log("[OutfitRegistry] '%s' declares %u variants but only the "
                        "first %zu carry their own abilities - variants past that "
                        "fall back to the branch's abilities\n",
                        e.key, static_cast<unsigned>(branchVars),
                        kAbilityVariantSlots);
            }
        }
    }

    static void ClearSuitParamDonors_NoLock(const OutfitEntry& e)
    {
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            if (e.selectorCode != 0)
            {
                g_SuitParamDonor[e.selectorCode][pt].store(
                    0, std::memory_order_relaxed);
                g_AbilityDefense[e.selectorCode][pt].store(
                    0, std::memory_order_relaxed);
                g_AbilityRegen[e.selectorCode][pt].store(
                    0, std::memory_order_relaxed);
            }
            for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
            {
                const std::uint8_t sel = e.variantSelectorCodes[vi];
                if (sel == 0) continue;
                g_SuitParamDonor[sel][pt].store(0, std::memory_order_relaxed);
                g_AbilityDefense[sel][pt].store(0, std::memory_order_relaxed);
                g_AbilityRegen[sel][pt].store(0, std::memory_order_relaxed);
            }
        }
    }

    static void UnbindOutfit_NoLock(OutfitEntry& e)
    {
        if (!e.bound)
            return;
        ClearSuitParamDonors_NoLock(e);
        g_ActiveVariant[e.partsType] = 0;
        e.partsType    = 0;
        e.selectorCode = 0;
        for (std::size_t i = 0; i < outfit::kMaxVariantsPerOutfit; ++i)
            e.variantSelectorCodes[i] = 0xFF;
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            auto& b = e.perPlayerType[pt];
            b.camoBonusType = b.camoBonusTypeDeclared;
        }
        e.bound = false;
    }

    static void SummarizeBranches(const OutfitDefinition& def,
                                  std::uint8_t& outBranchCount,
                                  std::uint8_t& outMaxVariants)
    {
        outBranchCount = 0;
        outMaxVariants = 0;
        for (const auto& b : def.perPlayerType)
        {
            if (!b.used) continue;
            ++outBranchCount;
            if (b.variantCount > outMaxVariants) outMaxVariants = b.variantCount;
        }
    }

    static std::unordered_set<std::uint16_t> g_AppliedThisSession;
    static std::unordered_set<std::uint16_t> g_OrderedThisSession;
    static std::unordered_set<std::uint16_t> g_MenuStampSet;

    static constexpr std::uint64_t kMenuStampFreshMs = 500;
    static constexpr std::uint64_t kRowBindRetryMs   = 300;
    static std::unordered_map<std::uint16_t, std::uint64_t> g_MenuStampTick;
    static std::unordered_map<std::uint16_t, std::uint64_t> g_RowBindBackoff;

    static int ReadStateBytesSEH(const std::uint8_t* state, std::size_t off,
                                 std::uint8_t* out, std::size_t n)
    {
        __try
        {
            for (std::size_t i = 0; i < n; ++i) out[i] = state[off + i];
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static bool EntryOwnsSelector_NoLock(const OutfitEntry& e, std::uint8_t sel)
    {
        if (!e.bound || sel == 0 || sel == 0xFF) return false;
        if (e.selectorCode == sel) return true;
        for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
            if (e.variantSelectorCodes[vi] == sel) return true;
        return false;
    }

    enum PinScope : unsigned
    {
        kPinCore            = 0u,
        kPinAppliedOrdered  = 1u << 0,
        kPinMenuStamps      = 1u << 1,
        kPinMenuStampsFresh = 1u << 2,
        kPinAll             = kPinAppliedOrdered | kPinMenuStamps,
    };

    struct RememberedOutfit
    {
        std::uint8_t partsType = 0;
        std::uint8_t selector  = 0;
        bool         valid     = false;
    };
    static RememberedOutfit g_LastOutfitPerPT[kPlayerTypeMax] = {};

    static bool GetRememberedPlayerTypeOutfitNoLock(std::uint8_t playerType,
                                                    std::uint8_t* outPartsType,
                                                    std::uint8_t* outSelector)
    {
        if (playerType >= kPlayerTypeMax) return false;

        if (g_LastOutfitPerPT[playerType].valid)
        {
            if (outPartsType) *outPartsType = g_LastOutfitPerPT[playerType].partsType;
            if (outSelector)  *outSelector  = g_LastOutfitPerPT[playerType].selector;
            return true;
        }

        const char* key = uniquedefaultoutfit::GetDefaultOutfitKey(playerType);
        if (!key || !key[0]) return false;

        for (auto& e : g_Entries)
        {
            if (!e.used) continue;
            if (std::strncmp(e.key, key, sizeof(e.key) - 1) != 0) continue;
            if (!e.bound || e.partsType == 0) return false;
            if (outPartsType) *outPartsType = e.partsType;
            if (outSelector)  *outSelector  = e.selectorCode;
            return true;
        }
        return false;
    }

    static void ComputePins_NoLock(std::vector<bool>& pinned, bool& scanAvailable,
                                   unsigned scope = kPinAll)
    {
        pinned.assign(g_Entries.size(), false);
        scanAvailable = true;

        auto pinByBytes = [&](std::uint8_t pt, std::uint8_t sel)
        {
            std::size_t idx = 0;
            for (const auto& e : g_Entries)
            {
                if (e.used && e.bound
                    && ((IsCustomPartsType(pt) && e.partsType == pt)
                        || EntryOwnsSelector_NoLock(e, sel)))
                    pinned[idx] = true;
                ++idx;
            }
        };
        auto pinByDevelopId = [&](std::uint16_t devId)
        {
            std::size_t idx = 0;
            for (const auto& e : g_Entries)
            {
                if (e.used && e.developId == devId) pinned[idx] = true;
                ++idx;
            }
        };

        std::uint8_t* state = GetQuarkLiveState();
        if (!state)
        {
            scanAvailable = false;
            return;
        }

        std::uint8_t worn[3] = {};
        if (ReadStateBytesSEH(state, 0xF8, worn, 3) != 1)
        {
            scanAvailable = false;
            return;
        }
        pinByBytes(worn[0], worn[1]);

        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            std::uint8_t rpt = 0, rsel = 0;
            if (GetRememberedPlayerTypeOutfitNoLock(pt, &rpt, &rsel))
                pinByBytes(rpt, rsel);
        }

        std::uint8_t fmt = 0;
        if (ReadStateBytesSEH(state, 0x1129, &fmt, 1) == 1 && fmt == 3)
        {
            for (int slot = 0; slot < 3; ++slot)
            {
                std::uint8_t row[3] = {};
                if (ReadStateBytesSEH(state,
                        0x112A + static_cast<std::size_t>(slot) * 0x73, row, 3) == 1)
                    pinByBytes(row[0], row[1]);
            }
        }
        else if (fmt != 3)
        {
            scanAvailable = false;
        }

        if (scope & kPinAppliedOrdered)
        {
            for (const std::uint16_t d : g_AppliedThisSession) pinByDevelopId(d);
            for (const std::uint16_t d : g_OrderedThisSession) pinByDevelopId(d);
        }
        if (scope & kPinMenuStamps)
            for (const std::uint16_t d : g_MenuStampSet)       pinByDevelopId(d);
        else if (scope & kPinMenuStampsFresh)
        {
            const std::uint64_t now = GetTickCount64();
            for (const std::uint16_t d : g_MenuStampSet)
            {
                auto it = g_MenuStampTick.find(d);
                if (it != g_MenuStampTick.end()
                    && now - it->second <= kMenuStampFreshMs)
                    pinByDevelopId(d);
            }
        }

        if (g_PendingSupplyDropDevelopId != 0)
            pinByDevelopId(g_PendingSupplyDropDevelopId);
        if (g_CrateDeliveredDevelopId != 0)
            pinByDevelopId(g_CrateDeliveredDevelopId);
    }

    static void ReleaseVictimBytes_NoLock(OutfitEntry& victim, const char* tier,
                                          const char* forKey)
    {
        const std::uint8_t vPt  = victim.partsType;
        const std::uint8_t vSel = victim.selectorCode;
        std::uint8_t vVars[outfit::kMaxVariantsPerOutfit];
        for (std::size_t i = 0; i < outfit::kMaxVariantsPerOutfit; ++i)
            vVars[i] = victim.variantSelectorCodes[i];

        LogDebug("[OutfitRegistry] recycled live bytes of key=%s (%s) to "
            "make room for key=%s\n", victim.key, tier, forKey ? forKey : "?");
        UnbindOutfit_NoLock(victim);

        if (IsCustomPartsType(vPt))
            g_PartsTypeReservedBy[vPt] = 0;
        auto releaseSel = [&](std::uint8_t s)
        {
            if (IsAllocatableSelector(s))
                g_SelectorReservedBy[s] = 0;
        };
        releaseSel(vSel);
        for (std::size_t i = 1; i < outfit::kMaxVariantsPerOutfit; ++i)
            releaseSel(vVars[i]);
        V_FrameWorkState::ClearPersistedOutfitIds(victim.key);
    }

    static bool TryEvictStaleStamp_NoLock(const char* forKey)
    {
        std::vector<bool> pinned;
        bool scanAvailable = false;
        ComputePins_NoLock(pinned, scanAvailable,
                           kPinAppliedOrdered | kPinMenuStampsFresh);
        if (!scanAvailable)
            return false;

        OutfitEntry*  best     = nullptr;
        int           bestCat  = 2;
        std::uint64_t bestTick = ~0ull;
        std::size_t   idx      = 0;
        for (auto& e : g_Entries)
        {
            const std::size_t here = idx++;
            if (!e.used || !e.bound)
                continue;
            if (here < pinned.size() && pinned[here])
                continue;

            int           cat  = 0;
            std::uint64_t tick = e.lastBindTouch;
            if (g_MenuStampSet.count(e.developId))
            {
                auto it = g_MenuStampTick.find(e.developId);
                cat  = 1;
                tick = (it != g_MenuStampTick.end()) ? it->second : 0;
            }
            if (!best || cat < bestCat
                || (cat == bestCat && tick < bestTick))
            {
                best     = &e;
                bestCat  = cat;
                bestTick = tick;
            }
        }
        if (!best)
            return false;

        ReleaseVictimBytes_NoLock(*best,
                                  bestCat == 0
                                      ? "menu-window, idle leftover"
                                      : "menu-window, off-screen stale",
                                  forKey);
        return true;
    }

    static bool TryEvictOne_NoLock(const char* forKey)
    {
        auto pickVictim = [&](unsigned scope, const char** tierOut,
                              const char* tierName) -> OutfitEntry*
        {
            std::vector<bool> pinned;
            bool scanAvailable = false;
            ComputePins_NoLock(pinned, scanAvailable, scope);
            if (!scanAvailable) return nullptr;

            OutfitEntry* best = nullptr;
            std::size_t idx = 0;
            for (auto& e : g_Entries)
            {
                if (e.used && e.bound && idx < pinned.size() && !pinned[idx])
                {
                    if (!best || e.lastBindTouch < best->lastBindTouch)
                        best = &e;
                }
                ++idx;
            }
            if (best) *tierOut = tierName;
            return best;
        };

        const char* tier  = nullptr;
        OutfitEntry* victim = pickVictim(kPinAll, &tier, "unpinned, LRU");
        if (!victim)
            victim = pickVictim(kPinAppliedOrdered, &tier,
                                "menu-listed only, LRU - no idle byte left");
        if (!victim)
            victim = pickVictim(kPinCore, &tier,
                                "tried-on/ordered earlier, LRU - last resort");
        if (!victim)
            return false;

        ReleaseVictimBytes_NoLock(*victim, tier, forKey);
        return true;
    }

    static bool ReclaimByte_NoLock(std::uint8_t wanted, std::uint64_t forKeyHash,
                                   const char* forKey, bool probeOnly)
    {
        if (wanted == 0xFF) return false;
        if (!IsCustomPartsType(wanted) && !IsAllocatableSelector(wanted))
            return false;

        std::vector<bool> pinned;
        bool scanAvailable = false;
        ComputePins_NoLock(pinned, scanAvailable, kPinCore);
        if (!scanAvailable) return false;

        std::size_t idx = 0;
        for (auto& e : g_Entries)
        {
            const std::size_t here = idx++;
            if (!e.used || !e.bound) continue;
            if (e.keyHash == forKeyHash) continue;

            const bool holdsIt =
                (IsCustomPartsType(wanted) && e.partsType == wanted)
                || EntryOwnsSelector_NoLock(e, wanted);
            if (!holdsIt) continue;

            if (here < pinned.size() && pinned[here])
            {
                if (!probeOnly)
                    LogDebug("[OutfitRegistry] reclaim of 0x%02X for key=%s "
                             "refused: holder key=%s is pinned "
                             "(worn/loadout/supply) - the requester degrades to "
                             "vanilla\n",
                        wanted, forKey ? forKey : "?", e.key);
                return false;
            }
            if (probeOnly) return true;

            const std::uint8_t rPt  = e.partsType;
            const std::uint8_t rSel = e.selectorCode;
            std::uint8_t rVars[outfit::kMaxVariantsPerOutfit];
            for (std::size_t i = 0; i < outfit::kMaxVariantsPerOutfit; ++i)
                rVars[i] = e.variantSelectorCodes[i];

            LogDebug("[OutfitRegistry] reclaimed 0x%02X from key=%s for its persisted "
                "owner key=%s\n", wanted, e.key, forKey ? forKey : "?");
            UnbindOutfit_NoLock(e);

            if (IsCustomPartsType(rPt))    g_PartsTypeReservedBy[rPt] = 0;
            if (IsAllocatableSelector(rSel)) g_SelectorReservedBy[rSel] = 0;
            for (std::size_t i = 1; i < outfit::kMaxVariantsPerOutfit; ++i)
                if (IsAllocatableSelector(rVars[i]))
                    g_SelectorReservedBy[rVars[i]] = 0;
            V_FrameWorkState::ClearPersistedOutfitIds(e.key);
            return true;
        }
        return false;
    }
}

namespace outfit
{

    bool IsSnakeAvatarBridge(std::uint8_t a, std::uint8_t b)
    {
        return (a == kPlayerType_Snake && b == kPlayerType_Avatar)
            || (a == kPlayerType_Avatar && b == kPlayerType_Snake);
    }


    const OutfitPlayerTypeData* OutfitEntry::GetPTData(std::uint8_t playerType) const
    {
        if (playerType >= kPlayerTypeMax) return nullptr;
        if (perPlayerType[playerType].used)
            return &perPlayerType[playerType];

        if (IsUniqueCharacterPlayerType(playerType))
            return nullptr;

        if (playerType == kPlayerType_Snake
            && perPlayerType[kPlayerType_Avatar].used)
            return &perPlayerType[kPlayerType_Avatar];
        if (playerType == kPlayerType_Avatar
            && perPlayerType[kPlayerType_Snake].used)
            return &perPlayerType[kPlayerType_Snake];

        for (std::uint8_t alt = 0; alt < kPlayerTypeMax; ++alt)
            if (!IsUniqueCharacterPlayerType(alt) && perPlayerType[alt].used
                && IsFemaleBodyPlayerType(alt) == IsFemaleBodyPlayerType(playerType))
                return &perPlayerType[alt];

        return nullptr;
    }

    bool OutfitEntry::IsPlayerTypeSupported(std::uint8_t playerType) const
    {
        return GetPTData(playerType) != nullptr;
    }

    bool OutfitEntry::DeclaresPlayerType(std::uint8_t playerType) const
    {
        return playerType < kPlayerTypeMax && perPlayerType[playerType].used;
    }

    std::uint8_t OutfitEntry::FirstSupportedPlayerType() const
    {
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            if (!IsUniqueCharacterPlayerType(pt) && perPlayerType[pt].used)
                return pt == kPlayerType_Avatar ? kPlayerType_Snake : pt;
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            if (perPlayerType[pt].used)
                return pt;
        return 0xFF;
    }


    bool OutfitEntry::IsArmEnabled(std::uint8_t playerType) const
    {
        if (playerType == kPlayerType_DDMale
            || playerType == kPlayerType_DDFemale
            || IsUniqueCharacterPlayerType(playerType))
            return false;
        const auto* d = GetPTData(playerType);
        if (!d) return false;
        const std::uint8_t vi = GetActiveVariant(partsType);
        if (const auto* v = (vi >= 1) ? d->Var(vi) : nullptr;
            v && v->used && v->hasEnableArm)
            return v->enableArm;
        return d->enableArm;
    }

    bool OutfitEntry::IsHeadEnabled(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return false;
        const std::uint8_t vi = GetActiveVariant(partsType);
        if (const auto* v = (vi >= 1) ? d->Var(vi) : nullptr;
            v && v->used && v->hasEnableHead)
            return v->enableHead;
        return d->enableHead;
    }

    bool OutfitEntry::IsCamoCustom(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->camoFpk > kSubAssetUseVanilla;
    }

    bool OutfitEntry::IsCamoFv2Custom(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->camoFv2 > kSubAssetUseVanilla;
    }

    bool OutfitEntry::IsFaceEnabled(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->faceFpk != kSubAssetDisabled;
    }

    bool OutfitEntry::IsDiamondEnabled(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->diamondFpk != kSubAssetDisabled;
    }

    bool OutfitEntry::IsDiamondCustom(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->diamondFpk > kSubAssetUseVanilla;
    }

    bool OutfitEntry::IsDiamondFv2Custom(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->diamondFv2 > kSubAssetUseVanilla;
    }

    bool OutfitEntry::IsVoiceCustom(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->voiceFpk > kSubAssetUseVanilla;
    }


    bool OutfitEntry::HasHeadOptions(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->supportsHeadOptions && d->headOptionCount > 0;
    }

    bool OutfitEntry::HasHeadOption(std::uint16_t equipId, std::uint8_t playerType) const
    {
        const std::uint16_t* ids = nullptr;
        std::uint8_t count = 0;
        if (!GetHeadOptionsFor(playerType, &ids, &count) || !ids) return false;
        for (std::uint8_t i = 0; i < count; ++i)
            if (ids[i] == equipId) return true;
        return false;
    }

    bool OutfitEntry::HasAnyHeadOptions() const
    {
        for (const auto& b : perPlayerType)
        {
            if (b.used && b.supportsHeadOptions && b.headOptionCount > 0)
                return true;
        }
        return false;
    }

    bool OutfitEntry::GetHeadOptionsFor(std::uint8_t playerType,
                                        const std::uint16_t** outEquipIds,
                                        std::uint8_t* outCount) const
    {
        if (const auto* d = GetPTData(playerType);
            d && d->supportsHeadOptions && d->headOptionCount > 0)
        {
            if (outEquipIds) *outEquipIds = d->headOptionEquipIds;
            if (outCount)    *outCount    = d->headOptionCount;
            return true;
        }
        if (outEquipIds) *outEquipIds = nullptr;
        if (outCount)    *outCount    = 0;
        return false;
    }

    bool OutfitEntry::GetHeadOptionsForVariant(std::uint8_t playerType,
                                               std::uint8_t variant,
                                               const std::uint16_t** outEquipIds,
                                               std::uint8_t* outCount) const
    {
        const auto* d = GetPTData(playerType);
        const auto* v = (d && variant >= 1) ? d->Var(variant) : nullptr;
        if (v && v->used)
        {
            if (v->headOptionsDeclared && v->headOptionCount > 0)
            {
                if (outEquipIds) *outEquipIds = v->headOptionEquipIds;
                if (outCount)    *outCount    = v->headOptionCount;
                return true;
            }
            if (outEquipIds) *outEquipIds = nullptr;
            if (outCount)    *outCount    = 0;
            return false;
        }
        return GetHeadOptionsFor(playerType, outEquipIds, outCount);
    }

    bool OutfitEntry::HasHeadOptionsForVariant(std::uint8_t playerType,
                                               std::uint8_t variant) const
    {
        const std::uint16_t* ids = nullptr;
        std::uint8_t count = 0;
        return GetHeadOptionsForVariant(playerType, variant, &ids, &count)
            && ids && count > 0;
    }

    bool OutfitEntry::HasHeadOptionForVariant(std::uint16_t equipId,
                                              std::uint8_t playerType,
                                              std::uint8_t variant) const
    {
        const std::uint16_t* ids = nullptr;
        std::uint8_t count = 0;
        if (!GetHeadOptionsForVariant(playerType, variant, &ids, &count) || !ids)
            return false;
        for (std::uint8_t i = 0; i < count; ++i)
            if (ids[i] == equipId) return true;
        return false;
    }

    bool OutfitEntry::HasHeadOptionAnyVariant(std::uint16_t equipId,
                                              std::uint8_t playerType) const
    {
        if (HasHeadOption(equipId, playerType)) return true;
        const auto* d = GetPTData(playerType);
        if (!d) return false;
        for (std::size_t vi = 1; vi < d->variants.size(); ++vi)
        {
            const auto& v = d->variants[vi];
            if (!v.used || !v.headOptionsDeclared) continue;
            for (std::uint8_t i = 0; i < v.headOptionCount; ++i)
                if (v.headOptionEquipIds[i] == equipId) return true;
        }
        return false;
    }

    int ResolvePendingHeadName(std::uint64_t nameHash, std::uint16_t equipId)
    {
        if (nameHash == 0 || equipId == 0) return 0;

        auto fill = [&](std::uint16_t* ids, std::uint8_t& count,
                        std::uint64_t* pending, std::uint8_t& pendingCount,
                        bool* supports,
                        std::uint8_t* idCamos, std::uint8_t* pendingCamos) -> int
        {
            int filled = 0;
            std::uint8_t keep = 0;
            for (std::uint8_t r = 0; r < pendingCount; ++r)
            {
                if (pending[r] != nameHash)
                {
                    if (pendingCamos) pendingCamos[keep] = pendingCamos[r];
                    pending[keep++] = pending[r];
                    continue;
                }
                const std::uint8_t camo =
                    pendingCamos ? pendingCamos[r] : kHeadOptionAnyCamo;
                bool dup = false;
                for (std::uint8_t j = 0; j < count; ++j)
                    if (ids[j] == equipId
                        && (!idCamos || idCamos[j] == camo))
                    { dup = true; break; }
                if (!dup && count < kMaxHeadOptionsPerOutfit)
                {
                    if (idCamos) idCamos[count] = camo;
                    ids[count++] = equipId;
                    if (supports) *supports = true;
                    ++filled;
                }
            }
            pendingCount = keep;
            return filled;
        };

        std::lock_guard<std::mutex> lock(g_Mutex);
        int total = 0;
        for (auto& e : g_Entries)
        {
            if (!e.used) continue;
            for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            {
                auto& b = e.perPlayerType[pt];
                if (!b.used) continue;
                total += fill(b.headOptionEquipIds, b.headOptionCount,
                              b.pendingHeadNameHashes, b.pendingHeadCount,
                              &b.supportsHeadOptions, nullptr, nullptr);
                for (std::size_t vi = 1; vi < b.variants.size(); ++vi)
                {
                    auto& v = b.variants[vi];
                    if (!v.used) continue;
                    total += fill(v.headOptionEquipIds, v.headOptionCount,
                                  v.pendingHeadNameHashes, v.pendingHeadCount,
                                  nullptr, nullptr, nullptr);
                }
            }
        }
        for (auto& x : g_VanillaExts)
        {
            if (!x.used) continue;
            for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            {
                auto& b = x.perPlayerType[pt];
                if (!b.declared) continue;
                total += fill(b.headOptionEquipIds, b.headOptionCount,
                              b.pendingHeadNameHashes, b.pendingHeadCount,
                              nullptr, b.headOptionSourceCamo,
                              b.pendingHeadSourceCamo);
            }
        }
        return total;
    }

    int RemapHeadOptionEquipId(std::uint16_t oldEquipId, std::uint16_t newEquipId)
    {
        if (oldEquipId == 0 || newEquipId == 0 || oldEquipId == newEquipId)
            return 0;

        auto patch = [&](std::uint16_t* ids, std::uint8_t count) -> int
        {
            int n = 0;
            for (std::uint8_t i = 0; i < count; ++i)
            {
                if (ids[i] == oldEquipId)
                {
                    ids[i] = newEquipId;
                    ++n;
                }
            }
            return n;
        };

        std::lock_guard<std::mutex> lock(g_Mutex);
        int patched = 0;
        for (auto& e : g_Entries)
        {
            if (!e.used) continue;
            for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            {
                auto& b = e.perPlayerType[pt];
                if (!b.used) continue;
                patched += patch(b.headOptionEquipIds, b.headOptionCount);
                for (auto& v : b.variants)
                {
                    if (v.used)
                        patched += patch(v.headOptionEquipIds,
                                         v.headOptionCount);
                }
            }
        }
        for (auto& x : g_VanillaExts)
        {
            if (!x.used) continue;
            for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            {
                auto& b = x.perPlayerType[pt];
                if (!b.declared) continue;
                patched += patch(b.headOptionEquipIds, b.headOptionCount);
            }
        }
        return patched;
    }


    std::uint8_t OutfitEntry::GetVariantCountFor(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d ? d->variantCount : std::uint8_t{0};
    }

    static std::unordered_map<std::uint16_t, std::uint8_t> g_VariantWindowStart;

    std::uint8_t GetVariantWindowStart(std::uint16_t developId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_VariantWindowStart.find(developId);
        return it != g_VariantWindowStart.end() ? it->second : std::uint8_t{0};
    }

    void SetVariantWindowStart(std::uint16_t developId, std::uint8_t start)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (start == 0)
            g_VariantWindowStart.erase(developId);
        else
            g_VariantWindowStart[developId] = start;
    }

    std::uint8_t OutfitEntry::GetVariantSelectorCode(std::uint8_t variantIdx) const
    {
        if (variantIdx < kMaxVariantsPerOutfit)
        {
            const std::uint8_t sc = variantSelectorCodes[variantIdx];
            if (sc != 0 && sc != 0xFF)
                return sc;
        }
        return selectorCode;
    }


    std::uint8_t OutfitEntry::GetCamoBonusType(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d ? d->camoBonusType : kCamoBonusTypeUnset;
    }

    bool OutfitEntry::HasCamoBonusValuesFor(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return d && d->hasCamoBonusValues;
    }

    const std::int32_t* OutfitEntry::GetCamoBonusValuesFor(std::uint8_t playerType) const
    {
        const auto* d = GetPTData(playerType);
        return (d && d->hasCamoBonusValues) ? d->camoBonusValues : nullptr;
    }


    std::uint64_t OutfitEntry::GetVariantPartsPath(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return 0;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (!v || !v->used)
            return d->partsPathCode64;
        return v->partsPathCode64 != 0
             ? v->partsPathCode64
             : d->partsPathCode64;
    }

    std::uint64_t OutfitEntry::GetVariantFpkPath(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return 0;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (!v || !v->used)
            return d->fpkPathCode64;
        return v->fpkPathCode64 != 0
             ? v->fpkPathCode64
             : d->fpkPathCode64;
    }

    std::uint64_t OutfitEntry::GetVariantCamoFpk(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return kSubAssetUseVanilla;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (!v || !v->used)
            return d->camoFpk;
        return v->camoFpk;
    }

    std::uint64_t OutfitEntry::GetVariantCamoFv2(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return kSubAssetUseVanilla;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (!v || !v->used)
            return d->camoFv2;
        return v->camoFv2;
    }

    std::uint64_t OutfitEntry::GetVariantDiamondFpk(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return kSubAssetDisabled;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (!v || !v->used)
            return d->diamondFpk;
        return v->diamondFpk;
    }

    static const char* const kMotionMtarNames[kMotionMtarSlotCount] = {
        "avatar_edit", "behind", "camera", "carry",
        "cbox", "cqc", "cure", "cypr",
        "ddf_facial", "ddm_facial", "elude", "facial_ddf_helispace",
        "facial_ddm_helispace", "facial_snake_helispace", "gimmick", "heli",
        "horse", "jump", "ladder", "liquid",
        "ocelot_facial", "okb_zero", "online", "paz",
        "pipe", "quiet_facial", "resident", "timecigarette",
        "trashbox", "vehicle", "vram_resident", "TppPlayer2Facial",
    };

    static std::uint64_t g_MotionMtarVanillaHashes[kMotionMtarSlotCount] = {};
    static std::once_flag g_MotionMtarHashOnce;

    static void BuildMotionMtarHashes()
    {
        for (std::size_t i = 0; i < kMotionMtarSlotCount; ++i)
        {
            std::string p = "/Assets/tpp/motion/mtar/player2/";
            if (std::strcmp(kMotionMtarNames[i], "TppPlayer2Facial") == 0)
                p += "TppPlayer2Facial.mtar";
            else
                p += std::string("player2_") + kMotionMtarNames[i] + ".mtar";
            g_MotionMtarVanillaHashes[i] = FoxHashes::PathCode64Ext(p);
        }
    }

    int MotionMtarSlotFromName(const char* name)
    {
        if (!name || !*name) return -1;
        for (std::size_t i = 0; i < kMotionMtarSlotCount; ++i)
            if (_stricmp(name, kMotionMtarNames[i]) == 0)
                return static_cast<int>(i);
        return -1;
    }

    std::uint64_t MotionMtarVanillaHash(std::size_t slot)
    {
        if (slot >= kMotionMtarSlotCount) return 0;
        std::call_once(g_MotionMtarHashOnce, BuildMotionMtarHashes);
        return g_MotionMtarVanillaHashes[slot];
    }

    int MotionMtarSlotFromVanillaHash(std::uint64_t pathHash)
    {
        if (pathHash == 0) return -1;
        std::call_once(g_MotionMtarHashOnce, BuildMotionMtarHashes);
        for (std::size_t i = 0; i < kMotionMtarSlotCount; ++i)
            if (g_MotionMtarVanillaHashes[i] == pathHash)
                return static_cast<int>(i);
        return -1;
    }

    std::uint64_t OutfitEntry::GetMotionMtarOverride(
        std::uint8_t playerType, std::size_t slot, std::uint8_t variantIdx) const
    {
        if (slot >= kMotionMtarSlotCount) return 0;
        const auto* d = GetPTData(playerType);
        if (!d) return 0;
        if (variantIdx != 0 && variantIdx < d->variantCount)
        {
            const auto* v = d->Var(variantIdx);
            if (v && v->used && v->motionMtars[slot] != 0)
                return v->motionMtars[slot];
        }
        return d->motionMtars[slot];
    }

    std::uint64_t OutfitEntry::GetVariantDiamondFv2(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return kSubAssetUseVanilla;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (!v || !v->used)
            return d->diamondFv2;
        return v->diamondFv2;
    }

    std::uint64_t OutfitEntry::GetVariantVoiceFpk(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return kSubAssetUseVanilla;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (!v || !v->used)
            return d->voiceFpk;
        return v->voiceFpk;
    }

    std::uint64_t OutfitEntry::GetVariantDisplayNameHash(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return 0;
        if (variantIdx == 0) return d->baseDisplayNameHash;
        if (variantIdx >= d->variantCount) return 0;
        const auto* v = d->Var(variantIdx);
        return v ? v->displayNameHash : 0;
    }

    std::uint64_t OutfitEntry::GetVariantIconPathHash(
        std::uint8_t playerType, std::uint8_t variantIdx) const
    {
        const auto* d = GetPTData(playerType);
        if (!d) return 0;
        const auto* v = (variantIdx != 0 && variantIdx < d->variantCount)
            ? d->Var(variantIdx) : nullptr;
        if (v && v->used && v->iconPathHash != 0)
            return v->iconPathHash;
        return d->baseIconPathHash;
    }


    bool RegisterOutfit(const OutfitDefinition& def, std::uint8_t* outAllocatedPartsType)
    {

        std::uint8_t branchCount  = 0;
        std::uint8_t maxVariants  = 0;
        SummarizeBranches(def, branchCount, maxVariants);

        if (branchCount == 0)
        {
            LogDebug("[OutfitRegistry] reject: no playerType branches populated - "
                     "at least one of snake/ddMale/ddFemale/avatar must supply "
                     "partsPath and fpkPath (key=%s)\n",
                def.key ? def.key : "(unkeyed)");
            return false;
        }


        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            const auto& b = def.perPlayerType[pt];
            if (!b.used) continue;
            if (b.partsPathCode64 == 0 || b.fpkPathCode64 == 0)
            {
                LogDebug("[OutfitRegistry] reject: playerType=%u branch is missing "
                    "required partsPath or fpkPath (key=%s)\n",
                    static_cast<unsigned>(pt),
                    def.key ? def.key : "(unkeyed)");
                return false;
            }
        }

        if (def.developId == 0)
        {
            LogDebug("[OutfitRegistry] reject: developId must be non-zero "
                "(key=%s)\n", def.key ? def.key : "(unkeyed)");
            return false;
        }


        constexpr std::uint16_t kEdcRowCapacity = 0x400;
        if (def.flowIndex >= kEdcRowCapacity)
        {
            LogDebug("[OutfitRegistry] reject: flowIndex=%u out of EDC capacity "
                "(max 0x3FF). (key=%s)\n",
                static_cast<unsigned>(def.flowIndex),
                def.key ? def.key : "(unkeyed)");
            return false;
        }

        std::lock_guard<std::mutex> lock(g_Mutex);

        BuildReservations_NoLock();
        const std::uint64_t keyHash = HashOutfitKey(def.key);


        for (auto& e : g_Entries)
        {
            if (!e.used) continue;

            if (e.developId == def.developId)
            {
                if (def.flowIndex != 0 && e.flowIndex != def.flowIndex)
                    e.flowIndex = def.flowIndex;
                else if (e.flowIndex == 0)
                    e.flowIndex = ResolvePersistedFlowIndex(def.key);
#ifdef _DEBUG
                LogDebug("[OutfitRegistry] re-registration of same outfit "
                    "developId=%u flowIndex=%u partsType=0x%02X - "
                    "returning existing entry (idempotent)\n",
                    static_cast<unsigned>(e.developId),
                    static_cast<unsigned>(e.flowIndex),
                    static_cast<unsigned>(e.partsType));
#endif
                if (outAllocatedPartsType) *outAllocatedPartsType = e.partsType;
                return true;
            }

            if (e.flowIndex != 0 && e.flowIndex == def.flowIndex)
            {
                LogDebug("[OutfitRegistry] flowIndex %u transferred to developId=%u "
                    "(previous claim by developId=%u was released for "
                    "paging)\n",
                    static_cast<unsigned>(def.flowIndex),
                    static_cast<unsigned>(def.developId),
                    static_cast<unsigned>(e.developId));
                e.flowIndex = 0;
            }
        }

        const std::uint8_t variantSlots =
            (maxVariants > outfit::kMaxVariantsPerOutfit)
                ? static_cast<std::uint8_t>(outfit::kMaxVariantsPerOutfit)
                : maxVariants;

        OutfitEntry* slot = nullptr;
        for (auto& e : g_Entries)
        {
            if (!e.used) { slot = &e; break; }
        }
        if (!slot)
        {
            g_Entries.emplace_back();
            slot = &g_Entries.back();
        }

        {
            static const OutfitEntry kBlankEntry{};
            *slot = kBlankEntry;
        }
        slot->used         = true;
        slot->bound        = false;
        slot->developId    = def.developId;
        slot->flowIndex    = (def.flowIndex != 0)
                                 ? def.flowIndex
                                 : ResolvePersistedFlowIndex(def.key);
        slot->partsType    = 0;
        slot->selectorCode = 0;
        slot->keyHash      = keyHash;
        if (def.key)
        {
            std::size_t n = 0;
            for (; def.key[n] && n < sizeof(slot->key) - 1; ++n)
                slot->key[n] = def.key[n];
            slot->key[n] = 0;
            if (def.key[n] != 0)
            {
                Log("[OutfitRegistry] outfit key '%s...' is longer than %zu "
                    "characters - REFUSED. A truncated key persists short but is "
                    "read back long, so the outfit takes a different partsType on "
                    "the next launch and whatever the player was wearing resolves "
                    "to the wrong suit\n",
                    slot->key, sizeof(slot->key) - 1);
                slot->used = false;
                return false;
            }
        }
        slot->partsTypeHint    = def.partsTypeHint;
        slot->selectorCodeHint = def.selectorCodeHint;
        for (std::size_t i = 0; i < outfit::kMaxVariantsPerOutfit; ++i)
        {
            slot->variantSelectorHints[i] = def.variantSelectorHints[i];
            slot->variantSelectorCodes[i] = 0xFF;
        }

        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            slot->perPlayerType[pt] = def.perPlayerType[pt];

        slot->variantCount = variantSlots;

        std::uint8_t defVar = 0;
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            const auto& b = slot->perPlayerType[pt];
            if (b.used && b.defaultVariant > defVar)
                defVar = b.defaultVariant;
        }
        if (variantSlots == 0 || defVar >= variantSlots)
            defVar = 0;
        slot->defaultVariant = defVar;

        bool hasPersistedBytes =
               IsCustomPartsType(slot->partsTypeHint)
            || IsCustomSelector(slot->selectorCodeHint);
        for (std::uint8_t vi = 1;
             !hasPersistedBytes && vi < outfit::kMaxVariantsPerOutfit; ++vi)
            hasPersistedBytes = IsCustomSelector(slot->variantSelectorHints[vi]);

        const bool demandPinned =
               (g_PendingSupplyDropDevelopId != 0
                && g_PendingSupplyDropDevelopId == slot->developId)
            || (g_CrateDeliveredDevelopId != 0
                && g_CrateDeliveredDevelopId == slot->developId)
            || g_AppliedThisSession.count(slot->developId) != 0
            || g_OrderedThisSession.count(slot->developId) != 0;

        if (hasPersistedBytes || demandPinned)
        {
            if (!BindOutfit_NoLock(*slot, false, "register"))
                LogDebug("[OutfitRegistry] '%s' has persisted live bytes but could "
                         "not take them back - registered UNBOUND, re-acquires on "
                         "first equip, order or menu stamp\n",
                    slot->key[0] ? slot->key : "(unkeyed)");
        }
#ifdef _DEBUG
        else
        {
            LogDebug("[OutfitRegistry] '%s' registered UNBOUND (lazy) - live bytes "
                "assigned on first equip, order, or menu stamp\n",
                slot->key[0] ? slot->key : "(unkeyed)");
        }
#endif

        if (outAllocatedPartsType) *outAllocatedPartsType = slot->partsType;

        if (const auto it = g_PendingSummaryDisplay.find(slot->developId);
            it != g_PendingSummaryDisplay.end())
        {
            if (it->second.nameHash != 0)
                slot->displaySummaryNameHash = it->second.nameHash;
            if (it->second.iconHash != 0)
                slot->displaySummaryIconHash = it->second.iconHash;
            g_PendingSummaryDisplay.erase(it);
#ifdef _DEBUG
            LogDebug("[OutfitRegistry] drained pending summary display for "
                "developId=%u (nameHash=0x%016llX iconHash=0x%016llX)\n",
                static_cast<unsigned>(slot->developId),
                static_cast<unsigned long long>(slot->displaySummaryNameHash),
                static_cast<unsigned long long>(slot->displaySummaryIconHash));
#endif
        }


        return true;
    }

    void ClearAllOutfits()
    {
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Entries.clear();
            for (auto& v : g_ActiveVariant)  v = 0;
            g_VanillaExts.clear();
            g_PendingSummaryDisplay.clear();
            g_PendingDevelopId            = 0;
            g_PendingHeadOptionEquipId    = 0;
            g_WornCustomHeadSlot          = 0;
            g_SupplyDropClickLatch        = false;
            g_PendingSupplyDropDevelopId  = 0;
            g_PendingSupplyDropVariantIdx = 0;
            g_CrateDeliveredDevelopId     = 0;
            g_CrateDeliveredVariantIdx    = 0;
            g_AppliedThisSession.clear();
            g_OrderedThisSession.clear();
            g_MenuStampSet.clear();
        }
        outfit::shadow::ResetAll("ClearAllOutfits");
        outfit::shadow::ResetArmTierCache();
    }

    bool TryGetOutfitByKey(const char* key, const OutfitEntry** outEntry)
    {
        if (!key || !key[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
        {
            if (!e.used) continue;
            if (std::strncmp(e.key, key, sizeof(e.key) - 1) != 0) continue;
            if (outEntry) *outEntry = &e;
            return true;
        }
        return false;
    }

    bool TryGetOutfitByPartsType(std::uint8_t partsType, const OutfitEntry** outEntry)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
        {
            if (e.used && e.bound && e.partsType == partsType)
            {
                e.lastBindTouch = GetTickCount64();
                if (outEntry) *outEntry = &e;
                return true;
            }
        }
        if (IsCustomPartsType(partsType))
        {
            for (auto& e : g_Entries)
            {
                if (!e.used || e.bound || e.partsTypeHint != partsType)
                    continue;
                if (BindOutfit_NoLock(e, false, "recognition", true)
                    && e.partsType == partsType)
                {
                    if (outEntry) *outEntry = &e;
                    return true;
                }
                Log("[OutfitRegistry] recognition bind failed for key=%s: "
                    "reserved partsType 0x%02X unavailable\n", e.key, partsType);
                break;
            }
        }
        return false;
    }

    bool TryGetOutfitBySelectorCode(std::uint8_t selectorCode, const OutfitEntry** outEntry)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
        {
            if (!e.used || !e.bound) continue;
            if (e.selectorCode == selectorCode)
            {
                e.lastBindTouch = GetTickCount64();
                if (outEntry) *outEntry = &e;
                return true;
            }
            for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
            {
                if (e.variantSelectorCodes[vi] == selectorCode)
                {
                    e.lastBindTouch = GetTickCount64();
                    if (outEntry) *outEntry = &e;
                    return true;
                }
            }
        }
        if (IsCustomSelector(selectorCode))
        {
            for (auto& e : g_Entries)
            {
                if (!e.used || e.bound) continue;
                bool hinted = e.selectorCodeHint == selectorCode;
                for (std::uint8_t vi = 1; !hinted && vi < e.variantCount; ++vi)
                    hinted = e.variantSelectorHints[vi] == selectorCode;
                if (!hinted) continue;
                if (BindOutfit_NoLock(e, false, "recognition", true))
                {
                    if (e.selectorCode == selectorCode)
                    {
                        if (outEntry) *outEntry = &e;
                        return true;
                    }
                    for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
                        if (e.variantSelectorCodes[vi] == selectorCode)
                        {
                            if (outEntry) *outEntry = &e;
                            return true;
                        }
                }
                break;
            }
        }
        return false;
    }

    bool TryGetOutfitByFlowIndex(std::uint16_t flowIndex, const OutfitEntry** outEntry)
    {
        if (flowIndex == 0)
            return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& e : g_Entries)
        {
            if (e.used && e.flowIndex == flowIndex)
            {
                if (outEntry) *outEntry = &e;
                return true;
            }
        }
        return false;
    }

    std::uint8_t GetSuitParamDonorLockFree(std::uint8_t selectorCode,
                                          std::uint8_t playerType)
    {
        if (playerType >= kPlayerTypeMax) return 0;
        return g_SuitParamDonor[selectorCode][playerType].load(
            std::memory_order_relaxed);
    }

    std::uint8_t GetActiveVariantLockFree(std::uint8_t partsType)
    {
        return g_ActiveVariant[partsType].load(std::memory_order_relaxed);
    }

    void GetAbilityLevelsForVariantLockFree(std::uint8_t selectorCode,
                                            std::uint8_t playerType,
                                            std::uint8_t variantIdx,
                                            std::uint8_t* outDefense,
                                            std::uint8_t* outRecovery)
    {
        GetAbilityLevelsLockFree(selectorCode, playerType,
                                 outDefense, outRecovery);
        if (playerType >= kPlayerTypeMax
            || variantIdx >= kAbilityVariantSlots)
            return;

        const std::uint8_t d =
            g_VarDefense[selectorCode][playerType][variantIdx].load(
                std::memory_order_relaxed);
        const std::uint8_t r =
            g_VarRegen[selectorCode][playerType][variantIdx].load(
                std::memory_order_relaxed);
        if (d != 0 && outDefense)  *outDefense  = static_cast<std::uint8_t>(d - 1);
        if (r != 0 && outRecovery) *outRecovery = static_cast<std::uint8_t>(r - 1);
    }

    bool TryGetVariantAbilities(std::uint8_t partsType, std::uint8_t playerType,
                                std::uint8_t variantIdx,
                                DeclaredAbilities* out)
    {
        if (!out || playerType >= kPlayerTypeMax) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
        {
            if (!e.used || !e.bound || e.partsType != partsType) continue;
            const OutfitPlayerTypeData* pd = e.GetPTData(playerType);
            const OutfitVariant* v = pd ? pd->Var(variantIdx) : nullptr;
            if (!v || !v->abilities.declared) return false;
            *out = v->abilities;
            return true;
        }
        return false;
    }

    void GetAbilityLevelsLockFree(std::uint8_t selectorCode,
                                  std::uint8_t playerType,
                                  std::uint8_t* outDefense,
                                  std::uint8_t* outRecovery)
    {
        if (playerType >= kPlayerTypeMax)
        {
            if (outDefense)  *outDefense  = 0;
            if (outRecovery) *outRecovery = 0;
            return;
        }
        if (outDefense)
            *outDefense = g_AbilityDefense[selectorCode][playerType].load(
                std::memory_order_relaxed);
        if (outRecovery)
            *outRecovery = g_AbilityRegen[selectorCode][playerType].load(
                std::memory_order_relaxed);
    }

    bool TryGetOutfitByVariantSelector(std::uint8_t selectorCode,
                                       const OutfitEntry** outEntry,
                                       std::uint8_t* outVariantIndex)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
        {
            if (!e.used || !e.bound) continue;

            if (e.selectorCode == selectorCode)
            {
                e.lastBindTouch = GetTickCount64();
                if (outEntry) *outEntry = &e;
                if (outVariantIndex) *outVariantIndex = 0;
                return true;
            }

            for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
            {
                if (e.variantSelectorCodes[vi] == selectorCode)
                {
                    e.lastBindTouch = GetTickCount64();
                    if (outEntry) *outEntry = &e;
                    if (outVariantIndex) *outVariantIndex = vi;
                    return true;
                }
            }
        }
        if (IsCustomSelector(selectorCode))
        {
            for (auto& e : g_Entries)
            {
                if (!e.used || e.bound) continue;
                bool hinted = e.selectorCodeHint == selectorCode;
                for (std::uint8_t vi = 1; !hinted && vi < e.variantCount; ++vi)
                    hinted = e.variantSelectorHints[vi] == selectorCode;
                if (!hinted) continue;
                if (BindOutfit_NoLock(e, false, "recognition", true))
                {
                    if (e.selectorCode == selectorCode)
                    {
                        if (outEntry) *outEntry = &e;
                        if (outVariantIndex) *outVariantIndex = 0;
                        return true;
                    }
                    for (std::uint8_t vi = 1; vi < e.variantCount; ++vi)
                        if (e.variantSelectorCodes[vi] == selectorCode)
                        {
                            if (outEntry) *outEntry = &e;
                            if (outVariantIndex) *outVariantIndex = vi;
                            return true;
                        }
                }
                break;
            }
        }
        return false;
    }

    bool BindOutfit(std::uint16_t developId, bool allowEvict, const char* reason)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        BuildReservations_NoLock();
        for (auto& e : g_Entries)
            if (e.used && e.developId == developId)
                return BindOutfit_NoLock(e, allowEvict, reason);
        return false;
    }

    bool UnbindOutfit(std::uint16_t developId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
            if (e.used && e.developId == developId)
            {
                UnbindOutfit_NoLock(e);
                return true;
            }
        return false;
    }

    bool ReleaseOutfitLiveBytesIfIdle(std::uint16_t developId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        std::vector<bool> pinned;
        bool scanAvailable = false;
        ComputePins_NoLock(pinned, scanAvailable, kPinCore);

        std::size_t idx = 0;
        for (auto& e : g_Entries)
        {
            if (e.used && e.developId == developId)
            {
                if (!e.bound) return false;
                if (scanAvailable && idx < pinned.size() && pinned[idx])
                    return false;
                UnbindOutfit_NoLock(e);
                return true;
            }
            ++idx;
        }
        return false;
    }

    bool IsOutfitBound(std::uint16_t developId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& e : g_Entries)
            if (e.used && e.developId == developId)
                return e.bound;
        return false;
    }

    bool CollectOutfitResidencyPins(
        std::unordered_set<std::uint32_t>& allDevelopIds,
        std::unordered_set<std::uint32_t>& livePinnedDevelopIds)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        std::vector<bool> pinned;
        bool scanAvailable = false;
        ComputePins_NoLock(pinned, scanAvailable, kPinAppliedOrdered);

        std::size_t idx = 0;
        for (const auto& e : g_Entries)
        {
            if (e.used && e.developId != 0)
            {
                allDevelopIds.insert(e.developId);
                if (!scanAvailable
                    || (idx < pinned.size() && pinned[idx]))
                    livePinnedDevelopIds.insert(e.developId);
            }
            ++idx;
        }
        return scanAvailable;
    }

    void NoteOutfitApplied(std::uint16_t developId)
    {
        if (developId == 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_AppliedThisSession.insert(developId);
    }

    void NoteOutfitOrdered(std::uint16_t developId)
    {
        if (developId == 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_OrderedThisSession.insert(developId);
    }

    void NoteOutfitMenuStamp(std::uint16_t developId)
    {
        if (developId == 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_MenuStampSet.insert(developId);
        g_MenuStampTick[developId] = GetTickCount64();
    }

    void ClearOutfitMenuStamps()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_MenuStampSet.clear();
        g_MenuStampTick.clear();
        g_RowBindBackoff.clear();
    }

    void NoteOutfitRowRefresh(std::uint16_t developId)
    {
        if (developId == 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_MenuStampSet.insert(developId);
        g_MenuStampTick[developId] = GetTickCount64();
    }

    bool BindOutfitForVisibleRow(std::uint16_t developId)
    {
        if (developId == 0) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        BuildReservations_NoLock();

        OutfitEntry* target = nullptr;
        for (auto& e : g_Entries)
            if (e.used && e.developId == developId)
            {
                target = &e;
                break;
            }
        if (!target)
            return false;

        const std::uint64_t now = GetTickCount64();
        if (target->bound)
        {
            g_MenuStampSet.insert(developId);
            g_MenuStampTick[developId] = now;
            return true;
        }

        auto bo = g_RowBindBackoff.find(developId);
        if (bo != g_RowBindBackoff.end() && now - bo->second < kRowBindRetryMs)
            return false;

        for (int attempt = 0; attempt < 64; ++attempt)
        {
            if (BindOutfit_NoLock(*target, false, "row-window"))
            {
                g_RowBindBackoff.erase(developId);
                g_MenuStampSet.insert(developId);
                g_MenuStampTick[developId] = GetTickCount64();
                return true;
            }
            if (!TryEvictStaleStamp_NoLock(target->key))
                break;
        }
        g_RowBindBackoff[developId] = now;
        return false;
    }

    std::uint64_t GetVariantDisplayNameHash(std::uint8_t partsType,
                                            std::uint8_t playerType,
                                            std::uint8_t variantIndex)
    {
        if (variantIndex >= outfit::kMaxVariantsPerOutfit) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& e : g_Entries)
        {
            if (e.used && e.partsType == partsType)
                return e.GetVariantDisplayNameHash(playerType, variantIndex);
        }
        return 0;
    }

    bool TryGetOutfitByDevelopId(std::uint16_t developId, const OutfitEntry** outEntry)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& e : g_Entries)
        {
            if (e.used && e.developId == developId)
            {
                if (outEntry) *outEntry = &e;
                return true;
            }
        }
        return false;
    }

    bool SetOutfitFlowIndexByDevelopId(std::uint16_t developId,
                                       std::uint16_t flowIndex)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
        {
            if (e.used && e.developId == developId)
            {
                e.flowIndex = flowIndex;
                return true;
            }
        }
        return false;
    }

    bool SetOutfitSummaryDisplay(std::uint16_t developId,
                                 std::uint64_t nameHash,
                                 std::uint64_t iconHash)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& e : g_Entries)
        {
            if (!e.used || e.developId != developId) continue;
            if (nameHash != 0) e.displaySummaryNameHash = nameHash;
            if (iconHash != 0) e.displaySummaryIconHash = iconHash;
#ifdef _DEBUG
            LogDebug("[OutfitRegistry] summary display set developId=%u "
                "nameHash=0x%016llX iconHash=0x%016llX\n",
                static_cast<unsigned>(developId),
                static_cast<unsigned long long>(e.displaySummaryNameHash),
                static_cast<unsigned long long>(e.displaySummaryIconHash));
#endif
            return true;
        }
        PendingSummaryDisplay& p = g_PendingSummaryDisplay[developId];
        if (nameHash != 0) p.nameHash = nameHash;
        if (iconHash != 0) p.iconHash = iconHash;
#ifdef _DEBUG
        LogDebug("[OutfitRegistry] summary display stashed (pending) developId=%u "
            "nameHash=0x%016llX iconHash=0x%016llX\n",
            static_cast<unsigned>(developId),
            static_cast<unsigned long long>(p.nameHash),
            static_cast<unsigned long long>(p.iconHash));
#endif
        return false;
    }

    bool TryGetOutfitByCamoVirtualId(std::uint8_t virtualId,
                                     const OutfitEntry** outEntry,
                                     std::uint8_t* outPlayerType)
    {
        if (virtualId < kCamoVirtualIdStart || virtualId > kCamoVirtualIdEnd)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& e : g_Entries)
        {
            if (!e.used) continue;
            for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
            {
                const auto& b = e.perPlayerType[pt];
                if (b.used && b.hasCamoBonusValues
                    && b.camoBonusType == virtualId)
                {
                    if (outEntry)      *outEntry      = &e;
                    if (outPlayerType) *outPlayerType = pt;
                    return true;
                }
            }
        }
        return false;
    }

    bool TryGetOutfitByFlowIndexForPlayerType(
            std::uint16_t flowIndex, std::uint8_t playerType,
            const OutfitEntry** outEntry)
    {
        if (flowIndex == 0)
            return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& e : g_Entries)
        {
            if (e.used && e.flowIndex == flowIndex
                && e.IsPlayerTypeSupported(playerType))
            {
                if (outEntry) *outEntry = &e;
                return true;
            }
        }
        return false;
    }

    bool TryGetOutfitByDevelopIdForPlayerType(
            std::uint16_t developId, std::uint8_t playerType,
            const OutfitEntry** outEntry)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& e : g_Entries)
        {
            if (e.used && e.developId == developId
                && e.IsPlayerTypeSupported(playerType))
            {
                if (outEntry) *outEntry = &e;
                return true;
            }
        }
        return false;
    }

    std::size_t GetAllOutfits(const OutfitEntry** outEntries, std::size_t maxEntries)
    {
        if (!outEntries || maxEntries == 0) return 0;

        std::lock_guard<std::mutex> lock(g_Mutex);
        std::size_t count = 0;
        for (const auto& e : g_Entries)
        {
            if (count >= maxEntries) break;
            if (e.used) outEntries[count++] = &e;
        }
        return count;
    }

    namespace
    {
        std::atomic<bool> g_BootRestoreScrubArmed{ true };
    }

    bool BootRestoreScrubActive()
    {
        return g_BootRestoreScrubArmed.load(std::memory_order_relaxed);
    }

    void EndBootRestoreScrub(const char* reason)
    {
        bool expected = true;
        if (g_BootRestoreScrubArmed.compare_exchange_strong(expected, false))
            LogDebug("[OutfitRegistry] BOOT-SCRUB disarmed (%s) - custom outfit "
                "restore/resolve is live again for the rest of the session\n",
                reason ? reason : "unspecified");
    }

    std::uint8_t ReadLivePartsType()
    {
        auto* state = GetQuarkLiveState();
        return state ? state[0xF8] : static_cast<std::uint8_t>(0xFF);
    }

    std::uint8_t ReadLiveSelectorCode()
    {
        auto* state = GetQuarkLiveState();
        return state ? state[0xF9] : static_cast<std::uint8_t>(0xFF);
    }

    std::uint8_t ReadLivePlayerType()
    {
        auto* state = GetQuarkLiveState();
        return state ? state[0xFB] : static_cast<std::uint8_t>(0xFF);
    }

    static std::atomic<std::uint16_t> g_MotionOutfitHint{ 0 };

    void SetMotionOutfitHint(std::uint8_t partsType, std::uint8_t playerType)
    {
        g_MotionOutfitHint.store(
            static_cast<std::uint16_t>(partsType
                | (static_cast<std::uint16_t>(playerType) << 8)),
            std::memory_order_relaxed);
    }

    void ClearMotionOutfitHint()
    {
        g_MotionOutfitHint.store(0, std::memory_order_relaxed);
    }

    std::uint8_t GetMotionOutfitHintPartsType()
    {
        return static_cast<std::uint8_t>(
            g_MotionOutfitHint.load(std::memory_order_relaxed) & 0xFF);
    }

    std::uint8_t GetMotionOutfitHintPlayerType()
    {
        return static_cast<std::uint8_t>(
            g_MotionOutfitHint.load(std::memory_order_relaxed) >> 8);
    }

    namespace
    {
        constexpr std::size_t kMaxMotionMtarOverrideHashes = 512;

        struct MotionMtarOverrideHash
        {
            std::uint64_t hash = 0;
            int           slot = -1;
        };
        MotionMtarOverrideHash   g_MotionMtarOverrideHashes[kMaxMotionMtarOverrideHashes] = {};
        std::atomic<std::size_t> g_MotionMtarOverrideHashCount{ 0 };
    }

    void RegisterMotionMtarOverrideHash(std::uint64_t pathHash, int slot)
    {
        if (pathHash == 0) return;

        const std::size_t count =
            g_MotionMtarOverrideHashCount.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i)
            if (g_MotionMtarOverrideHashes[i].hash == pathHash)
                return;

        if (count >= kMaxMotionMtarOverrideHashes)
        {
            Log("[OutfitRegistry] WARN: motionMtars override table full (%zu "
                "entries) - hash %016llX (slot %d) not registered; that archive "
                "falls back to vanilla if the additional-motion block cannot "
                "resolve it\n",
                kMaxMotionMtarOverrideHashes, pathHash, slot);
            return;
        }

        g_MotionMtarOverrideHashes[count].hash = pathHash;
        g_MotionMtarOverrideHashes[count].slot = slot;
        g_MotionMtarOverrideHashCount.store(count + 1, std::memory_order_release);
    }

    bool IsMotionMtarOverrideHash(std::uint64_t pathHash, int* outSlot)
    {
        if (pathHash == 0) return false;

        const std::size_t count =
            g_MotionMtarOverrideHashCount.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (g_MotionMtarOverrideHashes[i].hash == pathHash)
            {
                if (outSlot) *outSlot = g_MotionMtarOverrideHashes[i].slot;
                return true;
            }
        }
        return false;
    }

    bool AnyMotionMtarOverridesRegistered()
    {
        return g_MotionMtarOverrideHashCount.load(std::memory_order_acquire) != 0;
    }

    bool WriteLivePlayerOutfit(std::uint8_t partsType,
                               std::uint8_t selectorCode,
                               std::uint8_t playerType,
                               OutfitWriteSource source)
    {
        if (partsType >= kCustomPartsTypeStart
            && partsType <= kCustomPartsTypeEnd)
            SetMotionOutfitHint(partsType, playerType);
        else
            ClearMotionOutfitHint();

        auto* state = GetQuarkLiveState();
        if (!state) return false;

        bool changed = true;
        __try
        {
            changed = (state[0xF8] != partsType)
                   || (state[0xF9] != selectorCode)
                   || (state[0xFB] != playerType);
            state[0xF8] = partsType;
            state[0xF9] = selectorCode;
            state[0xFB] = playerType;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        if (changed)
        {
            static std::atomic<std::uint8_t> s_lastSource{ 0 };
            const std::uint8_t cur  = static_cast<std::uint8_t>(source);
            const std::uint8_t prev =
                s_lastSource.exchange(cur, std::memory_order_relaxed);

            if (cur != 0 && prev != 0 && cur < prev)
            {
                static std::atomic<int> s_inv{ 0 };
                if (s_inv.fetch_add(1, std::memory_order_relaxed) < 12)
                    Log("[OutfitRegistry] precedence inversion: a lower-priority "
                        "writer (source %u) changed the live outfit to "
                        "0x%02X/0x%02X pt=%u right after a higher-priority "
                        "writer (source %u) set it - the later value wins, which "
                        "may not be the intended suit\n",
                        static_cast<unsigned>(cur),
                        static_cast<unsigned>(partsType),
                        static_cast<unsigned>(selectorCode),
                        static_cast<unsigned>(playerType),
                        static_cast<unsigned>(prev));
            }

            RequestAdditionalMotionReresolve();
        }
        return true;
    }


    void RememberPlayerTypeOutfit(std::uint8_t playerType,
                                  std::uint8_t partsType, std::uint8_t selector)
    {
        if (playerType >= kPlayerTypeMax) return;
        g_LastOutfitPerPT[playerType] = { partsType, selector, true };
    }

    bool GetRememberedPlayerTypeOutfit(std::uint8_t playerType,
                                       std::uint8_t* outPartsType,
                                       std::uint8_t* outSelector)
    {
        if (playerType >= kPlayerTypeMax) return false;

        if (!g_LastOutfitPerPT[playerType].valid)
        {
            std::uint8_t defParts = 0;
            std::uint8_t defSel   = 0;
            if (!uniquedefaultoutfit::TryGetBindingFor(playerType,
                                                       &defParts, &defSel))
                return false;

            static bool s_logged[kPlayerTypeMax] = {};
            if (!s_logged[playerType])
            {
                s_logged[playerType] = true;
                Log("[OutfitRegistry] player type %u had no outfit yet this "
                    "session, so its default-outfit row (partsType=0x%02X) is "
                    "applied - without it the character keeps whatever suit the "
                    "previous one wore, which has no parts for this body\n",
                    static_cast<unsigned>(playerType),
                    static_cast<unsigned>(defParts));
            }

            if (outPartsType) *outPartsType = defParts;
            if (outSelector)  *outSelector  = defSel;
            return true;
        }
        if (outPartsType) *outPartsType = g_LastOutfitPerPT[playerType].partsType;
        if (outSelector)  *outSelector  = g_LastOutfitPerPT[playerType].selector;
        return true;
    }

    bool WriteLiveHeadSlot(std::uint8_t headSlot)
    {
        auto* state = GetQuarkLiveState();
        if (!state) return false;
        __try
        {
            state[0xFA] = headSlot;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool WriteLiveWornHeadCategory(std::uint16_t category)
    {
        auto* state = GetQuarkLiveState();
        if (!state) return false;
        __try
        {
            *reinterpret_cast<std::uint16_t*>(state + 0xFE) = category;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::uint16_t ReadLiveWornHeadCategory()
    {
        auto* state = GetQuarkLiveState();
        if (!state) return 0xFFFF;
        __try
        {
            return *reinterpret_cast<std::uint16_t*>(state + 0xFE);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0xFFFF;
        }
    }

    std::uint8_t ReadLiveHeadSlot()
    {
        auto* state = GetQuarkLiveState();
        if (!state) return 0xFF;
        __try
        {
            return state[0xFA];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0xFF;
        }
    }


    void SetActiveVariant(std::uint8_t partsType, std::uint8_t variantIndex)
    {
        std::uint8_t clamped = variantIndex;

        std::lock_guard<std::mutex> lock(g_Mutex);
        if (IsCustomPartsType(partsType))
        {
            for (const auto& e : g_Entries)
            {
                if (e.used && e.partsType == partsType)
                {
                    if (e.variantCount == 0) clamped = 0;
                    else if (clamped >= e.variantCount)
                        clamped = static_cast<std::uint8_t>(e.variantCount - 1);
                    break;
                }
            }
        }
        else
        {
            const VanillaSuitExtension* x = FindVanillaExt_NoLock(partsType);
            std::uint8_t maxCount = 0;
            if (x)
                for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
                    if (x->variantCount[pt] > maxCount)
                        maxCount = x->variantCount[pt];
            if (maxCount == 0) clamped = 0;
            else if (clamped >= maxCount)
                clamped = static_cast<std::uint8_t>(maxCount - 1);
        }
        g_ActiveVariant[partsType] = clamped;
    }

    std::uint8_t GetActiveVariant(std::uint8_t partsType)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_ActiveVariant[partsType];
    }

    void ClearActiveVariant(std::uint8_t partsType)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_ActiveVariant[partsType] = 0;
    }

    static std::atomic<bool> g_VanillaExtAbilitiesAny{ false };

    static void PublishVanillaExtSuitParam_NoLock(const VanillaSuitExtension* x);

    bool ExtendVanillaSuitVariants(std::uint8_t vanillaPartsType,
                                   std::uint8_t playerType,
                                   std::uint8_t sourceCamo,
                                   const VanillaSuitVariantAsset* variants,
                                   std::uint8_t count)
    {
        if (IsCustomPartsType(vanillaPartsType) || vanillaPartsType == 0xFF)
            return false;
        if (playerType >= kPlayerTypeMax || !variants || count == 0)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        BuildReservations_NoLock();
        VanillaSuitExtension* x = FindOrCreateVanillaExt_NoLock(vanillaPartsType);

        VanillaSuitVariantAsset incoming[kMaxVanillaExtVariants] = {};
        std::uint8_t incomingCount = 0;
        for (std::uint8_t i = 0;
             i < count && incomingCount < kMaxVanillaExtVariants; ++i)
        {
            if (variants[i].partsPathCode64 == 0
                && variants[i].fpkPathCode64 == 0)
                continue;
            incoming[incomingCount] = variants[i];
            ++incomingCount;
        }
        if (incomingCount == 0) return false;

        int base = -1;
        std::uint8_t blockSize = 0;
        for (std::uint8_t i = 0; i < x->variantSelectorCount; ++i)
        {
            if (x->variantSourceCamo[i] == sourceCamo)
            {
                if (base < 0) base = static_cast<int>(i);
                ++blockSize;
            }
        }

        const bool newBlock = (base < 0);
        if (newBlock)
        {
            std::uint8_t room =
                static_cast<std::uint8_t>(kMaxVanillaExtVariants
                                          - x->variantSelectorCount);
            if (room == 0) return false;
            std::uint8_t take =
                (incomingCount < room) ? incomingCount : room;

            char keyBuf[24];
            std::snprintf(keyBuf, sizeof(keyBuf), "__vext:0x%02X",
                          vanillaPartsType);
            const std::uint64_t keyHash = HashOutfitKey(keyBuf);

            std::uint8_t persisted[V_FrameWorkState::kPersistedVariantSelectorSlots] = {};
            V_FrameWorkState::GetPersistedOutfitVariantSelectors(
                keyBuf, persisted, V_FrameWorkState::kPersistedVariantSelectorSlots);
            base = static_cast<int>(x->variantSelectorCount);
            for (std::uint8_t j = 0; j < take; ++j)
            {
                const std::uint8_t slot =
                    static_cast<std::uint8_t>(base + j);
                std::uint8_t sel =
                    (slot < V_FrameWorkState::kPersistedVariantSelectorSlots)
                        ? persisted[slot] : std::uint8_t{0};
                if (!IsAllocatableSelector(sel) || IsSelectorTaken_NoLock(sel)
                    || IsVirtualIdTaken_NoLock(sel)
                    || IsSelectorReservedForOther_NoLock(sel, keyHash))
                    sel = AllocateSelector_NoLock(sel, keyHash);
                if (sel == 0xFF || sel == 0) break;
                x->variantSelectorCodes[slot] = sel;
                x->variantSourceCamo[slot]    = sourceCamo;
                ++blockSize;
                x->variantSelectorCount =
                    static_cast<std::uint8_t>(slot + 1);
            }
            x->variantSelectorCount =
                static_cast<std::uint8_t>(base + blockSize);
            if (blockSize == 0) return false;

            V_FrameWorkState::SetPersistedOutfitVariantSelectors(
                keyBuf, x->variantSelectorCodes, x->variantSelectorCount);
        }
        else if (incomingCount > blockSize)
        {
            const bool isTailBlock =
                static_cast<std::uint8_t>(base + blockSize)
                    == x->variantSelectorCount;
            if (isTailBlock)
            {
                std::uint8_t room =
                    static_cast<std::uint8_t>(kMaxVanillaExtVariants
                                              - x->variantSelectorCount);
                std::uint8_t need =
                    static_cast<std::uint8_t>(incomingCount - blockSize);
                if (need > room) need = room;
                if (need > 0)
                {
                    char keyBuf[24];
                    std::snprintf(keyBuf, sizeof(keyBuf), "__vext:0x%02X",
                                  vanillaPartsType);
                    const std::uint64_t keyHash = HashOutfitKey(keyBuf);
                    std::uint8_t persisted[V_FrameWorkState::kPersistedVariantSelectorSlots] = {};
                    V_FrameWorkState::GetPersistedOutfitVariantSelectors(
                        keyBuf, persisted,
                        V_FrameWorkState::kPersistedVariantSelectorSlots);
                    for (std::uint8_t g = 0; g < need; ++g)
                    {
                        const std::uint8_t slot = x->variantSelectorCount;
                        std::uint8_t sel =
                            (slot < V_FrameWorkState::kPersistedVariantSelectorSlots)
                                ? persisted[slot] : std::uint8_t{0};
                        if (!IsAllocatableSelector(sel)
                            || IsSelectorTaken_NoLock(sel)
                            || IsVirtualIdTaken_NoLock(sel)
                            || IsSelectorReservedForOther_NoLock(sel, keyHash))
                            sel = AllocateSelector_NoLock(sel, keyHash);
                        if (sel == 0xFF || sel == 0) break;
                        x->variantSelectorCodes[slot] = sel;
                        x->variantSourceCamo[slot]    = sourceCamo;
                        x->variantSelectorCount =
                            static_cast<std::uint8_t>(slot + 1);
                        ++blockSize;
                    }
                    V_FrameWorkState::SetPersistedOutfitVariantSelectors(
                        keyBuf, x->variantSelectorCodes,
                        x->variantSelectorCount);
                }
            }
            else
            {
                LogDebug("[OutfitRegistry] vext partsType=0x%02X sourceCamo=0x%02X "
                         "band (base=%d size=%u) is not the tail - cannot grow for "
                         "pt=%u (%u variants); %u extra dropped\n",
                    static_cast<unsigned>(vanillaPartsType),
                    static_cast<unsigned>(sourceCamo),
                    base, static_cast<unsigned>(blockSize),
                    static_cast<unsigned>(playerType),
                    static_cast<unsigned>(incomingCount),
                    static_cast<unsigned>(incomingCount - blockSize));
            }
        }

        std::uint8_t filled = 0;
        bool anyVariantAbilities = false;
        for (std::uint8_t j = 0; j < blockSize && j < incomingCount; ++j)
        {
            const std::uint8_t slot = static_cast<std::uint8_t>(base + j);
            x->variants[playerType][slot] = incoming[j];
            x->variants[playerType][slot].used = true;
            anyVariantAbilities =
                anyVariantAbilities || incoming[j].abilities.declared;
            ++filled;
        }
        if (filled == 0) return false;
        if (anyVariantAbilities)
            g_VanillaExtAbilitiesAny.store(true, std::memory_order_relaxed);

        x->variantCount[playerType] =
            static_cast<std::uint8_t>(x->variantSelectorCount + 1);
        PublishVanillaExtSuitParam_NoLock(x);
        return true;
    }

    std::uint8_t VanillaExtVariantCount(std::uint8_t vanillaPartsType,
                                        std::uint8_t playerType)
    {
        if (playerType >= kPlayerTypeMax) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        return x ? x->variantCount[playerType] : std::uint8_t{0};
    }

    std::uint8_t VanillaExtVariantSlotCount(std::uint8_t vanillaPartsType)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        return x ? x->variantSelectorCount : std::uint8_t{0};
    }

    const VanillaSuitVariantAsset* VanillaExtGetVariant(
        std::uint8_t vanillaPartsType, std::uint8_t playerType,
        std::uint8_t variantIdx)
    {
        if (playerType >= kPlayerTypeMax || variantIdx == 0) return nullptr;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return nullptr;
        const std::uint8_t slot = static_cast<std::uint8_t>(variantIdx - 1);
        if (slot >= kMaxVanillaExtVariants) return nullptr;
        const auto& v = x->variants[playerType][slot];
        return v.used ? &v : nullptr;
    }

    bool VanillaExtGetVariantAbilities(std::uint8_t vanillaPartsType,
                                       std::uint8_t playerType,
                                       std::uint8_t variantIdx,
                                       DeclaredAbilities* out)
    {
        if (!out || playerType >= kPlayerTypeMax || variantIdx == 0)
            return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        const std::uint8_t slot = static_cast<std::uint8_t>(variantIdx - 1);
        if (slot >= kMaxVanillaExtVariants) return false;
        const auto& v = x->variants[playerType][slot];
        if (!v.used || !v.abilities.declared) return false;
        *out = v.abilities;
        return true;
    }

    static std::uint8_t ResolveVextDonor_NoLock(const VanillaSuitExtension& x,
                                                std::uint8_t playerType)
    {
        const std::uint8_t partner =
            (playerType == kPlayerType_Avatar) ? kPlayerType_Snake
          : (playerType == kPlayerType_Snake)  ? kPlayerType_Avatar
                                               : std::uint8_t{0xFF};
        if (partner != 0xFF && x.variantCount[partner] > 0) return partner;
        for (std::uint8_t sp = 0; sp < kPlayerTypeMax; ++sp)
            if (sp != playerType && x.variantCount[sp] > 0) return sp;
        return 0xFF;
    }

    const VanillaSuitVariantAsset* VanillaExtGetVariantBridged(
        std::uint8_t vanillaPartsType, std::uint8_t playerType,
        std::uint8_t variantIdx)
    {
        if (variantIdx == 0 || playerType >= kPlayerTypeMax) return nullptr;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return nullptr;
        const std::uint8_t slot = static_cast<std::uint8_t>(variantIdx - 1);
        if (slot >= kMaxVanillaExtVariants) return nullptr;
        auto pick = [&](std::uint8_t pt, std::uint8_t s)
            -> const VanillaSuitVariantAsset*
        {
            if (pt >= kPlayerTypeMax || s >= kMaxVanillaExtVariants)
                return nullptr;
            const auto& v = x->variants[pt][s];
            return v.used ? &v : nullptr;
        };
        auto nearest = [&](std::uint8_t pt)
            -> const VanillaSuitVariantAsset*
        {
            if (const auto* v = pick(pt, slot)) return v;
            for (std::uint8_t s = slot; s > 0; --s)
                if (const auto* v = pick(pt, static_cast<std::uint8_t>(s - 1)))
                    return v;
            for (std::uint8_t s = static_cast<std::uint8_t>(slot + 1);
                 s < kMaxVanillaExtVariants; ++s)
                if (const auto* v = pick(pt, s)) return v;
            return nullptr;
        };
        if (x->variantCount[playerType] > 0)
            if (const auto* v = nearest(playerType))
                return v;
        const std::uint8_t donor = ResolveVextDonor_NoLock(*x, playerType);
        if (donor == 0xFF) return nullptr;
        return nearest(donor);
    }

    std::uint8_t VanillaExtCollectSelectorSeeds(std::uint8_t* outSelectors,
                                                std::uint8_t* outSourceCamos,
                                                std::uint8_t maxCount)
    {
        if (!outSelectors || !outSourceCamos || maxCount == 0) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        std::uint8_t n = 0;
        for (const auto& x : g_VanillaExts)
        {
            if (!x.used) continue;
            for (std::uint8_t i = 0;
                 i < x.variantSelectorCount && n < maxCount; ++i)
            {
                const std::uint8_t sel = x.variantSelectorCodes[i];
                const std::uint8_t src = x.variantSourceCamo[i];
                if (sel < kCustomSelectorStart || sel > kCustomSelectorEnd)
                    continue;
                if (src == 0xFF) continue;
                outSelectors[n]   = sel;
                outSourceCamos[n] = src;
                ++n;
            }
        }
        return n;
    }

    std::uint8_t VanillaExtResolveVariantDonor(std::uint8_t vanillaPartsType,
                                               std::uint8_t playerType)
    {
        if (playerType >= kPlayerTypeMax) return 0xFF;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return 0xFF;
        if (x->variantCount[playerType] > 0) return playerType;
        return ResolveVextDonor_NoLock(*x, playerType);
    }

    std::uint8_t VanillaExtGetVariantSelector(std::uint8_t vanillaPartsType,
                                              std::uint8_t variantIdx)
    {
        if (variantIdx == 0) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return 0;
        const std::uint8_t i = static_cast<std::uint8_t>(variantIdx - 1);
        if (i >= x->variantSelectorCount) return 0;
        return x->variantSelectorCodes[i];
    }

    std::uint8_t VanillaExtGetVariantSourceCamo(std::uint8_t vanillaPartsType,
                                                std::uint8_t variantIdx)
    {
        if (variantIdx == 0) return 0xFF;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return 0xFF;
        const std::uint8_t i = static_cast<std::uint8_t>(variantIdx - 1);
        if (i >= x->variantSelectorCount) return 0xFF;
        return x->variantSourceCamo[i];
    }

    void ResetAllVanillaExtVariants(std::uint8_t exceptPartsType)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& x : g_VanillaExts)
        {
            if (!x.used) continue;
            if (x.vanillaPartsType == exceptPartsType) continue;
            g_ActiveVariant[x.vanillaPartsType] = 0;
        }
    }

    bool TryGetVanillaExtByVariantSelector(std::uint8_t selector,
                                           std::uint8_t* outPartsType,
                                           std::uint8_t* outVariantIdx)
    {
        if (!IsCustomSelector(selector)) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& x : g_VanillaExts)
        {
            if (!x.used) continue;
            for (std::uint8_t i = 0; i < x.variantSelectorCount; ++i)
            {
                if (x.variantSelectorCodes[i] == selector)
                {
                    if (outPartsType)  *outPartsType  = x.vanillaPartsType;
                    if (outVariantIdx) *outVariantIdx =
                        static_cast<std::uint8_t>(i + 1);
                    return true;
                }
            }
        }
        return false;
    }

    bool ExtendVanillaSuitHeadOptions(std::uint8_t vanillaPartsType,
                                      std::uint8_t playerType,
                                      std::uint8_t sourceCamo,
                                      const std::uint16_t* equipIds,
                                      std::uint8_t idCount,
                                      const std::uint64_t* pendingHashes,
                                      std::uint8_t pendingCount)
    {
        if (IsCustomPartsType(vanillaPartsType) || vanillaPartsType == 0xFF)
            return false;
        if (playerType >= kPlayerTypeMax)
            return false;
        if (idCount == 0 && pendingCount == 0)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        VanillaSuitExtension* x = FindOrCreateVanillaExt_NoLock(vanillaPartsType);

        auto& b = x->perPlayerType[playerType];
        b.declared = true;
        for (std::uint8_t i = 0; equipIds && i < idCount; ++i)
        {
            const std::uint16_t id = equipIds[i];
            if (id == 0) continue;
            bool dup = false;
            for (std::uint8_t j = 0; j < b.headOptionCount; ++j)
                if (b.headOptionEquipIds[j] == id
                    && b.headOptionSourceCamo[j] == sourceCamo)
                { dup = true; break; }
            if (!dup && b.headOptionCount < kMaxHeadOptionsPerOutfit)
            {
                b.headOptionSourceCamo[b.headOptionCount] = sourceCamo;
                b.headOptionEquipIds[b.headOptionCount++] = id;
            }
        }
        for (std::uint8_t i = 0; pendingHashes && i < pendingCount; ++i)
        {
            const std::uint64_t h = pendingHashes[i];
            if (h == 0) continue;
            bool dup = false;
            for (std::uint8_t j = 0; j < b.pendingHeadCount; ++j)
                if (b.pendingHeadNameHashes[j] == h
                    && b.pendingHeadSourceCamo[j] == sourceCamo)
                { dup = true; break; }
            if (!dup && b.pendingHeadCount < kMaxHeadOptionsPerOutfit)
            {
                b.pendingHeadSourceCamo[b.pendingHeadCount] = sourceCamo;
                b.pendingHeadNameHashes[b.pendingHeadCount++] = h;
            }
        }
        return true;
    }

    bool ExtendVanillaSuitVoice(std::uint8_t vanillaPartsType,
                                std::uint8_t playerType,
                                std::uint8_t sourceCamo,
                                std::uint64_t voiceFpk)
    {
        if (IsCustomPartsType(vanillaPartsType) || vanillaPartsType == 0xFF)
            return false;
        if (playerType >= kPlayerTypeMax)
            return false;
        if (voiceFpk <= kSubAssetUseVanilla)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        VanillaSuitExtension* x = FindOrCreateVanillaExt_NoLock(vanillaPartsType);
        x->suitVoiceFpk[playerType]  = voiceFpk;
        x->suitVoiceCamo[playerType] = sourceCamo;
        return true;
    }

    std::uint64_t VanillaExtGetSuitVoiceFpk(std::uint8_t vanillaPartsType,
                                            std::uint8_t playerType,
                                            std::uint8_t wornCamo)
    {
        if (playerType >= kPlayerTypeMax) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return 0;

        std::uint8_t src = playerType;
        if (x->suitVoiceFpk[src] <= kSubAssetUseVanilla)
        {
            const std::uint8_t partner =
                (playerType == kPlayerType_Avatar) ? kPlayerType_Snake
              : (playerType == kPlayerType_Snake)  ? kPlayerType_Avatar
                                                   : std::uint8_t{0xFF};
            src = 0xFF;
            if (partner != 0xFF
                && x->suitVoiceFpk[partner] > kSubAssetUseVanilla)
            {
                src = partner;
            }
            else
            {
                for (std::uint8_t sp = 0; sp < kPlayerTypeMax; ++sp)
                    if (sp != playerType
                        && x->suitVoiceFpk[sp] > kSubAssetUseVanilla)
                    { src = sp; break; }
            }
            if (src == 0xFF) return 0;
        }

        const std::uint8_t scope = x->suitVoiceCamo[src];
        if (scope != 0xFF && wornCamo != scope)
        {
            bool match = false;
            for (std::uint8_t s = 0; s < x->variantSelectorCount
                 && s < kMaxVanillaExtVariants; ++s)
            {
                if (x->variantSelectorCodes[s] == wornCamo)
                {
                    match = (x->variantSourceCamo[s] == scope);
                    break;
                }
            }
            if (!match) return 0;
        }
        return x->suitVoiceFpk[src];
    }

    static bool HeadCamoMatches_NoLock(std::uint8_t storedCamo,
                                       std::uint8_t wornCamo)
    {
        if (storedCamo == kHeadOptionAnyCamo) return true;
        if (wornCamo   == kHeadOptionAnyCamo) return true;
        return storedCamo == wornCamo;
    }

    bool ExtendVanillaSuitArmHead(std::uint8_t vanillaPartsType,
                                  std::uint8_t playerType,
                                  std::uint8_t sourceCamo,
                                  bool hasArm, bool armVal,
                                  bool hasHead, bool headVal)
    {
        if (IsCustomPartsType(vanillaPartsType) || vanillaPartsType == 0xFF)
            return false;
        if (playerType >= kPlayerTypeMax)
            return false;
        if (!hasArm && !hasHead)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        VanillaSuitExtension* x = FindOrCreateVanillaExt_NoLock(vanillaPartsType);
        if (hasArm)
        {
            x->suitArmOverride[playerType] = true;
            x->suitArmEnable[playerType]   = armVal;
            x->suitArmCamo[playerType]     = sourceCamo;
        }
        if (hasHead)
        {
            x->suitHeadOverride[playerType] = true;
            x->suitHeadEnable[playerType]   = headVal;
            x->suitHeadCamo[playerType]     = sourceCamo;
        }
        return true;
    }

    bool VanillaExtGetSuitArm(std::uint8_t vanillaPartsType,
                              std::uint8_t playerType,
                              std::uint8_t wornCamo, bool* outEnable)
    {
        if (playerType >= kPlayerTypeMax) return false;
        if (playerType == kPlayerType_DDMale
            || playerType == kPlayerType_DDFemale)
        {
            if (outEnable) *outEnable = false;
            return true;
        }
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        std::uint8_t src = playerType;
        if (!x->suitArmOverride[src])
        {
            const std::uint8_t partner =
                (playerType == kPlayerType_Avatar) ? kPlayerType_Snake
              : (playerType == kPlayerType_Snake)  ? kPlayerType_Avatar
                                                   : std::uint8_t{0xFF};
            if (partner != 0xFF && x->suitArmOverride[partner]) src = partner;
            else return false;
        }
        if (!HeadCamoMatches_NoLock(x->suitArmCamo[src], wornCamo)) return false;
        if (outEnable) *outEnable = x->suitArmEnable[src];
        return true;
    }

    bool VanillaExtGetSuitHead(std::uint8_t vanillaPartsType,
                               std::uint8_t playerType,
                               std::uint8_t wornCamo, bool* outEnable)
    {
        if (playerType >= kPlayerTypeMax) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        std::uint8_t src = playerType;
        if (!x->suitHeadOverride[src])
        {
            const std::uint8_t partner =
                (playerType == kPlayerType_Avatar) ? kPlayerType_Snake
              : (playerType == kPlayerType_Snake)  ? kPlayerType_Avatar
                                                   : std::uint8_t{0xFF};
            if (partner != 0xFF && x->suitHeadOverride[partner]) src = partner;
            else return false;
        }
        if (!HeadCamoMatches_NoLock(x->suitHeadCamo[src], wornCamo)) return false;
        if (outEnable) *outEnable = x->suitHeadEnable[src];
        return true;
    }

    bool VanillaExtHasAnyAbilities()
    {
        return g_VanillaExtAbilitiesAny.load(std::memory_order_relaxed);
    }

    static void PublishVanillaExtSuitParam_NoLock(const VanillaSuitExtension* x)
    {
        if (!x) return;
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            const bool branch = x->suitAbilityOverride[pt];
            const std::uint8_t kind =
                branch ? x->suitAbilityParamKind[pt] : std::uint8_t{0};
            const std::uint8_t def  =
                branch ? x->suitAbilityDefense[pt] : std::uint8_t{0};
            const std::uint8_t reg  =
                branch ? x->suitAbilityRegen[pt] : std::uint8_t{0};
            for (std::uint8_t s = 0; s < x->variantSelectorCount
                 && s < kMaxVanillaExtVariants; ++s)
            {
                const std::uint8_t sel = x->variantSelectorCodes[s];
                if (sel == 0) continue;
                const VanillaSuitVariantAsset& v = x->variants[pt][s];
                const bool own = v.used && v.abilities.declared;
                if (!branch && !own) continue;
                g_SuitParamDonor[sel][pt].store(
                    own ? v.abilities.suitParamKind : kind,
                    std::memory_order_relaxed);
                g_AbilityDefense[sel][pt].store(
                    own ? v.abilities.defense : def,
                    std::memory_order_relaxed);
                g_AbilityRegen[sel][pt].store(
                    own ? v.abilities.lifeRecovery : reg,
                    std::memory_order_relaxed);
            }
        }
    }

    bool ExtendVanillaSuitAbilities(std::uint8_t vanillaPartsType,
                                    std::uint8_t playerType,
                                    std::uint8_t sourceCamo,
                                    const VanillaSuitAbilities& abilities)
    {
        if (IsCustomPartsType(vanillaPartsType) || vanillaPartsType == 0xFF)
            return false;
        if (playerType >= kPlayerTypeMax)
            return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        VanillaSuitExtension* x = FindOrCreateVanillaExt_NoLock(vanillaPartsType);
        x->suitAbilityOverride[playerType]  = true;
        x->suitAbilityCamo[playerType]      = sourceCamo;
        x->suitAbilitySilent[playerType]    = abilities.silentSteps;
        x->suitAbilityDefense[playerType]   = abilities.defense;
        x->suitAbilityRegen[playerType]     = abilities.lifeRecovery;
        x->suitAbilityRattle[playerType]    = abilities.rattleSuit;
        x->suitAbilityDamageSe[playerType]  = abilities.damageSe;
        x->suitAbilityParamKind[playerType] = abilities.suitParamKind;
        PublishVanillaExtSuitParam_NoLock(x);
        g_VanillaExtAbilitiesAny.store(true, std::memory_order_relaxed);
        return true;
    }

    static std::uint8_t VanillaExtAbilitySource_NoLock(
        const VanillaSuitExtension* x, std::uint8_t playerType)
    {
        if (x->suitAbilityOverride[playerType]) return playerType;
        const std::uint8_t partner =
            (playerType == kPlayerType_Avatar) ? kPlayerType_Snake
          : (playerType == kPlayerType_Snake)  ? kPlayerType_Avatar
                                               : std::uint8_t{0xFF};
        if (partner != 0xFF && x->suitAbilityOverride[partner]) return partner;
        return 0xFF;
    }

    bool VanillaExtGetSuitAbilities(std::uint8_t vanillaPartsType,
                                    std::uint8_t playerType,
                                    std::uint8_t wornCamo,
                                    VanillaSuitAbilities* out)
    {
        if (playerType >= kPlayerTypeMax) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        const std::uint8_t src = VanillaExtAbilitySource_NoLock(x, playerType);
        if (src == 0xFF) return false;
        if (!HeadCamoMatches_NoLock(x->suitAbilityCamo[src], wornCamo))
            return false;
        if (out)
        {
            out->silentSteps   = x->suitAbilitySilent[src];
            out->defense       = x->suitAbilityDefense[src];
            out->lifeRecovery  = x->suitAbilityRegen[src];
            out->rattleSuit    = x->suitAbilityRattle[src];
            out->damageSe      = x->suitAbilityDamageSe[src];
            out->suitParamKind = x->suitAbilityParamKind[src];
        }
        return true;
    }

    bool VanillaExtGetPinFlags(std::uint8_t vanillaPartsType,
                               VanillaExtPinFlags* out)
    {
        if (!out) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        bool any = false;
        for (std::uint8_t pt = 0; pt < kPlayerTypeMax; ++pt)
        {
            const std::uint8_t src = VanillaExtAbilitySource_NoLock(x, pt);
            if (src == 0xFF) continue;
            out->supported[pt] = true;
            out->quietMove[pt] = x->suitAbilityParamKind[src] != 0;
            any = true;
        }
        return any;
    }

    bool VanillaExtHasQuietMovement(std::uint8_t vanillaPartsType,
                                    std::uint8_t playerType)
    {
        if (playerType >= kPlayerTypeMax) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        const std::uint8_t src = VanillaExtAbilitySource_NoLock(x, playerType);
        if (src == 0xFF) return false;
        return x->suitAbilityParamKind[src] != 0;
    }

    bool VanillaExtHasAnyHeadOptions(std::uint8_t vanillaPartsType,
                                     std::uint8_t playerType,
                                     std::uint8_t wornCamo)
    {
        if (playerType >= kPlayerTypeMax) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        const auto& b = x->perPlayerType[playerType];
        for (std::uint8_t j = 0; j < b.headOptionCount; ++j)
            if (HeadCamoMatches_NoLock(b.headOptionSourceCamo[j], wornCamo))
                return true;
        return false;
    }

    bool VanillaExtHasHeadOption(std::uint8_t vanillaPartsType,
                                 std::uint16_t equipId,
                                 std::uint8_t playerType,
                                 std::uint8_t wornCamo)
    {
        if (playerType >= kPlayerTypeMax || equipId == 0) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        const auto& b = x->perPlayerType[playerType];
        for (std::uint8_t j = 0; j < b.headOptionCount; ++j)
            if (b.headOptionEquipIds[j] == equipId
                && HeadCamoMatches_NoLock(b.headOptionSourceCamo[j], wornCamo))
                return true;
        return false;
    }

    bool VanillaExtGetHeadOptions(std::uint8_t vanillaPartsType,
                                  std::uint8_t playerType,
                                  std::uint8_t wornCamo,
                                  std::uint16_t* outEquipIds,
                                  std::uint8_t maxEquipIds,
                                  std::uint8_t* outCount)
    {
        if (outCount) *outCount = 0;
        if (playerType >= kPlayerTypeMax || !outEquipIds || maxEquipIds == 0)
            return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const VanillaSuitExtension* x = FindVanillaExt_NoLock(vanillaPartsType);
        if (!x) return false;
        const auto& b = x->perPlayerType[playerType];
        std::uint8_t n = 0;
        for (std::uint8_t j = 0; j < b.headOptionCount && n < maxEquipIds; ++j)
            if (HeadCamoMatches_NoLock(b.headOptionSourceCamo[j], wornCamo))
                outEquipIds[n++] = b.headOptionEquipIds[j];
        if (outCount) *outCount = n;
        return n > 0;
    }


    void SetPendingOutfitDevelopId(std::uint16_t developId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PendingDevelopId = developId;
        ++g_PendingDevelopIdStamp;
    }

    std::uint32_t GetPendingOutfitDevelopIdStamp()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_PendingDevelopIdStamp;
    }

    std::uint16_t GetPendingOutfitDevelopId()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_PendingDevelopId;
    }

    void ClearPendingOutfitDevelopId()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PendingDevelopId = 0;
    }

    void SetPendingHeadOptionEquipId(std::uint16_t equipId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PendingHeadOptionEquipId = equipId;
    }

    std::uint16_t GetPendingHeadOptionEquipId()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_PendingHeadOptionEquipId;
    }

    void ClearPendingHeadOptionEquipId()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PendingHeadOptionEquipId = 0;
    }

    void SetWornCustomHeadSlot(std::uint8_t slot)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_WornCustomHeadSlot = slot;
    }

    std::uint8_t GetWornCustomHeadSlot()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_WornCustomHeadSlot;
    }

    void ClearWornCustomHeadSlot()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_WornCustomHeadSlot = 0;
    }

    void SetSupplyDropClickLatch()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_SupplyDropClickLatch = true;
    }

    bool ConsumeSupplyDropClickLatch()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const bool was = g_SupplyDropClickLatch;
        g_SupplyDropClickLatch = false;
        return was;
    }

    void SetPendingSupplyDropDevelopId(std::uint16_t developId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PendingSupplyDropDevelopId = developId;
    }

    std::uint16_t ConsumePendingSupplyDropDevelopId()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const std::uint16_t was = g_PendingSupplyDropDevelopId;
        g_PendingSupplyDropDevelopId = 0;
        return was;
    }

    std::uint16_t PeekPendingSupplyDropDevelopId()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_PendingSupplyDropDevelopId;
    }

    void SetPendingSupplyDropVariantIdx(std::uint8_t variantIndex)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PendingSupplyDropVariantIdx = variantIndex;
    }

    std::uint8_t ConsumePendingSupplyDropVariantIdx()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const std::uint8_t was = g_PendingSupplyDropVariantIdx;
        g_PendingSupplyDropVariantIdx = 0;
        return was;
    }

    std::uint8_t PeekPendingSupplyDropVariantIdx()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_PendingSupplyDropVariantIdx;
    }

    void SetCrateDeliveredVariant(std::uint16_t developId,
                                  std::uint8_t variantIndex)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_CrateDeliveredDevelopId  = developId;
        g_CrateDeliveredVariantIdx = variantIndex;
    }

    std::uint16_t PeekCrateDeliveredDevelopId()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_CrateDeliveredDevelopId;
    }

    std::uint8_t PeekCrateDeliveredVariantIdx()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_CrateDeliveredVariantIdx;
    }

    void ClearCrateDeliveredVariant()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_CrateDeliveredDevelopId  = 0;
        g_CrateDeliveredVariantIdx = 0;
    }
}
