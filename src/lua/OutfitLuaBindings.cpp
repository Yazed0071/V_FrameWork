#include "pch.h"

#include "OutfitLuaBindings.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "LuaApi.h"
#include "FoxHashes.h"
#include "log.h"
#include "V_FrameWorkState.h"
#include "../hooks/outfit/OutfitRegistry.h"
#include "../hooks/outfit/UniqueCharacterOwnSuit.h"
#include "../hooks/outfit/CustomHeadRegistry.h"
#include "../hooks/equip/EquipDevelop_AddToEquipDevelopTable.h"

static void SetLuaTop(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_settop)
        return;
    g_lua_settop(L, idx);
}

static void LuaPushNil(lua_State* L)
{
    if (!ResolveLuaApi() || !g_lua_pushnil)
        return;
    g_lua_pushnil(L);
}

static int LuaNext(lua_State* L, int idx)
{
    if (!ResolveLuaApi() || !g_lua_next)
        return 0;
    return g_lua_next(L, idx);
}

static bool TryReadTableStringField(lua_State* L, int tableIndex, const char* fieldName, const char*& outValue)
{
    outValue = nullptr;
    LuaGetField(L, tableIndex, const_cast<char*>(fieldName));

    const bool ok = (LuaType(L, -1) == 4);
    if (ok)
        outValue = GetLuaString(L, -1);

    SetLuaTop(L, -2);
    return ok;
}

static bool TryReadTableIntField(lua_State* L, int tableIndex, const char* fieldName, int& outValue)
{
    outValue = 0;
    LuaGetField(L, tableIndex, const_cast<char*>(fieldName));

    const bool ok = (LuaType(L, -1) == 3);
    if (ok)
        outValue = GetLuaInt(L, -1);

    SetLuaTop(L, -2);
    return ok;
}

static bool TryReadTableBoolField(lua_State* L, int tableIndex, const char* fieldName, bool defaultValue)
{
    LuaGetField(L, tableIndex, const_cast<char*>(fieldName));

    const int type = LuaType(L, -1);
    bool result = defaultValue;

    if (type != 0)
        result = GetLuaBool(L, -1) != 0;

    SetLuaTop(L, -2);
    return result;
}


namespace
{
    struct PlayerTypeBranchKey
    {
        const char*  key;
        std::uint8_t playerType;
    };
    static constexpr PlayerTypeBranchKey k_PtBranchKeys[] = {
        { "snake",    outfit::kPlayerType_Snake    },
        { "ddMale",   outfit::kPlayerType_DDMale   },
        { "ddFemale", outfit::kPlayerType_DDFemale },
        { "avatar",   outfit::kPlayerType_Avatar   },
        { "ocelot",   outfit::kPlayerType_Ocelot   },
        { "quiet",    outfit::kPlayerType_Quiet    },
    };

    static bool MatchPlayerTypeBranchKey(const char* name,
                                         std::uint8_t& outPlayerType)
    {
        if (!name || !name[0])
            return false;
        for (const auto& bk : k_PtBranchKeys)
        {
            if (_stricmp(name, bk.key) == 0)
            {
                outPlayerType = bk.playerType;
                return true;
            }
        }
        return false;
    }

    template <typename Fn>
    static void ForEachPlayerTypeBranch(lua_State* L, int tableIdx,
                                        const char* site, const char* key,
                                        Fn&& fn)
    {
        std::uint32_t seen = 0;
        LuaPushNil(L);
        while (LuaNext(L, tableIdx) != 0)
        {
            const int valueIdx = GetLuaTop(L);
            std::uint8_t playerType = 0;
            const char* branchName =
                (LuaType(L, -2) == LUA_TSTRING) ? GetLuaString(L, -2) : nullptr;
            if (branchName && LuaType(L, -1) == LUA_TTABLE
                && MatchPlayerTypeBranchKey(branchName, playerType))
            {
                const std::uint32_t bit = 1u << playerType;
                if (seen & bit)
                {
                    Log("[OutfitLua] %s: playerType branch declared more than once "
                        "(key=%s branch=%s) - Lua orders table keys arbitrarily, so "
                        "only the first spelling reached is applied\n",
                        site, key ? key : "(unnamed)", branchName);
                }
                else
                {
                    seen |= bit;
                    fn(playerType, branchName, valueIdx);
                }
            }
            SetLuaTop(L, valueIdx - 1);
        }
    }


    const char* RequiredExtForField(const char* fieldName)
    {
        if (std::strcmp(fieldName, "partsPath") == 0) return ".parts";
        if (std::strcmp(fieldName, "fpkPath")   == 0) return ".fpk";
        const std::size_t n = std::strlen(fieldName);
        if (n >= 3 && _stricmp(fieldName + n - 3, "Fpk") == 0) return ".fpk";
        if (n >= 3 && _stricmp(fieldName + n - 3, "Fv2") == 0) return ".fv2";
        return nullptr;
    }

    bool PathHasRequiredExt(const char* path, const char* ext)
    {
        const std::size_t n = std::strlen(path);
        const std::size_t m = std::strlen(ext);
        return n > m && _stricmp(path + n - m, ext) == 0;
    }

    std::uint64_t ReadIconFtexPathField(lua_State* L, int tableIndex)
    {
        LuaGetField(L, tableIndex, "iconFtexPath");
        std::uint64_t result = 0;
        if (LuaType(L, -1) == LUA_TSTRING)
        {
            if (const char* s = GetLuaString(L, -1); s && *s)
            {
                std::string path(s);
                if (!PathHasRequiredExt(path.c_str(), ".ftex"))
                    path += ".ftex";
                result = FoxHashes::PathCode64Ext(path);
            }
        }
        LuaPop(L, 1);
        return result;
    }

    std::uint64_t ReadSubAssetField(
        lua_State* L, int tableIndex, const char* fieldName,
        std::uint64_t defaultValue)
    {
        LuaGetField(L, tableIndex, fieldName);
        const int type = LuaType(L, -1);

        std::uint64_t result = defaultValue;

        if (type == 4)
        {
            const char* s = GetLuaString(L, -1);
            if (s && s[0] != '\0')
            {
                const char* ext = RequiredExtForField(fieldName);
                if (ext && !PathHasRequiredExt(s, ext))
                {
                    LogDebug("[OutfitLua] REJECTED %s '%s': must end in '%s' "
                        "(a wrong/missing extension hangs the engine loader "
                        "forever) - field ignored, using default\n",
                        fieldName, s, ext);
                }
                else
                {
                    result = FoxHashes::PathCode64Ext(s);
                }
            }
        }
        else if (type == 1)
        {
            const bool b = GetLuaBool(L, -1) != 0;
            result = b ? outfit::kSubAssetUseVanilla : outfit::kSubAssetDisabled;
        }


        SetLuaTop(L, -2);
        return result;
    }


    std::uint64_t ReadRequiredPathField(
        lua_State* L, int tableIndex, const char* fieldName)
    {
        LuaGetField(L, tableIndex, fieldName);
        const int type = LuaType(L, -1);

        std::uint64_t result = 0;
        if (type == 4)
        {
            const char* s = GetLuaString(L, -1);
            if (s && s[0] != '\0')
            {
                const char* ext = RequiredExtForField(fieldName);
                if (ext && !PathHasRequiredExt(s, ext))
                {
                    LogDebug("[OutfitLua] REJECTED %s '%s': must end in '%s' "
                        "(a wrong/missing extension hangs the engine loader "
                        "forever) - branch will be skipped\n",
                        fieldName, s, ext);
                }
                else
                {
                    result = FoxHashes::PathCode64Ext(s);
                }
            }
        }

        SetLuaTop(L, -2);
        return result;
    }


    struct HeadAlias
    {
        const char*    name;
        std::uint16_t  equipId;
    };
    static constexpr HeadAlias k_HeadAliases[] = {
        { "none",             0x400 },
        { "bandana",          0x20E },
        { "infinitebandana",  0x20F },
        { "balaclava",        0x210 },
        { "spheadgear",       0x211 },
        { "hpheadgear",       0x212 },
    };


    static void NormalizeHeadAlias(const char* in, char* out, std::size_t cap)
    {
        std::size_t j = 0;
        if (cap == 0) return;
        for (std::size_t i = 0; in && in[i] && j + 1 < cap; ++i)
        {
            const char c = in[i];
            if (c == '-' || c == '_' || c == ' ') continue;
            out[j++] = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
        }
        out[j] = '\0';
    }

    static std::uint16_t TryResolveHeadAlias(const char* name)
    {
        if (!name || !name[0]) return 0;
        char norm[64];
        NormalizeHeadAlias(name, norm, sizeof(norm));
        for (const HeadAlias& a : k_HeadAliases)
        {
            if (std::strcmp(norm, a.name) == 0)
                return a.equipId;
        }
        return 0;
    }


    constexpr std::uint8_t kQuietSuitAbilities = 0x74;

    static std::uint32_t ReadSoundSwitchField(lua_State* L, int tbl,
                                              const char* key,
                                              const char*& outName)
    {
        outName = nullptr;

        const char* name = nullptr;
        if (TryReadTableStringField(L, tbl, key, name)
            && name && name[0])
        {
            outName = name;
            return outfit::SoundSwitchHash(name);
        }

        LuaGetField(L, tbl, const_cast<char*>(key));
        std::uint32_t raw = outfit::kSoundSwitchUnset;
        if (LuaType(L, -1) == 3)
            raw = static_cast<std::uint32_t>(GetLuaInt64(L, -1));
        SetLuaTop(L, -2);
        return raw;
    }

    static std::uint8_t ClampAbilityLevel(int level)
    {
        if (level < 0) return 0;
        if (level > 9) return 9;
        return static_cast<std::uint8_t>(level);
    }

    void ReadDeclaredAbilities(lua_State* L, int ownerTblIdx,
                               outfit::DeclaredAbilities& out)
    {
        LuaGetField(L, ownerTblIdx, "abilities");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            SetLuaTop(L, -2);
            return;
        }

        const int tbl = GetLuaTop(L);
        out.declared = true;

        if (TryReadTableBoolField(L, tbl, "quietMovement", false))
            out.suitParamKind = kQuietSuitAbilities;

        out.silentSteps = TryReadTableBoolField(L, tbl, "silentFootsteps", false);

        int level = 0;
        if (TryReadTableIntField(L, tbl, "defense", level))
            out.defense = ClampAbilityLevel(level);
        if (TryReadTableIntField(L, tbl, "lifeRecovery", level))
            out.lifeRecovery = ClampAbilityLevel(level);

        const char* rattleName = nullptr;
        const std::uint32_t rattleSwitch =
            ReadSoundSwitchField(L, tbl, "rattleSuit", rattleName);
        if (rattleSwitch != outfit::kSoundSwitchUnset)
            out.rattleSuit = rattleSwitch;

        const char* damageSeName = nullptr;
        const std::uint32_t damageSeSwitch =
            ReadSoundSwitchField(L, tbl, "damageSe", damageSeName);
        if (damageSeSwitch != outfit::kSoundSwitchUnset)
            out.damageSe = damageSeSwitch;

        SetLuaTop(L, -2);
    }

    void ReadBranchAbilities(
        lua_State* L, int branchTblIdx, outfit::OutfitPlayerTypeData& branch,
        const char* branchName)
    {
        LuaGetField(L, branchTblIdx, "abilities");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            SetLuaTop(L, -2);
            return;
        }

        const int abilitiesTbl = GetLuaTop(L);

        if (TryReadTableBoolField(L, abilitiesTbl, "quietMovement", false))
            branch.suitParamKind = kQuietSuitAbilities;

        branch.abilitySilentSteps =
            TryReadTableBoolField(L, abilitiesTbl, "silentFootsteps", false);

        int level = 0;
        if (TryReadTableIntField(L, abilitiesTbl, "defense", level))
            branch.abilityDefense = ClampAbilityLevel(level);
        if (TryReadTableIntField(L, abilitiesTbl, "lifeRecovery", level))
            branch.abilityLifeRecovery = ClampAbilityLevel(level);

        const char* rattleName = nullptr;
        const std::uint32_t rattleSwitch =
            ReadSoundSwitchField(L, abilitiesTbl, "rattleSuit", rattleName);
        if (rattleSwitch != outfit::kSoundSwitchUnset)
            branch.abilityRattleSuit = rattleSwitch;

        const char* damageSeName = nullptr;
        const std::uint32_t damageSeSwitch =
            ReadSoundSwitchField(L, abilitiesTbl, "damageSe", damageSeName);
        if (damageSeSwitch != outfit::kSoundSwitchUnset)
            branch.abilityDamageSe = damageSeSwitch;

        if (branch.abilitySilentSteps || branch.abilityDefense
            || branch.abilityLifeRecovery
            || branch.abilityRattleSuit != outfit::kSoundSwitchUnset
            || branch.abilityDamageSe != outfit::kSoundSwitchUnset
            || branch.suitParamKind != 0)
            LogDebug("[OutfitLua] abilities on branch '%s': silentFootsteps=%d "
                "defense=%u lifeRecovery=%u rattleSuit='%s'=0x%08X "
                "damageSe='%s'=0x%08X quietMovement=%d\n",
                branchName ? branchName : "?",
                branch.abilitySilentSteps ? 1 : 0,
                static_cast<unsigned>(branch.abilityDefense),
                static_cast<unsigned>(branch.abilityLifeRecovery),
                rattleName ? rattleName : "(raw)",
                static_cast<unsigned>(branch.abilityRattleSuit),
                damageSeName ? damageSeName : "(raw)",
                static_cast<unsigned>(branch.abilityDamageSe),
                branch.suitParamKind != 0 ? 1 : 0);

        SetLuaTop(L, -2);
    }


    struct MaterialNameEntry
    {
        const char*     name;
        std::int32_t    index;
    };
    static constexpr MaterialNameEntry k_MaterialNames[] = {
        {"MTR_IRON_A", 0},  {"MTR_IRON_B", 1},  {"MTR_IRON_C", 2},  {"MTR_IRON_D", 3},
        {"MTR_IRON_E", 4},  {"MTR_IRON_F", 5},  {"MTR_IRON_G", 6},  {"MTR_IRON_M", 7},
        {"MTR_IRON_N", 8},  {"MTR_IRON_W", 9},
        {"MTR_PIPE_A", 10}, {"MTR_PIPE_B", 11}, {"MTR_PIPE_S", 12},
        {"MTR_TIN_A",  13},
        {"MTR_FENC_A", 14}, {"MTR_FENC_B", 15}, {"MTR_FENC_F", 16},
        {"MTR_CONC_A", 17}, {"MTR_CONC_B", 18},
        {"MTR_BRIC_A", 19},
        {"MTR_PLAS_A", 20}, {"MTR_PLAS_B", 21}, {"MTR_PLAS_W", 22},
        {"MTR_PAPE_A", 23}, {"MTR_PAPE_B", 24}, {"MTR_PAPE_C", 25}, {"MTR_PAPE_D", 26},
        {"MTR_RUBB_A", 27}, {"MTR_RUBB_B", 28},
        {"MTR_CLOT_A", 29}, {"MTR_CLOT_B", 30}, {"MTR_CLOT_C", 31}, {"MTR_CLOT_D", 32},
        {"MTR_CLOT_E", 33},
        {"MTR_GLAS_A", 34}, {"MTR_GLAS_B", 35}, {"MTR_GLAS_C", 36},
        {"MTR_VINL_A", 37}, {"MTR_VINL_W", 38},
        {"MTR_TILE_A", 39},
        {"MTR_TLRF_A", 40},
        {"MTR_ALRM_A", 41},
        {"MTR_COPS_A", 42}, {"MTR_COPS_B", 43},
        {"MTR_BRIR_A", 44},
        {"MTR_BLOD_A", 45},
        {"MTR_SOIL_A", 46}, {"MTR_SOIL_B", 47}, {"MTR_SOIL_C", 48}, {"MTR_SOIL_D", 49},
        {"MTR_SOIL_E", 50}, {"MTR_SOIL_F", 51}, {"MTR_SOIL_G", 52}, {"MTR_SOIL_H", 53},
        {"MTR_SOIL_R", 54}, {"MTR_SOIL_W", 55},
        {"MTR_GRAV_A", 56},
        {"MTR_SAND_A", 57}, {"MTR_SAND_B", 58}, {"MTR_SAND_C", 59},
        {"MTR_LEAF",   60}, {"MTR_RLEF",   61}, {"MTR_RLEF_B", 62},
        {"MTR_WOOD_A", 63}, {"MTR_WOOD_B", 64}, {"MTR_WOOD_C", 65}, {"MTR_WOOD_D", 66},
        {"MTR_WOOD_G", 67}, {"MTR_WOOD_M", 68}, {"MTR_WOOD_W", 69},
        {"MTR_FWOD_A", 70},
        {"MTR_PLNT_A", 71},
        {"MTR_ROCK_A", 72}, {"MTR_ROCK_B", 73}, {"MTR_ROCK_P", 74},
        {"MTR_MOSS_A", 75},
        {"MTR_TURF_A", 76},
        {"MTR_WATE_A", 77}, {"MTR_WATE_B", 78}, {"MTR_WATE_C", 79},
        {"MTR_AIR_A",  80},
        {"MTR_NONE_A", 81},
    };
    static_assert(sizeof(k_MaterialNames) / sizeof(k_MaterialNames[0])
                  == outfit::kCamoMaterialCount,
                  "k_MaterialNames must list all 82 MaterialType entries");

    static std::int32_t ResolveMaterialNameToIndex(const char* name)
    {
        if (!name) return -1;
        for (const auto& e : k_MaterialNames)
        {
            if (std::strcmp(e.name, name) == 0) return e.index;
        }
        return -1;
    }

    static constexpr const char* k_CamoTypeNames[] = {
        "OLIVEDRAB", "SPLITTER", "SQUARE", "TIGERSTRIPE", "GOLDTIGER", "FOXTROT",       // 0-5
        "WOODLAND", "WETWORK", "ARBANGRAY", "ARBANBLUE", "SANDSTORM", "REALTREE",       // 6-11
        "INVISIBLE", "BLACK", "SNEAKING_SUIT_GZ", "SNEAKING_SUIT_TPP", "BATTLEDRESS",   // 12-16
        "PARASITE", "NAKED", "LEATHER", "SOLIDSNAKE", "NINJA", "RAIDEN", "HOSPITAL",    // 17-23
        "GOLD", "SILVER", "PANTHER", "AVATAR_EDIT_MAN", "MGS3", "MGS3_NAKED",           // 24-29
        "MGS3_SNEAKING", "MGS3_TUXEDO", "EVA_CLOSE", "EVA_OPEN", "BOSS_CLOSE",          // 30-34
        "BOSS_OPEN",                                                                    // 35
        "C23", "C24", "C27", "C29", "C30", "C35", "C38", "C39", "C42", "C46", "C49",    // 36-46
        "C52", "C16", "C17", "C18", "C19", "C20", "C22", "C25", "C26", "C28", "C31",    // 47-57
        "C32", "C33", "C36", "C37", "C40", "C41", "C43", "C44", "C45", "C47", "C48",    // 58-68
        "C50", "C51", "C53", "C54", "C55", "C56", "C57", "C58", "C59", "C60",           // 69-78
        "SWIMWEAR_C00", "SWIMWEAR_C01", "SWIMWEAR_C02", "SWIMWEAR_C03", "SWIMWEAR_C05", // 79-83
        "SWIMWEAR_C06", "SWIMWEAR_C38", "SWIMWEAR_C39", "SWIMWEAR_C44", "SWIMWEAR_C46", // 84-88
        "SWIMWEAR_C48", "SWIMWEAR_C53",                                                 // 89-90
        "SWIMWEAR_G_C00", "SWIMWEAR_G_C01", "SWIMWEAR_G_C02", "SWIMWEAR_G_C03",         // 91-94
        "SWIMWEAR_G_C05", "SWIMWEAR_G_C06", "SWIMWEAR_G_C38", "SWIMWEAR_G_C39",         // 95-98
        "SWIMWEAR_G_C44", "SWIMWEAR_G_C46", "SWIMWEAR_G_C48", "SWIMWEAR_G_C53",         // 99-102
        "SWIMWEAR_H_C00", "SWIMWEAR_H_C01", "SWIMWEAR_H_C02", "SWIMWEAR_H_C03",         // 103-106
        "SWIMWEAR_H_C05", "SWIMWEAR_H_C06", "SWIMWEAR_H_C38", "SWIMWEAR_H_C39",         // 107-110
        "SWIMWEAR_H_C44", "SWIMWEAR_H_C46", "SWIMWEAR_H_C48", "SWIMWEAR_H_C53",         // 111-114
        "OCELOT", "QUIET",                                                              // 115-116
    };
    static_assert(sizeof(k_CamoTypeNames) / sizeof(k_CamoTypeNames[0])
                  == static_cast<std::size_t>(outfit::kVanillaCamoTypeMax) + 1,
                  "k_CamoTypeNames must list all 117 vanilla camo types (0..116)");

    static std::int32_t ResolveCamoTypeNameToIndex(const char* name)
    {
        if (!name || !name[0]) return -1;
        for (std::size_t i = 0;
             i < sizeof(k_CamoTypeNames) / sizeof(k_CamoTypeNames[0]); ++i)
        {
            if (_stricmp(k_CamoTypeNames[i], name) == 0)
                return static_cast<std::int32_t>(i);
        }
        return -1;
    }

    bool ReadHeadOptionsArrayInto(
        lua_State* L, int tableIndex,
        std::uint16_t* outIds, std::uint8_t& outCount,
        std::uint64_t* outPendingHashes, std::uint8_t& outPendingCount)
    {
        outCount = 0;
        outPendingCount = 0;

        LuaGetField(L, tableIndex, "headOptions");
        const bool present = (LuaType(L, -1) == LUA_TTABLE);
        if (present)
        {
            const std::size_t n   = LuaObjLen(L, -1);
            const std::size_t cap = outfit::kMaxHeadOptionsPerOutfit;
            const std::size_t lim = (n < cap) ? n : cap;

            for (std::size_t i = 1; i <= lim; ++i)
            {
                LuaRawGetI(L, -1, static_cast<int>(i));
                if (LuaIsNumber(L, -1))
                {
                    const int v = GetLuaInt(L, -1);
                    if (v > 0 && v <= 0xFFFF)
                    {
                        outIds[outCount++] = static_cast<std::uint16_t>(v);
                    }
                }
                else if (LuaIsString(L, -1))
                {
                    const char* name = GetLuaString(L, -1);
                    if (const std::uint16_t alias = TryResolveHeadAlias(name);
                        alias != 0)
                    {
                        outIds[outCount++] = alias;
                    }
                    else if (const auto* head =
                             outfit::TryGetCustomHeadByName(name))
                    {
                        outIds[outCount++] = head->equipId;
                    }
                    else if (name && name[0]
                             && outPendingCount < outfit::kMaxHeadOptionsPerOutfit)
                    {
                        outPendingHashes[outPendingCount++] =
                            FoxHashes::StrCode64(name);
#ifdef _DEBUG
                        LogDebug("[OutfitLua] headOptions: '%s' not registered yet -> "
                            "DEFERRED (resolves when its RegisterCustomHead runs)\n",
                            name);
#endif
                    }
                    else
                    {
                        LogDebug("[OutfitLua] headOptions: unknown/empty head name '%s' "
                            "skipped\n", name ? name : "(null)");
                    }
                }
                LuaPop(L, 1);
            }
        }
        LuaPop(L, 1);
        return present;
    }


    void ReadMotionMtarsInto(
        lua_State* L, int tableIndex,
        std::uint64_t (&dst)[outfit::kMotionMtarSlotCount]);

    void ReadVariantsArrayInto(
        lua_State* L, int branchTblIdx, outfit::OutfitPlayerTypeData& branch)
    {
        branch.variantCount = 0;

        LuaGetField(L, branchTblIdx, "variants");
        if (LuaType(L, -1) == LUA_TTABLE)
        {
            const std::size_t n = LuaObjLen(L, -1);


            const std::size_t cap = outfit::kMaxVariantsPerOutfit - 1;
            const std::size_t lim = (n < cap) ? n : cap;

            std::uint8_t maxFilledSlot = 0;
            for (std::size_t i = 1; i <= lim; ++i)
            {
                LuaRawGetI(L, -1, static_cast<int>(i));
                if (LuaType(L, -1) == LUA_TTABLE)
                {
                    outfit::OutfitVariant v{};
                    v.used            = true;
                    ReadDeclaredAbilities(L, GetLuaTop(L), v.abilities);
                    v.partsPathCode64 = ReadRequiredPathField(L, -1, "partsPath");
                    v.fpkPathCode64   = ReadRequiredPathField(L, -1, "fpkPath");
                    v.camoFpk         = ReadSubAssetField(L, -1, "camoFpk",
                                            outfit::kSubAssetUseVanilla);
                    v.camoFv2         = ReadSubAssetField(L, -1, "camoFv2",
                                            outfit::kSubAssetDisabled);
                    v.diamondFpk      = ReadSubAssetField(L, -1, "diamondFpk",
                                            outfit::kSubAssetDisabled);
                    v.diamondFv2      = ReadSubAssetField(L, -1, "diamondFv2",
                                            outfit::kSubAssetDisabled);
                    v.voiceFpk        = ReadSubAssetField(L, -1, "voiceFpk",
                                            outfit::kSubAssetUseVanilla);

                    ReadMotionMtarsInto(L, GetLuaTop(L), v.motionMtars);

                    LuaGetField(L, -1, "displayName");
                    if (LuaType(L, -1) == LUA_TSTRING)
                    {
                        if (const char* s = GetLuaString(L, -1); s && *s)
                            v.displayNameHash = FoxHashes::StrCode64(s);
                    }
                    LuaPop(L, 1);

                    if (v.displayNameHash == 0)
                    {
                        LuaGetField(L, -1, "displayNameHash");
                        const int t = LuaType(L, -1);
                        if (t == LUA_TNUMBER)
                        {


                            v.displayNameHash =
                                static_cast<std::uint64_t>(GetLuaInt(L, -1));
                        }
                        LuaPop(L, 1);
                    }

                    v.iconPathHash = ReadIconFtexPathField(L, -1);

                    LuaGetField(L, -1, "enableArm");
                    if (LuaType(L, -1) != 0)
                    {
                        v.hasEnableArm = true;
                        v.enableArm    = GetLuaBool(L, -1) != 0;
                    }
                    LuaPop(L, 1);

                    LuaGetField(L, -1, "enableHead");
                    if (LuaType(L, -1) != 0)
                    {
                        v.hasEnableHead = true;
                        v.enableHead    = GetLuaBool(L, -1) != 0;
                    }
                    LuaPop(L, 1);

                    {
                        const int variantTbl = GetLuaTop(L);
                        std::uint8_t vHeadCount    = 0;
                        std::uint8_t vPendingCount = 0;
                        const bool declared = ReadHeadOptionsArrayInto(
                            L, variantTbl, v.headOptionEquipIds, vHeadCount,
                            v.pendingHeadNameHashes, vPendingCount);
                        v.headOptionsDeclared = declared;
                        v.headOptionCount     = vHeadCount;
                        v.pendingHeadCount    = vPendingCount;
                    }


                    if (branch.defaultVariant == 0
                        && TryReadTableBoolField(L, GetLuaTop(L), "default", false))
                        branch.defaultVariant = static_cast<std::uint8_t>(i);

                    branch.EnsureVar(i) = v;
                    maxFilledSlot      = static_cast<std::uint8_t>(i);
                }
                LuaPop(L, 1);
            }

            if (maxFilledSlot > 0)
            {


                branch.variantCount = static_cast<std::uint8_t>(maxFilledSlot + 1);
            }
        }
        LuaPop(L, 1);
    }

    void ReadBranchCamoBonusValues(
        lua_State* L, int tableIndex, outfit::OutfitPlayerTypeData& branch)
    {
        LuaGetField(L, tableIndex, "camoBonusValues");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            SetLuaTop(L, -2);
            return;
        }

        const int valuesTbl = GetLuaTop(L);
        std::size_t writeCount = 0;

        LuaPushNil(L);
        while (LuaNext(L, valuesTbl) != 0)
        {
            std::int32_t materialIdx = -1;
            const int keyType = LuaType(L, -2);
            if (keyType == LUA_TSTRING)
            {
                const char* keyName = GetLuaString(L, -2);
                materialIdx = ResolveMaterialNameToIndex(keyName);
            }
            else if (keyType == LUA_TNUMBER)
            {
                const int keyIdx = GetLuaInt(L, -2);
                if (keyIdx >= 1
                    && keyIdx <= static_cast<int>(outfit::kCamoMaterialCount))
                {
                    materialIdx = keyIdx - 1;
                }
            }

            if (materialIdx >= 0
                && materialIdx < static_cast<std::int32_t>(outfit::kCamoMaterialCount))
            {
                branch.camoBonusValues[materialIdx] = GetLuaInt(L, -1);
                ++writeCount;
            }

            SetLuaTop(L, -2);
        }

        if (writeCount > 0)
            branch.hasCamoBonusValues = true;

        SetLuaTop(L, -2);
    }

    void ReadMotionMtarsInto(
        lua_State* L, int tableIndex,
        std::uint64_t (&dst)[outfit::kMotionMtarSlotCount])
    {
        LuaGetField(L, tableIndex, "motionMtars");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            SetLuaTop(L, -2);
            return;
        }

        const int tbl = GetLuaTop(L);

        LuaPushNil(L);
        while (LuaNext(L, tbl) != 0)
        {
            const char* keyName =
                (LuaType(L, -2) == LUA_TSTRING) ? GetLuaString(L, -2) : nullptr;
            const int slot = outfit::MotionMtarSlotFromName(keyName);

            if (slot < 0)
            {
                LogDebug("[OutfitLua] motionMtars: unknown archive '%s' - ignored; the "
                    "key must be a player2 motion archive name such as \"cqc\" or "
                    "\"jump\"\n", keyName ? keyName : "(non-string key)");
            }
            else if (LuaType(L, -1) == LUA_TBOOLEAN)
            {
                LogDebug("[OutfitLua] motionMtars.%s is a boolean - ignored; there is no "
                    "disable form, omit the key to keep the vanilla archive\n",
                    keyName);
            }
            else if (LuaType(L, -1) == LUA_TSTRING)
            {
                if (const char* s = GetLuaString(L, -1); s && *s)
                {
                    const std::uint64_t hash = FoxHashes::PathCode64Ext(s);
                    dst[slot] = hash;
                    outfit::RegisterMotionMtarOverrideHash(hash, slot);
                }
            }

            SetLuaTop(L, -2);
        }

        SetLuaTop(L, -2);
    }

    bool ReadPlayerTypeBranchTable(
        lua_State* L, int branchTblIdx, outfit::OutfitPlayerTypeData& branch,
        const char* branchName)
    {
        if (LuaType(L, branchTblIdx) != LUA_TTABLE) return false;

        branch                 = outfit::OutfitPlayerTypeData{};
        branch.partsPathCode64 = ReadRequiredPathField(L, branchTblIdx, "partsPath");
        branch.fpkPathCode64   = ReadRequiredPathField(L, branchTblIdx, "fpkPath");

        if (branch.partsPathCode64 == 0 || branch.fpkPathCode64 == 0)
            return false;

        branch.camoFpk    = ReadSubAssetField(L, branchTblIdx, "camoFpk",
                                outfit::kSubAssetDisabled);
        branch.faceFpk    = ReadSubAssetField(L, branchTblIdx, "faceFpk",
                                outfit::kSubAssetUseVanilla);
        branch.skinFv2    = ReadSubAssetField(L, branchTblIdx, "skinFv2",
                                outfit::kSubAssetUseVanilla);
        branch.diamondFpk = ReadSubAssetField(L, branchTblIdx, "diamondFpk",
                                outfit::kSubAssetDisabled);
        branch.voiceFpk   = ReadSubAssetField(L, branchTblIdx, "voiceFpk",
                                outfit::kSubAssetUseVanilla);
        branch.camoFv2    = ReadSubAssetField(L, branchTblIdx, "camoFv2",
                                outfit::kSubAssetDisabled);
        branch.diamondFv2 = ReadSubAssetField(L, branchTblIdx, "diamondFv2",
                                outfit::kSubAssetDisabled);
        ReadMotionMtarsInto(L, branchTblIdx, branch.motionMtars);


        branch.enableArm  = TryReadTableBoolField(L, branchTblIdx, "enableArm",  true);
        branch.enableHead = TryReadTableBoolField(L, branchTblIdx, "enableHead", true);

        ReadBranchAbilities(L, branchTblIdx, branch, branchName);


        LuaGetField(L, branchTblIdx, "displayName");
        if (LuaType(L, -1) == LUA_TSTRING)
        {
            if (const char* s = GetLuaString(L, -1); s && *s)
                branch.baseDisplayNameHash = FoxHashes::StrCode64(s);
        }
        LuaPop(L, 1);

        if (branch.baseDisplayNameHash == 0)
        {
            int displayHashRaw = 0;
            if (TryReadTableIntField(L, branchTblIdx, "displayNameHash", displayHashRaw)
                && displayHashRaw != 0)
            {
                branch.baseDisplayNameHash =
                    static_cast<std::uint64_t>(displayHashRaw);
            }
        }

        branch.baseIconPathHash = ReadIconFtexPathField(L, branchTblIdx);


        ReadVariantsArrayInto(L, branchTblIdx, branch);

        std::uint8_t branchHeadCount    = 0;
        std::uint8_t branchPendingCount = 0;
        ReadHeadOptionsArrayInto(L, branchTblIdx,
            branch.headOptionEquipIds, branchHeadCount,
            branch.pendingHeadNameHashes, branchPendingCount);
        branch.headOptionCount     = branchHeadCount;
        branch.pendingHeadCount    = branchPendingCount;
        branch.supportsHeadOptions = (branchHeadCount > 0);


        {
            LuaGetField(L, branchTblIdx, "camoBonusType");
            const int cbtType = LuaType(L, -1);
            std::int32_t camoIdx = -1;
            if (cbtType == LUA_TNUMBER)
            {
                const int n = GetLuaInt(L, -1);
                if (n >= 0 && n <= outfit::kVanillaCamoTypeMax) camoIdx = n;
            }
            else if (cbtType == LUA_TSTRING)
            {
                const char* nm = GetLuaString(L, -1);
                camoIdx = ResolveCamoTypeNameToIndex(nm);
                if (camoIdx < 0)
                    LogDebug("[OutfitLua] camoBonusType: unknown camo name '%s' - ignored "
                        "(use a playerCamoTypes name like \"RAIDEN\" or a number "
                        "0..116)\n", nm ? nm : "(null)");
            }
            SetLuaTop(L, -2);
            if (camoIdx >= 0 && camoIdx <= outfit::kVanillaCamoTypeMax)
                branch.camoBonusType = static_cast<std::uint8_t>(camoIdx);
        }
        branch.camoBonusTypeDeclared = branch.camoBonusType;
        ReadBranchCamoBonusValues(L, branchTblIdx, branch);


        branch.used = true;
        return true;
    }

    void BuildOutfitSummary(const outfit::OutfitEntry* e,
                            char* out, std::size_t outSize)
    {
        if (!out || outSize == 0) return;
        out[0] = '\0';
        if (!e) return;

        static const char* const kNames[outfit::kPlayerTypeMax] =
            { "Snake", "DDMale", "DDFemale", "Avatar", "pt4", "Ocelot", "Quiet" };

        std::size_t pos = 0;
        auto append = [&](const char* fmt, auto... args)
        {
            if (pos + 1 >= outSize) return;
            const int w = std::snprintf(out + pos, outSize - pos, fmt, args...);
            if (w < 0) return;
            const std::size_t room = outSize - pos - 1;
            pos += (static_cast<std::size_t>(w) < room)
                 ? static_cast<std::size_t>(w) : room;
        };

        append("branches=[");
        bool first = true;
        for (std::uint8_t pt = 0; pt < outfit::kPlayerTypeMax; ++pt)
        {
            if (!e->DeclaresPlayerType(pt)) continue;
            const std::uint16_t* ids = nullptr;
            std::uint8_t heads = 0;
            e->GetHeadOptionsFor(pt, &ids, &heads);
            unsigned motion = 0;
            for (std::size_t s = 0; s < outfit::kMotionMtarSlotCount; ++s)
                if (e->perPlayerType[pt].motionMtars[s] != 0)
                    ++motion;
            append("%s%s(variants=%u,heads=%u,arm=%d,head=%d,motion=%u)",
                   first ? "" : ",",
                   kNames[pt],
                   static_cast<unsigned>(e->GetVariantCountFor(pt)),
                   static_cast<unsigned>(heads),
                   e->perPlayerType[pt].enableArm ? 1 : 0,
                   e->perPlayerType[pt].enableHead ? 1 : 0,
                   motion);
            first = false;
        }
        append("] variants=%u selectors=[",
               static_cast<unsigned>(e->variantCount));
        for (std::uint8_t i = 0;
             i < e->variantCount && i < outfit::kMaxVariantsPerOutfit; ++i)
            append("%s0x%02X", (i == 0) ? "" : ",",
                   static_cast<unsigned>(e->variantSelectorCodes[i]));
        append("]");
    }
}

int __cdecl l_RegisterOutfit(lua_State* L)
{
    V_FrameWorkState::SaveBatch _saveBatch;

    if (LuaType(L, 1) != LUA_TTABLE)
    {
        LogDebug("[OutfitLua] RegisterOutfit: arg 1 must be a table\n");
        PushLuaBool(L, false);
        return 1;
    }

    const auto defStore = std::make_unique<outfit::OutfitDefinition>();
    outfit::OutfitDefinition& def = *defStore;


    const char* key = nullptr;
    TryReadTableStringField(L, 1, "key", key);
    def.key = key;

    if (!key || !key[0])
    {
        LogDebug("[OutfitLua] RegisterOutfit: 'key' (non-empty string) is required; "
                 "developId and flowIndex are auto-allocated and persisted under it "
                 "in V_FrameWork_State.lua\n");
        PushLuaBool(L, false);
        return 1;
    }
    std::uint8_t branchCount = 0;
    ForEachPlayerTypeBranch(L, 1, "RegisterOutfit", key,
        [&](std::uint8_t playerType, const char* branchName, int branchIdx)
    {
        const auto branchStore =
            std::make_unique<outfit::OutfitPlayerTypeData>();
        outfit::OutfitPlayerTypeData& branch = *branchStore;
        if (ReadPlayerTypeBranchTable(L, branchIdx, branch, branchName))
        {
            def.perPlayerType[playerType] = branch;
            ++branchCount;
        }
        else
        {
            LogDebug("[OutfitLua] RegisterOutfit: branch '%s' present but "
                "missing required partsPath/fpkPath - skipping (key=%s)\n",
                branchName, key);
        }
    });

    if (branchCount == 0)
    {
        LogDebug("[OutfitLua] RegisterOutfit: at least one playerType branch is "
                 "required (snake/ddMale/ddFemale/avatar), each a sub-table with "
                 "partsPath and fpkPath (key=%s)\n", key);
        PushLuaBool(L, false);
        return 1;
    }

    bool wasCreated = false;
    {
        std::int32_t newId = 0;
        if (V_FrameWorkState::ResolveOrCreateDevelopId(key, 0, newId, &wasCreated)
            && newId > 0 && newId <= 0xFFFF)
        {
            def.developId = static_cast<std::uint16_t>(newId);
        }
        V_FrameWorkState::SetRowKind(key, V_FrameWorkState::kRowKindOutfit);
    }
    if (def.developId == 0)
    {
        Log("[OutfitLua] RegisterOutfit: failed to allocate developId for "
            "key='%s'\n", key);
        PushLuaBool(L, false);
        return 1;
    }

    def.flowIndex = 0;


    if (def.partsTypeHint == 0xFF)
    {
        const std::uint8_t pp = V_FrameWorkState::GetPersistedOutfitPartsType(key);
        if (pp != 0) def.partsTypeHint = pp;
    }
    if (def.selectorCodeHint == 0xFF)
    {
        const std::uint8_t ps = V_FrameWorkState::GetPersistedOutfitSelector(key);
        if (ps != 0) def.selectorCodeHint = ps;
    }
    {
        std::uint8_t persisted[V_FrameWorkState::kPersistedVariantSelectorSlots] = {};
        V_FrameWorkState::GetPersistedOutfitVariantSelectors(
            key, persisted, sizeof(persisted));
        for (std::size_t i = 0;
             i < V_FrameWorkState::kPersistedVariantSelectorSlots; ++i)
            def.variantSelectorHints[i + 1] = persisted[i];
    }

    std::uint8_t allocatedPartsType = 0xFF;
    const bool ok = outfit::RegisterOutfit(def, &allocatedPartsType);

    if (!ok)
    {
        PushLuaBool(L, false);
        return 1;
    }

    std::uint8_t finalSelector = 0;
    const outfit::OutfitEntry* e = nullptr;
    if (outfit::TryGetOutfitByDevelopId(def.developId, &e) && e)
        finalSelector = e->selectorCode;

    char summary[320];
    BuildOutfitSummary(e, summary, sizeof(summary));

    if (wasCreated)
        LogDebug("[Outfit] Added \"%s\" (partsType 0x%02X, develop %u, flow %u, "
            "selector 0x%02X; %s) - first time; saved.\n",
            key, allocatedPartsType,
            static_cast<unsigned>(def.developId),
            static_cast<unsigned>(def.flowIndex),
            static_cast<unsigned>(finalSelector), summary);
    else
        LogDebug("[Outfit] Loaded \"%s\" (partsType 0x%02X, develop %u, flow %u, "
            "selector 0x%02X; %s)\n",
            key, allocatedPartsType,
            static_cast<unsigned>(def.developId),
            static_cast<unsigned>(def.flowIndex),
            static_cast<unsigned>(finalSelector), summary);

    PushLuaNumber(L, static_cast<float>(allocatedPartsType));
    PushLuaNumber(L, static_cast<float>(def.developId));
    PushLuaNumber(L, static_cast<float>(def.flowIndex));
    return 3;
}


int __cdecl l_RegisterHeadOption(lua_State* L)
{
    if (LuaType(L, 1) != LUA_TTABLE)
    {
        LogDebug("[CustomHead] RegisterHeadOption: arg 1 must be a table\n");
        PushLuaNumber(L, 0);
        return 1;
    }

    const char* key = nullptr;
    TryReadTableStringField(L, 1, "key", key);
    if (!key || !key[0])
    {
        LogDebug("[CustomHead] RegisterHeadOption: missing 'key'\n");
        PushLuaNumber(L, 0);
        return 1;
    }

    std::uint16_t faceIds[outfit::kPlayerTypeMax] = {};
    std::uint64_t faceFv2Codes[outfit::kPlayerTypeMax] = {};
    std::uint64_t faceFpkCodes[outfit::kPlayerTypeMax] = {};
    std::uint64_t snakeStageFv2[outfit::kSnakeFaceStageCount] = {};
    std::uint64_t snakeStageFpk[outfit::kSnakeFaceStageCount] = {};
    bool          haveSnakeStages = false;
    ForEachPlayerTypeBranch(L, 1, "RegisterHeadOption", key,
        [&](std::uint8_t playerType, const char* branchName, int branchIdx)
    {
        int rawBranchFace = 0;
        if (TryReadTableIntField(L, branchIdx, "TppEnemyFaceId",
                                 rawBranchFace)
            && rawBranchFace > 0 && rawBranchFace <= 0xFFFF)
        {
            faceIds[playerType] =
                static_cast<std::uint16_t>(rawBranchFace);
        }

        const char* fv2 = nullptr;
        if (TryReadTableStringField(L, branchIdx, "fv2", fv2) && fv2 && fv2[0])
            faceFv2Codes[playerType] = FoxHashes::PathCode64Ext(fv2);
        const char* fpk = nullptr;
        if (TryReadTableStringField(L, branchIdx, "fpk", fpk) && fpk && fpk[0])
            faceFpkCodes[playerType] = FoxHashes::PathCode64Ext(fpk);

        if (playerType == outfit::kPlayerType_Snake)
        {
            LuaGetField(L, branchIdx, "faceStages");
            if (LuaType(L, -1) == LUA_TTABLE)
            {
                for (std::uint8_t s = 0;
                     s < outfit::kSnakeFaceStageCount; ++s)
                {
                    LuaRawGetI(L, -1, static_cast<int>(s) + 1);
                    if (LuaType(L, -1) == LUA_TTABLE)
                    {
                        const char* sFv2 = nullptr;
                        if (TryReadTableStringField(L, -1, "fv2", sFv2)
                            && sFv2 && sFv2[0])
                        {
                            snakeStageFv2[s] =
                                FoxHashes::PathCode64Ext(sFv2);
                            haveSnakeStages = true;
                        }
                        const char* sFpk = nullptr;
                        if (TryReadTableStringField(L, -1, "fpk", sFpk)
                            && sFpk && sFpk[0])
                        {
                            snakeStageFpk[s] =
                                FoxHashes::PathCode64Ext(sFpk);
                            haveSnakeStages = true;
                        }
                    }
                    LuaPop(L, 1);
                }
            }
            LuaPop(L, 1);
        }
    });

    bool showInDevelopMenu = false;
    {
        LuaGetField(L, 1, "showInDevelopMenu");
        if (LuaType(L, -1) == 1 )
            showInDevelopMenu = (GetLuaBool(L, -1) != 0);
        SetLuaTop(L, -2);
    }

    bool infiniteAmmo = false;
    {
        LuaGetField(L, 1, "abilities");
        if (LuaType(L, -1) == LUA_TTABLE)
            infiniteAmmo = TryReadTableBoolField(L, GetLuaTop(L),
                                                 "infiniteAmmo", false);
        SetLuaTop(L, -2);
    }

    const std::uint16_t equipId = outfit::RegisterHeadOption(
        key, faceIds, faceFv2Codes, faceFpkCodes, showInDevelopMenu);

    outfit::SetCustomHeadInfiniteAmmo(key, infiniteAmmo);

    if (haveSnakeStages)
        outfit::SetCustomHeadSnakeFaceStages(key, snakeStageFv2, snakeStageFpk);

    PushLuaNumber(L, static_cast<float>(equipId));
    return 1;
}

namespace
{
    void Shim_LuaSetTop(lua_State* L, int idx)            { SetLuaTop(L, idx); }
    void Shim_LuaPushString(lua_State* L, const char* s)
    {
        if (ResolveLuaApi() && g_lua_pushstring)
            g_lua_pushstring(L, const_cast<char*>(s));
    }
    void Shim_LuaCreateTable(lua_State* L, int narr, int nrec)
    {
        if (ResolveLuaApi() && g_lua_createtable)
            g_lua_createtable(L, narr, nrec);
    }
    void Shim_LuaGetTable(lua_State* L, int idx)
    {
        if (ResolveLuaApi() && g_lua_gettable)
            g_lua_gettable(L, idx);
    }
    void Shim_LuaSetTable(lua_State* L, int idx)
    {
        if (ResolveLuaApi() && g_lua_settable)
            g_lua_settable(L, idx);
    }

    bool g_EquipDevelopBound = false;
}

void OutfitLua_EnsureEquipDevelopBound()
{
    if (g_EquipDevelopBound)
        return;

    EquipDevelopAdd::Deps deps{};
    deps.ResolveLuaApi   = &ResolveLuaApi;
    deps.GetLuaTop       = &GetLuaTop;
    deps.LuaType         = &LuaType;
    deps.LuaToBool       = &GetLuaBool;
    deps.LuaSetTop       = &Shim_LuaSetTop;
    deps.GetLuaString    = &GetLuaString;
    deps.GetLuaInt       = &GetLuaInt;
    deps.PushLuaNumber   = &PushLuaNumber;
    deps.LuaPushString   = &Shim_LuaPushString;
    deps.LuaCreateTable  = &Shim_LuaCreateTable;
    deps.LuaGetTable     = &Shim_LuaGetTable;
    deps.LuaSetTable     = &Shim_LuaSetTable;
    EquipDevelopAdd::Bind(deps);

    g_EquipDevelopBound = true;
#ifdef _DEBUG
    LogDebug("[OutfitLua] EquipDevelopAdd::Bind done\n");
#endif
}

static std::uint8_t ReadVanillaExtVariants(
    lua_State* L, int branchTblIdx,
    outfit::VanillaSuitVariantAsset* out, std::size_t cap)
{
    std::uint8_t filled = 0;
    LuaGetField(L, branchTblIdx, "variants");
    if (LuaType(L, -1) == LUA_TTABLE)
    {
        const std::size_t n   = LuaObjLen(L, -1);
        const std::size_t lim = (n < cap) ? n : cap;
        for (std::size_t i = 1; i <= lim; ++i)
        {
            LuaRawGetI(L, -1, static_cast<int>(i));
            if (LuaType(L, -1) == LUA_TTABLE)
            {
                outfit::VanillaSuitVariantAsset v{};
                ReadDeclaredAbilities(L, GetLuaTop(L), v.abilities);
                v.partsPathCode64 = ReadRequiredPathField(L, -1, "partsPath");
                v.fpkPathCode64   = ReadRequiredPathField(L, -1, "fpkPath");
                v.camoFpk    = ReadSubAssetField(L, -1, "camoFpk",
                                   outfit::kSubAssetUseVanilla);
                v.camoFv2    = ReadSubAssetField(L, -1, "camoFv2",
                                   outfit::kSubAssetUseVanilla);
                v.diamondFpk = ReadSubAssetField(L, -1, "diamondFpk",
                                   outfit::kSubAssetUseVanilla);
                v.diamondFv2 = ReadSubAssetField(L, -1, "diamondFv2",
                                   outfit::kSubAssetUseVanilla);
                v.voiceFpk   = ReadSubAssetField(L, -1, "voiceFpk",
                                   outfit::kSubAssetUseVanilla);

                LuaGetField(L, -1, "displayName");
                if (LuaType(L, -1) == LUA_TSTRING)
                    if (const char* s = GetLuaString(L, -1); s && *s)
                        v.displayNameHash = FoxHashes::StrCode64(s);
                LuaPop(L, 1);
                if (v.displayNameHash == 0)
                {
                    LuaGetField(L, -1, "displayNameHash");
                    if (LuaType(L, -1) == LUA_TNUMBER)
                        v.displayNameHash =
                            static_cast<std::uint64_t>(GetLuaInt(L, -1));
                    LuaPop(L, 1);
                }

                if (v.partsPathCode64 != 0 || v.fpkPathCode64 != 0)
                    out[filled++] = v;
            }
            LuaPop(L, 1);
        }
    }
    LuaPop(L, 1);
    return filled;
}

int __cdecl l_ExtendVanillaOutfit(lua_State* L)
{
    if (LuaType(L, 1) != LUA_TTABLE)
    {
        LogDebug("[OutfitLua] ExtendVanillaOutfit: arg 1 must be a table\n");
        PushLuaBool(L, false);
        return 1;
    }

    std::uint8_t vanillaPartsType = 0xFF;
    std::int32_t labelCamo        = -1;
    const char*  outfitName       = nullptr;

    if (TryReadTableStringField(L, 1, "outfit", outfitName)
        && outfitName && outfitName[0])
    {
        labelCamo = ResolveCamoTypeNameToIndex(outfitName);
        if (labelCamo < 0)
        {
            LogDebug("[OutfitLua] ExtendVanillaOutfit: unknown outfit name '%s' - "
                     "use a vanilla camo name (BATTLEDRESS, SNEAKING_SUIT_TPP, "
                     "NAKED) or a raw partsType\n", outfitName);
            PushLuaBool(L, false);
            return 1;
        }
        vanillaPartsType = outfit::ResolveVanillaPartsTypeForCamo(
            static_cast<std::uint8_t>(labelCamo));
    }
    else
    {
        int rawCamo  = -1;
        int rawParts = -1;
        if (TryReadTableIntField(L, 1, "outfit", rawCamo)
            && rawCamo >= 0
            && rawCamo <= static_cast<int>(outfit::kVanillaCamoTypeMax))
        {
            labelCamo = rawCamo;
            vanillaPartsType = outfit::ResolveVanillaPartsTypeForCamo(
                static_cast<std::uint8_t>(rawCamo));
        }
        else if (TryReadTableIntField(L, 1, "partsType", rawParts)
                 && rawParts >= 0
                 && rawParts < static_cast<int>(outfit::kCustomPartsTypeStart))
        {
            vanillaPartsType = static_cast<std::uint8_t>(rawParts);
        }
    }

    if (vanillaPartsType == 0xFF)
    {
        LogDebug("[OutfitLua] ExtendVanillaOutfit: could not resolve a vanilla "
            "partsType (missing/invalid 'outfit' name/camoType or "
            "'partsType'%s)\n",
            labelCamo >= 0 ? "; engine camo->partsType map unavailable" : "");
        PushLuaBool(L, false);
        return 1;
    }

    bool any = false;
    ForEachPlayerTypeBranch(L, 1, "ExtendVanillaOutfit", outfitName,
        [&](std::uint8_t playerType, const char* branchName, int branchIdx)
    {
        std::uint16_t ids[outfit::kMaxHeadOptionsPerOutfit]  = {};
        std::uint8_t  idCount = 0;
        std::uint64_t pend[outfit::kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t  pendCount = 0;
        ReadHeadOptionsArrayInto(L, branchIdx, ids, idCount, pend, pendCount);
        const std::uint8_t headScopeCamo =
            (labelCamo >= 0) ? static_cast<std::uint8_t>(labelCamo)
                             : outfit::kHeadOptionAnyCamo;
        if ((idCount > 0 || pendCount > 0)
            && outfit::ExtendVanillaSuitHeadOptions(vanillaPartsType,
                   playerType, headScopeCamo, ids, idCount,
                   pend, pendCount))
        {
            any = true;
            LogDebug("[Outfit] ExtendVanillaOutfit '%s' (camo %d) -> vanilla "
                "partsType 0x%02X: %s +%u head option(s) (+%u deferred)\n",
                outfitName ? outfitName : "(by number)",
                labelCamo,
                static_cast<unsigned>(vanillaPartsType),
                branchName,
                static_cast<unsigned>(idCount),
                static_cast<unsigned>(pendCount));
        }

        const std::uint64_t suitVoice = ReadSubAssetField(
            L, branchIdx, "voiceFpk", outfit::kSubAssetUseVanilla);
        if (suitVoice > outfit::kSubAssetUseVanilla
            && outfit::ExtendVanillaSuitVoice(vanillaPartsType,
                   playerType,
                   labelCamo >= 0 ? static_cast<std::uint8_t>(labelCamo)
                                  : std::uint8_t{0xFF},
                   suitVoice))
        {
            any = true;
            LogDebug("[Outfit] ExtendVanillaOutfit '%s' (camo %d) -> vanilla "
                "partsType 0x%02X: %s suit-wide voiceFpk set (applies to "
                "base + native variations + variants, kept in FOB)\n",
                outfitName ? outfitName : "(by number)",
                labelCamo,
                static_cast<unsigned>(vanillaPartsType),
                branchName);
        }

        bool hasArm = false, armVal = true;
        LuaGetField(L, branchIdx, "enableArm");
        if (LuaType(L, -1) != 0) { hasArm = true; armVal = GetLuaBool(L, -1) != 0; }
        LuaPop(L, 1);
        bool hasHead = false, headVal = false;
        LuaGetField(L, branchIdx, "enableHead");
        if (LuaType(L, -1) != 0) { hasHead = true; headVal = GetLuaBool(L, -1) != 0; }
        LuaPop(L, 1);
        if ((hasArm || hasHead)
            && outfit::ExtendVanillaSuitArmHead(vanillaPartsType, playerType,
                   labelCamo >= 0 ? static_cast<std::uint8_t>(labelCamo)
                                  : std::uint8_t{0xFF},
                   hasArm, armVal, hasHead, headVal))
        {
            any = true;
            LogDebug("[Outfit] ExtendVanillaOutfit '%s' (camo %d) -> vanilla "
                "partsType 0x%02X: %s arm/head override (arm=%s head=%s)\n",
                outfitName ? outfitName : "(by number)",
                labelCamo, static_cast<unsigned>(vanillaPartsType), branchName,
                hasArm ? (armVal ? "on" : "off") : "-",
                hasHead ? (headVal ? "on" : "off") : "-");
        }

        outfit::OutfitPlayerTypeData abilityBranch;
        ReadBranchAbilities(L, branchIdx, abilityBranch, branchName);
        if (abilityBranch.abilitySilentSteps
            || abilityBranch.abilityDefense
            || abilityBranch.abilityLifeRecovery
            || abilityBranch.abilityRattleSuit != outfit::kSoundSwitchUnset
            || abilityBranch.abilityDamageSe != outfit::kSoundSwitchUnset
            || abilityBranch.suitParamKind != 0)
        {
            outfit::VanillaSuitAbilities va{};
            va.silentSteps   = abilityBranch.abilitySilentSteps;
            va.defense       = abilityBranch.abilityDefense;
            va.lifeRecovery  = abilityBranch.abilityLifeRecovery;
            va.rattleSuit    = abilityBranch.abilityRattleSuit;
            va.damageSe      = abilityBranch.abilityDamageSe;
            va.suitParamKind = abilityBranch.suitParamKind;
            if (outfit::ExtendVanillaSuitAbilities(vanillaPartsType, playerType,
                    labelCamo >= 0 ? static_cast<std::uint8_t>(labelCamo)
                                   : std::uint8_t{0xFF},
                    va))
            {
                any = true;
                LogDebug("[Outfit] ExtendVanillaOutfit '%s' (camo %d) -> vanilla "
                    "partsType 0x%02X: %s abilities set (silentFootsteps=%d "
                    "defense=%u lifeRecovery=%u rattleSuit=0x%08X "
                    "damageSe=0x%08X quietMovement=%d) - they are dropped in "
                    "FOB like every other edit to a vanilla suit\n",
                    outfitName ? outfitName : "(by number)",
                    labelCamo,
                    static_cast<unsigned>(vanillaPartsType),
                    branchName,
                    va.silentSteps ? 1 : 0,
                    static_cast<unsigned>(va.defense),
                    static_cast<unsigned>(va.lifeRecovery),
                    static_cast<unsigned>(va.rattleSuit),
                    static_cast<unsigned>(va.damageSe),
                    va.suitParamKind != 0 ? 1 : 0);
            }
        }

        outfit::VanillaSuitVariantAsset
            vars[outfit::kMaxVariantsPerOutfit] = {};
        const std::uint8_t vCount = ReadVanillaExtVariants(
            L, branchIdx, vars, outfit::kMaxVariantsPerOutfit - 1);
        if (vCount > 0 && labelCamo < 0)
        {
            LogDebug("[OutfitLua] ExtendVanillaOutfit: variants need a "
                     "camo-named outfit (got raw partsType 0x%02X) - variants "
                     "skipped; use a camo name or number\n", static_cast<unsigned>(vanillaPartsType));
        }
        else if (vCount > 0
            && outfit::ExtendVanillaSuitVariants(vanillaPartsType,
                   playerType, static_cast<std::uint8_t>(labelCamo),
                   vars, vCount))
        {
            any = true;
            std::uint8_t vAbil = 0;
            for (std::uint8_t vi = 0; vi < vCount; ++vi)
                if (vars[vi].abilities.declared)
                    ++vAbil;
            LogDebug("[Outfit] ExtendVanillaOutfit '%s' (camo %d) -> vanilla "
                "partsType 0x%02X: %s +%u variant(s), %u of them declare their "
                "own abilities (those override the branch abilities while that "
                "variant is worn)\n",
                outfitName ? outfitName : "(by number)",
                labelCamo,
                static_cast<unsigned>(vanillaPartsType),
                branchName,
                static_cast<unsigned>(vCount),
                static_cast<unsigned>(vAbil));
        }
    });

    if (!any)
    {
        LogDebug("[OutfitLua] ExtendVanillaOutfit: no playerType branch with a "
            "headOptions or variants array - nothing registered\n");
        PushLuaBool(L, false);
        return 1;
    }

    PushLuaNumber(L, static_cast<float>(vanillaPartsType));
    return 1;
}

int __cdecl l_GetOutfitInfo(lua_State* L)
{
    const char* key = nullptr;
    if (LuaType(L, 1) == LUA_TSTRING)
        key = GetLuaString(L, 1);
    else if (LuaType(L, 1) == LUA_TTABLE)
        TryReadTableStringField(L, 1, "key", key);

    if (!key || !key[0])
    {
        LogDebug("[OutfitLua] GetOutfitInfo: pass the outfit key string "
            "(or a table with key=)\n");
        PushLuaBool(L, false);
        return 1;
    }

    const std::int32_t developId = V_FrameWorkState::GetDevelopIdByKey(key);
    if (developId <= 0 || developId > 0xFFFF)
    {
        LogDebug("[OutfitLua] GetOutfitInfo: unknown outfit key '%s' "
            "(RegisterOutfit must run before this)\n", key);
        PushLuaBool(L, false);
        return 1;
    }

    const outfit::OutfitEntry* e = nullptr;
    if (!outfit::TryGetOutfitByDevelopId(
            static_cast<std::uint16_t>(developId), &e) || !e)
    {
        LogDebug("[OutfitLua] GetOutfitInfo: key '%s' has developId=%d "
            "but no registered outfit entry\n", key, developId);
        PushLuaBool(L, false);
        return 1;
    }

    if (!e->bound)
    {
        outfit::BindOutfit(static_cast<std::uint16_t>(developId), true,
                           "lua-parts-bytes");
        if (!outfit::TryGetOutfitByDevelopId(
                static_cast<std::uint16_t>(developId), &e) || !e || !e->bound)
        {
            LogDebug("[OutfitLua] GetOutfitInfo: '%s' could not take "
                "live bytes (pools full) - returns false, vars stay vanilla\n",
                key);
            PushLuaBool(L, false);
            return 1;
        }
    }

    PushLuaNumber(L, static_cast<float>(e->partsType));
    PushLuaNumber(L, static_cast<float>(e->selectorCode));
    PushLuaNumber(L, static_cast<float>(e->developId));
    return 3;
}

int __cdecl l_AddToEquipDevelopTable(lua_State* L)
{
    OutfitLua_EnsureEquipDevelopBound();
    return EquipDevelopAdd::Lua_AddToEquipDevelopTable(L);
}
