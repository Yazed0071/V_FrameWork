#include "pch.h"

#include "OutfitAbilities.h"
#include "OutfitRegistry.h"
#include "BlockControllerImpl_LoadPartsNew.h"
#include "SkillAndItemParameterSystemImpl_GetSuitParam.h"
#include "CustomHeadRegistry.h"
#include "UniqueCharacterDefaultOutfit.h"
#include "ShadowState.h"
#include "AdditionalMotionTable_GetMtarPathId.h"
#include "MissionCodeGuard.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaBroadcaster.h"

namespace
{
    constexpr std::uintptr_t kSentinel_GetQuarkSystemTable   = 0x140bff050ull;
    constexpr std::uintptr_t kSentinel_GetQuarkSystemTableJp = 0x140bfefd0ull;

    static std::atomic<bool> g_AddressSetIsEn{ true };
    constexpr std::uintptr_t kAddr_SuitParamTable          = 0x1423997e0ull;
    constexpr std::uintptr_t kAddr_ScriptConditionBits     = 0x142c1c2aaull;

    constexpr std::uint8_t  kConditionBit_InfiniteAmmo = 0x04;
    constexpr std::uint32_t kSuitTableRows             = 149;
    constexpr std::uint32_t kSuitTableRowStride        = 12;
    constexpr std::uint32_t kCamo_SneakingSuitTpp      = 15;
    constexpr std::uint32_t kCamo_BattleDress          = 16;
    constexpr std::uint8_t  kMaxAbilityLevel           = 9;

    constexpr std::size_t kHolderOff_CalcDamage   = 0x8;
    constexpr std::size_t kHolderOff_LifeRecovery = 0x28;
    constexpr std::size_t kHolder_WornSuitArr     = 0x70;
    constexpr std::size_t kHolder_SlotCount       = 0x208;
    constexpr std::size_t kSeParam_PartsTypeOff   = 0x13;
    constexpr std::size_t kDmgCtl_Owner           = 0x28;
    constexpr std::size_t kDmgOwner_PlayerMgr     = 0x138;
    constexpr std::size_t kPlayerMgr_Parts        = 0x60;
    constexpr std::size_t kPlayerMgr_LocalSlot    = 0x214;
    constexpr std::size_t kParts_CamoArr          = 0x50;
    constexpr std::uint8_t kCamoByte_BattleDress  = 0x10;
    constexpr std::uint8_t kCamoByte_Plain        = 0x00;
    constexpr std::uint16_t kWornSuitNone         = 0xFFFF;
    constexpr std::size_t kMaxSlots = outfit::shadow::kMaxSlots;

    static std::atomic<bool> g_DonorSwapFaulted{ false };

    constexpr std::uint8_t kPrologue_CalcDamage[] =
        { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x89, 0x50, 0x10, 0x55 };
    constexpr std::uint8_t kPrologue_UpdateLife[] =
        { 0x40, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
          0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00 };
    constexpr std::uint8_t kPrologue_UpdateLifeNoRex[] =
        { 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
          0x48, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00 };
    constexpr std::uint8_t kPrologue_RattleBody[] =
        { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
          0x48, 0x89, 0x7C, 0x24, 0x18 };
    constexpr std::uint8_t kPrologue_Jmp[] = { 0xE9 };
    constexpr std::uint8_t kPrologue_FootStepBody[] =
        { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
          0x57, 0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xFE, 0xFF, 0xFF,
          0x48, 0x81, 0xEC, 0x40, 0x02, 0x00, 0x00 };
    constexpr std::uint8_t kPrologue_DamageEffect[] =
        { 0x40, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55,
          0x48, 0x8D, 0xAC, 0x24, 0x00, 0xFD, 0xFF, 0xFF,
          0x48, 0x81, 0xEC, 0x00, 0x04, 0x00, 0x00 };
    constexpr std::uint8_t kPrologue_FootStepBodyNoRex[] =
        { 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
          0x48, 0x8D, 0xAC, 0x24, 0xC0, 0xFE, 0xFF, 0xFF,
          0x48, 0x81, 0xEC, 0x40, 0x02, 0x00, 0x00 };

    struct HookCandidate
    {
        std::uintptr_t     addr;
        const std::uint8_t* expect;
        std::size_t        expectLen;
    };

    constexpr HookCandidate kCandidates_CalcDamage[] =
    {
        { 0x1411f5350ull, kPrologue_CalcDamage, sizeof(kPrologue_CalcDamage) },
        { 0x1411f5dc0ull, kPrologue_CalcDamage, sizeof(kPrologue_CalcDamage) },
    };
    constexpr HookCandidate kCandidates_UpdateLife[] =
    {
        { 0x1411f92d0ull, kPrologue_UpdateLife, sizeof(kPrologue_UpdateLife) },
        { 0x1411f92d0ull, kPrologue_UpdateLifeNoRex,
          sizeof(kPrologue_UpdateLifeNoRex) },
        { 0x1411f9d40ull, kPrologue_UpdateLife, sizeof(kPrologue_UpdateLife) },
    };
    constexpr HookCandidate kCandidates_Rattle[] =
    {
        { 0x140984c30ull, kPrologue_RattleBody, sizeof(kPrologue_RattleBody) },
        { 0x140983da0ull, kPrologue_Jmp, sizeof(kPrologue_Jmp) },
    };
    constexpr HookCandidate kCandidates_FootStep[] =
    {
        { 0x140984010ull, kPrologue_FootStepBody,
          sizeof(kPrologue_FootStepBody) },
        { 0x140984010ull, kPrologue_FootStepBodyNoRex,
          sizeof(kPrologue_FootStepBodyNoRex) },
    };

    constexpr HookCandidate kCandidates_DamageEffect[] =
    {
        { 0x141192840ull, kPrologue_DamageEffect,
          sizeof(kPrologue_DamageEffect) },
        { 0x141193070ull, kPrologue_DamageEffect,
          sizeof(kPrologue_DamageEffect) },
        { 0x1411928c0ull, kPrologue_DamageEffect,
          sizeof(kPrologue_DamageEffect) },
    };

    using CalcDamage_t = std::uint64_t (__fastcall*)(
        void*, std::uint32_t, void*, void*, void*, void*, void*, void*, void*);
    using UpdateLife_t = void (__fastcall*)(void*);
    using ConvertRattle_t = void* (__fastcall*)(
        void*, void*, std::uint64_t, void*, void*, void*, std::uint64_t, void*,
        void*);
    using ConvertFootStep_t = void (__fastcall*)(
        void*, void*, std::uint64_t, void*, void**, void**, std::uint32_t,
        void*, void*, void*);

    static CalcDamage_t    g_OrigCalcDamage    = nullptr;
    static UpdateLife_t    g_OrigUpdateLife    = nullptr;
    static ConvertRattle_t   g_OrigConvertRattle   = nullptr;
    static ConvertFootStep_t g_OrigConvertFootStep = nullptr;
    static bool g_InstalledCalcDamage = false;
    static bool g_InstalledUpdateLife = false;
    static bool g_InstalledRattle     = false;
    static bool g_InstalledFootStep   = false;

    using CallDamageEffect_t = bool (__fastcall*)(
        void*, std::uint32_t, void*, std::uint8_t, bool, bool);
    static CallDamageEffect_t g_OrigCallDamageEffect = nullptr;
    static bool g_InstalledDamageEffect = false;

    static std::atomic<bool> g_AmmoBitOwned{ false };

    struct DonorCacheEntry
    {
        std::uint16_t equipId = 0;
        std::uint8_t  level   = 0;
        std::uint8_t  state   = 0;
    };
    static DonorCacheEntry g_BdDonor[kMaxAbilityLevel + 1];
    static DonorCacheEntry g_SnkDonor[kMaxAbilityLevel + 1];
    static std::atomic<DWORD> g_DonorRetryTick{ 0 };

    static bool ReadSuitRow_SEH(std::uint32_t idx, std::uint16_t* outId,
                                std::uint32_t* outCamo, std::uint8_t* outLevel)
    {
        __try
        {
            const std::uint8_t* row = reinterpret_cast<const std::uint8_t*>(
                kAddr_SuitParamTable) + idx * kSuitTableRowStride;
            *outId    = *reinterpret_cast<const std::uint16_t*>(row);
            *outCamo  = *reinterpret_cast<const std::uint32_t*>(row + 4);
            *outLevel = row[8];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void DumpSuitTableHeadOnce()
    {
        static std::atomic<bool> s_dumped{ false };
        if (s_dumped.exchange(true))
            return;
        std::uint8_t raw[48] = {};
        bool ok = false;
        __try
        {
            std::memcpy(raw, reinterpret_cast<const void*>(kAddr_SuitParamTable),
                        sizeof(raw));
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        if (!ok)
        {
            LogDebug("[OutfitAbilities] suit table @ 0x%llX unreadable\n",
                static_cast<unsigned long long>(kAddr_SuitParamTable));
            return;
        }
        LogDebug("[OutfitAbilities] suit table head (first 4 rows raw): "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
            raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14],
            raw[15], raw[16], raw[17], raw[18], raw[19], raw[20], raw[21],
            raw[22], raw[23], raw[24], raw[25], raw[26], raw[27], raw[28],
            raw[29], raw[30], raw[31], raw[32], raw[33], raw[34], raw[35],
            raw[36], raw[37], raw[38], raw[39], raw[40], raw[41], raw[42],
            raw[43], raw[44], raw[45], raw[46], raw[47]);
    }

    static std::uint16_t ScanSuitTableForDonor(std::uint32_t camo,
                                               std::uint8_t wantLevel,
                                               std::uint8_t* outLevel)
    {
        DumpSuitTableHeadOnce();
        std::uint16_t minId = 0xFFFF;
        for (std::uint32_t i = 0; i < kSuitTableRows; ++i)
        {
            std::uint16_t id  = 0;
            std::uint32_t c   = 0;
            std::uint8_t  lvl = 0;
            if (!ReadSuitRow_SEH(i, &id, &c, &lvl))
                return 0;
            if (id == 0 || id == kWornSuitNone || c > 130 || lvl > 10)
                continue;
            if (id < minId)
                minId = id;
        }
        if (minId == 0xFFFF)
        {
            static std::atomic<bool> s_warnedEmpty{ false };
            if (!s_warnedEmpty.exchange(true))
                Log("[OutfitAbilities] suit table @ 0x%llX has no sane rows - "
                    "defense/lifeRecovery donors disabled this session\n",
                    static_cast<unsigned long long>(kAddr_SuitParamTable));
            return 0;
        }

        std::uint16_t bestLoId = 0;
        std::uint8_t  bestLo   = 0;
        std::uint16_t bestHiId = 0;
        std::uint8_t  bestHi   = 0xFF;
        std::uint32_t valid    = 0;
        for (std::uint32_t i = 0; i < kSuitTableRows; ++i)
        {
            std::uint16_t id  = 0;
            std::uint32_t c   = 0;
            std::uint8_t  lvl = 0;
            if (!ReadSuitRow_SEH(i, &id, &c, &lvl))
                return 0;
            if (id < minId || static_cast<std::uint32_t>(id - minId) >= 2048
                || c > 130 || lvl > 10)
                continue;
            ++valid;
            if (c != camo)
                continue;
            if (lvl <= wantLevel)
            {
                if (bestLoId == 0 || lvl > bestLo)
                {
                    bestLo   = lvl;
                    bestLoId = id;
                }
            }
            else if (lvl < bestHi)
            {
                bestHi   = lvl;
                bestHiId = id;
            }
        }
        if (valid < 100)
        {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                Log("[OutfitAbilities] suit table @ 0x%llX REJECTED (%u of %u rows "
                    "in id band %u..%u with camo<=130 level<=10) - "
                    "defense/lifeRecovery donors disabled\n",
                    static_cast<unsigned long long>(kAddr_SuitParamTable),
                    valid, kSuitTableRows, static_cast<unsigned>(minId),
                    static_cast<unsigned>(minId + 2047));
            return 0;
        }
        if (bestLoId != 0)
        {
            if (outLevel) *outLevel = bestLo;
            return bestLoId;
        }
        if (outLevel) *outLevel = bestHi;
        return bestHiId;
    }

    using GetSkillParam_t =
        float (__fastcall*)(void*, std::uint32_t, std::uint32_t);

    constexpr std::size_t   kPlayerMgr_OffSkillIface = 0x130;
    constexpr std::size_t   kSkillVt_GetSkillParam   = 0x28;
    constexpr std::size_t   kLifeState_OffDamageRate = 0x28;
    constexpr std::uint32_t kSkillParam_DamageRate   = 0x12;
    constexpr std::uint32_t kSkillParam_LifeRecovery = 0x10;

    static bool ReadSkillParams_SEH(void* lifeCtl, std::uint32_t playerIndex,
                                    float* outDefense, float* outRegen)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(lifeCtl);
            void* mgr = *reinterpret_cast<void**>(base + kHolderOff_LifeRecovery);
            if (!mgr) return false;
            void* iface = *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(mgr) + kPlayerMgr_OffSkillIface);
            if (!iface) return false;
            auto* vt = *reinterpret_cast<std::uint8_t**>(iface);
            if (!vt) return false;
            auto fn = *reinterpret_cast<GetSkillParam_t*>(
                vt + kSkillVt_GetSkillParam);
            if (!fn) return false;
            *outDefense = fn(iface, playerIndex, kSkillParam_DamageRate);
            *outRegen   = fn(iface, playerIndex, kSkillParam_LifeRecovery);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ReadFloatAt_SEH(void* p, std::size_t off, float* out)
    {
        __try
        {
            if (!p) return false;
            *out = *reinterpret_cast<float*>(
                reinterpret_cast<std::uint8_t*>(p) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    constexpr std::size_t  kSeParam_FlagsOff       = 0x12;
    constexpr std::uint8_t kSeFlag_SuppressRattle  = 0x02;
    constexpr std::size_t  kConvertInfoOff         = 0x08;
    constexpr std::size_t  kClothRowTableOff       = 0x7A0;

    static int RattleFoldIndex(std::uint8_t v)
    {
        if (v == 2 || v == 8) return 2;
        if (v == 3)           return 1;
        if (v == 9)           return 3;
        return 0;
    }

    static bool ReadClothRowTable_SEH(void* self, std::uint32_t* out4)
    {
        __try
        {
            if (!self) return false;
            void* ci = *reinterpret_cast<void**>(
                reinterpret_cast<char*>(self) + kConvertInfoOff);
            if (!ci) return false;
            const std::uint32_t* t = reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<char*>(ci) + kClothRowTableOff);
            for (int i = 0; i < 4; ++i) out4[i] = t[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ReadOutParamPP_SEH(void* pp, std::uint32_t* out)
    {
        __try
        {
            if (!pp) return false;
            std::uint32_t* inner = *reinterpret_cast<std::uint32_t**>(pp);
            if (!inner) return false;
            *out = inner[0];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool WriteOutParamPP_SEH(void* pp, std::uint32_t value)
    {
        __try
        {
            if (!pp) return false;
            std::uint32_t* inner = *reinterpret_cast<std::uint32_t**>(pp);
            if (!inner) return false;
            inner[0] = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool VextActiveVariantAbilities(std::uint8_t parts,
                                           std::uint8_t playerType,
                                           std::uint8_t selector,
                                           outfit::DeclaredAbilities* out)
    {
        std::uint8_t vextParts = 0xFF;
        std::uint8_t variantIdx = 0;
        if (!outfit::TryGetVanillaExtByVariantSelector(selector, &vextParts,
                                                       &variantIdx))
            return false;
        if (vextParts != parts)
            return false;
        return outfit::VanillaExtGetVariantAbilities(parts, playerType,
                                                     variantIdx, out);
    }

    static std::uint32_t ResolveClothSwitch(std::uint8_t seParts,
                                            std::uint8_t* outParts)
    {
        std::uint8_t parts = seParts;
        if (parts < outfit::kCustomPartsTypeStart
            || parts > outfit::kCustomPartsTypeEnd)
            parts = outfit::ReadLivePartsType();
        if (outParts) *outParts = parts;

        const std::uint8_t livePT = outfit::ReadLivePlayerType();
        std::uint32_t want = outfit::kSoundSwitchUnset;

        if (parts >= outfit::kCustomPartsTypeStart
            && parts <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* e = nullptr;
            if (outfit::TryGetOutfitByPartsType(parts, &e) && e)
            {
                const outfit::OutfitPlayerTypeData* d = e->GetPTData(livePT);
                if (d) want = d->abilityRattleSuit;
            }
            outfit::DeclaredAbilities rva{};
            if (outfit::TryGetVariantAbilities(
                    parts, livePT,
                    outfit::GetActiveVariantLockFree(parts), &rva)
                && rva.rattleSuit != outfit::kSoundSwitchUnset)
                want = rva.rattleSuit;
        }
        else if (outfit::VanillaExtHasAnyAbilities())
        {
            const std::uint8_t sel = outfit::ReadLiveSelectorCode();
            outfit::VanillaSuitAbilities va{};
            if (outfit::VanillaExtGetSuitAbilities(parts, livePT, sel, &va))
                want = va.rattleSuit;
            outfit::DeclaredAbilities vva{};
            if (VextActiveVariantAbilities(parts, livePT, sel, &vva)
                && vva.rattleSuit != outfit::kSoundSwitchUnset)
                want = vva.rattleSuit;
        }
        return want;
    }

    static bool ReadQwordAt_SEH(void* p, std::uint64_t* out)
    {
        __try
        {
            if (!p) return false;
            *out = *reinterpret_cast<std::uint64_t*>(p);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static std::uint16_t GetDonorEquipId(bool battleDress, std::uint8_t level)
    {
        if (level == 0) return 0;

        if (!g_AddressSetIsEn.load(std::memory_order_relaxed))
        {
            static std::atomic<bool> s_jpDonorLogged{ false };
            if (!s_jpDonorLogged.exchange(true))
                Log("[OutfitAbilities] the suit-donor table is a raw EN-only "
                    "address, so defense and lifeRecovery are off on this build - "
                    "reading it here would scan unrelated Japanese data and could "
                    "hand the engine a meaningless suit equipId\n");
            return 0;
        }

        if (level > kMaxAbilityLevel) level = kMaxAbilityLevel;
        DonorCacheEntry& e = battleDress ? g_BdDonor[level] : g_SnkDonor[level];
        if (e.state == 1) return e.equipId;
        if (e.state == 2)
        {
            const DWORD last = g_DonorRetryTick.load(std::memory_order_relaxed);
            if (last != 0 && (GetTickCount() - last) < 2000)
                return 0;
        }
        std::uint8_t served = 0;
        const std::uint16_t id = ScanSuitTableForDonor(
            battleDress ? kCamo_BattleDress : kCamo_SneakingSuitTpp, level,
            &served);
        if (id == 0)
        {
            e.state = 2;
            g_DonorRetryTick.store(GetTickCount(), std::memory_order_relaxed);
            return 0;
        }
        e.equipId = id;
        e.level   = served;
        e.state   = 1;
        LogDebug("[OutfitAbilities] donor resolved: %s level %u -> suit "
            "equipId=%u (row grade %u%s)\n",
            battleDress ? "BATTLEDRESS" : "SNEAKING_SUIT_TPP",
            static_cast<unsigned>(level), static_cast<unsigned>(id),
            static_cast<unsigned>(served),
            served == level ? "" : ", nearest available");
        return id;
    }

    struct SlotAbilities
    {
        bool         ok      = false;
        bool         silent  = false;
        std::uint8_t defense = 0;
        std::uint8_t regen   = 0;
    };

    static void ReportAbilityLookupMiss(std::size_t slot, std::uint8_t parts,
                                        std::uint8_t pt, const char* why)
    {
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1, std::memory_order_relaxed) >= 8) return;
        Log("[OutfitAbilities] slot %zu carries partsType 0x%02X for player "
            "type %u but no ability set resolved (%s) - the suit renders, its "
            "abilities do not\n",
            slot, static_cast<unsigned>(parts), static_cast<unsigned>(pt), why);
    }

    static SlotAbilities GetSlotAbilities(std::size_t slot)
    {
        SlotAbilities r{};
        outfit::shadow::Slot s;
        const bool haveShadow = outfit::shadow::Get(slot, &s);

        if (haveShadow
            && s.realPartsType >= outfit::kCustomPartsTypeStart
            && s.realPartsType <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* e = nullptr;
            if (!outfit::TryGetOutfitByPartsType(s.realPartsType, &e) || !e)
            {
                ReportAbilityLookupMiss(slot, s.realPartsType, s.realPlayerType,
                                        "no registered outfit owns that partsType");
                return r;
            }
            const outfit::OutfitPlayerTypeData* d = e->GetPTData(s.realPlayerType);
            if (!d)
            {
                ReportAbilityLookupMiss(slot, s.realPartsType, s.realPlayerType,
                                        "the outfit declares no branch for that "
                                        "player type");
                return r;
            }
            r.ok      = true;
            r.silent  = d->abilitySilentSteps;
            r.defense = d->abilityDefense;
            r.regen   = d->abilityLifeRecovery;

            outfit::DeclaredAbilities va{};
            if (outfit::TryGetVariantAbilities(s.realPartsType, s.realPlayerType,
                                               s.variantIdx, &va))
            {
                r.silent  = va.silentSteps;
                r.defense = va.defense;
                r.regen   = va.lifeRecovery;
            }
            return r;
        }

        if (!outfit::VanillaExtHasAnyAbilities())
            return r;

        const std::uint8_t parts = haveShadow ? s.realPartsType
                                              : outfit::ReadLivePartsType();
        const std::uint8_t pt    = haveShadow ? s.realPlayerType
                                              : outfit::ReadLivePlayerType();
        const std::uint8_t camo  = haveShadow ? s.realCamoType
                                              : outfit::ReadLiveSelectorCode();

        outfit::VanillaSuitAbilities va{};
        if (outfit::VanillaExtGetSuitAbilities(parts, pt, camo, &va))
        {
            r.ok      = true;
            r.silent  = va.silentSteps;
            r.defense = va.defense;
            r.regen   = va.lifeRecovery;
        }

        outfit::DeclaredAbilities vva{};
        if (VextActiveVariantAbilities(parts, pt, camo, &vva))
        {
            r.ok      = true;
            r.silent  = vva.silentSteps;
            r.defense = vva.defense;
            r.regen   = vva.lifeRecovery;
        }
        return r;
    }

    static std::uint16_t* GetWornArr_SEH(void* self, std::size_t holderOff,
                                         std::uint32_t* outCount)
    {
        __try
        {
            auto* holder = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(self) + holderOff);
            if (!holder)
                return nullptr;
            auto* arr = *reinterpret_cast<std::uint16_t**>(
                holder + kHolder_WornSuitArr);
            if (outCount)
                *outCount = *reinterpret_cast<std::uint32_t*>(
                    holder + kHolder_SlotCount);
            return arr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static bool ReadWorn_SEH(std::uint16_t* arr, std::size_t slot,
                             std::uint16_t* out)
    {
        __try
        {
            *out = arr[slot];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool WriteWorn_SEH(std::uint16_t* arr, std::size_t slot,
                              std::uint16_t value)
    {
        __try
        {
            arr[slot] = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static std::uint8_t ReadSeByte_SEH(void* p, std::size_t off,
                                       bool* okOut)
    {
        __try
        {
            const std::uint8_t v =
                reinterpret_cast<const std::uint8_t*>(p)[off];
            *okOut = true;
            return v;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *okOut = false;
            return 0;
        }
    }

    static bool WriteSeByte_SEH(void* p, std::size_t off, std::uint8_t v)
    {
        __try
        {
            reinterpret_cast<std::uint8_t*>(p)[off] = v;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool RmwAmmoBit_SEH(bool set)
    {
        if (!g_AddressSetIsEn.load(std::memory_order_relaxed))
        {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true, std::memory_order_relaxed))
                Log("[OutfitAbilities] abilities.infiniteAmmo DISABLED on this "
                    "build: the script-condition byte is an EN-only raw address and "
                    "writing it elsewhere would corrupt an unrelated variable. "
                    "Other abilities verify their bytes and are unaffected\n");
            return false;
        }

        __try
        {
            auto* p = reinterpret_cast<std::uint8_t*>(kAddr_ScriptConditionBits);
            const std::uint8_t cur = *p;
            if (set)
            {
                if ((cur & kConditionBit_InfiniteAmmo) == 0)
                    *p = cur | kConditionBit_InfiniteAmmo;
            }
            else
            {
                if ((cur & kConditionBit_InfiniteAmmo) != 0)
                    *p = cur & static_cast<std::uint8_t>(~kConditionBit_InfiniteAmmo);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void SyncInfiniteAmmoBit(bool forceOff)
    {
        bool desired = false;
        if (!forceOff)
        {
            const std::uint8_t slot = outfit::GetWornCustomHeadSlot();
            if (slot >= outfit::kCustomHeadSlotBase)
            {
                if (const outfit::CustomHeadEntry* h =
                        outfit::TryGetCustomHeadBySlot(slot))
                    desired = outfit::GetCustomHeadInfiniteAmmo(h->name);
            }
        }
        if (desired)
        {
            if (RmwAmmoBit_SEH(true)
                && !g_AmmoBitOwned.exchange(true, std::memory_order_relaxed))
                LogDebug("[OutfitAbilities] infinite ammo ON (custom head with "
                    "infiniteAmmo worn - condition bit 0x4 set)\n");
        }
        else if (g_AmmoBitOwned.exchange(false, std::memory_order_relaxed))
        {
            RmwAmmoBit_SEH(false);
            LogDebug("[OutfitAbilities] infinite ammo OFF (head removed or "
                "hooks bypassed - condition bit 0x4 cleared)\n");
        }
    }

    struct EquipDataProbe
    {
        void*         holder    = nullptr;
        void*         implArr   = nullptr;
        void*         impl      = nullptr;
        void*         equipData = nullptr;
        void*         rows280   = nullptr;
        void*         kinds2D8  = nullptr;
        void*         flt350    = nullptr;
        void*         flt358    = nullptr;
        void*         map138    = nullptr;
        void*         map60     = nullptr;
        void*         map18     = nullptr;
        std::uint32_t baseId    = 0;
        std::uint8_t  ptByte    = 0;
        std::uint8_t  head[64]  = {};
        std::uint8_t  rowsHead[32]  = {};
        std::uint8_t  kindsHead[16] = {};
    };

    static bool ReadEquipDataProbe_SEH(void* self, EquipDataProbe* p)
    {
        __try
        {
            auto* holder = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(self) + kHolderOff_LifeRecovery);
            if (!holder) return false;
            p->holder = holder;
            auto* ptArr = *reinterpret_cast<std::uint8_t**>(holder + 0x78);
            if (!ptArr) return false;
            p->ptByte = ptArr[0];
            auto* implArr = *reinterpret_cast<std::uint8_t**>(holder + 0x50);
            if (!implArr) return false;
            p->implArr = implArr;
            auto* impl = *reinterpret_cast<std::uint8_t**>(
                implArr + static_cast<std::size_t>(p->ptByte) * 8);
            if (!impl) return false;
            p->impl = impl;
            auto* ed = *reinterpret_cast<std::uint8_t**>(impl + 0x78);
            if (!ed) return false;
            p->equipData = ed;
            p->baseId   = *reinterpret_cast<std::uint32_t*>(ed + 0x8);
            p->rows280  = *reinterpret_cast<void**>(ed + 0x280);
            p->kinds2D8 = *reinterpret_cast<void**>(ed + 0x2D8);
            p->flt350   = *reinterpret_cast<void**>(ed + 0x350);
            p->flt358   = *reinterpret_cast<void**>(ed + 0x358);
            std::memcpy(p->head, ed, sizeof(p->head));
            auto* m138 = *reinterpret_cast<std::uint8_t**>(impl + 0x138);
            p->map138 = m138;
            if (m138)
            {
                auto* m60 = *reinterpret_cast<std::uint8_t**>(m138 + 0x60);
                p->map60 = m60;
                if (m60)
                    p->map18 = *reinterpret_cast<void**>(m60 + 0x18);
            }
            if (p->rows280)
                std::memcpy(p->rowsHead, p->rows280, sizeof(p->rowsHead));
            if (p->kinds2D8)
                std::memcpy(p->kindsHead, p->kinds2D8, sizeof(p->kindsHead));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void DumpEquipDataOnce(void* self)
    {
        static std::atomic<bool> s_done{ false };
        if (s_done.load(std::memory_order_relaxed))
            return;
        EquipDataProbe p;
        if (!ReadEquipDataProbe_SEH(self, &p))
            return;
        s_done.store(true, std::memory_order_relaxed);
        LogDebug("[OutfitAbilities] equipData probe: holder=%p implArr=%p "
            "pt=%u impl=%p equipData=%p baseId=%u rows280=%p kinds2D8=%p "
            "flt350=%p flt358=%p map138=%p map60=%p idToCamoMap18=%p\n",
            p.holder, p.implArr, static_cast<unsigned>(p.ptByte), p.impl,
            p.equipData, p.baseId, p.rows280, p.kinds2D8, p.flt350, p.flt358,
            p.map138, p.map60, p.map18);
        LogDebug("[OutfitAbilities] equipData head: %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
            p.head[0], p.head[1], p.head[2], p.head[3], p.head[4], p.head[5],
            p.head[6], p.head[7], p.head[8], p.head[9], p.head[10],
            p.head[11], p.head[12], p.head[13], p.head[14], p.head[15],
            p.head[16], p.head[17], p.head[18], p.head[19], p.head[20],
            p.head[21], p.head[22], p.head[23], p.head[24], p.head[25],
            p.head[26], p.head[27], p.head[28], p.head[29], p.head[30],
            p.head[31], p.head[32], p.head[33], p.head[34], p.head[35],
            p.head[36], p.head[37], p.head[38], p.head[39], p.head[40],
            p.head[41], p.head[42], p.head[43], p.head[44], p.head[45],
            p.head[46], p.head[47], p.head[48], p.head[49], p.head[50],
            p.head[51], p.head[52], p.head[53], p.head[54], p.head[55],
            p.head[56], p.head[57], p.head[58], p.head[59], p.head[60],
            p.head[61], p.head[62], p.head[63]);
        LogDebug("[OutfitAbilities] rows280 head: %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X | kinds2D8 head: %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
            p.rowsHead[0], p.rowsHead[1], p.rowsHead[2], p.rowsHead[3],
            p.rowsHead[4], p.rowsHead[5], p.rowsHead[6], p.rowsHead[7],
            p.rowsHead[8], p.rowsHead[9], p.rowsHead[10], p.rowsHead[11],
            p.rowsHead[12], p.rowsHead[13], p.rowsHead[14], p.rowsHead[15],
            p.rowsHead[16], p.rowsHead[17], p.rowsHead[18], p.rowsHead[19],
            p.rowsHead[20], p.rowsHead[21], p.rowsHead[22], p.rowsHead[23],
            p.rowsHead[24], p.rowsHead[25], p.rowsHead[26], p.rowsHead[27],
            p.rowsHead[28], p.rowsHead[29], p.rowsHead[30], p.rowsHead[31],
            p.kindsHead[0], p.kindsHead[1], p.kindsHead[2], p.kindsHead[3],
            p.kindsHead[4], p.kindsHead[5], p.kindsHead[6], p.kindsHead[7],
            p.kindsHead[8], p.kindsHead[9], p.kindsHead[10], p.kindsHead[11],
            p.kindsHead[12], p.kindsHead[13], p.kindsHead[14],
            p.kindsHead[15]);
    }

    using GetLocalPartsTypeFn  = std::uint16_t (__fastcall*)();
    using CheckStatusBitFn     = bool (__fastcall*)(std::uint32_t bit);

    constexpr std::uint8_t  kOcelotOwnParts   = 0x1A;
    constexpr std::uint8_t  kQuietOwnParts    = 0x1B;
    constexpr unsigned      kEnginePartsNone  = 0x00FFu;
    constexpr std::uint32_t kIdentityNone     = 0xFFFFFFFFu;
    constexpr int           kMaxUnsettledLines = 4;

    static std::atomic<std::uint32_t> g_LastIdentity{ kIdentityNone };
    static std::atomic<bool>          g_AwaitingSettle{ false };
    static std::atomic<int>           g_UnsettledLines{ 0 };

    static unsigned ReadEnginePartsType()
    {
        auto fn = reinterpret_cast<GetLocalPartsTypeFn>(
            ResolveGameAddress(gAddr.PlayerInfo_GetLocalPartsType));
        if (!fn) return kEnginePartsNone;
        __try { return fn(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return kEnginePartsNone; }
    }

    static const char* DescribeSuitClass(std::uint8_t parts, std::uint8_t pt)
    {
        std::uint8_t ownerPt = 0xFF;
        if (uniquedefaultoutfit::IsDefaultOutfitPartsType(parts, &ownerPt))
            return ownerPt == outfit::kPlayerType_Ocelot
                 ? "Ocelot's own suit" : "Quiet's own suit";
        if (pt == outfit::kPlayerType_Ocelot && parts == kOcelotOwnParts)
            return "Ocelot's own suit (vanilla parts type)";
        if (pt == outfit::kPlayerType_Quiet && parts == kQuietOwnParts)
            return "Quiet's own suit (vanilla parts type)";
        if (parts >= outfit::kCustomPartsTypeStart
            && parts <= outfit::kCustomPartsTypeEnd)
            return "custom outfit";
        return "vanilla suit";
    }

    static bool LogEngineIdentity(std::uint8_t parts, std::uint8_t sel,
                                  std::uint8_t pt, const char* trigger)
    {
        auto getEngineParts = reinterpret_cast<GetLocalPartsTypeFn>(
            ResolveGameAddress(gAddr.PlayerInfo_GetLocalPartsType));
        auto checkBit = reinterpret_cast<CheckStatusBitFn>(
            ResolveGameAddress(gAddr.PlayerInfo_CheckStatusBit));
        if (!getEngineParts || !checkBit)
        {
            static std::atomic<bool> s_toldUnported{ false };
            if (!s_toldUnported.exchange(true))
                Log("[AbilityProbe] no suit-identity samples on this build - "
                    "PlayerInfo_GetLocalPartsType and PlayerInfo_CheckStatusBit "
                    "are unported here, so the own-suit versus custom-outfit "
                    "comparison cannot be made\n");
            return true;
        }

        unsigned      engineParts = kEnginePartsNone;
        std::uint64_t bits        = 0;
        __try
        {
            engineParts = getEngineParts();
            for (std::uint32_t b = 0; b < 64; ++b)
                if (checkBit(b))
                    bits |= (1ull << b);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }

        const bool settled = engineParts == parts;

        if (settled)
            g_UnsettledLines.store(0, std::memory_order_relaxed);
        else if (g_UnsettledLines.fetch_add(1, std::memory_order_relaxed)
                 >= kMaxUnsettledLines)
            return false;

        Log("[AbilityProbe] %s (%s): parts=0x%02X camo=0x%02X quarkPT=%u | "
            "engineParts=0x%02X status[0..63]=%016llX%s. The status mask is "
            "scene state, not suit state - it reads the same under a custom "
            "outfit as under the own suit, so do not read a suit difference "
            "into it\n",
            DescribeSuitClass(parts, pt), trigger,
            static_cast<unsigned>(parts), static_cast<unsigned>(sel),
            static_cast<unsigned>(pt), engineParts,
            static_cast<unsigned long long>(bits),
            settled
                ? ""
                : (engineParts == kEnginePartsNone
                    ? " - the engine has not published a parts type for this "
                      "slot yet, so the sample is incomplete and is retaken "
                      "once it does"
                    : " - the engine still reports the PREVIOUS parts type for "
                      "this slot, so the sample is stale and is retaken once it "
                      "catches up"));
        return settled;
    }

    static void EmitPartsTypeChange()
    {
        const std::uint8_t parts = outfit::ReadLivePartsType();
        const std::uint8_t sel   = outfit::ReadLiveSelectorCode();
        const std::uint8_t pt    = outfit::ReadLivePlayerType();
        if (parts == 0xFF)
            return;

        const std::uint32_t key =
            static_cast<std::uint32_t>(parts)
            | (static_cast<std::uint32_t>(sel) << 8)
            | (static_cast<std::uint32_t>(pt) << 16);

        const bool changed =
            g_LastIdentity.load(std::memory_order_relaxed) != key;

        bool settleRetake = false;
        if (!changed && g_AwaitingSettle.load(std::memory_order_relaxed))
            settleRetake = ReadEnginePartsType() == parts;

        if (!changed && !settleRetake)
            return;

        if (changed)
            g_UnsettledLines.store(0, std::memory_order_relaxed);

        g_LastIdentity.store(key, std::memory_order_relaxed);

        if (g_DonorSwapFaulted.exchange(false))
            Log("[OutfitAbilities] suit-ability donors re-armed - the outfit "
                "context changed, so the engine's equip data was rebuilt\n");

        const bool identitySettled = LogEngineIdentity(parts, sel, pt,
            changed ? "suit changed" : "engine identity settled");
        g_AwaitingSettle.store(!identitySettled, std::memory_order_relaxed);

        outfit::NoteLiveOutfitIdentity(parts, identitySettled);

        if (!changed)
            return;

        V_FrameWork::EmitMessage("Player", "partsTypeChange",
            pt, parts, sel);
#ifdef _DEBUG
        LogDebug("[OutfitAbilities] Player.partsTypeChange: playerType=%u "
            "partsType=0x%02X camoType=0x%02X\n",
            static_cast<unsigned>(pt), static_cast<unsigned>(parts),
            static_cast<unsigned>(sel));
#endif
    }

    static int DonorSehAvOnly(unsigned int code)
    {
        return code == EXCEPTION_ACCESS_VIOLATION
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    static bool CallOrigUpdateLife_SEH(void* self)
    {
        __try
        {
            g_OrigUpdateLife(self);
            return true;
        }
        __except (DonorSehAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    static void EnsureNoiseProbeArmed();
    static void SyncQuietStepPatch();

    static void __fastcall hkUpdateLifeRecoverySpeed(void* self)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
        {
            SyncInfiniteAmmoBit(true);
            g_OrigUpdateLife(self);
            return;
        }

        SyncInfiniteAmmoBit(false);
        EmitPartsTypeChange();
        DumpEquipDataOnce(self);
        EnsureNoiseProbeArmed();
        SyncQuietStepPatch();

        std::uint32_t count = 0;
        std::uint16_t* arr = GetWornArr_SEH(self, kHolderOff_LifeRecovery, &count);
        if (!arr)
        {
            g_OrigUpdateLife(self);
            return;
        }
        if (count > kMaxSlots) count = kMaxSlots;

        const bool allowDonor =
            !g_DonorSwapFaulted.load(std::memory_order_relaxed)
            && !outfit::NativeSuitParamsActive();

        std::uint16_t saved[kMaxSlots] = {};
        bool swapped[kMaxSlots] = {};
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const SlotAbilities ab = GetSlotAbilities(i);
            if (!allowDonor || !ab.ok || ab.regen == 0)
                continue;
            const std::uint16_t donor = GetDonorEquipId(false, ab.regen);
            if (donor == 0)
                continue;
            std::uint16_t cur = 0;
            if (!ReadWorn_SEH(arr, i, &cur))
                continue;
            if (cur != kWornSuitNone && cur != 0)
                continue;
            if (WriteWorn_SEH(arr, i, donor))
            {
                saved[i]   = cur;
                swapped[i] = true;
#ifdef _DEBUG
                static std::atomic<int> s_log{ 0 };
                if (s_log.fetch_add(1) < 8)
                    LogDebug("[OutfitAbilities] regen donor served: slot=%u "
                        "equipId %u for recovery tick (was 0x%04X, restored "
                        "after)\n",
                        i, static_cast<unsigned>(donor),
                        static_cast<unsigned>(cur));
#endif
            }
        }

        const bool origOk = CallOrigUpdateLife_SEH(self);

        {
            static std::atomic<int> s_proof{ 0 };
            static float s_lastDef = -1.0f, s_lastReg = -1.0f;
            float def = 0.0f, reg = 0.0f;
            const std::uint32_t li = outfit::GetLocalPartsSlot();
            if (ReadSkillParams_SEH(self, li, &def, &reg)
                && (def != s_lastDef || reg != s_lastReg)
                && s_proof.fetch_add(1, std::memory_order_relaxed) < 24)
            {
                s_lastDef = def;
                s_lastReg = reg;
                const SlotAbilities ab = GetSlotAbilities(li);
                Log("[AbilityProof] engine GetSkillParam(player %u): "
                    "damageRate(0x12)=%.4f lifeRecovery(0x10)=%.4f | outfit "
                    "declares defense=%u lifeRecovery=%u. Vanilla with no suit "
                    "reads 1.0000/1.0000; BATTLEDRESS lv1-9 damageRate runs "
                    "0.80 0.70 0.60 0.60 0.60 0.60 0.525 0.45 0.375 and "
                    "SNEAKING_SUIT lv1-9 lifeRecovery runs 1.2 1.4 then 1.6\n",
                    li, def, reg,
                    static_cast<unsigned>(ab.defense),
                    static_cast<unsigned>(ab.regen));
            }
        }

        for (std::uint32_t i = 0; i < count; ++i)
            if (swapped[i])
                WriteWorn_SEH(arr, i, saved[i]);

        if (!origOk && !g_DonorSwapFaulted.exchange(true))
            Log("[OutfitAbilities] the engine faulted in UpdateLifeRecoverySpeed "
                "with the donor suit equipId swapped in - that donor is "
                "unresolvable right after a mission or suit change. Tick skipped, "
                "worn array restored, both donors parked until the next outfit "
                "change\n");
    }

    static std::uint64_t __fastcall hkCalcDamageValueAtIndex(
        void* self, std::uint32_t slotIndex, void* a3, void* a4, void* a5,
        void* a6, void* a7, void* a8, void* a9)
    {
        std::uint16_t* arr = nullptr;
        std::uint16_t saved = 0;
        bool swapped = false;

        if (!MissionCodeGuard::ShouldBypassHooks() && slotIndex < kMaxSlots
            && !g_DonorSwapFaulted.load(std::memory_order_relaxed)
            && !outfit::NativeSuitParamsActive())
        {
            const SlotAbilities ab = GetSlotAbilities(slotIndex);
            if (ab.ok && ab.defense != 0)
            {
                const std::uint16_t donor = GetDonorEquipId(true, ab.defense);
                if (donor != 0)
                {
                    arr = GetWornArr_SEH(self, kHolderOff_CalcDamage, nullptr);
                    std::uint16_t cur = 0;
                    if (arr && ReadWorn_SEH(arr, slotIndex, &cur)
                        && cur != donor
                        && (cur == kWornSuitNone || cur == 0)
                        && WriteWorn_SEH(arr, slotIndex, donor))
                    {
                        saved   = cur;
                        swapped = true;
#ifdef _DEBUG
                        static std::atomic<int> s_log{ 0 };
                        if (s_log.fetch_add(1) < 8)
                            LogDebug("[OutfitAbilities] defense donor served: "
                                "slot=%u equipId %u for damage calc\n",
                                slotIndex, static_cast<unsigned>(donor));
#endif
                    }
                }
            }
        }

        const std::uint64_t r = g_OrigCalcDamage(
            self, slotIndex, a3, a4, a5, a6, a7, a8, a9);

        {
            static std::atomic<int> s_dmgProof{ 0 };
            if (s_dmgProof.load(std::memory_order_relaxed) < 8)
            {
                float c3 = 0.0f, c4 = 0.0f;
                const bool ok3 =
                    ReadFloatAt_SEH(a3, kLifeState_OffDamageRate, &c3);
                const bool ok4 =
                    ReadFloatAt_SEH(a4, kLifeState_OffDamageRate, &c4);
                if (ok3 || ok4)
                {
                    s_dmgProof.fetch_add(1, std::memory_order_relaxed);
                    Log("[AbilityProof] damage calc slot=%u returned=%llu | "
                        "lifeState+0x28 candidates a3=%.4f a4=%.4f - the engine "
                        "multiplies the damage by this rate, so a value below "
                        "1.0 IS the defense reduction being applied\n",
                        slotIndex, static_cast<unsigned long long>(r),
                        ok3 ? c3 : -1.0f, ok4 ? c4 : -1.0f);
                }
            }
        }

        if (swapped)
            WriteWorn_SEH(arr, slotIndex, saved);
        return r;
    }

    static std::uint32_t ResolveDamageSeClass()
    {
        const std::uint8_t parts  = outfit::ReadLivePartsType();
        const std::uint8_t livePT = outfit::ReadLivePlayerType();
        std::uint32_t want = outfit::kSoundSwitchUnset;

        if (parts >= outfit::kCustomPartsTypeStart
            && parts <= outfit::kCustomPartsTypeEnd)
        {
            const outfit::OutfitEntry* e = nullptr;
            if (outfit::TryGetOutfitByPartsType(parts, &e) && e)
            {
                const outfit::OutfitPlayerTypeData* d = e->GetPTData(livePT);
                if (d) want = d->abilityDamageSe;
            }
            outfit::DeclaredAbilities rva{};
            if (outfit::TryGetVariantAbilities(
                    parts, livePT,
                    outfit::GetActiveVariantLockFree(parts), &rva)
                && rva.damageSe != outfit::kSoundSwitchUnset)
                want = rva.damageSe;
        }
        else if (outfit::VanillaExtHasAnyAbilities())
        {
            const std::uint8_t sel = outfit::ReadLiveSelectorCode();
            outfit::VanillaSuitAbilities va{};
            if (outfit::VanillaExtGetSuitAbilities(parts, livePT, sel, &va))
                want = va.damageSe;
            outfit::DeclaredAbilities vva{};
            if (VextActiveVariantAbilities(parts, livePT, sel, &vva)
                && vva.damageSe != outfit::kSoundSwitchUnset)
                want = vva.damageSe;
        }
        return want;
    }

    static bool DamageSeTargetCamo(std::uint32_t cls, std::uint8_t* out)
    {
        if (cls == outfit::kDamageSeBattleDress)
        {
            *out = kCamoByte_BattleDress;
            return true;
        }
        if (cls == outfit::kDamageSeDefault || cls == outfit::kDamageSeNormal)
        {
            *out = kCamoByte_Plain;
            return true;
        }
        static std::atomic<bool> s_warned{ false };
        if (!s_warned.exchange(true, std::memory_order_relaxed))
            Log("[OutfitAbilities] WARNING: damageSe resolves to 0x%08X, "
                "which is neither \"battledress\" nor \"default\" - the engine "
                "picks the damage reaction sound from a two-class gate and has "
                "no third class, so this outfit keeps the vanilla damage "
                "sound\n", cls);
        return false;
    }

    static void* LocalPlayerCamoArr_SEH(void* self, std::uint32_t slotIndex)
    {
        __try
        {
            std::uint8_t* ctl = reinterpret_cast<std::uint8_t*>(self);
            if (!ctl) return nullptr;
            std::uint8_t* owner =
                *reinterpret_cast<std::uint8_t**>(ctl + kDmgCtl_Owner);
            if (!owner) return nullptr;
            std::uint8_t* mgr =
                *reinterpret_cast<std::uint8_t**>(owner + kDmgOwner_PlayerMgr);
            if (!mgr) return nullptr;
            if (*reinterpret_cast<std::uint32_t*>(mgr + kPlayerMgr_LocalSlot)
                != slotIndex)
                return nullptr;
            std::uint8_t* parts =
                *reinterpret_cast<std::uint8_t**>(mgr + kPlayerMgr_Parts);
            if (!parts) return nullptr;
            return *reinterpret_cast<std::uint8_t**>(parts + kParts_CamoArr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static bool __fastcall hkCallDamageEffectWithReaction(
        void* self, std::uint32_t slotIndex, void* damage,
        std::uint8_t param3, bool param4, bool param5)
    {
        void* camoArr = nullptr;
        std::uint8_t saved = 0;
        bool swapped = false;

        if (!MissionCodeGuard::ShouldBypassHooks())
        {
            const std::uint32_t cls = ResolveDamageSeClass();
            std::uint8_t target = 0;
            if (cls != outfit::kSoundSwitchUnset
                && DamageSeTargetCamo(cls, &target))
            {
                camoArr = LocalPlayerCamoArr_SEH(self, slotIndex);
                bool ok = false;
                const std::uint8_t cur =
                    camoArr ? ReadSeByte_SEH(camoArr, slotIndex, &ok)
                            : std::uint8_t{ 0 };
                const bool needed = (target == kCamoByte_BattleDress)
                    ? (cur != kCamoByte_BattleDress)
                    : (cur == kCamoByte_BattleDress);
                if (ok && needed
                    && WriteSeByte_SEH(camoArr, slotIndex, target))
                {
                    saved   = cur;
                    swapped = true;
                }
            }
        }

        const bool r = g_OrigCallDamageEffect(self, slotIndex, damage, param3,
                                              param4, param5);

        if (swapped)
            WriteSeByte_SEH(camoArr, slotIndex, saved);
        return r;
    }

    static void* __fastcall hkConvertRattleSuitPlayer(
        void* self, void* retStorage, std::uint64_t stringId, void* p2,
        void* p3, void* p4, std::uint64_t p5, void* p6, void* seParam)
    {
        void* r = g_OrigConvertRattle(self, retStorage, stringId, p2, p3, p4,
                                      p5, p6, seParam);

        if (MissionCodeGuard::ShouldBypassHooks() || !seParam) return r;

        bool readOk = false;
        const std::uint8_t seParts =
            ReadSeByte_SEH(seParam, kSeParam_PartsTypeOff, &readOk);
        if (!readOk) return r;

        std::uint8_t parts = seParts;
        const std::uint32_t want = ResolveClothSwitch(seParts, &parts);
        if (want == outfit::kSoundSwitchUnset) return r;

        std::uint32_t vanilla = 0;
        const bool okVanilla = ReadOutParamPP_SEH(p4, &vanilla);
        if (okVanilla && vanilla == want) return r;
        if (!WriteOutParamPP_SEH(p4, want)) return r;

        static std::atomic<bool> s_proof{ false };
        if (!s_proof.exchange(true))
        {
            std::uint32_t tbl[4] = {};
            const bool okTbl = ReadClothRowTable_SEH(self, tbl);

            int known = -1;
            if (okTbl)
                for (int i = 0; i < 4; ++i)
                    if (tbl[i] == want) known = i;

            static const char* const kStockNames[4] =
                { " (nom)", " (bony)", " (snk)", " (amr)" };

            bool okGate = false;
            const std::uint8_t gate =
                ReadSeByte_SEH(seParam, kSeParam_FlagsOff, &okGate);

            Log("[OutfitAbilities] cloth foley: partsType 0x%02X drives "
                "player_type_switch value 0x%08X%s, replacing the 0x%08X the "
                "engine picked from fold index %d. The stock sound script "
                "registers four values at ConvertInfo+0x7A0 = "
                "[%08X %08X %08X %08X] = nom/bony/snk/amr; the suppress flag "
                "seParam+0x12 is 0x%02X (bit 0x02 set means the caller skips "
                "the conversion and no value can matter)\n",
                static_cast<unsigned>(parts), want,
                known >= 0 ? kStockNames[known] : " (custom name)",
                okVanilla ? vanilla : 0u, RattleFoldIndex(seParts),
                tbl[0], tbl[1], tbl[2], tbl[3],
                okGate ? static_cast<unsigned>(gate) : 0xFFu);

            if (okTbl && known < 0)
                Log("[OutfitAbilities] WARNING: rattleSuit resolves to "
                    "player_type_switch value 0x%08X, which is none of the "
                    "four the stock sound script registers - Wwise has no "
                    "container entry under that value, so the suit keeps its "
                    "default cloth foley until your own sound script adds that "
                    "name to SoundPlayerAnimEvent.SetupPlayerType\n", want);
        }
        return r;
    }

    static void __fastcall hkConvertFootStepPlayer(
        void* self, void* out, std::uint64_t stringId, void* actionOut,
        void** groups, void** values, std::uint32_t flags, void* p8,
        void* seParam, void* p10)
    {
        g_OrigConvertFootStep(self, out, stringId, actionOut, groups, values,
                              flags, p8, seParam, p10);

        if (MissionCodeGuard::ShouldBypassHooks() || !seParam || !values)
            return;

        bool readOk = false;
        const std::uint8_t seParts =
            ReadSeByte_SEH(seParam, kSeParam_PartsTypeOff, &readOk);
        if (!readOk) return;

        std::uint8_t parts = seParts;
        const std::uint32_t want = ResolveClothSwitch(seParts, &parts);
        if (want == outfit::kSoundSwitchUnset) return;

        std::uint32_t vanilla = 0;
        const bool okVanilla = ReadOutParamPP_SEH(values, &vanilla);
        if (okVanilla && vanilla == want) return;
        if (!WriteOutParamPP_SEH(values, want)) return;

        static std::atomic<bool> s_told{ false };
        if (!s_told.exchange(true))
            Log("[OutfitAbilities] cloth foley on footsteps: parts type 0x%02X "
                "now drives player_type_switch value 0x%08X here too, replacing "
                "the 0x%08X the footstep converter picked. Each anim event posts "
                "on its own sound object and sets that object's own switches, so "
                "this does not rescue the suit rattle - it is what makes the "
                "FOOTSTEP sounds use the outfit's cloth class instead of the "
                "vanilla one\n",
                static_cast<unsigned>(parts), want,
                okVanilla ? vanilla : 0u);
    }

    static bool DumpAndVerifyPrologue(const char* what, std::uintptr_t addr,
                                      const std::uint8_t* expect,
                                      std::size_t expectLen)
    {
        std::uint8_t bytes[32] = {};
        if (expectLen > sizeof(bytes))
        {
            Log("[OutfitAbilities] %s @ 0x%llX declares a %u-byte prologue but "
                "only %u bytes can be read - hook skipped rather than armed on "
                "a partial match\n",
                what, static_cast<unsigned long long>(addr),
                static_cast<unsigned>(expectLen),
                static_cast<unsigned>(sizeof(bytes)));
            return false;
        }
        const std::size_t readLen = expectLen > 20 ? expectLen : 20;
        bool readOk = false;
        __try
        {
            std::memcpy(bytes, reinterpret_cast<const void*>(addr), readLen);
            readOk = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            readOk = false;
        }
        if (!readOk)
        {
            Log("[OutfitAbilities] %s @ 0x%llX unreadable - hook skipped\n",
                what, static_cast<unsigned long long>(addr));
            return false;
        }
        LogDebug("[OutfitAbilities] %s @ 0x%llX prologue: %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X\n",
            what, static_cast<unsigned long long>(addr),
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
            bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15], bytes[16], bytes[17],
            bytes[18], bytes[19]);
        if (expect && std::memcmp(bytes, expect, expectLen) != 0)
        {
            Log("[OutfitAbilities] %s @ 0x%llX prologue mismatch - build drift, "
                "hook skipped\n",
                what, static_cast<unsigned long long>(addr));
            return false;
        }
        return true;
    }

    constexpr std::uintptr_t kAddr_NoiseAddThunks[] =
        { 0x140515270ull, 0x140514fb0ull };
    constexpr std::uintptr_t kAddr_NoiseVtableAddSlots[] =
        { 0x1421876a8ull, 0x1421876f8ull };
    constexpr std::uintptr_t kAddr_NoiseAddThunk      = kAddr_NoiseAddThunks[0];
    constexpr std::uintptr_t kAddr_NoiseVtableAddSlot =
        kAddr_NoiseVtableAddSlots[0];
    constexpr std::uintptr_t kNoiseScanBase          = 0x140400000ull;
    constexpr std::size_t    kNoiseScanSize          = 0x400000;
    constexpr std::size_t    kNoiseScanChunk         = 0x10000;

    constexpr std::uint8_t kPrologue_AddNoiseBody[] =
        { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
          0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };

    static std::atomic<std::uint32_t> g_NoisePrologueHits{ 0 };
    static std::atomic<std::uint32_t> g_NoiseLiveDisp{ 0 };

    constexpr std::uintptr_t kPlayerStepNoiseFnBegin = 0x140984010ull;
    constexpr std::uintptr_t kPlayerStepNoiseFnEnd   = 0x140984ae0ull;

    static bool ReadBytes_SEH(std::uintptr_t addr, void* out,
                              std::size_t n);

    constexpr std::uint16_t kStepNoiseQuietJzOrig = 0x0274;
    constexpr std::uint16_t kStepNoiseQuietJzNop  = 0x9090;

    constexpr std::uint8_t kStepNoiseQuietPrefix[] =
        { 0x44, 0x38, 0x7C, 0x24, 0x28 };
    constexpr std::uint8_t kStepNoiseQuietSuffix[] = { 0xFF, 0xCE };
    constexpr std::size_t  kStepNoiseQuietWindow   = 9;

    constexpr std::uintptr_t kQuietScanBase  = 0x140900000ull;
    constexpr std::size_t    kQuietScanSize  = 0x100000;
    constexpr std::size_t    kQuietScanChunk = 0x10000;

    static std::atomic<std::uintptr_t> g_QuietCmpAddr{ 0 };

    static bool QuietWindowMatches(std::uintptr_t cmpAddr,
                                   std::uint16_t expectMid)
    {
        std::uint8_t w[kStepNoiseQuietWindow] = {};
        if (!ReadBytes_SEH(cmpAddr, w, sizeof(w)))
            return false;
        if (std::memcmp(w, kStepNoiseQuietPrefix,
                        sizeof(kStepNoiseQuietPrefix)) != 0)
            return false;
        if (std::memcmp(w + 7, kStepNoiseQuietSuffix,
                        sizeof(kStepNoiseQuietSuffix)) != 0)
            return false;
        std::uint16_t mid = 0;
        std::memcpy(&mid, w + 5, sizeof(mid));
        return mid == expectMid;
    }

    static std::uint8_t g_QuietScanBuf[kQuietScanChunk];

    static std::uintptr_t ScanForQuietStepSite()
    {
        std::uintptr_t hit   = 0;
        std::uint32_t  hits  = 0;

        for (std::size_t off = 0; off < kQuietScanSize;
             off += kQuietScanChunk - kStepNoiseQuietWindow)
        {
            const std::size_t want =
                (kQuietScanSize - off < kQuietScanChunk)
                    ? (kQuietScanSize - off) : kQuietScanChunk;
            const std::uintptr_t base = kQuietScanBase + off;
            if (!ReadBytes_SEH(base, g_QuietScanBuf, want))
                continue;
            if (want < kStepNoiseQuietWindow)
                continue;

            for (std::size_t i = 0; i + kStepNoiseQuietWindow <= want; ++i)
            {
                if (g_QuietScanBuf[i]     != 0x44 ||
                    g_QuietScanBuf[i + 5] != 0x74 ||
                    g_QuietScanBuf[i + 6] != 0x02)
                    continue;
                if (std::memcmp(g_QuietScanBuf + i, kStepNoiseQuietPrefix,
                                sizeof(kStepNoiseQuietPrefix)) != 0)
                    continue;
                if (std::memcmp(g_QuietScanBuf + i + 7, kStepNoiseQuietSuffix,
                                sizeof(kStepNoiseQuietSuffix)) != 0)
                    continue;

                const std::uintptr_t found = base + i;
                if (hits == 0)
                {
                    hit  = found;
                    hits = 1;
                }
                else if (found != hit)
                {
                    return 0;
                }
            }
        }
        return hit;
    }

    static std::uintptr_t ResolveQuietStepSite()
    {
        const std::uintptr_t cached =
            g_QuietCmpAddr.load(std::memory_order_relaxed);
        if (cached)
            return cached;

        const std::uintptr_t mapped =
            gAddr.SoundPlayerAnimEvent_StepNoiseQuietCmp;
        if (mapped && QuietWindowMatches(mapped, kStepNoiseQuietJzOrig))
        {
            g_QuietCmpAddr.store(mapped, std::memory_order_relaxed);
            return mapped;
        }

        const std::uintptr_t found = ScanForQuietStepSite();
        if (found)
        {
            g_QuietCmpAddr.store(found, std::memory_order_relaxed);
            Log("[OutfitAbilities] silentFootsteps: the address set carries %s for "
                "StepNoiseQuietCmp, so the CMP/JZ/DEC ESI signature was scanned "
                "from 0x%llX+0x%X and found at 0x%llX - put that in the address set "
                "to skip the scan\n",
                mapped ? "an address that no longer matches" : "no address",
                static_cast<unsigned long long>(kQuietScanBase),
                static_cast<unsigned>(kQuietScanSize),
                static_cast<unsigned long long>(found));
        }
        return found;
    }

    static std::atomic<bool> g_QuietStepPatched{ false };
    static std::atomic<bool> g_QuietStepRefused{ false };

    static bool WriteQuietStepWord(std::uintptr_t jzAddr, std::uint16_t want)
    {
        auto* p = reinterpret_cast<std::uint16_t*>(jzAddr);
        DWORD prot = 0;
        if (!VirtualProtect(p, sizeof(*p), PAGE_EXECUTE_READWRITE, &prot))
            return false;
        bool ok = false;
        __try
        {
            *p = want;
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        DWORD back = 0;
        VirtualProtect(p, sizeof(*p), prot, &back);
        FlushInstructionCache(GetCurrentProcess(), p, sizeof(*p));
        return ok;
    }

    static void SetQuietStepPatch(bool on)
    {
        if (g_QuietStepRefused.load(std::memory_order_relaxed))
            return;
        if (g_QuietStepPatched.load(std::memory_order_relaxed) == on)
            return;

        const std::uintptr_t cmpAddr = ResolveQuietStepSite();
        if (!cmpAddr)
        {
            if (!g_QuietStepRefused.exchange(true, std::memory_order_relaxed))
                Log("[OutfitAbilities] silentFootsteps REFUSED: the CMP/JZ/DEC ESI "
                    "signature is not at the EN day3900 address and was not unique "
                    "in 0x%llX+0x%X - nothing patched\n",
                    static_cast<unsigned long long>(kQuietScanBase),
                    static_cast<unsigned>(kQuietScanSize));
            return;
        }

        const std::uintptr_t jzAddr = cmpAddr + 5;
        const std::uint16_t  expect = on ? kStepNoiseQuietJzOrig
                                         : kStepNoiseQuietJzNop;
        if (!QuietWindowMatches(cmpAddr, expect))
        {
            if (!g_QuietStepRefused.exchange(true, std::memory_order_relaxed))
                Log("[OutfitAbilities] silentFootsteps REFUSED: the window at "
                    "0x%llX no longer reads as CMP/JZ/DEC ESI with 0x%04X in the "
                    "branch slot - left untouched\n",
                    static_cast<unsigned long long>(cmpAddr), expect);
            return;
        }

        if (!WriteQuietStepWord(jzAddr, on ? kStepNoiseQuietJzNop
                                           : kStepNoiseQuietJzOrig))
            return;

        g_QuietStepPatched.store(on, std::memory_order_relaxed);
        Log("[OutfitAbilities] silentFootsteps %s: the engine's one-level-quieter "
            "branch at 0x%llX is now %s - crouch falls to level 1 and is skipped, "
            "standing and running stay audible\n",
            on ? "ON" : "OFF",
            static_cast<unsigned long long>(jzAddr),
            on ? "always taken" : "restored to vanilla");
    }

    static void SyncQuietStepPatch()
    {
        bool want = false;
        if (!MissionCodeGuard::ShouldBypassHooks())
        {
            const SlotAbilities ab =
                GetSlotAbilities(outfit::GetLocalPartsSlot());
            want = ab.ok && ab.silent;
        }
        SetQuietStepPatch(want);
    }

    using AddNoise_t = std::uint64_t (__fastcall*)(
        void*, void*, void*, void*, void*, void*, void*, void*);

    static AddNoise_t     g_OrigAddNoise      = nullptr;
    static std::uintptr_t g_ChosenAddNoise    = 0;
    static bool           g_InstalledAddNoise = false;

    static bool ReadBytes_SEH(std::uintptr_t addr, void* out, std::size_t n)
    {
        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(addr), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool BodyIsAddNoise(std::uintptr_t fn)
    {
        std::uint8_t b[26] = {};
        if (!ReadBytes_SEH(fn, b, sizeof(b)))
            return false;
        if (std::memcmp(b, kPrologue_AddNoiseBody,
                        sizeof(kPrologue_AddNoiseBody)) != 0)
            return false;

        g_NoisePrologueHits.fetch_add(1, std::memory_order_relaxed);

        if (b[20] != 0x8B || b[21] != 0xB9)
            return false;
        std::uint32_t disp = 0;
        std::memcpy(&disp, b + 22, sizeof(disp));
        if (disp < 0x1000u || disp > 0x20000u)
            return false;

        g_NoiseLiveDisp.store(disp, std::memory_order_relaxed);
        return true;
    }

    static bool ThunkTargetIsAddNoise(std::uintptr_t thunk,
                                      std::uintptr_t* outTarget)
    {
        std::uint8_t j[5] = {};
        if (!ReadBytes_SEH(thunk, j, sizeof(j)) || j[0] != 0xE9)
            return false;
        std::int32_t rel = 0;
        std::memcpy(&rel, j + 1, sizeof(rel));
        const std::uintptr_t target =
            static_cast<std::uintptr_t>(thunk + 5 + static_cast<std::intptr_t>(rel));
        if (!BodyIsAddNoise(target))
            return false;
        if (outTarget)
            *outTarget = target;
        return true;
    }

    static std::uint8_t g_NoiseScanBuf[kNoiseScanChunk];

    static bool VtableSlotIsAddNoise(std::uintptr_t slot, std::uintptr_t* outFn)
    {
        std::uintptr_t fn = 0;
        if (!ReadBytes_SEH(slot, &fn, sizeof(fn)))
            return false;
        if (fn < 0x140000000ull || fn > 0x150000000ull)
            return false;
        if (BodyIsAddNoise(fn) || ThunkTargetIsAddNoise(fn, nullptr))
        {
            *outFn = fn;
            return true;
        }
        return false;
    }

    static void DumpStepNoiseGateOnce()
    {
        static std::atomic<bool> s_done{ false };
        if (s_done.exchange(true, std::memory_order_relaxed))
            return;

        constexpr std::uintptr_t kGateDumpBase = 0x140984960ull;
        constexpr std::size_t    kGateDumpSize = 0xD0;

        std::uint8_t b[kGateDumpSize] = {};
        if (!ReadBytes_SEH(kGateDumpBase, b, sizeof(b)))
        {
            Log("[OutfitAbilities] step-noise gate dump: 0x%llX unreadable\n",
                static_cast<unsigned long long>(kGateDumpBase));
            return;
        }

        for (std::size_t row = 0; row < kGateDumpSize; row += 16)
        {
            char line[128];
            int  n = 0;
            for (std::size_t i = 0; i < 16 && (row + i) < kGateDumpSize; ++i)
                n += std::snprintf(line + n, sizeof(line) - n, "%02X ",
                                   b[row + i]);
            Log("[OutfitAbilities] gate %llX: %s\n",
                static_cast<unsigned long long>(kGateDumpBase + row), line);
        }
        Log("[OutfitAbilities] step-noise gate dump above covers the two jumps at "
            "0x1409849C3/0x1409849C8 that skip AddNoise\n");
    }

    static std::uintptr_t FindNoiseAddThunk()
    {
        std::uintptr_t target = 0;
        for (std::uintptr_t thunk : kAddr_NoiseAddThunks)
        {
            if (!ThunkTargetIsAddNoise(thunk, &target))
                continue;
            Log("[OutfitAbilities] noise AddNoise thunk @ 0x%llX -> body "
                "0x%llX (ring-count offset 0x%X)\n",
                static_cast<unsigned long long>(thunk),
                static_cast<unsigned long long>(target),
                g_NoiseLiveDisp.load(std::memory_order_relaxed));
            return thunk;
        }

        for (std::uintptr_t slot : kAddr_NoiseVtableAddSlots)
        {
            std::uintptr_t viaVtable = 0;
            if (!VtableSlotIsAddNoise(slot, &viaVtable))
                continue;
            Log("[OutfitAbilities] noise AddNoise via vtable slot @ 0x%llX -> "
                "0x%llX (ring-count offset 0x%X)\n",
                static_cast<unsigned long long>(slot),
                static_cast<unsigned long long>(viaVtable),
                g_NoiseLiveDisp.load(std::memory_order_relaxed));
            return viaVtable;
        }

        for (std::size_t off = 0; off < kNoiseScanSize; off += kNoiseScanChunk)
        {
            const std::size_t n =
                (kNoiseScanSize - off < kNoiseScanChunk)
                    ? (kNoiseScanSize - off) : kNoiseScanChunk;
            const std::uintptr_t chunkBase = kNoiseScanBase + off;
            if (!ReadBytes_SEH(chunkBase, g_NoiseScanBuf, n))
                continue;
            for (std::size_t i = 0; i + 5 <= n; ++i)
            {
                if (g_NoiseScanBuf[i] != 0xE9)
                    continue;
                std::int32_t rel = 0;
                std::memcpy(&rel, g_NoiseScanBuf + i + 1, sizeof(rel));
                const std::uintptr_t cand = chunkBase + i;
                const std::uintptr_t tgt =
                    cand + 5 + static_cast<std::intptr_t>(rel);
                if (tgt < 0x140000000ull || tgt > 0x150000000ull)
                    continue;
                if (!BodyIsAddNoise(tgt))
                    continue;
                target = tgt;
                Log("[OutfitAbilities] noise AddNoise RELOCATED: thunk @ 0x%llX -> "
                    "body 0x%llX (documented 0x%llX did not match)\n",
                    static_cast<unsigned long long>(cand),
                    static_cast<unsigned long long>(target),
                    static_cast<unsigned long long>(kAddr_NoiseAddThunk));
                return cand;
            }
        }

        Log("[OutfitAbilities] noise AddNoise not found (thunk 0x%llX, vtable slot "
            "0x%llX, 0x%llX+0x%X scan): %u thunk target(s) matched the prologue but "
            "none had the expected [RCX+disp] ring-count load, live disp=0x%X. A "
            "prologue count of 0 means the band is not decrypted yet\n",
            static_cast<unsigned long long>(kAddr_NoiseAddThunk),
            static_cast<unsigned long long>(kAddr_NoiseVtableAddSlot),
            static_cast<unsigned long long>(kNoiseScanBase),
            static_cast<unsigned>(kNoiseScanSize),
            g_NoisePrologueHits.load(std::memory_order_relaxed),
            g_NoiseLiveDisp.load(std::memory_order_relaxed));
        return 0;
    }

    static std::uint64_t __fastcall hkAddNoise(
        void* self, void* desc, void* a3, void* a4, void* a5, void* a6,
        void* a7, void* a8);

    static void EnsureNoiseProbeArmed()
    {
        static std::atomic<int>   s_attempts{ 0 };
        static std::atomic<DWORD> s_lastTry{ 0 };

        if (g_InstalledAddNoise)
            return;
        const int n = s_attempts.load(std::memory_order_relaxed);
        if (n >= 10)
            return;

        const DWORD now  = GetTickCount();
        const DWORD last = s_lastTry.load(std::memory_order_relaxed);
        if (last != 0 && (now - last) < 2000)
            return;
        s_lastTry.store(now, std::memory_order_relaxed);
        s_attempts.fetch_add(1, std::memory_order_relaxed);

        g_NoisePrologueHits.store(0, std::memory_order_relaxed);

        const std::uintptr_t at = FindNoiseAddThunk();
        if (!at)
            return;

        g_ChosenAddNoise = at;
        g_InstalledAddNoise = CreateAndEnableHook(
            reinterpret_cast<void*>(at),
            reinterpret_cast<void*>(&hkAddNoise),
            reinterpret_cast<void**>(&g_OrigAddNoise));
        Log("[OutfitAbilities] noise probe armed in gameplay at 0x%llX (attempt "
            "%d): hook=%s - next 48 AI-noise emissions logged\n",
            static_cast<unsigned long long>(at), n + 1,
            g_InstalledAddNoise ? "OK" : "MinHook refused");

        DumpStepNoiseGateOnce();
    }

    static void LogNoiseSample(void* desc, void* ra)
    {
        static std::atomic<int> s_count{ 0 };
        const int n = s_count.fetch_add(1, std::memory_order_relaxed);
        if (n >= 48)
            return;

        std::uint8_t d[64] = {};
        if (!ReadBytes_SEH(reinterpret_cast<std::uintptr_t>(desc), d, sizeof(d)))
            return;

        float f[8] = {};
        std::memcpy(f, d, sizeof(f));

        std::uint32_t g1 = 0xFFFFFFFFu;
        std::uint32_t g2 = 0xFFFFFFFFu;
        ReadBytes_SEH(0x142178248ull, &g1, sizeof(g1));
        ReadBytes_SEH(0x142c00a20ull, &g2, sizeof(g2));

        Log("[NoiseProbe] #%d ra=0x%llX parts=0x%02X pos=(%.2f %.2f %.2f) f3=%.3f "
            "f4=%.3f | gateSrc 142178248=0x%08X 142C00A20=0x%08X (read right before "
            "the engine's skip-noise gates)\n",
            n, static_cast<unsigned long long>(
                   reinterpret_cast<std::uintptr_t>(ra)),
            static_cast<unsigned>(outfit::ReadLivePartsType()),
            f[0], f[1], f[2], f[3], f[4], g1, g2);
    }

    static std::uint64_t __fastcall hkAddNoise(
        void* self, void* desc, void* a3, void* a4, void* a5, void* a6,
        void* a7, void* a8)
    {
        const std::uintptr_t ra =
            reinterpret_cast<std::uintptr_t>(_ReturnAddress());

        LogNoiseSample(desc, reinterpret_cast<void*>(ra));

        return g_OrigAddNoise(self, desc, a3, a4, a5, a6, a7, a8);
    }

    static std::uintptr_t g_ChosenCalcDamage = 0;
    static std::uintptr_t g_ChosenUpdateLife = 0;
    static std::uintptr_t g_ChosenRattle     = 0;
    static std::uintptr_t g_ChosenFootStep   = 0;
    static std::uintptr_t g_ChosenDamageEffect = 0;

    template <std::size_t N>
    static std::uintptr_t InstallFromCandidates(
        const char* what, const HookCandidate (&candidates)[N], void* detour,
        void** original)
    {
        for (const HookCandidate& c : candidates)
        {
            if (!DumpAndVerifyPrologue(what, c.addr, c.expect, c.expectLen))
                continue;
            if (CreateAndEnableHook(reinterpret_cast<void*>(c.addr), detour,
                                    original))
                return c.addr;
            Log("[OutfitAbilities] %s @ 0x%llX prologue OK but MinHook "
                "refused - trying next candidate\n",
                what, static_cast<unsigned long long>(c.addr));
        }
        return 0;
    }
}

namespace outfit
{
    void NoteOwnSuitOverwrittenByPin(std::uint8_t ownParts, std::uint8_t sel,
                                     std::uint8_t pt)
    {
        static std::atomic<int> s_logged{ 0 };
        if (s_logged.fetch_add(1, std::memory_order_relaxed) >= 8) return;

        Log("[AbilityProbe] %s was restored by the engine as parts type 0x%02X "
            "camo 0x%02X on player type %u, and the unique-character pin is "
            "about to put the remembered custom outfit back over it in the same "
            "frame - that is why an own-suit sample can be missing from a "
            "restore or re-stream, while a deliberate own-suit pick from the "
            "menu clears the memory first and does show up\n",
            pt == kPlayerType_Ocelot ? "Ocelot's own suit" : "Quiet's own suit",
            static_cast<unsigned>(ownParts), static_cast<unsigned>(sel),
            static_cast<unsigned>(pt));
    }

    void Reset_PartsTypeChangeTracking()
    {
        g_LastIdentity.store(kIdentityNone, std::memory_order_relaxed);
        g_AwaitingSettle.store(false, std::memory_order_relaxed);
        g_UnsettledLines.store(0, std::memory_order_relaxed);
    }

    bool Install_OutfitAbilities_Hooks()
    {
        const bool isEn = gAddr.GetQuarkSystemTable
                            == kSentinel_GetQuarkSystemTable;
        const bool isJp = gAddr.GetQuarkSystemTable
                            == kSentinel_GetQuarkSystemTableJp;
        if (!isEn && !isJp)
        {
            Log("[OutfitAbilities] address set is not the 1.0.15.4 family "
                "(GetQuarkSystemTable=0x%llX) - outfit abilities disabled\n",
                static_cast<unsigned long long>(gAddr.GetQuarkSystemTable));
            return false;
        }
        g_AddressSetIsEn.store(isEn, std::memory_order_relaxed);
        if (!isEn)
            Log("[OutfitAbilities] JAPANESE address set "
                "(GetQuarkSystemTable=0x%llX) - only self-locating abilities run: "
                "silentFootsteps scans for its site and the three hooks verify "
                "prologues. The suit-donor table and infinite-ammo byte are EN-only "
                "raw addresses, so defense/lifeRecovery/infiniteAmmo stay off\n",
                static_cast<unsigned long long>(gAddr.GetQuarkSystemTable));

        g_ChosenDamageEffect = InstallFromCandidates(
            "CallDamageEffectWithReaction", kCandidates_DamageEffect,
            reinterpret_cast<void*>(&hkCallDamageEffectWithReaction),
            reinterpret_cast<void**>(&g_OrigCallDamageEffect));
        g_InstalledDamageEffect = g_ChosenDamageEffect != 0;
        if (!g_InstalledDamageEffect)
            Log("[OutfitAbilities] the damage reaction could not be hooked - "
                "damageSe is ignored, so every outfit keeps the damage sound "
                "the engine picks from its camo type\n");

        g_ChosenCalcDamage = InstallFromCandidates(
            "CalculateDamageValueAtIndex", kCandidates_CalcDamage,
            reinterpret_cast<void*>(&hkCalcDamageValueAtIndex),
            reinterpret_cast<void**>(&g_OrigCalcDamage));
        g_InstalledCalcDamage = g_ChosenCalcDamage != 0;

        g_ChosenUpdateLife = InstallFromCandidates(
            "UpdateLifeRecoverySpeed", kCandidates_UpdateLife,
            reinterpret_cast<void*>(&hkUpdateLifeRecoverySpeed),
            reinterpret_cast<void**>(&g_OrigUpdateLife));
        g_InstalledUpdateLife = g_ChosenUpdateLife != 0;

        g_ChosenRattle = InstallFromCandidates(
            "ConvertRattleSuit(player)", kCandidates_Rattle,
            reinterpret_cast<void*>(&hkConvertRattleSuitPlayer),
            reinterpret_cast<void**>(&g_OrigConvertRattle));
        g_InstalledRattle = g_ChosenRattle != 0;

        g_ChosenFootStep = InstallFromCandidates(
            "ConvertFootStep(player)", kCandidates_FootStep,
            reinterpret_cast<void*>(&hkConvertFootStepPlayer),
            reinterpret_cast<void**>(&g_OrigConvertFootStep));
        g_InstalledFootStep = g_ChosenFootStep != 0;
        if (!g_InstalledFootStep)
            Log("[OutfitAbilities] the footstep cloth converter could not be "
                "hooked - footstep sounds keep the vanilla cloth class for this "
                "outfit while the suit rattle still uses the declared one, so "
                "the two will not match\n");

        Log("[OutfitAbilities] installed: damage=%s@0x%llX lifeRecovery=%s@0x%llX "
            "rattle=%s@0x%llX footstep=%s@0x%llX damageSe=%s@0x%llX "
            "(donors swap in and restore around the hooked calls; "
            "silentFootsteps forces the engine's quieter movement branch; "
            "damageSe swaps the camo byte the reaction sound is chosen from; "
            "infinite-ammo heads sync condition bit 0x4)\n",
            g_InstalledCalcDamage ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenCalcDamage),
            g_InstalledUpdateLife ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenUpdateLife),
            g_InstalledRattle     ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenRattle),
            g_InstalledFootStep   ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenFootStep),
            g_InstalledDamageEffect ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenDamageEffect));

        return g_InstalledCalcDamage || g_InstalledUpdateLife
            || g_InstalledRattle || g_InstalledFootStep
            || g_InstalledDamageEffect;
    }

    void Uninstall_OutfitAbilities_Hooks()
    {
        if (g_AmmoBitOwned.exchange(false, std::memory_order_relaxed))
            RmwAmmoBit_SEH(false);

        SetQuietStepPatch(false);

        if (g_InstalledAddNoise)
        {
            DisableAndRemoveHook(reinterpret_cast<void*>(g_ChosenAddNoise));
            g_OrigAddNoise      = nullptr;
            g_InstalledAddNoise = false;
            g_ChosenAddNoise    = 0;
        }
        if (g_InstalledFootStep)
        {
            DisableAndRemoveHook(reinterpret_cast<void*>(g_ChosenFootStep));
            g_OrigConvertFootStep = nullptr;
            g_InstalledFootStep   = false;
            g_ChosenFootStep      = 0;
        }

        if (g_InstalledRattle)
        {
            DisableAndRemoveHook(reinterpret_cast<void*>(g_ChosenRattle));
            g_OrigConvertRattle = nullptr;
            g_InstalledRattle   = false;
            g_ChosenRattle      = 0;
        }
        if (g_InstalledUpdateLife)
        {
            DisableAndRemoveHook(reinterpret_cast<void*>(g_ChosenUpdateLife));
            g_OrigUpdateLife      = nullptr;
            g_InstalledUpdateLife = false;
            g_ChosenUpdateLife    = 0;
        }
        if (g_InstalledDamageEffect)
        {
            DisableAndRemoveHook(
                reinterpret_cast<void*>(g_ChosenDamageEffect));
            g_OrigCallDamageEffect  = nullptr;
            g_InstalledDamageEffect = false;
            g_ChosenDamageEffect    = 0;
        }
        if (g_InstalledCalcDamage)
        {
            DisableAndRemoveHook(reinterpret_cast<void*>(g_ChosenCalcDamage));
            g_OrigCalcDamage      = nullptr;
            g_InstalledCalcDamage = false;
            g_ChosenCalcDamage    = 0;
        }
#ifdef _DEBUG
        LogDebug("[OutfitAbilities] removed\n");
#endif
    }
}
