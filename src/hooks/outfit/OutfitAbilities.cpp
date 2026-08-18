#include "pch.h"

#include "OutfitAbilities.h"
#include "OutfitRegistry.h"
#include "CustomHeadRegistry.h"
#include "ShadowState.h"
#include "MissionCodeGuard.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaBroadcaster.h"

namespace
{
    constexpr std::uintptr_t kSentinel_GetQuarkSystemTable = 0x140bff050ull;
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
    constexpr std::uint16_t kWornSuitNone         = 0xFFFF;
    constexpr std::size_t kMaxSlots = outfit::shadow::kMaxSlots;

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

    using CalcDamage_t = std::uint64_t (__fastcall*)(
        void*, std::uint32_t, void*, void*, void*, void*, void*, void*, void*);
    using UpdateLife_t = void (__fastcall*)(void*);
    using ConvertRattle_t = void* (__fastcall*)(
        void*, void*, std::uint64_t, void*, void*, void*, std::uint64_t, void*,
        void*);

    static CalcDamage_t    g_OrigCalcDamage    = nullptr;
    static UpdateLife_t    g_OrigUpdateLife    = nullptr;
    static ConvertRattle_t g_OrigConvertRattle = nullptr;
    static bool g_InstalledCalcDamage = false;
    static bool g_InstalledUpdateLife = false;
    static bool g_InstalledRattle     = false;

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
                Log("[OutfitAbilities] suit table @ 0x%llX REJECTED (only %u "
                    "of %u rows sit in the id band %u..%u with camo<=130 "
                    "level<=10) - defense/lifeRecovery donors disabled this "
                    "session\n",
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

    static std::uint16_t GetDonorEquipId(bool battleDress, std::uint8_t level)
    {
        if (level == 0) return 0;
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

    static SlotAbilities GetSlotAbilities(std::size_t slot)
    {
        SlotAbilities r{};
        outfit::shadow::Slot s;
        if (!outfit::shadow::Get(slot, &s))
            return r;
        const outfit::OutfitEntry* e = nullptr;
        if (!outfit::TryGetOutfitByPartsType(s.realPartsType, &e) || !e)
            return r;
        const outfit::OutfitPlayerTypeData* d = e->GetPTData(s.realPlayerType);
        if (!d)
            return r;
        r.ok      = true;
        r.silent  = d->abilitySilentSteps;
        r.defense = d->abilityDefense;
        r.regen   = d->abilityLifeRecovery;
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

    static void EmitPartsTypeChange()
    {
        static std::uint8_t s_prevParts = 0xFF;
        static std::uint8_t s_prevSel   = 0xFF;
        static std::uint8_t s_prevPt    = 0xFF;

        const std::uint8_t parts = outfit::ReadLivePartsType();
        const std::uint8_t sel   = outfit::ReadLiveSelectorCode();
        const std::uint8_t pt    = outfit::ReadLivePlayerType();
        if (parts == 0xFF)
            return;
        if (parts == s_prevParts && sel == s_prevSel && pt == s_prevPt)
            return;
        s_prevParts = parts;
        s_prevSel   = sel;
        s_prevPt    = pt;

        V_FrameWork::EmitMessage("Player", "partsTypeChange",
            pt, parts, sel);
#ifdef _DEBUG
        LogDebug("[OutfitAbilities] Player.partsTypeChange: playerType=%u "
            "partsType=0x%02X camoType=0x%02X\n",
            static_cast<unsigned>(pt), static_cast<unsigned>(parts),
            static_cast<unsigned>(sel));
#endif
    }

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

        std::uint32_t count = 0;
        std::uint16_t* arr = GetWornArr_SEH(self, kHolderOff_LifeRecovery, &count);
        if (!arr)
        {
            g_OrigUpdateLife(self);
            return;
        }
        if (count > kMaxSlots) count = kMaxSlots;

        std::uint16_t saved[kMaxSlots] = {};
        bool swapped[kMaxSlots] = {};
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const SlotAbilities ab = GetSlotAbilities(i);
            if (!ab.ok || ab.regen == 0)
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

        g_OrigUpdateLife(self);

        for (std::uint32_t i = 0; i < count; ++i)
            if (swapped[i])
                WriteWorn_SEH(arr, i, saved[i]);
    }

    static std::uint64_t __fastcall hkCalcDamageValueAtIndex(
        void* self, std::uint32_t slotIndex, void* a3, void* a4, void* a5,
        void* a6, void* a7, void* a8, void* a9)
    {
        std::uint16_t* arr = nullptr;
        std::uint16_t saved = 0;
        bool swapped = false;

        if (!MissionCodeGuard::ShouldBypassHooks() && slotIndex < kMaxSlots)
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

        if (swapped)
            WriteWorn_SEH(arr, slotIndex, saved);
        return r;
    }

    static void* __fastcall hkConvertRattleSuitPlayer(
        void* self, void* retStorage, std::uint64_t stringId, void* p2,
        void* p3, void* p4, std::uint64_t p5, void* p6, void* seParam)
    {
        std::uint8_t saved = 0;
        bool swapped = false;

        if (!MissionCodeGuard::ShouldBypassHooks() && seParam)
        {
            bool readOk = false;
            const std::uint8_t cur =
                ReadSeByte_SEH(seParam, kSeParam_PartsTypeOff, &readOk);
            if (readOk)
            {
                std::uint8_t parts = cur;
                if (parts < outfit::kCustomPartsTypeStart
                    || parts > outfit::kCustomPartsTypeEnd)
                    parts = outfit::ReadLivePartsType();
                if (parts >= outfit::kCustomPartsTypeStart
                    && parts <= outfit::kCustomPartsTypeEnd)
                {
                    const outfit::OutfitEntry* e = nullptr;
                    if (outfit::TryGetOutfitByPartsType(parts, &e) && e)
                    {
                        const outfit::OutfitPlayerTypeData* d =
                            e->GetPTData(outfit::ReadLivePlayerType());
                        if (d && d->abilityRattleSuit != 0xFF
                            && d->abilityRattleSuit != cur
                            && WriteSeByte_SEH(seParam, kSeParam_PartsTypeOff,
                                               d->abilityRattleSuit))
                        {
                            saved   = cur;
                            swapped = true;
#ifdef _DEBUG
                            static std::atomic<int> s_log{ 0 };
                            if (s_log.fetch_add(1) < 8)
                                LogDebug("[OutfitAbilities] rattleSuit donor "
                                    "served: partsType 0x%02X -> 0x%02X for "
                                    "cloth foley row\n",
                                    static_cast<unsigned>(cur),
                                    static_cast<unsigned>(d->abilityRattleSuit));
#endif
                        }
                    }
                }
            }
        }

        void* r = g_OrigConvertRattle(self, retStorage, stringId, p2, p3, p4,
                                      p5, p6, seParam);

        if (swapped)
            WriteSeByte_SEH(seParam, kSeParam_PartsTypeOff, saved);
        return r;
    }

    static bool DumpAndVerifyPrologue(const char* what, std::uintptr_t addr,
                                      const std::uint8_t* expect,
                                      std::size_t expectLen)
    {
        std::uint8_t bytes[20] = {};
        bool readOk = false;
        __try
        {
            std::memcpy(bytes, reinterpret_cast<const void*>(addr),
                        sizeof(bytes));
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
            Log("[OutfitAbilities] %s @ 0x%llX prologue mismatch - build "
                "drift, hook skipped (feature degrades, no patch applied)\n",
                what, static_cast<unsigned long long>(addr));
            return false;
        }
        return true;
    }

    static std::uintptr_t g_ChosenCalcDamage = 0;
    static std::uintptr_t g_ChosenUpdateLife = 0;
    static std::uintptr_t g_ChosenRattle     = 0;

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
    bool Install_OutfitAbilities_Hooks()
    {
        if (gAddr.GetQuarkSystemTable != kSentinel_GetQuarkSystemTable)
        {
            Log("[OutfitAbilities] address set is not the EN 1.0.15.4 family "
                "(GetQuarkSystemTable=0x%llX) - outfit abilities disabled\n",
                static_cast<unsigned long long>(gAddr.GetQuarkSystemTable));
            return false;
        }

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

        Log("[OutfitAbilities] installed: damage=%s@0x%llX "
            "lifeRecovery=%s@0x%llX rattle=%s@0x%llX "
            "(defense/regen serve donor suit equipIds strictly swap-in/"
            "restore around the hooked calls; silentFootsteps is INERT "
            "pending noise-emitter RE; infinite-ammo heads sync condition "
            "bit 0x4)\n",
            g_InstalledCalcDamage ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenCalcDamage),
            g_InstalledUpdateLife ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenUpdateLife),
            g_InstalledRattle     ? "OK" : "skip",
            static_cast<unsigned long long>(g_ChosenRattle));

        return g_InstalledCalcDamage || g_InstalledUpdateLife
            || g_InstalledRattle;
    }

    void Uninstall_OutfitAbilities_Hooks()
    {
        if (g_AmmoBitOwned.exchange(false, std::memory_order_relaxed))
            RmwAmmoBit_SEH(false);

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
