#include "pch.h"
#include "TppEquip_ReloadEquipIdTable.h"

#include <Windows.h>
#include <cstdint>
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

    static void WriteNativeRow(const EquipIdRow& row)
    {
        const std::int32_t index = EquipIdCompression::ComputeCompressed(row.equipId);
        if (!EquipIdCompression::IsCompressedInBounds(index))
        {
            if (EquipIdCompression::IsExtendedEquipId(row.equipId))
            {
                EquipIdCompression::MarkExtendedEquipIdUsed(row.equipId);
                std::lock_guard<std::mutex> lock(g_Mutex);
                g_Extended[row.equipId] = row;
                static std::size_t s_extLogged = 0;
                if (s_extLogged < 8)
                {
                    ++s_extLogged;
                    Log("[EquipIdTable] equipId=%d is beyond the %d-slot native "
                        "table - stored in the DLL extended table (served to the "
                        "engine via the hooked equip accessors)\n",
                        row.equipId, EquipIdCompression::kCompressedSlotBound);
                }
                return;
            }
            Log("[EquipIdTable] REFUSED write: equipId=%d compresses to 0x%X, "
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
            Log("[EquipIdTable] native table address(es) not resolved; write skipped\n");
            return;
        }

        std::uint8_t* dst = infoList + static_cast<size_t>(index) * kRowStride;
        if (!SafeStampRow(dst, typeWords + index, row))
        {
            Log("[EquipIdTable] SEH writing native row for equipId=%d - "
                "addresses wrong for this build; write skipped\n", row.equipId);
            return;
        }

        EquipIdCompression::MarkCompressedSlotUsed(index);
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
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
            Log("[EquipIdTable] EraseNativeRow refused: equipId=%d out of "
                "bounds\n", equipId);
            return false;
        }
        auto* infoList = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_InfoList));
        auto* typeWords = static_cast<std::uint16_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_TypeWords));
        if (!infoList || !typeWords)
        {
            Log("[EquipIdTable] EraseNativeRow: table address(es) not resolved; "
                "skipped\n");
            return false;
        }
        std::uint8_t* dst = infoList + static_cast<size_t>(index) * kRowStride;
        if (!SafeZeroRow(dst, typeWords + index))
        {
            Log("[EquipIdTable] SEH erasing native row for equipId=%d - "
                "skipped\n", equipId);
            return false;
        }
        EquipIdCompression::ClearCompressedSlotUsed(index);
#ifdef _DEBUG
        Log("[EquipIdTable] native row released for equipId=%d\n", equipId);
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

    static void QueueAndWrite(const EquipIdRow& row)
    {
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
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
        Log("[EquipIdTable] custom row: equipId=%d type=%d subId=%d block=%d "
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
            Log("[EquipIdTable] re-applied %zu custom row(s) after reload\n",
                applied);
#endif
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

        auto* infoList = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipIdTable_InfoList));
        if (!infoList)
        {
            Log("[ZetaDiag] #%d: EquipIdTable_InfoList unresolved for this build\n",
                myN);
            return;
        }

        std::vector<EquipIdRow> vfw;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            vfw = g_Rows;
        }

        Log("[ZetaDiag] ===== equip-id dump #%d: %zu V_FrameWork custom rows "
            "(native state AFTER the game/Zeta push, BEFORE V_FrameWork "
            "re-stamps) =====\n", myN, vfw.size());

        int ours = 0, foreign = 0, empty = 0, ext = 0;
        for (const auto& r : vfw)
        {
            const std::int32_t idx =
                EquipIdCompression::ComputeCompressed(r.equipId);
            if (!EquipIdCompression::IsCompressedInBounds(idx))
            {
                ++ext;
                Log("[ZetaDiag]   equipId=0x%X EXTENDED (DLL-side, not native)\n",
                    r.equipId);
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
            Log("[ZetaDiag]   equipId=0x%X slot=0x%X ourParts=%016llX "
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
        Log("[ZetaDiag] #%d summary: OURS=%d FOREIGN-collisions=%d EMPTY=%d "
            "EXTENDED=%d | weapon band 0x230-0x288 occupied=%d/89\n",
            myN, ours, foreign, empty, ext, wbOcc);
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

int __cdecl l_AddToEquipIdTable(lua_State* L)
{
    if (!ResolveLuaApi() || !g_lua_objlen || !g_lua_rawgeti)
        return 0;
    if (g_lua_type(L, 1) != LUA_TTABLE)
    {
        Log("[EquipIdTable] AddToEquipIdTable: argument #1 must be a table\n");
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
                Log("[EquipIdTable] AddToEquipIdTable: ignored malformed row %d\n", i);
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
        Log("[EquipIdTable] ReloadEquipIdTable address not set for this build - skipped\n");
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
        Log("[EquipIdTable] Reload hook Install -> OK (target=%p)\n", target);
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
            Log("[EquipIdTable] GetEquipTypeId hook Install -> OK (target=%p; serves "
                "the equip type of extended custom equipIds from the DLL table)\n",
                typeTarget);
#endif
    }
    return ok;
}

bool Uninstall_TppEquip_ReloadEquipIdTable_Hook()
{
    if (gAddr.EquipIdTable_GetEquipTypeId && g_OrigGetEquipTypeId)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.EquipIdTable_GetEquipTypeId));
    g_OrigGetEquipTypeId = nullptr;
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
