#include "pch.h"

extern "C" {
    #include "lua.h"
    #include "lauxlib.h"
}

#include "log.h"
#include "LuaApi.h"
#include "V_TppMotherBaseManagementLib.h"
#include "ChangeLocationMenu.h"
#include "UniqueStaffRegistry.h"
#include "GetPhotoAdditionalTextLangId.h"
#include "OutfitLuaBindings.h"
#include "V_TppGameObjectConstants.h"
#include "../hooks/equip/EquipDevelop_SetEquipUndeveloped.h"
#include "../core/V_FrameWorkState.h"
#include "../hooks/equip/CustomBluePrint.h"
#include "../hooks/equip/DataBaseControllerImpl_AddDataBase.h"
#include "FoxHashes.h"

#include <string>

namespace
{
    // V_TppMotherBaseManagement.AddToChangeLocationMenu({ locationCode1, locationCode2, ... })
    static int __cdecl l_AddToChangeLocationMenu(lua_State* L)
    {
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            LogDebug("[V_TppMotherBaseManagement] AddToChangeLocationMenu expected a table\n");
            return 0;
        }

        for (g_lua_pushnil(L); g_lua_next(L, -2); LuaPop(L, 1))
        {
            if (LuaType(L, -1) == LUA_TNUMBER)
                AddLocationIdToChangeLocationMenu(static_cast<unsigned short>(GetLuaInt(L, -1)));
        }
        return 0;
    }

    // V_TppMotherBaseManagement.AddPhotoAdditionalText({ {missionCode=, photoId=, photoType=, targetTypeLangId=""}, ... })
    static int __cdecl l_AddPhotoAdditionalText(lua_State* L)
    {
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            LogDebug("[V_TppMotherBaseManagement] AddPhotoAdditionalText expected a table\n");
            return 0;
        }

        for (g_lua_pushnil(L); g_lua_next(L, -2); LuaPop(L, 1))
        {
            if (LuaType(L, -1) != LUA_TTABLE)
                continue;

            unsigned short missionCode = 0xFFFF;
            unsigned char photoId = 0xFF;
            unsigned char photoType = 0xFF;
            const char* targetTypeLangIdStr = "";

            LuaGetField(L, -1, "missionCode");
            if (LuaType(L, -1) == LUA_TNUMBER)
                missionCode = static_cast<unsigned short>(GetLuaInt(L, -1));
            LuaPop(L, 1);

            LuaGetField(L, -1, "photoId");
            if (LuaType(L, -1) == LUA_TNUMBER)
                photoId = static_cast<unsigned char>(GetLuaInt(L, -1));
            LuaPop(L, 1);

            LuaGetField(L, -1, "photoType");
            if (LuaType(L, -1) == LUA_TNUMBER)
                photoType = static_cast<unsigned char>(GetLuaInt(L, -1));
            LuaPop(L, 1);

            LuaGetField(L, -1, "targetTypeLangId");
            if (LuaType(L, -1) == LUA_TSTRING)
                targetTypeLangIdStr = GetLuaString(L, -1);
            LuaPop(L, 1);

            if (missionCode == 0xFFFF || photoId == 0xFF || photoType == 0xFF)
                continue;

            AddPhotoAdditionalText(missionCode, photoId, photoType, targetTypeLangIdStr);
        }
        return 0;
    }

    // V_TppMotherBaseManagement.GetDevelopId(key)
    static int __cdecl l_GetDevelopId(lua_State* L)
    {
        std::int32_t developId = 0;
        if (LuaType(L, 1) == LUA_TSTRING)
        {
            const char* key = GetLuaString(L, 1);
            if (key && key[0])
                developId = V_FrameWorkState::GetDevelopIdByKey(key);
        }

        if (developId > 0)
            PushLuaNumber(L, static_cast<float>(developId));
        else
            g_lua_pushnil(L);
        return 1;
    }


    static const char* BluePrintKeyArg(lua_State* L)
    {
        if (LuaType(L, 1) != LUA_TSTRING)
            return nullptr;
        const char* key = GetLuaString(L, 1);
        return (key && key[0]) ? key : nullptr;
    }

    static std::int32_t BluePrintSlotArg(lua_State* L)
    {
        if (LuaType(L, 1) == LUA_TNUMBER)
            return bluePrint::SlotFromPublicId(GetLuaInt(L, 1));

        const char* key = BluePrintKeyArg(L);
        return key ? bluePrint::Find(key) : 0;
    }

    static int __cdecl l_RegisterDataBase(lua_State* L)
    {
        const char* key = BluePrintKeyArg(L);
        const std::int32_t slot = key ? bluePrint::Register(key) : 0;
        if (slot > 0)
        {
            bluePrint::InjectConstantFor(L, key);
            PushLuaNumber(L, static_cast<float>(bluePrint::PublicId(slot)));
        }
        else
        {
            g_lua_pushnil(L);
        }
        return 1;
    }

    static std::int32_t DocumentationTargetId(lua_State* L)
    {
        const int top = GetLuaTop(L);
        std::int32_t publicId = 0;

        LuaGetField(L, 1, "dataBaseId");
        if (LuaIsNumber(L, -1))
        {
            publicId = GetLuaInt(L, -1);
        }
        else
        {
            g_lua_settop(L, top);
            LuaGetField(L, 1, "key");
            if (const char* key = GetLuaString(L, -1))
            {
                const std::int32_t slot = bluePrint::Find(key);
                if (slot > 0)
                    publicId = bluePrint::PublicId(slot);
            }
        }

        g_lua_settop(L, top);
        return (bluePrint::SlotFromPublicId(publicId) > 0) ? publicId : 0;
    }

    static std::uint64_t DocumentationHashField(lua_State* L, const char* field, bool assetPath)
    {
        const int top = GetLuaTop(L);
        std::uint64_t hash = 0;

        LuaGetField(L, 1, field);
        if (LuaIsNumber(L, -1))
            hash = static_cast<std::uint64_t>(GetLuaInt(L, -1));
        else if (const char* text = GetLuaString(L, -1))
            hash = assetPath ? FoxHashes::PathCode64Ext(text) : FoxHashes::StrCode64(text);

        g_lua_settop(L, top);
        return hash;
    }

    static std::uint64_t DocumentationPathField(lua_State* L, const char* field,
                                                const char* legacyField)
    {
        if (const std::uint64_t hash = DocumentationHashField(L, field, true))
            return hash;

        const std::uint64_t legacy = DocumentationHashField(L, legacyField, true);
        if (legacy != 0)
        {
            static bool s_said = false;
            if (!s_said)
            {
                s_said = true;
                Log("[BluePrint] SetDataBaseDisplay was given '%s', which is now called '%s' - "
                    "the old name is still honoured but will not be forever; it was renamed "
                    "because this field takes an asset path, not a lang id\n",
                    legacyField, field);
            }
        }
        return legacy;
    }

    static int __cdecl l_SetDataBaseDisplay(lua_State* L)
    {
        if (LuaType(L, 1) != LUA_TTABLE)
        {
            Log("[BluePrint] SetDataBaseDisplay expects a table - the DATABASE row for that "
                "blueprint keeps the default '???' name\n");
            PushLuaBool(L, false);
            return 1;
        }

        const std::int32_t publicId = DocumentationTargetId(L);
        if (publicId <= 0)
        {
            Log("[BluePrint] SetDataBaseDisplay was given no dataBaseId or key that matches a "
                "registered blueprint, so no DATABASE row was named\n");
            PushLuaBool(L, false);
            return 1;
        }

        bluePrintDb::Documentation doc;
        doc.nameHash  = DocumentationHashField(L, "langDocName", false);
        doc.iconHash  = DocumentationPathField(L, "docIcon", "langDocIcon");
        doc.imageHash = DocumentationPathField(L, "docImage", "langDocImage");
        doc.infoHash  = DocumentationHashField(L, "langDocInfo", false);

        const int top = GetLuaTop(L);
        LuaGetField(L, 1, "tab");
        if (LuaIsNumber(L, -1))
        {
            const std::int32_t tab = GetLuaInt(L, -1);
            if (IsListableDataBaseTab(tab))
            {
                doc.tab = static_cast<std::uint8_t>(tab);
            }
            else
            {
                Log("[BluePrint] SetDataBaseDisplay for dataBaseId %d asked for DATABASE tab %d, "
                    "which the game builds no list for, so the row would never appear - kept it on "
                    "tab %u instead; use one of the V_TppDataBase.TAB_* values\n",
                    publicId, tab, static_cast<unsigned>(doc.tab));
            }
        }
        g_lua_settop(L, top);

        if (doc.nameHash == 0 && doc.iconHash == 0 && doc.imageHash == 0 && doc.infoHash == 0)
        {
            Log("[BluePrint] SetDataBaseDisplay for dataBaseId %d carried none of langDocName, "
                "docIcon, docImage or langDocInfo, so its DATABASE row keeps the default "
                "'???' text and '?' icon\n", publicId);
            PushLuaBool(L, false);
            return 1;
        }

        PushLuaBool(L, bluePrintDb::SetDocumentation(publicId, doc));
        return 1;
    }

    static int __cdecl l_SetBluePrint(lua_State* L)
    {
        const std::int32_t slot = BluePrintSlotArg(L);
        if (slot <= 0)
        {
            PushLuaBool(L, false);
            return 1;
        }

        if (LuaType(L, 2) != LUA_TBOOLEAN)
        {
            LogDebug("[BluePrint] SetBluePrint: argument #2 must be a boolean - true "
                     "grants the blueprint, false revokes it. Nothing was changed\n");
            PushLuaBool(L, false);
            return 1;
        }

        const bool owned        = GetLuaBool(L, 2);
        const bool alreadyOwned = bluePrint::Has(slot);
        const bool ok           = bluePrint::Set(slot, owned);
        if (ok)
            bluePrint::SetNew(slot, owned);
        if (ok && owned && !alreadyOwned)
            bluePrintDb::AnnounceBluePrintObtained(L, bluePrint::PublicId(slot));
        PushLuaBool(L, ok);
        return 1;
    }

    static int __cdecl l_GetBluePrintId(lua_State* L)
    {
        const std::int32_t slot = BluePrintSlotArg(L);
        if (slot > 0)
            PushLuaNumber(L, static_cast<float>(bluePrint::PublicId(slot)));
        else
            g_lua_pushnil(L);
        return 1;
    }

    static int __cdecl l_HasBluePrint(lua_State* L)
    {
        const std::int32_t slot = BluePrintSlotArg(L);
        PushLuaBool(L, slot > 0 ? bluePrint::Has(slot) : false);
        return 1;
    }

    struct UniqueStaffField
    {
        const char* name;
        int         requiredType;
    };

    static const UniqueStaffField kUniqueStaffRequiredFields[] =
    {
        { "nameLangMessageId",   LUA_TSTRING },
        { "combatSectionPoint",  LUA_TNUMBER },
        { "developSectionPoint", LUA_TNUMBER },
        { "baseDevSectionPoint", LUA_TNUMBER },
        { "supportSectionPoint", LUA_TNUMBER },
        { "spySectionPoint",     LUA_TNUMBER },
        { "medicalSectionPoint", LUA_TNUMBER },
        { "isEnmity",            LUA_TNONE },
        { "moraleEnmity",        LUA_TNUMBER },
        { "condition",           LUA_TSTRING },
        { "badConditionWeight",  LUA_TNUMBER },
        { "langProficEnglish",   LUA_TNONE },
        { "langProficRussian",   LUA_TNONE },
        { "langProficPashto",    LUA_TNONE },
        { "langProficKikongo",   LUA_TNONE },
        { "langProficAfrikaans", LUA_TNONE },
    };

    static bool UniqueStaffFieldsComplete(lua_State* L, const char* label)
    {
        const int top = GetLuaTop(L);

        for (const UniqueStaffField& field : kUniqueStaffRequiredFields)
        {
            LuaGetField(L, 1, field.name);
            const int type = LuaType(L, -1);
            g_lua_settop(L, top);

            if (type == LUA_TNIL || type == LUA_TNONE)
            {
                Log("[UniqueStaff] '%s' carries no %s - the game discards any unique-staff "
                    "record that is missing a mandatory field, so nothing was registered\n",
                    label, field.name);
                return false;
            }

            if (field.requiredType != LUA_TNONE && type != field.requiredType)
            {
                Log("[UniqueStaff] '%s' has %s of the wrong type (%s expected) - the game "
                    "discards the whole record, so nothing was registered\n",
                    label, field.name,
                    field.requiredType == LUA_TSTRING ? "a string" : "a number");
                return false;
            }
        }
        return true;
    }

    static int __cdecl l_RegisterUniqueStaff(lua_State* L)
    {
        if (!ResolveLuaApi() || !g_lua_pcall || !g_lua_settop || !g_lua_rawset
            || !g_lua_pushvalue)
        {
            Log("[UniqueStaff] the Lua API is unavailable, so no unique staffer was "
                "registered\n");
            g_lua_pushnil(L);
            return 1;
        }

        if (LuaType(L, 1) != LUA_TTABLE)
        {
            Log("[UniqueStaff] RegisterUniqueStaff expects a table - nothing was registered\n");
            g_lua_pushnil(L);
            return 1;
        }

        const int top = GetLuaTop(L);

        std::string key;
        LuaGetField(L, 1, "key");
        if (LuaType(L, -1) == LUA_TSTRING)
        {
            if (const char* text = GetLuaString(L, -1))
                key = text;
        }
        g_lua_settop(L, top);

        std::int32_t typeId = 0;

        if (!key.empty())
        {
            if (!uniqueStaff::IsValidKey(key.c_str()))
            {
                Log("[UniqueStaff] key '%s' is not usable - a key must start with a letter or "
                    "an underscore and hold only letters, digits, '_', '.' or '-' (96 "
                    "characters max), so nothing was registered\n", key.c_str());
                g_lua_pushnil(L);
                return 1;
            }

            typeId = uniqueStaff::Register(key.c_str());
            if (typeId <= 0)
            {
                Log("[UniqueStaff] no free uniqueTypeId is left for '%s' - all %d assignable "
                    "ids are claimed, so this staffer was not registered and "
                    "TppEnemy.AssignUniqueStaffType will never place it\n",
                    key.c_str(), uniqueStaff::PoolSize());
                g_lua_pushnil(L);
                return 1;
            }
        }
        else
        {
            LuaGetField(L, 1, "uniqueTypeId");
            if (LuaIsNumber(L, -1))
                typeId = GetLuaInt(L, -1);
            g_lua_settop(L, top);

            if (typeId <= 0 || typeId > 253)
            {
                Log("[UniqueStaff] RegisterUniqueStaff was given neither a key nor a "
                    "uniqueTypeId in 1..253, so nothing was registered\n");
                g_lua_pushnil(L);
                return 1;
            }

            if (const char* owner = uniqueStaff::KeyHoldingId(typeId))
            {
                Log("[UniqueStaff] uniqueTypeId %d was passed directly, but the key '%s' "
                    "already owns it - registering would overwrite that staffer, so nothing "
                    "was registered; use key= instead of a literal id\n", typeId, owner);
                g_lua_pushnil(L);
                return 1;
            }
        }

        const char* label = key.empty() ? "<uniqueTypeId>" : key.c_str();

        if (!UniqueStaffFieldsComplete(L, label))
        {
            g_lua_pushnil(L);
            return 1;
        }

        PushLuaString(L, "uniqueTypeId");
        PushLuaNumber(L, static_cast<float>(typeId));
        g_lua_rawset(L, 1);

        LuaGetField(L, LUA_GLOBALSINDEX_51, "TppMotherBaseManagement");
        if (LuaType(L, -1) != LUA_TTABLE)
        {
            g_lua_settop(L, top);
            Log("[UniqueStaff] the global TppMotherBaseManagement table is not loaded yet, so "
                "'%s' never reached the game's registrar and no staffer exists\n", label);
            g_lua_pushnil(L);
            return 1;
        }

        LuaGetField(L, -1, "RegisterUniqueStaff");
        if (LuaType(L, -1) != LUA_TFUNCTION)
        {
            g_lua_settop(L, top);
            Log("[UniqueStaff] TppMotherBaseManagement.RegisterUniqueStaff is missing, so '%s' "
                "was not registered\n", label);
            g_lua_pushnil(L);
            return 1;
        }

        g_lua_pushvalue(L, 1);
        if (g_lua_pcall(L, 1, 0, 0) != 0)
        {
            const char* err = LuaIsString(L, -1) ? GetLuaString(L, -1) : nullptr;
            Log("[UniqueStaff] the game's RegisterUniqueStaff rejected '%s': %s - id %d stays "
                "reserved for this key but no staffer exists\n",
                label, err ? err : "?", typeId);
            g_lua_settop(L, top);
            g_lua_pushnil(L);
            return 1;
        }

        g_lua_settop(L, top);
        PushLuaNumber(L, static_cast<float>(typeId));
        return 1;
    }

    static int __cdecl l_GetUniqueStaffTypeId(lua_State* L)
    {
        std::int32_t typeId = 0;
        if (LuaType(L, 1) == LUA_TSTRING)
        {
            if (const char* key = GetLuaString(L, 1))
                typeId = uniqueStaff::Find(key);
        }

        if (typeId > 0)
            PushLuaNumber(L, static_cast<float>(typeId));
        else
            g_lua_pushnil(L);
        return 1;
    }

    static luaL_Reg g_VTppMotherBaseManagementLib[] =
    {
        { "AddToChangeLocationMenu", l_AddToChangeLocationMenu },
        { "AddPhotoAdditionalText",  l_AddPhotoAdditionalText },

        { "GetDevelopId",            l_GetDevelopId },
        { "SetEquipDeveloped",       l_SetEquipDeveloped },
        { "SetEquipUndeveloped",     l_SetEquipUndeveloped },
        { "IsEquipDevelopable",      l_IsEquipDevelopable },
        { "IsEquipDeveloped",        l_IsEquipDeveloped },
        { "SetEquipNew",             l_SetEquipNew },
        { "IsEquipNew",              l_IsEquipNew },
        { "SetEquipDevelopVisible",  l_SetEquipDevelopVisible },

        { "AddToEquipDevelopTable",  l_AddToEquipDevelopTable },

        { "RegisterUniqueStaff",     l_RegisterUniqueStaff },
        { "GetUniqueStaffTypeId",    l_GetUniqueStaffTypeId },

        { "RegisterDataBase",       l_RegisterDataBase },
        { "SetDataBaseDisplay",        l_SetDataBaseDisplay },
        { "SetBluePrint",            l_SetBluePrint },
        { "HasBluePrint",            l_HasBluePrint },
        { "GetBluePrintId",          l_GetBluePrintId },


        { nullptr, nullptr }
    };
}

bool Register_V_TppMotherBaseManagementLibrary(lua_State* L)
{
    return RegisterLuaLibrary(L, "V_TppMotherBaseManagement", g_VTppMotherBaseManagementLib);
}
