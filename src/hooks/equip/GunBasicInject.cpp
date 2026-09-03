#include "pch.h"
#include "GunBasicInject.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "AddressSet.h"
#include "EquipPartParams.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaApi.h"
#include "../../core/V_FrameWorkState.h"

namespace
{
    struct GunBasicRow
    {
        std::int32_t f[13];
        std::int32_t logical[13];
        std::int32_t slotSpace[13];
    };

    using ReloadEquipParameterTables2_t = void(__fastcall*)(lua_State* L);
    static ReloadEquipParameterTables2_t g_OrigReload = nullptr;

    static std::recursive_mutex g_Mutex;
    static std::vector<GunBasicRow> g_Rows;
    static std::map<std::string, int> g_WpNameToId;
    static std::set<int> g_ClaimedIds;
    static std::set<int> g_ReservedIds;
    static bool g_PersistedReserved = false;

    static constexpr int kNoneValue = 0;

    static constexpr int kEssentialDefaultId = 1;

    static constexpr int kGunBasicMaxId = 65535;

    static constexpr int kPackedSubIdMax = 1023;

    static std::vector<std::uint8_t> g_GunBasicShadow;
    static bool g_GunBasicShadowActive = false;

    static void** GunBasicPtrLoc()
    {
        auto* impl = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipParameterTablesImpl_Instance));
        if (!impl)
            return nullptr;
        return reinterpret_cast<void**>(impl + 0x08);
    }

    static std::uint8_t* ReadPtrSEH(void** loc)
    {
        if (!loc)
            return nullptr;
        __try
        {
            return static_cast<std::uint8_t*>(*loc);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static int RedirectShadowSEH(void** loc, std::uint8_t* shadow, size_t copyBytes)
    {
        __try
        {
            std::uint8_t* stock = static_cast<std::uint8_t*>(*loc);
            if (!stock)
                return 0;
            std::memcpy(shadow, stock, copyBytes);
            *loc = shadow;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int StockSlotCount()
    {
        return kGunBasicParameters2StockSlots;
    }

    static bool EnsureGunBasicShadow()
    {
        void** loc = GunBasicPtrLoc();
        if (!loc)
            return false;

        const int stock = StockSlotCount();
        if (stock <= 0 || stock > kGunBasicMaxId)
            return false;

        if (g_GunBasicShadow.empty())
            g_GunBasicShadow.assign(static_cast<size_t>(kGunBasicMaxId) * 12, 0);

        if (ReadPtrSEH(loc) == g_GunBasicShadow.data())
        {
            g_GunBasicShadowActive = true;
            return true;
        }

        if (RedirectShadowSEH(loc, g_GunBasicShadow.data(),
                              static_cast<size_t>(stock) * 12) != 1)
        {
            if (!g_GunBasicShadowActive)
                g_GunBasicShadow.clear();
            return false;
        }

        if (!g_GunBasicShadowActive)
        {
            g_GunBasicShadowActive = true;
            LogDebug("[GunBasic] buffer shadow active (stock %d -> %d slots; custom weaponIds up to %d)\n",
                stock, kGunBasicMaxId, kGunBasicMaxId);
        }
        return true;
    }

    static std::uint8_t* BufferBase()
    {
        std::uint8_t* live = ReadPtrSEH(GunBasicPtrLoc());
        if (live)
            return live;
        return static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.GunBasicParameters2Buffer));
    }

    static int BufferSlotCount()
    {
        if (g_GunBasicShadowActive)
            return kGunBasicMaxId;
        return StockSlotCount();
    }

    static int SlotIsZeroSEH(const std::uint8_t* buf, int idx)
    {
        __try
        {
            const std::uint8_t* p = buf + static_cast<size_t>(idx) * 12;
            for (int k = 0; k < 12; ++k)
                if (p[k] != 0)
                    return 0;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static void WriteNativeRowSEH(std::uint8_t* buf, int weaponId, const std::int32_t* f12)
    {
        static const int kByteForField[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 10, 8, 9, 11 };
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(weaponId - 1) * 12;
            for (int k = 0; k < 12; ++k)
                p[kByteForField[k]] = static_cast<std::uint8_t>(f12[k] & 0xFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static int CopyRowBytesSEH(const std::uint8_t* src, unsigned char* dst)
    {
        __try
        {
            for (int k = 0; k < 12; ++k)
                dst[k] = src[k];
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void ReapplyAllNative()
    {
        std::vector<GunBasicRow> snapshot;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            EnsureGunBasicShadow();
            snapshot = g_Rows;
        }

        std::uint8_t* buf = BufferBase();
        const int cap = BufferSlotCount();
        if (!buf || cap <= 0)
            return;

        for (const GunBasicRow& row : snapshot)
        {
            if (row.f[0] >= 1 && row.f[0] <= cap)
                WriteNativeRowSEH(buf, row.f[0], &row.f[1]);
        }
#ifdef _DEBUG
        if (!snapshot.empty())
            LogDebug("[GunBasic] re-applied %zu native gunBasic row(s) after reload\n",
                snapshot.size());
#endif
    }

    static void __fastcall hkReloadEquipParameterTables2(lua_State* L)
    {
        if (g_OrigReload)
            g_OrigReload(L);
        ReapplyAllNative();
    }

    static bool ReadNamedInt(lua_State* L, int tableIdx, const char* name, int& out)
    {
        g_lua_getfield(L, tableIdx, const_cast<char*>(name));
        const bool ok = g_lua_isnumber(L, -1) != 0;
        if (ok)
            out = static_cast<int>(g_lua_tointeger(L, -1));
        g_lua_settop(L, -2);
        return ok;
    }

    static bool ReadNamedFlag(lua_State* L, int tableIdx, const char* name)
    {
        g_lua_getfield(L, tableIdx, const_cast<char*>(name));
        bool on = false;
        const int t = g_lua_type(L, -1);
        if (t == LUA_TBOOLEAN)
            on = g_lua_toboolean && g_lua_toboolean(L, -1) != 0;
        else if (g_lua_isnumber(L, -1))
            on = g_lua_tointeger(L, -1) != 0;
        g_lua_settop(L, -2);
        return on;
    }
}

int GunBasic_AllocateWeaponIdForName(const char* name)
{
    if (!name || !name[0])
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    auto it = g_WpNameToId.find(name);
    if (it != g_WpNameToId.end())
        return it->second;

    if (!g_PersistedReserved)
    {
        g_PersistedReserved = true;
        V_FrameWorkState::ForEachPersistedConstant("WP",
            [](const char* reservedName, std::int32_t v)
            {
                static_cast<void>(reservedName);
                if (v >= 2 && v <= kGunBasicMaxId)
                    g_ReservedIds.insert(static_cast<int>(v));
            });
    }

    EnsureGunBasicShadow();

    std::uint8_t* buf = BufferBase();
    const int cap = BufferSlotCount();
    if (!buf || cap <= 1)
        return 0;

    const int persisted = V_FrameWorkState::GetPersistedConstant("WP", name);
    if (persisted >= 2 && persisted <= cap &&
        !g_ClaimedIds.count(persisted) &&
        SlotIsZeroSEH(buf, persisted - 1) == 1)
    {
        g_ReservedIds.erase(persisted);
        g_ClaimedIds.insert(persisted);
        g_WpNameToId[name] = persisted;
        LogDebug("[GunBasic] '%s' -> weaponId %d (persisted slot)\n", name, persisted);
        return persisted;
    }
    if (persisted != 0)
        LogDebug("[GunBasic] persisted weaponId %d for '%s' no longer free - reallocating\n",
            persisted, name);

    const int packedHi = (cap < kPackedSubIdMax) ? cap : kPackedSubIdMax;
    for (int pass = 0; pass < 2; ++pass)
    {
        const int from = (pass == 0) ? packedHi - 1 : cap - 1;
        const int to   = (pass == 0) ? 1 : packedHi;
        if (from < to)
            continue;

        for (int idx = from; idx >= to; --idx)
        {
            const int weaponId = idx + 1;
            if (g_ClaimedIds.count(weaponId) || g_ReservedIds.count(weaponId))
                continue;
            if (SlotIsZeroSEH(buf, idx) != 1)
                continue;

            g_ClaimedIds.insert(weaponId);
            g_WpNameToId[name] = weaponId;
            V_FrameWorkState::SetPersistedConstant("WP", name, weaponId);
            if (weaponId > kPackedSubIdMax)
                LogDebug("[GunBasic] '%s' -> weaponId %d: past the %d-id "
                         "packed-word ceiling, so this weapon MUST hold an extended "
                         "equipId - a native row would truncate the subId to %d\n",
                    name, weaponId, kPackedSubIdMax, weaponId & 0x3FF);
            return weaponId;
        }
    }

    LogDebug("[GunBasic] no free native gunBasic slot for '%s' (cap=%d full) - "
             "custom weapon behavior unavailable\n",
        name, cap);
    return 0;
}

int GunBasic_MaxWeaponId()
{
    return kGunBasicMaxId;
}

int GunBasic_PackedSubIdMax()
{
    return kPackedSubIdMax;
}

bool GunBasic_ReadRowBytes(int weaponId, unsigned char* out12)
{
    if (weaponId <= 0 || !out12)
        return false;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    std::uint8_t* buf = BufferBase();
    if (!buf || weaponId > BufferSlotCount())
        return false;

    return CopyRowBytesSEH(buf + static_cast<size_t>(weaponId - 1) * 12, out12) == 1;
}

bool GunBasic_WeaponNeedsLaneBind(int weaponId)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    for (const auto& r : g_Rows)
    {
        if (r.f[0] != weaponId)
            continue;
        for (int i = 1; i <= 11; ++i)
        {
            if (r.slotSpace[i] < 0 || r.logical[i] < 0x100)
                continue;
            const int alias = EquipParam_GetWideAlias(r.slotSpace[i], r.logical[i]);
            if (alias <= 0 || r.f[i] != alias)
                return true;
        }
        return false;
    }
    return false;
}

// space-filtered on purpose: barrels/sights/etc are player-swappable in the
// gunsmith, so the desc can legitimately hold a customised part there and must
// not be overwritten. Only slots this build actually defers may be restamped.
int GunBasic_GetLogicalPart(int weaponId, int space)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    for (const auto& r : g_Rows)
    {
        if (r.f[0] != weaponId)
            continue;
        for (int i = 1; i <= 11; ++i)
            if (r.slotSpace[i] == space && r.logical[i] > 0)
                return r.logical[i];
        return 0;
    }
    return 0;
}

bool GunBasic_GetLogicalParts(int weaponId, int out11[11])
{
    for (int i = 0; i < 11; ++i)
        out11[i] = 0;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    for (const auto& r : g_Rows)
    {
        if (r.f[0] != weaponId)
            continue;
        for (int i = 1; i <= 11; ++i)
        {
            const int slot = i - 1;
            if (slot < 0 || slot >= 11)
                continue;
            out11[slot] = (r.logical[i] > 0) ? r.logical[i] : r.f[i];
        }
        return true;
    }
    return false;
}

int GunBasic_GetWideSlotLanes(int weaponId, int space, unsigned char outLanes[12])
{
    for (int i = 0; i < 12; ++i)
        outLanes[i] = 0;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    for (const auto& r : g_Rows)
    {
        if (r.f[0] != weaponId)
            continue;
        int n = 0;
        for (int i = 1; i <= 11; ++i)
        {
            if (r.slotSpace[i] != space || r.logical[i] < 0x100)
                continue;
            if (r.f[i] <= 0 || r.f[i] > 0xFF)
                continue;
            outLanes[i - 1] = static_cast<unsigned char>(r.f[i]);
            ++n;
        }
        return n;
    }
    return 0;
}

int GunBasic_ClearLaneFromRows(int space, int lane)
{
    if (space < 0 || lane <= 0 || lane > 0xFF)
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    const int cap = BufferSlotCount();
    std::uint8_t* buf = BufferBase();

    int cleared = 0;
    for (auto& r : g_Rows)
    {
        bool dirty = false;
        for (int i = 1; i <= 11; ++i)
        {
            if (r.slotSpace[i] != space || r.logical[i] < 0x100)
                continue;
            if (r.f[i] != lane)
                continue;
            const int donor = EquipParam_GetWideReceiverDonor(r.logical[i]);
            r.f[i] = (donor > 0) ? donor : kEssentialDefaultId;
            dirty = true;
            ++cleared;
        }
        if (dirty && buf && r.f[0] >= 1 && r.f[0] <= cap)
            WriteNativeRowSEH(buf, r.f[0], &r.f[1]);
    }
    return cleared;
}

int GunBasic_RebindWidePartsForWeapon(int weaponId)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    GunBasicRow* row = nullptr;
    for (auto& r : g_Rows)
        if (r.f[0] == weaponId)
        {
            row = &r;
            break;
        }
    if (!row)
        return 0;

    int rebound = 0;
    for (int i = 1; i <= 11; ++i)
    {
        if (row->slotSpace[i] < 0 || row->logical[i] < 0x100)
            continue;
        const int b = EquipParam_ResolvePartByte(row->slotSpace[i], row->logical[i]);
        if (b > 0 && b != row->f[i])
        {
            row->f[i] = b;
            ++rebound;
        }
    }

    if (rebound)
    {
        const int cap = BufferSlotCount();
        std::uint8_t* buf = BufferBase();
        if (buf && weaponId >= 1 && weaponId <= cap)
            WriteNativeRowSEH(buf, weaponId, &row->f[1]);
    }
    return rebound;
}

int __cdecl l_SetGunBasic(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[GunBasic] SetGunBasic: argument #1 must be a table\n");
        return 0;
    }

    int weaponId = 0;
    if (!ReadNamedInt(L, 1, "weaponId", weaponId) || weaponId <= 0)
    {
        LogDebug("[GunBasic] SetGunBasic: missing/invalid weaponId\n");
        return 0;
    }

    GunBasicRow row;
    row.f[0] = weaponId;

    static const char* const kSlotNames[11] =
    {
        "receiverId", "barrelId", "ammoId", "stockId", "muzzleId",
        "muzzleOptionId", "scope1Id", "scope2Id", "underBarrelId",
        "laserFlash1Id", "laserFlash2Id"
    };

    static const char* const kEssentialLabel[3] = { "receiverId", "barrelId", "ammoId" };
    for (int i = 0; i < 11; ++i)
    {
        int v = kNoneValue;
        const bool present = ReadNamedInt(L, 1, kSlotNames[i], v);
        row.f[i + 1] = v;
        if (i < 3 && (!present || v <= 0))
        {
            row.f[i + 1] = kEssentialDefaultId;
            LogDebug("[GunBasic] SetGunBasic weaponId=%d: %s is missing/unresolved "
                     "(its defining mod may not be installed) - substituting "
                     "vanilla default id %d, generic stats for that part\n",
                weaponId, kEssentialLabel[i], kEssentialDefaultId);
        }
    }

    int grade = 1;
    ReadNamedInt(L, 1, "weaponGrade", grade);
    if (grade < 1)
        grade = 1;
    else if (grade > 15)
        grade = 15;
    row.f[12] = grade;

    static const int kSlotSpaces[11] =
    {
        kVanillaSpace_Receiver, kVanillaSpace_Barrel, kVanillaSpace_Magazine,
        kVanillaSpace_Stock, -1, kVanillaSpace_MuzzleOption,
        kVanillaSpace_Sight, kVanillaSpace_Sight, kVanillaSpace_UnderBarrel,
        kVanillaSpace_Option, kVanillaSpace_Option
    };
    for (int i = 0; i < 13; ++i)
    {
        row.logical[i]   = row.f[i];
        row.slotSpace[i] = -1;
    }
    for (int i = 0; i < 11; ++i)
        row.slotSpace[i + 1] = kSlotSpaces[i];

    for (int i = 0; i < 11; ++i)
    {
        if (kSlotSpaces[i] < 0 || row.f[i + 1] < 0x100)
            continue;
        if (kSlotSpaces[i] == kVanillaSpace_Receiver)
        {
            const int wideRc = row.f[i + 1];
            const int donor  = EquipParam_GetWideReceiverDonor(wideRc);
            row.f[i + 1] = (donor > 0) ? donor : kEssentialDefaultId;
            if (donor <= 0)
                LogDebug("[GunBasic] SetGunBasic weaponId=%d: receiverId=%d does "
                         "not fit the one-byte row field and has no motionFrom "
                         "donor - falling back to vanilla receiver %d, so the "
                         "weapon takes THAT receiver's motion type, reload clips "
                         "and sound root. Give receiverId=%d a motionFrom, and call "
                         "SetReceiver before SetGunBasic\n",
                    weaponId, wideRc, kEssentialDefaultId, wideRc);
            continue;
        }
        row.f[i + 1] = EquipParam_ResolvePartByte(kSlotSpaces[i], row.f[i + 1]);
    }

    for (int i = 1; i <= 3; ++i)
    {
        if (row.slotSpace[i] == kVanillaSpace_Receiver
            && row.logical[i] >= 0x100)
            continue;   // deferred to the late bind, not a missing part
        if (row.f[i] <= 0)
        {
            LogDebug("[GunBasic] SetGunBasic weaponId=%d: %s did not resolve to a "
                     "valid part byte - substituting vanilla default id %d\n",
                weaponId, kEssentialLabel[i - 1], kEssentialDefaultId);
            row.f[i] = kEssentialDefaultId;
        }
    }

    for (int i = 1; i <= 12; ++i)
    {
        if (row.f[i] > 0xFF)
        {
            LogDebug("[GunBasic] SetGunBasic weaponId=%d: field #%d value %d "
                     "exceeds 255 - gunBasic part fields are one byte; truncated\n",
                weaponId, i, row.f[i]);
        }
    }

    const int cap = BufferSlotCount();
    std::uint8_t* buf = BufferBase();

    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
        bool replaced = false;
        for (auto& existing : g_Rows)
        {
            if (existing.f[0] == row.f[0])
            {
                existing = row;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            g_Rows.push_back(row);

        if (buf && weaponId >= 1 && weaponId <= cap)
        {
            const bool vanillaRow = weaponId <= StockSlotCount()
                && !g_ClaimedIds.count(weaponId);
            const std::uint8_t* rowPtr =
                buf + static_cast<size_t>(weaponId - 1) * 12;
            if (vanillaRow)
                EquipParam_VanillaPreWrite(kVanillaSpace_Weapon, weaponId,
                                           rowPtr, 12);
            WriteNativeRowSEH(buf, weaponId, &row.f[1]);
            if (vanillaRow)
                EquipParam_VanillaPostWrite(kVanillaSpace_Weapon, weaponId,
                                            rowPtr, 12);
        }
        else
        {
            LogDebug("[GunBasic] SetGunBasic weaponId=%d out of native buffer range "
                     "[1,%d] (or the buffer is unresolved) - NOT written; declare "
                     "the WP via V_TppEquip.DeclareWPs to get an in-range slot\n", weaponId, cap);
        }

        const int donorRc = EquipParam_GetWideReceiverDonor(row.f[1]);
        if (buf && donorRc > 0 && donorRc != row.f[1])
        {
            const int stock = StockSlotCount();
            int donorBa = 0, donorAm = 0, donorSt = 0;
            for (int id = 1; id <= stock && id <= cap; ++id)
            {
                const std::uint8_t* r =
                    buf + static_cast<size_t>(id - 1) * 12;
                if (r[0] != static_cast<std::uint8_t>(donorRc))
                    continue;
                donorBa = r[1];
                donorAm = r[2];
                donorSt = r[6];
                break;
            }
            int eligible = 0;
            const int applied = EquipParam_InheritPartMotionTypes(
                row.f[2], donorBa, row.f[3], donorAm, row.f[7], donorSt, &eligible);
            if (eligible > 0 && applied == 0)
                LogDebug("[ChimeraMotion] weaponId=%d: %d custom part(s) could "
                         "inherit an anim type but motionFrom donor receiver %d has "
                         "none (its vanilla row uses ba=%d am=%d st=%d, all type 0) "
                         "- the parts control gets no motion archive and the "
                         "slide/magazine stay frozen\n",
                    weaponId, eligible, donorRc, donorBa, donorAm, donorSt);
        }
    }

#ifdef _DEBUG
    LogDebug("[GunBasic] SetGunBasic weaponId=%d rc=%d ba=%d am=%d grade=%d -> native slot\n",
        row.f[0], row.f[1], row.f[2], row.f[3], row.f[12]);
#endif
    return 0;
}

bool Install_TppEquip_ReloadEquipParameterTables2_Hook()
{
    void* target = ResolveGameAddress(gAddr.ReloadEquipParameterTables2);
    if (!target)
    {
        LogDebug("[GunBasic] ReloadEquipParameterTables2 address not set for this build - reapply guard skipped\n");
        return true;
    }

    const bool ok = CreateAndEnableHook(
        target, &hkReloadEquipParameterTables2,
        reinterpret_cast<void**>(&g_OrigReload));
    if (!ok)
    {
        Log("[GunBasic] Reload reapply-guard hook Install -> FAIL (target=%p)\n", target);
    }
#ifdef _DEBUG
    else
    {
        LogDebug("[GunBasic] Reload reapply-guard hook Install -> OK (target=%p)\n", target);
    }
#endif
    return ok;
}

bool Uninstall_TppEquip_ReloadEquipParameterTables2_Hook()
{
    if (gAddr.ReloadEquipParameterTables2)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.ReloadEquipParameterTables2));
    g_OrigReload = nullptr;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    g_Rows.clear();
    g_WpNameToId.clear();
    g_ClaimedIds.clear();
    g_ReservedIds.clear();
    g_PersistedReserved = false;
    return true;
}
