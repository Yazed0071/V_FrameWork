#include "pch.h"
#include "CustomBluePrint.h"

#include <Windows.h>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "V_FrameWorkState.h"
#include "LuaApi.h"
#include "DataBaseControllerImpl_AddDataBase.h"

extern "C" {
#include "lua.h"
}

namespace
{
    constexpr std::size_t  kQuark_AppOffset    = 0x98;
    constexpr std::size_t  kApp_MbmOffset      = 0x110;
    constexpr std::size_t  kMbm_DataBaseCtrl   = 0xAE8;

    constexpr const char*  kLuaConstTable      = "TppMotherBaseManagementConst";
    constexpr const char*  kLuaLibTable        = "V_TppMotherBaseManagement";

    constexpr std::int32_t kPublicIdBase       = 0x2000;
    constexpr std::int32_t kMaxRowsPerTab      = 108;
    constexpr std::int32_t kListableTabCount   = 7;
    constexpr std::int32_t kMaxCustomEntries   = kMaxRowsPerTab * kListableTabCount;

    bool g_EntryCapLogged = false;

    constexpr std::int32_t kMaxPublicId        = 0xFFFE;
    constexpr std::int32_t kMaxBluePrints      = kMaxPublicId - kPublicIdBase + 1;
    constexpr int          kLuaGlobalsIndex    = -10002;

    using GetQuarkSystemTable_t = void* (*)();

    std::mutex g_Mutex;
    std::unordered_map<std::string, std::int32_t> g_IdByKey;
    std::unordered_map<std::int32_t, std::string> g_KeyById;

    std::unordered_map<std::int32_t, std::string>* g_RebuildTarget = nullptr;

    bool g_CapacityFailureLogged = false;
    bool g_ConstFailureLogged = false;

    void* ResolveControllerSeh()
    {
        auto getQuark = reinterpret_cast<GetQuarkSystemTable_t>(
            ResolveGameAddress(gAddr.GetQuarkSystemTable));
        if (!getQuark)
            return nullptr;

        __try
        {
            std::uint8_t* quark = static_cast<std::uint8_t*>(getQuark());
            if (!quark) return nullptr;

            std::uint8_t* app = *reinterpret_cast<std::uint8_t**>(quark + kQuark_AppOffset);
            if (!app) return nullptr;

            std::uint8_t* mbm = *reinterpret_cast<std::uint8_t**>(app + kApp_MbmOffset);
            if (!mbm) return nullptr;

            return *reinterpret_cast<std::uint8_t**>(mbm + kMbm_DataBaseCtrl);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void EnsureDataBaseGuard()
    {
        bluePrintDb::EnsureAddDataBaseHook(ResolveControllerSeh());
    }

    void RebuildCallback(const char* name, std::int32_t value, bool)
    {
        if (g_RebuildTarget && name && value > 0)
            (*g_RebuildTarget)[value] = name;
    }

    std::string KeyForSlot(std::int32_t slot)
    {
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_KeyById.find(slot);
            if (it != g_KeyById.end())
                return it->second;
        }

        std::unordered_map<std::int32_t, std::string> rebuilt;
        g_RebuildTarget = &rebuilt;
        V_FrameWorkState::ForEachBluePrint(&RebuildCallback);
        g_RebuildTarget = nullptr;

        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const auto& kv : rebuilt)
        {
            g_KeyById[kv.first] = kv.second;
            g_IdByKey[kv.second] = kv.first;
        }
        auto it = g_KeyById.find(slot);
        return (it != g_KeyById.end()) ? it->second : std::string();
    }

    void Remember(const std::string& key, std::int32_t slot)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_IdByKey[key] = slot;
        g_KeyById[slot] = key;
    }

    bool IsLuaIdentifier(const char* name)
    {
        if (!name || !name[0])
            return false;
        const char c0 = name[0];
        if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
            return false;
        for (const char* p = name; *p; ++p)
        {
            const char c = *p;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                         || (c >= '0' && c <= '9') || c == '_';
            if (!ok)
                return false;
        }
        return true;
    }

    bool RawSetInTable(lua_State* L, int tableIndex, const char* name, std::int32_t value)
    {
        if (g_lua_type(L, tableIndex) != LUA_TTABLE)
            return false;
        g_lua_pushstring(L, const_cast<char*>(name));
        g_lua_pushnumber(L, static_cast<lua_Number>(value));
        g_lua_rawset(L, tableIndex - 2);
        return true;
    }

    bool WriteConstantIntoGlobalTable(lua_State* L, const char* table,
                                      const char* name, std::int32_t value,
                                      bool createIfAbsent)
    {
        const int top = g_lua_gettop(L);

        g_lua_getfield(L, kLuaGlobalsIndex, const_cast<char*>(table));
        const int foundType = g_lua_type(L, -1);

        if (foundType == LUA_TTABLE)
        {
            const bool ok = RawSetInTable(L, -1, name, value);
            g_lua_settop(L, top);
            return ok;
        }

        g_lua_settop(L, top);

        if (foundType != LUA_TNIL || !createIfAbsent)
            return false;

        g_lua_pushvalue(L, kLuaGlobalsIndex);
        g_lua_pushstring(L, const_cast<char*>(table));
        g_lua_createtable(L, 0, 8);
        g_lua_pushstring(L, const_cast<char*>(name));
        g_lua_pushnumber(L, static_cast<lua_Number>(value));
        g_lua_rawset(L, -3);
        g_lua_rawset(L, -3);
        g_lua_settop(L, top);
        return true;
    }

    void WriteLuaConstant(lua_State* L, const char* name, std::int32_t value)
    {
        if (!L || !ResolveLuaApi() || !g_lua_rawset || !g_lua_pushvalue)
        {
            if (!g_ConstFailureLogged)
            {
                g_ConstFailureLogged = true;
                Log("[BluePrint] ERROR: the Lua API is unavailable, so no constant "
                    "for '%s' was published - read the id back with "
                    "V_TppMotherBaseManagement.GetBluePrintId instead\n", name);
            }
            return;
        }

        const bool constTableOk =
            WriteConstantIntoGlobalTable(L, kLuaConstTable, name, value, true);
        WriteConstantIntoGlobalTable(L, kLuaLibTable, name, value, false);

        if (!constTableOk && !g_ConstFailureLogged)
        {
            g_ConstFailureLogged = true;
            Log("[BluePrint] ERROR: global '%s' is not a writable table, so %s.%s stays "
                "nil - use %s.%s or GetBluePrintId instead\n",
                kLuaConstTable, kLuaConstTable, name, kLuaLibTable, name);
        }
    }
}

namespace bluePrint
{
    std::int32_t Capacity()
    {
        return kMaxBluePrints;
    }

    std::int32_t PublicId(std::int32_t slot)
    {
        if (slot <= 0 || slot > kMaxBluePrints)
            return 0;
        return kPublicIdBase + slot - 1;
    }

    std::int32_t SlotFromPublicId(std::int32_t publicId)
    {
        const std::int32_t slot = publicId - kPublicIdBase + 1;
        if (slot <= 0 || slot > kMaxBluePrints)
            return 0;
        return slot;
    }

    std::string KeyFromSlot(std::int32_t slot)
    {
        return (slot <= 0) ? std::string() : KeyForSlot(slot);
    }

    void InjectConstantFor(lua_State* L, const char* key)
    {
        if (!L || !IsLuaIdentifier(key))
            return;
        const std::int32_t slot = Find(key);
        if (slot > 0)
            WriteLuaConstant(L, key, PublicId(slot));
    }

    void InjectConstants(lua_State* L)
    {
        if (!L)
            return;
        std::unordered_map<std::string, std::int32_t> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            snapshot = g_IdByKey;
        }
        for (const auto& entry : snapshot)
        {
            if (IsLuaIdentifier(entry.first.c_str()))
                WriteLuaConstant(L, entry.first.c_str(), PublicId(entry.second));
        }
    }

    std::int32_t Register(const char* key)
    {
        if (!key || !key[0])
            return 0;

        EnsureDataBaseGuard();

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_IdByKey.find(key);
            if (it != g_IdByKey.end())
                return it->second;
        }

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (g_IdByKey.size() >= static_cast<std::size_t>(kMaxCustomEntries))
            {
                if (!g_EntryCapLogged)
                {
                    g_EntryCapLogged = true;
                    Log("[BluePrint] '%s' was refused - %d custom DATABASE entries are already "
                        "registered, which is every row the seven listable tabs can render "
                        "(%d rows each), so another entry could never appear\n",
                        key, kMaxCustomEntries, kMaxRowsPerTab);
                }
                return 0;
            }
        }

        std::int32_t value = 0;
        if (!V_FrameWorkState::ResolveOrCreateBluePrintId(key, value) || value <= 0)
        {
            Log("[BluePrint] ERROR: no persistent id could be allocated for '%s' - "
                "that blueprint can never be granted or tested this session\n", key);
            return 0;
        }

        if (value > kMaxBluePrints)
        {
            if (!g_CapacityFailureLogged)
            {
                g_CapacityFailureLogged = true;
                Log("[BluePrint] ERROR: '%s' allocated id %d, past the %d addressable "
                    "blueprint ids - dataBaseId is 16-bit, so that blueprint cannot be "
                    "dropped or granted\n", key, value, kMaxBluePrints);
            }
            return 0;
        }

        Remember(key, value);
        return value;
    }

    std::int32_t Find(const char* key)
    {
        if (!key || !key[0])
            return 0;

        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_IdByKey.find(key);
            if (it != g_IdByKey.end())
                return it->second;
        }

        const std::int32_t value = V_FrameWorkState::GetBluePrintId(key);
        if (value <= 0 || value > kMaxBluePrints)
            return 0;

        Remember(key, value);
        return value;
    }

    bool HasKey(const char* key)
    {
        if (!key || !key[0])
            return false;
        return V_FrameWorkState::GetBluePrintOwned(key);
    }

    bool GetNew(std::int32_t id)
    {
        if (id <= 0 || id > kMaxBluePrints)
            return false;
        const std::string key = KeyForSlot(id);
        return !key.empty() && V_FrameWorkState::GetBluePrintNew(key.c_str());
    }

    bool SetNew(std::int32_t id, bool isNew)
    {
        if (id <= 0 || id > kMaxBluePrints)
            return false;
        const std::string key = KeyForSlot(id);
        if (key.empty())
            return false;
        V_FrameWorkState::SetBluePrintNew(key.c_str(), isNew);
        return true;
    }

    bool SetKey(const char* key, bool owned)
    {
        if (!key || !key[0])
            return false;
        if (Register(key) <= 0)
            return false;
        V_FrameWorkState::SetBluePrintOwned(key, owned);
        return true;
    }

    bool Has(std::int32_t id)
    {
        if (id <= 0 || id > kMaxBluePrints)
            return false;
        const std::string key = KeyForSlot(id);
        return !key.empty() && HasKey(key.c_str());
    }

    bool Set(std::int32_t id, bool owned)
    {
        if (id <= 0 || id > kMaxBluePrints)
            return false;
        const std::string key = KeyForSlot(id);
        if (key.empty())
        {
            Log("[BluePrint] blueprint id %d has no registered name, so ownership could "
                "not be stored - call RegisterDataBase before granting it\n",
                PublicId(id));
            return false;
        }
        return SetKey(key.c_str(), owned);
    }
}
