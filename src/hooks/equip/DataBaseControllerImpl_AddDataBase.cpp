#include "pch.h"
#include "DataBaseControllerImpl_AddDataBase.h"

#include <Windows.h>
#include <cstdint>

#include "CustomBluePrint.h"
#include "../../core/V_FrameWorkState.h"
#include "HookUtils.h"
#include "AddressSet.h"
#include "log.h"
#include "../../lua/LuaApi.h"

#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "FoxHashes.h"

namespace
{
    constexpr std::uint16_t kNoDataBaseId   = 0xFFFF;
    constexpr std::uint16_t kFlagByteCount  = 0x1CB;
    constexpr std::int32_t  kMaxRowsPerTab  = 108;

    constexpr std::size_t   kCtl_TempBuffer   = 0x320;
    constexpr std::size_t   kTempBuf_Count    = 0x40A;
    constexpr std::size_t   kTempBuf_Stride   = 8;
    constexpr std::uint16_t kTempBuf_Capacity = 0x80;
    constexpr std::uint32_t kTempEntryMarker  = 0xBF169F98;

    using AddDataBase_t = void(__fastcall*)(void* self, std::uint16_t id, bool isNew);
    using IsGotDataBase_t = int(__fastcall*)(lua_State* L);
    using AddTempDataBase_t = int(__fastcall*)(lua_State* L);

    void* g_Controller = nullptr;

    AddDataBase_t g_Orig = nullptr;
    IsGotDataBase_t g_OrigIsGot = nullptr;
    AddTempDataBase_t g_OrigAddTemp = nullptr;
    bool g_Installed = false;
    bool g_IsGotInstalled = false;
    bool g_AddTempInstalled = false;
    bool g_OutOfRangeLogged = false;
    bool g_IsGotOutOfRangeLogged = false;
    bool g_AddTempOutOfRangeLogged = false;

    void __fastcall hkAddDataBase(void* self, std::uint16_t id, bool isNew)
    {
        if (id == kNoDataBaseId)
            return;

        const std::int32_t slot = bluePrint::SlotFromPublicId(static_cast<std::int32_t>(id));
        if (slot > 0)
        {
            if (!bluePrint::Set(slot, true))
            {
                Log("[BluePrint] custom blueprint id %u could not be stored - the DataBase "
                    "flag array was unreachable, so the blueprint is not granted\n",
                    static_cast<unsigned>(id));
            }
            else if (isNew)
            {
                bluePrint::SetNew(slot, true);
            }
            return;
        }

        if (id < kFlagByteCount)
        {
            if (g_Orig)
                g_Orig(self, id, isNew);
            return;
        }

        if (!g_OutOfRangeLogged)
        {
            g_OutOfRangeLogged = true;
            Log("[BluePrint] dataBaseId %u is past the %u-entry flag array and is not a custom "
                "blueprint id - the grant was dropped to stop it corrupting neighbouring save "
                "vars\n", static_cast<unsigned>(id), static_cast<unsigned>(kFlagByteCount));
        }
    }

    bool ReadDataBaseIdArg(lua_State* L, std::int32_t& out)
    {
        if (!ResolveLuaApi())
            return false;

        if (LuaIsNumber(L, 1))
        {
            out = GetLuaInt(L, 1);
            return true;
        }

        if (LuaType(L, 1) != LUA_TTABLE)
            return false;

        const int top = GetLuaTop(L);
        LuaGetField(L, 1, "dataBaseId");
        const bool ok = LuaIsNumber(L, -1);
        if (ok)
            out = GetLuaInt(L, -1);
        g_lua_settop(L, top);
        return ok;
    }

    constexpr std::int32_t kRewardCategoryMbManagement = 32;
    constexpr std::int32_t kRewardTypeKeyItem          = 4;
    constexpr std::int32_t kRewardLangFirstCustom      = 58;
    constexpr std::int32_t kRewardLangCeiling          = 255;
    constexpr const char*  kRewardLangSpace            = "REWARDLANG32";

    bool ReadRewardStackSize(lua_State* L, std::int32_t& out)
    {
        const int top = GetLuaTop(L);
        bool ok = false;

        LuaGetField(L, LUA_GLOBALSINDEX_51, "gvars");
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            LuaGetField(L, -1, "rwd_mbManagementRewardStackSize");
            if (LuaIsNumber(L, -1))
            {
                out = GetLuaInt(L, -1);
                ok = true;
            }
        }

        g_lua_settop(L, top);
        return ok;
    }

    bool PushRewardLangTable(lua_State* L)
    {
        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppReward");
        if (LuaType(L, -1) != LUA_TTABLE)
            return false;

        LuaGetField(L, -1, "LANG_ENUM");
        if (LuaType(L, -1) != LUA_TTABLE)
            return false;

        PushLuaNumber(L, static_cast<float>(kRewardCategoryMbManagement));
        g_lua_gettable(L, -2);
        return LuaType(L, -1) == LUA_TTABLE;
    }

    std::string RewardLangIdForDataBaseId(lua_State* L, std::int32_t publicId)
    {
        const int top = GetLuaTop(L);
        std::string langId;

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppTerminal");
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            LuaGetField(L, -1, "keyItemRewardTable");
            if (LuaType(L, -1) == LUA_TTABLE)
            {
                PushLuaNumber(L, static_cast<float>(publicId));
                g_lua_gettable(L, -2);
                if (const char* text = GetLuaString(L, -1))
                    langId = text;
            }
        }

        g_lua_settop(L, top);
        return langId;
    }

    void EnsureRewardLangRegistered(lua_State* L, std::int32_t publicId)
    {
        const std::string langId = RewardLangIdForDataBaseId(L, publicId);
        if (langId.empty())
            return;

        const int top = GetLuaTop(L);

        if (!PushRewardLangTable(L))
        {
            g_lua_settop(L, top);
            return;
        }

        LuaGetField(L, -1, langId.c_str());
        const bool already = LuaIsNumber(L, -1);
        LuaPop(L, 1);
        if (already)
        {
            g_lua_settop(L, top);
            return;
        }

        std::int32_t queued = 0;
        const bool queueEmpty = ReadRewardStackSize(L, queued) && queued <= 0;

        std::int32_t index = kRewardLangFirstCustom;
        if (!queueEmpty)
        {
            for (; index <= kRewardLangCeiling; ++index)
            {
                LuaRawGetI(L, -1, index);
                const bool taken = (LuaType(L, -1) != LUA_TNIL);
                LuaPop(L, 1);
                if (!taken)
                    break;
            }

            if (index > kRewardLangCeiling)
            {
                Log("[BluePrint] %d reward names are already queued unflushed, which is "
                    "every TppReward.LANG_ENUM[%d] slot from %d to %d, so reward lang '%s' "
                    "was not registered and TppReward.Push will silently drop it\n",
                    queued, kRewardCategoryMbManagement, kRewardLangFirstCustom,
                    kRewardLangCeiling, langId.c_str());
                g_lua_settop(L, top);
                return;
            }
        }

        LuaRawGetI(L, -1, index);
        std::string evicted;
        if (const char* prev = GetLuaString(L, -1))
            evicted = prev;
        LuaPop(L, 1);

        if (!evicted.empty() && evicted != langId)
        {
            PushLuaString(L, evicted.c_str());
            PushLuaNil(L);
            g_lua_settable(L, -3);
            V_FrameWorkState::SetPersistedConstant(kRewardLangSpace, evicted.c_str(), 0);
        }

        V_FrameWorkState::SetPersistedConstant(kRewardLangSpace, langId.c_str(), index);

        PushLuaString(L, langId.c_str());
        PushLuaNumber(L, static_cast<float>(index));
        g_lua_settable(L, -3);

        PushLuaNumber(L, static_cast<float>(index));
        PushLuaString(L, langId.c_str());
        g_lua_settable(L, -3);

        g_lua_settop(L, top);

        Log("[BluePrint] reward lang '%s' registered as LANG_ENUM[%d] index %d\n",
            langId.c_str(), kRewardCategoryMbManagement, index);
    }

    std::mutex g_DocMutex;
    std::unordered_map<std::int32_t, bluePrintDb::Documentation> g_DocById;

    void CacheBluePrintNameLangId(lua_State* L, std::int32_t publicId)
    {
        {
            std::lock_guard<std::mutex> lock(g_DocMutex);
            const auto it = g_DocById.find(publicId);
            if (it != g_DocById.end() && it->second.nameHash != 0)
                return;
        }

        const int top = GetLuaTop(L);
        std::string langId;

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppTerminal");
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            LuaGetField(L, -1, "keyItemAnnounceLogTable");
            if (LuaType(L, -1) == LUA_TTABLE)
            {
                PushLuaNumber(L, static_cast<float>(publicId));
                g_lua_gettable(L, -2);
                if (const char* text = GetLuaString(L, -1))
                    langId = text;
            }
        }

        g_lua_settop(L, top);

        if (langId.empty())
            return;

        const std::uint64_t hash = FoxHashes::StrCode64(langId);
        if (hash == 0)
            return;

        std::lock_guard<std::mutex> lock(g_DocMutex);
        bluePrintDb::Documentation& doc = g_DocById[publicId];
        if (doc.nameHash == 0)
            doc.nameHash = hash;
    }

    std::unordered_set<std::int32_t> g_AnnounceLangReported;

    void ReportBluePrintAnnounceLang(lua_State* L, std::int32_t publicId)
    {
        {
            std::lock_guard<std::mutex> lock(g_DocMutex);
            if (!g_AnnounceLangReported.insert(publicId).second)
                return;
        }

        const int top = GetLuaTop(L);
        std::string langId;

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppTerminal");
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            LuaGetField(L, -1, "BLUE_PRINT_LANG_ID");
            if (LuaType(L, -1) == LUA_TTABLE)
            {
                PushLuaNumber(L, static_cast<float>(publicId));
                g_lua_gettable(L, -2);
                if (const char* text = GetLuaString(L, -1))
                    langId = text;
            }
        }

        g_lua_settop(L, top);

        if (langId.empty())
        {
            Log("[BluePrint] TppTerminal.BLUE_PRINT_LANG_ID[%d] is unset - PickUpBluePrint has no name to put in its announce, so the HUD line shows the raw format string\n",
                publicId);
            return;
        }

#ifdef _DEBUG
        LogDebug("[BluePrintAnnounce] AddTempDataBase reached for blueprint id %d, BLUE_PRINT_LANG_ID='%s' - PickUpBluePrint raises the get_blueprint announce as soon as this returns\n", publicId, langId.c_str());
#endif
    }

    std::int32_t g_WantedLangIndex = 0;
    std::string  g_FoundLangName;

    void MatchPersistedLangIndex(const char* name, std::int32_t value)
    {
        if (name && value == g_WantedLangIndex && g_FoundLangName.empty())
            g_FoundLangName = name;
    }

    bool ReadRewardLangEnumAt(lua_State* L, std::int32_t slot, std::int32_t& out)
    {
        const int top = GetLuaTop(L);
        bool ok = false;

        LuaGetField(L, LUA_GLOBALSINDEX_51, "gvars");
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            LuaGetField(L, -1, "rwd_mbManagementRewardLangEnum");
            if (LuaType(L, -1) != LUA_TNIL)
            {
                PushLuaNumber(L, static_cast<float>(slot));
                g_lua_gettable(L, -2);
                if (LuaIsNumber(L, -1))
                {
                    out = GetLuaInt(L, -1);
                    ok = true;
                }
            }
        }

        g_lua_settop(L, top);
        return ok;
    }

    void RestoreQueuedRewardLangs(lua_State* L)
    {
        std::int32_t queued = 0;
        if (!ReadRewardStackSize(L, queued) || queued <= 0)
            return;

        for (std::int32_t i = 0; i < queued; ++i)
        {
            std::int32_t index = 0;
            if (!ReadRewardLangEnumAt(L, i, index))
                continue;
            if (index < kRewardLangFirstCustom || index > kRewardLangCeiling)
                continue;

            const int top = GetLuaTop(L);
            if (!PushRewardLangTable(L))
            {
                g_lua_settop(L, top);
                return;
            }

            LuaRawGetI(L, -1, index);
            const bool present = (LuaType(L, -1) != LUA_TNIL);
            LuaPop(L, 1);
            if (present)
            {
                g_lua_settop(L, top);
                continue;
            }

            g_WantedLangIndex = index;
            g_FoundLangName.clear();
            V_FrameWorkState::ForEachPersistedConstant(kRewardLangSpace,
                                                       &MatchPersistedLangIndex);
            const std::string name = g_FoundLangName;
            g_FoundLangName.clear();

            if (name.empty())
            {
                g_lua_settop(L, top);
                Log("[BluePrint] a reward queued before this launch used LANG_ENUM[%d] index %d, "
                    "which no persisted name claims, so the REWARDS screen will render that row "
                    "blank\n", kRewardCategoryMbManagement, index);
                continue;
            }

            PushLuaString(L, name.c_str());
            PushLuaNumber(L, static_cast<float>(index));
            g_lua_settable(L, -3);

            PushLuaNumber(L, static_cast<float>(index));
            PushLuaString(L, name.c_str());
            g_lua_settable(L, -3);

            g_lua_settop(L, top);
        }
    }

    bool PushCustomReward(lua_State* L, std::int32_t publicId)
    {
        const std::string langId = RewardLangIdForDataBaseId(L, publicId);
        if (langId.empty())
            return false;

        const int top = GetLuaTop(L);

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppReward");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            g_lua_settop(L, top);
            return false;
        }

        LuaGetField(L, -1, "Push");
        if (LuaType(L, -1) != LUA_TFUNCTION)
        {
            g_lua_settop(L, top);
            return false;
        }

        g_lua_createtable(L, 0, 3);

        PushLuaString(L, "category");
        PushLuaNumber(L, static_cast<float>(kRewardCategoryMbManagement));
        g_lua_rawset(L, -3);

        PushLuaString(L, "langId");
        PushLuaString(L, langId.c_str());
        g_lua_rawset(L, -3);

        PushLuaString(L, "rewardType");
        PushLuaNumber(L, static_cast<float>(kRewardTypeKeyItem));
        g_lua_rawset(L, -3);

        const int rc = g_lua_pcall(L, 1, 0, 0);
        if (rc != 0)
        {
            const char* err = GetLuaString(L, -1);
            Log("[BluePrint] TppReward.Push failed for reward lang '%s' (%s), so the pickup will "
                "not appear on the REWARDS screen\n", langId.c_str(), err ? err : "no message");
            g_lua_settop(L, top);
            return false;
        }

        g_lua_settop(L, top);
        return true;
    }

    int __fastcall hkIsGotDataBase(lua_State* L)
    {
        std::int32_t id = 0;
        if (!ReadDataBaseIdArg(L, id) || id == kNoDataBaseId)
            return g_OrigIsGot(L);

        const std::int32_t slot = bluePrint::SlotFromPublicId(id);
        if (slot > 0)
        {
            const bool owned = bluePrint::Has(slot);
            CacheBluePrintNameLangId(L, id);
            RestoreQueuedRewardLangs(L);
            Log("[BluePrint] IsGotDataBase(%d) -> %s%s\n", id, owned ? "true" : "false",
                owned ? " - a caller gated on 'not owned' (AcquireKeyItem, PickUpBluePrint) "
                        "will do nothing, including its announce and reward" : "");
            PushLuaBool(L, owned);
            return 1;
        }

        if (id >= 0 && id < static_cast<std::int32_t>(kFlagByteCount))
            return g_OrigIsGot(L);

        if (!g_IsGotOutOfRangeLogged)
        {
            g_IsGotOutOfRangeLogged = true;
            Log("[BluePrint] IsGotDataBase was asked about dataBaseId %d, which is past the "
                "%u-entry flag array and is not a custom blueprint id - answered false instead of "
                "letting the unbounded vanilla read return a neighbouring save var\n",
                id, static_cast<unsigned>(kFlagByteCount));
        }

        PushLuaBool(L, false);
        return 1;
    }

    bool BagCustomTempDataBaseSeh(void* controller, std::uint16_t id)
    {
        if (!controller)
            return false;

        __try
        {
            std::uint8_t* buffer = static_cast<std::uint8_t*>(controller) + kCtl_TempBuffer;
            auto* count = reinterpret_cast<std::uint16_t*>(buffer + kTempBuf_Count);

            const std::uint16_t used = *count;
            if (used >= kTempBuf_Capacity)
                return false;

            std::uint8_t* entry = buffer + static_cast<std::size_t>(used) * kTempBuf_Stride;
            *reinterpret_cast<std::uint16_t*>(entry)     = id;
            *reinterpret_cast<std::uint32_t*>(entry + 4) = kTempEntryMarker;
            *count = static_cast<std::uint16_t>(used + 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int __fastcall hkAddTempDataBase(lua_State* L)
    {
        std::int32_t id = 0;
        if (!ReadDataBaseIdArg(L, id) || id == kNoDataBaseId)
            return g_OrigAddTemp(L);

        const std::int32_t slot = bluePrint::SlotFromPublicId(id);
        if (slot > 0)
        {
            ReportBluePrintAnnounceLang(L, id);

            if (BagCustomTempDataBaseSeh(g_Controller, static_cast<std::uint16_t>(id)))
            {
                CacheBluePrintNameLangId(L, id);
                RestoreQueuedRewardLangs(L);
                EnsureRewardLangRegistered(L, id);
                PushCustomReward(L, id);
                return 0;
            }

            if (!bluePrint::Set(slot, true))
            {
                Log("[BluePrint] custom blueprint id %d reached neither the temp buffer nor the "
                    "flag store, so the pickup was lost outright\n", id);
            }
            else
            {
                Log("[BluePrint] custom blueprint id %d could not be held in the temp buffer "
                    "(full at %u entries, or the DataBase controller was unreachable), so it was "
                    "granted at once instead of on extraction\n",
                    id, static_cast<unsigned>(kTempBuf_Capacity));
            }
            return 0;
        }

        if (id >= 0 && id < static_cast<std::int32_t>(kFlagByteCount))
            return g_OrigAddTemp(L);

        if (!g_AddTempOutOfRangeLogged)
        {
            g_AddTempOutOfRangeLogged = true;
            Log("[BluePrint] AddTempDataBase was handed dataBaseId %d, which is past the %u-entry "
                "flag array and is not a custom blueprint id - dropped, because the vanilla path "
                "maps an unknown id to category 0xFF and increments a byte far outside its "
                "counter block\n", id, static_cast<unsigned>(kFlagByteCount));
        }

        return 0;
    }

    void EnsureAddTempDataBaseHook()
    {
        if (g_AddTempInstalled || !gAddr.MbmImpl_AddTempDataBase)
            return;

        g_AddTempInstalled = true;

        void* target = ResolveGameAddress(gAddr.MbmImpl_AddTempDataBase);
        if (!CreateAndEnableHook(target, &hkAddTempDataBase,
                                 reinterpret_cast<void**>(&g_OrigAddTemp)))
        {
            g_OrigAddTemp = nullptr;
            Log("[BluePrint] ERROR: the AddTempDataBase guard was refused at %p - routing a custom "
                "blueprint through TppTerminal.PickUpBluePrint would corrupt the Mother Base "
                "management block; do not use that path on this build\n", target);
        }
    }

    void EnsureIsGotDataBaseHook()
    {
        if (g_IsGotInstalled || !gAddr.MbmImpl_IsGotDataBase)
            return;

        g_IsGotInstalled = true;

        void* target = ResolveGameAddress(gAddr.MbmImpl_IsGotDataBase);
        if (!CreateAndEnableHook(target, &hkIsGotDataBase,
                                 reinterpret_cast<void**>(&g_OrigIsGot)))
        {
            g_OrigIsGot = nullptr;
            Log("[BluePrint] ERROR: the IsGotDataBase guard was refused at %p - vanilla Lua asking "
                "about a custom blueprint reads past the DataBase flag array and gets a garbage "
                "answer\n", target);
        }
    }

    void* ReadVirtualSeh(void* controller)
    {
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(controller);
            return vtbl ? vtbl[0] : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }
}

namespace bluePrintDb
{
    bool SetDocumentation(std::int32_t publicId, const Documentation& doc)
    {
        std::lock_guard<std::mutex> lock(g_DocMutex);

        auto existing = g_DocById.find(publicId);
        if (existing == g_DocById.end() || existing->second.tab != doc.tab)
        {
            std::size_t onTab = 0;
            for (const auto& kv : g_DocById)
                if (kv.second.tab == doc.tab && kv.first != publicId)
                    ++onTab;

            if (onTab >= static_cast<std::size_t>(kMaxRowsPerTab))
            {
                Log("[BluePrint] dataBaseId %d was not given a DATABASE row - tab %u already "
                    "holds %zu custom entries and the tab renders at most %d rows in total, so "
                    "this row could never be drawn; move it to another tab\n",
                    publicId, static_cast<unsigned>(doc.tab), onTab, kMaxRowsPerTab);
                return false;
            }
        }

        g_DocById[publicId] = doc;
        return true;
    }

    bool TryGetDocumentation(std::int32_t publicId, Documentation& out)
    {
        std::lock_guard<std::mutex> lock(g_DocMutex);
        auto it = g_DocById.find(publicId);
        if (it == g_DocById.end())
            return false;
        out = it->second;
        return true;
    }

    bool AnnounceBluePrintObtained(lua_State* L, std::int32_t publicId)
    {
        if (!L || !ResolveLuaApi())
            return false;

        const int top = GetLuaTop(L);
        std::string langId;

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppTerminal");
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            LuaGetField(L, -1, "BLUE_PRINT_LANG_ID");
            if (LuaType(L, -1) == LUA_TTABLE)
            {
                PushLuaNumber(L, static_cast<float>(publicId));
                g_lua_gettable(L, -2);
                if (const char* text = GetLuaString(L, -1))
                    langId = text;
            }
        }
        g_lua_settop(L, top);

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppUI");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            g_lua_settop(L, top);
            return false;
        }

        LuaGetField(L, -1, "ShowAnnounceLog");
        if (LuaType(L, -1) != LUA_TFUNCTION)
        {
            g_lua_settop(L, top);
            return false;
        }

        PushLuaString(L, "get_blueprint");
        if (langId.empty())
            PushLuaNil(L);
        else
            PushLuaString(L, langId.c_str());

        const int rc = g_lua_pcall(L, 2, 0, 0);
        if (rc != 0)
        {
            const char* err = GetLuaString(L, -1);
            Log("[BluePrint] TppUI.ShowAnnounceLog failed for blueprint id %d (%s), so the grant is silent\n", publicId, err ? err : "no message");
            g_lua_settop(L, top);
            return false;
        }

        g_lua_settop(L, top);
        return true;
    }

    void ForEachDocumentedId(std::uint8_t tab, void (*fn)(std::int32_t publicId))
    {
        if (!fn)
            return;

        std::vector<std::int32_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_DocMutex);
            for (const auto& entry : g_DocById)
            {
                if (entry.second.tab == tab)
                    ids.push_back(entry.first);
            }
        }

        for (const std::int32_t id : ids)
            fn(id);
    }

    void EnsureAddDataBaseHook(void* controller)
    {
        if (controller)
            g_Controller = controller;

        EnsureIsGotDataBaseHook();
        EnsureAddTempDataBaseHook();

        if (g_Installed || !controller)
            return;

        void* target = ReadVirtualSeh(controller);
        if (!target)
            return;

        if (!CreateAndEnableHook(target, &hkAddDataBase,
                                 reinterpret_cast<void**>(&g_Orig)))
        {
            g_Installed = true;
            Log("[BluePrint] ERROR: the AddDataBase guard was refused at %p - granting a custom "
                "blueprint through TppTerminal.PickUpBluePrint would write past the DataBase "
                "flag array and corrupt the save\n", target);
            return;
        }

        g_Installed = true;
    }

    void RemoveAddDataBaseHook()
    {
        g_Installed = false;
        g_Orig = nullptr;
        g_IsGotInstalled = false;
        g_OrigIsGot = nullptr;
        g_AddTempInstalled = false;
        g_OrigAddTemp = nullptr;
    }
}
