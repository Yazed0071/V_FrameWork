#include "pch.h"
#include "EquipPartParams.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AddressSet.h"
#include "EquipIdCompression.h"
#include "FoxHashes.h"
#include "../../core/FoxPathInternal.h"
#include "GunBasicInject.h"
#include "PartIdWiden.h"
#include "HookUtils.h"
#include "log.h"
#include "LuaApi.h"
#include "BulletLockOn.h"
#include "../../lua/V_TppEquipLib.h"
#include "BulletMultiShot.h"
#include "TppEquip_ReloadEquipIdTable.h"
#include "TppEquipConstRegistry.h"
#include "../../core/V_FrameWorkState.h"

namespace outfit { void EndBootRestoreScrub(const char* reason); }

namespace
{
    struct PartBuffer
    {
        const char* name;
        std::ptrdiff_t implOffset;
        int stride;
        int stockCount;
        int maxId;
        std::vector<std::uint8_t> shadow;
        bool active;
        int nextId;
        std::map<std::string, int> nameToId;
        int space = -1;
        std::uint8_t* origBuf = nullptr;
    };

    static PartBuffer g_Magazine = { "magazine", 0x20, 8, 191, 255, {}, false, 192, {}, kVanillaSpace_Magazine };
    static PartBuffer g_Stock    = { "stock",    0x40, 2, 42,  255, {}, false, 43,  {}, kVanillaSpace_Stock };
    static PartBuffer g_Muzzle   = { "muzzleOption", 0x28, 3, 39, 255, {}, false, 40, {}, kVanillaSpace_MuzzleOption };
    static PartBuffer g_Sight    = { "sight",    0x38, 5, 24,  255, {}, false, 25,  {}, kVanillaSpace_Sight };
    static PartBuffer g_Barrel   = { "barrel",   0x18, 2, 114, 255, {}, false, 115, {}, kVanillaSpace_Barrel };
    static PartBuffer g_UnderBarrel = { "underBarrel", 0x48, 3, 22, 255, {}, false, 23, {}, kVanillaSpace_UnderBarrel };
    static PartBuffer g_Option   = { "option",   0x30, 1, 9,  255, {}, false, 10,  {}, kVanillaSpace_Option };
    static PartBuffer g_Bullet   = { "bullet",   0x50, 14, 112, 255, {}, false, 113, {}, -1 };

    static std::recursive_mutex g_Mutex;

    static void** PartPtrLoc(const PartBuffer& pb)
    {
        auto* impl = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipParameterTablesImpl_Instance));
        if (!impl)
            return nullptr;
        return reinterpret_cast<void**>(impl + pb.implOffset);
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

    static int InitShadowSEH(void** loc, std::uint8_t* shadow, size_t copyBytes)
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

    static bool EnsurePartShadow(PartBuffer& pb)
    {
        if (pb.active)
            return true;

        void** loc = PartPtrLoc(pb);
        if (!loc)
            return false;

        std::uint8_t* stockBuf = ReadPtrSEH(loc);

        if (pb.shadow.empty())
            pb.shadow.assign(static_cast<size_t>(pb.maxId) * pb.stride, 0);

        if (InitShadowSEH(loc, pb.shadow.data(),
                          static_cast<size_t>(pb.stockCount) * pb.stride) != 1)
        {
            pb.shadow.clear();
            return false;
        }

        pb.origBuf = stockBuf;
        pb.active = true;
        LogDebug("[EquipParam] %s shadow active (stock %d -> %d slots; custom ids %d..%d)\n",
            pb.name, pb.stockCount, pb.maxId, pb.stockCount + 1, pb.maxId);
        return true;
    }

    static std::uint8_t* PartCurrentBuf(PartBuffer& pb)
    {
        return ReadPtrSEH(PartPtrLoc(pb));
    }

    static int ProbeSupplyCountLocSEH(void* getQuarkFn, void*** outLoc,
                                      std::uint8_t** outCur)
    {
        __try
        {
            using GetQuark_t = std::uint8_t* (__fastcall*)();
            std::uint8_t* quark = reinterpret_cast<GetQuark_t>(getQuarkFn)();
            if (!quark)
                return 0;
            std::uint8_t* app = *reinterpret_cast<std::uint8_t**>(quark + 0x98);
            if (!app)
                return 0;
            std::uint8_t* holder = *reinterpret_cast<std::uint8_t**>(app + 0x1e8);
            if (!holder)
                return 0;
            std::uint8_t* paramObj = *reinterpret_cast<std::uint8_t**>(holder + 0x10);
            if (!paramObj)
                return 0;
            void** loc = reinterpret_cast<void**>(paramObj + 0xa0);
            *outCur = static_cast<std::uint8_t*>(*loc);
            *outLoc = loc;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int WritePtrSEH(void** loc, void* value)
    {
        __try
        {
            *loc = value;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int CompareMagazineRowsSEH(const std::uint8_t* a, const std::uint8_t* b,
                                      int rows, int* fullEqual)
    {
        __try
        {
            *fullEqual =
                (std::memcmp(a, b, static_cast<size_t>(rows) * 8) == 0) ? 1 : 0;
            if (*fullEqual)
                return rows;
            int match = 0;
            for (int i = 0; i < rows; ++i)
            {
                const std::uint8_t* ra = a + static_cast<size_t>(i) * 8;
                const std::uint8_t* rb = b + static_cast<size_t>(i) * 8;
                if (*reinterpret_cast<const std::uint16_t*>(ra + 0)
                        == *reinterpret_cast<const std::uint16_t*>(rb + 0)
                    && *reinterpret_cast<const std::uint16_t*>(ra + 4)
                        == *reinterpret_cast<const std::uint16_t*>(rb + 4))
                    ++match;
            }
            return match;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static int DumpRowBytesSEH(const std::uint8_t* p, char* out, size_t outLen)
    {
        __try
        {
            for (int i = 0; i < 16 && (static_cast<size_t>(i) * 3 + 3) < outLen; ++i)
                std::snprintf(out + i * 3, 4, "%02X ", p[i]);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void HealSupplyAmmoCountTable()
    {
        void* getQuark = ResolveGameAddress(gAddr.GetQuarkSystemTable);
        if (!getQuark)
            return;
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
        if (!g_Magazine.active || g_Magazine.shadow.empty() || !g_Magazine.origBuf)
            return;
        void** loc = nullptr;
        std::uint8_t* cur = nullptr;
        if (ProbeSupplyCountLocSEH(getQuark, &loc, &cur) != 1 || !loc || !cur)
            return;
        if (cur == g_Magazine.shadow.data())
            return;
        const int rows = g_Magazine.stockCount;
        bool confirmed = (cur == g_Magazine.origBuf);
        int fullEq = confirmed ? 1 : 0;
        int match = confirmed ? rows : 0;
        if (!confirmed)
        {
            match = CompareMagazineRowsSEH(cur, g_Magazine.origBuf, rows, &fullEq);
            confirmed = (match >= rows - 8);
        }
        if (confirmed)
        {
            if (WritePtrSEH(loc, g_Magazine.shadow.data()) == 1)
            {
                static std::atomic<bool> s_healLogged{ false };
                if (!s_healLogged.exchange(true))
                    LogDebug("[EquipParam] supply ammo-count table healed: the supply "
                        "refill reads its amounts through a magazine-data pointer at "
                        "supplyParams+0xa0 that held %p (content-verified against the "
                        "stock table: %d/%d rows match, fullEqual=%d) - a vanilla-only "
                        "copy the shadow redirect never touched, so custom ammoIds "
                        "%d..%d read past its %d rows and every ammo supply granted 0. "
                        "Repointed to the live shadow %p.\n",
                        static_cast<void*>(cur), match, rows, fullEq,
                        g_Magazine.stockCount + 1, g_Magazine.maxId, rows,
                        static_cast<void*>(g_Magazine.shadow.data()));
            }
            return;
        }
        static std::atomic<bool> s_probeLogged{ false };
        if (!s_probeLogged.exchange(true))
        {
            char curHex[64] = {};
            char stockHex[64] = {};
            DumpRowBytesSEH(cur, curHex, sizeof(curHex));
            DumpRowBytesSEH(g_Magazine.origBuf, stockHex, sizeof(stockHex));
            LogDebug("[EquipParam] supply ammo-count probe: ptr at supplyParams+0xa0 is "
                "%p (stock=%p shadow=%p) and its content does NOT match the magazine "
                "table (%d/%d rows) - left untouched. first 16 bytes there: %s| stock: "
                "%s\n",
                static_cast<void*>(cur), static_cast<void*>(g_Magazine.origBuf),
                static_cast<void*>(g_Magazine.shadow.data()), match, rows,
                curHex, stockHex);
        }
    }

    struct WidePartState
    {
        std::vector<std::uint8_t> row;
        bool rowSet = false;
        int alias = 0;
        int pendingUbType = 0;
        int motionFrom = 0;
        int motionType = -1;
    };
    static const int kWideIdBase = 0x100;
    static const int kAliasReserve = 16;
    static const int kRcvAliasReserve = 12;
    static std::map<int, WidePartState> g_WideParts[kVanillaSpace_Count];
    static int g_WideNext[kVanillaSpace_Count];
    static int ResolvePartByteLocked(int space, int id);
    static WidePartState* WideStateFor(int space, int id);

    static int AllocateWidePartId(PartBuffer& pb, const char* name)
    {
        if (pb.space < 0 || pb.space >= kVanillaSpace_Count)
            return 0;
        if (g_WideNext[pb.space] < kWideIdBase)
            g_WideNext[pb.space] = kWideIdBase;
        const int id = g_WideNext[pb.space]++;
        pb.nameToId[name] = id;
        g_WideParts[pb.space][id];
        LogDebug("[EquipParam] '%s' -> %s id %d (WIDE: the engine byte lane is nearly "
            "full, so this part lives past it; a lane byte is bound the first time "
            "a weapon references it)\n",
            name, pb.name, id);
        return id;
    }

    static int AllocatePartSlot(PartBuffer& pb, const char* name)
    {
        if (!name || !name[0])
            return 0;

        auto it = pb.nameToId.find(name);
        if (it != pb.nameToId.end())
            return it->second;

        if (!EnsurePartShadow(pb))
            return 0;

        if (pb.nextId <= pb.stockCount)
        {
            LogDebug("[EquipParam] %s allocator floor was %d, at or below the %d stock rows - "
                "raised to %d; a custom part id inside the stock range overwrites a vanilla "
                "part and breaks every vanilla weapon built on it\n",
                pb.name, pb.nextId, pb.stockCount, pb.stockCount + 1);
            pb.nextId = pb.stockCount + 1;
        }

        if (!PartIdWiden_IsActive()
            && pb.nextId > pb.maxId - kAliasReserve && pb.space >= 0)
            return AllocateWidePartId(pb, name);

        if (PartIdWiden_IsActive() && pb.space == kVanillaSpace_Option)
        {
            while (pb.nextId <= pb.maxId && (pb.nextId & 0xFF) == 6)
                ++pb.nextId;
        }

        if (pb.nextId > pb.maxId)
        {
            LogDebug("[EquipParam] %s custom id space exhausted (%d..%d) for '%s'\n",
                pb.name, pb.stockCount + 1, pb.maxId, name);
            return 0;
        }

        const int id = pb.nextId++;
        pb.nameToId[name] = id;
        LogDebug("[EquipParam] '%s' -> %s id %d (shadow slot)\n", name, pb.name, id);
        return id;
    }

    static WidePartState* WideStateFor(int space, int id)
    {
        if (space < 0 || space >= kVanillaSpace_Count || id < kWideIdBase)
            return nullptr;
        auto it = g_WideParts[space].find(id);
        return (it != g_WideParts[space].end()) ? &it->second : nullptr;
    }

    static std::uint8_t* WideRowFor(PartBuffer& pb, int id, int& writeId)
    {
        if (pb.space < 0)
            return nullptr;
        auto& w = g_WideParts[pb.space][id];
        if (w.row.size() != static_cast<size_t>(pb.stride))
            w.row.assign(static_cast<size_t>(pb.stride), 0);
        w.rowSet = true;
        writeId = 1;
        return w.row.data();
    }

    static void SyncWideRowToAlias(PartBuffer& pb, int id)
    {
        WidePartState* w = WideStateFor(pb.space, id);
        if (!w || !w->alias || w->row.empty())
            return;
        std::uint8_t* buf = PartCurrentBuf(pb);
        if (buf)
            std::memcpy(buf + static_cast<size_t>(w->alias - 1) * pb.stride,
                        w->row.data(), static_cast<size_t>(pb.stride));
    }

    static int Clamp(int v, int lo, int hi)
    {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
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

    static bool ReadNamedIntAlias(lua_State* L, int tableIdx, const char* name,
                                  const char* legacy, int& out)
    {
        if (ReadNamedInt(L, tableIdx, name, out))
            return true;
        return ReadNamedInt(L, tableIdx, legacy, out);
    }

    static bool ReadNamedFloat(lua_State* L, int tableIdx, const char* name, double& out)
    {
        g_lua_getfield(L, tableIdx, const_cast<char*>(name));
        const bool ok = g_lua_isnumber(L, -1) != 0;
        if (ok)
            out = static_cast<double>(g_lua_tonumber(L, -1));
        g_lua_settop(L, -2);
        return ok;
    }

    static int ReadNamedBoolTri(lua_State* L, int tableIdx, const char* name)
    {
        g_lua_getfield(L, tableIdx, const_cast<char*>(name));
        const int t = g_lua_type(L, -1);
        int v = -1;
        if (t == LUA_TBOOLEAN)
            v = g_lua_toboolean(L, -1) != 0 ? 1 : 0;
        else if (t == LUA_TNUMBER)
            v = g_lua_tointeger(L, -1) != 0 ? 1 : 0;
        g_lua_settop(L, -2);
        return v;
    }

    static int ScaleByte(double v, double scale)
    {
        return Clamp(static_cast<int>(v * scale + 0.5), 0, 255);
    }

    static const char* const kBulletBaseNames[] = {
        "tranqNear", "tranqFar", "tranqResidual",
        "damageNear", "damageFar", "damageResidual",
        "impactNear", "impactFar", "impactResidual",
        "penNear", "penFar", "penSwitchDistance", nullptr };
    static const char* const kBarrelBaseNames[] = {
        "fireRateMult", "unk2", "gunAimAdjustMult", "rangeMult",
        "rangeUIMult", "spreadMaxMult", "percentOverride", nullptr };
    static const char* const kRcvBaseNames[] = {
        "fireRate", "aimAssistDist", "gunAimAdjust", "effectiveRange",
        "effectiveRangeUI", "adsZoom", "adsFov", "reloadSpeed", nullptr };
    static const char* const kRcvWobNames[] = {
        "spreadPerShot", "unk2", "spreadRecovery", "spreadMin",
        "spreadMax", "shotKick", "shotKick2", nullptr };
    static const char* const kRcvSysNames[] = {
        "eqpType", "reticleUiId", "triggerId",
        "showMagazineMesh", "plusOneChamber", "missileMeshVariant",
        "modelDedupExclude", "flag5", "sightMountMesh",
        "railMountMesh", "railMountMesh2", "altMagazineSocket", nullptr };

    static int ReadNumberOrFloatTable(lua_State* L, int tableIdx, const char* field,
                                      int& num, double* vals, int maxN, int& count,
                                      const char* const* names = nullptr)
    {
        g_lua_getfield(L, tableIdx, const_cast<char*>(field));
        const int t = g_lua_type(L, -1);
        if (t == LUA_TNUMBER)
        {
            num = static_cast<int>(g_lua_tointeger(L, -1));
            g_lua_settop(L, -2);
            return 1;
        }
        if (t == LUA_TTABLE)
        {
            count = 0;
            const int n = static_cast<int>(g_lua_objlen(L, -1));
            for (int i = 1; i <= n && count < maxN; ++i)
            {
                g_lua_rawgeti(L, -1, i);
                if (g_lua_isnumber(L, -1))
                    vals[count++] = static_cast<double>(g_lua_tonumber(L, -1));
                g_lua_settop(L, -2);
            }
            if (names)
            {
                for (int i = 0; i < maxN && names[i]; ++i)
                {
                    g_lua_getfield(L, -1, const_cast<char*>(names[i]));
                    if (g_lua_isnumber(L, -1))
                    {
                        vals[i] = static_cast<double>(g_lua_tonumber(L, -1));
                        if (count < i + 1) count = i + 1;
                    }
                    g_lua_settop(L, -2);
                }
            }
            g_lua_settop(L, -2);
            return 2;
        }
        g_lua_settop(L, -2);
        return 0;
    }

    static std::string ReadNamedStringOrTable1(lua_State* L, int tableIdx, const char* name)
    {
        std::string out;
        g_lua_getfield(L, tableIdx, const_cast<char*>(name));
        const int t = g_lua_type(L, -1);
        if (t == LUA_TSTRING)
        {
            size_t len = 0;
            const char* s = g_lua_tolstring(L, -1, &len);
            if (s) out.assign(s, len);
        }
        else if (t == LUA_TTABLE)
        {
            g_lua_rawgeti(L, -1, 1);
            if (g_lua_type(L, -1) == LUA_TSTRING)
            {
                size_t len = 0;
                const char* s = g_lua_tolstring(L, -1, &len);
                if (s) out.assign(s, len);
            }
            g_lua_settop(L, -2);
        }
        g_lua_settop(L, -2);
        return out;
    }

    static std::vector<std::uint64_t> g_TrailShadow;
    static bool g_TrailShadowActive = false;

    static int ScanTrailListSEH(std::uint64_t** basePtr, std::uint32_t* countPtr,
                                std::uint64_t*& base, std::uint32_t& count,
                                std::uint64_t pathHash, int& found)
    {
        __try
        {
            base  = *basePtr;
            count = *countPtr;
            found = -1;
            if (!base || count == 0 || count > 0x1000)
                return 0;
            for (std::uint32_t i = 0; i < count; ++i)
                if (base[i] == pathHash) { found = static_cast<int>(i); break; }
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void RedirectTrailListSEH(std::uint64_t** basePtr, std::uint32_t* countPtr,
                                     std::uint64_t* newBase, std::uint32_t newCount)
    {
        __try
        {
            *basePtr  = newBase;
            *countPtr = newCount;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static int ResolveTrailPathIndex(std::uint64_t pathHash)
    {
        if (pathHash == 0)
            return -1;
        auto* impl = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipParameterTablesImpl_Instance));
        if (!impl)
            return -1;

        auto** basePtr  = reinterpret_cast<std::uint64_t**>(impl + 0x88);
        auto*  countPtr = reinterpret_cast<std::uint32_t*>(impl + 0x90);

        std::uint64_t* base = nullptr;
        std::uint32_t  count = 0;
        int found = -1;
        if (ScanTrailListSEH(basePtr, countPtr, base, count, pathHash, found) != 1)
            return -1;
        if (found >= 0)
            return found;

        if (!g_TrailShadowActive)
        {
            g_TrailShadow.assign(base, base + count);
            g_TrailShadow.reserve(count + 64);
            g_TrailShadowActive = true;
        }
        g_TrailShadow.push_back(pathHash);
        const int idx = static_cast<int>(g_TrailShadow.size() - 1);
        RedirectTrailListSEH(basePtr, countPtr, g_TrailShadow.data(),
                             static_cast<std::uint32_t>(g_TrailShadow.size()));
        return idx;
    }

    static void WriteMagazineSEH(std::uint8_t* buf, int ammoId,
                                 int eqpAmmoId, int magCapacity,
                                 int totalCarry, int bulletId)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(ammoId - 1) * 8;
            *reinterpret_cast<std::uint16_t*>(p + 0) =
                static_cast<std::uint16_t>(eqpAmmoId & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 2) =
                static_cast<std::uint16_t>(Clamp(magCapacity, 0, 0x3FF));
            *reinterpret_cast<std::uint16_t*>(p + 4) =
                static_cast<std::uint16_t>(Clamp(totalCarry <= 0 ? 0x3FFF : totalCarry, 0, 0x3FFF));
            p[6] = static_cast<std::uint8_t>(bulletId & 0xFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteStockSEH(std::uint8_t* buf, int stockId, int mod2, int mod3)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(stockId - 1) * 2;
            p[0] = static_cast<std::uint8_t>(mod2);
            p[1] = static_cast<std::uint8_t>(mod3);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteMuzzleSEH(std::uint8_t* buf, int muzzleOptionId,
                               int grouping, int durability, int suppressor)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(muzzleOptionId - 1) * 3;
            p[0] = static_cast<std::uint8_t>(grouping);
            p[1] = static_cast<std::uint8_t>(durability & 0xFF);
            p[2] = static_cast<std::uint8_t>(suppressor & 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteOptionSEH(std::uint8_t* buf, int optionId, int isLight, int isLaser)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(optionId - 1);
            p[0] = static_cast<std::uint8_t>((p[0] & 0xFC) | (isLight ? 1 : 0) | (isLaser ? 2 : 0));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteUnderBarrelSEH(std::uint8_t* buf, int underBarrelId,
                                    int receiverId, int magazineId, int weaponGrade)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(underBarrelId - 1) * 3;
            p[0] = static_cast<std::uint8_t>(receiverId & 0xFF);
            p[1] = static_cast<std::uint8_t>(magazineId & 0xFF);
            p[2] = static_cast<std::uint8_t>(weaponGrade & 0xFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteBarrelSEH(std::uint8_t* buf, int barrelId, int baseIdx,
                               int barrelLength, int scopeMount, int unkBit6, int sideMount,
                               int underMount)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(barrelId - 1) * 2;
            std::uint8_t b0 = static_cast<std::uint8_t>(barrelLength & 0x0f);
            if (underMount) b0 |= 0x10;
            if (scopeMount) b0 |= 0x20;
            if (unkBit6)    b0 |= 0x40;
            if (sideMount)  b0 |= 0x80;
            p[0] = b0;
            p[1] = static_cast<std::uint8_t>(baseIdx & 0xFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteBulletRowSEH(std::uint8_t* buf, int bulletId,
                                  const int u16v[3], const int u8v[7],
                                  int flagD, int typeD)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(bulletId - 1) * 14;
            for (int k = 0; k < 3; ++k)
                *reinterpret_cast<std::uint16_t*>(p + k * 2) =
                    static_cast<std::uint16_t>(u16v[k] & 0xFFFF);
            p[0x06] = static_cast<std::uint8_t>(u8v[0] & 0xFF);
            p[0x07] = static_cast<std::uint8_t>(u8v[1] & 0xFF);
            p[0x08] = static_cast<std::uint8_t>(u8v[2] & 0xFF);
            p[0x09] = static_cast<std::uint8_t>(u8v[3] & 0xFF);
            p[0x0a] = static_cast<std::uint8_t>(u8v[4] & 0xFF);
            p[0x0b] = static_cast<std::uint8_t>(u8v[5] & 0xFF);
            p[0x0c] = static_cast<std::uint8_t>(u8v[6] & 0xFF);
            p[0x0d] = static_cast<std::uint8_t>((flagD ? 1 : 0) | ((typeD & 0x1f) << 1));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteBarrelBaseRowSEH(std::uint8_t* pool, int idx, const double* v, int n)
    {
        __try
        {
            std::uint8_t* p = pool + static_cast<size_t>(idx) * 7;
            for (int k = 0; k < 7; ++k) p[k] = 0;
            for (int k = 0; k < 7 && k < n; ++k)
                p[k] = static_cast<std::uint8_t>(Clamp(static_cast<int>(v[k] * 100.0 + 0.5), 0, 255));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteBulletBaseRowSEH(std::uint8_t* pool, int idx, const double* v, int n)
    {
        __try
        {
            std::uint8_t* p = pool + static_cast<size_t>(idx) * 22;
            for (int k = 0; k < 22; ++k) p[k] = 0;
            if (n >  0) *reinterpret_cast<std::uint16_t*>(p + 0x00) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[0]  * 10.0  + 0.5), 0, 65535));
            if (n >  1) *reinterpret_cast<std::uint16_t*>(p + 0x02) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[1]  * 10.0  + 0.5), 0, 65535));
            if (n >  2) *reinterpret_cast<std::uint16_t*>(p + 0x04) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[2]  * 100.0 + 0.5), 0, 65535));
            if (n >  3) *reinterpret_cast<std::uint16_t*>(p + 0x06) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[3]  * 10.0  + 0.5), 0, 65535));
            if (n >  4) *reinterpret_cast<std::uint16_t*>(p + 0x08) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[4]  * 10.0  + 0.5), 0, 65535));
            if (n >  5) *reinterpret_cast<std::uint16_t*>(p + 0x0a) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[5]  * 100.0 + 0.5), 0, 65535));
            if (n >  6) *reinterpret_cast<std::uint16_t*>(p + 0x0c) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[6]  * 10.0  + 0.5), 0, 65535));
            if (n >  7) *reinterpret_cast<std::uint16_t*>(p + 0x0e) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[7]  * 10.0  + 0.5), 0, 65535));
            if (n >  8) *reinterpret_cast<std::uint16_t*>(p + 0x10) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[8]  * 100.0 + 0.5), 0, 65535));
            if (n >  9) p[0x12] = static_cast<std::uint8_t>(Clamp(static_cast<int>(v[9]  + 0.5), 0, 255));
            if (n > 10) p[0x13] = static_cast<std::uint8_t>(Clamp(static_cast<int>(v[10] + 0.5), 0, 255));
            if (n > 11) *reinterpret_cast<std::uint16_t*>(p + 0x14) = static_cast<std::uint16_t>(Clamp(static_cast<int>(v[11] * 10.0  + 0.5), 0, 65535));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteSightSEH(std::uint8_t* buf, int scopeId,
                             int zoom1, int zoom2, int zoom3, int scopeUiId, int flags)
    {
        __try
        {
            std::uint8_t* p = buf + static_cast<size_t>(scopeId - 1) * 5;
            p[0] = static_cast<std::uint8_t>(zoom1);
            p[1] = static_cast<std::uint8_t>(zoom2);
            p[2] = static_cast<std::uint8_t>(zoom3);
            p[3] = static_cast<std::uint8_t>(scopeUiId);
            p[4] = static_cast<std::uint8_t>(flags & 0xFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static const int kReceiverImplOffset = 0x10;
    static const int kReceiverStride     = 6;
    static const int kReceiverCapacity   = 233;
    static const int kReceiverMaxId      = 255;
    static PartBuffer g_RcvIdBuf = { "receiverBuffer", 0x10, 6, 233, 255, {}, false, 234, {}, kVanillaSpace_Receiver };

    struct RcvPool
    {
        PartBuffer pb;
        int rowByteOffset;
        bool counted;
    };
    static RcvPool g_Base = { { "receiverParamSetsBase",     0x58, 12, 0, 65536, {}, false, 0, {} }, 2, false };
    static RcvPool g_Wob  = { { "receiverParamSetsWobbling", 0x60, 14, 0, 65536, {}, false, 0, {} }, 3, false };
    static RcvPool g_Sys  = { { "receiverParamSetsSystem",   0x68,  3, 0, 65536, {}, false, 0, {} }, 4, false };
    static RcvPool g_Snd  = { { "receiverParamSetsSound",    0x70,  8, 0, 65536, {}, false, 0, {} }, 5, false };

    enum RcvPoolKind { POOL_BASE, POOL_WOB, POOL_SYS, POOL_SND };

    static RcvPool& PoolFor(RcvPoolKind k)
    {
        switch (k)
        {
        case POOL_WOB: return g_Wob;
        case POOL_SYS: return g_Sys;
        case POOL_SND: return g_Snd;
        default:       return g_Base;
        }
    }


    struct FireSoundSpec
    {
        std::string text;
        int mSeg = -1;
        bool isEvent = false;
        std::string supText;
        bool supIsEvent = false;
    };
    static std::mutex g_FireSoundMutex;
    static std::map<int, FireSoundSpec> g_FireSoundByRow;

    static bool ReadSndRowSEH(const std::uint8_t* pool, int idx, char out[9])
    {
        __try
        {
            const std::uint8_t* p = pool + static_cast<std::size_t>(idx) * 8;
            for (int i = 0; i < 8; ++i)
                out[i] = static_cast<char>(p[i]);
            out[8] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ReadReceiverSndIndexSEH(const std::uint8_t* rbuf, int receiverId,
                                        int& outIdx)
    {
        __try
        {
            outIdx = rbuf[static_cast<std::size_t>(receiverId) * kReceiverStride
                          + g_Snd.rowByteOffset];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ResolveDonorSoundRoot(int donorReceiverId, std::string& out)
    {
        out.clear();
        if (donorReceiverId <= 0 || donorReceiverId > kReceiverMaxId)
            return false;

        const std::uint8_t* rbuf = PartCurrentBuf(g_RcvIdBuf);
        const std::uint8_t* pool = PartCurrentBuf(g_Snd.pb);
        if (!rbuf || !pool)
            return false;

        int sndIdx = 0;
        if (!ReadReceiverSndIndexSEH(rbuf, donorReceiverId, sndIdx))
            return false;

        char root[9] = {};
        if (!ReadSndRowSEH(pool, sndIdx, root))
            return false;
        if (root[0] == '\0')
            return false;

        out.assign(root);
        return true;
    }

    static void RegisterFireSoundSupOverride(int idx, const std::string& text,
                                             bool isEvent, const char* field)
    {
        if (text.empty())
            return;
        if (gAddr.WeaponSystem_DefineWeaponFireSound == 0)
        {
            LogDebug("[EquipParam] SetReceiver: %s suppressed-sound override needs the "
                "fire-sound hook, unavailable on this build - vanilla suppressed "
                "sound kept\n", field);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_FireSoundMutex);
            FireSoundSpec& spec = g_FireSoundByRow[idx];
            spec.supText = text;
            spec.supIsEvent = isEvent;
        }
        if (isEvent)
            LogDebug("[EquipParam] SetReceiver: %s suppressed-sound event='%s' "
                "(played verbatim)\n", field, text.c_str());
        else
            LogDebug("[EquipParam] SetReceiver: %s suppressed-sound root='%s' "
                "(plays sfx_w_p_%s_sup_active)\n", field, text.c_str(), text.c_str());
    }

    static void RegisterFireSoundOverride(int idx, const std::string& text,
                                          int mSeg, bool isEvent, const char* field)
    {
        const bool needOverride = isEvent || text.size() > 7 || mSeg >= 0;
        if (!needOverride)
            return;
        if (gAddr.WeaponSystem_DefineWeaponFireSound == 0)
        {
            if (isEvent || text.size() > 7)
                LogDebug("[EquipParam] SetReceiver: %s '%s' needs the fire-sound override, "
                    "unavailable on this build - falling back to the 7-char row\n",
                    field, text.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_FireSoundMutex);
            g_FireSoundByRow[idx] = FireSoundSpec{ text, mSeg, isEvent };
        }
        if (isEvent)
            LogDebug("[EquipParam] SetReceiver: %s fire-sound event='%s' (played verbatim; "
                "the suppressed sound stays vanilla unless supEvent/sup is given)\n",
                field, text.c_str());
        else
            LogDebug("[EquipParam] SetReceiver: %s fire-sound override root='%s' _m=%s\n",
                field, text.c_str(),
                mSeg == 1 ? "on" : mSeg == 2 ? "off" : "weapon-default");
    }

    struct RefPool
    {
        PartBuffer pb;
        int refImplOffset;
        int refStride;
        int refCapacity;
        int refByteOffset;
        bool counted;
    };
    // barrelParamSetsBase (impl+0x78, 7 bytes) <- barrel buffer (impl+0x18, stride 2) byte 1
    static RefPool g_BarrelBase = { { "barrelParamSetsBase", 0x78, 7,  0, 65536, {}, false, 0, {} }, 0x18, 2, 114, 1, false };
    // bulletParamSetsBase (impl+0x80, 22 bytes) <- bullet buffer (impl+0x50, stride 14) byte 6
    static RefPool g_BulletBase = { { "bulletParamSetsBase", 0x80, 22, 0, 65536, {}, false, 0, {} }, 0x50, 14, 112, 6, false };

    static void EnsureOnePoolRelocated(PartBuffer& pb)
    {
        if (!pb.active || pb.shadow.empty())
            return;
        void** loc = PartPtrLoc(pb);
        if (!loc)
            return;
        std::uint8_t* cur = ReadPtrSEH(loc);
        if (!cur || cur == pb.shadow.data())
            return;
        if (InitShadowSEH(loc, pb.shadow.data(),
                          static_cast<size_t>(pb.stockCount) * pb.stride) == 1)
            LogDebug("[EquipParam] %s shadow re-established: an equip-table reload re-pointed "
                "it to a fresh stock buffer, orphaning our custom rows - the reloaded "
                "weapon's receiver/part params were reading as zero (single-shot, wrong "
                "fire rate, null fire-sound root); stock rows re-synced, custom rows "
                "preserved\n", pb.name);
    }

    static bool RestampRcvRowSEH(std::uint8_t* dst, const std::uint8_t* src,
                                 int stride)
    {
        __try
        {
            if (std::memcmp(dst, src, static_cast<size_t>(stride)) == 0)
                return false;
            std::memcpy(dst, src, static_cast<size_t>(stride));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void ReassertWideReceiverLanes()
    {
        const int space = g_RcvIdBuf.space;
        if (space < 0 || space >= kVanillaSpace_Count)
            return;
        std::uint8_t* buf = PartCurrentBuf(g_RcvIdBuf);
        if (!buf)
            return;

        int repaired = 0;
        int firstLane = 0;
        for (auto& kv : g_WideParts[space])
        {
            WidePartState& w = kv.second;
            if (!w.alias
                || w.row.size() != static_cast<size_t>(g_RcvIdBuf.stride))
                continue;
            std::uint8_t* dst = buf
                + static_cast<size_t>(w.alias - 1) * g_RcvIdBuf.stride;
            if (!RestampRcvRowSEH(dst, w.row.data(), g_RcvIdBuf.stride))
                continue;
            ++repaired;
            if (!firstLane)
                firstLane = w.alias;
        }

        static bool s_warned = false;
        if (repaired && !s_warned)
        {
            s_warned = true;
            Log("[EquipParam] WARNING: %d claimed receiver lane row(s) had been "
                "overwritten with vanilla data (first: lane %d) - an equip-table "
                "reload re-parses the receiver buffer IN PLACE, so the pointer "
                "never moves and the relocation guard cannot see it; every lane "
                "reclaimed from the vanilla 1..%d range loses its baseIdx/sysIdx "
                "and the weapon fires with the vanilla receiver's fire rate and "
                "fire mode. Rows re-asserted before this gunInfo build.\n",
                repaired, firstLane, kReceiverCapacity);
        }
    }

    static void EnsureShadowsRelocated()
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
        EnsureOnePoolRelocated(g_RcvIdBuf);
        ReassertWideReceiverLanes();
        EnsureOnePoolRelocated(g_Base.pb);
        EnsureOnePoolRelocated(g_Wob.pb);
        EnsureOnePoolRelocated(g_Sys.pb);
        EnsureOnePoolRelocated(g_Snd.pb);
        EnsureOnePoolRelocated(g_Magazine);
        EnsureOnePoolRelocated(g_Bullet);
        EnsureOnePoolRelocated(g_Muzzle);
        EnsureOnePoolRelocated(g_Stock);
        EnsureOnePoolRelocated(g_Sight);
        EnsureOnePoolRelocated(g_Barrel);
        EnsureOnePoolRelocated(g_UnderBarrel);
        EnsureOnePoolRelocated(g_Option);
        EnsureOnePoolRelocated(g_BarrelBase.pb);
        EnsureOnePoolRelocated(g_BulletBase.pb);
    }

    constexpr int kPoolDirectMax     = 253;
    constexpr int kPoolScratch1      = 254;
    constexpr int kPoolScratch0      = 255;
    constexpr int kPoolOverflowStart = 256;

    struct RcvOverflowRef { int idx[4] = { -1, -1, -1, -1 }; };
    static std::map<int, RcvOverflowRef> g_RcvOverflow;
    static std::map<int, int> g_BarrelOverflow;
    static std::map<int, int> g_BulletBaseOverflow;

    struct BarrelExtraMult { double rem[6] = { 1, 1, 1, 1, 1, 1 }; };
    static std::map<int, BarrelExtraMult> g_BarrelExtraMult;

    static std::map<int, int> g_ReceiverMotionDonor;

    static std::map<std::string, int> g_RcvNameToId;
    static std::set<int> g_RcvClaimed;
    static std::atomic<bool> g_LaneIsCustomRcv[256];
    static void LaneProbe_NoteReceiverRead(unsigned int receiverId);
    static void LaneWindow_BindForBuild(void* desc, unsigned int equipId);

    static std::uint8_t* ReceiverTypeTable()
    {
        return static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.MotionLoaderImpl_ReceiverTypeTable));
    }

    static const int kPartTypeExtCount = 65536;

    static std::uint8_t g_RcvTypeExt[kPartTypeExtCount];
    static bool g_RcvTypeExtReady = false;

    static int CopyRcvTypeExtSEH(const std::uint8_t* src)
    {
        __try
        {
            for (int i = 0; i < 240; ++i) g_RcvTypeExt[i] = src[i];
            for (int i = 240; i < kPartTypeExtCount; ++i) g_RcvTypeExt[i] = 0;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void EnsureRcvTypeExt()
    {
        if (g_RcvTypeExtReady)
            return;
        const std::uint8_t* src = ReceiverTypeTable();
        if (src && CopyRcvTypeExtSEH(src) == 1)
            g_RcvTypeExtReady = true;
    }

    using GetReceiverType_t = unsigned int(__fastcall*)(void* self, unsigned int receiverId);
    static GetReceiverType_t g_OrigGetReceiverType = nullptr;

    unsigned int PartCode_TapReceiverType(unsigned int receiverId, unsigned int result);

    static unsigned int __fastcall hkGetReceiverType(void* self, unsigned int receiverId)
    {
        if (!g_RcvTypeExtReady)
            EnsureRcvTypeExt();
        LaneProbe_NoteReceiverRead(receiverId);
        unsigned int r;
        if (g_RcvTypeExtReady && receiverId < kPartTypeExtCount)
            r = g_RcvTypeExt[receiverId];
        else
            r = g_OrigGetReceiverType ? g_OrigGetReceiverType(self, receiverId) : 0;
        return PartCode_TapReceiverType(receiverId, r);
    }

    static bool WriteReceiverType(int receiverId, int type)
    {
        if (receiverId < 0 || receiverId >= kPartTypeExtCount)
            return false;
        EnsureRcvTypeExt();
        if (!g_RcvTypeExtReady)
            return false;
        g_RcvTypeExt[receiverId] = static_cast<std::uint8_t>(type);
        return true;
    }

    static int PartRowByte(const PartBuffer& pb, int id, int byteOff);

    static std::uint8_t* UnderBarrelTypeTable()
    {
        return static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.MotionLoaderImpl_UnderBarrelTypeTable));
    }

    static std::uint8_t g_UbTypeExt[kPartTypeExtCount];
    static bool g_UbTypeExtReady = false;

    static int CopyUbTypeExtSEH(const std::uint8_t* src)
    {
        __try
        {
            for (int i = 0; i < 23; ++i) g_UbTypeExt[i] = src[i];
            for (int i = 23; i < kPartTypeExtCount; ++i) g_UbTypeExt[i] = 0;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void EnsureUbTypeExt()
    {
        if (g_UbTypeExtReady)
            return;
        const std::uint8_t* src = UnderBarrelTypeTable();
        if (src && CopyUbTypeExtSEH(src) == 1)
            g_UbTypeExtReady = true;
    }

    using GetUnderBarrelType_t = unsigned int(__fastcall*)(void* self, unsigned int underBarrelId);
    static GetUnderBarrelType_t g_OrigGetUnderBarrelType = nullptr;

    unsigned int PartCode_TapPartId(int slot, unsigned int partId);

    static unsigned int __fastcall hkGetUnderBarrelType(void* self, unsigned int underBarrelId)
    {
        underBarrelId = PartCode_TapPartId(4, underBarrelId);
        if (!g_UbTypeExtReady)
            EnsureUbTypeExt();
        if (g_UbTypeExtReady && underBarrelId < kPartTypeExtCount)
            return g_UbTypeExt[underBarrelId];
        return g_OrigGetUnderBarrelType ? g_OrigGetUnderBarrelType(self, underBarrelId) : 0;
    }

    static const int kBarrelTypeVanillaRows   = 115;
    static const int kMagazineTypeVanillaRows = 192;
    static const int kSightTypeVanillaRows    = 25;

    static std::uint8_t g_BarrelTypeExt[kPartTypeExtCount];
    static bool         g_BarrelTypeExtReady = false;
    static std::uint8_t g_MagazineTypeExt[kPartTypeExtCount];
    static bool         g_MagazineTypeExtReady = false;
    static std::uint8_t g_SightTypeExt[kPartTypeExtCount];
    static bool         g_SightTypeExtReady = false;

    static int CopyPartTypeExtSEH(std::uint8_t* dst, const std::uint8_t* src, int rows)
    {
        __try
        {
            std::memset(dst, 0, kPartTypeExtCount);
            for (int i = 0; i < rows; ++i)
                dst[i] = src[i];
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void EnsurePartTypeExt(std::uint8_t* dst, bool& ready,
                                  uintptr_t tableAddr, int rows)
    {
        if (ready)
            return;
        const auto* src = static_cast<const std::uint8_t*>(
            ResolveGameAddress(tableAddr));
        if (src && CopyPartTypeExtSEH(dst, src, rows) == 1)
            ready = true;
    }

    using GetPartType_t = unsigned int(__fastcall*)(void* self, unsigned int partId);

    static GetPartType_t g_OrigGetBarrelType   = nullptr;
    static GetPartType_t g_OrigGetMagazineType = nullptr;
    static GetPartType_t g_OrigGetSightType    = nullptr;

    static unsigned int __fastcall hkGetBarrelType(void* self, unsigned int barrelId)
    {
        barrelId = PartCode_TapPartId(1, barrelId);
        EnsurePartTypeExt(g_BarrelTypeExt, g_BarrelTypeExtReady,
                          gAddr.MotionLoaderImpl_BarrelTypeTable,
                          kBarrelTypeVanillaRows);
        if (g_BarrelTypeExtReady && barrelId < kPartTypeExtCount)
            return g_BarrelTypeExt[barrelId];
        return g_OrigGetBarrelType ? g_OrigGetBarrelType(self, barrelId) : 0;
    }

    static unsigned int __fastcall hkGetMagazineType(void* self, unsigned int magazineId)
    {
        magazineId = PartCode_TapPartId(2, magazineId);
        EnsurePartTypeExt(g_MagazineTypeExt, g_MagazineTypeExtReady,
                          gAddr.MotionLoaderImpl_MagazineTypeTable,
                          kMagazineTypeVanillaRows);
        if (g_MagazineTypeExtReady && magazineId < kPartTypeExtCount)
            return g_MagazineTypeExt[magazineId];
        return g_OrigGetMagazineType ? g_OrigGetMagazineType(self, magazineId) : 0;
    }

    static unsigned int __fastcall hkGetSightType(void* self, unsigned int sightId)
    {
        sightId = PartCode_TapPartId(3, sightId);
        EnsurePartTypeExt(g_SightTypeExt, g_SightTypeExtReady,
                          gAddr.MotionLoaderImpl_SightTypeTable,
                          kSightTypeVanillaRows);
        if (g_SightTypeExtReady && sightId < kPartTypeExtCount)
            return g_SightTypeExt[sightId];
        return g_OrigGetSightType ? g_OrigGetSightType(self, sightId) : 0;
    }

    static bool CopyPartTypeExt(std::uint8_t* ext, bool& ready,
                                uintptr_t tableAddr, int rows,
                                int dstId, int srcId)
    {
        EnsurePartTypeExt(ext, ready, tableAddr, rows);
        if (!ready)
            return false;
        if (dstId < rows || dstId >= kPartTypeExtCount)
            return false;
        if (srcId <= 0 || srcId >= kPartTypeExtCount)
            return false;
        const std::uint8_t t = ext[srcId];
        if (t == 0)
            return false;
        ext[dstId] = t;
        return true;
    }

    static bool WriteUnderBarrelType(int underBarrelId, int type)
    {
        if (underBarrelId < 0 || underBarrelId >= kPartTypeExtCount)
            return false;
        EnsureUbTypeExt();
        if (!g_UbTypeExtReady)
            return false;
        g_UbTypeExt[underBarrelId] = static_cast<std::uint8_t>(type);
        return true;
    }

    static std::uint8_t* ImplBufPtr(std::ptrdiff_t off)
    {
        auto* impl = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipParameterTablesImpl_Instance));
        if (!impl)
            return nullptr;
        return ReadPtrSEH(reinterpret_cast<void**>(impl + off));
    }

    static bool ReadSysTriadSEH(const std::uint8_t* sysPool, int idx,
                                int* eqp, int* reticle, int* trigger)
    {
        __try
        {
            const std::uint8_t* sp = sysPool + static_cast<size_t>(idx) * 3;
            *eqp     = sp[0] & 0x1f;
            *reticle = sp[1] & 0x1f;
            *trigger = sp[1] >> 5;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ReadReceiverSysIndexSEH(const std::uint8_t* rbuf, int recvId, int* sysIdx)
    {
        __try
        {
            *sysIdx = rbuf[static_cast<size_t>(recvId - 1) * kReceiverStride + 4];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static int RcvRowIsZeroSEH(const std::uint8_t* buf, int idx0)
    {
        __try
        {
            const std::uint8_t* p = buf + static_cast<size_t>(idx0) * kReceiverStride;
            for (int k = 0; k < kReceiverStride; ++k)
                if (p[k] != 0)
                    return 0;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static int ComputeRcvPoolStockCountSEH(const std::uint8_t* rbuf, int rowByteOffset)
    {
        __try
        {
            int mx = 0;
            for (int i = 0; i < kReceiverCapacity; ++i)
            {
                const std::uint8_t* p = rbuf + static_cast<size_t>(i) * kReceiverStride;
                bool nonzero = false;
                for (int k = 0; k < kReceiverStride; ++k)
                    if (p[k]) { nonzero = true; break; }
                if (!nonzero)
                    continue;
                if (p[rowByteOffset] > mx)
                    mx = p[rowByteOffset];
            }
            return mx;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void WriteBaseRowRawSEH(std::uint8_t* pool, int idx, const int* v, int n)
    {
        __try
        {
            std::uint8_t* p = pool + static_cast<size_t>(idx) * 12;
            for (int k = 0; k < 12; ++k) p[k] = 0;
            if (n > 0) *reinterpret_cast<std::uint16_t*>(p + 0) = static_cast<std::uint16_t>(v[0]);
            if (n > 1) p[2] = static_cast<std::uint8_t>(v[1]);
            if (n > 2) p[3] = static_cast<std::uint8_t>(v[2]);
            if (n > 3) p[4] = static_cast<std::uint8_t>(v[3]);
            if (n > 4) p[5] = static_cast<std::uint8_t>(v[4]);
            if (n > 5) *reinterpret_cast<std::uint16_t*>(p + 6) = static_cast<std::uint16_t>(v[5]);
            if (n > 6) *reinterpret_cast<std::uint16_t*>(p + 8) = static_cast<std::uint16_t>(v[6]);
            if (n > 7) p[0xa] = static_cast<std::uint8_t>(v[7]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteWobRowRawSEH(std::uint8_t* pool, int idx, const int* v, int n)
    {
        __try
        {
            std::uint8_t* p = pool + static_cast<size_t>(idx) * 14;
            for (int k = 0; k < 14; ++k) p[k] = 0;
            for (int k = 0; k < 7 && k < n; ++k)
                *reinterpret_cast<std::uint16_t*>(p + k * 2) = static_cast<std::uint16_t>(v[k]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteSysRowRawSEH(std::uint8_t* pool, int idx, int b0, int b1, int b2)
    {
        __try
        {
            std::uint8_t* p = pool + static_cast<size_t>(idx) * 3;
            p[0] = static_cast<std::uint8_t>(b0);
            p[1] = static_cast<std::uint8_t>(b1);
            p[2] = static_cast<std::uint8_t>(b2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteSndRowSEH(std::uint8_t* pool, int idx, const char* label)
    {
        __try
        {
            std::uint8_t* p = pool + static_cast<size_t>(idx) * 8;
            for (int k = 0; k < 8; ++k) p[k] = 0;
            for (int k = 0; k < 7 && label[k]; ++k)
                p[k] = static_cast<std::uint8_t>(label[k]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void WriteReceiverRowSEH(std::uint8_t* rbuf, int receiverId,
                                    int attackId, int baseIdx1, int wob, int sys, int snd)
    {
        __try
        {
            std::uint8_t* p = rbuf + static_cast<size_t>(receiverId - 1) * kReceiverStride;
            *reinterpret_cast<std::uint16_t*>(p + 0) =
                static_cast<std::uint16_t>(attackId & 0xFFFF);
            p[2] = static_cast<std::uint8_t>(baseIdx1 & 0xFF);
            p[3] = static_cast<std::uint8_t>(wob & 0xFF);
            p[4] = static_cast<std::uint8_t>(sys & 0xFF);
            p[5] = static_cast<std::uint8_t>(snd & 0xFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static bool EnsureRcvPoolShadow(RcvPool& rp)
    {
        if (rp.pb.active)
            return true;
        if (!rp.counted)
        {
            const std::uint8_t* rbuf = ImplBufPtr(kReceiverImplOffset);
            if (!rbuf)
                return false;
            const int sc = ComputeRcvPoolStockCountSEH(rbuf, rp.rowByteOffset);
            if (sc <= 0)
            {
                LogDebug("[EquipParam] %s: could not determine vanilla row count\n", rp.pb.name);
                return false;
            }
            rp.pb.stockCount = sc + 1;
            rp.pb.nextId = sc + 1;
            rp.counted = true;
        }
        return EnsurePartShadow(rp.pb);
    }

    static int AllocateRcvPoolRow(RcvPool& rp)
    {
        if (!EnsureRcvPoolShadow(rp))
            return -1;
        if (rp.pb.nextId == kPoolScratch1)
            rp.pb.nextId = kPoolOverflowStart;
        if (rp.pb.nextId >= rp.pb.maxId)
        {
            LogDebug("[EquipParam] %s pool exhausted (max %d rows)\n", rp.pb.name, rp.pb.maxId);
            return -1;
        }
        return rp.pb.nextId++;
    }

    static int ComputeRefPoolStockCountSEH(const std::uint8_t* refBuf, int refStride,
                                           int refCapacity, int refByteOffset)
    {
        __try
        {
            int mx = 0;
            for (int i = 0; i < refCapacity; ++i)
            {
                const std::uint8_t* p = refBuf + static_cast<size_t>(i) * refStride;
                bool nonzero = false;
                for (int k = 0; k < refStride; ++k)
                    if (p[k]) { nonzero = true; break; }
                if (!nonzero)
                    continue;
                if (p[refByteOffset] > mx)
                    mx = p[refByteOffset];
            }
            return mx;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static bool EnsureRefPoolShadow(RefPool& rp)
    {
        if (rp.pb.active)
            return true;
        if (!rp.counted)
        {
            const std::uint8_t* refBuf = ImplBufPtr(rp.refImplOffset);
            if (!refBuf)
                return false;
            const int sc = ComputeRefPoolStockCountSEH(
                refBuf, rp.refStride, rp.refCapacity, rp.refByteOffset);
            if (sc <= 0)
            {
                LogDebug("[EquipParam] %s: could not determine vanilla row count\n", rp.pb.name);
                return false;
            }
            rp.pb.stockCount = sc + 1;
            rp.pb.nextId = sc + 1;
            rp.counted = true;
        }
        return EnsurePartShadow(rp.pb);
    }

    static int AllocateRefPoolRow(RefPool& rp)
    {
        if (!EnsureRefPoolShadow(rp))
            return -1;
        if (rp.pb.nextId == kPoolScratch1)
            rp.pb.nextId = kPoolOverflowStart;
        if (rp.pb.nextId >= rp.pb.maxId)
        {
            LogDebug("[EquipParam] %s pool exhausted (max %d rows)\n", rp.pb.name, rp.pb.maxId);
            return -1;
        }
        return rp.pb.nextId++;
    }

    static int ResolvePoolField(lua_State* L, const char* field, RcvPoolKind kind)
    {
        g_lua_getfield(L, 1, const_cast<char*>(field));
        const int t = g_lua_type(L, -1);
        if (t == LUA_TNIL)
        {
            g_lua_settop(L, -2);
            return -1;
        }
        if (t == LUA_TNUMBER)
        {
            const int v = static_cast<int>(g_lua_tointeger(L, -1));
            g_lua_settop(L, -2);
            return v;
        }
        if (t == LUA_TSTRING && kind == POOL_SND)
        {
            char label[8] = { 0 };
            size_t len = 0;
            const char* s = g_lua_tolstring(L, -1, &len);
            std::string fullRoot;
            if (s)
            {
                fullRoot.assign(s, len);
                for (int c = 0; c < 7 && s[c]; ++c) label[c] = s[c];
            }
            g_lua_settop(L, -2);

            RcvPool& sp = PoolFor(kind);
            const int idx = AllocateRcvPoolRow(sp);
            if (idx < 0) return -2;
            std::uint8_t* pool = PartCurrentBuf(sp.pb);
            if (!pool) return -2;
            WriteSndRowSEH(pool, idx, label);
            RegisterFireSoundOverride(idx, fullRoot, -1, false, field);
            return idx;
        }

        if (t != LUA_TTABLE)
        {
            g_lua_settop(L, -2);
            LogDebug("[EquipParam] SetReceiver: '%s' must be a number (game index) or a table (custom values)\n", field);
            return -2;
        }

        RcvPool& rp = PoolFor(kind);

        if (kind == POOL_SND)
        {
            char label[8] = { 0 };
            std::string text;
            bool isEvent = false;
            int mSeg = -1;

            g_lua_getfield(L, -1, const_cast<char*>("event"));
            if (g_lua_type(L, -1) == LUA_TSTRING)
            {
                size_t len = 0;
                const char* s = g_lua_tolstring(L, -1, &len);
                if (s) { text.assign(s, len); isEvent = true; }
            }
            g_lua_settop(L, -2);

            if (!isEvent)
            {
                g_lua_getfield(L, -1, const_cast<char*>("name"));
                if (g_lua_type(L, -1) != LUA_TSTRING)
                {
                    g_lua_settop(L, -2);
                    g_lua_rawgeti(L, -1, 1);
                }
                if (g_lua_type(L, -1) != LUA_TSTRING)
                {
                    g_lua_settop(L, -2);
                    g_lua_getfield(L, -1, const_cast<char*>("label"));
                }
                if (g_lua_type(L, -1) == LUA_TSTRING)
                {
                    size_t len = 0;
                    const char* s = g_lua_tolstring(L, -1, &len);
                    if (s) { text.assign(s, len); for (int c = 0; c < 7 && s[c]; ++c) label[c] = s[c]; }
                }
                g_lua_settop(L, -2);

                g_lua_getfield(L, -1, const_cast<char*>("middle"));
                if (g_lua_type(L, -1) == LUA_TBOOLEAN)
                    mSeg = g_lua_toboolean(L, -1) != 0 ? 1 : 2;
                g_lua_settop(L, -2);
            }
            else
            {
                g_lua_getfield(L, -1, const_cast<char*>("name"));
                if (g_lua_type(L, -1) == LUA_TSTRING)
                {
                    size_t len = 0;
                    const char* s = g_lua_tolstring(L, -1, &len);
                    if (s)
                        for (int c = 0; c < 7 && s[c]; ++c)
                            label[c] = s[c];
                }
                g_lua_settop(L, -2);
            }

            std::string supText;
            bool supIsEvent = false;
            g_lua_getfield(L, -1, const_cast<char*>("supEvent"));
            if (g_lua_type(L, -1) == LUA_TSTRING)
            {
                size_t len = 0;
                const char* s = g_lua_tolstring(L, -1, &len);
                if (s) { supText.assign(s, len); supIsEvent = true; }
            }
            g_lua_settop(L, -2);
            if (supText.empty())
            {
                g_lua_getfield(L, -1, const_cast<char*>("sup"));
                if (g_lua_type(L, -1) == LUA_TSTRING)
                {
                    size_t len = 0;
                    const char* s = g_lua_tolstring(L, -1, &len);
                    if (s) supText.assign(s, len);
                }
                g_lua_settop(L, -2);
            }

            g_lua_settop(L, -2);

            const int idx = AllocateRcvPoolRow(rp);
            if (idx < 0) return -2;
            std::uint8_t* pool = PartCurrentBuf(rp.pb);
            if (!pool) return -2;
            WriteSndRowSEH(pool, idx, label);
            RegisterFireSoundOverride(idx, text, mSeg, isEvent, field);
            RegisterFireSoundSupOverride(idx, supText, supIsEvent, field);
            return idx;
        }

        double vals[12] = { 0 };
        int n = static_cast<int>(g_lua_objlen(L, -1));
        if (n > 12) n = 12;
        for (int i = 1; i <= n; ++i)
        {
            g_lua_rawgeti(L, -1, i);
            vals[i - 1] = static_cast<double>(g_lua_tonumber(L, -1));
            g_lua_settop(L, -2);
        }
        const char* const* names =
            kind == POOL_BASE ? kRcvBaseNames
            : kind == POOL_WOB ? kRcvWobNames : kRcvSysNames;
        for (int i = 0; i < 12 && names[i]; ++i)
        {
            g_lua_getfield(L, -1, const_cast<char*>(names[i]));
            if (g_lua_isnumber(L, -1))
            {
                vals[i] = static_cast<double>(g_lua_tonumber(L, -1));
                if (n < i + 1) n = i + 1;
            }
            g_lua_settop(L, -2);
        }
        g_lua_settop(L, -2);

        const int idx = AllocateRcvPoolRow(rp);
        if (idx < 0) return -2;
        std::uint8_t* pool = PartCurrentBuf(rp.pb);
        if (!pool) return -2;

        if (kind == POOL_BASE)
        {
            int v[8];
            v[0] = Clamp(static_cast<int>(vals[0] * 10.0 + 0.5), 0, 65535);
            v[1] = Clamp(static_cast<int>(vals[1] + 0.5), 0, 255);
            v[2] = Clamp(static_cast<int>(vals[2] * 200.0 + 0.5), 0, 255);
            v[3] = Clamp(static_cast<int>(vals[3] + 0.5), 0, 255);
            v[4] = Clamp(static_cast<int>(vals[4] + 0.5), 0, 255);
            v[5] = Clamp(static_cast<int>(vals[5] * 1000.0 + 0.5), 0, 65535);
            v[6] = Clamp(static_cast<int>(vals[6] * 100.0 + 0.5), 0, 65535);
            v[7] = Clamp(static_cast<int>(vals[7] * 100.0 + 0.5), 0, 255);
            WriteBaseRowRawSEH(pool, idx, v, n);
        }
        else if (kind == POOL_WOB)
        {
            int v[7];
            for (int k = 0; k < 7; ++k)
                v[k] = Clamp(static_cast<int>(vals[k] * 1000.0 + 0.5), 0, 65535);
            WriteWobRowRawSEH(pool, idx, v, n);
        }
        else // POOL_SYS
        {
            int iv[12];
            for (int k = 0; k < 12; ++k)
                iv[k] = static_cast<int>(vals[k] + 0.5);
            const int b0 = (iv[0] & 0x1f) | (iv[3] ? 0x20 : 0) | (iv[4] ? 0x40 : 0) | (iv[5] ? 0x80 : 0);
            const int b1 = (iv[1] & 0x1f) | ((iv[2] & 0x7) << 5);
            const int b2 = (iv[6] ? 1 : 0) | (iv[7] ? 2 : 0) | (iv[8] ? 4 : 0)
                         | (iv[9] ? 8 : 0) | (iv[10] ? 0x10 : 0) | (iv[11] ? 0x20 : 0);
            WriteSysRowRawSEH(pool, idx, b0, b1, b2);
        }
        return idx;
    }
}

int EquipParam_AllocateMagazineSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_Magazine, name);
}

int EquipParam_AllocateStockSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_Stock, name);
}

int EquipParam_AllocateMuzzleSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_Muzzle, name);
}

int EquipParam_AllocateSightSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_Sight, name);
}

int EquipParam_AllocateBarrelSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_Barrel, name);
}

int EquipParam_AllocateUnderBarrelSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_UnderBarrel, name);
}

int EquipParam_AllocateOptionSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_Option, name);
}

int EquipParam_AllocateBulletSlotForName(const char* name)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return AllocatePartSlot(g_Bullet, name);
}

int __cdecl l_SetOption(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetOption: argument #1 must be a table\n");
        return 0;
    }

    int optionId = 0;
    if (!ReadNamedInt(L, 1, "optionId", optionId) || optionId <= 0)
    {
        LogDebug("[EquipParam] SetOption: missing/invalid optionId (declare one via V_TppEquip.DeclareLTLS)\n");
        return 0;
    }

    int isLight = 0, isLaser = 0;
    ReadNamedInt(L, 1, "isLight", isLight);
    ReadNamedInt(L, 1, "isLaser", isLaser);

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_Option))
    {
        LogDebug("[EquipParam] SetOption: buffer not available for this build - skipped\n");
        return 0;
    }
    int writeId = optionId;
    bool wideRow = false;
    std::uint8_t* buf = nullptr;
    if (optionId >= kWideIdBase && WideStateFor(kVanillaSpace_Option, optionId))
    {
        buf = WideRowFor(g_Option, optionId, writeId);
        wideRow = true;
    }
    else if (optionId > g_Option.maxId)
    {
        LogDebug("[EquipParam] SetOption: optionId=%d out of range [1,%d] - not written\n",
            optionId, g_Option.maxId);
        return 0;
    }
    else
        buf = PartCurrentBuf(g_Option);
    if (!buf)
        return 0;

    const bool vanillaRow = !wideRow && optionId <= g_Option.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(writeId - 1);
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Option, optionId, rowPtr, 1);

    WriteOptionSEH(buf, writeId, isLight, isLaser);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Option, optionId, rowPtr, 1);
    if (wideRow)
        SyncWideRowToAlias(g_Option, optionId);

#ifdef _DEBUG
    LogDebug("[EquipParam] SetOption optionId=%d isLight=%d isLaser=%d -> native slot\n",
        optionId, isLight, isLaser);
#endif
    return 0;
}

int __cdecl l_SetUnderBarrel(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetUnderBarrel: argument #1 must be a table\n");
        return 0;
    }

    int underBarrelId = 0;
    if (!ReadNamedInt(L, 1, "underBarrelId", underBarrelId) || underBarrelId <= 0)
    {
        LogDebug("[EquipParam] SetUnderBarrel: missing/invalid underBarrelId (declare one via V_TppEquip.DeclareUBs)\n");
        return 0;
    }

    int receiverId = 0, magazineId = 0, underBarrelGrade = 0, motionFrom = 0;
    const bool hasReceiver = ReadNamedInt(L, 1, "receiverId", receiverId);
    ReadNamedInt(L, 1, "magazineId", magazineId);
    ReadNamedInt(L, 1, "underBarrelGrade", underBarrelGrade);
    const bool hasMotionFrom = ReadNamedInt(L, 1, "motionFrom", motionFrom);

    if (!hasReceiver || receiverId <= 0)
    {
        LogDebug("[EquipParam] SetUnderBarrel underBarrelId=%d: requires a receiverId (the underbarrel is a "
            "sub-weapon - its receiverId picks its firing behavior from the receiver buffer, magazineId "
            "its ammo). Row rejected.\n", underBarrelId);
        return 0;
    }

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_UnderBarrel))
    {
        LogDebug("[EquipParam] SetUnderBarrel: buffer not available for this build - skipped\n");
        return 0;
    }
    receiverId = ResolvePartByteLocked(kVanillaSpace_Receiver, receiverId);
    magazineId = ResolvePartByteLocked(kVanillaSpace_Magazine, magazineId);
    if (receiverId <= 0)
    {
        LogDebug("[EquipParam] SetUnderBarrel underBarrelId=%d: its receiver could not "
            "bind an engine lane byte - row rejected\n", underBarrelId);
        return 0;
    }

    int writeId = underBarrelId;
    bool wideRow = false;
    std::uint8_t* buf = nullptr;
    if (underBarrelId >= kWideIdBase && WideStateFor(kVanillaSpace_UnderBarrel, underBarrelId))
    {
        buf = WideRowFor(g_UnderBarrel, underBarrelId, writeId);
        wideRow = true;
    }
    else if (underBarrelId > g_UnderBarrel.maxId)
    {
        LogDebug("[EquipParam] SetUnderBarrel: underBarrelId=%d out of range [1,%d] - not written\n",
            underBarrelId, g_UnderBarrel.maxId);
        return 0;
    }
    else
        buf = PartCurrentBuf(g_UnderBarrel);
    if (!buf)
        return 0;

    const bool vanillaRow = !wideRow && underBarrelId <= g_UnderBarrel.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(writeId - 1) * 3;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_UnderBarrel, underBarrelId, rowPtr, 3);

    WriteUnderBarrelSEH(buf, writeId, receiverId, magazineId, underBarrelGrade);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_UnderBarrel, underBarrelId, rowPtr, 3);
    if (wideRow)
        SyncWideRowToAlias(g_UnderBarrel, underBarrelId);

    EnsureUbTypeExt();
    if (g_UbTypeExtReady && underBarrelId > g_UnderBarrel.stockCount)
    {
        int ubType = 0;
        const char* how = nullptr;
        int donor = 0;

        if (hasMotionFrom && motionFrom > 0 && motionFrom < kPartTypeExtCount)
        {
            ubType = g_UbTypeExt[motionFrom];
            donor = motionFrom;
            how = "motionFrom";
        }
        else
        {
            EnsureRcvTypeExt();
            const int wantRcType = (g_RcvTypeExtReady && receiverId > 0
                                    && receiverId < kPartTypeExtCount)
                ? g_RcvTypeExt[receiverId] : 0;
            if (wantRcType != 0)
            {
                for (int id = 1; id <= g_UnderBarrel.stockCount; ++id)
                {
                    const int rc = PartRowByte(g_UnderBarrel, id, 0);
                    if (rc <= 0 || rc >= 256)
                        continue;
                    if (g_RcvTypeExt[rc] != wantRcType)
                        continue;
                    if (g_UbTypeExt[id] == 0)
                        continue;
                    ubType = g_UbTypeExt[id];
                    donor = id;
                    how = "receiver motion match";
                    break;
                }
            }
        }

        bool typeApplied = false;
        if (ubType != 0)
        {
            if (wideRow)
            {
                WidePartState* w = WideStateFor(kVanillaSpace_UnderBarrel, underBarrelId);
                if (w)
                {
                    w->pendingUbType = ubType;
                    typeApplied = w->alias ? WriteUnderBarrelType(w->alias, ubType)
                                           : true;
                }
            }
            else
                typeApplied = WriteUnderBarrelType(underBarrelId, ubType);
        }
        if (ubType != 0 && typeApplied)
        {
            LogDebug("[ChimeraMotion] underBarrelId=%d animation type %d inherited from vanilla "
                "underBarrel %d (%s) - the grenade-launcher motion set now loads for weapons "
                "carrying this under-barrel.\n",
                underBarrelId, ubType, donor, how);
        }
        else
        {
            LogDebug("[ChimeraMotion] underBarrelId=%d has NO animation type - the engine's type table "
                "only covers vanilla ids 1..%d, so a custom id contributes no motion and the parent "
                "weapon's own animations play instead. Set motionFrom=<vanilla UB id> on this "
                "under-barrel, or give its receiver a motionFrom that matches one.\n",
                underBarrelId, g_UnderBarrel.stockCount);
        }
    }

#ifdef _DEBUG
    LogDebug("[EquipParam] SetUnderBarrel underBarrelId=%d receiver=%d magazine=%d grade=%d -> native slot\n",
        underBarrelId, receiverId, magazineId, underBarrelGrade);
#endif
    return 0;
}

int __cdecl l_SetBarrel(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetBarrel: argument #1 must be a table\n");
        return 0;
    }

    int barrelId = 0;
    if (!ReadNamedInt(L, 1, "barrelId", barrelId) || barrelId <= 0)
    {
        LogDebug("[EquipParam] SetBarrel: missing/invalid barrelId (declare one via V_TppEquip.DeclareBAs)\n");
        return 0;
    }

    int base = 0, barrelLength = 0, scopeMount = 0, unkFlag = 0, sideMount = 0, underMount = 0;
    double baseCurve[7] = { 0 };
    int baseCurveN = 0, baseNum = 0;
    int baseKind = ReadNumberOrFloatTable(L, 1, "barrelParamSetsBase", baseNum,
                                          baseCurve, 7, baseCurveN,
                                          kBarrelBaseNames);
    if (baseKind == 1)
        base = baseNum;
    ReadNamedInt(L, 1, "barrelLength", barrelLength);
    ReadNamedInt(L, 1, "hasScopeMount", scopeMount);
    ReadNamedInt(L, 1, "unk2", unkFlag);
    ReadNamedInt(L, 1, "hasSideMount", sideMount);
    ReadNamedInt(L, 1, "hasUnderMount", underMount);

    if (baseKind == 1 && (base < 0 || base > 255))
    {
        LogDebug("[EquipParam] SetBarrel barrelId=%d: base=%d out of range [0,255] - the barrel "
            "ballistics/range index (barrelParamSetsBase curve) is a full byte. Masked.\n",
            barrelId, base);
    }
    if (barrelLength < 0 || barrelLength > 15)
    {
        LogDebug("[EquipParam] SetBarrel barrelId=%d: barrelLength=%d out of range [0,15] - "
            "BarrelLengthType is a 4-bit field. Masked.\n", barrelId, barrelLength);
    }

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_Barrel))
    {
        LogDebug("[EquipParam] SetBarrel: buffer not available for this build - skipped\n");
        return 0;
    }
    int writeId = barrelId;
    bool wideRow = false;
    std::uint8_t* buf = nullptr;
    if (barrelId >= kWideIdBase && WideStateFor(kVanillaSpace_Barrel, barrelId))
    {
        buf = WideRowFor(g_Barrel, barrelId, writeId);
        wideRow = true;
    }
    else if (barrelId > g_Barrel.maxId)
    {
        LogDebug("[EquipParam] SetBarrel: barrelId=%d out of range [1,%d] - not written\n",
            barrelId, g_Barrel.maxId);
        return 0;
    }
    else
        buf = PartCurrentBuf(g_Barrel);
    if (!buf)
        return 0;

    if (baseKind == 2)
    {
        BarrelExtraMult ex;
        bool wantExtra = false;
        static const int kEngineMaxFields[3] = { 0, 4, 5 };
        static const int kIdentityFields[2]  = { 2, 3 };
        for (int f = 0; f < 3; ++f)
        {
            const int k = kEngineMaxFields[f];
            const double v = baseCurve[k];
            if (!(v >= 0.0) || v > 1.0e6)
            {
                baseCurve[k] = 0;
                LogDebug("[EquipParam] SetBarrel barrelId=%d: barrelParamSetsBase[%d] "
                    "invalid value - zeroed\n", barrelId, k + 1);
                continue;
            }
            if (v > 2.555)
            {
                ex.rem[k] = v / 2.55;
                baseCurve[k] = 2.55;
                wantExtra = true;
            }
        }
        for (int f = 0; f < 2; ++f)
        {
            const int k = kIdentityFields[f];
            const double v = baseCurve[k];
            if (!(v >= 0.0) || v > 1.0e6)
            {
                baseCurve[k] = 0;
                LogDebug("[EquipParam] SetBarrel barrelId=%d: barrelParamSetsBase[%d] "
                    "invalid value - zeroed\n", barrelId, k + 1);
                continue;
            }
            if (v > 2.555)
            {
                ex.rem[k] = v;
                baseCurve[k] = 1.0;
                wantExtra = true;
            }
        }
        if (baseCurve[1] > 2.555)
            LogDebug("[EquipParam] SetBarrel barrelId=%d: unk2 clamped to 2.55 - no "
                "known engine consumer to post-scale\n", barrelId);
        if (baseCurve[6] > 2.555)
            LogDebug("[EquipParam] SetBarrel barrelId=%d: percentOverride clamped to "
                "2.55 - GunInfo+0x83 is a byte\n", barrelId);

        const int idx = AllocateRefPoolRow(g_BarrelBase);
        if (idx >= 0)
        {
            std::uint8_t* pool = PartCurrentBuf(g_BarrelBase.pb);
            if (pool)
            {
                WriteBarrelBaseRowSEH(pool, idx, baseCurve, baseCurveN);
                if (idx > kPoolDirectMax)
                {
                    g_BarrelOverflow[barrelId] = idx;
                    base = 0;
                    LogDebug("[EquipParam] SetBarrel barrelId=%d: curve row %d is past "
                        "the 254-slot direct window - served through the overflow "
                        "swap at gun setup\n", barrelId, idx);
                    if (barrelId <= g_Barrel.stockCount)
                        EquipParam_VanillaForceTaint(kVanillaSpace_Barrel, barrelId,
                            "overflow curve rows the pre-image diff cannot see");
                }
                else
                {
                    g_BarrelOverflow.erase(barrelId);
                    base = idx;
                }
                if (wantExtra)
                {
                    g_BarrelExtraMult[barrelId] = ex;
                    LogDebug("[EquipParam] SetBarrel barrelId=%d: multiplier(s) above "
                        "the 2.55x byte ceiling - remainder applied at gun setup "
                        "(fireRate x%.2f aim x%.2f range x%.2f rangeUI x%.2f "
                        "spreadMax x%.2f)\n", barrelId,
                        ex.rem[0], ex.rem[2], ex.rem[3], ex.rem[4], ex.rem[5]);
                    if (barrelId <= g_Barrel.stockCount)
                        EquipParam_VanillaForceTaint(kVanillaSpace_Barrel, barrelId,
                            "above-ceiling multipliers the pre-image diff cannot see");
                }
                else
                {
                    g_BarrelExtraMult.erase(barrelId);
                }
            }
        }
        else
        {
            g_BarrelExtraMult.erase(barrelId);
            LogDebug("[EquipParam] SetBarrel barrelId=%d: could not append custom "
                "barrelParamSetsBase curve - any >2.55x request dropped\n",
                barrelId);
        }
    }
    else
    {
        g_BarrelOverflow.erase(barrelId);
        g_BarrelExtraMult.erase(barrelId);
    }

    const bool vanillaRow = !wideRow && barrelId <= g_Barrel.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(writeId - 1) * 2;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Barrel, barrelId, rowPtr, 2);

    WriteBarrelSEH(buf, writeId, base, barrelLength, scopeMount, unkFlag, sideMount, underMount);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Barrel, barrelId, rowPtr, 2);
    if (wideRow)
        SyncWideRowToAlias(g_Barrel, barrelId);

#ifdef _DEBUG
    LogDebug("[EquipParam] SetBarrel barrelId=%d base=%d scope=%d side=%d under=%d len=%d -> native slot\n",
        barrelId, base, scopeMount, sideMount, underMount, barrelLength);
#endif
    return 0;
}

int __cdecl l_SetMagazine(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetMagazine: argument #1 must be a table\n");
        return 0;
    }

    int ammoId = 0;
    if (!ReadNamedInt(L, 1, "ammoId", ammoId) || ammoId <= 0)
    {
        LogDebug("[EquipParam] SetMagazine: missing/invalid ammoId\n");
        return 0;
    }

    int eqpAmmoId = 0;
    int capacity = 0;
    int totalCarry = 0;
    int bulletId = 0;
    ReadNamedInt(L, 1, "equipAmmoId", eqpAmmoId);
    ReadNamedInt(L, 1, "capacity", capacity);
    ReadNamedInt(L, 1, "totalCarry", totalCarry);
    ReadNamedInt(L, 1, "bulletId", bulletId);

    if (capacity < 0 || capacity > 0x3FF)
        LogDebug("[EquipParam] SetMagazine ammoId=%d: capacity=%d exceeds the "
            "native 10-bit field (max 1023) - clamped to the field range\n",
            ammoId, capacity);
    if (totalCarry > 0x3FFF)
        LogDebug("[EquipParam] SetMagazine ammoId=%d: totalCarry=%d exceeds the "
            "native 14-bit field (max 16383) - clamped; use 0 for the "
            "unlimited-carry sentinel\n",
            ammoId, totalCarry);
    if (eqpAmmoId < 0 || eqpAmmoId > 0xFFFF)
        LogDebug("[EquipParam] SetMagazine ammoId=%d: equipAmmoId=%d exceeds the "
            "native field (0..65535) and wraps to %d\n",
            ammoId, eqpAmmoId, eqpAmmoId & 0xFFFF);
    if (bulletId < 0 || bulletId > 0xFF)
        LogDebug("[EquipParam] SetMagazine ammoId=%d: bulletId=%d exceeds the "
            "native byte field (0..255) and wraps to %d\n",
            ammoId, bulletId, bulletId & 0xFF);

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_Magazine))
    {
        LogDebug("[EquipParam] SetMagazine: buffer not available for this build - skipped\n");
        return 0;
    }
    int writeId = ammoId;
    bool wideRow = false;
    std::uint8_t* buf = nullptr;
    if (ammoId >= kWideIdBase && WideStateFor(kVanillaSpace_Magazine, ammoId))
    {
        buf = WideRowFor(g_Magazine, ammoId, writeId);
        wideRow = true;
    }
    else if (ammoId > g_Magazine.maxId)
    {
        LogDebug("[EquipParam] SetMagazine: ammoId=%d out of range [1,%d] - not written\n",
            ammoId, g_Magazine.maxId);
        return 0;
    }
    else
        buf = PartCurrentBuf(g_Magazine);
    if (!buf)
        return 0;

    const bool vanillaRow = !wideRow && ammoId <= g_Magazine.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(writeId - 1) * 8;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Magazine, ammoId, rowPtr, 8);

    WriteMagazineSEH(buf, writeId, eqpAmmoId, capacity, totalCarry, bulletId);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Magazine, ammoId, rowPtr, 8);
    if (wideRow)
        SyncWideRowToAlias(g_Magazine, ammoId);

    int rootAmmoId = wideRow ? 0 : writeId;
    if (wideRow)
    {
        WidePartState* w = WideStateFor(kVanillaSpace_Magazine, ammoId);
        if (w && w->alias)
            rootAmmoId = w->alias;
    }
    if (eqpAmmoId > 0 && rootAmmoId > 0)
        TppEquip_NoteAmmoRootParam(eqpAmmoId, rootAmmoId);
    HealSupplyAmmoCountTable();

#ifdef _DEBUG
    LogDebug("[EquipParam] SetMagazine ammoId=%d eqpAmmo=%d cap=%d total=%d bullet=%d -> native slot\n",
        ammoId, eqpAmmoId, capacity, totalCarry, bulletId);
#endif
    return 0;
}

int __cdecl l_SetBullet(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetBullet: argument #1 must be a table\n");
        return 0;
    }

    int bulletId = 0;
    if (!ReadNamedInt(L, 1, "bulletId", bulletId) || bulletId <= 0)
    {
        LogDebug("[EquipParam] SetBullet: missing/invalid bulletId (declare one via V_TppEquip.DeclareBLs)\n");
        return 0;
    }

    int u16v[3] = { 0, 0, 0 };
    int u8v[7]  = { 0, 0, 0, 0, 0, 0, 0 };
    int unk11 = 0, eqpType = 0;
    ReadNamedInt(L, 1, "bulletSpeed", u16v[0]);
    ReadNamedIntAlias(L, 1, "npcBulletSpeed", "unk02", u16v[1]);
    ReadNamedInt(L, 1, "dropRate", u16v[2]);
    double bbCurve[12] = { 0 };
    int bbCurveN = 0, bbNum = 0;
    const int bbKind = ReadNumberOrFloatTable(L, 1, "bulletParamSetsBase", bbNum,
                                              bbCurve, 12, bbCurveN,
                                              kBulletBaseNames);
    if (bbKind == 1)
        u8v[0] = bbNum;
    double npcCurve[12] = { 0 };
    int npcCurveN = 0, npcNum = 0;
    int npcKind = ReadNumberOrFloatTable(L, 1, "npcBulletParamSetsBase", npcNum,
                                         npcCurve, 12, npcCurveN, kBulletBaseNames);
    if (npcKind == 0)
        npcKind = ReadNumberOrFloatTable(L, 1, "unk07", npcNum,
                                         npcCurve, 12, npcCurveN, kBulletBaseNames);
    if (npcKind == 1)
        u8v[1] = npcNum;
    if (!ReadNamedInt(L, 1, "bulletTrailEffect", u8v[2]))
    {
        const std::string trailPath = ReadNamedStringOrTable1(L, 1, "bulletTrailEffect");
        if (!trailPath.empty())
        {
            const int idx = ResolveTrailPathIndex(FoxHashes::PathCode64Ext(trailPath));
            if (idx >= 0 && idx <= 255)
                u8v[2] = idx;
            else
                LogDebug("[EquipParam] SetBullet: bulletTrailEffect path '%s' not registered "
                    "(trail list unavailable or index>255)\n", trailPath.c_str());
        }
    }
    ReadNamedInt(L, 1, "unk09", u8v[3]);
    ReadNamedInt(L, 1, "bulletType",   u8v[4]);
    ReadNamedInt(L, 1, "ricochetSize", u8v[5]);
    ReadNamedInt(L, 1, "blastId", u8v[6]);
    ReadNamedIntAlias(L, 1, "isLethal", "unk11", unk11);
    ReadNamedInt(L, 1, "eqpType", eqpType);

    auto warnWidth = [&](const char* name, int v, int maxV)
    {
        if (v < 0 || v > maxV)
            LogDebug("[EquipParam] SetBullet bulletId=%d: %s=%d exceeds the native "
                "field (0..%d) and wraps to %d in the engine row - clamp or "
                "rescale the value\n",
                bulletId, name, v, maxV, v & maxV);
    };
    warnWidth("bulletSpeed",    u16v[0], 0xFFFF);
    warnWidth("npcBulletSpeed", u16v[1], 0xFFFF);
    warnWidth("dropRate",       u16v[2], 0xFFFF);
    if (bbKind == 1)  warnWidth("bulletParamSetsBase",    u8v[0], 0xFF);
    if (npcKind == 1) warnWidth("npcBulletParamSetsBase", u8v[1], 0xFF);
    warnWidth("bulletTrailEffect", u8v[2], 0xFF);
    warnWidth("unk09",        u8v[3], 0xFF);
    warnWidth("bulletType",   u8v[4], 0xFF);
    warnWidth("ricochetSize", u8v[5], 0xFF);
    warnWidth("blastId",      u8v[6], 0xFF);

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_Bullet))
    {
        LogDebug("[EquipParam] SetBullet: buffer not available for this build - skipped\n");
        return 0;
    }
    if (bulletId > g_Bullet.maxId)
    {
        LogDebug("[EquipParam] SetBullet: bulletId=%d out of range [1,%d] - not written\n",
            bulletId, g_Bullet.maxId);
        return 0;
    }

    std::uint8_t* buf = PartCurrentBuf(g_Bullet);
    if (!buf)
        return 0;

    if (bbKind == 2)
    {
        const int idx = AllocateRefPoolRow(g_BulletBase);
        if (idx >= 0)
        {
            std::uint8_t* pool = PartCurrentBuf(g_BulletBase.pb);
            if (pool)
            {
                WriteBulletBaseRowSEH(pool, idx, bbCurve, bbCurveN);
                if (idx > kPoolDirectMax)
                {
                    g_BulletBaseOverflow[bulletId] = idx;
                    u8v[0] = 0;
                    LogDebug("[EquipParam] SetBullet bulletId=%d: falloff row %d is past "
                        "the 254-slot direct window - served through the overflow "
                        "swap at fire time\n", bulletId, idx);
                    if (bulletId <= g_Bullet.stockCount)
                        EquipParam_VanillaForceTaint(kVanillaSpace_Bullet, bulletId,
                            "overflow falloff rows the pre-image diff cannot see");
                }
                else
                {
                    g_BulletBaseOverflow.erase(bulletId);
                    u8v[0] = idx;
                }
            }
        }
        else
        {
            LogDebug("[EquipParam] SetBullet bulletId=%d: could not append custom bulletParamSetsBase curve\n",
                bulletId);
        }
    }
    else
    {
        g_BulletBaseOverflow.erase(bulletId);
    }

    if (npcKind == 2)
    {
        const int idx = AllocateRefPoolRow(g_BulletBase);
        if (idx >= 0)
        {
            std::uint8_t* pool = PartCurrentBuf(g_BulletBase.pb);
            if (pool)
            {
                WriteBulletBaseRowSEH(pool, idx, npcCurve, npcCurveN);
                if (idx > kPoolDirectMax)
                {
                    u8v[1] = 0;
                    LogDebug("[EquipParam] SetBullet bulletId=%d: NPC falloff row %d is "
                        "past the direct window and NPC-fired shots cannot use the "
                        "overflow swap - NPC shots fall back to curve 0\n",
                        bulletId, idx);
                }
                else
                {
                    u8v[1] = idx;
                }
            }
        }
        else
        {
            LogDebug("[EquipParam] SetBullet bulletId=%d: could not append custom npcBulletParamSetsBase curve\n",
                bulletId);
        }
    }

    const bool vanillaRow = bulletId <= g_Bullet.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(bulletId - 1) * 14;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Bullet, bulletId, rowPtr, 14);

    WriteBulletRowSEH(buf, bulletId, u16v, u8v, unk11, eqpType);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Bullet, bulletId, rowPtr, 14);

    bool lockOnGiven = false;
    int lockBulletType = -1;
    int lockAmmoPerShot = 0;
    g_lua_getfield(L, 1, const_cast<char*>("lockOn"));
    if (g_lua_type(L, -1) == LUA_TTABLE)
    {
        lockOnGiven = true;
        const int lockTbl = g_lua_gettop(L);
        int count = 1;
        double timeSec = 1.0, turnDeg = 90.0, lockedSpeed = 0.0;
        double minRange = 0.0, maxRange = 0.0, homingStart = 0.0;
        ReadNamedInt(L, lockTbl, "count", count);
        ReadNamedFloat(L, lockTbl, "time", timeSec);
        ReadNamedFloat(L, lockTbl, "turnRate", turnDeg);
        ReadNamedFloat(L, lockTbl, "minRange", minRange);
        ReadNamedFloat(L, lockTbl, "maxRange", maxRange);
        ReadNamedFloat(L, lockTbl, "bulletSpeed", lockedSpeed);
        ReadNamedFloat(L, lockTbl, "homingStartDistance", homingStart);
        ReadNamedInt(L, lockTbl, "bulletType", lockBulletType);
        ReadNamedInt(L, lockTbl, "ammoPerShot", lockAmmoPerShot);
        equip::LockOnCategories cats{};
        cats.soldiers = ReadNamedBoolTri(L, lockTbl, "canLockOnSoldier");
        cats.vehicles = ReadNamedBoolTri(L, lockTbl, "canLockOnVehicle");
        equip::LockOn_RegisterBullet(bulletId, count, timeSec, turnDeg,
                                     minRange, maxRange, lockedSpeed,
                                     static_cast<double>(u16v[0]),
                                     homingStart, &cats);
        if (lockBulletType >= 0)
            equip::LockOn_RegisterBulletType(bulletId, lockBulletType, u8v[4]);
    }
    g_lua_settop(L, -2);

    int ammoPerShot = 0;
    ReadNamedInt(L, 1, "ammoPerShot", ammoPerShot);
    if (ammoPerShot > 1 || lockAmmoPerShot > 1)
        equip::MultiShot_RegisterBullet(bulletId, ammoPerShot, lockAmmoPerShot);

    if (vanillaRow && lockOnGiven)
        EquipParam_VanillaForceTaint(kVanillaSpace_Bullet, bulletId, "lockOn homing");
    if (vanillaRow && (ammoPerShot > 1 || lockAmmoPerShot > 1))
        EquipParam_VanillaForceTaint(kVanillaSpace_Bullet, bulletId, "multi-shot ammoPerShot");

#ifdef _DEBUG
    LogDebug("[EquipParam] SetBullet bulletId=%d ricochet=%d type=%d eqpType=%d -> native slot\n",
        bulletId, u8v[5], u8v[4], eqpType);
#endif
    return 0;
}

int __cdecl l_SetStock(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetStock: argument #1 must be a table\n");
        return 0;
    }

    int stockId = 0;
    if (!ReadNamedInt(L, 1, "stockId", stockId) || stockId <= 0)
    {
        LogDebug("[EquipParam] SetStock: missing/invalid stockId\n");
        return 0;
    }

    double spreadRecovery = 1.0;
    double movementSway = 1.0;
    ReadNamedFloat(L, 1, "spreadRecovery", spreadRecovery);
    ReadNamedFloat(L, 1, "movementSway", movementSway);

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_Stock))
    {
        LogDebug("[EquipParam] SetStock: buffer not available for this build - skipped\n");
        return 0;
    }
    int writeId = stockId;
    bool wideRow = false;
    std::uint8_t* buf = nullptr;
    if (stockId >= kWideIdBase && WideStateFor(kVanillaSpace_Stock, stockId))
    {
        buf = WideRowFor(g_Stock, stockId, writeId);
        wideRow = true;
    }
    else if (stockId > g_Stock.maxId)
    {
        LogDebug("[EquipParam] SetStock: stockId=%d out of range [1,%d] - not written\n",
            stockId, g_Stock.maxId);
        return 0;
    }
    else
        buf = PartCurrentBuf(g_Stock);
    if (!buf)
        return 0;

    const bool vanillaRow = !wideRow && stockId <= g_Stock.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(writeId - 1) * 2;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Stock, stockId, rowPtr, 2);

    WriteStockSEH(buf, writeId, ScaleByte(spreadRecovery, 100.0), ScaleByte(movementSway, 100.0));

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Stock, stockId, rowPtr, 2);
    if (wideRow)
        SyncWideRowToAlias(g_Stock, stockId);

#ifdef _DEBUG
    LogDebug("[EquipParam] SetStock stockId=%d spreadRecovery=%.2f movementSway=%.2f -> native slot\n",
        stockId, spreadRecovery, movementSway);
#endif
    return 0;
}

int __cdecl l_SetMuzzle(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetMuzzle: argument #1 must be a table\n");
        return 0;
    }

    int muzzleOptionId = 0;
    if (!ReadNamedInt(L, 1, "muzzleOptionId", muzzleOptionId) || muzzleOptionId <= 0)
    {
        LogDebug("[EquipParam] SetMuzzle: missing/invalid muzzleOptionId\n");
        return 0;
    }

    double grouping = 1.0;
    int durability = -1;
    int suppressor = 0;
    ReadNamedFloat(L, 1, "grouping", grouping);
    ReadNamedInt(L, 1, "durability", durability);
    ReadNamedInt(L, 1, "suppressor", suppressor);

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_Muzzle))
    {
        LogDebug("[EquipParam] SetMuzzle: buffer not available for this build - skipped\n");
        return 0;
    }
    int writeId = muzzleOptionId;
    bool wideRow = false;
    std::uint8_t* buf = nullptr;
    if (muzzleOptionId >= kWideIdBase && WideStateFor(kVanillaSpace_MuzzleOption, muzzleOptionId))
    {
        buf = WideRowFor(g_Muzzle, muzzleOptionId, writeId);
        wideRow = true;
    }
    else if (muzzleOptionId > g_Muzzle.maxId)
    {
        LogDebug("[EquipParam] SetMuzzle: muzzleOptionId=%d out of range [1,%d] - not written\n",
            muzzleOptionId, g_Muzzle.maxId);
        return 0;
    }
    else
        buf = PartCurrentBuf(g_Muzzle);
    if (!buf)
        return 0;

    const bool vanillaRow = !wideRow && muzzleOptionId <= g_Muzzle.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(writeId - 1) * 3;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_MuzzleOption, muzzleOptionId, rowPtr, 3);

    WriteMuzzleSEH(buf, writeId, ScaleByte(grouping, 100.0), durability, suppressor);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_MuzzleOption, muzzleOptionId, rowPtr, 3);
    if (wideRow)
        SyncWideRowToAlias(g_Muzzle, muzzleOptionId);

#ifdef _DEBUG
    LogDebug("[EquipParam] SetMuzzle muzzleOptionId=%d grouping=%.2f durability=%d suppressor=%d -> native slot\n",
        muzzleOptionId, grouping, durability, suppressor);
#endif
    return 0;
}

int __cdecl l_SetSight(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetSight: argument #1 must be a table\n");
        return 0;
    }

    int scopeId = 0;
    if (!ReadNamedInt(L, 1, "scopeId", scopeId) || scopeId <= 0)
    {
        LogDebug("[EquipParam] SetSight: missing/invalid scopeId (declare one via V_TppEquip.DeclareSTs)\n");
        return 0;
    }

    int zoom1 = 0, zoom2 = 0, zoom3 = 0, scopeUiId = 0;
    int booster = 0, nvg = 0, builtIn = 0, rangeFinder = 0, rangeFinderBulletDrop = 0;
    ReadNamedInt(L, 1, "zoom1", zoom1);
    ReadNamedInt(L, 1, "zoom2", zoom2);
    ReadNamedInt(L, 1, "zoom3", zoom3);
    ReadNamedInt(L, 1, "scopeUiId", scopeUiId);
    ReadNamedInt(L, 1, "booster", booster);
    ReadNamedInt(L, 1, "nvg", nvg);
    ReadNamedInt(L, 1, "builtIn", builtIn);
    ReadNamedInt(L, 1, "rangeFinder", rangeFinder);
    ReadNamedInt(L, 1, "rangeFinderBulletDrop", rangeFinderBulletDrop);

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!EnsurePartShadow(g_Sight))
    {
        LogDebug("[EquipParam] SetSight: buffer not available for this build - skipped\n");
        return 0;
    }
    int writeId = scopeId;
    bool wideRow = false;
    std::uint8_t* buf = nullptr;
    if (scopeId >= kWideIdBase && WideStateFor(kVanillaSpace_Sight, scopeId))
    {
        buf = WideRowFor(g_Sight, scopeId, writeId);
        wideRow = true;
    }
    else if (scopeId > g_Sight.maxId)
    {
        LogDebug("[EquipParam] SetSight: scopeId=%d out of range [1,%d] - not written\n",
            scopeId, g_Sight.maxId);
        return 0;
    }
    else
        buf = PartCurrentBuf(g_Sight);
    if (!buf)
        return 0;

    const int flags = (booster ? 0x01 : 0) | (nvg ? 0x02 : 0) | (builtIn ? 0x04 : 0)
                    | (rangeFinder ? 0x08 : 0) | (rangeFinderBulletDrop ? 0x10 : 0);
    const bool vanillaRow = !wideRow && scopeId <= g_Sight.stockCount;
    const std::uint8_t* rowPtr = buf + static_cast<size_t>(writeId - 1) * 5;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Sight, scopeId, rowPtr, 5);

    WriteSightSEH(buf, writeId, Clamp(zoom1, 0, 255), Clamp(zoom2, 0, 255),
                  Clamp(zoom3, 0, 255), Clamp(scopeUiId, 0, 255), flags);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Sight, scopeId, rowPtr, 5);
    if (wideRow)
        SyncWideRowToAlias(g_Sight, scopeId);

#ifdef _DEBUG
    LogDebug("[EquipParam] SetSight scopeId=%d zoom=%d/%d/%d ui=%d flags=0x%02X -> native slot\n",
        scopeId, zoom1, zoom2, zoom3, scopeUiId, flags);
#endif
    return 0;
}

static const int kGunBasicScanMaxId  = 65535;
static const int kMinPlausibleRcvRefs = 32;

static bool BuildReferencedReceiverIds(std::set<int>& out)
{
    out.clear();
    for (int weaponId = 1; weaponId <= kGunBasicScanMaxId; ++weaponId)
    {
        unsigned char row[12] = {};
        if (!GunBasic_ReadRowBytes(weaponId, row))
            continue;
        if (row[0])
            out.insert(static_cast<int>(row[0]));
    }
    if (static_cast<int>(out.size()) < kMinPlausibleRcvRefs)
    {
        static std::atomic<int> s_thinLogged{ 0 };
        if (s_thinLogged.fetch_add(1) < 4)
            LogDebug("[EquipParam] receiver lane reclaim SKIPPED: the gunBasic table names only "
                "%d distinct receivers, far fewer than vanilla's - the table is not "
                "populated yet, so every unnamed row would look free and reclaiming one "
                "would steal a receiver a vanilla weapon still uses. Falling back to "
                "zero-row lanes only\n",
                static_cast<int>(out.size()));
        out.clear();
        return false;
    }
    return true;
}

static int CountFreeReceiverLaneSlots()
{
    const bool grown = EnsurePartShadow(g_RcvIdBuf);
    std::uint8_t* rbuf = grown ? PartCurrentBuf(g_RcvIdBuf) : ImplBufPtr(kReceiverImplOffset);
    if (!rbuf)
        return 0;
    const int cap = grown ? kReceiverMaxId : kReceiverCapacity;
    int freeCount = 0;
    for (int idx0 = cap - 1; idx0 >= 1; --idx0)
    {
        const int receiverId = idx0 + 1;
        if (receiverId == 0xD0 || receiverId == 0xE7)
            continue;
        if (g_RcvClaimed.count(receiverId))
            continue;
        if (RcvRowIsZeroSEH(rbuf, idx0) != 1)
            continue;
        ++freeCount;
    }
    return freeCount;
}

static int AllocateReceiverLaneSlot(const char* logLabel)
{
    const bool grown = EnsurePartShadow(g_RcvIdBuf);
    std::uint8_t* rbuf = grown ? PartCurrentBuf(g_RcvIdBuf) : ImplBufPtr(kReceiverImplOffset);
    if (!rbuf)
        return 0;

    const int cap = grown ? kReceiverMaxId : kReceiverCapacity;
    for (int idx0 = cap - 1; idx0 >= 1; --idx0)
    {
        const int receiverId = idx0 + 1;
        if (receiverId == 0xD0 || receiverId == 0xE7)
            continue;
        if (g_RcvClaimed.count(receiverId))
            continue;
        if (RcvRowIsZeroSEH(rbuf, idx0) != 1)
            continue;

        g_RcvClaimed.insert(receiverId);
        if (receiverId >= 0 && receiverId < 256)
            g_LaneIsCustomRcv[receiverId].store(true, std::memory_order_relaxed);
        LogDebug("[EquipParam] '%s' -> receiverId %d (%s slot)\n",
            logLabel, receiverId,
            (receiverId > kReceiverCapacity) ? "grown" : "free native");
        return receiverId;
    }

    std::set<int> referenced;
    if (!BuildReferencedReceiverIds(referenced))
        return 0;

    for (int idx0 = cap - 1; idx0 >= 1; --idx0)
    {
        const int receiverId = idx0 + 1;
        if (receiverId == 0xD0 || receiverId == 0xE7)
            continue;
        if (g_RcvClaimed.count(receiverId))
            continue;
        if (referenced.count(receiverId))
            continue;
        if (RcvRowIsZeroSEH(rbuf, idx0) < 0)
            continue;

        g_RcvClaimed.insert(receiverId);
        if (receiverId >= 0 && receiverId < 256)
            g_LaneIsCustomRcv[receiverId].store(true, std::memory_order_relaxed);
        LogDebug("[EquipParam] '%s' -> receiverId %d (RECLAIMED: every zero row was taken, so "
            "this non-zero vanilla receiver row was claimed instead - no weapon in the "
            "%d-row gunBasic table references it, so overwriting it changes no gun that "
            "can be equipped. If a vanilla weapon's recoil/fire-mode looks wrong after "
            "this, that receiver was reached by something outside gunBasic and this id "
            "must be excluded)\n",
            logLabel, receiverId, kGunBasicScanMaxId);
        return receiverId;
    }
    return 0;
}

int EquipParam_AllocateReceiverSlotForName(const char* name)
{
    if (!name || !name[0])
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    auto it = g_RcvNameToId.find(name);
    if (it != g_RcvNameToId.end())
        return it->second;

    if (PartIdWiden_IsActive())
    {
        const int shadowId = AllocatePartSlot(g_RcvIdBuf, name);
        if (shadowId)
        {
            g_RcvNameToId[name] = shadowId;
            return shadowId;
        }
    }
    else
    {
        const int wideId = AllocateWidePartId(g_RcvIdBuf, name);
        if (wideId)
        {
            g_RcvNameToId[name] = wideId;
            return wideId;
        }
    }

    const int receiverId = AllocateReceiverLaneSlot(name);
    if (receiverId)
    {
        g_RcvNameToId[name] = receiverId;
        return receiverId;
    }

    LogDebug("[EquipParam] no free receiver slot for '%s' (all %d used) - custom receiver "
        "unavailable\n", name, kReceiverMaxId);
    return 0;
}

int __cdecl l_SetReceiver(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    ChimeraMotion_EnsureWrapInstalled(L);

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetReceiver: argument #1 must be a table\n");
        return 0;
    }

    int receiverId = 0;
    if (!ReadNamedInt(L, 1, "receiverId", receiverId) || receiverId <= 0)
    {
        LogDebug("[EquipParam] SetReceiver: missing/invalid receiverId (declare one via V_TppEquip.DeclareRCs)\n");
        return 0;
    }

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    std::uint8_t* rbuf = ImplBufPtr(kReceiverImplOffset);
    if (!rbuf)
    {
        LogDebug("[EquipParam] SetReceiver: receiver buffer not available for this build - skipped\n");
        return 0;
    }
    bool wideRow = false;
    int writeId = receiverId;
    std::uint8_t* rowBuf = rbuf;
    if (receiverId >= kWideIdBase && WideStateFor(kVanillaSpace_Receiver, receiverId))
    {
        EnsurePartShadow(g_RcvIdBuf);
        rowBuf = WideRowFor(g_RcvIdBuf, receiverId, writeId);
        if (!rowBuf)
            return 0;
        wideRow = true;
    }
    else if (receiverId > kReceiverMaxId)
    {
        if (!PartIdWiden_IsActive() || !EnsurePartShadow(g_RcvIdBuf)
            || receiverId > g_RcvIdBuf.maxId)
        {
            LogDebug("[EquipParam] SetReceiver: receiverId=%d out of range [1,%d] - not written\n",
                receiverId, PartIdWiden_IsActive() ? g_RcvIdBuf.maxId : kReceiverMaxId);
            return 0;
        }
        rowBuf = PartCurrentBuf(g_RcvIdBuf);
        if (!rowBuf)
        {
            LogDebug("[EquipParam] SetReceiver: receiverId=%d grown receiver buffer "
                "unavailable - not written\n", receiverId);
            return 0;
        }
        rbuf = rowBuf;
    }

    int attackId = 0, motionFrom = 0;
    const bool hasAttack     = ReadNamedInt(L, 1, "attackId", attackId);
    const bool hasMotionFrom = ReadNamedInt(L, 1, "motionFrom", motionFrom);

    int base = ResolvePoolField(L, "receiverParamSetsBase", POOL_BASE);
    int wob = ResolvePoolField(L, "receiverParamSetsWobbling", POOL_WOB);
    int sys = ResolvePoolField(L, "receiverParamSetsSystem", POOL_SYS);
    int snd = ResolvePoolField(L, "receiverParamSetsSound", POOL_SND);

    if (sys > 0)
    {
        const std::uint8_t* sysPool = ImplBufPtr(0x68);
        if (sysPool)
        {
            int e = 0, r = 0, t = 0;
            if (ReadSysTriadSEH(sysPool, sys, &e, &r, &t))
                LogDebug("[EquipParam] SetReceiver receiverId=%d: resolved "
                    "receiverParamSetsSystem eqpType=%d reticleUiId=%d triggerId=%d "
                    "(triggerId 0 = single-shot; symbolic TppEquip.TRIGGER_*/RETICLE_UI_* "
                    "are undeclared -> nil -> 0. Use literal numbers.)\n",
                    receiverId, e, r, t);
            int di = 0, de = 0, dr = 0, dt = 0;
            if (hasMotionFrom && motionFrom > 0 && motionFrom <= kReceiverMaxId
                && ReadReceiverSysIndexSEH(rbuf, motionFrom, &di)
                && ReadSysTriadSEH(sysPool, di, &de, &dr, &dt))
                LogDebug("[EquipParam] SetReceiver receiverId=%d: motionFrom donor RC=%d "
                    "uses eqpType=%d reticleUiId=%d triggerId=%d - copy these literal "
                    "numbers into receiverParamSetsSystem for matching fire-mode/reticle\n",
                    receiverId, motionFrom, de, dr, dt);
        }
    }

    if (base == -2 || wob == -2 || sys == -2 || snd == -2)
        return 0;
    if (base < 0 || wob < 0 || sys < 0 || snd < 0)
    {
        LogDebug("[EquipParam] SetReceiver receiverId=%d: requires receiverParamSetsBase + "
            "receiverParamSetsWobbling + receiverParamSetsSystem + receiverParamSetsSound; "
            "each is a game index (number) or a {custom values} table\n", receiverId);
        return 0;
    }

    {
        std::uint8_t* sndPool = PartCurrentBuf(g_Snd.pb);
        char existing[9] = {};
        if (sndPool && ReadSndRowSEH(sndPool, snd, existing) && existing[0] == '\0')
        {
            std::string donorRoot;
            if (hasMotionFrom && ResolveDonorSoundRoot(motionFrom, donorRoot))
            {
                char label[8] = {};
                for (int c = 0; c < 7 && donorRoot[c]; ++c)
                    label[c] = donorRoot[c];
                WriteSndRowSEH(sndPool, snd, label);
                LogDebug("[EquipParam] SetReceiver receiverId=%d: sound root inherited from "
                    "motionFrom=%d -> '%s'; whichever of event/supEvent you omit plays "
                    "that donor's vanilla sound\n",
                    receiverId, motionFrom, label);
            }
            else
            {
                LogDebug("[EquipParam] SetReceiver receiverId=%d: receiverParamSetsSound gave "
                    "an explicit event but no root to fall back on - set motionFrom=<vanilla "
                    "RC> to inherit one, or add name=\"<root>\", or give both event and "
                    "supEvent. The side you omitted has no sound.\n", receiverId);
            }
        }
    }
    if (!hasAttack)
    {
        attackId = 0;
        LogDebug("[EquipParam] SetReceiver receiverId=%d: no valid attackId (nil?) - writing receiver "
            "with attackId=0 so the weapon still aims/fires (but deals ATK_Push/0 damage). Set "
            "attackId to a vanilla TppDamage.ATK_ value for real damage.\n", receiverId);
    }

    {
        RcvOverflowRef of;
        of.idx[POOL_BASE] = base > kPoolDirectMax ? base : -1;
        of.idx[POOL_WOB]  = wob  > kPoolDirectMax ? wob  : -1;
        of.idx[POOL_SYS]  = sys  > kPoolDirectMax ? sys  : -1;
        of.idx[POOL_SND]  = snd  > kPoolDirectMax ? snd  : -1;
        if (of.idx[0] >= 0 || of.idx[1] >= 0 || of.idx[2] >= 0 || of.idx[3] >= 0)
        {
            g_RcvOverflow[receiverId] = of;
            LogDebug("[EquipParam] SetReceiver receiverId=%d: pool row(s) past the "
                "254-slot direct window (base=%d wob=%d sys=%d snd=%d) - served "
                "through the overflow swap at gun setup\n",
                receiverId, base, wob, sys, snd);
            if (receiverId <= kReceiverCapacity && !g_RcvClaimed.count(receiverId))
                EquipParam_VanillaForceTaint(kVanillaSpace_Receiver, receiverId,
                    "overflow pool rows the pre-image diff cannot see");
        }
        else
        {
            g_RcvOverflow.erase(receiverId);
        }
        if (base > kPoolDirectMax) base = 0;
        if (wob  > kPoolDirectMax) wob  = 0;
        if (sys  > kPoolDirectMax) sys  = 0;
        if (snd  > kPoolDirectMax) snd  = 0;
    }

    const bool vanillaRow = !wideRow && receiverId <= kReceiverCapacity
        && !g_RcvClaimed.count(receiverId);
    const std::uint8_t* rowPtr =
        rowBuf + static_cast<size_t>(writeId - 1) * kReceiverStride;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Receiver, receiverId, rowPtr,
                                   kReceiverStride);

    WriteReceiverRowSEH(rowBuf, writeId, attackId, base, wob, sys, snd);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Receiver, receiverId, rowPtr,
                                    kReceiverStride);
    if (wideRow)
        SyncWideRowToAlias(g_RcvIdBuf, receiverId);

    int motionType = -1;
    if (hasMotionFrom && motionFrom > 0 && motionFrom < kPartTypeExtCount)
    {
        EnsureRcvTypeExt();
        if (g_RcvTypeExtReady)
        {
            motionType = g_RcvTypeExt[motionFrom];
            if (motionType == 0)
                Log("[EquipParam] SetReceiver receiverId=%d: WARNING motionFrom=%d has "
                    "motion type 0 (not a holdable gun's receiver) - without a valid type "
                    "the engine invalidates the weapon on draw; pick the receiver of a "
                    "real vanilla gun\n", receiverId, motionFrom);
            else if (wideRow)
            {
                WidePartState* w = WideStateFor(kVanillaSpace_Receiver, receiverId);
                if (w)
                {
                    w->motionFrom = motionFrom;
                    w->motionType = motionType;
                    if (w->alias)
                    {
                        ChimeraMotion_InheritFromMotionFrom(w->alias, motionFrom);
                        g_ReceiverMotionDonor[w->alias] = motionFrom;
                    }
                }
            }
            else
            {
                ChimeraMotion_InheritFromMotionFrom(receiverId, motionFrom);
                if (receiverId > 0)
                    g_ReceiverMotionDonor[receiverId] = motionFrom;
            }
        }
    }

    if (motionType < 0)
    {
        motionType = 0;
        LogDebug("[EquipParam] SetReceiver receiverId=%d: no motionFrom - animation defaulted to "
            "type 0; set motionFrom=<vanilla RC> for proper animations\n", receiverId);
    }
    if (wideRow)
    {
        WidePartState* w = WideStateFor(kVanillaSpace_Receiver, receiverId);
        if (w)
        {
            w->motionType = motionType;
            if (w->alias && !WriteReceiverType(w->alias, motionType))
                Log("[EquipParam] SetReceiver receiverId=%d: motion type write failed "
                    "(table unavailable)\n", receiverId);
        }
    }
    else
    {
        EnsureRcvTypeExt();
        const int prevType = (g_RcvTypeExtReady && receiverId > 0
                              && receiverId < kPartTypeExtCount)
            ? g_RcvTypeExt[receiverId] : -1;
        if (!WriteReceiverType(receiverId, motionType))
        {
            Log("[EquipParam] SetReceiver receiverId=%d: motion type write failed (table unavailable)\n",
                receiverId);
        }
        else if (vanillaRow && prevType >= 0 && motionType != prevType)
        {
            EquipParam_VanillaForceTaint(kVanillaSpace_Receiver, receiverId,
                                         "a different motion/animation type");
        }
    }

#ifdef _DEBUG
    LogDebug("[EquipParam] SetReceiver receiverId=%d attackId=%d base=%d wob=%d sys=%d snd=%d "
        "motionType=%d -> native\n",
        receiverId, attackId, base, wob, sys, snd, motionType);
#endif
    return 0;
}

namespace
{
    static PartBuffer* SpaceBuffer(int space)
    {
        switch (space)
        {
        case kVanillaSpace_Receiver:     return &g_RcvIdBuf;
        case kVanillaSpace_Barrel:       return &g_Barrel;
        case kVanillaSpace_Magazine:     return &g_Magazine;
        case kVanillaSpace_Stock:        return &g_Stock;
        case kVanillaSpace_MuzzleOption: return &g_Muzzle;
        case kVanillaSpace_Sight:        return &g_Sight;
        case kVanillaSpace_UnderBarrel:  return &g_UnderBarrel;
        case kVanillaSpace_Option:       return &g_Option;
        default:                         return nullptr;
        }
    }

    static int ResolvePartByteLocked(int space, int id)
    {
        if (id < kWideIdBase)
            return id;
        WidePartState* w = WideStateFor(space, id);
        if (!w)
        {
            if (space == kVanillaSpace_Receiver)
            {
                const auto itd = g_ReceiverMotionDonor.find(id);
                if (itd != g_ReceiverMotionDonor.end()
                    && itd->second > 0 && itd->second < 234)
                    return itd->second;
            }
            static std::set<long long> logged;
            if (logged.size() < 32 &&
                logged.insert((static_cast<long long>(space) << 32) | id).second)
                LogDebug("[EquipParam] part reference %d (space %d) is not a declared "
                    "wide id - treated as no part\n", id, space);
            return 0;
        }
        if (w->alias)
            return w->alias;

        PartBuffer* pb = SpaceBuffer(space);
        int alias = 0;
        if (space == kVanillaSpace_Receiver)
            alias = AllocateReceiverLaneSlot("(wide receiver)");
        else if (pb && EnsurePartShadow(*pb) && pb->nextId <= pb->maxId)
            alias = pb->nextId++;
        if (!alias)
        {
            LogDebug("[EquipParam] WIDE %s id %d cannot bind: every engine lane byte for "
                "this part type is already active this session - the referencing "
                "weapon gets part 0\n", pb ? pb->name : "part", id);
            return 0;
        }
        w->alias = alias;

        if (pb && !w->row.empty())
        {
            EnsurePartShadow(*pb);
            std::uint8_t* buf = PartCurrentBuf(*pb);
            if (buf)
                std::memcpy(buf + static_cast<size_t>(alias - 1) * pb->stride,
                            w->row.data(), static_cast<size_t>(pb->stride));
        }

        switch (space)
        {
        case kVanillaSpace_Receiver:
            if (w->motionFrom > 0 && w->motionType > 0)
            {
                ChimeraMotion_InheritFromMotionFrom(alias, w->motionFrom);
                g_ReceiverMotionDonor[alias] = w->motionFrom;
            }
            WriteReceiverType(alias, w->motionType >= 0 ? w->motionType : 0);
            {
                auto ito = g_RcvOverflow.find(id);
                if (ito != g_RcvOverflow.end())
                    g_RcvOverflow[alias] = ito->second;
            }
            break;
        case kVanillaSpace_UnderBarrel:
            if (w->pendingUbType > 0)
                WriteUnderBarrelType(alias, w->pendingUbType);
            break;
        case kVanillaSpace_Barrel:
            {
                auto ito = g_BarrelOverflow.find(id);
                if (ito != g_BarrelOverflow.end())
                    g_BarrelOverflow[alias] = ito->second;
                auto itx = g_BarrelExtraMult.find(id);
                if (itx != g_BarrelExtraMult.end())
                    g_BarrelExtraMult[alias] = itx->second;
            }
            break;
        default:
            break;
        }

        LogDebug("[EquipParam] WIDE %s id %d -> engine lane byte %d (bound on first "
            "weapon reference; row content and motion wiring applied at the lane "
            "slot)\n",
            pb ? pb->name : "part", id, alias);
        return alias;
    }
}

int EquipParam_ResolvePartByte(int space, int id)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return ResolvePartByteLocked(space, id);
}

bool Install_MotionLoader_ReceiverTypeHook()
{
    EnsureRcvTypeExt();

    void* target = ResolveGameAddress(gAddr.MotionLoaderImpl_GetReceiverType);
    if (!target)
    {
        LogDebug("[EquipParam] GetReceiverType address not set for this build - extended receiver-type table skipped\n");
        return true;
    }

    const bool ok = CreateAndEnableHook(
        target, &hkGetReceiverType,
        reinterpret_cast<void**>(&g_OrigGetReceiverType));
    if (!ok)
        Log("[EquipParam] GetReceiverType hook Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] GetReceiverType hook Install -> OK (target=%p, extended type table ready=%d)\n",
            target, g_RcvTypeExtReady ? 1 : 0);
#endif
    return ok;
}

void Uninstall_MotionLoader_ReceiverTypeHook()
{
    if (gAddr.MotionLoaderImpl_GetReceiverType)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.MotionLoaderImpl_GetReceiverType));
    g_OrigGetReceiverType = nullptr;
    g_RcvTypeExtReady = false;
}

namespace
{
    struct PartTypeHookSpec
    {
        const char*   label;
        uintptr_t     fnAddr;
        uintptr_t     tableAddr;
        std::uint8_t* ext;
        bool*         ready;
        int           rows;
        void*         detour;
        void**        orig;
        int           stock;
    };

    static bool InstallPartTypeHook(const PartTypeHookSpec& s)
    {
        EnsurePartTypeExt(s.ext, *s.ready, s.tableAddr, s.rows);

        void* target = ResolveGameAddress(s.fnAddr);
        if (!target)
        {
            LogDebug("[EquipParam] Get%sType address not set for this build - custom %s ids "
                "past %d keep reading whatever byte follows the engine's %d-entry table, "
                "so they report a garbage animation type and their motion layer binds to "
                "nothing\n", s.label, s.label, s.stock, s.rows);
            return true;
        }

        const bool ok = CreateAndEnableHook(target, s.detour, s.orig);
        if (!ok)
            Log("[EquipParam] Get%sType hook Install -> FAIL (target=%p)\n",
                s.label, target);
#ifdef _DEBUG
        else
            LogDebug("[EquipParam] Get%sType hook Install -> OK (target=%p, extended type table "
                "ready=%d; the engine's own table holds %d rows for stock ids 0..%d and has "
                "no bounds check, so a custom %s read past it as a raw byte)\n",
                s.label, target, *s.ready ? 1 : 0, s.rows, s.stock, s.label);
#endif
        return ok;
    }
}

int EquipParam_InheritPartMotionTypes(int barrelDst, int barrelSrc,
                                     int magazineDst, int magazineSrc,
                                     int sightDst, int sightSrc,
                                     int* outEligible)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    if (outEligible)
        *outEligible =
            (barrelDst   >= kBarrelTypeVanillaRows   ? 1 : 0) +
            (magazineDst >= kMagazineTypeVanillaRows ? 1 : 0) +
            (sightDst    >= kSightTypeVanillaRows    ? 1 : 0);
    int applied = 0;
    if (CopyPartTypeExt(g_BarrelTypeExt, g_BarrelTypeExtReady,
                        gAddr.MotionLoaderImpl_BarrelTypeTable,
                        kBarrelTypeVanillaRows, barrelDst, barrelSrc))
        ++applied;
    if (CopyPartTypeExt(g_MagazineTypeExt, g_MagazineTypeExtReady,
                        gAddr.MotionLoaderImpl_MagazineTypeTable,
                        kMagazineTypeVanillaRows, magazineDst, magazineSrc))
        ++applied;
    if (CopyPartTypeExt(g_SightTypeExt, g_SightTypeExtReady,
                        gAddr.MotionLoaderImpl_SightTypeTable,
                        kSightTypeVanillaRows, sightDst, sightSrc))
        ++applied;
    return applied;
}

bool Install_MotionLoader_BarrelTypeHook()
{
    const PartTypeHookSpec s{
        "Barrel", gAddr.MotionLoaderImpl_GetBarrelType,
        gAddr.MotionLoaderImpl_BarrelTypeTable,
        g_BarrelTypeExt, &g_BarrelTypeExtReady, kBarrelTypeVanillaRows,
        &hkGetBarrelType, reinterpret_cast<void**>(&g_OrigGetBarrelType), 114 };
    return InstallPartTypeHook(s);
}

void Uninstall_MotionLoader_BarrelTypeHook()
{
    if (gAddr.MotionLoaderImpl_GetBarrelType)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.MotionLoaderImpl_GetBarrelType));
    g_OrigGetBarrelType = nullptr;
    g_BarrelTypeExtReady = false;
}

bool Install_MotionLoader_MagazineTypeHook()
{
    const PartTypeHookSpec s{
        "Magazine", gAddr.MotionLoaderImpl_GetMagazineType,
        gAddr.MotionLoaderImpl_MagazineTypeTable,
        g_MagazineTypeExt, &g_MagazineTypeExtReady, kMagazineTypeVanillaRows,
        &hkGetMagazineType, reinterpret_cast<void**>(&g_OrigGetMagazineType), 191 };
    return InstallPartTypeHook(s);
}

void Uninstall_MotionLoader_MagazineTypeHook()
{
    if (gAddr.MotionLoaderImpl_GetMagazineType)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.MotionLoaderImpl_GetMagazineType));
    g_OrigGetMagazineType = nullptr;
    g_MagazineTypeExtReady = false;
}

bool Install_MotionLoader_SightTypeHook()
{
    const PartTypeHookSpec s{
        "Sight", gAddr.MotionLoaderImpl_GetSightType,
        gAddr.MotionLoaderImpl_SightTypeTable,
        g_SightTypeExt, &g_SightTypeExtReady, kSightTypeVanillaRows,
        &hkGetSightType, reinterpret_cast<void**>(&g_OrigGetSightType), 24 };
    return InstallPartTypeHook(s);
}

void Uninstall_MotionLoader_SightTypeHook()
{
    if (gAddr.MotionLoaderImpl_GetSightType)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.MotionLoaderImpl_GetSightType));
    g_OrigGetSightType = nullptr;
    g_SightTypeExtReady = false;
}

bool Install_MotionLoader_UnderBarrelTypeHook()
{
    EnsureUbTypeExt();

    void* target = ResolveGameAddress(gAddr.MotionLoaderImpl_GetUnderBarrelType);
    if (!target)
    {
        LogDebug("[EquipParam] GetUnderBarrelType address not set for this build - custom under-barrels "
            "will contribute no animation set\n");
        return true;
    }

    const bool ok = CreateAndEnableHook(
        target, &hkGetUnderBarrelType,
        reinterpret_cast<void**>(&g_OrigGetUnderBarrelType));
    if (!ok)
        Log("[EquipParam] GetUnderBarrelType hook Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] GetUnderBarrelType hook Install -> OK (target=%p, extended type table ready=%d; "
            "the engine's own table stops at vanilla id %d, so custom under-barrels read past it "
            "and load no motion without this)\n",
            target, g_UbTypeExtReady ? 1 : 0, g_UnderBarrel.stockCount);
#endif
    return ok;
}

void Uninstall_MotionLoader_UnderBarrelTypeHook()
{
    if (gAddr.MotionLoaderImpl_GetUnderBarrelType)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.MotionLoaderImpl_GetUnderBarrelType));
    g_OrigGetUnderBarrelType = nullptr;
    g_UbTypeExtReady = false;
}

namespace
{

    using GetAttackId_t = unsigned short(__fastcall*)(void* self, int equipId);
    static GetAttackId_t g_OrigGetAttackId = nullptr;

    static int SehAvOnly(unsigned long code)
    {
        return (code == EXCEPTION_ACCESS_VIOLATION)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    static unsigned short __fastcall hkGetAttackIdByEquipId(void* self, int equipId)
    {
        const int declared = EquipParam_GetDeclaredWeaponAttackId(equipId);
        if (declared > 0)
            return static_cast<unsigned short>(declared & 0xFFFF);

        __try
        {
            return g_OrigGetAttackId(self, equipId);
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            static unsigned long long lastMs = 0;
            const unsigned long long now = GetTickCount64();
            if (now - lastMs > 2000)
            {
                lastMs = now;
                LogDebug("[EquipParam] GetAttackIdByEquipId guarded a crash: equipId=%d "
                    "resolves to an empty gunBasic row (module missing SetGunBasic "
                    "for that weapon?) - returning attackId 0\n", equipId);
            }
            return 0;
        }
    }
}

namespace
{

    using SetUpGunInfo_t = void(__fastcall*)(void*, void*, unsigned int, void*,
                                             void*, void*, void*, void*, void*);
    static SetUpGunInfo_t g_OrigSetUpGunInfo = nullptr;

    static int ReadByteAtSEH(const void* base, size_t off)
    {
        __try
        {
            return static_cast<const std::uint8_t*>(base)[off];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static int WriteByteAtSEH(void* base, size_t off, std::uint8_t val)
    {
        __try
        {
            static_cast<std::uint8_t*>(base)[off] = val;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int SwapByteSEH(std::uint8_t* loc, std::uint8_t val, std::uint8_t* savedOut)
    {
        __try
        {
            *savedOut = *loc;
            *loc = val;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int RestoreByteSEH(std::uint8_t* loc, std::uint8_t val)
    {
        __try
        {
            *loc = val;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int CopyPoolRowSEH(std::uint8_t* pool, int dstIdx, int srcIdx, int stride)
    {
        __try
        {
            std::memcpy(pool + static_cast<size_t>(dstIdx) * stride,
                        pool + static_cast<size_t>(srcIdx) * stride,
                        static_cast<size_t>(stride));
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    struct PoolSwapState
    {
        std::uint8_t* loc[12] = {};
        std::uint8_t  saved[12] = {};
        int           count = 0;
    };

    static void SwapReceiverPools(int rcId, int scratchIdx, int sndSlot,
                                  PoolSwapState& st)
    {
        (void)sndSlot;
        const auto it = g_RcvOverflow.find(rcId);
        if (it == g_RcvOverflow.end())
            return;
        std::uint8_t* rbuf = ImplBufPtr(kReceiverImplOffset);
        if (!rbuf)
            return;
        for (int k = 0; k < 4; ++k)
        {
            const int idx = it->second.idx[k];
            if (idx < 0)
                continue;
            RcvPool& rp = PoolFor(static_cast<RcvPoolKind>(k));
            std::uint8_t* pool = PartCurrentBuf(rp.pb);
            if (!pool)
                continue;
            if (CopyPoolRowSEH(pool, scratchIdx, idx, rp.pb.stride) != 1)
                continue;
            std::uint8_t* loc = rbuf
                + static_cast<size_t>(rcId - 1) * kReceiverStride
                + rp.rowByteOffset;
            if (k == POOL_SND)
            {
                std::uint8_t prev = 0;
                if (SwapByteSEH(loc, static_cast<std::uint8_t>(scratchIdx),
                                &prev) == 1)
                {
                    std::lock_guard<std::mutex> lock(g_FireSoundMutex);
                    const auto fit = g_FireSoundByRow.find(idx);
                    if (fit != g_FireSoundByRow.end())
                        g_FireSoundByRow[scratchIdx] = fit->second;
                    else
                        g_FireSoundByRow.erase(scratchIdx);
                }
                continue;
            }
            if (st.count < 12
                && SwapByteSEH(loc, static_cast<std::uint8_t>(scratchIdx),
                               &st.saved[st.count]) == 1)
            {
                st.loc[st.count] = loc;
                ++st.count;
            }
        }
    }

    static void SwapBarrelPool(int barrelId, PoolSwapState& st)
    {
        const auto it = g_BarrelOverflow.find(barrelId);
        if (it == g_BarrelOverflow.end())
            return;
        std::uint8_t* pool = PartCurrentBuf(g_BarrelBase.pb);
        std::uint8_t* bbuf = PartCurrentBuf(g_Barrel);
        if (!pool || !bbuf)
            return;
        if (CopyPoolRowSEH(pool, kPoolScratch0, it->second, g_BarrelBase.pb.stride) != 1)
            return;
        std::uint8_t* loc = bbuf + static_cast<size_t>(barrelId - 1) * 2 + 1;
        if (st.count < 12
            && SwapByteSEH(loc, static_cast<std::uint8_t>(kPoolScratch0),
                           &st.saved[st.count]) == 1)
        {
            st.loc[st.count] = loc;
            ++st.count;
        }
    }

    static void PrepareGunInfoSwap(void* desc, PoolSwapState& st)
    {
        if (!desc)
            return;
        if (g_RcvOverflow.empty() && g_BarrelOverflow.empty())
            return;
        const int rc = ReadByteAtSEH(desc, 0);
        const int ba = ReadByteAtSEH(desc, 1);
        const int ub = ReadByteAtSEH(desc, 10);
        if (rc > 0)
            SwapReceiverPools(rc, kPoolScratch0, 0, st);
        int ubRc = 0;
        if (ub > 0)
        {
            std::uint8_t* ubuf = ImplBufPtr(0x48);
            if (ubuf)
                ubRc = ReadByteAtSEH(ubuf, static_cast<size_t>(ub - 1) * 3);
        }
        if (ubRc > 0 && ubRc != rc)
            SwapReceiverPools(ubRc, kPoolScratch1, 1, st);
        if (ba > 0)
            SwapBarrelPool(ba, st);
    }

    static void RestoreGunInfoSwap(PoolSwapState& st)
    {
        for (int i = st.count - 1; i >= 0; --i)
            RestoreByteSEH(st.loc[i], st.saved[i]);
    }

    static int CallOrigGunInfoSEH(void* self, void* desc, unsigned int equipId,
                                  void* gunInfo, void* a5, void* a6, void* a7,
                                  void* a8, void* a9)
    {
        __try
        {
            g_OrigSetUpGunInfo(self, desc, equipId, gunInfo, a5, a6, a7, a8, a9);
            return 1;
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            static unsigned long long lastMs = 0;
            const unsigned long long now = GetTickCount64();
            if (now - lastMs > 2000)
            {
                lastMs = now;
                LogDebug("[EquipParam] SetUpGunInfoFromGunPartsDesc guarded a crash: "
                    "equipId=%u desc(rc=%d ba=%d ub=%d) - the original faulted "
                    "mid-setup (likely an out-of-range base/wob/sys pool sub-index "
                    "for this receiver); GunInfo left partial and its fire-sound root "
                    "NULL, which the DefineWeaponFireSound guard now neutralizes\n",
                    equipId, ReadByteAtSEH(desc, 0), ReadByteAtSEH(desc, 1),
                    ReadByteAtSEH(desc, 10));
            }
            return 0;
        }
    }

    static int ApplyBarrelExtraSEH(void* gunInfo, const BarrelExtraMult& m)
    {
        __try
        {
            std::uint8_t* g = static_cast<std::uint8_t*>(gunInfo);
            if (m.rem[0] != 1.0)
            {
                const double d = *reinterpret_cast<std::uint16_t*>(g + 0x68) * m.rem[0] + 0.5;
                *reinterpret_cast<std::uint16_t*>(g + 0x68) =
                    d > 65535.0 ? 65535 : static_cast<std::uint16_t>(d);
            }
            if (m.rem[2] != 1.0)
            {
                const double d = g[0x55] * m.rem[2] + 0.5;
                g[0x55] = d > 255.0 ? 255 : static_cast<std::uint8_t>(d);
            }
            if (m.rem[3] != 1.0)
            {
                double d = g[0x54] * m.rem[3] + 0.5;
                g[0x54] = d > 255.0 ? 255 : static_cast<std::uint8_t>(d);
                d = *reinterpret_cast<std::uint16_t*>(g + 0x6c) * m.rem[3] + 0.5;
                *reinterpret_cast<std::uint16_t*>(g + 0x6c) =
                    d > 65535.0 ? 65535 : static_cast<std::uint16_t>(d);
            }
            if (m.rem[4] != 1.0)
            {
                const double d = g[0x81] * m.rem[4] + 0.5;
                g[0x81] = d > 255.0 ? 255 : static_cast<std::uint8_t>(d);
            }
            if (m.rem[5] != 1.0)
            {
                const double d = *reinterpret_cast<std::uint16_t*>(g + 0x1e) * m.rem[5] + 0.5;
                *reinterpret_cast<std::uint16_t*>(g + 0x1e) =
                    d > 65535.0 ? 65535 : static_cast<std::uint16_t>(d);
            }
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static std::uint8_t g_SafeDescRow[12] = {};
    static bool         g_HaveSafeDescRow = false;

    static bool EnsureSafeDescRow()
    {
        if (g_HaveSafeDescRow)
            return true;
        const int hi = kGunBasicParameters2StockSlots;
        unsigned char row[12];
        for (int wid = 1; wid <= hi; ++wid)
        {
            if (!GunBasic_ReadRowBytes(wid, row))
                continue;
            if (row[0] >= 1 && row[0] <= 253
                && row[1] >= 1 && row[1] <= 253
                && row[2] >= 1 && row[2] <= 253)
            {
                std::memcpy(g_SafeDescRow, row, 12);
                g_HaveSafeDescRow = true;
                return true;
            }
        }
        return false;
    }

    static int SubstituteEmptyDesc(void* desc, std::uint8_t saved[12], std::uint8_t mask[12])
    {
        for (int k = 0; k < 12; ++k)
            mask[k] = 0;
        if (!desc)
            return 0;
        if (ReadByteAtSEH(desc, 0) != 0)   // has a receiver -> real weapon, leave untouched
            return 0;
        if (!EnsureSafeDescRow())
            return 0;
        auto* d = static_cast<std::uint8_t*>(desc);
        int n = 0;
        for (int k = 0; k < 12; ++k)
            if (SwapByteSEH(d + k, g_SafeDescRow[k], &saved[k]) == 1)
            {
                mask[k] = 1;
                ++n;
            }
        return n > 0 ? 1 : 0;
    }

    static void RestoreSubstitutedDesc(void* desc, const std::uint8_t saved[12],
                                       const std::uint8_t mask[12])
    {
        if (!desc)
            return;
        auto* d = static_cast<std::uint8_t*>(desc);
        for (int k = 0; k < 12; ++k)
            if (mask[k])
                RestoreByteSEH(d + k, saved[k]);
    }

    static void FillCustomMotionEntriesEarly();

    static void __fastcall hkSetUpGunInfo(void* self, void* desc,
                                          unsigned int equipId, void* gunInfo,
                                          void* a5, void* a6, void* a7,
                                          void* a8, void* a9)
    {
        EnsureShadowsRelocated();
        FillCustomMotionEntriesEarly();

        const bool widen = PartIdWiden_IsActive();
        std::uint8_t wideDesc[24] = {};
        void* passDesc = desc;
        int wideIds[11] = {};
        bool haveWideIds = false;

        if (widen && desc)
        {
            const int sub = TppEquip_GetSubIdForEquipId(static_cast<int>(equipId));
            haveWideIds = (sub > 0) && GunBasic_GetLogicalParts(sub, wideIds);

            unsigned char narrow[12] = {};
            for (int k = 0; k < 12; ++k)
            {
                const int b = ReadByteAtSEH(desc, static_cast<size_t>(k));
                narrow[k] = (b > 0) ? static_cast<unsigned char>(b) : 0;
            }
            PartIdWiden_BuildWideDesc(narrow, haveWideIds ? wideIds : nullptr, wideDesc);
            passDesc = wideDesc;
        }
        else
        {
            LaneWindow_BindForBuild(desc, equipId);
        }
        std::uint8_t descSaved[12], descMask[12];
        const int substituted = SubstituteEmptyDesc(desc, descSaved, descMask);
        if (substituted)
        {
            static unsigned long long lastMs = 0;
            const unsigned long long now = GetTickCount64();
            if (now - lastMs > 2000)
            {
                lastMs = now;
                LogDebug("[EquipParam] equipId=%u has no gunBasic row - substituted a safe "
                    "vanilla weapon for setup so it renders as a harmless dummy instead "
                    "of crashing. Fix: give this weapon a valid SetGunBasic.\n", equipId);
            }
        }

        PoolSwapState st;
        PrepareGunInfoSwap(desc, st);
        const int ok = CallOrigGunInfoSEH(self, passDesc, equipId, gunInfo,
                                          a5, a6, a7, a8, a9);
        RestoreGunInfoSwap(st);

        if (substituted)
            RestoreSubstitutedDesc(desc, descSaved, descMask);

        if (ok == 1 && gunInfo && !g_BarrelExtraMult.empty())
        {
            const int ba = (widen && haveWideIds) ? wideIds[1] : ReadByteAtSEH(desc, 1);
            if (ba > 0)
            {
                const auto it = g_BarrelExtraMult.find(ba);
                if (it != g_BarrelExtraMult.end()
                    && ApplyBarrelExtraSEH(gunInfo, it->second) != 1)
                    LogDebug("[EquipParam] barrel extra multiplier: GunInfo write "
                        "faulted for barrelId=%d - engine base values kept\n", ba);
            }
        }

        if (ok == 1 && gunInfo)
        {
            const int rc = (widen && haveWideIds) ? wideIds[0] : ReadByteAtSEH(desc, 0);
            const int ub = (widen && haveWideIds) ? wideIds[8] : ReadByteAtSEH(desc, 10);
            int ubRc = 0;
            if (ub > 0)
                ubRc = PartRowByte(g_UnderBarrel, ub, 0);

            static std::mutex mx;
            static std::set<int> logged;
            auto shouldLog = [&](int key) {
                std::lock_guard<std::mutex> lock(mx);
                return logged.size() < 64 && logged.insert(key).second;
            };

            if (rc > 0 && !g_ReceiverMotionDonor.empty())
            {
                const auto it = g_ReceiverMotionDonor.find(rc);
                if (it != g_ReceiverMotionDonor.end() && it->second > 0 && it->second < 256
                    && ReadByteAtSEH(gunInfo, 0x7a) == (rc & 0xFF)
                    && WriteByteAtSEH(gunInfo, 0x7a, static_cast<std::uint8_t>(it->second)) == 1
                    && shouldLog(rc))
                    LogDebug("[ChimeraMotion] receiverId=%d part-motion row redirected to donor "
                        "receiverId=%d - the custom receiver has no row in the 233-entry "
                        "part-motion table, so its bolt/slide reads the donor's populated "
                        "row instead of a blank one.\n",
                        rc, it->second);
            }

            if (ubRc > 0 && !g_ReceiverMotionDonor.empty())
            {
                const auto it = g_ReceiverMotionDonor.find(ubRc);
                if (it != g_ReceiverMotionDonor.end() && it->second > 0 && it->second < 256
                    && ReadByteAtSEH(gunInfo, 0x7b) == (ubRc & 0xFF)
                    && WriteByteAtSEH(gunInfo, 0x7b, static_cast<std::uint8_t>(it->second)) == 1
                    && shouldLog(0x10000 + ubRc))
                    LogDebug("[ChimeraMotion] under-barrel receiverId=%d part-motion row redirected "
                        "to donor receiverId=%d (second weapon block, gunInfo+0x7b).\n",
                        ubRc, it->second);
            }

#ifdef _DEBUG
            if ((ub > 0 || rc >= 234) && shouldLog(0x20000 + static_cast<int>(equipId)))
            {
                const int opt2 =
                    (widen && haveWideIds) ? wideIds[9] : ReadByteAtSEH(desc, 8);
                int b[12];
                for (int i = 0; i < 12; ++i)
                    b[i] = ReadByteAtSEH(gunInfo, 0x74 + i);
                int nd[12];
                for (int i = 0; i < 12; ++i)
                    nd[i] = ReadByteAtSEH(desc, static_cast<size_t>(i));
                const int fr =
                    ReadByteAtSEH(gunInfo, 0x68) | (ReadByteAtSEH(gunInfo, 0x69) << 8);
                const unsigned trigWord =
                    static_cast<unsigned>(ReadByteAtSEH(gunInfo, 0x88))
                    | (static_cast<unsigned>(ReadByteAtSEH(gunInfo, 0x89)) << 8)
                    | (static_cast<unsigned>(ReadByteAtSEH(gunInfo, 0x8a)) << 16)
                    | (static_cast<unsigned>(ReadByteAtSEH(gunInfo, 0x8b)) << 24);
                const int trig = (trigWord >> 10) & 0x7;
                LogDebug("[ChimeraMotion] rowbytes eq=%u rc=%d ub=%d ubRc=%d opt2=%d "
                    "gunInfo+0x74..0x7f: "
                    "%02X %02X %02X %02X | mt=%02X %02X row=%02X %02X | hw=%02X %02X %02X %02X "
                    "|| fireRate(+0x68)=%d trigger(+0x88 bits10-12)=%d [+0x88 word=0x%08X] "
                    "|| engineDesc: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                    "(fireRate is rpm*10; a full-auto AR ~ 6000. trigger 0=single/semi, "
                    "non-0=auto/burst - this is the LIVE value the fire FSM reads. engineDesc is "
                    "the game's OWN 12-byte parts desc before substitution: byte 10 is the "
                    "under-barrel/foregrip, bytes 8 and 9 are the two light/laser slots)\n",
                    equipId, rc, ub, ubRc, opt2,
                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
                    fr, trig, trigWord,
                    nd[0], nd[1], nd[2], nd[3], nd[4], nd[5],
                    nd[6], nd[7], nd[8], nd[9], nd[10], nd[11]);
            }
#endif
        }
    }
}

namespace
{
    static std::uint8_t* g_BulletSwapLoc   = nullptr;
    static std::uint8_t  g_BulletSwapSaved = 0;
}

int EquipParam_BulletFalloffSwapBegin(int bulletId)
{
    if (bulletId <= 0 || g_BulletBaseOverflow.empty())
        return 0;
    if (g_BulletSwapLoc)
        return 0;
    const auto it = g_BulletBaseOverflow.find(bulletId);
    if (it == g_BulletBaseOverflow.end())
        return 0;
    std::uint8_t* pool = PartCurrentBuf(g_BulletBase.pb);
    std::uint8_t* bbuf = PartCurrentBuf(g_Bullet);
    if (!pool || !bbuf)
        return 0;
    if (CopyPoolRowSEH(pool, kPoolScratch0, it->second, g_BulletBase.pb.stride) != 1)
        return 0;
    std::uint8_t* loc = bbuf + static_cast<size_t>(bulletId - 1) * 14 + 6;
    if (SwapByteSEH(loc, static_cast<std::uint8_t>(kPoolScratch0),
                    &g_BulletSwapSaved) != 1)
        return 0;
    g_BulletSwapLoc = loc;
    return 1;
}

void EquipParam_BulletFalloffSwapEnd()
{
    if (!g_BulletSwapLoc)
        return;
    RestoreByteSEH(g_BulletSwapLoc, g_BulletSwapSaved);
    g_BulletSwapLoc = nullptr;
}

namespace
{

    using SetupWeaponInfo_t = void(__fastcall*)(void* self, void* work, int slot);
    static SetupWeaponInfo_t g_OrigSetupWeaponInfo = nullptr;

    //   vtbl+0x20 -> family block code ; vtbl+0x40 -> validity ; vtbl+0x150 -> template.
    using Resolver20_t  = std::uint32_t(__fastcall*)(void* self, std::uint32_t key);
    using Resolver40_t  = char(__fastcall*)(void* self, std::uint32_t key);
    using Resolver150_t = void*(__fastcall*)(void* self, std::uint32_t key);
    static Resolver20_t  g_OrigResolver20  = nullptr;
    static Resolver40_t  g_OrigResolver40  = nullptr;
    static Resolver150_t g_OrigResolver150 = nullptr;
    static Resolver150_t g_Finder148       = nullptr;


    using PartCode_t = std::uint32_t(__fastcall*)(void* self, std::uint32_t partId);
    static PartCode_t g_OrigPartCode[5] = {};
    static void*      g_PartCodeAddr[5] = {};
    static bool       g_PartCodeExact[5] = {};
    static const size_t kPartVtblOff[5] = { 0x20, 0x28, 0x30, 0x38, 0x40 };
    static const int    kPartGbByte[5]  = { 0, 1, 2, 3, 10 };
    static thread_local std::uint32_t g_TlsCustomEquip  = 0;
    static thread_local std::uint32_t g_TlsVanillaEquip = 0;
    static void* g_ResolverB = nullptr;
    static std::set<std::uint32_t> g_PartLogged;

    struct FamilyGb
    {
        std::uint16_t subId = 0;
        unsigned char gb[12] = {};
        bool ok = false;
    };
    static std::map<std::uint32_t, FamilyGb> g_GbByEquipId;
    static void* g_Resolver20Addr  = nullptr;
    static void* g_Resolver40Addr  = nullptr;
    static void* g_Resolver150Addr = nullptr;
    static std::atomic<bool> g_ResolverHookTried{ false };
    static thread_local bool g_InSetupWeaponInfo = false;

    static std::mutex g_WeaponKeyMutex;
    static std::set<std::uint32_t> g_WeaponKeysLogged;

    static std::map<std::uint32_t, std::uint32_t> g_FamilyFrom;
    static std::atomic<bool> g_HasSwaps{ false };
    static std::set<std::uint32_t> g_FamilyLogged;
    static std::map<std::uint32_t, void*> g_TemplateByEquipId;
    static std::set<std::uint32_t> g_WeaponInfoDumped;

    static bool IsFamilyMappedEither(std::uint32_t equipId)
    {
        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
        if (g_FamilyFrom.count(equipId))
            return true;
        for (const auto& kv : g_FamilyFrom)
            if (kv.second == equipId)
                return true;
        return false;
    }

    static uintptr_t SetupWeaponInfoAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x14105fdb0ull;
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_4: return 0x14105fe20ull;
        case ::AddressSetRuntime::GameBuild::En_1_0_15_3: return 0x1410605f0ull;
        case ::AddressSetRuntime::GameBuild::Jp_1_0_15_3: return 0x141060640ull;
        default:                                          return 0;
        }
    }

    static std::uint32_t MapEquipId(std::uint32_t equipId)
    {
        if (!g_HasSwaps.load(std::memory_order_relaxed))
            return equipId;
        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
        const auto it = g_FamilyFrom.find(equipId);
        return it != g_FamilyFrom.end() ? it->second : equipId;
    }

    static int ReadU16SEH(const void* p, std::uint16_t& v)
    {
        __try
        {
            v = *static_cast<const std::uint16_t*>(p);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int WriteU8SEH(void* p, std::uint8_t v)
    {
        __try
        {
            *static_cast<std::uint8_t*>(p) = v;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static std::uint32_t GetEquipTypeForEquipId(std::uint32_t equipId)
    {
        const std::int32_t idx =
            EquipIdCompression::ComputeCompressed(static_cast<std::int32_t>(equipId));
        if (!EquipIdCompression::IsCompressedInBounds(idx))
            return 0;
        auto* words = static_cast<std::uint16_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
        std::uint16_t w = 0;
        if (!words || ReadU16SEH(words + idx, w) != 1)
            return 0;
        return w & 0x3F;
    }

    static bool GetGbForEquipId(std::uint32_t equipId, FamilyGb& out)
    {
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            const auto it = g_GbByEquipId.find(equipId);
            if (it != g_GbByEquipId.end())
            {
                out = it->second;
                return out.ok;
            }
        }
        FamilyGb f;
        const std::int32_t idx =
            EquipIdCompression::ComputeCompressed(static_cast<std::int32_t>(equipId));
        if (EquipIdCompression::IsCompressedInBounds(idx))
        {
            auto* words = static_cast<std::uint16_t*>(
                ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
            std::uint16_t w = 0;
            if (words && ReadU16SEH(words + idx, w) == 1)
            {
                f.subId = (w >> 6) & 0x3FF;
                if (f.subId != 0 && GunBasic_ReadRowBytes(f.subId, f.gb))
                    f.ok = true;
            }
        }
        if (f.ok && f.gb[0] != 0)
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            g_GbByEquipId[equipId] = f;
        }
        out = f;
        return f.ok && f.gb[0] != 0;
    }

    static std::atomic<std::uint32_t> g_PartCallCount{ 0 };

}

int EquipParam_GetWideAlias(int space, int wideId)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    WidePartState* w = WideStateFor(space, wideId);
    return (w && w->alias > 0 && w->alias <= 0xFF) ? w->alias : 0;
}

int EquipParam_GetWideReceiverDonor(int wideId)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    if (WidePartState* w = WideStateFor(kVanillaSpace_Receiver, wideId))
    {
        if (w->alias > 0 && w->alias <= 0xFF)
            return w->alias;
        if (w->motionFrom > 0 && w->motionFrom < 234)
            return w->motionFrom;
    }
    const auto it = g_ReceiverMotionDonor.find(wideId);
    if (it != g_ReceiverMotionDonor.end() && it->second > 0 && it->second < 234)
        return it->second;
    return 0;
}

void EquipParam_EnableWidePartIds(int newMaxId)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    PartBuffer* all[] = { &g_Magazine, &g_Stock, &g_Muzzle, &g_Sight, &g_Barrel,
                          &g_UnderBarrel, &g_Option, &g_Bullet, &g_RcvIdBuf };
    for (PartBuffer* pb : all)
    {
        if (!pb || pb->active || newMaxId <= pb->maxId)
            continue;
        pb->maxId = newMaxId;
    }
}

int EquipParam_GetDeclaredWeaponAttackId(int equipId)
{
    const int sub = TppEquip_GetSubIdForEquipId(equipId);
    if (sub <= 0)
        return 0;
    const int logical = GunBasic_GetLogicalPart(sub, kVanillaSpace_Receiver);
    if (logical < kWideIdBase)
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    WidePartState* w = WideStateFor(kVanillaSpace_Receiver, logical);
    if (w && w->rowSet && w->row.size() >= 2)
        return static_cast<int>(static_cast<unsigned>(w->row[0]) |
                                (static_cast<unsigned>(w->row[1]) << 8));

    if (!PartIdWiden_IsActive() || !g_RcvIdBuf.active || logical > g_RcvIdBuf.maxId)
        return 0;
    const int lo = PartRowByte(g_RcvIdBuf, logical, 0);
    const int hi = PartRowByte(g_RcvIdBuf, logical, 1);
    return static_cast<int>(static_cast<unsigned>(lo)
                            | (static_cast<unsigned>(hi) << 8));
}

namespace
{


    struct KeyTypeSwap
    {
        std::uint32_t* loc[16];
        std::uint32_t  saved[16];
        int n = 0;
    };

    static int SwapKeyWordsSEH(void* self, int slot,
                               const std::uint32_t* fromIds,
                               const std::uint32_t* toTypes, int m,
                               KeyTypeSwap* st)
    {
        __try
        {
            std::uint8_t* a = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(self) + 0x8);
            if (!a) return 0;
            a = *reinterpret_cast<std::uint8_t**>(a + 0x138);
            if (!a) return 0;
            a = *reinterpret_cast<std::uint8_t**>(a + 0xC8);
            if (!a) return 0;
            std::uint8_t* obj = *reinterpret_cast<std::uint8_t**>(a + 0x8);
            if (!obj) return 0;
            const int stride = *reinterpret_cast<int*>(obj + 0xC);
            std::uint32_t* table = *reinterpret_cast<std::uint32_t**>(obj + 0x40);
            if (!table || stride <= 0 || stride > 16 || slot < 0 || slot > 15)
                return 0;
            for (int j = 0; j < stride && st->n < 16; ++j)
            {
                std::uint32_t* w = table + static_cast<size_t>(stride) * slot + j;
                const std::uint32_t v = *w;
                if ((v >> 16) == 0xFFFF)
                    continue;
                const std::uint32_t eq = (v & 0xFFFF) >> 5;
                for (int i = 0; i < m; ++i)
                {
                    if (eq == fromIds[i] && toTypes[i] >= 1 && toTypes[i] <= 8
                        && (v & 0x1F) != toTypes[i])
                    {
                        st->loc[st->n] = w;
                        st->saved[st->n] = v;
                        *w = (v & ~0x1Fu) | toTypes[i];
                        ++st->n;
                        break;
                    }
                }
            }
            return st->n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void RestoreKeyWordsSEH(KeyTypeSwap* st)
    {
        __try
        {
            for (int i = 0; i < st->n; ++i)
                *st->loc[i] = st->saved[i];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        st->n = 0;
    }

 
    static std::atomic<std::uint32_t> g_LastFsmEquip{ 0 };

    using ExecStateChange_t = void(__fastcall*)(void*, void*, unsigned int, int);
    static ExecStateChange_t g_OrigExecStateChange = nullptr;
    static void* g_ExecStateChangeAddr = nullptr;
    static int ReadU32AtSEH(const void* base, size_t off, std::uint32_t& out);

    static uintptr_t AttackFsmStateChangeAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141040a00ull;
        default:                                          return 0;
        }
    }

    static const char* AtkStateName(int s)
    {
        static const char* const kNames[] = {
            "None", "BareHoldStart", "BareHold", "BareHoldEnd", "CrawlBareHoldMove",
            "Void5", "GunHoldStart", "GunHold", "GunHoldEnd", "GunFire", "GunFire2",
            "GunReload", "GunReload2", "GunCock", "CrawlGunHoldMove", "DisableGunHold",
            "GunHoldChangeDirection", "SnatchWeapon", "ShieldHoldStart", "ShieldHold",
            "ShieldHoldEnd", "GunHoldToShieldHold", "ShieldHoldToGunHold",
            "SpecialGunHold", "SpecialGunFire", "SpecialGunFire2", "SpecialGunAction",
            "SpecialMissionPrepare"
        };
        if (s >= 0 && s < static_cast<int>(sizeof(kNames) / sizeof(kNames[0])))
            return kNames[s];
        return "?";
    }

    static void __fastcall hkExecStateChange(void* work, void* impl,
                                             unsigned int player, int state)
    {
        std::uint32_t eq = 0, prevWord = 0;
        const int eqOk = ReadU32AtSEH(work, 0x24c, eq);
        if (eqOk && eq)
            g_LastFsmEquip.store(eq, std::memory_order_relaxed);
        const int prevOk = ReadU32AtSEH(work, 0x2c0, prevWord);
        if (eqOk && prevOk)
        {
            const int prev = static_cast<int>(prevWord & 0xFF);
            static std::atomic<std::uint64_t> lastKey{ 0xFFFFFFFFFFFFFFFFull };
            const std::uint64_t key =
                (static_cast<std::uint64_t>(eq) << 16) | (static_cast<std::uint64_t>(prev & 0xFF) << 8)
                | static_cast<std::uint64_t>(state & 0xFF);
            if (lastKey.exchange(key, std::memory_order_relaxed) != key)
                LogDebug("[WeaponKey] FSM eq=%u %s(%d) -> %s(%d)\n",
                    eq, AtkStateName(prev), prev, AtkStateName(state), state);
        }
        g_OrigExecStateChange(work, impl, player, state);
    }

    using MotionEntry_t = void*(__fastcall*)(void*, void*, void*, void**);
    static MotionEntry_t g_OrigMotionEntryA = nullptr;
    static MotionEntry_t g_OrigMotionEntryB = nullptr;
    static void* g_MotionEntryAAddr = nullptr;
    static void* g_MotionEntryBAddr = nullptr;

    static void ProbeMotionEntrySEH(void** ent, int& type, unsigned long long& path,
                                    unsigned long long* payload, char* nodeHex, size_t nodeHexCap)
    {
        __try
        {
            if (!ent) return;
            void** vt = *reinterpret_cast<void***>(ent);
            if (!vt) return;
            using TypeFn = int (__fastcall*)(void*);
            using PathFn = void (__fastcall*)(void*, void*, int);
            type = reinterpret_cast<TypeFn>(vt[0x20 / 8])(ent);
            unsigned long long out = 0;
            reinterpret_cast<PathFn>(vt[0x18 / 8])(ent, &out, 0);
            path = out;

            unsigned long long* raw = reinterpret_cast<unsigned long long*>(ent);
            for (int i = 0; i < 5; ++i)
                payload[i] = raw[2 + i];

            const unsigned char* node = reinterpret_cast<const unsigned char*>(payload[1]);
            if (node && nodeHexCap > 8)
            {
                size_t w = 0;
                for (int i = 0; i < 24 && w + 3 < nodeHexCap; ++i)
                    w += (size_t)sprintf_s(nodeHex + w, nodeHexCap - w, "%02X ", node[i]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void LogMotionEntry(const char* which, void** ent)
    {
        int type = -1;
        unsigned long long path = 0;
        unsigned long long payload[5] = {};
        char nodeHex[128] = {};
        ProbeMotionEntrySEH(ent, type, path, payload, nodeHex, sizeof(nodeHex));
        const bool malformed = (path >> 48) == 0;
        const std::uint32_t eq = g_LastFsmEquip.load(std::memory_order_relaxed);
        static std::mutex mx;
        static std::set<std::pair<std::uint32_t, unsigned long long>> seen;
        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(mx);
            if (seen.size() < 512)
                isNew = seen.insert({ eq, payload[1] }).second;
        }
        if (isNew)
            LogDebug("[WeaponKey] motionEntry %s eq=%u slot node=%016llX -> path=%016llX%s\n"
                "             argBlk=%016llX binder=%016llX user=%016llX chunk=%016llX\n"
                "             node[0..23]= %s\n",
                which, eq, payload[1], path, malformed ? "  <<< MALFORMED" : "",
                payload[0], payload[2], payload[3], payload[4], nodeHex);
    }

    static thread_local void* g_TlsMotionNode = nullptr;

    static void* MotionEntryNodeSEH(void** ent)
    {
        __try
        {
            return ent ? reinterpret_cast<void**>(ent)[3] : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* __fastcall hkMotionEntryA(void* a, void* b, void* c, void** ent)
    {
        void* prev = g_TlsMotionNode;
        g_TlsMotionNode = MotionEntryNodeSEH(ent);
#ifdef _DEBUG
        LogMotionEntry("A", ent);
#endif
        void* r = g_OrigMotionEntryA(a, b, c, ent);
        g_TlsMotionNode = prev;
        return r;
    }

    static void* __fastcall hkMotionEntryB(void* a, void* b, void* c, void** ent)
    {
        void* prev = g_TlsMotionNode;
        g_TlsMotionNode = MotionEntryNodeSEH(ent);
#ifdef _DEBUG
        LogMotionEntry("B", ent);
#endif
        void* r = g_OrigMotionEntryB(a, b, c, ent);
        g_TlsMotionNode = prev;
        return r;
    }

    using GetAnimFromGani_t = void*(__fastcall*)(void*, unsigned long long);
    static GetAnimFromGani_t g_OrigGetAnimFromGani = nullptr;
    static void* g_GetAnimFromGaniAddr = nullptr;

    static uintptr_t GetAnimFileFromGaniPathAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141a6c300ull;
        default:                                          return 0;
        }
    }

    using GetStringValueByIndex_t =
        unsigned long long*(__fastcall*)(void*, unsigned long long*, unsigned long long, int*);
    static GetStringValueByIndex_t g_OrigGetStringValueByIndex = nullptr;
    static void* g_GetStringValueByIndexAddr = nullptr;

    static uintptr_t GetStringValueByIndexAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141c64420ull;
        default:                                          return 0;
        }
    }

    using GetControlString_t =
        unsigned long long*(__fastcall*)(void*, unsigned long long*, unsigned int);
    static GetControlString_t g_OrigGetControlString = nullptr;
    static void* g_GetControlStringAddr = nullptr;

    static uintptr_t GetControlStringAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141c60750ull;
        default:                                          return 0;
        }
    }

    static unsigned long long* __fastcall hkGetControlString(
        void* ctx, unsigned long long* out, unsigned int index)
    {
        unsigned long long* r = g_OrigGetControlString(ctx, out, index);

        if (out && r)
        {
            const unsigned long long v = *out;
            if (v != 0 && v != 0xb8a0bf169f98ull && (v >> 48) == 0)
            {
                static std::mutex mx;
                static std::set<unsigned long long> logged;
                bool logIt = false;
                {
                    std::lock_guard<std::mutex> lock(mx);
                    if (logged.size() < 16)
                        logIt = logged.insert(v).second;
                }
                if (logIt)
                    LogDebug("[WeaponKey] control port %u holds %016llX, which carries no PathId type "
                        "code - this blend layer was never bound to a clip. The usual cause is a "
                        "part id past the end of one of MotionLoaderImpl's type tables, so that "
                        "part contributed no mtar; check the [ChimeraMotion] lines for a part "
                        "reporting no animation type.\n",
                        index, v);
            }
        }
        return r;
    }

    static void ReadBinderArraysSEH(void* binder, unsigned long long& tbl,
                                    unsigned long long& valuesBase, unsigned long long& flagsBase)
    {
        __try
        {
            const unsigned long long t = *reinterpret_cast<unsigned long long*>(
                reinterpret_cast<char*>(binder) + 8);
            tbl = t;
            if (!t) return;
            valuesBase = *reinterpret_cast<unsigned long long*>(
                reinterpret_cast<char*>(t) + 0x10);
            flagsBase = *reinterpret_cast<unsigned long long*>(
                reinterpret_cast<char*>(t) + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static unsigned long long* __fastcall hkGetStringValueByIndex(
        void* binder, unsigned long long* out, unsigned long long index, int* err)
    {
        unsigned long long* r = g_OrigGetStringValueByIndex(binder, out, index, err);

        const unsigned long long got = (out && r) ? *out : 0;
        const bool bad = got != 0 && got != 0xb8a0bf169f98ull && (got >> 48) == 0;

        static std::mutex mx;
        static std::set<unsigned long long> seenIdx;
        bool logIt = false;
        {
            std::lock_guard<std::mutex> lock(mx);
            if (bad || seenIdx.size() < 24)
                logIt = seenIdx.insert(index | (bad ? 0x1000000ull : 0ull)).second;
        }

        if (logIt)
        {
            unsigned long long tbl = 0, valuesBase = 0, flagsBase = 0;
            ReadBinderArraysSEH(binder, tbl, valuesBase, flagsBase);
            LogDebug("[WeaponKey] stringValue idx=%llu(0x%llX) -> %016llX%s | binder=%p tbl=%016llX "
                "values=%016llX(+0x%llX) flags=%016llX(+0x%llX) err=%d\n",
                index, index, got, bad ? "  <<< MALFORMED" : "",
                binder, tbl,
                valuesBase, index * 0x18ull,
                flagsBase, index * 8ull,
                err ? *err : 0);
        }
        return r;
    }

    using RealizedEquipRealize_t =
        unsigned long long(__fastcall*)(void*, unsigned int, unsigned int);
    static RealizedEquipRealize_t g_OrigRealizedEquipRealize = nullptr;
    static void* g_RealizeAddr = nullptr;

    static uintptr_t RealizedEquipRealizeAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141307d70ull;
        default:                                          return 0;
        }
    }

    static void* ReadPtrAtSEH(void* base, size_t off);

    static void*            g_GunInfoAccessor = nullptr;
    static bool             g_LanePinned[256] = {};
    static std::uint64_t    g_LanePinnedStamp = 0;
    static std::atomic<int> g_LaneProbeLogged{ 0 };
    static std::atomic<int> g_PoolMaxOccupied{ 0 };

    static int RefreshPinnedLanes()
    {
        void* rA   = g_GunInfoAccessor;
        void* pool = rA ? ReadPtrAtSEH(rA, 0x1d8) : nullptr;
        if (!pool)
            return -1;
        for (int i = 0; i < 256; ++i)
            g_LanePinned[i] = false;
        int occupied = 0;
        for (int e = 0; e < 48; ++e)
        {
            std::uint8_t* ep = static_cast<std::uint8_t*>(pool)
                + static_cast<std::size_t>(e) * 0x90;
            std::uint16_t tag = 0x7FF;
            if (ReadU16SEH(ep + 0x5e, tag) != 1)
                continue;
            if (tag == 0x7FF || tag == 0)
                continue;
            ++occupied;
            const int sub = TppEquip_GetSubIdForEquipId(static_cast<int>(tag));
            if (sub <= 0)
                continue;
            unsigned char row[12] = {};
            if (!GunBasic_ReadRowBytes(sub, row))
                continue;
            if (row[0])
                g_LanePinned[row[0]] = true;
        }
        return occupied;
    }


    static void LaneProbe_NoteReceiverRead(unsigned int receiverId)
    {
        if (receiverId == 0 || receiverId >= 256)
            return;
        if (!g_LaneIsCustomRcv[receiverId].load(std::memory_order_relaxed))
            return;
        if (!g_GunInfoAccessor)
            return;

        const std::uint64_t now = GetTickCount64();
        if (g_LanePinnedStamp == 0 || now - g_LanePinnedStamp > 250)
        {
            const int occupied = RefreshPinnedLanes();
            if (occupied < 0)
                return;
            g_LanePinnedStamp = now ? now : 1;
            if (occupied > g_PoolMaxOccupied.load(std::memory_order_relaxed))
            {
                g_PoolMaxOccupied.store(occupied, std::memory_order_relaxed);
                LogDebug("[LaneProbe] gunInfo template pool high-water: %d of 48 "
                    "entries occupied\n", occupied);
            }
        }
        if (g_LanePinned[receiverId])
            return;
        if (g_LaneProbeLogged.fetch_add(1, std::memory_order_relaxed) < 24)
            LogDebug("[LaneProbe] receiver lane %u read while NO gunInfo in the "
                "template pool references it - evicting that lane at this "
                "moment would have handed this caller another weapon's "
                "receiver\n", receiverId);
    }

    static bool ZeroRcvLaneSEH(std::uint8_t* buf, int alias)
    {
        __try
        {
            std::memset(buf + static_cast<size_t>(alias - 1) * kReceiverStride,
                        0, kReceiverStride);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void EvictUnpinnedReceiverLanesLocked(std::vector<int>& freed)
    {
        freed.clear();
        if (RefreshPinnedLanes() < 0)
            return;
        g_LanePinnedStamp = GetTickCount64();

        std::uint8_t* buf = PartCurrentBuf(g_RcvIdBuf);
        if (!buf)
            return;

        for (auto& kv : g_WideParts[kVanillaSpace_Receiver])
        {
            WidePartState& w = kv.second;
            if (w.alias <= 0 || w.alias >= 256)
                continue;
            if (g_LanePinned[w.alias])
                continue;

            if (!ZeroRcvLaneSEH(buf, w.alias))
                continue;

            g_RcvClaimed.erase(w.alias);
            g_LaneIsCustomRcv[w.alias].store(false, std::memory_order_relaxed);
            g_ReceiverMotionDonor.erase(w.alias);
            g_RcvOverflow.erase(w.alias);
            freed.push_back(w.alias);
            w.alias = 0;
        }
    }

    static bool PatchDescWideSlotsSEH(void* desc, const unsigned char lanes[12])
    {
        __try
        {
            std::uint8_t* d = static_cast<std::uint8_t*>(desc);
            for (int i = 0; i < 11; ++i)
                if (lanes[i])
                    d[i] = lanes[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void LaneWindow_BindForBuild(void* desc, unsigned int equipId)
    {
        if (!desc)
            return;
        const int sub = TppEquip_GetSubIdForEquipId(static_cast<int>(equipId));
        if (sub <= 0)
            return;

        unsigned char lanes[12] = {};
        if (!GunBasic_WeaponNeedsLaneBind(sub))
        {
            // Already bound: the desc snapshot can still be stale (built before
            // an earlier bind, or holding a lane since reassigned), so restamp.
            if (GunBasic_GetWideSlotLanes(sub, kVanillaSpace_Receiver, lanes) > 0)
                PatchDescWideSlotsSEH(desc, lanes);
            return;
        }

        // Lock order is GunBasic -> EquipParam everywhere else (rebind reaches
        // ResolvePartByte), so the EquipParam lock is released before any
        // GunBasic call below.
        std::vector<int> freed;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            EvictUnpinnedReceiverLanesLocked(freed);
        }
        for (int lane : freed)
            GunBasic_ClearLaneFromRows(kVanillaSpace_Receiver, lane);

        const int bound = GunBasic_RebindWidePartsForWeapon(sub);
        if (GunBasic_GetWideSlotLanes(sub, kVanillaSpace_Receiver, lanes) > 0)
            PatchDescWideSlotsSEH(desc, lanes);
        if (bound == 0)
        {
            static unsigned long long lastMs = 0;
            const unsigned long long now = GetTickCount64();
            if (now - lastMs > 2000)
            {
                lastMs = now;
                Log("[LaneWindow] WARNING: equipId %u (weaponId %d) needed a part "
                    "lane and none could be bound - every lane is held by a "
                    "resident gunInfo. This weapon builds with no receiver this "
                    "time and resolves once another weapon leaves the pool.\n",
                    equipId, sub);
            }
        }
    }

    static void* CallDescProviderSEH(void* self)
    {
        __try
        {
            void* provider = *reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(self) + 0x48);
            if (!provider)
                return nullptr;
            void** vtbl = *reinterpret_cast<void***>(provider);
            if (!vtbl || !vtbl[0])
                return nullptr;
            using Vtbl0_t = void*(__fastcall*)(void*);
            return reinterpret_cast<Vtbl0_t>(vtbl[0])(provider);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    using PostPutMotion_t =
        unsigned long long(__fastcall*)(void*, void*, void*, void*);
    static PostPutMotion_t g_OrigPostPutMotion = nullptr;
    static void* g_PostPutMotionAddr = nullptr;

    static uintptr_t PostPutMotionAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141307150ull;
        default:                                          return 0;
        }
    }

    static unsigned ReadRecWord(void* base, int slot, int off)
    {
        const int lo = ReadByteAtSEH(base, static_cast<size_t>(slot) * 0x14 + off);
        const int hi = ReadByteAtSEH(base, static_cast<size_t>(slot) * 0x14 + off + 1);
        if (lo < 0 || hi < 0)
            return 0xFFFFFFFFu;
        return static_cast<unsigned>(lo) | (static_cast<unsigned>(hi) << 8);
    }

    static bool ReadAmmoRowSEH(void* self, int slot, unsigned& idx, int& b0B, int& b0C)
    {
        __try
        {
            std::uint8_t* p = static_cast<std::uint8_t*>(self);
            void* p60 = *reinterpret_cast<void**>(p + 0x60);
            if (!p60) return false;
            void* map = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(p60) + 0x10);
            if (!map) return false;
            const int base54 = *reinterpret_cast<int*>(p + 0x54);
            idx = *reinterpret_cast<unsigned*>(
                static_cast<std::uint8_t*>(map) +
                static_cast<size_t>(base54 + slot) * 4);
            if (idx == 0xFFFFFFFFu) return false;
            void* p58 = *reinterpret_cast<void**>(p + 0x58);
            if (!p58) return false;
            void* rows = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(p58) + 0x28);
            if (!rows) return false;
            std::uint8_t* row = static_cast<std::uint8_t*>(rows) +
                                static_cast<size_t>(idx) * 0xE;
            b0B = row[0xB];
            b0C = row[0xC];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static thread_local int t_probeSlots = 0;
    static thread_local unsigned t_probeSub[8] = { 0 };
    static thread_local int t_probeBones[8] = { 0 };
    static thread_local int t_probeC[8] = { 0 };
    static thread_local unsigned t_probeAmmo[8] = { 0 };

    static thread_local unsigned t_meshEq = 0;
    static thread_local int t_meshCount = 0;
    static thread_local unsigned t_meshHash[64] = { 0 };
    static thread_local unsigned char t_meshFA[64] = { 0 };
    static thread_local unsigned char t_meshFB[64] = { 0 };
    static thread_local void* t_cylCtrl = nullptr;
    static thread_local int t_cylSlot = 0;
    static thread_local void* t_cylModel = nullptr;
    static thread_local unsigned t_cylAmmo = 0;
    static thread_local unsigned t_drivenHash = 0;
    static thread_local int t_roundCount = 0;

    static void FeedCylinderVisibilitySEH(void* self)
    {
        t_probeSlots = 0;
        t_meshCount = 0;
        t_cylCtrl = nullptr;
        __try
        {
            std::uint8_t* p = static_cast<std::uint8_t*>(self);
            void* ctrl = *reinterpret_cast<void**>(p + 0x30);
            void* recsV = *reinterpret_cast<void**>(p + 0xE8);
            void* p60 = *reinterpret_cast<void**>(p + 0x60);
            void* p58 = *reinterpret_cast<void**>(p + 0x58);
            if (!ctrl || !recsV || !p60 || !p58) return;
            void* ctrlModels = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(ctrl) + 0x58);
            void* map = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(p60) + 0x10);
            void* rows = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(p58) + 0x28);
            if (!ctrlModels || !map || !rows) return;
            const int slotCount = *reinterpret_cast<int*>(p + 0x50);
            const int lim = slotCount < 0 ? 0 : (slotCount > 8 ? 8 : slotCount);
            const int base54 = *reinterpret_cast<int*>(p + 0x54);
            std::uint8_t* recs = static_cast<std::uint8_t*>(recsV);
            for (int slot = 0; slot < lim; ++slot)
            {
                std::uint8_t* rec = recs + static_cast<size_t>(slot) * 0x14;
                t_probeSub[slot] = (static_cast<unsigned>(rec[4]) |
                                    (static_cast<unsigned>(rec[5]) << 8)) & 0x7FFu;
                void* model = *reinterpret_cast<void**>(
                    static_cast<std::uint8_t*>(ctrlModels) + static_cast<size_t>(slot) * 8);
                const int bones = model
                    ? *reinterpret_cast<std::int16_t*>(static_cast<std::uint8_t*>(model) + 0xF8)
                    : -1;
                t_probeBones[slot] = bones;
                const unsigned idx = *reinterpret_cast<unsigned*>(
                    static_cast<std::uint8_t*>(map) + static_cast<size_t>(base54 + slot) * 4);
                int b0C = -1;
                unsigned ammo = 0;
                if (idx != 0xFFFFFFFFu)
                {
                    std::uint8_t* row = static_cast<std::uint8_t*>(rows) +
                                        static_cast<size_t>(idx) * 0xE;
                    b0C = row[0xC];
                    ammo = *reinterpret_cast<std::uint16_t*>(row + 4);
                }
                t_probeC[slot] = b0C;
                t_probeAmmo[slot] = ammo;
                if ((b0C & 0x20) && bones > 0 && !t_cylCtrl)
                {
                    t_cylCtrl = ctrl;
                    t_cylSlot = slot;
                    t_cylModel = model;
                    t_cylAmmo = ammo;
                    const unsigned mc = *reinterpret_cast<std::uint16_t*>(
                        static_cast<std::uint8_t*>(model) + 0x1E8);
                    void* harr = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(model) + 0x180);
                    void* fa = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(model) + 0x178);
                    void* fb = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(model) + 0x170);
                    if (harr && mc)
                    {
                        const unsigned n = mc > 64 ? 64 : mc;
                        for (unsigned i = 0; i < n; ++i)
                        {
                            t_meshHash[i] = *reinterpret_cast<std::uint32_t*>(
                                static_cast<std::uint8_t*>(harr) + i * 4);
                            t_meshFA[i] = fa ? *(static_cast<std::uint8_t*>(fa) + i) : 0xFF;
                            t_meshFB[i] = fb ? *(static_cast<std::uint8_t*>(fb) + i) : 0xFF;
                        }
                        t_meshCount = static_cast<int>(n);
                        t_meshEq = t_probeSub[slot];
                    }
                }
            }
            t_probeSlots = lim;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void DriveMeshSEH(void* ctrl, unsigned slot, unsigned long long hash, int flag)
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>(ctrl);
            auto fn = reinterpret_cast<void(__fastcall*)(void*, unsigned, unsigned long long, int)>(
                vt[0x100 / 8]);
            fn(ctrl, slot, hash, flag);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    struct RoundMeshList { unsigned hash[16]; int count; };
    static std::mutex g_roundsMeshMx;
    static std::map<void*, RoundMeshList> g_roundsMesh;
    static void DriveCylinderRounds()
    {
        t_drivenHash = 0;
        t_roundCount = 0;
        if (!t_cylCtrl || t_meshCount <= 0) return;
        RoundMeshList rl;
        {
            std::lock_guard<std::mutex> lock(g_roundsMeshMx);
            auto it = g_roundsMesh.find(t_cylModel);
            if (it != g_roundsMesh.end())
            {
                rl = it->second;
            }
            else
            {
                rl.count = 0;
                for (int i = 0; i < t_meshCount && rl.count < 16; ++i)
                    if (t_meshFA[i] == 0xFF)                 // hidden-by-default = a loaded-round mesh
                        rl.hash[rl.count++] = t_meshHash[i];
                g_roundsMesh[t_cylModel] = rl;
            }
        }
        const int show = t_cylAmmo > 16u ? 16 : static_cast<int>(t_cylAmmo);
        for (int i = 0; i < rl.count; ++i)
            DriveMeshSEH(t_cylCtrl, static_cast<unsigned>(t_cylSlot),
                         static_cast<unsigned long long>(rl.hash[i]), i < show ? 1 : 0);
        t_roundCount = rl.count;
        if (rl.count > 0) t_drivenHash = rl.hash[0];
    }

    struct CylBoneCandidate { unsigned hash; const char* name; };
    static const CylBoneCandidate kCylBoneCandidates[] = {
        { 0x69E02A5Eu, "SKL_002_CYLINDER" },
        { 0x23E96556u, "SKL_001_CYLINDER" },
        { 0xD7E97EC3u, "SKL_003_CYLINDER" },
        { 0x23227DE0u, "SKL_004_CYLINDER" },
        { 0xC0889846u, "SKL_005_CYLINDER" },
        { 0x8D655BE1u, "SKL_006_CYLINDER" },
        { 0xF7719044u, "WEAPON_CYLINDER" },
        { 0x26A4DD2Fu, "CYLINDER" },
    };

    struct CylRotState
    {
        int boneIdx = -2;               // -2 = unresolved, -1 = no cylinder bone (skip), >=0 = bone index
        unsigned chambers = 5;
        unsigned prevAmmo = 0xFFFFFFFFu;
        double target = 0.0;
        double current = 0.0;
        bool haveBase = false;
        float base[16] = {};
        float lastWritten[16] = {};
    };
    static std::mutex g_cylRotMx;
    static std::map<void*, CylRotState> g_cylRot;

    static int ResolveBoneIdxSEH(void* ctrl, unsigned slot, unsigned hash)
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>(ctrl);
            auto fn = reinterpret_cast<unsigned long long(__fastcall*)(
                void*, unsigned long long, unsigned long long, unsigned long long)>(vt[0x140 / 8]);
            return static_cast<int>(fn(ctrl, slot, static_cast<unsigned long long>(hash), 0));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static bool ReadMtx64SEH(void* model, int idx, float* out16)
    {
        __try
        {
            void* pose = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(model) + 0xE0);
            if (!pose) return false;
            memcpy(out16, static_cast<std::uint8_t*>(pose) + static_cast<size_t>(idx) * 0x40, 0x40);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool WriteMtx64SEH(void* model, int idx, const float* in16)
    {
        __try
        {
            void* pose = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(model) + 0xE0);
            if (!pose) return false;
            memcpy(static_cast<std::uint8_t*>(pose) + static_cast<size_t>(idx) * 0x40, in16, 0x40);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void DriveCylinderRotation()
    {
        if (!t_cylCtrl || !t_cylModel) return;
        std::lock_guard<std::mutex> lock(g_cylRotMx);
        CylRotState& st = g_cylRot[t_cylModel];
        if (st.boneIdx == -2)
        {
            st.boneIdx = -1;
            for (const auto& cand : kCylBoneCandidates)
            {
                const int idx = ResolveBoneIdxSEH(
                    t_cylCtrl, static_cast<unsigned>(t_cylSlot), cand.hash);
                if (idx >= 0) { st.boneIdx = idx; break; }
            }
            st.chambers = (t_roundCount > 0) ? static_cast<unsigned>(t_roundCount) : 5u;
#ifdef _DEBUG
            static std::mutex mxL;
            static std::set<void*> seen;
            std::lock_guard<std::mutex> ll(mxL);
            if (seen.insert(t_cylModel).second)
                LogDebug("[WeaponKey] CYL-ROT eq=%u model=%p cylBone=%d chambers=%u (%s) - %s\n",
                    t_meshEq, t_cylModel, st.boneIdx, st.chambers,
                    st.boneIdx >= 0 ? "resolved" : "NO cylinder bone",
                    st.boneIdx >= 0 ? "spinning about local Z, 360/chambers per shot"
                                    : "model has no SKL_00N_CYLINDER bone - rotation skipped");
#endif
        }
        if (st.boneIdx < 0) return;

        float cur[16];
        if (!ReadMtx64SEH(t_cylModel, st.boneIdx, cur)) return;

        const float basis = fabsf(cur[0]) + fabsf(cur[1]) + fabsf(cur[2]) +
                            fabsf(cur[4]) + fabsf(cur[5]) + fabsf(cur[6]) +
                            fabsf(cur[8]) + fabsf(cur[9]) + fabsf(cur[10]);
        if (basis < 0.25f) return;

        if (!st.haveBase || memcmp(cur, st.lastWritten, sizeof(cur)) != 0)
        {
            memcpy(st.base, cur, sizeof(cur));
            st.haveBase = true;
        }
        if (st.prevAmmo == 0xFFFFFFFFu) st.prevAmmo = t_cylAmmo;
        if (t_cylAmmo != st.prevAmmo && st.chambers > 0)
        {
            int delta = static_cast<int>(st.prevAmmo) - static_cast<int>(t_cylAmmo);
            if (delta < 0) delta = -delta;
            const double step = 6.283185307179586 / static_cast<double>(st.chambers);
            st.target += step * static_cast<double>(delta);
        }
        st.prevAmmo = t_cylAmmo;
        st.current += (st.target - st.current) * 0.35;
        if (st.current >= 6.283185307179586 && st.target >= 6.283185307179586)
        {
            st.current -= 6.283185307179586;
            st.target -= 6.283185307179586;
        }
        const float c = static_cast<float>(cos(st.current));
        const float s = static_cast<float>(sin(st.current));
        float m[16];
        for (int j = 0; j < 4; ++j)
        {
            m[0 * 4 + j] =  c * st.base[0 * 4 + j] + s * st.base[1 * 4 + j];
            m[1 * 4 + j] = -s * st.base[0 * 4 + j] + c * st.base[1 * 4 + j];
            m[2 * 4 + j] = st.base[2 * 4 + j];
            m[3 * 4 + j] = st.base[3 * 4 + j];
        }
        WriteMtx64SEH(t_cylModel, st.boneIdx, m);
        memcpy(st.lastWritten, m, sizeof(m));
    }

    static bool WritePtrAtSEH(void* base, size_t off, void* val)
    {
        __try
        {
            *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + off) = val;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static std::uint32_t GetNativeSubId(std::uint32_t equipId)
    {
        const std::int32_t idx =
            EquipIdCompression::ComputeCompressed(static_cast<std::int32_t>(equipId));
        if (!EquipIdCompression::IsCompressedInBounds(idx))
            return 0;
        auto* words = static_cast<std::uint16_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
        std::uint16_t w = 0;
        if (!words || ReadU16SEH(words + idx, w) != 1)
            return 0;
        return (w >> 6) & 0x3FF;
    }

    static int MotionEntryIndex(std::uint32_t equipId)
    {
        if (equipId >= 1 && equipId < 0x7D)
            return static_cast<int>(equipId);
        if (equipId >= 0x400 && equipId < 0x450)
            return static_cast<int>(equipId - 899);
        return -1;
    }

    static void* ReadMotionEntry(void* table, std::uint32_t equipId)
    {
        const int idx = MotionEntryIndex(equipId);
        if (idx < 0)
            return nullptr;
        return ReadPtrAtSEH(table, 8 + static_cast<size_t>(idx) * 8);
    }

    static std::uint32_t NextDonorCandidate(std::uint32_t v)
    {
        if (v + 1 == 0x7D)
            return 0x400;
        if (v + 1 == 0x450)
            return 0;
        return v + 1;
    }

    struct MotionResidencyPlan
    {
        std::uint32_t donorEq = 0;
        std::uint64_t donorPack = 0;
        std::uint64_t familyPack = 0;
    };
    static std::map<std::uint32_t, MotionResidencyPlan> g_MotionResidency;
    static std::map<std::uint64_t, std::uint32_t> g_RemapEntryEq;

    using MtarGetAnimFileFn_t = void*(__fastcall*)(void*, unsigned long long);

    static std::atomic<void*> g_EquipDataProvider{ nullptr };

    static void CaptureEquipDataProvider(void* reoi)
    {
        if (!reoi || g_EquipDataProvider.load(std::memory_order_relaxed))
            return;
        void* provider = ReadPtrAtSEH(reoi, 0x68);
        void* vt = provider ? ReadPtrAtSEH(provider, 0) : nullptr;
        void* fn = vt ? ReadPtrAtSEH(vt, 0x10) : nullptr;
        if (!fn)
            return;
        g_EquipDataProvider.store(provider, std::memory_order_relaxed);
        LogDebug("[WeaponKey] entry->mtar resolution armed: EquipDataSystemImpl "
            "provider=%p vtbl=%p findByPathId(vtbl+0x10)=%p - this variant "
            "fetches its own fox::Block, unlike the vtbl+0x28 form which "
            "takes the block as a THIRD argument and was previously called "
            "with two (searching whatever stale block the last call left in "
            "R8, faulting inside fox::Block::GetFileForPathId)\n",
            provider, vt, fn);
    }

    static void* ResolveMtarForEntrySEH(unsigned long long entry)
    {
        void* provider = g_EquipDataProvider.load(std::memory_order_relaxed);
        if (!provider)
        {
            static std::atomic<bool> s_Warned{ false };
            if (!s_Warned.exchange(true))
                LogDebug("[WeaponKey] entry->mtar resolution is OFF until the "
                    "EquipDataSystemImpl provider is seen (first weapon "
                    "Realize): cross-family clip fallback and mtar "
                    "identification report not-found until then\n");
            return nullptr;
        }
        __try
        {
            void* vt = *reinterpret_cast<void**>(provider);
            if (!vt)
                return nullptr;
            void* fn = *reinterpret_cast<void**>(static_cast<char*>(vt) + 0x10);
            if (!fn)
                return nullptr;
            return reinterpret_cast<MtarGetAnimFileFn_t>(fn)(provider, entry);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* CallGetAnimFileSEH(void* fn, void* mtar, unsigned long long clip)
    {
        __try
        {
            return reinterpret_cast<MtarGetAnimFileFn_t>(fn)(mtar, clip);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        }
    }

    static int ReadDwordAtSEH2(void* base, size_t off)
    {
        int v = 0;
        for (int i = 0; i < 4; ++i)
            v |= (ReadByteAtSEH(base, off + static_cast<size_t>(i)) & 0xFF) << (8 * i);
        return v;
    }

    static MtarGetAnimFileFn_t g_OrigGetAnimFile = nullptr;
    static void* g_GetAnimFileAddr = nullptr;

    static void* FindClipInRegisteredArchivesSEH(void* mtar, unsigned long long clip,
                                                 std::uint64_t* servedFrom)
    {
        std::uint64_t cands[96];
        int cnt = 0;
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            for (const auto& kv : g_RemapEntryEq)
            {
                if (cnt >= 96)
                    break;
                cands[cnt++] = kv.first;
            }
        }
        for (int i = 0; i < cnt; ++i)
        {
            void* m = ResolveMtarForEntrySEH(cands[i]);
            if (!m || m == mtar)
                continue;
            void* af = CallGetAnimFileSEH(
                reinterpret_cast<void*>(g_OrigGetAnimFile), m, clip);
            if (af && reinterpret_cast<uintptr_t>(af) != 1)
            {
                if (servedFrom)
                    *servedFrom = cands[i];
                return af;
            }
        }
        return nullptr;
    }

    static void* __fastcall hkGetAnimFile(void* mtar, unsigned long long clip)
    {
        void* r = g_OrigGetAnimFile(mtar, clip);
        if (r || !mtar)
            return r;
        static thread_local int depth = 0;
        if (depth)
            return r;
        ++depth;
        std::uint64_t from = 0;
        void* alt = FindClipInRegisteredArchivesSEH(mtar, clip, &from);
        --depth;
        if (!alt)
            return r;
#ifdef _DEBUG
        static std::mutex mx;
        static std::set<unsigned long long> logged;
        bool isNew;
        {
            std::lock_guard<std::mutex> lock(mx);
            isNew = logged.size() < 64 && logged.insert(clip).second;
        }
        if (isNew)
            LogDebug("[WeaponKey] cross-family clip lookup: clip %016llX is not in the "
                "requesting motion archive - served from registered family archive "
                "%016llX (the DLL merges every archive a weapon's rows imply at "
                "lookup time, so mixed-family weapons play all their clips with "
                "nothing extra shipped)\n",
                clip, from);
#else
        (void)from;
#endif
        return alt;
    }

    using ClipSetFn_t =
        unsigned char(__fastcall*)(void*, unsigned int, unsigned long long);
    static ClipSetFn_t g_OrigClipSetByPath = nullptr;
    static void** g_ClipFallbackVtbl = nullptr;
    static int g_ClipFallbackSlot = -1;

    struct LastClipState
    {
        unsigned long long clip;
        bool foreign;
    };
    static std::map<std::uint64_t, LastClipState> g_LastClipByCtlSlot;
    static std::map<std::uintptr_t, LastClipState> g_LastClipByCtl;

    static void RecordClipState(void* ctl, unsigned int slot, unsigned long long clip,
                                bool foreign)
    {
        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
        if (g_LastClipByCtlSlot.size() > 512)
            g_LastClipByCtlSlot.clear();
        if (g_LastClipByCtl.size() > 128)
            g_LastClipByCtl.clear();
        const std::uint64_t key =
            (reinterpret_cast<std::uint64_t>(ctl) << 8) | (slot & 0xFF);
        g_LastClipByCtlSlot[key] = { clip, foreign };
        g_LastClipByCtl[reinterpret_cast<std::uintptr_t>(ctl)] = { clip, foreign };
    }

    static bool ClipForeignToBoundArchive(void* ctl, unsigned int slot,
                                          unsigned long long clip)
    {
        if (!g_OrigGetAnimFile)
            return false;
        void* slots = ReadPtrAtSEH(ctl, 0x60);
        void* outer = slots ? ReadPtrAtSEH(slots, static_cast<size_t>(slot) * 8)
                            : nullptr;
        if (!outer)
            return false;
        std::uint8_t* inner =
            static_cast<std::uint8_t*>(outer) + ReadDwordAtSEH2(outer, 0xc);
        void* bound = ReadPtrAtSEH(inner, 0x80);
        if (!bound)
            return false;
        void* af = CallGetAnimFileSEH(
            reinterpret_cast<void*>(g_OrigGetAnimFile), bound, clip);
        return !af || reinterpret_cast<uintptr_t>(af) == 1;
    }

    static unsigned char __fastcall hkClipArchiveFallback(void* ctl, unsigned int slot,
                                                          unsigned long long clip)
    {

        {
            void* mdlArr = ReadPtrAtSEH(ctl, 0x58);
            void* model = mdlArr ? ReadPtrAtSEH(mdlArr, static_cast<size_t>(slot) * 8) : nullptr;
            if (!model)
            {
#ifdef _DEBUG
                static std::mutex mxN;
                static std::set<std::uint64_t> loggedN;
                const std::uint64_t k =
                    (reinterpret_cast<std::uint64_t>(ctl) << 8) | (slot & 0xFF);
                bool isNewN;
                {
                    std::lock_guard<std::mutex> lk(mxN);
                    isNewN = loggedN.size() < 32 && loggedN.insert(k).second;
                }
                if (isNewN)
                    LogDebug("[WeaponKey] clip-set SKIPPED - model[slot] is null ctl=%p slot=%u "
                        "clip=%016llX (this part's fmdl did not load; SetMotionData would deref a "
                        "null model and crash - the weapon has a missing/bad .parts model for this "
                        "slot)\n",
                        ctl, slot, clip);
#endif
                return 0;
            }
        }
        const unsigned char r = g_OrigClipSetByPath(ctl, slot, clip);
        if (r)
        {
            RecordClipState(ctl, slot, clip, ClipForeignToBoundArchive(ctl, slot, clip));
            return r;
        }
        std::uint64_t cands[96];
        int cnt = 0;
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            for (const auto& kv : g_RemapEntryEq)
            {
                if (cnt >= 96)
                    break;
                cands[cnt++] = kv.first;
            }
        }
        if (cnt == 0)
            return 0;
        void* gaf = ResolveGameAddress(gAddr.Mtar_GetAnimFile);
        if (!gaf)
            return 0;
        void* slots = ReadPtrAtSEH(ctl, 0x60);
        void* outer = slots ? ReadPtrAtSEH(slots, static_cast<size_t>(slot) * 8)
                            : nullptr;
        if (!outer)
            return 0;
        std::uint8_t* inner =
            static_cast<std::uint8_t*>(outer) + ReadDwordAtSEH2(outer, 0xc);
        void* bound = ReadPtrAtSEH(inner, 0x80);
        void* afBound = bound ? CallGetAnimFileSEH(gaf, bound, clip) : nullptr;
        if (afBound && reinterpret_cast<uintptr_t>(afBound) != 1)
            return 0;
        for (int i = 0; i < cnt; ++i)
        {
            void* m = ResolveMtarForEntrySEH(cands[i]);
            if (!m || m == bound)
                continue;
            void* af = CallGetAnimFileSEH(gaf, m, clip);
            if (!af || reinterpret_cast<uintptr_t>(af) == 1)
                continue;
            if (!WritePtrAtSEH(inner, 0x80, m))
                continue;
            const unsigned char r2 = g_OrigClipSetByPath(ctl, slot, clip);
            if (r2)
            {
                RecordClipState(ctl, slot, clip, true);
#ifdef _DEBUG
                static std::mutex mx;
                static std::set<std::uint64_t> logged;
                bool isNew;
                {
                    std::lock_guard<std::mutex> lock(mx);
                    isNew = logged.size() < 64 && logged.insert(clip).second;
                }
                if (isNew)
                    LogDebug("[WeaponKey] clip archive fallback: clip %016llX was not in the "
                        "control's bound motion archive - rebound to registered family "
                        "archive %016llX and SET (a weapon can carry parts from several "
                        "animation families; each clip now plays from the archive that "
                        "actually contains it)\n",
                        clip, cands[i]);
#endif
                return r2;
            }
            WritePtrAtSEH(inner, 0x80, bound);
        }
#ifdef _DEBUG
        {
            static std::atomic<int> missBudget{ 6 };
            if (missBudget.fetch_sub(1) > 0)
            {
                int resolved = 0, found = 0;
                char detail[512];
                int dl = 0;
                detail[0] = 0;
                for (int i = 0; i < cnt; ++i)
                {
                    void* m = ResolveMtarForEntrySEH(cands[i]);
                    if (!m)
                        continue;
                    ++resolved;
                    void* af = CallGetAnimFileSEH(gaf, m, clip);
                    const bool hit =
                        af && reinterpret_cast<uintptr_t>(af) != 1;
                    if (hit)
                        ++found;
                    if (dl < 380)
                        dl += _snprintf_s(detail + dl,
                                          sizeof(detail) - dl, _TRUNCATE,
                                          " %016llX=%p%s", cands[i], m,
                                          hit ? "HIT"
                                              : (reinterpret_cast<uintptr_t>(af) == 1
                                                     ? "FAULT"
                                                     : ""));
                }
                LogDebug("[WeaponKey] fallback MISS clip=%016llX candidates=%d "
                    "resolved=%d containing=%d bound=%p |%s\n",
                    clip, cnt, resolved, found, bound, detail);
            }
        }
#endif
        return 0;
    }

    using HideMagazine_t =
        void(__fastcall*)(void*, unsigned long long, unsigned char);
    static HideMagazine_t g_OrigHideMagazine = nullptr;
    static void* g_HideMagazineAddr = nullptr;

    using GetPartsControllerFn_t =
        void*(__fastcall*)(void*, unsigned int, unsigned int*);

    static void* CallGetPartsControllerSEH(void* equipSys, unsigned int key,
                                           unsigned int* outIdx)
    {
        auto fn = reinterpret_cast<GetPartsControllerFn_t>(
            ResolveGameAddress(gAddr.EquipSystemImpl_GetPartsController));
        if (!fn)
            return nullptr;
        __try
        {
            return fn(static_cast<char*>(equipSys) - 0x20, key, outIdx);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void __fastcall hkHideMagazine(void* self, unsigned long long handle,
                                          unsigned char which)
    {
        const std::uint32_t eqpType = static_cast<std::uint32_t>(handle) & 0x1f;
        const std::uint32_t eq = (static_cast<std::uint32_t>(handle) >> 5) & 0x7ff;
        if (which == 0 && eqpType >= 1 && eqpType <= 8 &&
            TppEquip_GetSubIdForEquipId(static_cast<int>(eq)) != 0)
        {
            unsigned int idx = 0;
            void* ctl = CallGetPartsControllerSEH(
                self, static_cast<unsigned int>(handle), &idx);
            bool foreign = false, found = false;
            unsigned long long clip = 0;
            if (ctl)
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                const std::uint64_t key =
                    (reinterpret_cast<std::uint64_t>(ctl) << 8) | (idx & 0xFF);
                auto it = g_LastClipByCtlSlot.find(key);
                if (it != g_LastClipByCtlSlot.end())
                {
                    found = true;
                    foreign = it->second.foreign;
                    clip = it->second.clip;
                }
                else
                {
                    auto itc =
                        g_LastClipByCtl.find(reinterpret_cast<std::uintptr_t>(ctl));
                    if (itc != g_LastClipByCtl.end())
                    {
                        found = true;
                        foreign = itc->second.foreign;
                        clip = itc->second.clip;
                    }
                }
            }
#ifdef _DEBUG
            static std::atomic<int> hideBudget{ 24 };
            if (hideBudget.fetch_sub(1) > 0)
                LogDebug("[WeaponKey] HideMagazine eq=%u which=%u ctl=%p idx=%u "
                    "found=%d foreign=%d clip=%016llX RA=%p\n",
                    eq, which, ctl, idx, found ? 1 : 0, foreign ? 1 : 0, clip,
                    _ReturnAddress());
#endif
            if (foreign)
            {
                static std::set<std::uint32_t> suppressLogged;
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                    first = suppressLogged.insert(eq).second;
                }
                if (first)
                    LogDebug("[WeaponKey] main-magazine hide SUPPRESSED: eq=%u is playing "
                        "a cross-family clip (%016llX, an under-barrel action) and the "
                        "engine's reload handler asked to hide the weapon's OWN "
                        "magazine - on a mixed-family weapon that request belongs to "
                        "the attachment, not the rifle mag; hides during the weapon's "
                        "own reload still pass through\n",
                        eq, clip);
                return;
            }
        }
#ifdef _DEBUG
        else if (which != 0 && eqpType >= 1 && eqpType <= 8 &&
                 TppEquip_GetSubIdForEquipId(static_cast<int>(eq)) != 0)
        {
            static std::atomic<int> ubBudget{ 12 };
            if (ubBudget.fetch_sub(1) > 0)
                LogDebug("[WeaponKey] HideMagazine eq=%u which=%u (second-mag path) RA=%p\n",
                    eq, which, _ReturnAddress());
        }
#endif
        g_OrigHideMagazine(self, handle, which);
    }

    static ClipSetFn_t g_OrigSetMotionByPathReal = nullptr;
    static void* g_SetMotionByPathRealAddr = nullptr;

    static unsigned char __fastcall hkSetMotionByPathSafe(void* ctl, unsigned int slot,
                                                          unsigned long long clip)
    {
        void* mdlArr = ReadPtrAtSEH(ctl, 0x58);
        void* model = mdlArr ? ReadPtrAtSEH(mdlArr, static_cast<size_t>(slot) * 8) : nullptr;
        if (!model)
            return 0;
        __try
        {
            return g_OrigSetMotionByPathReal(ctl, slot, clip);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    using PartsVoidSetterFn_t =
        void(__fastcall*)(void*, unsigned int, unsigned long long, unsigned long long);

    static bool PartsSlotModelNullSEH(void* self, unsigned int index)
    {
        void* mdlArr = ReadPtrAtSEH(self, 0x58);
        void* model = mdlArr ? ReadPtrAtSEH(mdlArr, static_cast<size_t>(index) * 8) : nullptr;
        return model == nullptr;
    }

    static PartsVoidSetterFn_t g_OrigPartsThermo = nullptr;   // vtbl+0x1a0 SetThermoGraphy   0x140ADDDB0
    static PartsVoidSetterFn_t g_OrigPartsB0 = nullptr;       // vtbl+0xb0                    0x140ADE4D0
    static PartsVoidSetterFn_t g_OrigPartsVisMesh = nullptr;  // vtbl+0x100 SetVisibilityMesh 0x140ADE5C0

    static void __fastcall hkPartsThermoSafe(void* self, unsigned int index,
                                             unsigned long long a2, unsigned long long a3)
    {
        if (PartsSlotModelNullSEH(self, index))
            return;
        __try { g_OrigPartsThermo(self, index, a2, a3); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall hkPartsB0Safe(void* self, unsigned int index,
                                         unsigned long long a2, unsigned long long a3)
    {
        if (PartsSlotModelNullSEH(self, index))
            return;
        __try { g_OrigPartsB0(self, index, a2, a3); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall hkPartsVisMeshSafe(void* self, unsigned int index,
                                              unsigned long long a2, unsigned long long a3)
    {
        if (PartsSlotModelNullSEH(self, index))
            return;
        __try { g_OrigPartsVisMesh(self, index, a2, a3); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    struct PartsSlotGuardDef
    {
        uintptr_t en154Addr;
        void* hook;
        void** orig;
        const char* label;
    };

    static void* g_PartsSlotGuardAddrs[3] = { nullptr, nullptr, nullptr };

    static void InstallPartsSlotModelGuards()
    {
        if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
            return;
        const PartsSlotGuardDef defs[3] = {
            { 0x140adddb0ull, reinterpret_cast<void*>(&hkPartsThermoSafe),
              reinterpret_cast<void**>(&g_OrigPartsThermo),  "SetThermoGraphy(vtbl+0x1a0)" },
            { 0x140ade4d0ull, reinterpret_cast<void*>(&hkPartsB0Safe),
              reinterpret_cast<void**>(&g_OrigPartsB0),      "partsSetter(vtbl+0xb0)" },
            { 0x140ade5c0ull, reinterpret_cast<void*>(&hkPartsVisMeshSafe),
              reinterpret_cast<void**>(&g_OrigPartsVisMesh), "SetVisibilityMesh(vtbl+0x100)" },
        };
        for (int i = 0; i < 3; ++i)
        {
            void* target = reinterpret_cast<void*>(ResolveGameAddress(defs[i].en154Addr));
            if (!target)
                continue;
            if (CreateAndEnableHook(target, defs[i].hook, defs[i].orig))
            {
                g_PartsSlotGuardAddrs[i] = target;
#ifdef _DEBUG
                LogDebug("[EquipParam] parts-slot model guard Install -> OK (%s target=%p; skips the "
                    "setter when model[slot] is null so a weapon with an unloaded part model "
                    "degrades instead of crashing)\n", defs[i].label, target);
#endif
            }
            else
                Log("[EquipParam] parts-slot model guard Install -> FAIL (%s target=%p)\n",
                    defs[i].label, target);
        }
    }

    static void RemovePartsSlotModelGuards()
    {
        for (int i = 0; i < 3; ++i)
        {
            if (g_PartsSlotGuardAddrs[i])
            {
                DisableAndRemoveHook(g_PartsSlotGuardAddrs[i]);
                g_PartsSlotGuardAddrs[i] = nullptr;
            }
        }
        g_OrigPartsThermo = nullptr;
        g_OrigPartsB0 = nullptr;
        g_OrigPartsVisMesh = nullptr;
    }

    static void ArmClipArchiveFallback()
    {
        if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
            return;
        void** vtbl = static_cast<void**>(
            reinterpret_cast<void*>(ResolveGameAddress(gAddr.SimplePartsControllerImpl_Vtable)));
        void* target = ResolveGameAddress(gAddr.SimplePartsControllerImpl_SetMotionDataByPath);
        if (!vtbl || !target)
            return;
        const int slot = 0x1b0 / 8;
        if (vtbl[slot] != target)
        {
            LogDebug("[WeaponKey] clip archive fallback NOT armed (vtbl slot content "
                "unexpected: %p)\n", vtbl[slot]);
            return;
        }
        DWORD prot = 0;
        if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &prot))
            return;
        g_OrigClipSetByPath = reinterpret_cast<ClipSetFn_t>(vtbl[slot]);
        vtbl[slot] = reinterpret_cast<void*>(&hkClipArchiveFallback);
        VirtualProtect(&vtbl[slot], sizeof(void*), prot, &prot);
        g_ClipFallbackVtbl = vtbl;
        g_ClipFallbackSlot = slot;
#ifdef _DEBUG
        LogDebug("[WeaponKey] clip archive fallback armed (SetMotionDataByPath vtbl+0x1B0; "
            "a clip missing from a control's bound motion archive is served from any "
            "registered custom family archive that contains it)\n");
#endif
    }

    static void DisarmClipArchiveFallback()
    {
        if (!g_ClipFallbackVtbl || g_ClipFallbackSlot < 0 || !g_OrigClipSetByPath)
            return;
        DWORD prot = 0;
        if (VirtualProtect(&g_ClipFallbackVtbl[g_ClipFallbackSlot], sizeof(void*),
                           PAGE_READWRITE, &prot))
        {
            g_ClipFallbackVtbl[g_ClipFallbackSlot] =
                reinterpret_cast<void*>(g_OrigClipSetByPath);
            VirtualProtect(&g_ClipFallbackVtbl[g_ClipFallbackSlot], sizeof(void*),
                           prot, &prot);
        }
        g_ClipFallbackVtbl = nullptr;
        g_ClipFallbackSlot = -1;
        g_OrigClipSetByPath = nullptr;
    }

    static const char* const kChimeraCatDirs[4] = {
        "receiver", "barrel", "magazine", "underBarrel" };
    static const char* const kPlayerPkgPrefixes[4] = {
        "/Assets/tpp/pack/player/motion/equip/receiver/pl_rcvr_",
        "/Assets/tpp/pack/player/motion/equip/barrel/pl_brrl_",
        "/Assets/tpp/pack/player/motion/equip/magazine/pl_mgzn_",
        "/Assets/tpp/pack/player/motion/equip/under_barrel/pl_ubrrl_" };

    static std::unordered_map<std::uint64_t, std::pair<int, std::string>>
        g_FamilyRootByPkg;
    static std::once_flag g_FamilyRootOnce;

    static void BuildFamilyRootTable()
    {
        g_FamilyRootByPkg.reserve(4 * 26 * 26 * 100);
        for (int c = 0; c < 4; ++c)
        {
            std::string p = std::string(kPlayerPkgPrefixes[c]) + "aa00.fpk";
            const size_t off = std::strlen(kPlayerPkgPrefixes[c]);
            for (char a = 'a'; a <= 'z'; ++a)
                for (char b = 'a'; b <= 'z'; ++b)
                    for (int d = 0; d < 100; ++d)
                    {
                        p[off] = a;
                        p[off + 1] = b;
                        p[off + 2] = static_cast<char>('0' + d / 10);
                        p[off + 3] = static_cast<char>('0' + d % 10);
                        g_FamilyRootByPkg.emplace(
                            FoxHashes::PathCode64Ext(p),
                            std::make_pair(c, p.substr(off, 4)));
                    }
        }
    }

    static void WarmFamilyRootTable()
    {
        std::thread([] { std::call_once(g_FamilyRootOnce, BuildFamilyRootTable); })
            .detach();
    }

    static void RecoverFamilyRoot(std::uint64_t pkg, int& cat, std::string& root)
    {
        std::call_once(g_FamilyRootOnce, BuildFamilyRootTable);
        cat = -1;
        root.clear();
        auto it = g_FamilyRootByPkg.find(pkg);
        if (it != g_FamilyRootByPkg.end())
        {
            cat = it->second.first;
            root = it->second.second;
        }
    }

    static std::set<std::uint64_t> g_ChimeraPackNames;

    static bool ScanChimeraPackArraySEH(void* base, std::uint64_t fpkHash,
                                        int* validCount)
    {
        const std::uint64_t want = fpkHash & 0x7FFFFFFFFFFFFFFFull;
        bool found = false;
        int valid = 0;
        __try
        {
            const std::uint64_t* p = static_cast<const std::uint64_t*>(base);
            for (int i = 0; i < 512 && !found; ++i, p += 8)
            {
                const std::uint64_t fpk = p[1] & 0x7FFFFFFFFFFFFFFFull;
                if ((fpk >> 56) != 0x52ull)
                    continue;
                ++valid;
                if (fpk == want)
                    found = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (validCount)
            *validCount = valid;
        return found;
    }

    using ReloadChimeraParts_t = int(__fastcall*)(lua_State*);
    static ReloadChimeraParts_t g_OrigReloadChimeraParts = nullptr;
    static void* g_ReloadChimeraPartsAddr = nullptr;

    static int __fastcall hkReloadChimeraPartsInfoTable(lua_State* L)
    {
        if (ResolveLuaApi() && g_lua_getfield && g_lua_rawgeti && g_lua_settop &&
            g_lua_objlen && g_lua_type && g_lua_tolstring)
        {
            std::set<std::uint64_t> names;
            g_lua_getfield(L, -1, const_cast<char*>("packageInfos"));
            if (g_lua_type(L, -1) == LUA_TTABLE)
            {
                const int rows = static_cast<int>(g_lua_objlen(L, -1));
                for (int i = 1; i <= rows; ++i)
                {
                    g_lua_rawgeti(L, -1, i);
                    if (g_lua_type(L, -1) == LUA_TTABLE)
                    {
                        g_lua_rawgeti(L, -1, 2);
                        if (g_lua_type(L, -1) == LUA_TSTRING)
                        {
                            const char* s = g_lua_tolstring(L, -1, nullptr);
                            if (s && *s)
                                names.insert(FoxHashes::PathCode64Ext(s));
                        }
                        g_lua_settop(L, -2);
                    }
                    g_lua_settop(L, -2);
                }
            }
            g_lua_settop(L, -2);
            if (!names.empty())
            {
                std::size_t count = names.size();
                {
                    std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                    g_ChimeraPackNames.swap(names);
                }
#ifdef _DEBUG
                static std::atomic<bool> logged{ false };
                if (!logged.exchange(true))
                    LogDebug("[WeaponKey] chimera package table captured at parse: %zu "
                        "pack names (the game's own list of every mountable chimera "
                        "part pack - the family mount gate compares against these, "
                        "so only packs the chimera system itself would load are "
                        "ever appended to a loadout)\n",
                        count);
#else
                (void)count;
#endif
            }
        }
        return g_OrigReloadChimeraParts ? g_OrigReloadChimeraParts(L) : 0;
    }

    static bool ChimeraPackTableContains(std::uint64_t fpkHash, bool* tableValid)
    {
        if (tableValid)
            *tableValid = false;
        if (!fpkHash || !::AddressSetRuntime::IsEn154Family(gGameBuild))
            return false;
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            if (!g_ChimeraPackNames.empty())
            {
                if (tableValid)
                    *tableValid = true;
                return g_ChimeraPackNames.count(fpkHash) != 0;
            }
        }
        void* base = ReadPtrAtSEH(
            reinterpret_cast<void*>(ResolveGameAddress(gAddr.Equip_ChimeraPartsPackageInfos)), 0);
#ifdef _DEBUG
        {
            static std::atomic<int> dumped{ 0 };
            if (dumped.fetch_add(1) < 3)
            {
                std::uint64_t e[6] = {};
                for (int k = 0; k < 6; ++k)
                    e[k] = reinterpret_cast<std::uint64_t>(
                        ReadPtrAtSEH(base, (k / 2) * 0x40 + (k % 2) * 8));
                LogDebug("[WeaponKey] chimera pack table dump: base=%p e0={%016llX,"
                    "%016llX} e1={%016llX,%016llX} e2={%016llX,%016llX} probe="
                    "%016llX\n",
                    base, e[0], e[1], e[2], e[3], e[4], e[5], fpkHash);
            }
        }
#endif
        if (!base)
            return false;
        int valid = 0;
        const bool found = ScanChimeraPackArraySEH(base, fpkHash, &valid);
        if (tableValid)
            *tableValid = valid > 0;
        return found;
    }

    static std::uint64_t FindFamilyChimeraPack(int cat, const std::string& root)
    {
        static std::mutex mx;
        static std::map<std::string, std::uint64_t> cache;
        const std::string key = std::string(kChimeraCatDirs[cat]) + "/" + root;
        {
            std::lock_guard<std::mutex> lock(mx);
            auto it = cache.find(key);
            if (it != cache.end())
                return it->second;
        }
        static const char* const kMidByCat[4][2] = {
            { "_main", nullptr },
            { "_main", "_barl" },
            { "_main", "_ammo" },
            { "_main", nullptr } };
        static const char* const kTails[2] = { "_def_v00.fpk", "_def.fpk" };
        const std::string base =
            std::string("/Assets/tpp/pack/collectible/chimera/")
            + kChimeraCatDirs[cat] + "/" + root;
        std::uint64_t found = 0;
        bool anyValid = false;
        for (int m = 0; m < 2 && !found; ++m)
        {
            if (!kMidByCat[cat][m])
                continue;
            for (int v = 0; v < 10 && !found; ++v)
                for (int t = 0; t < 2 && !found; ++t)
                {
                    const std::uint64_t h = FoxHashes::PathCode64Ext(
                        base + kMidByCat[cat][m]
                        + static_cast<char>('0' + v) + kTails[t]);
                    bool tableValid = false;
                    if (ChimeraPackTableContains(h, &tableValid))
                        found = h;
                    anyValid = anyValid || tableValid;
                }
        }
        if (found || anyValid)
        {
            std::lock_guard<std::mutex> lock(mx);
            cache[key] = found;
        }
        return found;
    }

    static std::uint64_t RegisterFamilyArchives(const std::string& root,
                                                std::uint32_t equipId)
    {
        static const char* const kSuffixes[] = {
            "_default", "_under", "_under_1", "_under_2", "_under_3", "_under_4",
            "_under_5", "_fullauto" };
        std::uint64_t defEntry = 0;
        for (const char* s : kSuffixes)
        {
            const std::uint64_t h = FoxHashes::PathCode64Ext(
                "/Assets/tpp/motion/mtar/equip/chimera/receiver/" + root + s
                + ".mtar");
            if (!h)
                continue;
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                g_RemapEntryEq[h] = equipId;
            }
            if (s == kSuffixes[0])
                defEntry = h;
        }
        return defEntry;
    }

    static std::uint64_t GetNativeRowPackHash(std::uint32_t equipId)
    {
        const std::int32_t idx =
            EquipIdCompression::ComputeCompressed(static_cast<std::int32_t>(equipId));
        if (!EquipIdCompression::IsCompressedInBounds(idx))
            return 0;
        auto* infoList = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_InfoList));
        if (!infoList)
            return 0;
        return reinterpret_cast<std::uint64_t>(
            ReadPtrAtSEH(infoList, static_cast<size_t>(idx) * 0x18 + 8));
    }

    struct FoxPathArray
    {
        std::uint32_t Count;
        std::uint32_t Capacity;
        std::uint64_t* Data;
    };

    using FoxArrayExtend_t =
        bool(__fastcall*)(void*, std::uint32_t, std::uint32_t, void*);

    static bool AppendPackPath(void* arr_, std::uint64_t h)
    {
        if (!arr_ || !h)
            return false;
        if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
            return false;
        auto* arr = static_cast<FoxPathArray*>(arr_);
        __try
        {
            for (std::uint32_t i = 0; i < arr->Count; ++i)
                if (arr->Data[i] == h)
                    return true;
            if (arr->Count >= arr->Capacity)
            {
                struct
                {
                    void** data;
                    std::uint32_t* count;
                    std::uint32_t* cap;
                    std::uint64_t elemSize;
                    std::uint64_t align;
                } info = { reinterpret_cast<void**>(&arr->Data), &arr->Count,
                           &arr->Capacity, 8, 8 };
                std::uint64_t opObj = reinterpret_cast<std::uint64_t>(
                    ResolveGameAddress(gAddr.Fox_ArrayOperatorVtbl));
                auto extend = reinterpret_cast<FoxArrayExtend_t>(
                    ResolveGameAddress(gAddr.Fox_ArrayBaseExtend));
                if (!extend || !opObj)
                    return false;
                const std::uint32_t newCap = arr->Count ? arr->Count * 2 : 16;
                if (!extend(&info, newCap, 0xd0001u, &opObj))
                    return false;
            }
            arr->Data[arr->Count] = h;
            arr->Count += 1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void RegisterCustomFamilyFallbacks()
    {
        static std::set<std::uint32_t> s_done;
        for (std::uint32_t eq = 1; eq < 0x450; ++eq)
        {
            if (eq >= 0x7D && eq < 0x36D)
                continue;
            if (TppEquip_GetSubIdForEquipId(static_cast<int>(eq)) == 0)
                continue;
            const std::uint32_t t = GetEquipTypeForEquipId(eq) & 0x1F;
            if (t < 1 || t > 8)
                continue;
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                if (!s_done.insert(eq).second)
                    continue;
            }
            FamilyGb cus;
            if (!GetGbForEquipId(eq, cus))
                continue;
            int rc = cus.gb[0];
            if (rc >= 234)
            {
                const auto itd = g_ReceiverMotionDonor.find(rc);
                rc = (itd != g_ReceiverMotionDonor.end()) ? itd->second : 0;
            }
            if (rc <= 0 || rc >= 234)
                continue;
            std::string root;
            if (!ResolveDonorSoundRoot(rc, root) || root.empty())
                continue;

            RegisterFamilyArchives(root, eq);
            const std::uint64_t ph = FindFamilyChimeraPack(0, root);
            if (ph)
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                g_MotionResidency[eq].familyPack = ph;
            }
            else
                Log("[WeaponKey] WARNING: custom eq=%u animation family '%s' has "
                    "no receiver pack in the game's chimera package table - its "
                    "archives are registered as clip-fallback candidates but "
                    "nothing mounts them, so a clip this weapon's own archive "
                    "lacks (typically the reload) stays refused and the parts "
                    "freeze for that action\n",
                    eq, root.c_str());
        }
    }

    static void FillCustomMotionEntriesTable(void* table)
    {
        if (!table)
            return;
        for (std::uint32_t eq = 1; eq < 0x7D; ++eq)
        {
            const int subId = TppEquip_GetSubIdForEquipId(static_cast<int>(eq));
            if (subId == 0)
                continue;
            const std::uint32_t t = GetEquipTypeForEquipId(eq) & 0x1F;
            if (t < 1 || t > 8)
                continue;
            if (ReadMotionEntry(table, eq))
                continue;
            std::uint32_t donor = 0;
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                auto it = g_FamilyFrom.find(eq);
                if (it != g_FamilyFrom.end())
                    donor = it->second;
            }
            if (MotionEntryIndex(donor) < 0 && subId < 514)
            {
                for (std::uint32_t v = 1; v != 0; v = NextDonorCandidate(v))
                {
                    if (v == eq)
                        continue;
                    const std::uint32_t vt = GetEquipTypeForEquipId(v) & 0x1F;
                    if (vt < 1 || vt > 8)
                        continue;
                    if (GetNativeSubId(v) == static_cast<std::uint32_t>(subId) &&
                        TppEquip_GetSubIdForEquipId(static_cast<int>(v)) == 0 &&
                        ReadMotionEntry(table, v))
                    {
                        donor = v;
                        break;
                    }
                }
            }
            if (MotionEntryIndex(donor) < 0)
            {
                FamilyGb cus;
                if (GetGbForEquipId(eq, cus))
                {
                    int rc = cus.gb[0];
                    if (rc == 0)
                    {
                        // The receiver lane is bound late, so the row byte is 0
                        // until the gunInfo build. Derive the animation family
                        // from the declared WIDE receiver's motionFrom donor,
                        // which SetReceiver records regardless of any lane.
                        const int sub =
                            TppEquip_GetSubIdForEquipId(static_cast<int>(eq));
                        const int logical = (sub > 0)
                            ? GunBasic_GetLogicalPart(sub, kVanillaSpace_Receiver)
                            : 0;
                        if (logical >= kWideIdBase)
                        {
                            WidePartState* w =
                                WideStateFor(kVanillaSpace_Receiver, logical);
                            if (w && w->motionFrom > 0)
                                rc = w->motionFrom;
                        }
                    }
                    if (rc >= 234)
                    {
                        const auto itd = g_ReceiverMotionDonor.find(rc);
                        rc = (itd != g_ReceiverMotionDonor.end()) ? itd->second : 0;
                    }
                    if (rc > 0 && rc < 234)
                    {
                        EnsureRcvTypeExt();
                        const int ctype =
                            g_RcvTypeExtReady ? g_RcvTypeExt[rc] : -1;
                        std::string cusRoot;
                        ResolveDonorSoundRoot(rc, cusRoot);
                        std::uint32_t rootDonor = 0, typeDonor = 0;
                        for (std::uint32_t v = 1; v != 0; v = NextDonorCandidate(v))
                        {
                            if (v == eq)
                                continue;
                            const std::uint32_t vt = GetEquipTypeForEquipId(v) & 0x1F;
                            if (vt < 1 || vt > 8)
                                continue;
                            if (TppEquip_GetSubIdForEquipId(static_cast<int>(v)) != 0)
                                continue;
                            FamilyGb van;
                            if (!GetGbForEquipId(v, van))
                                continue;
                            if (!ReadMotionEntry(table, v))
                                continue;
                            if (van.gb[0] == static_cast<unsigned char>(rc))
                            {
                                donor = v;
                                break;
                            }
                            if (rootDonor == 0 && !cusRoot.empty() && van.gb[0] < 234)
                            {
                                std::string vroot;
                                if (ResolveDonorSoundRoot(van.gb[0], vroot) &&
                                    vroot == cusRoot)
                                    rootDonor = v;
                            }
                            if (typeDonor == 0 && ctype > 0 &&
                                g_RcvTypeExt[van.gb[0]] == ctype)
                                typeDonor = v;
                        }
                        if (MotionEntryIndex(donor) < 0 && rootDonor != 0)
                            donor = rootDonor;
                        if (MotionEntryIndex(donor) < 0 && typeDonor != 0)
                            donor = typeDonor;
                    }
                }
            }
            void* donorEntry = ReadMotionEntry(table, donor);
            if (!donorEntry)
            {
                static std::set<std::uint32_t> noDonorLogged;
                bool first = false;
                {
                    std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                    first = noDonorLogged.insert(eq).second;
                }
                FamilyGb cus;
                int rcRaw = -1, rcRes = -1, ctype = -1;
                std::string cusRoot;
                if (GetGbForEquipId(eq, cus))
                {
                    rcRaw = cus.gb[0];
                    rcRes = rcRaw;
                    if (rcRes >= 234)
                    {
                        const auto itd = g_ReceiverMotionDonor.find(rcRes);
                        rcRes = (itd != g_ReceiverMotionDonor.end()) ? itd->second : -1;
                    }
                    EnsureRcvTypeExt();
                    if (g_RcvTypeExtReady && rcRes >= 0 && rcRes < kPartTypeExtCount)
                        ctype = g_RcvTypeExt[rcRes];
                    if (rcRes > 0 && rcRes < 234)
                        ResolveDonorSoundRoot(rcRes, cusRoot);
                }
                if (!cusRoot.empty())
                {
                    static int hasherValidated = 0;
                    if (hasherValidated == 0)
                    {
                        void* known = ReadMotionEntry(table, 27);
                        const std::uint64_t check = FoxHashes::PathCode64Ext(
                            "/Assets/tpp/motion/mtar/equip/chimera/assemble/ar00_asm.mtar");
                        hasherValidated =
                            (!known || check == reinterpret_cast<std::uint64_t>(known))
                                ? 1 : -1;
                        if (hasherValidated < 0)
                            LogDebug("[WeaponKey] MotionEntry synth disabled: engine path hash "
                                "%016llX does not match the known ar00 entry %p\n",
                                check, known);
                    }
                    if (hasherValidated > 0)
                    {
                        const std::string paths[2] = {
                            "/Assets/tpp/motion/mtar/equip/chimera/receiver/"
                                + cusRoot + "_default.mtar",
                            "/Assets/tpp/motion/mtar/equip/chimera/assemble/"
                                + cusRoot + "_asm.mtar"
                        };
                        for (const std::string& p : paths)
                        {
                            const std::uint64_t h = FoxHashes::PathCode64Ext(p);
                            if (!h)
                                continue;
                            if (WritePtrAtSEH(table, 8 + static_cast<size_t>(eq) * 8,
                                              reinterpret_cast<void*>(h)))
                            {
                                {
                                    std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                                    g_RemapEntryEq[h] = eq;
                                }
                                if (&p == &paths[0])
                                {
                                    const std::uint64_t ph =
                                        FindFamilyChimeraPack(0, cusRoot);
                                    if (ph)
                                    {
                                        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                                        g_MotionResidency[eq].familyPack = ph;
                                    }
                                    else if (first)
                                        LogDebug("[WeaponKey] MotionEntry synth eq=%u: family "
                                            "'%s' has no receiver pack in the game's "
                                            "chimera package table - the entry is written "
                                            "but nothing mounts its mtar; clips will stay "
                                            "refused unless another pack carries it\n",
                                            eq, cusRoot.c_str());
                                }
                                if (first)
                                    LogDebug("[WeaponKey] MotionEntry synthesized: custom eq=%u "
                                        "family='%s' -> %s (no vanilla weapon registers this "
                                        "animation family; entry hashed with the engine's own "
                                        "path hasher, mounted via the pack the game's chimera "
                                        "package table names for the family)\n",
                                        eq, cusRoot.c_str(), p.c_str());
                            }
                            break;
                        }
                        if (ReadMotionEntry(table, eq))
                            continue;
                    }
                }
                if (first)
                {
                    LogDebug("[WeaponKey] MotionEntry NO DONOR for custom eq=%u (subId=%d rc=%d "
                        "resolved=%d motionType=%d family='%s') - no vanilla weapon shares "
                        "this receiver family; register SetWeaponHandling{...familyFrom="
                        "<vanilla equipId>} or V_TppEquip.SetAssembleMotion{equipId=..., "
                        "copyFrom=...} to pick its animation clip set\n",
                        eq, subId, rcRaw, rcRes, ctype,
                        cusRoot.empty() ? "?" : cusRoot.c_str());
#ifdef _DEBUG
                    static std::atomic<bool> dumped{ false };
                    if (!dumped.exchange(true))
                    {
                        EnsureRcvTypeExt();
                        for (std::uint32_t v = 1; v != 0; v = NextDonorCandidate(v))
                        {
                            if (!ReadMotionEntry(table, v))
                                continue;
                            FamilyGb van;
                            const bool gbOk = GetGbForEquipId(v, van);
                            std::string vroot;
                            if (gbOk && van.gb[0] < 234)
                                ResolveDonorSoundRoot(van.gb[0], vroot);
                            LogDebug("[WeaponKey]   donor candidate eq=%u subId=%d rc=%d "
                                "type=%d root='%s' entry=%p\n",
                                v, gbOk ? van.subId : -1, gbOk ? van.gb[0] : -1,
                                (gbOk && g_RcvTypeExtReady) ? g_RcvTypeExt[van.gb[0]] : -1,
                                vroot.empty() ? "?" : vroot.c_str(),
                                ReadMotionEntry(table, v));
                        }
                    }
#endif
                }
                continue;
            }
            if (WritePtrAtSEH(table, 8 + static_cast<size_t>(eq) * 8, donorEntry))
            {
                {
                    std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                    auto& plan = g_MotionResidency[eq];
                    plan.donorEq = donor;
                    plan.donorPack = GetNativeRowPackHash(donor);
                    g_RemapEntryEq[reinterpret_cast<std::uint64_t>(donorEntry)] = eq;
                }
                LogDebug("[WeaponKey] MotionEntry alias: custom eq=%u -> donor eq=%u entry=%p "
                    "(the per-equipId weapon-motion mtar entry was empty; without it Realize "
                    "creates NO anim control - no clip-driven bolt/parts animation and the "
                    "default-pose stomp. Aliasing the donor's entry makes Realize build the "
                    "real SimpleControl, the same path vanilla weapons use)\n",
                    eq, donor, donorEntry);
            }
        }
    }

    static void FillCustomMotionEntriesEarly()
    {
        if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
            return;
        RegisterCustomFamilyFallbacks();
        FillCustomMotionEntriesTable(
            reinterpret_cast<void*>(ResolveGameAddress(gAddr.Equip_MotionEntryTable)));
    }

    static void FillCustomMotionEntries(void* self)
    {
        FillCustomMotionEntriesTable(ReadPtrAtSEH(self, 0x70));
    }

    static void* GetEquipMotionLoaderIface()
    {
        if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
            return nullptr;
        using QST_t = std::uint8_t*(__fastcall*)();
        auto qst = reinterpret_cast<QST_t>(ResolveGameAddress(gAddr.Fox_GetQuarkSystemTable));
        if (!qst)
            return nullptr;
        __try
        {
            std::uint8_t* t = qst();
            if (!t)
                return nullptr;
            std::uint8_t* app = *reinterpret_cast<std::uint8_t**>(t + 0x98);
            if (!app)
                return nullptr;
            std::uint8_t* q = *reinterpret_cast<std::uint8_t**>(app + 0x1e8);
            if (!q)
                return nullptr;
            return *reinterpret_cast<void**>(q + 0x18);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static std::uint32_t CallMtarIdsSEH(void* fn, void* self, std::uint8_t* out,
                                        std::uint32_t* eq)
    {
        if (!fn)
            return 0;
        __try
        {
            return reinterpret_cast<
                std::uint32_t(__fastcall*)(void*, std::uint8_t*, std::uint32_t*)>(fn)(
                self, out, eq);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static std::uint64_t CallPathByIdSEH(void* fn, void* self, std::uint32_t id)
    {
        if (!fn)
            return 0;
        __try
        {
            std::uint64_t out[2] = {};
            auto* p = reinterpret_cast<
                std::uint64_t*(__fastcall*)(void*, std::uint64_t*, std::uint32_t)>(fn)(
                self, out, id);
            return p ? *p : out[0];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static bool CallMtarPathsSEH(void* fn, void* self, std::uint64_t* out,
                                 std::uint8_t* ids, std::uint32_t count)
    {
        if (!fn)
            return false;
        __try
        {
            reinterpret_cast<void(__fastcall*)(void*, std::uint64_t*, std::uint8_t*,
                                               std::uint32_t)>(fn)(self, out, ids,
                                                                   count);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    using AddMtarBlockPackagePaths_t = void(__fastcall*)(void*, std::uint32_t);
    static AddMtarBlockPackagePaths_t g_OrigAddMtarBlockPackagePaths = nullptr;
    static void* g_AddMtarBlockPackagePathsAddr = nullptr;

    static uintptr_t AddMtarBlockPackagePathsAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140a02290ull;
        default:                                          return 0;
        }
    }

    struct StallWatch
    {
        const char*   what;
        std::uint32_t eq;
        LARGE_INTEGER t0;

        StallWatch(const char* label, std::uint32_t equipId)
            : what(label), eq(equipId)
        {
            QueryPerformanceCounter(&t0);
        }

        ~StallWatch()
        {
            LARGE_INTEGER t1, f;
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            if (!f.QuadPart)
                return;
            const double ms =
                (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
            if (ms < 16.0)
                return;
            LogDebug("[WeaponKey] %s for eq=%u blocked the calling thread for %.0f ms "
                "- a weapon realize that lands mid-gameplay stutters by that "
                "much\n", what, eq, ms);
        }
    };

    static void __fastcall hkAddMtarBlockPackagePaths(void* arr, std::uint32_t equipId)
    {
        g_OrigAddMtarBlockPackagePaths(arr, equipId);
        if (!arr || equipId == 0 || equipId >= 0x800)
            return;
        StallWatch _stall("motion pack mount", equipId);
        if (TppEquip_GetSubIdForEquipId(static_cast<int>(equipId)) == 0)
            return;
        const std::uint32_t t = GetEquipTypeForEquipId(equipId) & 0x1F;
        if (t < 1 || t > 8)
            return;
        FillCustomMotionEntriesEarly();
        MotionResidencyPlan plan;
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            auto it = g_MotionResidency.find(equipId);
            if (it != g_MotionResidency.end())
                plan = it->second;
        }
        bool donorPackOk = false, familyPackOk = false;
#ifdef _DEBUG
        static std::set<std::uint32_t> probeLogged;
        bool firstProbe = false;
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            firstProbe = probeLogged.insert(equipId).second;
        }
        if (firstProbe)
        {
            void* mlDbg = GetEquipMotionLoaderIface();
            void* mlvDbg = mlDbg ? ReadPtrAtSEH(mlDbg, 0) : nullptr;
            if (mlvDbg)
            {
                std::uint8_t idsDbg[16] = {};
                std::uint32_t eqDbg = equipId;
                const std::uint32_t nDbg =
                    CallMtarIdsSEH(ReadPtrAtSEH(mlvDbg, 0x18), mlDbg, idsDbg, &eqDbg);
                std::uint64_t pathsDbg[16] = {};
                const bool gotDbg =
                    (nDbg > 0 && nDbg <= 16) &&
                    CallMtarPathsSEH(ReadPtrAtSEH(mlvDbg, 0x8), mlDbg, pathsDbg,
                                     idsDbg, nDbg);
                LogDebug("[WeaponKey] engine motion map eq=%u ids=%u%s\n", equipId, nDbg,
                    nDbg == 0 ? " (by-equipId lookup returned nothing)" : "");
                for (std::uint32_t i = 0; i < nDbg && i < 16; ++i)
                    LogDebug("[WeaponKey]   eq=%u id[%u]=%u mtar=%016llX pkg=%016llX\n",
                        equipId, i, idsDbg[i], gotDbg ? pathsDbg[i] : 0,
                        CallPathByIdSEH(ReadPtrAtSEH(mlvDbg, 0x10), mlDbg, idsDbg[i]));
            }
        }
#endif
        if (plan.donorEq)
        {
            g_OrigAddMtarBlockPackagePaths(arr, plan.donorEq);
            donorPackOk = AppendPackPath(arr, plan.donorPack);
        }
        std::uint64_t engineEntry = 0, engineFpk = 0;
        std::string engineRoot;
        std::string familyList;
        {
            void* ml = GetEquipMotionLoaderIface();
            void* mlv = ml ? ReadPtrAtSEH(ml, 0) : nullptr;
            if (mlv)
            {
                std::uint8_t ids[16] = {};
                std::uint32_t eqIn = equipId;
                const std::uint32_t n =
                    CallMtarIdsSEH(ReadPtrAtSEH(mlv, 0x18), ml, ids, &eqIn);
                std::set<std::string> seenRoots;
                for (std::uint32_t i = 0; i < n && i < 16; ++i)
                {
                    const std::uint64_t pkg =
                        CallPathByIdSEH(ReadPtrAtSEH(mlv, 0x10), ml, ids[i]);
                    if ((pkg >> 56) != 0x52)
                        continue;
                    int cat = -1;
                    std::string root;
                    RecoverFamilyRoot(pkg, cat, root);
                    if (root.empty() || cat < 0 || !seenRoots.insert(root).second)
                        continue;
                    const std::uint64_t fpk = FindFamilyChimeraPack(cat, root);
                    const bool fpkOk = fpk != 0;
                    if (fpkOk)
                        AppendPackPath(arr, fpk);
                    const std::uint64_t defEntry =
                        RegisterFamilyArchives(root, equipId);
                    if (cat == 0 && engineRoot.empty() && defEntry && fpkOk)
                    {
                        engineRoot = root;
                        engineEntry = defEntry;
                        engineFpk = fpk;
                    }
                    if (!familyList.empty())
                        familyList += " ";
                    familyList += kChimeraCatDirs[cat];
                    familyList += ":";
                    familyList += root;
                    if (!fpkOk)
                        familyList += "(no pack)";
                }
            }
        }
        if (engineEntry && !plan.donorEq)
        {
            const int idx = MotionEntryIndex(equipId);
            void* table =
                reinterpret_cast<void*>(ResolveGameAddress(gAddr.Equip_MotionEntryTable));
            if (idx >= 0 && table)
                WritePtrAtSEH(table, 8 + static_cast<size_t>(idx) * 8,
                              reinterpret_cast<void*>(engineEntry));
        }
        if (!engineEntry && plan.familyPack)
            familyPackOk = AppendPackPath(arr, plan.familyPack);
        if (!plan.donorEq && !plan.familyPack && !engineEntry &&
            familyList.empty())
            return;
#ifdef _DEBUG
        static std::set<std::uint32_t> mountLogged;
        bool firstLog = false;
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            firstLog = mountLogged.insert(equipId).second;
        }
        if (firstLog)
        {
            if (plan.donorEq)
                Log("[WeaponKey] motion pack mount: custom eq=%u -> donor eq=%u "
                    "(donor family packages + donor EQP pack %016llX%s) + families "
                    "[%s] appended to the engine's own loadout package list - every "
                    "part family the engine maps for this weapon gets its chimera "
                    "archive mounted, so each part's clips are resident regardless of "
                    "which family they come from\n",
                    equipId, plan.donorEq, plan.donorPack,
                    plan.donorPack ? (donorPackOk ? " OK" : " FAIL") : "",
                    familyList.empty() ? "-" : familyList.c_str());
            else if (engineEntry)
                LogDebug("[WeaponKey] motion pack mount: custom eq=%u -> receiver family "
                    "'%s' (engine motion map): entry = receiver/%s_default.mtar "
                    "%016llX via collectible/chimera fpk %016llX + families [%s] "
                    "mounted (the engine derives this weapon's clip NAMES from its "
                    "receiver row, so the receiver family's default mtar is the one "
                    "file guaranteed to contain them)\n",
                    equipId, engineRoot.c_str(), engineRoot.c_str(), engineEntry,
                    engineFpk, familyList.empty() ? "-" : familyList.c_str());
            else
                Log("[WeaponKey] motion pack mount: custom eq=%u families [%s] "
                    "family pack %016llX%s appended to the engine's own loadout "
                    "package list\n",
                    equipId, familyList.empty() ? "-" : familyList.c_str(),
                    plan.familyPack,
                    plan.familyPack ? (familyPackOk ? " OK" : " FAIL") : "");
        }
#else
        (void)donorPackOk;
        (void)familyPackOk;
        (void)engineFpk;
#endif
    }

#ifdef _DEBUG
    using BoltBoneFn_t = unsigned long long(__fastcall*)(
        void*, unsigned long long, unsigned long long, unsigned long long);
    static BoltBoneFn_t g_OrigBoltBoneIdx = nullptr;
    static BoltBoneFn_t g_OrigBoltBoneWrite = nullptr;
    static void** g_BoltBoneVtbl = nullptr;
    static std::mutex g_BoltBoneMx;

    struct CtlInfo
    {
        unsigned eq = 0;
        void* iface = nullptr;
    };
    static std::map<void*, CtlInfo> g_CtlToEq;
    static std::map<unsigned long long, unsigned> g_CtlLastHash;

    using ChimSet_t = unsigned long long(__fastcall*)(
        void*, unsigned long long, unsigned long long, unsigned long long,
        unsigned long long, unsigned long long);
    static BoltBoneFn_t g_OrigChimBoneIdx = nullptr;
    static ChimSet_t g_OrigChimSet = nullptr;
    static void** g_ChimVtbl = nullptr;

    struct BoltWriteSample
    {
        void* addr = nullptr;
        float x = 0, y = 0, z = 0;
        unsigned eq = 0;
        int checks = 0;
    };
    static std::map<void*, BoltWriteSample> g_BoltWriteSamples;

    struct BoltModelSample
    {
        unsigned long long boneIdx = 0;
        unsigned eq = 0;
        void* writerCtl = nullptr;
        unsigned long long lastWriteTick = 0;
        bool haveRest = false;
        unsigned char mtx[0x40] = {};
        unsigned char rest[0x40] = {};
    };
    static std::map<void*, BoltModelSample> g_BoltModelSamples;

    static bool CopyBoltBytesSEH(void* dst, const void* src, size_t n)
    {
        __try
        {
            memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ReadF32x3AtSEH(void* addr, float* out3)
    {
        __try
        {
            memcpy(out3, addr, 12);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static CtlInfo LookupCtlInfo(void* ctl)
    {
        std::lock_guard<std::mutex> lock(g_BoltBoneMx);
        auto it = g_CtlToEq.find(ctl);
        return it == g_CtlToEq.end() ? CtlInfo{} : it->second;
    }

    static void* BoltPoseSlotAddr(void* ctl, unsigned long long slot, unsigned long long boneIdx)
    {
        void* models = ReadPtrAtSEH(ctl, 0x58);
        if (!models)
            return nullptr;
        void* model = ReadPtrAtSEH(models, static_cast<size_t>(slot) * 8);
        if (!model)
            return nullptr;
        void* pose = ReadPtrAtSEH(model, 0xE0);
        if (!pose)
            return nullptr;
        return static_cast<std::uint8_t*>(pose) + boneIdx * 0x40;
    }

    static unsigned long long __fastcall hkBoltBoneIdx(
        void* ctl, unsigned long long slot, unsigned long long c, unsigned long long d)
    {
        const unsigned long long r = g_OrigBoltBoneIdx(ctl, slot, c, d);
        const CtlInfo info = LookupCtlInfo(ctl);
        {
            std::lock_guard<std::mutex> lock(g_BoltBoneMx);
            g_CtlLastHash[reinterpret_cast<unsigned long long>(ctl) + slot] =
                static_cast<unsigned>(c & 0xffffffffull);
        }
        static std::mutex mx;
        static std::map<unsigned long long, int> cnt;
        std::lock_guard<std::mutex> lock(mx);

        const unsigned long long key = (static_cast<unsigned long long>(info.eq) << 40) |
                                       ((slot & 0xFF) << 32) | (c & 0xffffffffull);
        int& n = cnt[key];
        ++n;
        if (n == 1)
        {
            void* models = ReadPtrAtSEH(ctl, 0x58);
            void* model = models ? ReadPtrAtSEH(models, static_cast<size_t>(slot) * 8) : nullptr;
            LogDebug("[WeaponKey] BoltBone GET-IDX ctl=%p slot=%llu hash=%08llX model=%p -> %lld eq=%u%s\n",
                ctl, slot, c & 0xffffffffull, model,
                static_cast<long long>(static_cast<int>(r)), info.eq,
                (static_cast<int>(r) < 0)
                    ? "  <<< NO BONE BOUND (clip references this bone but the custom model has no "
                      "bone by that name - if this is the cylinder, that is why it does not rotate)"
                    : "");
        }
        return r;
    }

    static unsigned long long __fastcall hkBoltBoneWrite(
        void* ctl, unsigned long long slot, unsigned long long boneIdx, unsigned long long mtx)
    {
        const CtlInfo info = LookupCtlInfo(ctl);
        const unsigned long long r = g_OrigBoltBoneWrite(ctl, slot, boneIdx, mtx);
        void* addr = BoltPoseSlotAddr(ctl, slot, boneIdx);
        float now[3] = { 0, 0, 0 };
        const bool haveNow = addr && ReadF32x3AtSEH(static_cast<std::uint8_t*>(addr) + 0x30, now);
        {
            static std::mutex mx;
            static std::map<unsigned long long, int> cnt;
            std::lock_guard<std::mutex> lock(mx);
            int& n = cnt[(static_cast<unsigned long long>(info.eq) << 8) | (slot & 0xFF)];
            ++n;
            if (n <= 10)
                LogDebug("[WeaponKey] BoltBone WRITE ctl=%p slot=%llu boneIdx=%llu eq=%u pose=%p "
                    "pos=(%.5f %.5f %.5f) (matrix landed in the model's local pose buffer)\n",
                    ctl, slot, boneIdx, info.eq, addr,
                    haveNow ? now[0] : 0.0f, haveNow ? now[1] : 0.0f, haveNow ? now[2] : 0.0f);
        }

        if (info.eq == 96 && boneIdx == 2)
        {
            float mm[8] = { 0,0,0,0,0,0,0,0 };
            if (CopyBoltBytesSEH(mm, reinterpret_cast<const void*>(mtx), sizeof(mm)))
            {
                const double ang = atan2(static_cast<double>(mm[1]), static_cast<double>(mm[0])) *
                                   57.29577951308232;
                static std::mutex mxR;
                static int nr = 0;
                std::lock_guard<std::mutex> lock(mxR);
                if (nr < 40)
                {
                    ++nr;
                    LogDebug("[WeaponKey] CYL-ENGINE-ROT eq=96 bone2 engine-row0=(%.4f %.4f %.4f) "
                        "row1=(%.4f %.4f %.4f) Zang=%.1fdeg (sweeps => vanilla clip already spins the "
                        "cylinder; ~constant => only the DLL does)\n",
                        mm[0], mm[1], mm[2], mm[4], mm[5], mm[6], ang);
                }
            }
        }
        if (haveNow)
        {
            std::lock_guard<std::mutex> lock(g_BoltBoneMx);
            BoltWriteSample& s = g_BoltWriteSamples[ctl];
            s.addr = addr;
            s.x = now[0]; s.y = now[1]; s.z = now[2];
            s.eq = info.eq;
        }
        if (haveNow && info.eq && (info.eq - 0x367u) >= 6u &&
            TppEquip_GetSubIdForEquipId(static_cast<int>(info.eq)) != 0)
        {
            void* models = ReadPtrAtSEH(ctl, 0x58);
            void* model = models ? ReadPtrAtSEH(models, static_cast<size_t>(slot) * 8) : nullptr;
            if (model)
            {
                unsigned char m[0x40];
                if (CopyBoltBytesSEH(m, reinterpret_cast<const void*>(mtx), 0x40))
                {
                    std::lock_guard<std::mutex> lock(g_BoltBoneMx);
                    BoltModelSample& s = g_BoltModelSamples[model];
                    s.boneIdx = boneIdx;
                    s.eq = info.eq;
                    s.writerCtl = ctl;
                    s.lastWriteTick = GetTickCount64();
                    memcpy(s.mtx, m, 0x40);
                }
            }
        }
        if (info.iface && info.eq && (info.eq - 0x367u) >= 6u &&
            g_OrigChimBoneIdx && g_OrigChimSet &&
            TppEquip_GetSubIdForEquipId(static_cast<int>(info.eq)) != 0)
        {
            unsigned hash = 0;
            {
                std::lock_guard<std::mutex> lock(g_BoltBoneMx);
                auto it = g_CtlLastHash.find(
                    reinterpret_cast<unsigned long long>(ctl) + slot);
                if (it != g_CtlLastHash.end())
                    hash = it->second;
            }
            if (hash)
            {
                for (unsigned long long ch = 0; ch < 6; ++ch)
                {
                    const int cidx = static_cast<int>(
                        g_OrigChimBoneIdx(info.iface, ch, 0, hash));
                    if (cidx < 0)
                        continue;
                    g_OrigChimSet(info.iface, ch, 0,
                                  static_cast<unsigned long long>(cidx), mtx, 0);
                    static std::mutex mxB;
                    static std::map<unsigned long long, int> cntB;
                    std::lock_guard<std::mutex> lock(mxB);
                    int& n = cntB[(static_cast<unsigned long long>(info.eq) << 8) | ch];
                    ++n;
                    if (n <= 12)
                        LogDebug("[WeaponKey] BoltBridge eq=%u ch=%llu chimIdx=%d "
                            "(mirrored the engine-computed bolt matrix into the player-rig "
                            "chimera model - if the bolt now MOVES on screen, the rendered "
                            "in-hand model is the chimera instance and this bridge is the fix)\n",
                            info.eq, ch, cidx);
                }
            }
        }
        return r;
    }

    static void CheckBoltWritePersist(void* ctl)
    {
        BoltWriteSample snap;
        {
            std::lock_guard<std::mutex> lock(g_BoltBoneMx);
            auto it = g_BoltWriteSamples.find(ctl);
            if (it == g_BoltWriteSamples.end() || !it->second.addr || it->second.checks >= 6)
                return;
            ++it->second.checks;
            snap = it->second;
        }
        float now[3] = { 0, 0, 0 };
        if (!ReadF32x3AtSEH(static_cast<std::uint8_t*>(snap.addr) + 0x30, now))
            return;
        const bool same = (now[0] == snap.x) && (now[1] == snap.y) && (now[2] == snap.z);
        LogDebug("[WeaponKey] BoltBone PERSIST eq=%u ctl=%p addr=%p wrote=(%.5f %.5f %.5f) "
            "nextFrame=(%.5f %.5f %.5f) -> %s\n",
            snap.eq, ctl, snap.addr, snap.x, snap.y, snap.z, now[0], now[1], now[2],
            same ? "PERSISTED (nothing overwrote the pose - if the bolt still looks frozen, "
                   "the rendered weapon is a DIFFERENT model instance)"
                 : "STOMPED (something re-poses this model every frame - the procedural "
                   "write is being overwritten before render)");
    }

    static unsigned long long __fastcall hkChimBoneIdx(
        void* iface, unsigned long long ch, unsigned long long part, unsigned long long hash)
    {
        const unsigned long long r = g_OrigChimBoneIdx(iface, ch, part, hash);
        static std::mutex mx;
        static std::map<unsigned long long, int> cnt;
        std::lock_guard<std::mutex> lock(mx);
        int& n = cnt[(ch << 40) | ((part & 0xFF) << 32) | (hash & 0xffffffffull)];
        ++n;
        if (n <= 6)
            LogDebug("[WeaponKey] ChimBone GET-IDX ch=%llu part=%llu hash=%08llX -> %lld "
                "(the PLAYER-RIG bolt path - only equipIds 871-876 ever reach this)\n",
                ch, part & 0xFF, hash & 0xffffffffull,
                static_cast<long long>(static_cast<int>(r)));
        return r;
    }

    static unsigned long long __fastcall hkChimSet(
        void* iface, unsigned long long ch, unsigned long long part,
        unsigned long long boneIdx, unsigned long long mtx, unsigned long long spare)
    {
        {
            static std::mutex mx;
            static std::map<unsigned long long, int> cnt;
            std::lock_guard<std::mutex> lock(mx);
            int& n = cnt[(ch << 8) | (part & 0xFF)];
            ++n;
            if (n <= 10)
                LogDebug("[WeaponKey] ChimBone WRITE ch=%llu part=%llu boneIdx=%llu "
                    "(player-rig bolt matrix write)\n",
                    ch, part & 0xFF, boneIdx);
        }
        return g_OrigChimSet(iface, ch, part, boneIdx, mtx, spare);
    }

    static void TryArmChimeraHooks(void* obj)
    {
        if (g_ChimVtbl)
            return;
        void* iface = ReadPtrAtSEH(obj, 0x120);
        if (!iface)
            return;
        void** vtbl = static_cast<void**>(ReadPtrAtSEH(iface, 0));
        if (!vtbl)
            return;
        void* idxFn = ReadPtrAtSEH(vtbl, 0x170);
        void* setFn = ReadPtrAtSEH(vtbl, 0x188);
        if (!idxFn || !setFn)
            return;
        DWORD oldProt = 0;
        if (!VirtualProtect(&vtbl[0x170 / 8], sizeof(void*) * 4, PAGE_READWRITE, &oldProt))
            return;
        g_OrigChimBoneIdx = reinterpret_cast<BoltBoneFn_t>(idxFn);
        g_OrigChimSet = reinterpret_cast<ChimSet_t>(setFn);
        vtbl[0x170 / 8] = reinterpret_cast<void*>(&hkChimBoneIdx);
        vtbl[0x188 / 8] = reinterpret_cast<void*>(&hkChimSet);
        VirtualProtect(&vtbl[0x170 / 8], sizeof(void*) * 4, oldProt, &oldProt);
        g_ChimVtbl = vtbl;
        LogDebug("[WeaponKey] chimera bolt probes armed: iface=%p vtbl=%p GET-IDX(+0x170)=%p "
            "SET(+0x188)=%p (this is ChimeraSystemImpl - the path the 871-876 player builds "
            "use; comparing its live calls against the standalone controller shows what the "
            "custom weapon is missing)\n",
            iface, vtbl, idxFn, setFn);
    }

    static BoltBoneFn_t g_OrigPutMotion = nullptr;
    static BoltBoneFn_t g_OrigPutMotionAll = nullptr;

    static bool SlotHasAnimControl(void* ctl, unsigned long long slot)
    {
        void* arr = ReadPtrAtSEH(ctl, 0x60);
        return arr && ReadPtrAtSEH(arr, static_cast<size_t>(slot) * 8);
    }

    static void ReapplyBoltToModel(void* ctl, unsigned long long slot,
                                   const char* fromFn, void* caller)
    {
        if (SlotHasAnimControl(ctl, slot))
            return;
        void* models = ReadPtrAtSEH(ctl, 0x58);
        if (!models)
            return;
        void* model = ReadPtrAtSEH(models, static_cast<size_t>(slot) * 8);
        if (!model)
            return;
        BoltModelSample s;
        {
            std::lock_guard<std::mutex> lock(g_BoltBoneMx);
            auto it = g_BoltModelSamples.find(model);
            if (it == g_BoltModelSamples.end())
                return;
            if (GetTickCount64() - it->second.lastWriteTick > 600)
            {
                g_BoltModelSamples.erase(it);
                return;
            }
            s = it->second;
        }
        void* pose = ReadPtrAtSEH(model, 0xE0);
        if (!pose)
            return;
        void* dst = static_cast<std::uint8_t*>(pose) + s.boneIdx * 0x40;
        unsigned char restNow[0x40];
        const bool gotRest = CopyBoltBytesSEH(restNow, dst, 0x40);
        if (!CopyBoltBytesSEH(dst, s.mtx, 0x40))
            return;
        if (gotRest)
        {
            std::lock_guard<std::mutex> lock(g_BoltBoneMx);
            auto it = g_BoltModelSamples.find(model);
            if (it != g_BoltModelSamples.end())
            {
                memcpy(it->second.rest, restNow, 0x40);
                it->second.haveRest = true;
            }
        }
        static std::mutex mx;
        static std::map<void*, int> cnt;
        std::lock_guard<std::mutex> lock(mx);
        int& n = cnt[caller];
        ++n;
        if (n <= 8)
            LogDebug("[WeaponKey] BoltReapply eq=%u model=%p boneIdx=%llu ctl=%p after %s "
                "(stomp caller=%p) - post-stomp rest captured as baseline, then the last "
                "engine-computed bolt matrix re-applied for render\n",
                s.eq, model, s.boneIdx, ctl, fromFn, caller);
    }

    static void RestoreBoltBaseline(void* ctl)
    {
        for (unsigned long long slot = 0; slot < 2; ++slot)
        {
            if (SlotHasAnimControl(ctl, slot))
                continue;
            void* models = ReadPtrAtSEH(ctl, 0x58);
            if (!models)
                return;
            void* model = ReadPtrAtSEH(models, static_cast<size_t>(slot) * 8);
            if (!model)
                continue;
            BoltModelSample s;
            {
                std::lock_guard<std::mutex> lock(g_BoltBoneMx);
                auto it = g_BoltModelSamples.find(model);
                if (it == g_BoltModelSamples.end() || !it->second.haveRest ||
                    it->second.writerCtl != ctl)
                    continue;
                s = it->second;
            }
            void* pose = ReadPtrAtSEH(model, 0xE0);
            if (!pose)
                continue;
            CopyBoltBytesSEH(static_cast<std::uint8_t*>(pose) + s.boneIdx * 0x40,
                             s.rest, 0x40);
        }
    }

    static unsigned long long __fastcall hkPutMotion(
        void* ctl, unsigned long long slot, unsigned long long c, unsigned long long d)
    {
        const unsigned long long r = g_OrigPutMotion(ctl, slot, c, d);
        ReapplyBoltToModel(ctl, slot, "PutMotion", _ReturnAddress());
        return r;
    }

    static unsigned long long __fastcall hkPutMotionAll(
        void* ctl, unsigned long long b, unsigned long long c, unsigned long long d)
    {
        const unsigned long long r = g_OrigPutMotionAll(ctl, b, c, d);
        const int count = ReadByteAtSEH(ctl, 0x50);
        for (int s = 0; s < count && s < 8; ++s)
            ReapplyBoltToModel(ctl, static_cast<unsigned long long>(s),
                               "PutMotionAll", _ReturnAddress());
        return r;
    }

    using SetMotionDataFn_t = unsigned char(__fastcall*)(void*, unsigned int, void*);
    using SetMotionByPathFn_t =
        unsigned char(__fastcall*)(void*, unsigned int, unsigned long long);
    static SetMotionDataFn_t g_OrigSetMotionData = nullptr;
    static SetMotionByPathFn_t g_OrigSetMotionByPath = nullptr;
    static int g_SetMotionDataSlot = -1;
    static int g_SetMotionByPathSlot = -1;
    static std::atomic<int> g_SetMotionLogBudget{ 400 };

    static unsigned EqForCtl(void* ctl)
    {
        std::lock_guard<std::mutex> lock(g_BoltBoneMx);
        auto it = g_CtlToEq.find(ctl);
        return it != g_CtlToEq.end() ? it->second.eq : 0;
    }

    static unsigned SlotLockFlag(void* ctl, unsigned int slot)
    {
        void* flagsArr = ReadPtrAtSEH(ctl, 0xd0);
        return flagsArr ? (ReadByteAtSEH(flagsArr, slot) & 0xFF) : 0xFFFF;
    }

    static unsigned char __fastcall hkSetMotionData(void* ctl, unsigned int slot,
                                                    void* desc)
    {
        const unsigned char r = g_OrigSetMotionData(ctl, slot, desc);
        if (g_SetMotionLogBudget.fetch_sub(1) > 0)
        {
            const unsigned long long clip =
                reinterpret_cast<unsigned long long>(ReadPtrAtSEH(desc, 8));
            LogDebug("[WeaponKey] SetMotionData ctl=%p slot=%u eq=%u clip=%016llX -> %s "
                "flag=0x%02X (REFUSED with flag bit2 set = the control's motion mtar "
                "never finished streaming - its data is not in any loaded pack)\n",
                ctl, slot, EqForCtl(ctl), clip, r ? "SET" : "REFUSED",
                SlotLockFlag(ctl, slot));
        }
        return r;
    }

    using AnimGetDataInfoFn_t = void(__fastcall*)(void*, void*);

    static bool CallGetDataInfoSEH(void* fn, void* animFile, void* out)
    {
        __try
        {
            reinterpret_cast<AnimGetDataInfoFn_t>(fn)(animFile, out);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static std::atomic<int> g_RefuseForensicsBudget{ 24 };
    static std::atomic<int> g_SetForensicsBudget{ 8 };

    static void LogSetMotionForensics(void* ctl, unsigned int slot,
                                      unsigned long long clip, unsigned char result)
    {
        void* slots = ReadPtrAtSEH(ctl, 0x60);
        void* outer = slots ? ReadPtrAtSEH(slots, static_cast<size_t>(slot) * 8) : nullptr;
        if (!outer)
        {
            LogDebug("[WeaponKey] clip forensics ctl=%p slot=%u clip=%016llX: NO SimpleControl "
                "bound for the slot - Realize never attached clip playback here\n",
                ctl, slot, clip);
            return;
        }
        const int innerOff = ReadDwordAtSEH2(outer, 0xc);
        void* inner = static_cast<char*>(outer) + innerOff;
        void* mtar = ReadPtrAtSEH(inner, 0x80);
        if (!mtar)
        {
            LogDebug("[WeaponKey] clip forensics ctl=%p slot=%u clip=%016llX: SimpleControl "
                "%p has NO MtarFile bound (inner+0x80 empty) - the control exists but "
                "carries no motion archive\n",
                ctl, slot, clip, outer);
            return;
        }
        void* dir = ReadPtrAtSEH(mtar, 0x98);
        const int dirCount = dir ? ReadDwordAtSEH2(dir, 4) : -1;
        const int dirFlags = dir
            ? ((ReadByteAtSEH(dir, 0x12) & 0xFF) | ((ReadByteAtSEH(dir, 0x13) & 0xFF) << 8))
            : -1;
        void* af = nullptr;
        void* dataPtr = nullptr;
        int dataHead = -1;
        if (::AddressSetRuntime::IsEn154Family(gGameBuild))
        {
            af = CallGetAnimFileSEH(ResolveGameAddress(gAddr.Mtar_GetAnimFile), mtar, clip);
            if (af && reinterpret_cast<uintptr_t>(af) > 1)
            {
                unsigned char info[0x60] = {};
                if (CallGetDataInfoSEH(ResolveGameAddress(gAddr.Mtar_GetDataInfo), af, info))
                {
                    dataPtr = *reinterpret_cast<void**>(info);
                    if (dataPtr)
                        dataHead = ReadDwordAtSEH2(dataPtr, 0);
                }
            }
        }
        const char* ident = "UNKNOWN mtar";
        static const struct { const char* path; const char* name; } kKnown[] = {
            { "/Assets/tpp/motion/mtar/equip/chimera/receiver/hg02_default.mtar",
              "receiver/hg02_default.mtar" },
            { "/Assets/tpp/motion/mtar/equip/chimera/receiver/hg09_default.mtar",
              "receiver/hg09_default.mtar" },
            { "/Assets/tpp/motion/mtar/equip/chimera/assemble/ar00_asm.mtar",
              "assemble/ar00_asm.mtar" },
            { "/Assets/tpp/motion/mtar/player2/Receiver/pl_rcvr_hg02.mtar",
              "player2/pl_rcvr_hg02.mtar" },
        };
        for (const auto& k : kKnown)
        {
            if (ResolveMtarForEntrySEH(FoxHashes::PathCode64Ext(k.path)) == mtar)
            {
                ident = k.name;
                break;
            }
        }
        void* models = ReadPtrAtSEH(ctl, 0x58);
        void* model = models ? ReadPtrAtSEH(models, static_cast<size_t>(slot) * 8) : nullptr;
        int boneA = -1, boneB = -1;
        if (model)
        {
            boneA = (ReadByteAtSEH(model, 0xa0 + 0x58) & 0xFF)
                    | ((ReadByteAtSEH(model, 0xa0 + 0x59) & 0xFF) << 8);
            void* ctxB = ReadPtrAtSEH(model, 0x190);
            if (ctxB)
                boneB = (ReadByteAtSEH(ctxB, 0x58) & 0xFF)
                        | ((ReadByteAtSEH(ctxB, 0x59) & 0xFF) << 8);
        }
        LogDebug("[WeaponKey] clip forensics ctl=%p slot=%u eq=%u clip=%016llX result=%s: "
            "bound mtar=%p [%s] dir=%p entries=%d dirFlags=0x%04X | GetAnimFile -> %p "
            "(%s) trackData=%p head=%d | model=%p boneCapA=%d boneCapB=%d (mtar identity "
            "names which archive the control actually searches; GetAnimFile NULL = the "
            "clip name is not in that archive; non-NULL with REFUSED = the downstream "
            "track-vs-bone guard rejected it)\n",
            ctl, slot, EqForCtl(ctl), clip, result ? "SET" : "REFUSED",
            mtar, ident, dir, dirCount, dirFlags, af,
            (reinterpret_cast<uintptr_t>(af) == 1) ? "FAULT" : (af ? "FOUND" : "MISS"),
            dataPtr, dataHead, model, boneA, boneB);
    }

    static unsigned char __fastcall hkSetMotionByPath(void* ctl, unsigned int slot,
                                                      unsigned long long clip)
    {
        const unsigned char r = g_OrigSetMotionByPath(ctl, slot, clip);
        if (g_SetMotionLogBudget.fetch_sub(1) > 0)
            LogDebug("[WeaponKey] SetMotionByPath ctl=%p slot=%u eq=%u clip=%016llX -> %s "
                "flag=0x%02X\n",
                ctl, slot, EqForCtl(ctl), clip, r ? "SET" : "REFUSED",
                SlotLockFlag(ctl, slot));
        if (!r && g_RefuseForensicsBudget.fetch_sub(1) > 0)
            LogSetMotionForensics(ctl, slot, clip, r);
        else if (r && g_SetForensicsBudget.fetch_sub(1) > 0)
            LogSetMotionForensics(ctl, slot, clip, r);
        return r;
    }

    static void TryArmBoltBoneHooks(void* obj)
    {
        if (g_BoltBoneVtbl)
            return;
        void* ctl = ReadPtrAtSEH(obj, 0x30);
        if (!ctl)
            return;
        void** vtbl = static_cast<void**>(ReadPtrAtSEH(ctl, 0));
        if (!vtbl)
            return;
        void* idxFn = ReadPtrAtSEH(vtbl, 0x140);
        void* writeFn = ReadPtrAtSEH(vtbl, 0x168);
        void* putAllFn = ReadPtrAtSEH(vtbl, 0x200);
        void* putFn = ReadPtrAtSEH(vtbl, 0x210);
        if (!idxFn || !writeFn || !putAllFn || !putFn)
            return;
        DWORD oldProt = 0;
        if (!VirtualProtect(&vtbl[0x140 / 8], sizeof(void*) * 6, PAGE_READWRITE, &oldProt))
            return;
        g_OrigBoltBoneIdx = reinterpret_cast<BoltBoneFn_t>(idxFn);
        g_OrigBoltBoneWrite = reinterpret_cast<BoltBoneFn_t>(writeFn);
        vtbl[0x140 / 8] = reinterpret_cast<void*>(&hkBoltBoneIdx);
        vtbl[0x168 / 8] = reinterpret_cast<void*>(&hkBoltBoneWrite);
        VirtualProtect(&vtbl[0x140 / 8], sizeof(void*) * 6, oldProt, &oldProt);
        if (VirtualProtect(&vtbl[0x200 / 8], sizeof(void*) * 3, PAGE_READWRITE, &oldProt))
        {
            g_OrigPutMotionAll = reinterpret_cast<BoltBoneFn_t>(putAllFn);
            g_OrigPutMotion = reinterpret_cast<BoltBoneFn_t>(putFn);
            vtbl[0x200 / 8] = reinterpret_cast<void*>(&hkPutMotionAll);
            vtbl[0x210 / 8] = reinterpret_cast<void*>(&hkPutMotion);
            VirtualProtect(&vtbl[0x200 / 8], sizeof(void*) * 3, oldProt, &oldProt);
        }
        g_BoltBoneVtbl = vtbl;
        if (::AddressSetRuntime::IsEn154Family(gGameBuild))
        {
            void* smdTarget =
                reinterpret_cast<void*>(ResolveGameAddress(gAddr.SimplePartsControllerImpl_SetMotionData));
            void* smbpTarget =
                reinterpret_cast<void*>(ResolveGameAddress(gAddr.SimplePartsControllerImpl_SetMotionDataByPath));
            for (int i = 0; i < 0x300 / 8; ++i)
            {
                void* fn = ReadPtrAtSEH(vtbl, static_cast<size_t>(i) * 8);
                if (fn == smdTarget)
                    g_SetMotionDataSlot = i;
                else if (fn == smbpTarget ||
                         fn == reinterpret_cast<void*>(&hkClipArchiveFallback))
                    g_SetMotionByPathSlot = i;
            }
            if (g_SetMotionDataSlot >= 0 && g_SetMotionByPathSlot >= 0)
            {
                const int lo = (g_SetMotionDataSlot < g_SetMotionByPathSlot)
                                   ? g_SetMotionDataSlot : g_SetMotionByPathSlot;
                const int hi = (g_SetMotionDataSlot < g_SetMotionByPathSlot)
                                   ? g_SetMotionByPathSlot : g_SetMotionDataSlot;
                DWORD prot = 0;
                if (VirtualProtect(&vtbl[lo],
                                   sizeof(void*) * static_cast<size_t>(hi - lo + 1),
                                   PAGE_READWRITE, &prot))
                {
                    g_OrigSetMotionData = reinterpret_cast<SetMotionDataFn_t>(
                        vtbl[g_SetMotionDataSlot]);
                    g_OrigSetMotionByPath = reinterpret_cast<SetMotionByPathFn_t>(
                        vtbl[g_SetMotionByPathSlot]);
                    vtbl[g_SetMotionDataSlot] =
                        reinterpret_cast<void*>(&hkSetMotionData);
                    vtbl[g_SetMotionByPathSlot] =
                        reinterpret_cast<void*>(&hkSetMotionByPath);
                    VirtualProtect(&vtbl[lo],
                                   sizeof(void*) * static_cast<size_t>(hi - lo + 1),
                                   prot, &prot);
                    LogDebug("[WeaponKey] clip-set probes armed: SetMotionData slot +0x%X, "
                        "SetMotionDataByPath slot +0x%X (logs every weapon clip the "
                        "engine sets on a parts control - vanilla vs custom comparison "
                        "shows whether the custom weapon's states ever set clips)\n",
                        g_SetMotionDataSlot * 8, g_SetMotionByPathSlot * 8);
                }
            }
            else
                LogDebug("[WeaponKey] clip-set probes NOT armed (SetMotionData/ByPath not "
                    "found in vtbl scan: %d/%d)\n",
                    g_SetMotionDataSlot, g_SetMotionByPathSlot);
        }
        LogDebug("[WeaponKey] bolt-bone vtable probes armed: vtbl=%p GET-IDX(+0x140)=%p "
            "WRITE(+0x168)=%p PutMotionAll(+0x200)=%p PutMotion(+0x210)=%p (PutMotion "
            "default-poses any slot whose model has no anim control - the custom weapon's "
            "case - so the bolt matrix is re-applied right after every such stomp)\n",
            vtbl, idxFn, writeFn, putAllFn, putFn);
    }
#endif

    static unsigned long long __fastcall hkPostPutMotion(
        void* self, void* b, void* c, void* d)
    {
#ifdef _DEBUG
        unsigned eq = 0;
        bool tracked = false;
        unsigned pre4[2] = { 0, 0 };
        unsigned pre10[2] = { 0, 0 };
        void* base = ReadPtrAtSEH(self, 0xe8);
        if (base)
        {
            const unsigned w4 = ReadRecWord(base, 0, 4);
            const unsigned hi = w4 & 0xF800u;
            if (w4 != 0xFFFFFFFFu && (hi == 0x1000u || hi == 0x1800u))
            {
                eq = w4 & 0x7FFu;
                tracked = true;
                for (int s = 0; s < 2; ++s)
                {
                    pre4[s] = ReadRecWord(base, s, 4);
                    pre10[s] = ReadRecWord(base, s, 0x10);
                }
            }
        }
        {
            void* ctl = ReadPtrAtSEH(self, 0x30);
            if (ctl)
            {
                CheckBoltWritePersist(ctl);
                RestoreBoltBaseline(ctl);
                if (tracked)
                {
                    TryArmBoltBoneHooks(self);
                    TryArmChimeraHooks(self);
                    CtlInfo info;
                    info.eq = eq;
                    info.iface = ReadPtrAtSEH(self, 0x120);
                    std::lock_guard<std::mutex> lock(g_BoltBoneMx);
                    g_CtlToEq[ctl] = info;
                }
            }
        }
        if (tracked)
        {

            void* baseC8 = ReadPtrAtSEH(self, 0xc8);
            const unsigned e6 = ReadRecWord(base, 0, 6);
            const unsigned c6 = baseC8 ? ReadRecWord(baseC8, 0, 6) : 0xFFFFFFFFu;
            unsigned ai0 = 0xFFFFFFFFu;
            int b0B = -1, b0C = -1;
            ReadAmmoRowSEH(self, 0, ai0, b0B, b0C);
            static std::mutex mxC;
            static std::map<unsigned, unsigned> lastC;
            static std::map<unsigned, int> cntC;
            std::lock_guard<std::mutex> lock(mxC);
            const unsigned packed = ((e6 & 0xFFFFu) << 16) | (c6 & 0xFFFFu);
            int& n = cntC[eq];
            unsigned& last = lastC[eq];
            if (n < 40 && (packed != last || n < 2))
            {
                ++n;
                last = packed;
                LogDebug("[WeaponKey] CYLINDER-DIAG eq=%u obj=%p | rec6[e8]=%04X (isCyl=%u vis6-11=%02X) | "
                    "rec6[c8]=%04X (vis=%02X) | ammoRow +0xB=%02X +0xC=%02X (show 0x20 %s) - e8 is "
                    "what the reader+positioner read; c8 is what SetAmmoToCylinder writes. During a "
                    "loaded cylinder: e8 vis!=0 => rounds should place; e8 vis=0 while c8 vis!=0 => "
                    "load state landing in a record the reader never reads (the desync); both 0 => "
                    "SetAmmoToCylinder never populated it (dispatch/type gate)\n",
                    eq, self,
                    e6, e6 & 1u, (e6 >> 6) & 0x3Fu,
                    c6, (c6 >> 6) & 0x3Fu,
                    b0B & 0xFF, b0C & 0xFF, (b0C & 0x20) ? "SET" : "CLEAR");
            }
        }
        if (tracked && (pre10[0] & 0xFF) != 2)
        {
            static std::mutex mxA;
            static std::map<unsigned, int> cntA;
            std::lock_guard<std::mutex> lock(mxA);
            int& n = cntA[eq];
            ++n;
            if (n <= 6)
                LogDebug("[WeaponKey] reader ACTIVE eq=%u obj=%p state=%04X (the reader is "
                    "servicing this object while its bolt state is non-idle - if this "
                    "line never appears for an equip whose DoFire obj armed the state, "
                    "the two are different objects and the armed one is never updated)\n",
                    eq, self, pre10[0]);
        }
        const unsigned long long r = g_OrigPostPutMotion(self, b, c, d);
        FeedCylinderVisibilitySEH(self);
        DriveCylinderRounds();
        DriveCylinderRotation();
        if (t_probeSlots > 0 && t_drivenHash)
        {
            static std::mutex mxP;
            static std::map<unsigned, int> cntP;
            std::lock_guard<std::mutex> lock(mxP);
            int& np = cntP[t_meshEq];
            if (np < 4)
            {
                ++np;
                LogDebug("[WeaponKey] CYL-MESH eq=%u ammo=%u -> showing %u of %d hidden round meshes "
                    "(round meshes are the hidden-by-default ones, flag 0xFF; first `ammo` shown, rest "
                    "hidden - so the loaded count is exact and empties as you fire)\n",
                    t_meshEq, t_cylAmmo,
                    (t_cylAmmo > (unsigned)t_roundCount ? (unsigned)t_roundCount : t_cylAmmo),
                    t_roundCount);
            }
        }
        if (t_meshCount > 0)
        {
            static std::mutex mxM;
            static std::map<unsigned, int> cntM;
            std::lock_guard<std::mutex> lock(mxM);
            int& nm = cntM[t_meshEq];
            if (nm < 1)
            {
                ++nm;
                bool amoPresent = false;
                for (int i = 0; i < t_meshCount; ++i)
                    if (t_meshHash[i] == 0x4E943DFCu) amoPresent = true;
                LogDebug("[WeaponKey] CYL-MESHDUMP eq=%u meshCount=%d MESH_AMO(0x4E943DFC)present=%d - the "
                    "cylinder round mesh is named per-model (no universal hash); its 32-bit name-hash "
                    "is one of these. Identify it (usually a mesh whose flag differs from the body "
                    "meshes / your fmdl's ammo mesh) and I drive THAT hash by ammo:\n",
                    t_meshEq, t_meshCount, amoPresent ? 1 : 0);
                for (int i = 0; i < t_meshCount; ++i)
                    LogDebug("[WeaponKey]   mesh[%02d] hash=%08X flagA=%02X flagB=%02X\n",
                        i, t_meshHash[i], t_meshFA[i], t_meshFB[i]);
            }
        }
        if (tracked)
        {
            for (int s = 0; s < 2; ++s)
            {
                const unsigned post4 = ReadRecWord(base, s, 4);
                const unsigned post10 = ReadRecWord(base, s, 0x10);
                if (post4 == 0xFFFFFFFFu || post10 == 0xFFFFFFFFu)
                    continue;
                if (pre4[s] != post4 || pre10[s] != post10)
                {
                    static std::mutex mx;
                    static std::map<unsigned, int> cnt;
                    std::lock_guard<std::mutex> lock(mx);
                    int& n = cnt[(eq << 4) | static_cast<unsigned>(s)];
                    ++n;
                    if (n <= 12)
                        LogDebug("[WeaponKey] BOLT PASS eq=%u slot=%d obj=%p rec4 %04X->%04X state "
                            "%04X->%04X (the reader received and consumed a bolt/slide request "
                            "for this weapon)\n",
                            eq, s, self, pre4[s], post4, pre10[s], post10);
                }
            }
        }
        return r;
#else
        const unsigned long long r = g_OrigPostPutMotion(self, b, c, d);
        FeedCylinderVisibilitySEH(self);
        DriveCylinderRounds();
        DriveCylinderRotation();
        return r;
#endif
    }


    using PartsCtlCreateSlot_t =
        void(__fastcall*)(void*, std::uint32_t, void*, std::uint8_t);
    static PartsCtlCreateSlot_t g_OrigPartsCtlCreateSlot = nullptr;
    static void* g_PartsCtlCreateSlotAddr = nullptr;

    static uintptr_t PartsCtlCreateSlotAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x140adde80ull;
        default:                                          return 0;
        }
    }

    struct RemapCreateFns
    {
        void* size;
        void* place;
        void* heap;
        void* dtorPool;
        void* dtorHeap;
        void* aux;
    };

    static bool ResolveRemapCreateFns(RemapCreateFns& f)
    {
        if (!::AddressSetRuntime::IsEn154Family(gGameBuild))
            return false;
        f.size = ResolveGameAddress(gAddr.Animx_GetControlSize);
        f.place = ResolveGameAddress(gAddr.Animx_SimpleControlCtorPool);
        f.heap = ResolveGameAddress(gAddr.Animx_SimpleControlCtorHeap);
        f.dtorPool = ResolveGameAddress(gAddr.Animx_SimpleControlDtorPool);
        f.dtorHeap = ResolveGameAddress(gAddr.Animx_SimpleControlDtorHeap);
        f.aux = ResolveGameAddress(gAddr.Animx_SimpleControlAux);
        return f.size && f.place && f.heap && f.dtorPool && f.dtorHeap && f.aux;
    }

    static int SnapshotControlsSEH(void* ctl, void** out, int cap)
    {
        __try
        {
            std::uint8_t* c = static_cast<std::uint8_t*>(ctl);
            int n = *reinterpret_cast<std::int32_t*>(c + 0x8);
            void** arr = *reinterpret_cast<void***>(c + 0x60);
            if (!arr || n <= 0)
                return 0;
            if (n > cap)
                n = cap;
            for (int i = 0; i < n; ++i)
                out[i] = arr[i];
            return n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int FindChangedControlSEH(void* ctl, void** before, int n)
    {
        __try
        {
            void** arr = *reinterpret_cast<void***>(
                static_cast<std::uint8_t*>(ctl) + 0x60);
            if (!arr)
                return -1;
            for (int i = 0; i < n; ++i)
                if (arr[i] && arr[i] != before[i])
                    return i;
            return -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    struct RemapCreateParams
    {
        std::uint32_t flags;
        std::uint32_t count;
        void* mtar;
        void* model;
        void* helpBone;
    };

    static int RebuildControlWithRemapSEH(void* ctlIn, int idx, void* infoIn,
                                          const RemapCreateFns& f,
                                          unsigned* outSize, unsigned* outStride)
    {
        __try
        {
            std::uint8_t* ctl = static_cast<std::uint8_t*>(ctlIn);
            std::uint8_t* info = static_cast<std::uint8_t*>(infoIn);
            void** ctlArr = *reinterpret_cast<void***>(ctl + 0x60);
            void** mdlArr = *reinterpret_cast<void***>(ctl + 0x58);
            if (!ctlArr || !mdlArr)
                return -1;
            std::uint8_t* old = static_cast<std::uint8_t*>(ctlArr[idx]);
            void* model = mdlArr[idx];
            if (!old || !model)
                return -2;
            std::uint8_t* desc = *reinterpret_cast<std::uint8_t**>(info + 0x10);
            if (!desc)
                return -3;
            void* mtar = *reinterpret_cast<void**>(desc);
            if (!mtar)
                return -4;
            std::uint8_t* inner = old + *reinterpret_cast<std::int32_t*>(old + 0xc);
            if (*reinterpret_cast<std::int32_t*>(inner + 0x30) != 0)
                return 1;
            const std::uint8_t descFlags = desc[0xc];
            RemapCreateParams p{};
            p.flags = 0x200u
                      | (*reinterpret_cast<void**>(ctl + 0x78) ? 8u : 0u)
                      | ((descFlags & 2) ? 1u : 0u)
                      | 4u;
            p.count = *reinterpret_cast<std::uint32_t*>(desc + 8) + 1;
            p.mtar = mtar;
            p.model = model;
            p.helpBone = nullptr;
            if (descFlags & 1)
            {
                void* hb = *reinterpret_cast<void**>(
                    static_cast<std::uint8_t*>(model) + 0xe0);
                if (hb)
                    p.helpBone = hb;
            }
            const std::uint64_t size =
                reinterpret_cast<std::uint64_t(__fastcall*)(void*)>(f.size)(&p);
            std::uint8_t* pool = *reinterpret_cast<std::uint8_t**>(ctl + 0x68);
            const std::uint32_t stride = *reinterpret_cast<std::uint32_t*>(ctl + 0x70);
            if (outSize)
                *outSize = static_cast<unsigned>(size);
            if (outStride)
                *outStride = pool ? stride : 0u;
            if (pool && size > stride)
                return -5;
            if (pool)
            {

                reinterpret_cast<void(__fastcall*)(void*)>(f.dtorPool)(old);
                void* mem = pool + static_cast<std::size_t>(stride)
                                       * static_cast<unsigned>(idx);
                reinterpret_cast<void*(__fastcall*)(void*, void*, std::uint64_t)>(
                    f.place)(mem, &p, size);
                ctlArr[idx] = mem;
                if (*reinterpret_cast<void**>(ctl + 0xd8))
                {
                    std::uint8_t* aux = *reinterpret_cast<std::uint8_t**>(ctl + 0xe8);
                    if (aux)
                        reinterpret_cast<void(__fastcall*)(void*, void*)>(f.aux)(
                            mem, aux + static_cast<std::size_t>(idx) * 0x50);
                }
                return 0;
            }

            bool remapped = true;
            void* fresh = reinterpret_cast<void*(__fastcall*)(void*)>(f.heap)(&p);
            if (!fresh)
            {
                p.flags &= ~4u;
                remapped = false;
                fresh = reinterpret_cast<void*(__fastcall*)(void*)>(f.heap)(&p);
            }
            if (!fresh)
                return -6;
            reinterpret_cast<void(__fastcall*)(void*)>(f.dtorHeap)(old);
            ctlArr[idx] = fresh;
            if (*reinterpret_cast<void**>(ctl + 0xd8))
            {
                std::uint8_t* aux = *reinterpret_cast<std::uint8_t**>(ctl + 0xe8);
                if (aux)
                    reinterpret_cast<void(__fastcall*)(void*, void*)>(f.aux)(
                        fresh, aux + static_cast<std::size_t>(idx) * 0x50);
            }
            return remapped ? 0 : -7;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -100;
        }
    }

    static void* DescMtarSEH(void* info)
    {
        __try
        {
            std::uint8_t* desc = *reinterpret_cast<std::uint8_t**>(
                static_cast<std::uint8_t*>(info) + 0x10);
            return desc ? *reinterpret_cast<void**>(desc) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static int MtarMaxTracksSEH(void* mtar)
    {
        __try
        {
            std::uint8_t* dir = *reinterpret_cast<std::uint8_t**>(
                static_cast<std::uint8_t*>(mtar) + 0x98);
            return dir ? *reinterpret_cast<std::uint16_t*>(dir + 8) : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static int ModelBoneCapSEH(void* ctl, int idx)
    {
        __try
        {
            void** mdlArr = *reinterpret_cast<void***>(
                static_cast<std::uint8_t*>(ctl) + 0x58);
            std::uint8_t* model =
                mdlArr ? static_cast<std::uint8_t*>(mdlArr[idx]) : nullptr;
            return model ? *reinterpret_cast<std::int16_t*>(model + 0xa0 + 0x58) : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static bool CallOrigCreateSlotSEH(void* ctl, std::uint32_t slot, void* info, std::uint8_t d)
    {
        __try
        {
            g_OrigPartsCtlCreateSlot(ctl, slot, info, d);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void __fastcall hkPartsCtlCreateSlot(void* ctl, std::uint32_t slot,
                                                void* info, std::uint8_t d)
    {
        void* before[32] = {};
        int n = 0;
        if (ctl && info)
            n = SnapshotControlsSEH(ctl, before, 32);
        if (!CallOrigCreateSlotSEH(ctl, slot, info, d))
        {
#ifdef _DEBUG
            static std::atomic<int> budget{ 8 };
            if (budget.fetch_sub(1) > 0)
                LogDebug("[WeaponKey] PartsCtlCreateSlot FAULTED (caught) ctl=%p slot=%u info=%p - a part "
                    "could not build its control (missing/bad model asset); slot left uncreated to "
                    "avoid a hard crash. The weapon has an unloaded .parts/fpk.\n",
                    ctl, slot, info);
#endif
            return;
        }
        if (n <= 0)
            return;
        RemapCreateFns f{};
        if (!ResolveRemapCreateFns(f))
            return;
        const int idx = FindChangedControlSEH(ctl, before, n);
        void* mtar = (idx >= 0) ? DescMtarSEH(info) : nullptr;
        const int need = mtar ? MtarMaxTracksSEH(mtar) : -1;
        const int cap = (idx >= 0) ? ModelBoneCapSEH(ctl, idx) : -1;
#ifdef _DEBUG
        static std::atomic<int> probeBudget{ 40 };
        if (idx >= 0 && probeBudget.fetch_sub(1) > 0)
            LogDebug("[WeaponKey] partsCtl create probe ctl=%p slot=%u idx=%d mtar=%p "
                "mtarTracks=%d modelBoneCap=%d (a line with tracks-1 > cap is the "
                "pairing the track guard refuses and the remap rebuild targets)\n",
                ctl, slot, idx, mtar, need, cap);
#endif
        if (idx < 0 || !mtar)
            return;
        if (need <= 0 || cap < 0 || cap >= need - 1)
            return;
        std::uint32_t eq = 0;
        {
            std::uint64_t entries[64];
            std::uint32_t eqIds[64];
            int cnt = 0;
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                for (const auto& kv : g_RemapEntryEq)
                {
                    if (cnt >= 64)
                        break;
                    entries[cnt] = kv.first;
                    eqIds[cnt] = kv.second;
                    ++cnt;
                }
            }
            for (int i = 0; i < cnt; ++i)
                if (ResolveMtarForEntrySEH(entries[i]) == mtar)
                {
                    eq = eqIds[i];
                    break;
                }
        }
        if (idx != static_cast<int>(slot))
        {
            LogDebug("[WeaponKey] remap control: eq=%u slot=%u SKIPPED (engine placed the "
                "control at index %d, pool addressing would not match)\n",
                eq, slot, idx);
            return;
        }
        unsigned size = 0, stride = 0;
        const int rc = RebuildControlWithRemapSEH(ctl, idx, info, f, &size, &stride);
        static std::mutex mx;
        static std::set<std::uint64_t> logged;
        bool isNew;
        {
            std::lock_guard<std::mutex> lock(mx);
            isNew = logged.size() < 128 &&
                    logged.insert((static_cast<std::uint64_t>(eq) << 8)
                                  | static_cast<std::uint8_t>(rc)).second;
        }
        if (!isNew)
            return;
        if (rc == 0)
        {
#ifdef _DEBUG
            LogDebug("[WeaponKey] remap control: eq=%u slot=%u rebuilt with the skeleton-"
                "remap variant (size=%u poolStride=%u mtarTracks=%d modelBoneCap=%d) - "
                "clips now bind to this weapon's bones by NAME, so chimera-rig clips "
                "with more tracks than the model has bones drive the bones that exist "
                "instead of being refused whole\n",
                eq, slot, size, stride, need, cap);
            void* pool = ReadPtrAtSEH(ctl, 0x68);
            const int strideQ = ReadDwordAtSEH2(ctl, 0x70);
            std::uint8_t* mem =
                pool ? static_cast<std::uint8_t*>(pool)
                           + static_cast<size_t>(strideQ) * idx
                     : nullptr;
            if (mem)
            {
                std::uint8_t* inner2 = mem + ReadDwordAtSEH2(mem, 0xc);
                const int roff = ReadDwordAtSEH2(inner2, 0x30);
                if (roff > 0 && roff < 0x2000)
                {
                    char buf[256];
                    int bl = 0;
                    buf[0] = 0;
                    int miss = 0;
                    for (int t = 0; t < need && t < 24; ++t)
                    {
                        const int w =
                            (ReadByteAtSEH(inner2, roff + t * 2) & 0xFF)
                            | ((ReadByteAtSEH(inner2, roff + t * 2 + 1) & 0xFF)
                               << 8);
                        if (w == 0xFFFF)
                            ++miss;
                        if (bl < 200)
                            bl += _snprintf_s(buf + bl, sizeof(buf) - bl,
                                              _TRUNCATE, " %d",
                                              w == 0xFFFF ? -1 : w);
                    }
                    LogDebug("[WeaponKey] remap table eq=%u: %d clip tracks, %d "
                        "UNMAPPED (track order:%s) - each -1 is a chimera-rig "
                        "bone the weapon model does not carry (for a revolver "
                        "these are typically the cylinder BULLET bones, which "
                        "is why loaded rounds never appear); adding bones with "
                        "the chimera family's names to the fmdl makes those "
                        "tracks bind\n",
                        eq, need, miss, buf);
                }
            }
#endif
        }
        else if (rc == 1)
            ;
        else if (rc == -5)
            LogDebug("[WeaponKey] remap control: eq=%u slot=%u NOT rebuilt - remap variant "
                "needs %u bytes but the controller pool slot is %u; original control "
                "kept\n",
                eq, slot, size, stride);
        else
            Log("[WeaponKey] remap control: eq=%u slot=%u rebuild failed rc=%d "
                "(size=%u poolStride=%u) - original control %s\n",
                eq, slot, rc, size, stride,
                (rc == -7) ? "recreated without remap" : "left as-is");
    }

    static unsigned long long __fastcall hkRealizedEquipRealize(
        void* self, unsigned int a, unsigned int b)
    {
        FillCustomMotionEntries(self);
        unsigned long long r = 0;
        {
            StallWatch _stall("the engine's own equip Realize", a);
            r = g_OrigRealizedEquipRealize(self, a, b);
        }
        CaptureEquipDataProvider(self);
#ifdef _DEBUG
        void* desc = CallDescProviderSEH(self);
        int d[6] = { -1, -1, -1, -1, -1, -1 };
        if (desc)
            for (int i = 0; i < 6; ++i)
                d[i] = ReadByteAtSEH(desc, 0x78 + static_cast<size_t>(i));
        if (r == 0)
            return r;
        void* recBase = ReadPtrAtSEH(self, 0xc8);
        char recs[192];
        int pos = 0;
        recs[0] = 0;
        for (std::uint32_t s = 0; s < 2 && recBase; ++s)
        {
            char hex[64];
            int hp = 0;
            for (int i = 0; i < 0x14; ++i)
                hp += std::snprintf(hex + hp, sizeof(hex) - static_cast<size_t>(hp),
                                    "%02X", ReadByteAtSEH(recBase, s * 0x14 + i) & 0xFF);
            pos += std::snprintf(recs + pos, sizeof(recs) - static_cast<size_t>(pos),
                                 " [%u]=%s", s, hex);
        }
        LogDebug("[WeaponKey] Realize self=%p args=%u,%u ret=%llu desc=%p bytes(+78..7d)="
            "%02X %02X %02X %02X %02X %02X recs:%s\n",
            self, a, b, r, desc,
            d[0] & 0xFF, d[1] & 0xFF, d[2] & 0xFF, d[3] & 0xFF, d[4] & 0xFF, d[5] & 0xFF,
            recs);
        unsigned base32 = 0;
        for (int i = 0; i < 4; ++i)
            base32 |= static_cast<unsigned>(ReadByteAtSEH(self, 0x34 + static_cast<size_t>(i)) & 0xFF)
                      << (8 * i);
        const unsigned vslot = a - base32;
        void* vctl = ReadPtrAtSEH(self, 0x10);
        unsigned eqRec = 0;
        if (recBase && vslot < 2)
            eqRec = ((ReadByteAtSEH(recBase, vslot * 0x14 + 4) & 0xFF)
                     | ((ReadByteAtSEH(recBase, vslot * 0x14 + 5) & 0xFF) << 8)) & 0x7FF;
        LogDebug("[WeaponKey] Realize verdict eq=%u slot=%u ctl=%p animControl=%s (YES = the "
            "motion-mtar entry resolved to a LOADED motion file and the clip control was "
            "created - reload/cock clips should animate the parts; NO = entry missing OR "
            "the mtar file is not resident in memory, parts stay frozen)\n",
            eqRec, vslot, vctl,
            (vctl && SlotHasAnimControl(vctl, vslot)) ? "YES" : "NO");
        static std::atomic<int> resolverIdBudget{ 3 };
        if (resolverIdBudget.fetch_sub(1) > 0)
        {
            void* p58 = ReadPtrAtSEH(self, 0x58);
            void* v58 = p58 ? ReadPtrAtSEH(p58, 0) : nullptr;
            void* f28 = v58 ? ReadPtrAtSEH(v58, 0x28) : nullptr;
            void* p68 = ReadPtrAtSEH(self, 0x68);
            void* v68 = p68 ? ReadPtrAtSEH(p68, 0) : nullptr;
            void* f00 = v68 ? ReadPtrAtSEH(v68, 0) : nullptr;
            LogDebug("[WeaponKey] resolver identity: [REOI+0x58]=%p vtbl=%p mtarResolver(vtbl"
                "+0x28)=%p | [REOI+0x68]=%p vtbl=%p provider(vtbl+0x00)=%p (static "
                "addresses, no ASLR - game+offset maps directly into the EN154 "
                "disassembly dump; the +0x28 target IS the entry-to-MtarFile resolver)\n",
                p58, v58, f28, p68, v68, f00);
        }
#endif
        return r;
    }

    static void* __fastcall hkGetAnimFromGani(void* self, unsigned long long pathId)
    {
        void* r = g_OrigGetAnimFromGani(self, pathId);

#ifdef _DEBUG
        static std::mutex mx;
        static std::set<unsigned long long> seen;
        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(mx);
            if (seen.size() < 256)
                isNew = seen.insert(pathId).second;
        }
        if (isNew)
            LogDebug("[WeaponKey] gani %s: path=%016llX eq=%u\n",
                r ? "HIT " : "MISS", pathId, g_TlsCustomEquip);
#endif
        return r;
    }

    using SearchMotionData_t = void*(__fastcall*)(void*, unsigned long long, void*);
    static SearchMotionData_t g_OrigSearchMotionData = nullptr;
    static void* g_SearchMotionDataAddr = nullptr;

    static uintptr_t AnimSearchMotionDataAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141a94710ull;
        default:                                          return 0;
        }
    }

    static void* __fastcall hkSearchMotionData(void* self, unsigned long long hash,
                                               void* pkg)
    {
        void* r = g_OrigSearchMotionData(self, hash, pkg);
        if (!r)
        {
            static std::mutex mx;
            static std::set<unsigned long long> seen;
            bool isNew = false;
            {
                std::lock_guard<std::mutex> lock(mx);
                if (seen.size() < 64)
                    isNew = seen.insert(hash).second;
            }
            if (isNew)
            {
                const void* ra = _ReturnAddress();
                std::uintptr_t canon = 0;
                if (void* probe = ResolveGameAddress(0x140000000ull))
                    canon = reinterpret_cast<std::uintptr_t>(ra)
                          - reinterpret_cast<std::uintptr_t>(probe) + 0x140000000ull;
                LogDebug("[WeaponKey] motion clip MISS: hash=%016llX not found in any "
                    "loaded mtar | asked from RA=%p (canonical 0x%llX) self=%p pkg=%p\n",
                    hash, ra, (unsigned long long)canon, self, pkg);
            }
        }
        return r;
    }

    using EjectCasing_t = void(__fastcall*)(void*, unsigned int, bool);
    static EjectCasing_t g_OrigEjectCasing = nullptr;
    static void* g_EjectCasingAddr = nullptr;

    static uintptr_t EjectCasingAddr()
    {
        switch (gGameBuild)
        {
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4a:
        case ::AddressSetRuntime::GameBuild::En_1_0_15_4: return 0x141303da0ull;
        default:                                          return 0;
        }
    }

    static void ProbeEjectSEH(void* self, unsigned int slot, std::uint32_t& subId,
                              std::uint32_t& hw, std::uint8_t* d, void*& blk)
    {
        __try
        {
            std::uint8_t* a = static_cast<std::uint8_t*>(self);
            std::uint8_t* e8 = *reinterpret_cast<std::uint8_t**>(a + 0xe8);
            if (e8)
                subId = *reinterpret_cast<std::uint16_t*>(
                    e8 + 4 + static_cast<size_t>(slot) * 0x14) & 0x7ff;
            std::uint8_t* t60 = *reinterpret_cast<std::uint8_t**>(a + 0x60);
            std::uint8_t* idxArr = t60 ? *reinterpret_cast<std::uint8_t**>(t60 + 0x10)
                                       : nullptr;
            const int base54 = *reinterpret_cast<int*>(a + 0x54);
            if (idxArr)
                hw = *reinterpret_cast<std::uint32_t*>(
                    idxArr + (static_cast<size_t>(base54) + slot) * 4);
            std::uint8_t* t58 = *reinterpret_cast<std::uint8_t**>(a + 0x58);
            std::uint8_t* table = t58 ? *reinterpret_cast<std::uint8_t**>(t58 + 0x28)
                                      : nullptr;
            if (table)
                memcpy(d, table + static_cast<size_t>(hw) * 0xe, 14);
            std::uint8_t** r68 = *reinterpret_cast<std::uint8_t***>(a + 0x68);
            if (r68)
            {
                using Fn = void*(__fastcall*)(void*, std::uint8_t);
                blk = reinterpret_cast<Fn>(
                    *reinterpret_cast<void**>(*r68))(r68, d[8]);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static void __fastcall hkEjectCasing(void* self, unsigned int slot, bool alt)
    {
        std::uint32_t subId = 0, hw = 0;
        std::uint8_t d[14] = {};
        void* blk = nullptr;
        ProbeEjectSEH(self, slot, subId, hw, d, blk);
        static std::atomic<std::uint32_t> lastSub{ 0xFFFFFFFF };
        if (lastSub.exchange(subId, std::memory_order_relaxed) != subId)
            LogDebug("[WeaponKey] EjectCasing probe: subId=%u hw=%u tmplIdx=%u resolve=%p "
                "desc=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X\n",
                subId, hw, d[8], blk, d[0], d[1], d[2], d[3], d[4], d[5], d[6],
                d[7], d[8], d[9], d[10], d[11], d[12], d[13]);
        g_OrigEjectCasing(self, slot, alt);
    }

    static std::set<std::uint32_t> g_KeyTypeLogged;

    static void ApplyKeyTypeSwaps(void* self, int slot, KeyTypeSwap& st)
    {
        st.n = 0;
        if (!self || !g_HasSwaps.load(std::memory_order_relaxed))
            return;
        std::uint32_t fromIds[8], donors[8];
        int m = 0;
        {
            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
            for (const auto& kv : g_FamilyFrom)
            {
                if (m >= 8)
                    break;
                fromIds[m] = kv.first;
                donors[m] = kv.second;
                ++m;
            }
        }
        if (m == 0)
            return;
        std::uint32_t toTypes[8];
        for (int i = 0; i < m; ++i)
            toTypes[i] = GetEquipTypeForEquipId(donors[i]) & 0x1F;
        if (SwapKeyWordsSEH(self, slot, fromIds, toTypes, m, &st) > 0)
        {
            bool isNew;
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                isNew = g_KeyTypeLogged.insert(fromIds[0] ^ (slot << 16)).second;
            }
#ifdef _DEBUG
            if (isNew)
                LogDebug("[WeaponKey] key eqpType swapped to donor family for %d word(s) "
                    "slot=%d (menu type untouched)\n", st.n, slot);
#else
            (void)isNew;
#endif
        }
    }

    static std::uint32_t PartCodeCommon(int slot, void* self, std::uint32_t partId)
    {
        g_PartCallCount.fetch_add(1, std::memory_order_relaxed);
        if (g_InSetupWeaponInfo && g_TlsVanillaEquip != 0)
        {
            FamilyGb van;
            if (!GetGbForEquipId(g_TlsVanillaEquip, van))
            {
                static unsigned long long lastMs = 0;
                const unsigned long long now = GetTickCount64();
                if (now - lastMs > 2000)
                {
                    lastMs = now;
                    Log("[WeaponKey] WARNING: donor equipId=%u gunBasic unresolved at "
                        "draw time - part substitution skipped (slot %d); the weapon "
                        "may not animate correctly\n", g_TlsVanillaEquip, slot);
                }
            }
            else
            {
                std::uint32_t use = partId;
                if (g_PartCodeExact[slot])
                {
                    use = van.gb[kPartGbByte[slot]];
                    if ((slot == 3 || slot == 4) && use != 0)
                    {
                        FamilyGb cus;
                        if (GetGbForEquipId(g_TlsCustomEquip, cus)
                            && cus.gb[kPartGbByte[slot]] == 0)
                        {
                            use = partId;
#ifdef _DEBUG
                            static std::atomic<int> logged{ 0 };
                            const int bit = 1 << slot;
                            if (!(logged.fetch_or(bit, std::memory_order_relaxed) & bit))
                                LogDebug("[WeaponKey] donor %s (part %u) suppressed: "
                                    "custom weapon has none - hold would abort otherwise\n",
                                    slot == 3 ? "stock" : "underbarrel",
                                    van.gb[kPartGbByte[slot]]);
#endif
                        }
                    }
                }
                else
                {
                    FamilyGb cus;
                    if (partId != 0 && GetGbForEquipId(g_TlsCustomEquip, cus))
                        for (int k = 0; k < 5; ++k)
                            if (partId == cus.gb[kPartGbByte[k]])
                            {
                                use = van.gb[kPartGbByte[k]];
                                break;
                            }
                }
                const std::uint32_t code = g_OrigPartCode[slot](self, use);
                if (slot == 0 && (code == 0 || code == 0x2f))
                    Log("[WeaponKey] WARNING: receiver part code came out %u (0x%X) for "
                        "equipId=%u (rc %u->%u) - the engine will invalidate this "
                        "weapon; pick a donor whose receiver has a real motion type\n",
                        code, code, g_TlsCustomEquip, partId, use);
                return code;
            }
        }
        return g_OrigPartCode[slot](self, partId);
    }

    static std::uint32_t __fastcall hkPartCode0(void* s, std::uint32_t p) { return PartCodeCommon(0, s, p); }
    static std::uint32_t __fastcall hkPartCode1(void* s, std::uint32_t p) { return PartCodeCommon(1, s, p); }
    static std::uint32_t __fastcall hkPartCode2(void* s, std::uint32_t p) { return PartCodeCommon(2, s, p); }
    static std::uint32_t __fastcall hkPartCode3(void* s, std::uint32_t p) { return PartCodeCommon(3, s, p); }
    static std::uint32_t __fastcall hkPartCode4(void* s, std::uint32_t p) { return PartCodeCommon(4, s, p); }
    static void* const kPartCodeDetours[5] = {
        reinterpret_cast<void*>(&hkPartCode0), reinterpret_cast<void*>(&hkPartCode1),
        reinterpret_cast<void*>(&hkPartCode2), reinterpret_cast<void*>(&hkPartCode3),
        reinterpret_cast<void*>(&hkPartCode4) };

    static void DumpDescriptorSEH(void* self, std::uint32_t key)
    {
        __try
        {
            std::uint8_t* a = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(self) + 0x120);
            if (!a) return;
            std::uint8_t* table = *reinterpret_cast<std::uint8_t**>(a + 0x28);
            if (!table) return;
            const unsigned hw = key >> 16;
            std::uint8_t* d = table + static_cast<size_t>(hw) * 0xe;
            LogDebug("[WeaponKey] desc key=0x%X hw=%u @%p: %02X %02X %02X %02X %02X %02X %02X "
                "%02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X (block=%u tmplIdx=%u)\n",
                key, hw, d,
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15], d[16], d[17],
                static_cast<unsigned>(*reinterpret_cast<std::uint16_t*>(d)), d[8]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static std::uint32_t __fastcall hkResolver20(void* self, std::uint32_t key)
    {
        const std::uint32_t equipId = g_OrigResolver20(self, key);
        std::uint32_t vanilla = equipId;
        if (g_InSetupWeaponInfo)
        {
            vanilla = MapEquipId(equipId);
            if (vanilla != equipId)
            {
                g_TlsCustomEquip = equipId;
                g_TlsVanillaEquip = vanilla;
            }
        }
#ifdef _DEBUG
        if (g_InSetupWeaponInfo && key != 0)
        {
            bool isNew;
            {
                std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                isNew = g_WeaponKeysLogged.insert(key).second;
            }
            if (isNew)
            {
                const unsigned eqpType = key & 0x1f;
                if (vanilla != equipId)
                    LogDebug("[WeaponKey] KEY=%u (0x%X) eqpType=%u -> equipId=%u  FAMILY-FROM "
                        "equipId=%u (template+part codes only; identity stays %u)\n",
                        key, key, eqpType, equipId, vanilla, equipId);
                else
                    LogDebug("[WeaponKey] KEY=%u (0x%X) eqpType=%u -> equipId=%u\n",
                        key, key, eqpType, equipId);
                DumpDescriptorSEH(self, key);
            }
        }
#endif
        return equipId;
    }

    static char __fastcall hkResolver40(void* self, std::uint32_t key)
    {
        return g_OrigResolver40(self, key);
    }

    static void* CallTemplateFinderSEH(void* self, std::uint32_t vanillaEquipId)
    {
        __try
        {
            return g_Finder148 ? g_Finder148(self, vanillaEquipId) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static int FixDescTemplateIndexSEH(void* self, std::uint32_t key, void* own)
    {
        __try
        {
            std::uint8_t* a = static_cast<std::uint8_t*>(self);
            std::uint8_t* pool = *reinterpret_cast<std::uint8_t**>(a + 0x1d8);
            if (!pool)
                return -1;
            std::uint8_t* op = static_cast<std::uint8_t*>(own);
            if (op < pool)
                return -1;
            const size_t diff = static_cast<size_t>(op - pool);
            if (diff % 0x90 != 0)
                return -1;
            const size_t idx = diff / 0x90;
            if (idx >= 48)
                return -1;
            std::uint8_t* t = *reinterpret_cast<std::uint8_t**>(a + 0x120);
            if (!t)
                return -1;
            std::uint8_t* table = *reinterpret_cast<std::uint8_t**>(t + 0x28);
            if (!table)
                return -1;
            std::uint8_t* d = table + static_cast<size_t>(key >> 16) * 0xe;
            if (d[8] == static_cast<std::uint8_t>(idx))
                return 0;
            d[8] = static_cast<std::uint8_t>(idx);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    static void* __fastcall hkResolver150(void* self, std::uint32_t key)
    {
        if (g_InSetupWeaponInfo && g_HasSwaps.load(std::memory_order_relaxed))
        {
            void* own = g_OrigResolver150(self, key);
            if (g_TlsVanillaEquip == 0)
            {
                if (own && g_OrigResolver20)
                {
                    const std::uint32_t equipId = g_OrigResolver20(self, key);
                    if (equipId != 0)
                    {
                        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                        g_TemplateByEquipId[equipId] = own;
                    }
                }
                return own;
            }
            const std::uint32_t vanilla = g_TlsVanillaEquip;
            if (own)
            {
                std::uint8_t cur = 0;
                std::uint16_t tag = 0;
                std::uint8_t* op = static_cast<std::uint8_t*>(own);
                ReadU16SEH(op + 0x5e, tag);
                std::uint16_t cur16 = 0;
                ReadU16SEH(op + 0x78, cur16);
                cur = static_cast<std::uint8_t>(cur16 & 0xFF);
                const std::uint32_t vanType = GetEquipTypeForEquipId(vanilla) & 0x1f;
                bool patched = false;
                if (vanType >= 1 && vanType <= 8 && (cur & 0x1f) != vanType)
                    patched = WriteU8SEH(op + 0x78,
                        static_cast<std::uint8_t>((cur & ~0x1f) | vanType)) == 1;
                bool isNew;
                {
                    std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                    isNew = g_FamilyLogged.insert(g_TlsCustomEquip).second;
                }
                const bool failed = !patched && (cur & 0x1f) != vanType;
                if (isNew && failed)
                    Log("[WeaponKey] WARNING: template family patch failed for "
                        "equipId=%u (eqpType %u -> %u) - the weapon may handle as "
                        "the wrong family\n",
                        g_TlsCustomEquip, cur & 0x1f, vanType);
#ifdef _DEBUG
                if (isNew && !failed)
                    LogDebug("[WeaponKey] template equipId=%u familyFrom=%u: own block @%p "
                        "tag=0x%X eqpType %u -> %u%s\n",
                        g_TlsCustomEquip, vanilla, own, tag, cur & 0x1f, vanType,
                        patched ? " (patched)" : " (already matches)");
#endif
                const int descFix = FixDescTemplateIndexSEH(self, key, own);
#ifdef _DEBUG
                if (descFix == 1)
                    LogDebug("[WeaponKey] descriptor tmplIdx corrected for equipId=%u "
                        "(casing eject restored)\n", g_TlsCustomEquip);
#else
                (void)descFix;
#endif
            }
            return own;
        }
        return g_OrigResolver150(self, key);
    }

    static int ReadU32AtSEH(const void* base, size_t off, std::uint32_t& out)
    {
        __try
        {
            out = *reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(base) + off);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int CopyBytesSEH(const void* src, std::uint8_t* dst, size_t n)
    {
        __try
        {
            std::memcpy(dst, src, n);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static void DumpWeaponInfo(void* work, std::uint32_t key, std::uint32_t equipIdFromKey)
    {
        std::uint8_t w[0xF0];
        if (CopyBytesSEH(static_cast<std::uint8_t*>(work) + 0x1b0, w, sizeof(w)) != 1)
            return;
        char line[176];
        for (int row = 0; row < 5; ++row)
        {
            int pos = 0;
            for (int i = 0; i < 0x30; ++i)
                pos += std::snprintf(line + pos, sizeof(line) - pos, "%02X ",
                                     w[row * 0x30 + i]);
            LogDebug("[WeaponKey] winfo eq=%u key=0x%X +0x%02X: %s\n",
                equipIdFromKey, key, row * 0x30, line);
        }
    }

    static void* ReadPtrAtSEH(void* base, size_t off)
    {
        __try
        {
            return *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(base) + off);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* VtblSlotSEH(void* obj, size_t byteOff)
    {
        __try
        {
            void** vtable = *reinterpret_cast<void***>(obj);
            if (!vtable)
                return nullptr;
            return vtable[byteOff / sizeof(void*)];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* ReadVtableSlotSEH(void* self, size_t byteOff)
    {
        void* resolver = ReadPtrAtSEH(self, 0x60);
        return resolver ? VtblSlotSEH(resolver, byteOff) : nullptr;
    }

    static void EnsureResolverMethodHook(void* self)
    {
        bool expected = false;
        if (!g_ResolverHookTried.compare_exchange_strong(expected, true))
            return;

        void* m20  = ReadVtableSlotSEH(self, 0x20);
        void* m40  = ReadVtableSlotSEH(self, 0x40);
        void* m150 = ReadVtableSlotSEH(self, 0x150);
        g_Finder148 = reinterpret_cast<Resolver150_t>(ReadVtableSlotSEH(self, 0x148));
        if (!m20)
        {
            LogDebug("[WeaponKey] could not resolve family-resolver methods - key log/swap off\n");
            return;
        }
        bool ok20 = CreateAndEnableHook(m20, &hkResolver20,
                                        reinterpret_cast<void**>(&g_OrigResolver20));
        if (ok20) g_Resolver20Addr = m20;
        bool ok40 = false, ok150 = false;
        if (m40)
        {
            ok40 = CreateAndEnableHook(m40, &hkResolver40,
                                       reinterpret_cast<void**>(&g_OrigResolver40));
            if (ok40) g_Resolver40Addr = m40;
        }
        if (m150)
        {
            ok150 = CreateAndEnableHook(m150, &hkResolver150,
                                        reinterpret_cast<void**>(&g_OrigResolver150));
            if (ok150) g_Resolver150Addr = m150;
        }
        if (!ok20 || !ok40 || !ok150)
            Log("[WeaponKey] WARNING: resolver hooks incomplete: block(+0x20)@%p=%s "
                "validity(+0x40)@%p=%s template(+0x150)@%p=%s\n",
                m20, ok20 ? "OK" : "FAIL", m40, ok40 ? "OK" : "FAIL",
                m150, ok150 ? "OK" : "FAIL");
#ifdef _DEBUG
        else
            LogDebug("[WeaponKey] resolver hooks: block(+0x20)@%p=OK validity(+0x40)@%p=OK "
                "template(+0x150)@%p=OK\n", m20, m40, m150);
#endif

        void* rA = ReadPtrAtSEH(self, 0x60);
        void* rB = rA ? ReadPtrAtSEH(rA, 0x18) : nullptr;
        g_GunInfoAccessor = rA;

#ifdef _DEBUG
        void* pool = rA ? ReadPtrAtSEH(rA, 0x1d8) : nullptr;
        if (pool)
        {
            char tag[176];
            for (int row = 0; row < 4; ++row)
            {
                int pos = 0;
                for (int i = 0; i < 12; ++i)
                {
                    const int e = row * 12 + i;
                    std::uint16_t v5e = 0xFFFF;
                    std::uint8_t* ep = static_cast<std::uint8_t*>(pool)
                        + static_cast<size_t>(e) * 0x90;
                    ReadU16SEH(ep + 0x5e, v5e);
                    std::uint16_t v78 = 0xFF;
                    ReadU16SEH(ep + 0x78, v78);
                    pos += std::snprintf(tag + pos, sizeof(tag) - pos, "%d:%X/%u ",
                                         e, v5e, v78 & 0xFF);
                }
                LogDebug("[WeaponKey] template pool tags (+0x5e/+0x78): %s\n", tag);
            }
        }
#endif

        if (!rB)
        {
            LogDebug("[WeaponKey] part-code resolver (B) not reachable - familyFrom covers "
                "template only\n");
            return;
        }
        g_ResolverB = rB;
        void* rtHookTarget = ResolveGameAddress(gAddr.MotionLoaderImpl_GetReceiverType);
        void* ubHookTarget = ResolveGameAddress(gAddr.MotionLoaderImpl_GetUnderBarrelType);
        void* brHookTarget = ResolveGameAddress(gAddr.MotionLoaderImpl_GetBarrelType);
        void* mgHookTarget = ResolveGameAddress(gAddr.MotionLoaderImpl_GetMagazineType);
        void* stHookTarget = ResolveGameAddress(gAddr.MotionLoaderImpl_GetSightType);
        for (int k = 0; k < 5; ++k)
        {
            void* mB = VtblSlotSEH(rB, kPartVtblOff[k]);
            int cs = -9, es = -9;
            const char* note = "";
            if (!mB)
                note = "null";
            else if (mB == m20 || mB == m40 || mB == m150)
                note = "=resolverA";
            else if (mB == rtHookTarget)
                note = "=GetReceiverType";
            else if (ubHookTarget && mB == ubHookTarget)
                note = "=GetUnderBarrelType";
            else if (brHookTarget && mB == brHookTarget)
                note = "=GetBarrelType";
            else if (mgHookTarget && mB == mgHookTarget)
                note = "=GetMagazineType";
            else if (stHookTarget && mB == stHookTarget)
                note = "=GetSightType";
            else
            {
                int prev = -1;
                for (int j = 0; j < k; ++j)
                    if (g_PartCodeAddr[j] == mB)
                    {
                        prev = j;
                        break;
                    }
                if (prev >= 0)
                {
                    g_OrigPartCode[k] = g_OrigPartCode[prev];
                    g_PartCodeExact[k] = false;
                    g_PartCodeExact[prev] = false;
                    note = "shared";
                }
                else
                {
                    cs = MH_CreateHook(mB, kPartCodeDetours[k],
                                       reinterpret_cast<void**>(&g_OrigPartCode[k]));
                    es = (cs == MH_OK) ? MH_EnableHook(mB) : -9;
                    if (cs == MH_OK && (es == MH_OK || es == MH_ERROR_ENABLED))
                    {
                        g_PartCodeAddr[k] = mB;
                        g_PartCodeExact[k] = true;
                    }
                }
            }
#ifdef _DEBUG
            LogDebug("[WeaponKey] part-code slot%d (+0x%zX) method=%p cs=%d es=%d %s\n",
                k, kPartVtblOff[k], mB, cs, es, note);
#else
            if (mB && cs != -9 && (cs != MH_OK || (es != MH_OK && es != MH_ERROR_ENABLED)))
                Log("[WeaponKey] WARNING: part-code slot%d hook failed (cs=%d es=%d) - "
                    "donor part substitution incomplete for this slot\n", k, cs, es);
#endif
        }
    }

    unsigned int PartCode_TapPartId(int slot, unsigned int partId)
    {
        g_PartCallCount.fetch_add(1, std::memory_order_relaxed);
        if (!g_InSetupWeaponInfo || g_TlsVanillaEquip == 0)
            return partId;
        FamilyGb van;
        if (!GetGbForEquipId(g_TlsVanillaEquip, van))
        {
            static unsigned long long lastMs = 0;
            const unsigned long long now = GetTickCount64();
            if (now - lastMs > 2000)
            {
                lastMs = now;
                Log("[WeaponKey] WARNING: donor equipId=%u gunBasic unresolved at "
                    "draw time - part substitution skipped (slot %d); the weapon "
                    "may not animate correctly\n", g_TlsVanillaEquip, slot);
            }
            return partId;
        }
        unsigned int use = van.gb[kPartGbByte[slot]];
        if ((slot == 3 || slot == 4) && use != 0)
        {
            FamilyGb cus;
            if (GetGbForEquipId(g_TlsCustomEquip, cus)
                && cus.gb[kPartGbByte[slot]] == 0)
                use = partId;
        }
#ifdef _DEBUG
        if (use != partId)
        {
            static std::atomic<int> logged{ 0 };
            const int bit = 1 << slot;
            if (!(logged.fetch_or(bit, std::memory_order_relaxed) & bit))
                LogDebug("[WeaponKey] part-code tap slot%d: %u -> donor %u "
                    "(served through the EquipParam type hook)\n",
                    slot, partId, use);
        }
#endif
        return use;
    }

    unsigned int PartCode_TapReceiverType(unsigned int receiverId, unsigned int result)
    {
        if (g_InSetupWeaponInfo && g_TlsVanillaEquip != 0)
        {
            FamilyGb van;
            if (GetGbForEquipId(g_TlsVanillaEquip, van) && van.gb[0] != 0)
            {
                FamilyGb cus;
                const bool isCustomRc =
                    GetGbForEquipId(g_TlsCustomEquip, cus) && receiverId == cus.gb[0];
                if (isCustomRc)
                {
                    unsigned int donorType = 0;
                    if (g_RcvTypeExtReady && van.gb[0] < 240)
                        donorType = g_RcvTypeExt[van.gb[0]];
                    const unsigned int finalType = result != 0 ? result : donorType;
                    static std::atomic<std::uint32_t> lastSig{ 0xFFFFFFFF };
                    const std::uint32_t sig =
                        (receiverId << 16) | ((result & 0xFF) << 8) | (finalType & 0xFF);
                    if (lastSig.exchange(sig, std::memory_order_relaxed) != sig)
                    {
                        if (finalType == 0)
                            Log("[WeaponKey] WARNING: rc=%u has NO motion type and "
                                "donor rc=%u has none either - the engine will INVALIDATE "
                                "this weapon; point motionFrom at a receiver with a real "
                                "motion type\n", receiverId, van.gb[0]);
#ifdef _DEBUG
                        else
                            LogDebug("[WeaponKey] receiverType tap: rc=%u motionFrom type=%u, "
                                "donor rc=%u type=%u -> using %u\n",
                                receiverId, result, van.gb[0], donorType, finalType);
#endif
                    }
                    return finalType;
                }
                if (receiverId >= 234)
                {
                    static std::atomic<std::uint32_t> lastMiss{ 0xFFFFFFFF };
                    if (lastMiss.exchange(receiverId, std::memory_order_relaxed) != receiverId)
                        Log("[WeaponKey] WARNING: custom-range rc=%u queried during "
                            "mapped setup but does not match the mapped weapon's receiver - "
                            "no donor substitution applied\n", receiverId);
                }
            }
        }
        return result;
    }

    static void __fastcall hkSetupWeaponInfo(void* self, void* work, int slot)
    {
        outfit::EndBootRestoreScrub("first SetupWeaponInfo - player is live");

        if (self)
            EnsureResolverMethodHook(self);

        const std::uint32_t callsBefore =
            g_PartCallCount.load(std::memory_order_relaxed);
        KeyTypeSwap keySwap;
        ApplyKeyTypeSwaps(self, slot, keySwap);
        g_TlsCustomEquip = 0;
        g_TlsVanillaEquip = 0;
        g_InSetupWeaponInfo = true;
        g_OrigSetupWeaponInfo(self, work, slot);
        g_InSetupWeaponInfo = false;
        RestoreKeyWordsSEH(&keySwap);
        const std::uint32_t partCalls =
            g_PartCallCount.load(std::memory_order_relaxed) - callsBefore;
        (void)partCalls;
        const std::uint32_t mappedThis = g_TlsCustomEquip;
        g_TlsCustomEquip = 0;
        g_TlsVanillaEquip = 0;

        if (work)
        {
            std::uint8_t* wim = static_cast<std::uint8_t*>(work) + 0x1b0;
            std::uint32_t mintKey = 0;
            if (ReadU32AtSEH(wim, 0x90, mintKey) == 1 && mintKey != 0
                && ReadByteAtSEH(wim, 0x7a) == 0)
            {
                const std::uint32_t mintEq = (mintKey & 0xFFFF) >> 5;
                FamilyGb gbRow;
                if (GetGbForEquipId(mintEq, gbRow) && gbRow.gb[0] != 0)
                {
                    const int rc = gbRow.gb[0];
                    int rowVal = rc;
                    if (rc > 233)
                    {
                        const auto it = g_ReceiverMotionDonor.find(rc);
                        rowVal = (it != g_ReceiverMotionDonor.end()) ? it->second : 0;
                    }
                    if (rowVal > 0 && rowVal <= 233
                        && WriteByteAtSEH(wim, 0x7a, static_cast<std::uint8_t>(rowVal)) == 1)
                    {
                        bool logIt = false;
                        {
                            std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                            logIt = g_PartLogged.insert(0x40000000u | mintEq).second;
                        }
                        if (logIt)
                            LogDebug("[ChimeraMotion] minted weapon info for eq=%u carried no "
                                "part-motion row (wi+0x7a=0; the hw=0 template mint skips the "
                                "per-hw fill) - stamped row=%d from receiver %d so the bolt/"
                                "slide transform binds.\n",
                                mintEq, rowVal, rc);
                    }
                }
            }
        }

        if (work && g_HasSwaps.load(std::memory_order_relaxed))
        {
            std::uint8_t* wi = static_cast<std::uint8_t*>(work) + 0x1b0;
            std::uint32_t key = 0;
            const int keyOk = ReadU32AtSEH(wi, 0x90, key);
            if (keyOk == 1 && key == 0)
            {
                std::uint16_t subId = 0;
                ReadU16SEH(wi + 0x70, subId);
                if (subId != 0 || mappedThis != 0)
                {
                    static unsigned long long lastMs = 0;
                    const unsigned long long now = GetTickCount64();
                    if (now - lastMs > 1500)
                    {
                        lastMs = now;
                        Log("[WeaponKey] WARNING: the engine invalidated a custom "
                            "weapon's setup (slot=%d subId=%u equipId=%u) - the weapon "
                            "will not function; check that motionFrom points at a real "
                            "gun's receiver and familyFrom at a usable donor\n",
                            slot, subId, mappedThis);
                    }
                }
            }
#ifdef _DEBUG
            else if (keyOk == 1)
            {
                const std::uint32_t eq = (key & 0xFFFF) >> 5;
                if (IsFamilyMappedEither(eq))
                {
                    std::uint8_t b[16] = {};
                    std::uint32_t cc = 0, d0 = 0;
                    CopyBytesSEH(wi + 0xb0, b, 16);
                    ReadU32AtSEH(wi, 0xcc, cc);
                    ReadU32AtSEH(wi, 0xd0, d0);
                    std::uint16_t t78 = 0;
                    ReadU16SEH(wi + 0x78, t78);
                    LogDebug("[WeaponKey] POST-SETUP eq=%u VALID: eqpType78=%u b9..bd="
                        "%02X %02X %02X %02X %02X  cc=%08X d0=%08X partCalls=%u\n",
                        eq, t78 & 0x1f, b[9], b[10], b[11], b[12], b[13], cc, d0,
                        partCalls);
                    bool doDump = false;
                    {
                        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
                        doDump = g_WeaponInfoDumped.insert(eq).second;
                    }
                    if (doDump)
                        DumpWeaponInfo(work, key, eq);
                }
            }
#endif
        }
    }
}

void EquipParam_SetWeaponHandling(std::uint32_t equipId, std::uint32_t familyFrom)
{
    if (equipId == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
        g_GbByEquipId.clear();
        g_FamilyLogged.erase(equipId);
    }
    if (familyFrom == 0 || familyFrom == equipId)
    {
        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
        g_FamilyFrom.erase(equipId);
        g_HasSwaps.store(!g_FamilyFrom.empty(), std::memory_order_relaxed);
        LogDebug("[WeaponKey] SetWeaponHandling: mapping for equipId=%u cleared\n", equipId);
        return;
    }
    FamilyGb van, cus;
    const bool vanOk = GetGbForEquipId(familyFrom, van);
    const bool cusOk = GetGbForEquipId(equipId, cus);
    const std::uint32_t vanType = GetEquipTypeForEquipId(familyFrom) & 0x1f;
    if (!vanOk || vanType < 1 || vanType > 8)
    {
        LogDebug("[WeaponKey] SetWeaponHandling REFUSED: familyFrom=%u does not resolve to a "
            "native weapon (subId=%u eqpType=%u). Vanilla EQP_WP_* Lua constants are NOT "
            "equip-table ids - draw the donor weapon once and use the number from its "
            "'[WeaponKey] ... equipId=N' log line. Mapping NOT registered so the weapon "
            "keeps its previous behavior.\n", familyFrom, van.subId, vanType);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_WeaponKeyMutex);
        g_FamilyFrom[equipId] = familyFrom;
        g_HasSwaps.store(true, std::memory_order_relaxed);
    }
    int donorMotionType = -1;
    EnsureRcvTypeExt();
    if (g_RcvTypeExtReady && van.gb[0] < 240)
        donorMotionType = g_RcvTypeExt[van.gb[0]];
    LogDebug("[WeaponKey] SetWeaponHandling: equipId=%u borrows the animation family of "
        "vanilla equipId=%u (eqpType=%u subId=%u rc=%u motionType=%d ba=%u am=%u "
        "sk=%u ub=%u; custom subId=%u %s)\n",
        equipId, familyFrom, vanType, van.subId, van.gb[0], donorMotionType,
        van.gb[1], van.gb[2], van.gb[3], van.gb[10], cus.subId,
        cusOk ? "ok" : "resolves at draw");
}

bool Install_WeaponKeyLog()
{
#ifdef _DEBUG
    LogDebug("[WeaponKey] diagnostics build " __DATE__ " " __TIME__ "\n");
#endif
    const uintptr_t addr = SetupWeaponInfoAddr();
    if (!addr)
    {
        LogDebug("[WeaponKey] SetupWeaponInfo not pinned for this build - key log skipped\n");
        return true;
    }
    void* target = ResolveGameAddress(addr);
    const bool ok = CreateAndEnableHook(
        target, &hkSetupWeaponInfo,
        reinterpret_cast<void**>(&g_OrigSetupWeaponInfo));
    if (!ok)
        Log("[WeaponKey] SetupWeaponInfo key-log Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[WeaponKey] SetupWeaponInfo key-log Install -> OK (target=%p)\n", target);
#endif

    if (::AddressSetRuntime::IsEn154Family(gGameBuild))
    {
        g_MotionEntryAAddr = ResolveGameAddress(gAddr.Equip_MotionEntrySlotHookA);
        const bool aOk = CreateAndEnableHook(
            g_MotionEntryAAddr, &hkMotionEntryA,
            reinterpret_cast<void**>(&g_OrigMotionEntryA));
        if (!aOk) g_MotionEntryAAddr = nullptr;

        g_MotionEntryBAddr = ResolveGameAddress(gAddr.Equip_MotionEntrySlotHookB);
        const bool bOk = CreateAndEnableHook(
            g_MotionEntryBAddr, &hkMotionEntryB,
            reinterpret_cast<void**>(&g_OrigMotionEntryB));
        if (!bOk) g_MotionEntryBAddr = nullptr;

        Log("[WeaponKey] motion-entry slot tracking A=%s B=%s\n",
            aOk ? "armed" : "FAILED", bOk ? "armed" : "FAILED");
    }

    const uintptr_t gcsAddr = GetControlStringAddr();
    if (gcsAddr)
    {
        g_GetControlStringAddr = ResolveGameAddress(gcsAddr);
        const bool gcsOk = CreateAndEnableHook(
            g_GetControlStringAddr, &hkGetControlString,
            reinterpret_cast<void**>(&g_OrigGetControlString));
        Log("[WeaponKey] unbound animation-port report %s (GetControlString=%p; reports a blend "
            "layer that was never bound to a clip - almost always a part id past the end of one "
            "of MotionLoaderImpl's type tables)\n",
            gcsOk ? "armed" : "FAILED", g_GetControlStringAddr);
        if (!gcsOk)
            g_GetControlStringAddr = nullptr;
    }

    const uintptr_t svAddr = GetStringValueByIndexAddr();
    if (svAddr)
    {
        g_GetStringValueByIndexAddr = ResolveGameAddress(svAddr);
        const bool svOk = CreateAndEnableHook(
            g_GetStringValueByIndexAddr, &hkGetStringValueByIndex,
            reinterpret_cast<void**>(&g_OrigGetStringValueByIndex));
        Log("[WeaponKey] string-value probe %s (GetStringValueByIndex=%p; logs the control-port "
            "index and the binder's array bases so the malformed value can be located exactly)\n",
            svOk ? "armed" : "FAILED", g_GetStringValueByIndexAddr);
        if (!svOk)
            g_GetStringValueByIndexAddr = nullptr;
    }

    const uintptr_t realizeAddr = RealizedEquipRealizeAddr();
    if (realizeAddr)
    {
        g_RealizeAddr = ResolveGameAddress(realizeAddr);
        const bool rOk = CreateAndEnableHook(
            g_RealizeAddr, &hkRealizedEquipRealize,
            reinterpret_cast<void**>(&g_OrigRealizedEquipRealize));
        Log("[WeaponKey] weapon-motion entry alias %s (RealizedEquipObjectImpl::Realize=%p; "
            "custom weapon equipIds get their empty per-equipId motion-mtar entry aliased to "
            "the familyFrom donor - or the vanilla owner of a shared subId - so Realize builds "
            "the real anim control: clip-driven bolt/parts animation like vanilla weapons)\n",
            rOk ? "armed" : "FAILED", g_RealizeAddr);
        if (!rOk)
            g_RealizeAddr = nullptr;
    }

#ifdef _DEBUG
    const uintptr_t ganiFixAddr = GetAnimFileFromGaniPathAddr();
    if (ganiFixAddr)
    {
        g_GetAnimFromGaniAddr = ResolveGameAddress(ganiFixAddr);
        const bool ganiOk = CreateAndEnableHook(
            g_GetAnimFromGaniAddr, &hkGetAnimFromGani,
            reinterpret_cast<void**>(&g_OrigGetAnimFromGani));
        Log("[WeaponKey] gani clip tracer %s (GetAnimFileFromGaniPath=%p; logs which animation "
            "clips resolve and which miss)\n",
            ganiOk ? "armed" : "FAILED", g_GetAnimFromGaniAddr);
        if (!ganiOk)
            g_GetAnimFromGaniAddr = nullptr;
    }

    const uintptr_t ppmAddr = PostPutMotionAddr();
    if (ppmAddr)
    {
        g_PostPutMotionAddr = ResolveGameAddress(ppmAddr);
        const bool pOk = CreateAndEnableHook(
            g_PostPutMotionAddr, &hkPostPutMotion,
            reinterpret_cast<void**>(&g_OrigPostPutMotion));
        Log("[WeaponKey] reader probe %s (PostPutMotionExecute=%p; diffs each record's "
            "request bit and state words across the reader call - a BOLT PASS line proves the "
            "per-shot request reached AND was consumed for that weapon, with no extra hooks "
            "on the unknown-arity apply functions)\n",
            pOk ? "armed" : "FAILED", g_PostPutMotionAddr);
        if (!pOk)
            g_PostPutMotionAddr = nullptr;
    }
#endif

#ifdef _DEBUG
    const uintptr_t fsmAddr = AttackFsmStateChangeAddr();
    if (fsmAddr)
    {
        g_ExecStateChangeAddr = ResolveGameAddress(fsmAddr);
        const bool fsmOk = CreateAndEnableHook(
            g_ExecStateChangeAddr, &hkExecStateChange,
            reinterpret_cast<void**>(&g_OrigExecStateChange));
        Log("[WeaponKey] attack-FSM tracer %s (ExecStateChangeImpl=%p)\n",
            fsmOk ? "armed" : "FAILED", g_ExecStateChangeAddr);
        if (!fsmOk)
            g_ExecStateChangeAddr = nullptr;
    }
    const uintptr_t missAddr = AnimSearchMotionDataAddr();
    if (missAddr)
    {
        g_SearchMotionDataAddr = ResolveGameAddress(missAddr);
        const bool missOk = CreateAndEnableHook(
            g_SearchMotionDataAddr, &hkSearchMotionData,
            reinterpret_cast<void**>(&g_OrigSearchMotionData));
        Log("[WeaponKey] motion-miss logger %s (SearchMotionData=%p)\n",
            missOk ? "armed" : "FAILED", g_SearchMotionDataAddr);
        if (!missOk)
            g_SearchMotionDataAddr = nullptr;
    }
    const uintptr_t ejAddr = EjectCasingAddr();
    if (ejAddr)
    {
        g_EjectCasingAddr = ResolveGameAddress(ejAddr);
        const bool ejOk = CreateAndEnableHook(
            g_EjectCasingAddr, &hkEjectCasing,
            reinterpret_cast<void**>(&g_OrigEjectCasing));
        Log("[WeaponKey] casing-eject probe %s (EjectCasing=%p)\n",
            ejOk ? "armed" : "FAILED", g_EjectCasingAddr);
        if (!ejOk)
            g_EjectCasingAddr = nullptr;
    }
#endif
    return ok;
}

void Uninstall_WeaponKeyLog()
{
#ifdef _DEBUG
    if (g_BoltBoneVtbl)
    {
        DWORD oldProt = 0;
        if (VirtualProtect(&g_BoltBoneVtbl[0x140 / 8], sizeof(void*) * 6, PAGE_READWRITE, &oldProt))
        {
            g_BoltBoneVtbl[0x140 / 8] = reinterpret_cast<void*>(g_OrigBoltBoneIdx);
            g_BoltBoneVtbl[0x168 / 8] = reinterpret_cast<void*>(g_OrigBoltBoneWrite);
            VirtualProtect(&g_BoltBoneVtbl[0x140 / 8], sizeof(void*) * 6, oldProt, &oldProt);
        }
        if (g_OrigPutMotionAll && g_OrigPutMotion &&
            VirtualProtect(&g_BoltBoneVtbl[0x200 / 8], sizeof(void*) * 3, PAGE_READWRITE, &oldProt))
        {
            g_BoltBoneVtbl[0x200 / 8] = reinterpret_cast<void*>(g_OrigPutMotionAll);
            g_BoltBoneVtbl[0x210 / 8] = reinterpret_cast<void*>(g_OrigPutMotion);
            VirtualProtect(&g_BoltBoneVtbl[0x200 / 8], sizeof(void*) * 3, oldProt, &oldProt);
        }
        if (g_OrigSetMotionData && g_SetMotionDataSlot >= 0 &&
            VirtualProtect(&g_BoltBoneVtbl[g_SetMotionDataSlot], sizeof(void*),
                           PAGE_READWRITE, &oldProt))
        {
            g_BoltBoneVtbl[g_SetMotionDataSlot] =
                reinterpret_cast<void*>(g_OrigSetMotionData);
            VirtualProtect(&g_BoltBoneVtbl[g_SetMotionDataSlot], sizeof(void*),
                           oldProt, &oldProt);
        }
        if (g_OrigSetMotionByPath && g_SetMotionByPathSlot >= 0 &&
            VirtualProtect(&g_BoltBoneVtbl[g_SetMotionByPathSlot], sizeof(void*),
                           PAGE_READWRITE, &oldProt))
        {
            g_BoltBoneVtbl[g_SetMotionByPathSlot] =
                reinterpret_cast<void*>(g_OrigSetMotionByPath);
            VirtualProtect(&g_BoltBoneVtbl[g_SetMotionByPathSlot], sizeof(void*),
                           oldProt, &oldProt);
        }
        g_BoltBoneVtbl = nullptr;
    }
    if (g_ChimVtbl)
    {
        DWORD oldProt = 0;
        if (VirtualProtect(&g_ChimVtbl[0x170 / 8], sizeof(void*) * 4, PAGE_READWRITE, &oldProt))
        {
            g_ChimVtbl[0x170 / 8] = reinterpret_cast<void*>(g_OrigChimBoneIdx);
            g_ChimVtbl[0x188 / 8] = reinterpret_cast<void*>(g_OrigChimSet);
            VirtualProtect(&g_ChimVtbl[0x170 / 8], sizeof(void*) * 4, oldProt, &oldProt);
        }
        g_ChimVtbl = nullptr;
    }
#endif
    if (g_Resolver20Addr)  { DisableAndRemoveHook(g_Resolver20Addr);  g_Resolver20Addr  = nullptr; }
    if (g_Resolver40Addr)  { DisableAndRemoveHook(g_Resolver40Addr);  g_Resolver40Addr  = nullptr; }
    if (g_Resolver150Addr) { DisableAndRemoveHook(g_Resolver150Addr); g_Resolver150Addr = nullptr; }
    g_OrigResolver20 = nullptr;
    g_OrigResolver40 = nullptr;
    g_OrigResolver150 = nullptr;
    g_Finder148 = nullptr;
    for (int k = 0; k < 5; ++k)
    {
        if (g_PartCodeAddr[k])
            DisableAndRemoveHook(g_PartCodeAddr[k]);
        g_PartCodeAddr[k] = nullptr;
        g_OrigPartCode[k] = nullptr;
        g_PartCodeExact[k] = false;
    }
    g_ResolverB = nullptr;

    const uintptr_t addr = SetupWeaponInfoAddr();
    if (addr && g_OrigSetupWeaponInfo)
        DisableAndRemoveHook(ResolveGameAddress(addr));
    g_OrigSetupWeaponInfo = nullptr;

    if (g_ExecStateChangeAddr)
    {
        DisableAndRemoveHook(g_ExecStateChangeAddr);
        g_ExecStateChangeAddr = nullptr;
    }
    g_OrigExecStateChange = nullptr;

    if (g_SearchMotionDataAddr)
    {
        DisableAndRemoveHook(g_SearchMotionDataAddr);
        g_SearchMotionDataAddr = nullptr;
    }
    g_OrigSearchMotionData = nullptr;

    if (g_MotionEntryAAddr)
    {
        DisableAndRemoveHook(g_MotionEntryAAddr);
        g_MotionEntryAAddr = nullptr;
    }
    g_OrigMotionEntryA = nullptr;

    if (g_MotionEntryBAddr)
    {
        DisableAndRemoveHook(g_MotionEntryBAddr);
        g_MotionEntryBAddr = nullptr;
    }
    g_OrigMotionEntryB = nullptr;

    if (g_GetControlStringAddr)
    {
        DisableAndRemoveHook(g_GetControlStringAddr);
        g_GetControlStringAddr = nullptr;
    }
    g_OrigGetControlString = nullptr;

    if (g_GetStringValueByIndexAddr)
    {
        DisableAndRemoveHook(g_GetStringValueByIndexAddr);
        g_GetStringValueByIndexAddr = nullptr;
    }
    g_OrigGetStringValueByIndex = nullptr;

    if (g_GetAnimFromGaniAddr)
    {
        DisableAndRemoveHook(g_GetAnimFromGaniAddr);
        g_GetAnimFromGaniAddr = nullptr;
    }
    g_OrigGetAnimFromGani = nullptr;

    if (g_EjectCasingAddr)
    {
        DisableAndRemoveHook(g_EjectCasingAddr);
        g_EjectCasingAddr = nullptr;
    }
    g_OrigEjectCasing = nullptr;

    if (g_RealizeAddr)
    {
        DisableAndRemoveHook(g_RealizeAddr);
        g_RealizeAddr = nullptr;
    }
    g_OrigRealizedEquipRealize = nullptr;
}

namespace
{
    using BulletEffectSpawn_t = void(__fastcall*)(void*, unsigned long long, unsigned int,
                                                  void*, unsigned int);
    static BulletEffectSpawn_t g_OrigBulletEffectSpawn = nullptr;

    enum class BulletEffectRange { InRange, OutOfRange, Unverifiable };

    static BulletEffectRange BulletEffectIndexRangeSEH(void* emitter,
                                                       unsigned int index,
                                                       unsigned long long& outCount)
    {
        outCount = 0;
        __try
        {
            const unsigned char* base =
                *reinterpret_cast<const unsigned char* const*>(
                    reinterpret_cast<const unsigned char*>(emitter) + 0x50);
            if (!base)
                return BulletEffectRange::Unverifiable;
            const unsigned long long count =
                *reinterpret_cast<const unsigned long long*>(base - 8);
            outCount = count;
            if (count == 0 || count > 0x1000)
                return BulletEffectRange::Unverifiable;
            return static_cast<unsigned long long>(index) < count
                ? BulletEffectRange::InRange
                : BulletEffectRange::OutOfRange;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return BulletEffectRange::Unverifiable;
        }
    }

    static void __fastcall hkBulletEffectSpawn(void* emitter, unsigned long long a2,
                                               unsigned int index, void* a4, unsigned int a5)
    {
        if (emitter && index != 0)
        {
            unsigned long long count = 0;
            const BulletEffectRange r =
                BulletEffectIndexRangeSEH(emitter, index, count);

            if (r == BulletEffectRange::OutOfRange)
            {
                static std::atomic<bool> s_Warned{ false };
                if (!s_Warned.exchange(true))
                    LogDebug("[EquipParam] bullet trail/muzzle effect index %u is past this "
                        "weapon's effect FilePtr array (count=%llu) - skipping the effect "
                        "instead of reading past the array end (the engine does no bounds "
                        "check; a donor-aliased custom weapon asking for an index its own "
                        "equip row does not cover would otherwise GP-fault)\n",
                        index, count);
                return;
            }

            if (r == BulletEffectRange::Unverifiable)
            {
                static std::atomic<bool> s_Warned{ false };
                if (!s_Warned.exchange(true))
                    LogDebug("[EquipParam] bullet trail/muzzle effect index %u: this emitter's "
                        "array count is not readable or not credible (raw=%llu), so the "
                        "index cannot be proven out of range - letting the effect through. "
                        "If this line is followed by a GP-fault in BulletEffectController, "
                        "the count header is at a different offset on this path.\n",
                        index, count);
            }
        }
        g_OrigBulletEffectSpawn(emitter, a2, index, a4, a5);
    }
}

bool Install_BulletEffectGuard()
{
    void* target = ResolveGameAddress(gAddr.BulletEffectController_CreateEffect);
    if (!target)
        return true;
    const bool ok = CreateAndEnableHook(
        target, &hkBulletEffectSpawn,
        reinterpret_cast<void**>(&g_OrigBulletEffectSpawn));
    if (!ok)
        Log("[EquipParam] BulletEffect index guard Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] BulletEffect index guard Install -> OK (target=%p)\n", target);
#endif
    return ok;
}

void Uninstall_BulletEffectGuard()
{
    if (gAddr.BulletEffectController_CreateEffect && g_OrigBulletEffectSpawn)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.BulletEffectController_CreateEffect));
    g_OrigBulletEffectSpawn = nullptr;
}

bool Install_GunInfoGuard()
{
    void* target = ResolveGameAddress(gAddr.EquipSystem_SetUpGunInfoFromGunPartsDesc);
    if (!target)
        return true;
    const bool ok = CreateAndEnableHook(
        target, &hkSetUpGunInfo,
        reinterpret_cast<void**>(&g_OrigSetUpGunInfo));
    if (!ok)
        Log("[EquipParam] SetUpGunInfo guard Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] SetUpGunInfo guard Install -> OK (target=%p)\n", target);
#endif
    const uintptr_t addMtar = AddMtarBlockPackagePathsAddr();
    if (addMtar)
    {
        g_AddMtarBlockPackagePathsAddr = ResolveGameAddress(addMtar);
        const bool ok2 = CreateAndEnableHook(
            g_AddMtarBlockPackagePathsAddr, &hkAddMtarBlockPackagePaths,
            reinterpret_cast<void**>(&g_OrigAddMtarBlockPackagePaths));
        if (!ok2)
        {
            Log("[EquipParam] AddMtarBlockPackagePaths mount hook Install -> FAIL "
                "(target=%p)\n", g_AddMtarBlockPackagePathsAddr);
            g_AddMtarBlockPackagePathsAddr = nullptr;
        }
        else
        {
            WarmFamilyRootTable();
#ifdef _DEBUG
            LogDebug("[EquipParam] AddMtarBlockPackagePaths mount hook Install -> OK "
                "(target=%p)\n", g_AddMtarBlockPackagePathsAddr);
#endif
        }
    }
    if (::AddressSetRuntime::IsEn154Family(gGameBuild))
    {
        g_ReloadChimeraPartsAddr = ResolveGameAddress(gAddr.Equip_ReloadChimeraPartsInfoTable);
        const bool okC = CreateAndEnableHook(
            g_ReloadChimeraPartsAddr, &hkReloadChimeraPartsInfoTable,
            reinterpret_cast<void**>(&g_OrigReloadChimeraParts));
        if (!okC)
        {
            Log("[EquipParam] ReloadChimeraPartsInfoTable capture Install -> FAIL "
                "(target=%p)\n", g_ReloadChimeraPartsAddr);
            g_ReloadChimeraPartsAddr = nullptr;
        }
#ifdef _DEBUG
        else
            LogDebug("[EquipParam] ReloadChimeraPartsInfoTable capture Install -> OK "
                "(target=%p; pack names harvested at parse time feed the family "
                "mount gate)\n", g_ReloadChimeraPartsAddr);
#endif
        g_GetAnimFileAddr = ResolveGameAddress(gAddr.Mtar_GetAnimFile);
        const bool okG = CreateAndEnableHook(
            g_GetAnimFileAddr, &hkGetAnimFile,
            reinterpret_cast<void**>(&g_OrigGetAnimFile));
        if (!okG)
        {
            Log("[EquipParam] mtar GetAnimFile merge hook Install -> FAIL "
                "(target=%p)\n", g_GetAnimFileAddr);
            g_GetAnimFileAddr = nullptr;
        }
#ifdef _DEBUG
        else
            LogDebug("[EquipParam] mtar GetAnimFile merge hook Install -> OK (target=%p; "
                "a clip missing from a weapon's bound archive is served from any "
                "registered resident family archive that contains it - the lookup-"
                "time equivalent of shipping one merged per-weapon mtar)\n",
                g_GetAnimFileAddr);
#endif
        g_HideMagazineAddr = ResolveGameAddress(gAddr.EquipSystemImpl_HideMagazine);
        const bool okH = CreateAndEnableHook(
            g_HideMagazineAddr, &hkHideMagazine,
            reinterpret_cast<void**>(&g_OrigHideMagazine));
        if (!okH)
        {
            Log("[EquipParam] HideMagazine guard Install -> FAIL (target=%p)\n",
                g_HideMagazineAddr);
            g_HideMagazineAddr = nullptr;
        }
#ifdef _DEBUG
        else
            LogDebug("[EquipParam] HideMagazine guard Install -> OK (target=%p; while a "
                "custom weapon plays a cross-family clip - an under-barrel action - "
                "the engine's request to hide the weapon's OWN magazine is refused, "
                "so the rifle mag no longer vanishes during UB reloads)\n",
                g_HideMagazineAddr);
#endif
    }
    const uintptr_t creator = PartsCtlCreateSlotAddr();
    if (creator)
    {
        g_PartsCtlCreateSlotAddr = ResolveGameAddress(creator);
        const bool ok3 = CreateAndEnableHook(
            g_PartsCtlCreateSlotAddr, &hkPartsCtlCreateSlot,
            reinterpret_cast<void**>(&g_OrigPartsCtlCreateSlot));
        if (!ok3)
        {
            Log("[EquipParam] parts-control remap-create hook Install -> FAIL "
                "(target=%p)\n", g_PartsCtlCreateSlotAddr);
            g_PartsCtlCreateSlotAddr = nullptr;
        }
#ifdef _DEBUG
        else
            LogDebug("[EquipParam] parts-control remap-create hook Install -> OK "
                "(target=%p; custom weapons get the skeleton-remap control variant, "
                "so receiver-family clips authored for the bigger chimera rig bind "
                "per-bone instead of being refused by the track-count guard)\n",
                g_PartsCtlCreateSlotAddr);
#endif
    }
    g_SetMotionByPathRealAddr =
        reinterpret_cast<void*>(ResolveGameAddress(gAddr.SimplePartsControllerImpl_SetMotionDataByPath));
    if (g_SetMotionByPathRealAddr)
    {
        const bool okS = CreateAndEnableHook(
            g_SetMotionByPathRealAddr, &hkSetMotionByPathSafe,
            reinterpret_cast<void**>(&g_OrigSetMotionByPathReal));
        if (!okS)
        {
            Log("[EquipParam] SetMotionDataByPath model-guard detour Install -> FAIL "
                "(target=%p)\n", g_SetMotionByPathRealAddr);
            g_SetMotionByPathRealAddr = nullptr;
        }
#ifdef _DEBUG
        else
            Log("[EquipParam] SetMotionDataByPath model-guard detour Install -> OK "
                "(target=%p; catches DIRECT reader/Realize callers the vtable hook misses - "
                "skips the clip-set when model[slot] is null so a weapon whose part model "
                "failed to load degrades instead of crashing in SetMotionData)\n",
                g_SetMotionByPathRealAddr);
#endif
    }
    InstallPartsSlotModelGuards();
    ArmClipArchiveFallback();
    return ok;
}

void Uninstall_GunInfoGuard()
{
    DisarmClipArchiveFallback();
    RemovePartsSlotModelGuards();
    if (g_SetMotionByPathRealAddr)
    {
        DisableAndRemoveHook(g_SetMotionByPathRealAddr);
        g_SetMotionByPathRealAddr = nullptr;
    }
    g_OrigSetMotionByPathReal = nullptr;
    if (g_PartsCtlCreateSlotAddr)
    {
        DisableAndRemoveHook(g_PartsCtlCreateSlotAddr);
        g_PartsCtlCreateSlotAddr = nullptr;
    }
    g_OrigPartsCtlCreateSlot = nullptr;
    if (gAddr.EquipSystem_SetUpGunInfoFromGunPartsDesc && g_OrigSetUpGunInfo)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.EquipSystem_SetUpGunInfoFromGunPartsDesc));
    g_OrigSetUpGunInfo = nullptr;
    if (g_AddMtarBlockPackagePathsAddr)
    {
        DisableAndRemoveHook(g_AddMtarBlockPackagePathsAddr);
        g_AddMtarBlockPackagePathsAddr = nullptr;
    }
    g_OrigAddMtarBlockPackagePaths = nullptr;
    if (g_ReloadChimeraPartsAddr)
    {
        DisableAndRemoveHook(g_ReloadChimeraPartsAddr);
        g_ReloadChimeraPartsAddr = nullptr;
    }
    g_OrigReloadChimeraParts = nullptr;
    if (g_GetAnimFileAddr)
    {
        DisableAndRemoveHook(g_GetAnimFileAddr);
        g_GetAnimFileAddr = nullptr;
    }
    g_OrigGetAnimFile = nullptr;
    if (g_HideMagazineAddr)
    {
        DisableAndRemoveHook(g_HideMagazineAddr);
        g_HideMagazineAddr = nullptr;
    }
    g_OrigHideMagazine = nullptr;
}

namespace
{

    using DefineFireSound_t = void(__fastcall*)(void*, unsigned int, unsigned __int64,
                                                const char*, char, char, unsigned int*);
    static DefineFireSound_t g_OrigDefineFireSound = nullptr;

    static bool ApplyEventSoundSEH(void* sys, const char* name,
                                   unsigned int* outHashes, bool alsoSuppressed)
    {
        __try
        {
            std::uint8_t* obj =
                *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(sys) + 0x1d8);
            if (!obj || !outHashes)
                return false;
            std::uint8_t* vtbl = *reinterpret_cast<std::uint8_t**>(obj);
            if (!vtbl)
                return false;
            using Hasher_t = unsigned int(__fastcall*)(void*, const char*);
            Hasher_t fn = *reinterpret_cast<Hasher_t*>(vtbl + 0x18);
            if (!fn)
                return false;
            const unsigned int h = fn(obj, name);
            outHashes[0] = h;
            outHashes[2] = h;
            if (alsoSuppressed)
            {
                outHashes[1] = h;
                outHashes[3] = h;
            }
            return true;
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    static bool ApplySupSoundSEH(void* sys, const char* name,
                                 unsigned int* outHashes)
    {
        __try
        {
            std::uint8_t* obj =
                *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(sys) + 0x1d8);
            if (!obj || !outHashes)
                return false;
            std::uint8_t* vtbl = *reinterpret_cast<std::uint8_t**>(obj);
            if (!vtbl)
                return false;
            using Hasher_t = unsigned int(__fastcall*)(void*, const char*);
            Hasher_t fn = *reinterpret_cast<Hasher_t*>(vtbl + 0x18);
            if (!fn)
                return false;
            const unsigned int h = fn(obj, name);
            outHashes[1] = h;
            outHashes[3] = h;
            return true;
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    static bool RootTerminatedWithinSEH(const char* root, size_t maxLen)
    {
        __try
        {
            for (size_t i = 0; i < maxLen; ++i)
                if (root[i] == '\0')
                    return true;
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void __fastcall hkDefineFireSound(void* sys, unsigned int eqpType,
                                             unsigned __int64 r8, const char* root,
                                             char suffixSel, char buildSup,
                                             unsigned int* outHashes)
    {
        FireSoundSpec spec;
        bool found = false;
        if (root)
        {
            const char* base = reinterpret_cast<const char*>(PartCurrentBuf(g_Snd.pb));
            if (base && root >= base)
            {
                const std::uintptr_t off = static_cast<std::uintptr_t>(root - base);
                if ((off & 7) == 0)
                {
                    const int idx = static_cast<int>(off >> 3);
                    std::lock_guard<std::mutex> lock(g_FireSoundMutex);
                    auto it = g_FireSoundByRow.find(idx);
                    if (it != g_FireSoundByRow.end())
                    {
                        spec = it->second;
                        found = true;
                    }
                }
            }
        }

        const char* safeRoot = root;
        if (!root || !RootTerminatedWithinSEH(root, 40))
        {
            safeRoot = "ar00";
            static std::atomic<bool> s_BadRootWarned{ false };
            if (!s_BadRootWarned.exchange(true))
                LogDebug("[EquipParam] DefineWeaponFireSound root is NULL or unterminated "
                    "(eqpType=%u): substituting \"ar00\" so the game's strcat_s cannot "
                    "overrun - the weapon's GunInfo setup faulted and was guarded, so "
                    "its stats/sound are degraded but it no longer crashes\n", eqpType);
        }

        const bool haveSup = found && !spec.supText.empty();

        bool handled = false;
        bool origRan = false;
        if (found && !spec.text.empty())
        {
            if (spec.isEvent)
            {
                if (!haveSup)
                {
                    g_OrigDefineFireSound(sys, eqpType, r8, safeRoot, suffixSel,
                                          buildSup, outHashes);
                    origRan = true;
                }
                handled = ApplyEventSoundSEH(sys, spec.text.c_str(), outHashes,
                                             haveSup);
                if (!handled && origRan)
                    handled = true;
            }
            else
            {
                const unsigned int et = (spec.mSeg >= 0)
                    ? static_cast<unsigned int>(spec.mSeg) : eqpType;
                g_OrigDefineFireSound(sys, et, r8, spec.text.c_str(),
                                      suffixSel, buildSup, outHashes);
                handled = true;
            }
        }
        if (!handled)
            g_OrigDefineFireSound(sys, eqpType, r8, safeRoot, suffixSel, buildSup,
                                  outHashes);
        if (found && !spec.supText.empty())
        {
            std::string supName;
            if (spec.supIsEvent)
                supName = spec.supText;
            else
            {
                supName = "sfx_w_p_";
                supName += spec.supText;
                supName += "_sup";
                supName += suffixSel != 0 ? "_nonact" : "_active";
            }
            ApplySupSoundSEH(sys, supName.c_str(), outHashes);
        }
    }
}

bool Install_FireSoundOverride_Hook()
{
    void* target = ResolveGameAddress(gAddr.WeaponSystem_DefineWeaponFireSound);
    if (!target)
        return true;
    const bool ok = CreateAndEnableHook(
        target, &hkDefineFireSound,
        reinterpret_cast<void**>(&g_OrigDefineFireSound));
    if (!ok)
        Log("[EquipParam] fire-sound override Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] fire-sound override Install -> OK (target=%p)\n", target);
#endif
    return ok;
}

void Uninstall_FireSoundOverride_Hook()
{
    if (gAddr.WeaponSystem_DefineWeaponFireSound && g_OrigDefineFireSound)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.WeaponSystem_DefineWeaponFireSound));
    g_OrigDefineFireSound = nullptr;
    std::lock_guard<std::mutex> lock(g_FireSoundMutex);
    g_FireSoundByRow.clear();
}

bool Install_GetAttackIdGuard()
{
    void* target = ResolveGameAddress(gAddr.EquipParams_GetAttackIdByEquipId);
    if (!target)
        return true;
    const bool ok = CreateAndEnableHook(
        target, &hkGetAttackIdByEquipId,
        reinterpret_cast<void**>(&g_OrigGetAttackId));
    if (!ok)
        Log("[EquipParam] GetAttackIdByEquipId guard Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] GetAttackIdByEquipId guard Install -> OK (target=%p)\n", target);
#endif
    return ok;
}

void Uninstall_GetAttackIdGuard()
{
    if (gAddr.EquipParams_GetAttackIdByEquipId && g_OrigGetAttackId)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.EquipParams_GetAttackIdByEquipId));
    g_OrigGetAttackId = nullptr;
}

namespace
{
    using UpdateLoadoutRequest_t = void(__fastcall*)(void*, std::uint32_t);
    static UpdateLoadoutRequest_t g_OrigUpdateLoadoutRequest = nullptr;


    static thread_local bool t_LoadoutBuildActive = false;

    using GetGunInfoById_t = void*(__fastcall*)(void* self, unsigned int equipId);
    static GetGunInfoById_t g_OrigGetGunInfoById = nullptr;

    static thread_local unsigned char t_GunInfoRing[8][0x90];
    static thread_local int t_GunInfoRingIdx = 0;

    static void* BuildLoadoutGunInfoOnMissSEH(void* self, unsigned int equipId)
    {
        unsigned char* buf = t_GunInfoRing[t_GunInfoRingIdx & 7];
        t_GunInfoRingIdx = (t_GunInfoRingIdx + 1) & 7;
        __try
        {
            std::memset(buf, 0, 0x90);
            *reinterpret_cast<std::uint16_t*>(buf + 0x5e) =
                static_cast<std::uint16_t>(equipId);
            void** vt = *reinterpret_cast<void***>(self);
            using BuildGunInfo_t = void(__fastcall*)(void*, unsigned int, void*);
            BuildGunInfo_t builder =
                reinterpret_cast<BuildGunInfo_t>(vt[(0x158 + 0xF0) / 8]);
            builder(self, equipId, buf);
            return buf;
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            std::memset(buf, 0, 0x90);
            *reinterpret_cast<std::uint16_t*>(buf + 0x5e) =
                static_cast<std::uint16_t>(equipId);
            return buf;
        }
    }

    static void* __fastcall hkGetGunInfoById(void* self, unsigned int equipId)
    {
        void* r = g_OrigGetGunInfoById(self, equipId);
        if (r != nullptr)
            return r;
        if (!t_LoadoutBuildActive || self == nullptr)
            return nullptr;
        return BuildLoadoutGunInfoOnMissSEH(self, equipId);
    }

    static bool WeaponEquipIdIsPhantom(std::int32_t equipId)
    {
   
        if (equipId == 0)
            return false;
        const std::int32_t idx = EquipIdCompression::ComputeCompressed(equipId);
        if (!EquipIdCompression::IsCompressedInBounds(idx))
            return false;
        std::uint16_t* tw = static_cast<std::uint16_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
        if (!tw)
            return false;
        return tw[idx] == 0;
    }

    static void BlankPhantomLoadoutWeaponsSEH(void* self, std::uint32_t slot,
                                              std::int32_t* outPinned,
                                              int* outPinnedCount)
    {
        __try
        {
            std::uint8_t* self8 = static_cast<std::uint8_t*>(self);
            std::uint8_t* loadoutSubsys = *reinterpret_cast<std::uint8_t**>(self8 + 0x1a0);
            if (!loadoutSubsys) return;
            std::uint8_t* l138 = *reinterpret_cast<std::uint8_t**>(loadoutSubsys + 0x138);
            if (!l138) return;
            std::uint8_t* sourceBase = *reinterpret_cast<std::uint8_t**>(l138 + 0x98);
            if (!sourceBase) return;
            std::uint8_t* sourceReq = sourceBase + static_cast<std::size_t>(slot) * 0xa0;

            std::uint8_t* workingReq = nullptr;
            std::uint8_t* workingBase = *reinterpret_cast<std::uint8_t**>(self8 + 0x1e0);
            if (workingBase)
            {
                const std::int32_t bias = *reinterpret_cast<std::int32_t*>(loadoutSubsys + 0xc);
                workingReq = workingBase
                    + static_cast<std::size_t>(static_cast<std::int32_t>(slot) - bias) * 0xb0;
            }

            for (int i = 0; i < 3; ++i)
            {
                std::int32_t* srcW =
                    reinterpret_cast<std::int32_t*>(sourceReq + static_cast<std::size_t>(i) * 0x18);
                const std::int32_t id = *srcW;
                if (!WeaponEquipIdIsPhantom(id))
                {
                    if (id > 0 && id < 0x289 && *outPinnedCount < 3)
                        outPinned[(*outPinnedCount)++] = id;
                    continue;
                }
                *srcW = 0;   // EQP_None
                if (workingReq)
                    *reinterpret_cast<std::int32_t*>(
                        workingReq + 0x10 + static_cast<std::size_t>(i) * 0x18) = 0;
                LogDebug("[EquipParam] loadout: phantom weapon equipId=%d (its mod was uninstalled "
                    "while it was equipped) -> set to EQP_None (empty slot).\n", id);
            }
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
        }
    }

    static std::atomic<int> g_LoadoutPickLogged{ 0 };

    static int ReadLoadoutSlotIdsSEH(void* self, std::uint32_t slot,
                                     std::int32_t* src, std::int32_t* work)
    {
        __try
        {
            std::uint8_t* self8 = static_cast<std::uint8_t*>(self);
            std::uint8_t* loadoutSubsys = *reinterpret_cast<std::uint8_t**>(self8 + 0x1a0);
            if (!loadoutSubsys) return 0;
            std::uint8_t* l138 = *reinterpret_cast<std::uint8_t**>(loadoutSubsys + 0x138);
            if (!l138) return 0;
            std::uint8_t* sourceBase = *reinterpret_cast<std::uint8_t**>(l138 + 0x98);
            if (!sourceBase) return 0;
            std::uint8_t* sourceReq = sourceBase + static_cast<std::size_t>(slot) * 0xa0;
            for (int i = 0; i < 3; ++i)
                src[i] = *reinterpret_cast<std::int32_t*>(
                    sourceReq + static_cast<std::size_t>(i) * 0x18);

            std::uint8_t* workingBase = *reinterpret_cast<std::uint8_t**>(self8 + 0x1e0);
            if (!workingBase)
                return 1;
            const std::int32_t bias = *reinterpret_cast<std::int32_t*>(loadoutSubsys + 0xc);
            std::uint8_t* workingReq = workingBase
                + static_cast<std::size_t>(static_cast<std::int32_t>(slot) - bias) * 0xb0;
            for (int i = 0; i < 3; ++i)
                work[i] = *reinterpret_cast<std::int32_t*>(
                    workingReq + 0x10 + static_cast<std::size_t>(i) * 0x18);
            return 2;
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            return 0;
        }
    }

    static bool LoadoutStateChangedSinceLastLog(std::uint32_t slot,
                                                const std::int32_t* sAfter,
                                                const std::int32_t* wAfter)
    {
        static std::map<std::uint32_t, std::array<std::int32_t, 6>> s_last;
        std::array<std::int32_t, 6> now = {
            sAfter[0], sAfter[1], sAfter[2], wAfter[0], wAfter[1], wAfter[2]
        };
        auto it = s_last.find(slot);
        if (it != s_last.end() && it->second == now)
            return false;
        s_last[slot] = now;
        return true;
    }

    static void __fastcall hkUpdateLoadoutRequest(void* self, std::uint32_t slot)
    {
        HealSupplyAmmoCountTable();
        FillCustomMotionEntriesEarly();
        std::int32_t pinned[3] = {};
        int pinnedCount = 0;
        BlankPhantomLoadoutWeaponsSEH(self, slot, pinned, &pinnedCount);
        for (int i = 0; i < pinnedCount; ++i)
            V_FrameWorkState::NotePinnedEquipId(pinned[i]);

        std::int32_t sBefore[3] = {}, wBefore[3] = {};
        const int okBefore = ReadLoadoutSlotIdsSEH(self, slot, sBefore, wBefore);

        t_LoadoutBuildActive = true;
        __try
        {
            g_OrigUpdateLoadoutRequest(self, slot);
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            static unsigned long long lastMs = 0;
            const unsigned long long now = GetTickCount64();
            if (now - lastMs > 5000)
            {
                lastMs = now;
                LogDebug("[EquipParam] UpdateLoadoutRequest guarded a crash (a phantom loadout "
                    "weapon could not be cleared this frame) - request skipped.\n");
            }
        }
        t_LoadoutBuildActive = false;

        std::int32_t sAfter[3] = {}, wAfter[3] = {};
        const int okAfter = ReadLoadoutSlotIdsSEH(self, slot, sAfter, wAfter);
        const bool interesting = (okBefore != 2 || okAfter != 2)
            || LoadoutStateChangedSinceLastLog(slot, sAfter, wAfter);
        if (interesting && g_LoadoutPickLogged.fetch_add(1) < 400)
            LogDebug("[LoadoutPick] slot=%u src[%d %d %d]->[%d %d %d] "
                "work[%d %d %d]->[%d %d %d] read=%d/%d\n",
                slot, sBefore[0], sBefore[1], sBefore[2],
                sAfter[0], sAfter[1], sAfter[2],
                wBefore[0], wBefore[1], wBefore[2],
                wAfter[0], wAfter[1], wAfter[2], okBefore, okAfter);
    }
}

bool Install_LoadoutRequestGuard()
{
    void* target = ResolveGameAddress(gAddr.CorePlugin_UpdateLoadoutRequest);
    if (!target)
        return true;
    const bool ok = CreateAndEnableHook(
        target, &hkUpdateLoadoutRequest,
        reinterpret_cast<void**>(&g_OrigUpdateLoadoutRequest));
    if (!ok)
        Log("[EquipParam] UpdateLoadoutRequest guard Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] UpdateLoadoutRequest guard Install -> OK (target=%p)\n", target);
#endif
    return ok;
}

void Uninstall_LoadoutRequestGuard()
{
    if (gAddr.CorePlugin_UpdateLoadoutRequest && g_OrigUpdateLoadoutRequest)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.CorePlugin_UpdateLoadoutRequest));
    g_OrigUpdateLoadoutRequest = nullptr;
}

bool Install_LoadoutGunInfoGetOrBuild()
{
    void* target = ResolveGameAddress(gAddr.EquipSystem_GetGunInfoById);
    if (!target)
    {
        LogDebug("[EquipParam] GetGunInfoById address not set for this build - loadout "
            "get-or-build disabled (a loadout weapon whose gun-info is not yet resident "
            "would crash UpdateLoadoutRequest on this build)\n");
        return true;
    }
    const bool ok = CreateAndEnableHook(
        target, &hkGetGunInfoById,
        reinterpret_cast<void**>(&g_OrigGetGunInfoById));
    if (!ok)
        Log("[EquipParam] GetGunInfoById get-or-build Install -> FAIL (target=%p)\n", target);
#ifdef _DEBUG
    else
        LogDebug("[EquipParam] GetGunInfoById get-or-build Install -> OK (target=%p)\n", target);
#endif
    return ok;
}

void Uninstall_LoadoutGunInfoGetOrBuild()
{
    if (gAddr.EquipSystem_GetGunInfoById && g_OrigGetGunInfoById)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.EquipSystem_GetGunInfoById));
    g_OrigGetGunInfoById = nullptr;
}

namespace
{
    using GetSuppressorAmount_t = unsigned char(__fastcall*)(void* self, std::uintptr_t developRow);
    static GetSuppressorAmount_t g_OrigGetSuppressorAmount = nullptr;

    static int ReadSuppressorRowSEH(const std::uint8_t* gunBasic, const std::uint8_t* muzzleOpt,
                                    int subId, int* outSeg)
    {
        __try
        {
            const int muzzleOptionId = gunBasic[static_cast<size_t>(subId - 1) * 0x0c + 5];
            if (muzzleOptionId == 0)
            {
                *outSeg = 0;
                return 1;
            }
            const std::uint8_t* mrow = muzzleOpt + static_cast<size_t>(muzzleOptionId - 1) * 3;
            if ((mrow[2] & 1) == 0)
            {
                *outSeg = 0;
                return 1;
            }
            const unsigned char dur = mrow[1];
            int seg;
            if (dur == 0xff)      seg = 4;
            else if (dur < 0x10)  seg = 1;
            else                  seg = (dur > 0x1e) ? 3 : 2;
            *outSeg = seg;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int CallDevResolverSEH(void* self, unsigned int developRow, int* outVal)
    {
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(self);
            using DevResolver_t = unsigned short(__fastcall*)(void*, std::uintptr_t);
            DevResolver_t fn = reinterpret_cast<DevResolver_t>(
                *reinterpret_cast<void**>(reinterpret_cast<char*>(vtbl) + 0x130));
            *outVal = static_cast<int>(fn(self, developRow));
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static unsigned char __fastcall hkGetSuppressorAmount(void* self, std::uintptr_t developRow)
    {
        const unsigned char orig =
            g_OrigGetSuppressorAmount ? g_OrigGetSuppressorAmount(self, developRow) : 0;
        if (orig != 0)
            return orig;

        const unsigned int row = static_cast<unsigned int>(developRow & 0xFFFF);

        int equipId = -1;
        CallDevResolverSEH(self, row, &equipId);
        const int subId = (equipId > 0) ? TppEquip_GetSubIdForEquipId(equipId) : 0;

        int seg = -1;
        const std::uint8_t* gunBasic = ImplBufPtr(0x08);
        const std::uint8_t* muzzleOpt = ImplBufPtr(0x28);
        if (subId > 0 && gunBasic && muzzleOpt)
            ReadSuppressorRowSEH(gunBasic, muzzleOpt, subId, &seg);

        if (subId > 0 && seg > 0)
            return static_cast<unsigned char>(seg);
        return orig;
    }
}

bool Install_SuppressorGauge_Hook()
{
    void* target = ResolveGameAddress(gAddr.EquipDevelopControllerImpl_GetSuppressorAmount);
    if (!target)
    {
        LogDebug("[SuppressorGauge] GetSuppressorAmount address not set for this build - custom suppressor gauge skipped\n");
        return true;
    }

    const bool ok = CreateAndEnableHook(
        target, &hkGetSuppressorAmount,
        reinterpret_cast<void**>(&g_OrigGetSuppressorAmount));
    if (ok)
        LogDebug("[SuppressorGauge] GetSuppressorAmount hook Install -> OK (target=%p)\n", target);
    else
        Log("[SuppressorGauge] GetSuppressorAmount hook Install -> FAIL (target=%p)\n", target);
    return ok;
}

void Uninstall_SuppressorGauge_Hook()
{
    if (gAddr.EquipDevelopControllerImpl_GetSuppressorAmount)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.EquipDevelopControllerImpl_GetSuppressorAmount));
    g_OrigGetSuppressorAmount = nullptr;
}

namespace
{
    static const int kDamageStride     = 0x1a;
    static const int kDamageCapacity   = 489;
    static const int kDamageCustomBase = kDamageCapacity;

    static const int kDamageCustomHardMax = 0xFFFE;
    static const int kDamageCustomCap  = kDamageCustomHardMax - kDamageCustomBase;

    struct DamageRow
    {
        int damageId;
        int lethalDamage, staminaDamage, impactForce, lethalDamageUI;
        int unk3, unk4, unk5, unk6, unk7, unk8;
        int flags;
        int damageSource, injureType, injurePart, unk11, unk12;
    };

    static std::vector<DamageRow> g_DamageRows;
    static std::map<std::string, int> g_DamageNameToId;
    static int g_DamageCustomNextFree = kDamageCustomBase;

    static std::vector<std::uint8_t> g_CustomDamageBuffer;

    static bool DamageIdIsCustom(int id)
    {
        return id >= kDamageCustomBase && id < kDamageCustomBase + kDamageCustomCap;
    }

    static std::uint8_t* CustomDamageRowPtr(int id)
    {
        if (!DamageIdIsCustom(id))
            return nullptr;
        const size_t off = static_cast<size_t>(id - kDamageCustomBase) * kDamageStride;
        if (off + kDamageStride > g_CustomDamageBuffer.size())
            return nullptr;
        return g_CustomDamageBuffer.data() + off;
    }

    static std::uint8_t* CustomDamageRowPtrGrow(int id)
    {
        if (!DamageIdIsCustom(id))
            return nullptr;
        const size_t need = (static_cast<size_t>(id - kDamageCustomBase) + 1) * kDamageStride;
        if (need > g_CustomDamageBuffer.size())
        {
            const size_t chunk = static_cast<size_t>(256) * kDamageStride;
            const size_t grown = ((need + chunk - 1) / chunk) * chunk;
            g_CustomDamageBuffer.resize(grown, 0);
        }
        return g_CustomDamageBuffer.data() +
               static_cast<size_t>(id - kDamageCustomBase) * kDamageStride;
    }

    static std::uint8_t* DamageBufBase()
    {
        void** loc = static_cast<void**>(
            ResolveGameAddress(gAddr.DamageParameterTable_Instance));
        if (!loc)
            return nullptr;
        std::uint8_t* inst = ReadPtrSEH(loc);
        if (!inst)
            return nullptr;
        return inst + 8;
    }

    static void WriteDamageRowSEH(std::uint8_t* buf, const DamageRow& r)
    {
        std::uint8_t* p = CustomDamageRowPtrGrow(r.damageId);
        if (!p)
        {
            if (!buf || r.damageId <= 0 || r.damageId >= kDamageCapacity)
                return;
            p = buf + static_cast<size_t>(r.damageId) * kDamageStride;
        }
        __try
        {
            *reinterpret_cast<std::uint16_t*>(p + 0x00) = static_cast<std::uint16_t>(r.lethalDamage & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x02) = static_cast<std::uint16_t>(r.staminaDamage & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x04) = static_cast<std::uint16_t>(r.impactForce & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x06) = static_cast<std::uint16_t>(r.lethalDamageUI & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x08) = static_cast<std::uint16_t>(r.unk3 & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x0a) = static_cast<std::uint16_t>(r.unk4 & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x0c) = static_cast<std::uint16_t>(r.unk5 & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x0e) = static_cast<std::uint16_t>(r.unk6 & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x10) = static_cast<std::uint16_t>(r.unk7 & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x12) = static_cast<std::uint16_t>(r.unk8 & 0xFFFF);
            *reinterpret_cast<std::uint16_t*>(p + 0x14) = static_cast<std::uint16_t>(r.flags & 0xFFFF);
            p[0x16] = static_cast<std::uint8_t>(r.damageSource & 0xFF);
            p[0x17] = static_cast<std::uint8_t>((r.injureType & 0xF) | ((r.injurePart & 0xF) << 4));
            p[0x18] = static_cast<std::uint8_t>(r.unk11 & 0xFF);
            p[0x19] = static_cast<std::uint8_t>(r.unk12 & 0xFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    using ReloadDamageParameter_t = int(__fastcall*)(lua_State* L);
    static ReloadDamageParameter_t g_OrigReloadDamage = nullptr;

    static void ReapplyDamageRows()
    {
        std::vector<DamageRow> snapshot;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            snapshot = g_DamageRows;
        }
        std::uint8_t* buf = DamageBufBase();
        for (const auto& r : snapshot)
            WriteDamageRowSEH(buf, r);
    }

    static int __fastcall hkReloadDamageParameter(lua_State* L)
    {
        const int ret = g_OrigReloadDamage ? g_OrigReloadDamage(L) : 0;
        ReapplyDamageRows();
        return ret;
    }

    using GetDamageParameter_t = std::uint8_t*(__fastcall*)(std::uint8_t* self, int attackId);
    static GetDamageParameter_t g_OrigGetDamageParameter = nullptr;

    static std::uint8_t g_DamageZeroRow[kDamageStride] = {};

    static std::uint8_t* __fastcall hkGetDamageParameter(std::uint8_t* self, int attackId)
    {
        std::uint8_t* custom = CustomDamageRowPtr(attackId);
        if (custom)
            return custom;

        if (DamageIdIsCustom(attackId))
            return g_DamageZeroRow;
        if (g_OrigGetDamageParameter)
            return g_OrigGetDamageParameter(self, attackId);
        return self + 8 + static_cast<size_t>(static_cast<unsigned>(attackId)) * kDamageStride;
    }
}

int EquipParam_AllocateDamageSlotForName(const char* name)
{
    if (!name || !name[0])
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    auto it = g_DamageNameToId.find(name);
    if (it != g_DamageNameToId.end())
        return it->second;

    if (g_DamageCustomNextFree >= kDamageCustomBase + kDamageCustomCap)
    {
        LogDebug("[EquipParam] custom damage id space exhausted for '%s' (cap=%d custom slots)\n",
            name, kDamageCustomCap);
        return 0;
    }

    const int id = g_DamageCustomNextFree++;
    g_DamageNameToId[name] = id;

    CustomDamageRowPtrGrow(id);
    LogDebug("[EquipParam] '%s' -> damageId %d (custom; served from DLL buffer via GetDamageParameter hook)\n",
        name, id);
    return id;
}

int __cdecl l_SetDamage(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        LogDebug("[EquipParam] SetDamage: argument #1 must be a table\n");
        return 0;
    }

    int damageId = 0;
    if (!ReadNamedInt(L, 1, "damageId", damageId) || damageId <= 0)
    {
        LogDebug("[EquipParam] SetDamage: missing/invalid damageId (declare one via V_TppEquip.DeclareDamages)\n");
        return 0;
    }
    if (damageId >= kDamageCustomBase + kDamageCustomCap)
    {
        LogDebug("[EquipParam] SetDamage: damageId=%d out of range (1..%d = vanilla override, %d..%d = custom) - not written\n",
            damageId, kDamageCapacity - 1, kDamageCustomBase, kDamageCustomBase + kDamageCustomCap - 1);
        return 0;
    }

    DamageRow r{};
    r.damageId = damageId;
    ReadNamedInt(L, 1, "lethalDamage",   r.lethalDamage);
    ReadNamedInt(L, 1, "staminaDamage",  r.staminaDamage);
    ReadNamedInt(L, 1, "impactForce",    r.impactForce);
    ReadNamedInt(L, 1, "lethalDamageUI", r.lethalDamageUI);
    ReadNamedInt(L, 1, "unk3", r.unk3);
    ReadNamedInt(L, 1, "unk4", r.unk4);
    ReadNamedInt(L, 1, "unk5", r.unk5);
    ReadNamedInt(L, 1, "unk6", r.unk6);
    ReadNamedInt(L, 1, "unk7", r.unk7);
    ReadNamedInt(L, 1, "unk8", r.unk8);
    ReadNamedInt(L, 1, "damageSource", r.damageSource);
    ReadNamedInt(L, 1, "injureType",   r.injureType);
    ReadNamedInt(L, 1, "injurePart",   r.injurePart);
    ReadNamedInt(L, 1, "unk11", r.unk11);
    ReadNamedInt(L, 1, "unk12", r.unk12);

    auto warnWidth = [&](const char* name, int v, int maxV)
    {
        if (v < 0 || v > maxV)
            LogDebug("[EquipParam] SetDamage damageId=%d: %s=%d exceeds the native "
                "field (0..%d) and wraps to %d in the engine row - clamp or "
                "rescale the value\n",
                damageId, name, v, maxV, v & maxV);
    };
    warnWidth("lethalDamage",   r.lethalDamage,   0xFFFF);
    warnWidth("staminaDamage",  r.staminaDamage,  0xFFFF);
    warnWidth("impactForce",    r.impactForce,    0xFFFF);
    warnWidth("lethalDamageUI", r.lethalDamageUI, 0xFFFF);
    warnWidth("unk3", r.unk3, 0xFFFF);
    warnWidth("unk4", r.unk4, 0xFFFF);
    warnWidth("unk5", r.unk5, 0xFFFF);
    warnWidth("unk6", r.unk6, 0xFFFF);
    warnWidth("unk7", r.unk7, 0xFFFF);
    warnWidth("unk8", r.unk8, 0xFFFF);
    warnWidth("damageSource", r.damageSource, 0xFF);
    warnWidth("injureType",   r.injureType,   0xF);
    warnWidth("injurePart",   r.injurePart,   0xF);
    warnWidth("unk11", r.unk11, 0xFF);
    warnWidth("unk12", r.unk12, 0xFF);

    auto readFlag = [&](const char* name, const char* legacy) -> int
    {
        int v = 0;
        if (ReadNamedInt(L, 1, name, v) && v)
            return 1;
        if (legacy && ReadNamedInt(L, 1, legacy, v) && v)
            return 1;
        return 0;
    };
    const int hitNPC       = readFlag("hitNPC", nullptr);
    const int isSniper     = readFlag("isSniper", "unk14");
    const int isShotgun    = readFlag("isShotgun", "unk15");
    const int isTranq      = readFlag("isTranq", nullptr);
    const int isStun       = readFlag("isStun", nullptr);
    const int isExplosive  = readFlag("isExplosive", "unk18");
    const int isMelee      = readFlag("isMelee", "unk19");
    const int isBlade      = readFlag("isBlade", "unk20");
    const int isFire       = readFlag("isFire", nullptr);
    const int isWater      = readFlag("isWater", "unk27");
    const int isElectric   = readFlag("isElectric", nullptr);
    const int isParasite   = readFlag("isParasite", "unk22");
    const int isGas        = readFlag("isGas", nullptr);
    const int isVehicleHit = readFlag("isVehicleHit", "unk24");
    const int unk25        = readFlag("unk25", nullptr);
    const int isPenetrating = readFlag("isPenetrating", "unk28");
    r.flags = (hitNPC ? 0x1 : 0)   | (isSniper ? 0x2 : 0)        | (isShotgun ? 0x4 : 0)    | (isTranq ? 0x8 : 0)
            | (isStun ? 0x10 : 0)  | (isExplosive ? 0x20 : 0)    | (isMelee ? 0x40 : 0)     | (isBlade ? 0x80 : 0)
            | (isFire ? 0x100 : 0) | (isWater ? 0x200 : 0)       | (isElectric ? 0x400 : 0) | (isParasite ? 0x800 : 0)
            | (isGas ? 0x1000 : 0) | (isVehicleHit ? 0x2000 : 0) | (unk25 ? 0x4000 : 0)     | (isPenetrating ? 0x8000 : 0);

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    std::uint8_t* buf = DamageBufBase();
    if (!buf && damageId < kDamageCapacity)
    {
        LogDebug("[EquipParam] SetDamage: native damage table not available for this build - vanilla override skipped\n");
        return 0;
    }

    bool replaced = false;
    for (auto& e : g_DamageRows)
        if (e.damageId == damageId) { e = r; replaced = true; break; }
    if (!replaced)
        g_DamageRows.push_back(r);

    const bool vanillaRow = damageId < kDamageCustomBase;
    const std::uint8_t* rowPtr = (buf && vanillaRow)
        ? buf + static_cast<size_t>(damageId) * kDamageStride : nullptr;
    if (vanillaRow)
        EquipParam_VanillaPreWrite(kVanillaSpace_Damage, damageId, rowPtr,
                                   kDamageStride);

    WriteDamageRowSEH(buf, r);

    if (vanillaRow)
        EquipParam_VanillaPostWrite(kVanillaSpace_Damage, damageId, rowPtr,
                                    kDamageStride);

#ifdef _DEBUG
    LogDebug("[EquipParam] SetDamage damageId=%d lethal=%d stamina=%d flags=0x%04X src=%d -> %s\n",
        damageId, r.lethalDamage, r.staminaDamage, r.flags, r.damageSource,
        (damageId >= kDamageCustomBase) ? "custom DLL slot" : "vanilla override");
#endif
    return 0;
}

int __cdecl l_DeclareDamages(lua_State* L)
{
    if (!ResolveLuaApi())
        return 0;

    auto declareOne = [&](const char* name) -> int
    {
        if (!name || !name[0])
            return 0;
        const int id = EquipParam_AllocateDamageSlotForName(name);
        if (id > 0)
            TppDamageConst_SetValue(name, static_cast<std::uint32_t>(id), L);
        return id;
    };

    if (g_lua_type(L, 1) == LUA_TTABLE)
    {
        const int n = static_cast<int>(g_lua_objlen(L, 1));
        int declared = 0;
        for (int i = 1; i <= n; ++i)
        {
            g_lua_rawgeti(L, 1, i);
            std::string nm;
            if (g_lua_type(L, -1) == LUA_TSTRING)
            {
                size_t len = 0;
                const char* s = g_lua_tolstring(L, -1, &len);
                if (s)
                    nm.assign(s, len);
            }
            g_lua_settop(L, -2);
            if (!nm.empty() && declareOne(nm.c_str()) > 0)
                ++declared;
        }
        g_lua_pushnumber(L, static_cast<lua_Number>(declared));
        return 1;
    }

    std::string nm;
    if (g_lua_type(L, 1) == LUA_TSTRING)
    {
        size_t len = 0;
        const char* s = g_lua_tolstring(L, 1, &len);
        if (s)
            nm.assign(s, len);
    }
    const int id = declareOne(nm.empty() ? nullptr : nm.c_str());
    g_lua_pushnumber(L, static_cast<lua_Number>(id));
    return 1;
}

bool Install_DamageParameter_Hook()
{
    bool ok = true;

    void* reloadTarget = ResolveGameAddress(gAddr.DamageParameterTable_ReloadDamageParameter);
    if (reloadTarget)
    {
        const bool r = CreateAndEnableHook(
            reloadTarget, &hkReloadDamageParameter,
            reinterpret_cast<void**>(&g_OrigReloadDamage));
        if (r)
            LogDebug("[Damage] ReloadDamageParameter reapply hook Install -> OK (target=%p)\n", reloadTarget);
        else
            Log("[Damage] ReloadDamageParameter reapply hook Install -> FAIL (target=%p)\n", reloadTarget);
        ok = ok && r;
    }
    else
    {
        LogDebug("[Damage] ReloadDamageParameter address not set for this build - reapply guard skipped\n");
    }

    void* getterTarget = ResolveGameAddress(gAddr.DamageParameterTable_GetDamageParameter);
    if (getterTarget)
    {
        const bool r = CreateAndEnableHook(
            getterTarget, &hkGetDamageParameter,
            reinterpret_cast<void**>(&g_OrigGetDamageParameter));
        if (r)
            LogDebug("[Damage] GetDamageParameter redirect hook Install -> OK (target=%p; custom attackIds %d..%d served from DLL buffer)\n",
                getterTarget, kDamageCustomBase, kDamageCustomBase + kDamageCustomCap - 1);
        else
            Log("[Damage] GetDamageParameter redirect hook Install -> FAIL (target=%p)\n", getterTarget);
        ok = ok && r;
    }
    else
    {
        LogDebug("[Damage] GetDamageParameter address not set for this build - custom damage attackIds unavailable\n");
    }

    return ok;
}

void Uninstall_DamageParameter_Hook()
{
    if (gAddr.DamageParameterTable_ReloadDamageParameter)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.DamageParameterTable_ReloadDamageParameter));
    g_OrigReloadDamage = nullptr;

    if (gAddr.DamageParameterTable_GetDamageParameter)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.DamageParameterTable_GetDamageParameter));
    g_OrigGetDamageParameter = nullptr;
}

namespace
{
    static std::mutex g_VanillaTaintMutex;
    static std::set<int> g_VanillaTaint[kVanillaSpace_Count];
    static std::map<int, std::vector<std::uint8_t>> g_VanillaPreImage[kVanillaSpace_Count];
    static bool g_VanillaTaintAny = false;

    static const int kVanillaRowMax = 32;

    static const char* const kVanillaSpaceNames[kVanillaSpace_Count] =
    {
        "receiver", "barrel", "magazine", "bullet", "stock", "muzzleOption",
        "sight", "underBarrel", "option", "gunBasic weapon", "damage"
    };

    static int CopyBytesSEH(const std::uint8_t* src, std::uint8_t* dst, int n)
    {
        __try
        {
            for (int k = 0; k < n; ++k)
                dst[k] = src[k];
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static bool VanillaTaintHas(int space, int id)
    {
        if (id <= 0)
            return false;
        std::lock_guard<std::mutex> lock(g_VanillaTaintMutex);
        return g_VanillaTaint[space].find(id) != g_VanillaTaint[space].end();
    }

    static bool VanillaTaintSpaceUsed(int space)
    {
        std::lock_guard<std::mutex> lock(g_VanillaTaintMutex);
        return !g_VanillaTaint[space].empty();
    }

    static void* ResolveEquipIdTableObj()
    {
        using GetQuark_t = std::uint8_t* (__fastcall*)();
        auto getQuark = reinterpret_cast<GetQuark_t>(
            ResolveGameAddress(gAddr.GetQuarkSystemTable));
        if (!getQuark)
            return nullptr;
        __try
        {
            std::uint8_t* quark = getQuark();
            if (!quark)
                return nullptr;
            std::uint8_t* app = *reinterpret_cast<std::uint8_t**>(quark + 0x98);
            if (!app)
                return nullptr;
            std::uint8_t* holder = *reinterpret_cast<std::uint8_t**>(app + 0x1e8);
            if (!holder)
                return nullptr;
            return *reinterpret_cast<void**>(holder + 8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static int EquipObjTypeSEH(void* obj, int equipId)
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>(obj);
            using Fn = int(__fastcall*)(void*, int);
            return reinterpret_cast<Fn>(vt[0])(obj, equipId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int EquipObjWeaponIdSEH(void* obj, int equipId)
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>(obj);
            using Fn = unsigned short(__fastcall*)(void*, int);
            return reinterpret_cast<Fn>(vt[2])(obj, equipId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int ReadPartByteSEH(const std::uint8_t* p, std::uint8_t& out)
    {
        __try
        {
            out = *p;
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static int PartRowByte(const PartBuffer& pb, int id, int byteOff)
    {
        if (id <= 0)
            return 0;
        const int bound = pb.active ? pb.maxId : pb.stockCount;
        if (id > bound)
            return 0;
        std::uint8_t* buf = ReadPtrSEH(PartPtrLoc(pb));
        if (!buf)
            return 0;
        std::uint8_t v = 0;
        if (ReadPartByteSEH(buf + static_cast<size_t>(id - 1) * pb.stride + byteOff, v) != 1)
            return 0;
        return v;
    }

    static unsigned short SafeGetAttackIdSEH(void* self, int equipId)
    {
        __try
        {
            return g_OrigGetAttackId(self, equipId);
        }
        __except (SehAvOnly(GetExceptionCode()))
        {
            return 0;
        }
    }

    static bool MagazineChainTainted(int ammoId)
    {
        if (ammoId <= 0)
            return false;
        if (VanillaTaintHas(kVanillaSpace_Magazine, ammoId))
            return true;
        const int bulletId = PartRowByte(g_Magazine, ammoId, 6);
        return bulletId > 0 && VanillaTaintHas(kVanillaSpace_Bullet, bulletId);
    }

    static int ReadChimeraDescSEH(const std::uint8_t* table, int slot,
                                  unsigned char* out11)
    {
        __try
        {
            const std::uint8_t* d = table + 0x12 + static_cast<size_t>(slot) * 0xe;
            for (int k = 0; k < 11; ++k)
                out11[k] = d[k];
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static bool PartsBytesTainted(const unsigned char* b)
    {
        if (VanillaTaintHas(kVanillaSpace_Receiver, b[0]))
            return true;
        if (VanillaTaintHas(kVanillaSpace_Barrel, b[1]))
            return true;
        if (VanillaTaintHas(kVanillaSpace_Stock, b[3]))
            return true;
        if (VanillaTaintHas(kVanillaSpace_MuzzleOption, b[5]))
            return true;
        if (VanillaTaintHas(kVanillaSpace_Sight, b[6])
            || VanillaTaintHas(kVanillaSpace_Sight, b[7]))
            return true;
        if (VanillaTaintHas(kVanillaSpace_Option, b[8])
            || VanillaTaintHas(kVanillaSpace_Option, b[9]))
            return true;
        if (MagazineChainTainted(b[2]))
            return true;
        if (b[10])
        {
            if (VanillaTaintHas(kVanillaSpace_UnderBarrel, b[10]))
                return true;
            const int ubReceiver = PartRowByte(g_UnderBarrel, b[10], 0);
            if (ubReceiver > 0 && VanillaTaintHas(kVanillaSpace_Receiver, ubReceiver))
                return true;
            if (MagazineChainTainted(PartRowByte(g_UnderBarrel, b[10], 1)))
                return true;
        }
        return false;
    }
}

void EquipParam_VanillaPreWrite(int space, int id, const unsigned char* row, int stride)
{
    if (space < 0 || space >= kVanillaSpace_Count || id <= 0 || !row
        || stride <= 0 || stride > kVanillaRowMax)
        return;

    {
        std::lock_guard<std::mutex> lock(g_VanillaTaintMutex);
        if (g_VanillaPreImage[space].find(id) != g_VanillaPreImage[space].end())
            return;
    }

    std::uint8_t pre[kVanillaRowMax];
    if (CopyBytesSEH(row, pre, stride) != 1)
        return;

    std::lock_guard<std::mutex> lock(g_VanillaTaintMutex);
    g_VanillaPreImage[space].emplace(
        id, std::vector<std::uint8_t>(pre, pre + stride));
}

void EquipParam_VanillaPostWrite(int space, int id, const unsigned char* row, int stride)
{
    if (space < 0 || space >= kVanillaSpace_Count || id <= 0
        || stride <= 0 || stride > kVanillaRowMax)
        return;

    std::uint8_t now[kVanillaRowMax];
    const bool haveNow = row && CopyBytesSEH(row, now, stride) == 1;

    int transition = 0;
    {
        std::lock_guard<std::mutex> lock(g_VanillaTaintMutex);
        bool matchesVanilla = false;
        auto it = g_VanillaPreImage[space].find(id);
        if (haveNow && it != g_VanillaPreImage[space].end()
            && it->second.size() == static_cast<size_t>(stride)
            && std::memcmp(it->second.data(), now, static_cast<size_t>(stride)) == 0)
            matchesVanilla = true;

        const bool was = g_VanillaTaint[space].count(id) != 0;
        if (!matchesVanilla && !was)
        {
            g_VanillaTaint[space].insert(id);
            transition = 1;
        }
        else if (matchesVanilla && was)
        {
            g_VanillaTaint[space].erase(id);
            transition = -1;
        }
        if (transition != 0)
        {
            bool any = false;
            for (int s = 0; s < kVanillaSpace_Count && !any; ++s)
                any = !g_VanillaTaint[s].empty();
            g_VanillaTaintAny = any;
        }
    }

    if (transition > 0)
        LogDebug("[FobGuard] vanilla %s id %d differs from its stock values after a module "
            "write - vanilla weapons built on it are no longer FOB-deployable\n",
            kVanillaSpaceNames[space], id);
    else if (transition < 0)
        LogDebug("[FobGuard] vanilla %s id %d restored to stock values - FOB block lifted\n",
            kVanillaSpaceNames[space], id);
}

void EquipParam_VanillaForceTaint(int space, int id, const char* why)
{
    if (space < 0 || space >= kVanillaSpace_Count || id <= 0)
        return;

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(g_VanillaTaintMutex);
        inserted = g_VanillaTaint[space].insert(id).second;
        g_VanillaTaintAny = true;
    }
    if (inserted)
        LogDebug("[FobGuard] vanilla %s id %d given %s by a module - vanilla weapons built "
            "on it are no longer FOB-deployable\n",
            kVanillaSpaceNames[space], id, why ? why : "modified behavior");
}

bool EquipParam_IsEquipIdFobTainted(unsigned int equipId, int isWeaponSlot)
{
    {
        std::lock_guard<std::mutex> lock(g_VanillaTaintMutex);
        if (!g_VanillaTaintAny)
            return false;
    }
    if (equipId == 0)
        return false;

    if (VanillaTaintSpaceUsed(kVanillaSpace_Damage))
    {
        void* self = ResolveGameAddress(gAddr.EquipParameterTablesImpl_Instance);
        if (self && g_OrigGetAttackId)
        {
            const int attackId = SafeGetAttackIdSEH(self, static_cast<int>(equipId));
            if (attackId > 0 && VanillaTaintHas(kVanillaSpace_Damage, attackId))
                return true;
        }
    }

    if (!isWeaponSlot)
        return false;

    unsigned char b[12] = {};

    if (equipId >= 0x367 && equipId <= 0x36C)
    {
        auto* chimera = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipSystem_ChimeraPartsSetWork));
        if (!chimera)
            return false;
        if (ReadChimeraDescSEH(chimera, static_cast<int>(equipId) - 0x367, b) != 1)
            return false;
#ifdef _DEBUG
        LogDebug("[FobGuard] walk equipId=%u CHIMERA slot=%d rc=%u ba=%u am=%u sk=%u "
            "mz=%u mo=%u st=%u/%u lt=%u/%u ub=%u\n",
            equipId, static_cast<int>(equipId) - 0x367, b[0], b[1], b[2], b[3],
            b[4], b[5], b[6], b[7], b[8], b[9], b[10]);
#endif
    }
    else
    {
        void* obj = ResolveEquipIdTableObj();
        if (!obj)
            return false;
        const int eqpType = EquipObjTypeSEH(obj, static_cast<int>(equipId));
        if (eqpType < 1 || eqpType > 8)
            return false;
        const int weaponId = EquipObjWeaponIdSEH(obj, static_cast<int>(equipId));
        if (weaponId <= 0)
            return false;

        if (VanillaTaintHas(kVanillaSpace_Weapon, weaponId))
            return true;

        if (!GunBasic_ReadRowBytes(weaponId, b))
            return false;

#ifdef _DEBUG
        LogDebug("[FobGuard] walk equipId=%u type=%d weaponId=%d rc=%u ba=%u am=%u sk=%u "
            "mz=%u mo=%u st=%u/%u lt=%u/%u ub=%u\n",
            equipId, eqpType, weaponId, b[0], b[1], b[2], b[3], b[4], b[5], b[6],
            b[7], b[8], b[9], b[10]);
#endif
    }

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return PartsBytesTainted(b);
}
