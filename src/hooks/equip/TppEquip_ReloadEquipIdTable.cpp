#include "pch.h"
#include "TppEquip_ReloadEquipIdTable.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "FoxHashes.h"
#include "LuaApi.h"
#include "EquipIdCompression.h"
#include "GunBasicInject.h"

namespace
{
    static constexpr size_t kRowStride = 0x18;

    struct EquipIdRow
    {
        std::int32_t equipId = 0;
        std::int32_t equipType = 0;
        std::int32_t subId = 0;
        std::int32_t block = 0;
        std::uint64_t partsHash = 0;
        std::uint64_t packHash = 0;
        bool resident = false;
        bool released = false;
    };

    static void LogEquipIdBudgetOnce();
    static uintptr_t GetEquipSubIdAddr();

    static bool SafeStampRow(std::uint8_t* dst, std::uint16_t* word,
                             const EquipIdRow& row)
    {
        __try
        {
            *reinterpret_cast<std::uint64_t*>(dst + 0x00) = row.partsHash;
            *reinterpret_cast<std::uint64_t*>(dst + 0x08) = row.packHash;
            dst[0x10] = static_cast<std::uint8_t>(row.block);
            *word = static_cast<std::uint16_t>(
                (row.equipType & 0x3F) | ((row.subId & 0x3FF) << 6));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool SafeZeroRow(std::uint8_t* dst, std::uint16_t* word)
    {
        __try
        {
            *reinterpret_cast<std::uint64_t*>(dst + 0x00) = 0;
            *reinterpret_cast<std::uint64_t*>(dst + 0x08) = 0;
            dst[0x10] = 0;
            *word = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    using ReloadEquipIdTable_t = int(__fastcall*)(lua_State* L);
    static ReloadEquipIdTable_t g_OrigReloadEquipIdTable = nullptr;

    static std::mutex g_Mutex;
    static std::vector<EquipIdRow> g_Rows;
    static std::map<int, EquipIdRow> g_Extended;
    static std::map<std::int32_t, std::int32_t> g_AmmoRootParams;

    static std::uint8_t* g_InfoMirror = nullptr;
    static bool g_MirrorSitesPatched = false;
    static std::size_t g_MirrorSitesSkipped = 0;
    static constexpr std::int32_t kMirrorRows = 0x10000;
    static constexpr std::int32_t kNativeInfoRows = 0x28D;

    static bool MirrorCopyNativeSEH()
    {
        const auto* infoList = static_cast<const std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_InfoList));
        if (!g_InfoMirror || !infoList)
            return false;
        __try
        {
            std::memset(g_InfoMirror, 0,
                        static_cast<size_t>(kMirrorRows) * kRowStride);
            std::memcpy(g_InfoMirror, infoList,
                        static_cast<size_t>(
                            EquipIdCompression::kCompressedSlotBound) * kRowStride);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void MirrorStampRowNoLock(const EquipIdRow& row)
    {
        if (!g_InfoMirror)
            return;
        const std::int32_t idx =
            EquipIdCompression::ComputeCompressed(row.equipId);
        if (idx < 0 || idx >= kMirrorRows)
            return;
        std::uint16_t dummy = 0;
        SafeStampRow(g_InfoMirror + static_cast<size_t>(idx) * kRowStride,
                     &dummy, row);
    }

    static void RefreshInfoMirror()
    {
        if (!MirrorCopyNativeSEH())
            return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& kv : g_Extended)
        {
            if (kv.second.released)
                continue;
            MirrorStampRowNoLock(kv.second);
        }
    }

    static void* AllocateMirrorNear(std::uintptr_t nearAddr, std::size_t size)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const std::uintptr_t granularity = si.dwAllocationGranularity;
        const std::uintptr_t roundedNear = nearAddr & ~(granularity - 1);
        const std::uintptr_t maxDistance = 0x60000000ull;
        for (std::uintptr_t offset = granularity; offset < maxDistance;
             offset += granularity)
        {
            if (roundedNear >= offset)
            {
                void* p = VirtualAlloc(
                    reinterpret_cast<LPVOID>(roundedNear - offset), size,
                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (p) return p;
            }
            void* p2 = VirtualAlloc(
                reinterpret_cast<LPVOID>(roundedNear + offset), size,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p2) return p2;
        }
        return nullptr;
    }

    struct MirrorAbsDispSite
    {
        std::uintptr_t va;
        std::uint8_t   prefix[4];
        std::uint8_t   prefixLen;
        std::uint8_t   dispOffset;
        std::uint32_t  expectedDisp;
        std::uint32_t  rowOffset;
    };

    struct MirrorRipSite
    {
        std::uintptr_t va;
        std::uint32_t  rowOffset;
    };


    static std::size_t PatchInfoListLeaSites()
    {
        static const MirrorRipSite kRipSites[] = {
            { 0x140a03587ull, 0x00u }, { 0x140a03ca4ull, 0x00u },
            { 0x140a047a4ull, 0x10u }, { 0x140a04832ull, 0x10u },
            { 0x140a05487ull, 0x00u }, { 0x140a05b80ull, 0x00u },
            { 0x140a05dd2ull, 0x00u }, { 0x140a05e15ull, 0x00u },
            { 0x140a0676cull, 0x00u }, { 0x140a07556ull, 0x00u },
            { 0x140a079d8ull, 0x00u }, { 0x140a081bbull, 0x00u },
        };
        static const MirrorAbsDispSite kAbsSites[] = {
            { 0x140a0588aull, { 0x4C, 0x8D, 0xBA, 0x00 }, 3, 3, 0x02C20FD0u, 0x00u },
            { 0x140a058beull, { 0x0F, 0xB6, 0x94, 0xCA }, 4, 4, 0x02C20FE0u, 0x10u },
        };
        const std::uintptr_t nativeBase = 0x142c20fd0ull;
        const std::uintptr_t imageBase  = 0x140000000ull;
        std::size_t patched = 0;
        std::size_t skipped = 0;

        for (const MirrorRipSite& site : kRipSites)
        {
            const std::uintptr_t va = site.va;
            auto* p = reinterpret_cast<std::uint8_t*>(va);
            if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D)
            {
                ++skipped;
                LogDebug("[EquipIdTable] mirror site 0x%llX is not the expected LEA - "
                    "skipped; this equip-data reader keeps the 653-row native "
                    "table and reads a garbage row for any extended equipId\n",
                    static_cast<unsigned long long>(va));
                continue;
            }
            const std::int32_t disp = *reinterpret_cast<const std::int32_t*>(p + 3);
            if (va + 7 + static_cast<std::intptr_t>(disp)
                != nativeBase + site.rowOffset)
            {
                ++skipped;
                LogDebug("[EquipIdTable] mirror site 0x%llX does not point at the "
                    "native InfoList - skipped; extended equipIds keep reading a "
                    "garbage row through it\n",
                    static_cast<unsigned long long>(va));
                continue;
            }
            const std::intptr_t delta =
                reinterpret_cast<std::intptr_t>(g_InfoMirror) + site.rowOffset
                - static_cast<std::intptr_t>(va + 7);
            if (delta < INT32_MIN || delta > INT32_MAX)
            {
                Log("[EquipIdTable] ERROR: the InfoList mirror landed outside "
                    "rel32 range of the equip-data readers - extended weapons "
                    "keep queueing a garbage package at loadout spawn and never "
                    "finish loading\n");
                break;
            }
            DWORD old = 0;
            if (VirtualProtect(p + 3, 4, PAGE_EXECUTE_READWRITE, &old))
            {
                *reinterpret_cast<std::int32_t*>(p + 3) =
                    static_cast<std::int32_t>(delta);
                VirtualProtect(p + 3, 4, old, &old);
                ++patched;
            }
        }

        for (const MirrorAbsDispSite& site : kAbsSites)
        {
            auto* p = reinterpret_cast<std::uint8_t*>(site.va);
            if (std::memcmp(p, site.prefix, site.prefixLen) != 0
                || *reinterpret_cast<const std::uint32_t*>(p + site.dispOffset)
                       != site.expectedDisp)
            {
                ++skipped;
                LogDebug("[EquipIdTable] mirror site 0x%llX is not the expected "
                    "image-base read of the native InfoList - skipped; the "
                    "block-with-pack resolver keeps reading a garbage row for "
                    "extended equipIds, so their model package is never verified "
                    "and the weapon never realizes\n",
                    static_cast<unsigned long long>(site.va));
                continue;
            }
            const std::intptr_t delta =
                reinterpret_cast<std::intptr_t>(g_InfoMirror) + site.rowOffset
                - static_cast<std::intptr_t>(imageBase);
            if (delta < INT32_MIN || delta > INT32_MAX)
            {
                Log("[EquipIdTable] ERROR: the InfoList mirror landed outside "
                    "disp32 range of the image base - the block-with-pack "
                    "resolver keeps reading a garbage row for extended "
                    "equipIds\n");
                break;
            }
            DWORD old = 0;
            if (VirtualProtect(p + site.dispOffset, 4, PAGE_EXECUTE_READWRITE,
                               &old))
            {
                *reinterpret_cast<std::int32_t*>(p + site.dispOffset) =
                    static_cast<std::int32_t>(delta);
                VirtualProtect(p + site.dispOffset, 4, old, &old);
                ++patched;
            }
        }

        if (skipped)
            Log("[EquipIdTable] WARNING: %zu of %zu InfoList reader site(s) could "
                "not be repointed at the mirror - an extended weapon reached "
                "through one of those readers still resolves no package and its "
                "loadout slot will wait forever\n",
                skipped, skipped + patched);
        g_MirrorSitesSkipped = skipped;
        return patched;
    }

    static void WriteNativeRow(const EquipIdRow& row)
    {
        const std::int32_t index = EquipIdCompression::ComputeCompressed(row.equipId);
        if (!EquipIdCompression::IsCompressedInBounds(index))
        {
            if (EquipIdCompression::IsExtendedEquipId(row.equipId))
            {
                EquipIdCompression::MarkExtendedEquipIdUsed(row.equipId);
                TppEquip_EnsureInfoListMirror();
                std::lock_guard<std::mutex> lock(g_Mutex);
                g_Extended[row.equipId] = row;
                MirrorStampRowNoLock(row);
                static std::size_t s_extLogged = 0;
                if (s_extLogged < 8)
                {
                    ++s_extLogged;
                    LogDebug("[EquipIdTable] equipId=%d is beyond the %d-slot native "
                        "table - stored in the DLL extended table (served to the "
                        "engine via the hooked equip accessors)\n",
                        row.equipId, EquipIdCompression::kCompressedSlotBound);
                }
                return;
            }
            LogDebug("[EquipIdTable] REFUSED write: equipId=%d compresses to 0x%X, "
                "out of the 0x%X-slot native table and not in the handle-"
                "representable extended range - row dropped.\n",
                row.equipId, index, EquipIdCompression::kCompressedSlotBound);
            return;
        }

        auto* infoList = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_InfoList));
        auto* typeWords = static_cast<std::uint16_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
        if (!infoList || !typeWords)
        {
            LogDebug("[EquipIdTable] native table address(es) not resolved; write skipped\n");
            return;
        }

        if (row.subId > 0x3FF)
        {
            LogDebug("[EquipIdTable] REFUSED write: equipId=%d carries subId=%d, but a native equip "
                "row packs subId into 10 bits (max 1023) alongside a 6-bit equipType - writing "
                "it would truncate to %d and the loadout would resolve a DIFFERENT weapon. This "
                "weapon needs an extended equipId; row dropped.\n",
                row.equipId, row.subId, row.subId & 0x3FF);
            return;
        }

        std::uint8_t* dst = infoList + static_cast<size_t>(index) * kRowStride;
        if (!SafeStampRow(dst, typeWords + index, row))
        {
            LogDebug("[EquipIdTable] SEH writing native row for equipId=%d - "
                "addresses wrong for this build; write skipped\n", row.equipId);
            return;
        }

        EquipIdCompression::MarkCompressedSlotUsed(index);
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            MirrorStampRowNoLock(row);
            for (auto& existing : g_Rows)
                if (existing.equipId == row.equipId)
                {
                    existing.resident = true;
                    break;
                }
        }
    }

    static bool EraseNativeRow(std::int32_t equipId)
    {
        const std::int32_t index = EquipIdCompression::ComputeCompressed(equipId);
        if (!EquipIdCompression::IsCompressedInBounds(index))
        {
            LogDebug("[EquipIdTable] EraseNativeRow refused: equipId=%d out of "
                "bounds\n", equipId);
            return false;
        }
        auto* infoList = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_InfoList));
        auto* typeWords = static_cast<std::uint16_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
        if (!infoList || !typeWords)
        {
            LogDebug("[EquipIdTable] EraseNativeRow: table address(es) not resolved; "
                "skipped\n");
            return false;
        }
        std::uint8_t* dst = infoList + static_cast<size_t>(index) * kRowStride;
        if (!SafeZeroRow(dst, typeWords + index))
        {
            LogDebug("[EquipIdTable] SEH erasing native row for equipId=%d - "
                "skipped\n", equipId);
            return false;
        }
        EquipIdCompression::ClearCompressedSlotUsed(index);
#ifdef _DEBUG
        LogDebug("[EquipIdTable] native row released for equipId=%d\n", equipId);
#endif
        return true;
    }

    static bool ReadRowNumber(lua_State* L, int n, double& out)
    {
        out = 0.0;
        g_lua_rawgeti(L, -1, n);
        const bool ok = g_lua_isnumber(L, -1) != 0;
        if (ok)
            out = static_cast<double>(g_lua_tonumber(L, -1));
        g_lua_settop(L, -2);
        return ok;
    }

    static bool ReadRowPathHash(lua_State* L, int n, std::uint64_t& out)
    {
        out = 0;
        g_lua_rawgeti(L, -1, n);
        const bool ok = g_lua_type(L, -1) == LUA_TSTRING;
        if (ok)
        {
            const char* path = g_lua_tolstring(L, -1, nullptr);
            if (path && path[0])
                out = FoxHashes::PathCode64Ext(path);
        }
        g_lua_settop(L, -2);
        return ok;
    }

    static void QueueAndWrite(const EquipIdRow& rowIn)
    {
        EquipIdRow row = rowIn;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (row.subId == 0)
            {
                auto it = g_AmmoRootParams.find(row.equipId);
                if (it != g_AmmoRootParams.end())
                {
                    row.subId = it->second;
                    LogDebug("[EquipIdTable] equipId=%d is a registered ammo item - subId "
                        "auto-set to its ammoId %d (the supply-crate refill resolves the "
                        "refill amount through this field; with subId 0 an ammo supply "
                        "delivered nothing for this ammo)\n",
                        row.equipId, row.subId);
                }
            }
            bool replaced = false;
            for (auto& existing : g_Rows)
            {
                if (existing.equipId == row.equipId)
                {
                    existing = row;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                g_Rows.push_back(row);
        }

        WriteNativeRow(row);
#ifdef _DEBUG
        LogDebug("[EquipIdTable] custom row: equipId=%d type=%d subId=%d block=%d "
            "parts=%016llX pack=%016llX\n",
            row.equipId, row.equipType, row.subId, row.block,
            static_cast<unsigned long long>(row.partsHash),
            static_cast<unsigned long long>(row.packHash));
#endif
    }

    static void ReapplyAll()
    {
        std::vector<EquipIdRow> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            snapshot = g_Rows;
        }
        std::size_t applied = 0;
        for (const auto& row : snapshot)
        {
            if (row.released)
                continue;
            WriteNativeRow(row);
            ++applied;
        }
#ifdef _DEBUG
        if (applied)
            LogDebug("[EquipIdTable] re-applied %zu custom row(s) after reload\n",
                applied);
#endif
        TppEquip_EnsureInfoListMirror();
        RefreshInfoMirror();
        LogEquipIdBudgetOnce();
    }

    static int ScanTypeWordsSEH(const std::uint16_t* words, int count,
                                int* maxType, int* maxSub, int* typeBit5)
    {
        __try
        {
            for (int i = 0; i < count; ++i)
            {
                const unsigned w = words[i];
                if (!w)
                    continue;
                const int t = static_cast<int>(w & 0x3F);
                const int s = static_cast<int>((w >> 6) & 0x3FF);
                if (t > *maxType) *maxType = t;
                if (s > *maxSub)  *maxSub  = s;
                if (t & 0x20)     ++(*typeBit5);
            }
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void LogSubIdCeilingOnce()
    {
        static std::atomic<bool> s_logged{ false };
        if (s_logged.exchange(true))
            return;

        const auto* words = static_cast<const std::uint16_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
        if (!words)
            return;

        int maxType = 0, maxSub = 0, typeBit5 = 0;
        if (!ScanTypeWordsSEH(words, kNativeInfoRows, &maxType, &maxSub, &typeBit5))
        {
            LogDebug("[EquipIdCeiling] SEH scanning the packed type/subId words - "
                "the weapon-count ceiling could not be measured on this build\n");
            return;
        }

        LogDebug("[EquipIdCeiling] native packed word is type:6|subId:10 - live scan of %d rows: "
            "max equipType=%d, max subId=%d, rows using type bit5=%d. NATIVE equip rows stay "
            "capped at subId %d (the engine unpacks this word from staged copies, so the "
            "unpack sites are not statically enumerable and re-packing it would corrupt equip "
            "types). EXTENDED equipIds are served from the DLL table by the GetEquipTypeId/"
            "GetEquipSubId hooks and never touch this word, so their subId ceiling is the "
            "gunBasic shadow instead, currently %d.\n",
            static_cast<int>(kNativeInfoRows), maxType, maxSub, typeBit5,
            GunBasic_PackedSubIdMax(), GunBasic_MaxWeaponId());
    }

    static void LogEquipIdBudgetOnce()
    {
        static std::atomic<bool> s_logged{ false };
        if (s_logged.exchange(true))
            return;

        LogSubIdCeilingOnce();

        std::vector<EquipIdRow> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            snapshot = g_Rows;
        }

        int weaponNative = 0, weaponExt = 0, otherNative = 0, otherExt = 0;
        for (const auto& row : snapshot)
        {
            if (row.released || row.equipId <= 0)
                continue;
            const bool ext = EquipIdCompression::IsExtendedEquipId(row.equipId);
            if (row.subId != 0)
                ext ? ++weaponExt : ++weaponNative;
            else
                ext ? ++otherExt : ++otherNative;
        }

        int freeItem = 0, freeWeapon = 0;
        for (std::int32_t i = EquipIdCompression::kItemBandFirst;
             i <= EquipIdCompression::kItemBandLast; ++i)
            if (!EquipIdCompression::IsCompressedSlotUsed(i))
                ++freeItem;
        for (std::int32_t i = EquipIdCompression::kWeaponBandFirst;
             i <= EquipIdCompression::kWeaponBandLastUsable; ++i)
            if (!EquipIdCompression::IsCompressedSlotUsed(i))
                ++freeWeapon;

        const int headroom = freeItem + freeWeapon + otherNative;
        const int stranded = (weaponExt > headroom) ? (weaponExt - headroom) : 0;

        LogDebug("[EquipIdBudget] custom equip rows %d: WEAPONS %d (native %d / "
            "EXTENDED %d) | non-weapons %d (native %d / extended %d). Native "
            "slots still free: item band %d, weapon band %d.\n",
            weaponNative + weaponExt + otherNative + otherExt,
            weaponNative + weaponExt, weaponNative, weaponExt,
            otherNative + otherExt, otherNative, otherExt,
            freeItem, freeWeapon);

        const bool subIdServed = GetEquipSubIdAddr() != 0;
        const bool mirrorArmed = g_InfoMirror != nullptr;

        if (weaponExt > 0 && (!subIdServed || !mirrorArmed))
            LogDebug("[EquipIdBudget] %d weapon(s) hold EXTENDED equipIds but this "
                "build lacks %s%s%s, so those weapons cannot be equipped: "
                "without the subId accessor the loadout resolves them to "
                "nothing and equips a vanilla gun instead, and without the "
                "mirror their block package never goes resident and the deploy "
                "hangs on an infinite load. Recoverable native headroom = %d "
                "free + %d currently spent on non-weapon rows = %d; "
                "reallocating weapons into native ids first would still strand "
                "%d of them.\n",
                weaponExt,
                subIdServed ? "" : "the GetEquipSubId hook",
                (!subIdServed && !mirrorArmed) ? " and " : "",
                mirrorArmed ? "" : "the extended InfoList mirror",
                freeItem + freeWeapon, otherNative, headroom, stranded);
    }

    static bool DiagReadHash(const std::uint8_t* p, std::uint64_t& out)
    {
        __try
        {
            out = *reinterpret_cast<const std::uint64_t*>(p);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void DiagDumpEquipOccupancy()
    {
        static int   s_dumps = 0;
        static DWORD s_lastTick = 0;
        if (s_dumps >= 6)
            return;
        const DWORD now = GetTickCount();
        if (s_lastTick != 0 && (now - s_lastTick) < 4000u)
            return;
        s_lastTick = now;
        const int myN = ++s_dumps;

        auto* infoList = g_InfoMirror
            ? g_InfoMirror
            : static_cast<std::uint8_t*>(
                  ResolveGameAddress(gAddr.EquipIdTable_InfoList));
        if (!infoList)
        {
            LogDebug("[ZetaDiag] #%d: EquipIdTable_InfoList unresolved for this build\n",
                myN);
            return;
        }

        std::vector<EquipIdRow> vfw;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            vfw = g_Rows;
        }

        LogDebug("[ZetaDiag] ===== equip-id dump #%d: %zu V_FrameWork custom rows "
            "(%s, folded-indexed, AFTER the game/Zeta push, BEFORE V_FrameWork "
            "re-stamps) =====\n", myN, vfw.size(),
            g_InfoMirror ? "mirror - what the repointed readers actually resolve"
                         : "native table");

        int ours = 0, foreign = 0, empty = 0, ext = 0;
        for (const auto& r : vfw)
        {
            const std::int32_t idx =
                EquipIdCompression::ComputeCompressed(r.equipId);
            if (idx < 0 || idx >= (g_InfoMirror
                                       ? kMirrorRows
                                       : EquipIdCompression::kCompressedSlotBound))
            {
                ++ext;
                LogDebug("[ZetaDiag]   equipId=0x%X folds to 0x%X - past the readable "
                    "bound\n", r.equipId, idx);
                continue;
            }
            std::uint64_t cur = 0;
            const bool ok = DiagReadHash(
                infoList + static_cast<size_t>(idx) * kRowStride, cur);
            const char* verdict;
            if (!ok)                     { verdict = "SEH-unreadable"; }
            else if (cur == 0)           { verdict = "EMPTY(push dropped it)"; ++empty; }
            else if (cur == r.partsHash) { verdict = "OURS"; ++ours; }
            else                         { verdict = "FOREIGN<-collision (Zeta/vanilla holds this slot)"; ++foreign; }
            LogDebug("[ZetaDiag]   equipId=0x%X slot=0x%X ourParts=%016llX "
                "native=%016llX %s\n",
                r.equipId, idx,
                static_cast<unsigned long long>(r.partsHash),
                static_cast<unsigned long long>(cur), verdict);
        }

        int wbOcc = 0;
        for (std::int32_t i = EquipIdCompression::kWeaponBandFirst;
             i <= EquipIdCompression::kWeaponBandLastUsable; ++i)
        {
            std::uint64_t cur = 0;
            if (DiagReadHash(infoList + static_cast<size_t>(i) * kRowStride, cur)
                && cur != 0)
                ++wbOcc;
        }
        LogDebug("[ZetaDiag] #%d summary: OURS=%d FOREIGN-collisions=%d EMPTY=%d "
            "PAST-BOUND=%d | weapon band 0x230-0x288 occupied=%d/89 (read from "
            "the %s)\n",
            myN, ours, foreign, empty, ext, wbOcc,
            g_InfoMirror ? "mirror" : "native table");
    }

    static int __fastcall hkReloadEquipIdTable(lua_State* L)
    {
        const int ret = g_OrigReloadEquipIdTable ? g_OrigReloadEquipIdTable(L) : 0;
        DiagDumpEquipOccupancy();
        ReapplyAll();
        return ret;
    }

    using GetEquipTypeId_t = unsigned int(__fastcall*)(void* self, unsigned int equipId);
    static GetEquipTypeId_t g_OrigGetEquipTypeId = nullptr;

    static unsigned int __fastcall hkGetEquipTypeId(void* self, unsigned int equipId)
    {
        if (EquipIdCompression::IsExtendedEquipId(static_cast<std::int32_t>(equipId)))
        {
            V_ExtendedEquipRow ext;
            if (TppEquip_GetExtendedEquipRow(static_cast<int>(equipId), &ext))
                return static_cast<unsigned int>(ext.equipType & 0x3F);
        }
        return g_OrigGetEquipTypeId ? g_OrigGetEquipTypeId(self, equipId) : 0;
    }

    using GetEquipSubId_t = unsigned int(__fastcall*)(void* self, unsigned int equipId);
    static GetEquipSubId_t g_OrigGetEquipSubId = nullptr;

    static uintptr_t GetEquipSubIdAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4:  return 0x140a2a520ull;
        default:                                           return 0;
        }
    }

    static unsigned int __fastcall hkGetEquipSubId(void* self, unsigned int equipId)
    {
        if (EquipIdCompression::IsExtendedEquipId(static_cast<std::int32_t>(equipId)))
        {
            V_ExtendedEquipRow ext;
            if (TppEquip_GetExtendedEquipRow(static_cast<int>(equipId), &ext))
            {
                if (ext.subId < 0 || ext.subId > 0x3FF)
                    return static_cast<unsigned int>(ext.subId) & 0x3FF;
                return static_cast<unsigned int>(ext.subId);
            }
        }
        return g_OrigGetEquipSubId ? g_OrigGetEquipSubId(self, equipId) : 0;
    }
}

int TppEquip_GetSubIdForEquipId(int equipId)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    for (const auto& r : g_Rows)
        if (r.equipId == equipId)
            return r.subId;
    return 0;
}

bool TppEquip_GetExtendedEquipRow(int equipId, V_ExtendedEquipRow* out)
{
    if (!out)
        return false;
    std::lock_guard<std::mutex> lock(g_Mutex);
    auto it = g_Extended.find(equipId);
    if (it == g_Extended.end() || it->second.released)
        return false;
    out->equipType = it->second.equipType;
    out->subId     = it->second.subId;
    out->block     = it->second.block;
    out->partsHash = it->second.partsHash;
    out->packHash  = it->second.packHash;
    return true;
}

bool TppEquip_EnsureInfoListMirror()
{
    if (g_MirrorSitesPatched)
        return true;
    if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
    {
        LogDebug("[EquipIdTable] InfoList mirror sites not ported for this build - "
            "the equip-data readers index the 653-row native table with the raw "
            "extended equipId, so an extended weapon's loadout slot queues a "
            "garbage package, never reports ready, and the deploy hangs\n");
        g_MirrorSitesPatched = true;
        return true;
    }
    if (!g_InfoMirror)
    {
        g_InfoMirror = static_cast<std::uint8_t*>(AllocateMirrorNear(
            0x140a00000ull, static_cast<size_t>(kMirrorRows) * kRowStride));
        if (!g_InfoMirror)
        {
            Log("[EquipIdTable] ERROR: could not allocate the InfoList mirror "
                "near the game image - extended weapons keep queueing a garbage "
                "package at loadout spawn and the deploy hangs\n");
            return false;
        }
    }
    RefreshInfoMirror();
    const std::size_t patched = PatchInfoListLeaSites();
    g_MirrorSitesPatched = true;
    if (patched == 0)
    {
        Log("[EquipIdTable] ERROR: no InfoList reader site could be repointed - "
            "extended weapons resolve no model package and deploying with one "
            "equipped will hang on the loading screen\n");
        return false;
    }
    if (g_MirrorSitesSkipped != 0)
        Log("[EquipIdTable] WARNING: %zu InfoList reader site(s) could not be "
            "repointed at the mirror - an extended weapon reached through one of "
            "them resolves no package and its loadout slot waits forever\n",
            g_MirrorSitesSkipped);
#ifdef _DEBUG
    LogDebug("[EquipIdTable] extended InfoList mirror armed at %p: %zu equip-data "
        "reader site(s) repointed from the 653-row native table to a %d-row "
        "mirror, indexed by the engine's own folded row (vanilla aliasing left "
        "intact); extended equipIds %d..%d fold to rows 0x%X..0x%X, clear of "
        "every vanilla row\n",
        static_cast<void*>(g_InfoMirror), patched, kMirrorRows,
        EquipIdCompression::kExtendedAllocFirst,
        EquipIdCompression::kExtendedEquipIdLast,
        EquipIdCompression::kExtendedFoldedFirst,
        EquipIdCompression::kExtendedFoldedLast);
#endif
    return true;
}

int __cdecl l_AddToEquipIdTable(lua_State* L)
{
    if (!ResolveLuaApi() || !g_lua_objlen || !g_lua_rawgeti)
        return 0;
    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipIdTable] AddToEquipIdTable: argument #1 must be a table\n");
        return 0;
    }

    const int rowCount = static_cast<int>(g_lua_objlen(L, 1));
    for (int i = 1; i <= rowCount; ++i)
    {
        g_lua_rawgeti(L, 1, i);
        if (g_lua_type(L, -1) == LUA_TTABLE)
        {
            double idN, typeN, subN, blockN;
            EquipIdRow row;
            if (ReadRowNumber(L, 1, idN) &&
                ReadRowNumber(L, 2, typeN) &&
                ReadRowNumber(L, 3, subN) &&
                ReadRowNumber(L, 4, blockN) &&
                ReadRowPathHash(L, 5, row.partsHash) &&
                ReadRowPathHash(L, 6, row.packHash))
            {
                row.equipId   = static_cast<std::int32_t>(idN);
                row.equipType = static_cast<std::int32_t>(typeN);
                row.subId     = static_cast<std::int32_t>(subN);
                row.block     = static_cast<std::int32_t>(blockN);
                if (row.equipId > 0)
                    QueueAndWrite(row);
            }
            else
            {
                LogDebug("[EquipIdTable] AddToEquipIdTable: ignored malformed row %d\n", i);
            }
        }
        g_lua_settop(L, -2);
    }

    return 0;
}

bool Install_TppEquip_ReloadEquipIdTable_Hook()
{
    void* target = ResolveGameAddress(gAddr.EquipIdTable_ReloadEquipIdTable);
    if (!target)
    {
        LogDebug("[EquipIdTable] ReloadEquipIdTable address not set for this build - skipped\n");
        return true;
    }

    const bool ok = CreateAndEnableHook(
        target, &hkReloadEquipIdTable,
        reinterpret_cast<void**>(&g_OrigReloadEquipIdTable));
    if (!ok)
    {
        Log("[EquipIdTable] Reload hook Install -> FAIL (target=%p)\n", target);
    }
#ifdef _DEBUG
    else
    {
        LogDebug("[EquipIdTable] Reload hook Install -> OK (target=%p)\n", target);
    }
#endif

    void* typeTarget = ResolveGameAddress(gAddr.EquipIdTable_GetEquipTypeId);
    if (typeTarget)
    {
        const bool okT = CreateAndEnableHook(
            typeTarget, &hkGetEquipTypeId,
            reinterpret_cast<void**>(&g_OrigGetEquipTypeId));
        if (!okT)
            Log("[EquipIdTable] GetEquipTypeId hook Install -> FAIL (target=%p) - "
                "extended equipIds will report type 0\n", typeTarget);
#ifdef _DEBUG
        else
            LogDebug("[EquipIdTable] GetEquipTypeId hook Install -> OK (target=%p; serves "
                "the equip type of extended custom equipIds from the DLL table)\n",
                typeTarget);
#endif
    }

    if (const uintptr_t subAddr = GetEquipSubIdAddr())
    {
        void* subTarget = ResolveGameAddress(subAddr);
        const bool okS = CreateAndEnableHook(
            subTarget, &hkGetEquipSubId,
            reinterpret_cast<void**>(&g_OrigGetEquipSubId));
        if (!okS)
            Log("[EquipIdTable] GetEquipSubId hook Install -> FAIL (target=%p) - the "
                "native accessor answers extended custom equipIds with a weapon-band "
                "ordinal instead of the real weapon subId, so the loadout builds a "
                "different gun than the one picked\n", subTarget);
#ifdef _DEBUG
        else
            LogDebug("[EquipIdTable] GetEquipSubId hook Install -> OK (target=%p; serves "
                "the weapon subId of extended custom equipIds from the DLL table)\n",
                subTarget);
#endif
    }
    else
        LogDebug("[EquipIdTable] GetEquipSubId address not ported for this build - the "
            "native accessor answers extended custom equipIds with a weapon-band "
            "ordinal instead of the real weapon subId, so picking a custom weapon "
            "builds a different gun\n");
    return ok;
}

bool Uninstall_TppEquip_ReloadEquipIdTable_Hook()
{
    if (gAddr.EquipIdTable_GetEquipTypeId && g_OrigGetEquipTypeId)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.EquipIdTable_GetEquipTypeId));
    g_OrigGetEquipTypeId = nullptr;
    if (g_OrigGetEquipSubId)
        DisableAndRemoveHook(ResolveGameAddress(GetEquipSubIdAddr()));
    g_OrigGetEquipSubId = nullptr;
    if (gAddr.EquipIdTable_ReloadEquipIdTable)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.EquipIdTable_ReloadEquipIdTable));
    g_OrigReloadEquipIdTable = nullptr;

    std::vector<std::int32_t> resident;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& row : g_Rows)
            if (row.resident)
                resident.push_back(row.equipId);
        g_Rows.clear();
    }
    for (const std::int32_t id : resident)
        EraseNativeRow(id);
    return true;
}

void TppEquip_NoteAmmoRootParam(int eqpAmmoEquipId, int ammoId)
{
    if (eqpAmmoEquipId <= 0 || ammoId <= 0 || ammoId > 0x3FF)
        return;
    EquipIdRow updated{};
    bool restamp = false;
    int prevSub = 0;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_AmmoRootParams[eqpAmmoEquipId] = ammoId;
        for (auto& r : g_Rows)
        {
            if (r.equipId != eqpAmmoEquipId || r.released)
                continue;
            if (r.subId != ammoId)
            {
                prevSub = r.subId;
                r.subId = ammoId;
                updated = r;
                restamp = true;
            }
            break;
        }
        auto it = g_Extended.find(eqpAmmoEquipId);
        if (it != g_Extended.end() && !it->second.released
            && it->second.subId != ammoId)
        {
            it->second.subId = ammoId;
            MirrorStampRowNoLock(it->second);
        }
    }
    if (restamp)
    {
        WriteNativeRow(updated);
        LogDebug("[EquipIdTable] ammo item equipId=%d subId %d -> %d (its ammoId; the "
            "supply-crate refill resolves the refill amount through this field - the row "
            "was stamped before SetMagazine assigned the ammo row, so ammo supplies "
            "delivered nothing for it)\n",
            eqpAmmoEquipId, prevSub, ammoId);
    }
}

int TppEquip_ReleaseEquipRow(int equipId)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& row : g_Rows)
            if (row.equipId == equipId)
            {
                row.resident = false;
                row.released = true;
                found = true;
                break;
            }
    }
    if (!found)
        return 0;
    return EraseNativeRow(equipId) ? 1 : 0;
}
