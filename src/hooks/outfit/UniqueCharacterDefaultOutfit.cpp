#include "pch.h"

#include "UniqueCharacterDefaultOutfit.h"

#include "OutfitRegistry.h"
#include "FoxHashes.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"
#include "../../lua/LuaApi.h"
#include "../../core/V_FrameWorkState.h"
#include "log.h"

#include <atomic>
#include <cstring>

namespace
{
    struct DefaultOutfitSpec
    {
        std::uint8_t playerType;
        const char*  key;
        const char*  branch;
        const char*  partsPath;
        const char*  fpkPath;
        bool         quietMovement;
        const char*  langEquipName;
        const char*  langEquipInfo;
        const char*  langEquipRealName;
        const char*  iconFtexPath;
    };

    const DefaultOutfitSpec kDefaults[] =
    {
        { outfit::kPlayerType_Quiet,
          "V_Framework:QuietDefault", "quiet",
          "/Assets/tpp/parts/chara/qui/quip_main0_def_v00.parts",
          "/Assets/tpp/pack/player/parts/plparts_quiet.fpk",
          true,
          "name_qe_51000", "info_qe_51000", "name_qe_51000",
          "/Assets/tpp/ui/texture/EquipIcon/buddy/ui_qwp_suit_qui0_alp", },

        { outfit::kPlayerType_Ocelot,
          "V_Framework:OcelotDefault", "ocelot",
          "/Assets/tpp/parts/chara/ooc/ooc0_main1_def_v00.parts",
          "/Assets/tpp/pack/player/parts/plparts_ocelot.fpk",
          false,
          "name_dd_24007", "name_dd_24007", "name_dd_24007",
          "/Assets/tpp/ui/texture/EquipIcon/suit/ui_st_ooc0_alp", },
    };

    constexpr std::size_t kDefaultCount =
        sizeof(kDefaults) / sizeof(kDefaults[0]);

    constexpr std::uint8_t kQuietSuitParamKind = 0x74;

    constexpr int kSeedEquipId          = 508;
    constexpr int kSeedDevelopTypeId    = 20;

    constexpr int kMaxAttempts    = 8;
    constexpr int kMaxGlobalWaits = 20000;

    std::atomic<bool> g_Done{ false };
    std::atomic<int>  g_Attempts{ 0 };
    std::atomic<int>  g_GlobalWaits{ 0 };
    std::atomic<bool> g_GaveUpLogged{ false };
    std::atomic<bool> g_InProgress{ false };
    std::atomic<bool> g_RegisteringFobAllowed{ false };

    int SlotOf(std::uint8_t playerType)
    {
        for (std::size_t i = 0; i < kDefaultCount; ++i)
            if (kDefaults[i].playerType == playerType) return static_cast<int>(i);
        return -1;
    }

    bool LuaReady()
    {
        return g_lua_getfield && g_lua_pushstring && g_lua_pushnumber
            && g_lua_pushboolean && g_lua_createtable && g_lua_settable
            && g_lua_gettop && g_lua_settop && g_lua_type && g_lua_pcall
            && g_lua_isnumber && g_lua_tonumber && g_lua_pushvalue;
    }

    void PushStr(lua_State* L, const char* s)
    {
        g_lua_pushstring(L, const_cast<char*>(s));
    }

    void GetField(lua_State* L, int idx, const char* k)
    {
        g_lua_getfield(L, idx, const_cast<char*>(k));
    }

    void SetStrField(lua_State* L, const char* k, const char* v)
    {
        PushStr(L, k);
        PushStr(L, v);
        g_lua_settable(L, -3);
    }

    void SetNumField(lua_State* L, const char* k, double v)
    {
        PushStr(L, k);
        g_lua_pushnumber(L, v);
        g_lua_settable(L, -3);
    }

    void SetBoolField(lua_State* L, const char* k, bool v)
    {
        PushStr(L, k);
        g_lua_pushboolean(L, v ? 1 : 0);
        g_lua_settable(L, -3);
    }

    bool TryGetGlobalNumber(lua_State* L, const char* table, const char* field,
                            double* out)
    {
        const int top = g_lua_gettop(L);
        bool ok = false;

        GetField(L, LUA_GLOBALSINDEX_51, table);
        if (g_lua_type(L, -1) == LUA_TTABLE)
        {
            GetField(L, -1, field);
            if (g_lua_isnumber(L, -1))
            {
                *out = g_lua_tonumber(L, -1);
                ok = true;
            }
        }

        g_lua_settop(L, top);
        return ok;
    }

    bool CallLibFunction(lua_State* L, const char* lib, const char* fn,
                         int argCount)
    {
        GetField(L, LUA_GLOBALSINDEX_51, lib);
        if (g_lua_type(L, -1) != LUA_TTABLE) return false;

        GetField(L, -1, fn);
        if (g_lua_type(L, -1) != LUA_TFUNCTION) return false;

        for (int i = 0; i < argCount; ++i)
            g_lua_pushvalue(L, -2 - argCount);

        return g_lua_pcall(L, argCount, 0, 0) == 0;
    }

    bool RegisterOutfitViaLua(lua_State* L, const DefaultOutfitSpec& spec)
    {
        const int top = g_lua_gettop(L);

        g_lua_createtable(L, 0, 2);
        SetStrField(L, "key", spec.key);

        PushStr(L, spec.branch);
        g_lua_createtable(L, 0, 3);
        SetStrField(L, "partsPath", spec.partsPath);
        SetStrField(L, "fpkPath", spec.fpkPath);
        if (spec.quietMovement)
        {
            PushStr(L, "abilities");
            g_lua_createtable(L, 0, 1);
            SetBoolField(L, "quietMovement", true);
            g_lua_settable(L, -3);
        }
        g_lua_settable(L, -3);

        const bool ok = CallLibFunction(L, "V_Player", "RegisterOutfit", 1);
        g_lua_settop(L, top);
        return ok;
    }

    bool AddDevelopRowViaLua(lua_State* L, const DefaultOutfitSpec& spec,
                             double equipId, double developTypeId)
    {
        const int top = g_lua_gettop(L);

        PushStr(L, spec.key);

        g_lua_createtable(L, 0, 2);

        PushStr(L, "const");
        g_lua_createtable(L, 0, 6);
        SetNumField(L, "equipID", equipId);
        SetNumField(L, "equipDevelopTypeID", developTypeId);
        SetStrField(L, "langEquipName", spec.langEquipName);
        SetStrField(L, "langEquipInfo", spec.langEquipInfo);
        SetStrField(L, "langEquipRealName", spec.langEquipRealName);
        SetStrField(L, "iconFtexPath", spec.iconFtexPath);
        g_lua_settable(L, -3);

        PushStr(L, "flow");
        g_lua_createtable(L, 0, 3);
        SetNumField(L, "grade", 1);
        SetNumField(L, "developGmpCost", 0);
        SetNumField(L, "initialAvailable", 1);
        g_lua_settable(L, -3);

        const bool ok = CallLibFunction(
            L, "V_TppMotherBaseManagement", "AddToEquipDevelopTable", 2);
        g_lua_settop(L, top);
        return ok;
    }
}

namespace uniquedefaultoutfit
{
    bool TryGetBindingFor(std::uint8_t playerType,
                          std::uint8_t* outPartsType,
                          std::uint8_t* outSelector)
    {
        const int slot = SlotOf(playerType);
        if (slot < 0) return false;

        const outfit::OutfitEntry* entry = nullptr;
        if (!outfit::TryGetOutfitByKey(kDefaults[slot].key, &entry) || !entry)
            return false;
        if (!entry->bound || entry->partsType == 0) return false;

        if (outPartsType) *outPartsType = entry->partsType;
        if (outSelector)  *outSelector  = entry->selectorCode;
        return true;
    }

    void QueueDevelopRowsEarly()
    {
        static std::atomic<bool> s_queued{ false };
        if (s_queued.exchange(true)) return;

        for (const DefaultOutfitSpec& spec : kDefaults)
        {
            const outfit::OutfitEntry* existing = nullptr;
            if (outfit::TryGetOutfitByKey(spec.key, &existing) && existing)
                continue;

            std::int32_t developId = 0;
            if (!V_FrameWorkState::ResolveOrCreateDevelopId(spec.key, 0, developId)
                || developId <= 0 || developId > 0xFFFF)
            {
                Log("[UniqueDefaultOutfit] no developId could be reserved for '%s' "
                    "at start-up, so its develop row is only injected once the Lua "
                    "pass registers the outfit - by then the persisted outfits hold "
                    "the low rows and it sorts last in the Uniform list\n", spec.key);
                continue;
            }
            V_FrameWorkState::SetRowKind(spec.key,
                                         V_FrameWorkState::kRowKindOutfit);

            outfit::OutfitDefinition def{};
            def.key       = spec.key;
            def.developId = static_cast<std::uint16_t>(developId);

            const std::uint8_t persistedParts =
                V_FrameWorkState::GetPersistedOutfitPartsType(spec.key);
            const std::uint8_t persistedSel =
                V_FrameWorkState::GetPersistedOutfitSelector(spec.key);
            if (persistedParts != 0) def.partsTypeHint    = persistedParts;
            if (persistedSel   != 0) def.selectorCodeHint = persistedSel;

            outfit::OutfitPlayerTypeData& branch =
                def.perPlayerType[spec.playerType];
            branch.used            = true;
            branch.partsPathCode64 = FoxHashes::PathCode64Ext(spec.partsPath);
            branch.fpkPathCode64   = FoxHashes::PathCode64Ext(spec.fpkPath);
            branch.enableArm       = true;
            branch.enableHead      = true;
            if (spec.quietMovement) branch.suitParamKind = kQuietSuitParamKind;

            std::uint8_t allocated = 0;
            if (!outfit::RegisterOutfit(def, &allocated))
                Log("[UniqueDefaultOutfit] the outfit registry refused '%s' at "
                    "start-up, so its develop row cannot be injected before the "
                    "persisted outfits and it lands last in the Uniform list\n",
                    spec.key);
        }

        for (const DefaultOutfitSpec& spec : kDefaults)
        {
            const EquipDevelopAdd::NativeDevelopNumber constNums[] = {
                { "p01", kSeedEquipId },
                { "p02", kSeedDevelopTypeId },
            };
            const EquipDevelopAdd::NativeDevelopString constStrs[] = {
                { "p06", spec.langEquipName },
                { "p07", spec.langEquipInfo },
                { "p30", spec.langEquipRealName },
                { "p08", spec.iconFtexPath },
            };
            const EquipDevelopAdd::NativeDevelopNumber flowNums[] = {
                { "p52", 1 },
                { "p53", 0 },
                { "p62", 1 },
            };

            if (!EquipDevelopAdd::QueueNativeDevelopRequest(
                    spec.key, constNums, 2, constStrs, 4, flowNums, 3))
                Log("[UniqueDefaultOutfit] could not queue the develop row for "
                    "'%s' before the persisted outfits, so it lands after them in "
                    "the Uniform list\n", spec.key);
        }
    }

    bool EnsureRegistered(lua_State* L)
    {
        if (g_Done.load(std::memory_order_acquire)) return true;
        if (!L || !LuaReady()) return false;
        if (g_InProgress.exchange(true)) return false;

        double equipId = 0.0;
        double developTypeId = 0.0;
        if (!TryGetGlobalNumber(L, "TppEquip", "EQP_SUIT", &equipId)
         || !TryGetGlobalNumber(L, "TppMbDev", "EQP_DEV_TYPE_Suit",
                                &developTypeId))
        {
            if (g_GlobalWaits.fetch_add(1, std::memory_order_relaxed)
                    < kMaxGlobalWaits)
                g_InProgress.store(false, std::memory_order_relaxed);
            else if (!g_GaveUpLogged.exchange(true))
                Log("[UniqueDefaultOutfit] TppEquip.EQP_SUIT or "
                    "TppMbDev.EQP_DEV_TYPE_Suit never appeared in the Lua globals, "
                    "so Ocelot and Quiet get no develop row of their own and their "
                    "Uniform list keeps borrowing a vanilla suit row with the wrong "
                    "name, icon and details\n");
            return false;
        }

        if (g_Attempts.fetch_add(1, std::memory_order_relaxed) >= kMaxAttempts)
        {
            if (!g_GaveUpLogged.exchange(true))
                Log("[UniqueDefaultOutfit] the built-in default-outfit rows for "
                    "Ocelot and Quiet were refused %d times, so both characters keep "
                    "borrowing a vanilla suit row with the wrong name, icon and "
                    "details\n", kMaxAttempts);
            return false;
        }

        int registered = 0;
        for (std::size_t i = 0; i < kDefaultCount; ++i)
        {
            const DefaultOutfitSpec& spec = kDefaults[i];

            const outfit::OutfitEntry* already = nullptr;
            const bool haveEntry =
                outfit::TryGetOutfitByKey(spec.key, &already) && already;

            if (!haveEntry && !RegisterOutfitViaLua(L, spec))
            {
                Log("[UniqueDefaultOutfit] V_Player.RegisterOutfit refused '%s', so "
                    "that character keeps the borrowed vanilla suit row\n", spec.key);
                continue;
            }

            g_RegisteringFobAllowed.store(true, std::memory_order_relaxed);
            const bool rowOk =
                AddDevelopRowViaLua(L, spec, equipId, developTypeId);
            g_RegisteringFobAllowed.store(false, std::memory_order_relaxed);
            if (!rowOk)
            {
                Log("[UniqueDefaultOutfit] the develop row for '%s' was refused, so "
                    "the outfit exists but never reaches the Uniform list\n",
                    spec.key);
                continue;
            }

            ++registered;
        }

        g_InProgress.store(false, std::memory_order_relaxed);
        if (registered == 0) return false;

        g_Done.store(true, std::memory_order_release);

#ifdef _DEBUG
        LogDebug("[UniqueDefaultOutfit] registered %d default-outfit row(s) for "
                 "Ocelot and Quiet (equipID=%d equipDevelopTypeID=%d) - their own "
                 "suit is now a real develop row, so its name, info and icon come "
                 "from that row instead of a borrowed vanilla one\n",
                 registered, static_cast<int>(equipId),
                 static_cast<int>(developTypeId));
#endif
        return true;
    }

    bool TryGetPlayerTypeForBinding(std::uint8_t partsType,
                                    std::uint8_t selector,
                                    std::uint8_t* outPlayerType)
    {
        for (const DefaultOutfitSpec& spec : kDefaults)
        {
            std::uint8_t p = 0;
            std::uint8_t s = 0;
            if (!TryGetBindingFor(spec.playerType, &p, &s)) continue;
            if (partsType != 0)
            {
                if (partsType != p) continue;
            }
            else if (selector != s) continue;
            if (outPlayerType) *outPlayerType = spec.playerType;
            return true;
        }
        return false;
    }

    bool IsDefaultOutfitPartsType(std::uint8_t partsType,
                                  std::uint8_t* outPlayerType)
    {
        if (partsType == 0) return false;
        for (const DefaultOutfitSpec& spec : kDefaults)
        {
            std::uint8_t p = 0;
            std::uint8_t sel = 0;
            if (!TryGetBindingFor(spec.playerType, &p, &sel)) continue;
            if (p != partsType) continue;
            if (outPlayerType) *outPlayerType = spec.playerType;
            return true;
        }
        return false;
    }

    bool IsDefaultOutfitKey(const char* key)
    {
        if (!key || !key[0]) return false;
        for (const DefaultOutfitSpec& spec : kDefaults)
            if (std::strcmp(spec.key, key) == 0) return true;
        return false;
    }

    const char* GetDefaultOutfitKey(std::uint8_t playerType)
    {
        const int slot = SlotOf(playerType);
        return slot < 0 ? nullptr : kDefaults[slot].key;
    }

    bool IsRegisteringFobAllowedRow()
    {
        return g_RegisteringFobAllowed.load(std::memory_order_relaxed);
    }

    std::uint16_t GetDefaultOutfitRow(std::uint8_t playerType)
    {
        const int slot = SlotOf(playerType);
        if (slot < 0) return 0;

        const outfit::OutfitEntry* entry = nullptr;
        if (!outfit::TryGetOutfitByKey(kDefaults[slot].key, &entry) || !entry)
            return 0;
        if (!entry->used) return 0;

        if (entry->flowIndex != 0) return entry->flowIndex;
        if (entry->developId == 0) return 0;

        std::uint16_t injected = 0;
        if (EquipDevelopAdd::TryGetFlowIndexForDevelopId(entry->developId,
                                                        injected))
            return injected;
        return 0;
    }

    bool IsDefaultOutfitRow(std::uint8_t playerType, std::uint16_t flowIndex)
    {
        if (flowIndex == 0) return false;

        const int slot = SlotOf(playerType);
        if (slot < 0) return false;

        const outfit::OutfitEntry* entry = nullptr;
        if (!outfit::TryGetOutfitByKey(kDefaults[slot].key, &entry) || !entry)
            return false;
        if (!entry->used) return false;

        if (entry->flowIndex == flowIndex) return true;
        if (entry->developId == 0) return false;

        std::uint16_t injected = 0;
        return EquipDevelopAdd::TryGetFlowIndexForDevelopId(entry->developId,
                                                            injected)
            && injected == flowIndex;
    }

    bool IsRegisteredFor(std::uint8_t playerType)
    {
        const int slot = SlotOf(playerType);
        if (slot < 0) return false;

        const outfit::OutfitEntry* entry = nullptr;
        if (!outfit::TryGetOutfitByKey(kDefaults[slot].key, &entry) || !entry)
            return false;
        if (!entry->used) return false;

        if (entry->flowIndex != 0) return true;
        if (entry->developId == 0) return false;

        std::uint16_t flowIndex = 0;
        return EquipDevelopAdd::TryGetFlowIndexForDevelopId(entry->developId,
                                                            flowIndex)
            && flowIndex != 0;
    }
}
