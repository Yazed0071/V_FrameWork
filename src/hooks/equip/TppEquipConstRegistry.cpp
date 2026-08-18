#include "pch.h"
#include "TppEquipConstRegistry.h"

#include <map>
#include <mutex>
#include <string>

#include "log.h"
#include "V_FrameWorkState.h"
#include "LuaApi.h"
#include "GunBasicInject.h"
#include "EquipPartParams.h"

namespace
{
    static const char* const kSpaceTags[kTppEquipConstSpaceCount] =
    {
        "EQP",
        "EQP_TYPE",
        "SWP_TYPE",
        "EQP_BLOCK",
        "SWP",
        "BL",
        "BLA",
        "CASING",
        "MZ",
        "LTLS",
        "WP",
        "MO",
        "UB",
        "AM",
        "ST",
        "RC",
        "BA",
        "SK",
        "RETICLE_UI",
        "SCOPE_UI",
        "BARREL_LENGTH",
        "RICOCHET_SIZE",
        "BULLET_TYPE",
        "PENETRATE_LEVEL",
        "TRIGGER",
        "WEAPON_PAINT",
    };

    static constexpr std::int32_t kSpaceEqp            = 0;
    static constexpr std::int32_t kSpaceBl             = 5;
    static constexpr std::int32_t kSpaceLtls           = 9;
    static constexpr std::int32_t kSpaceWp             = 10;
    static constexpr std::int32_t kSpaceMo             = 11;
    static constexpr std::int32_t kSpaceUb             = 12;
    static constexpr std::int32_t kSpaceAm             = 13;
    static constexpr std::int32_t kSpaceSt             = 14;
    static constexpr std::int32_t kSpaceRc             = 15;
    static constexpr std::int32_t kSpaceBa             = 16;
    static constexpr std::int32_t kSpaceSk             = 17;
    static constexpr std::int32_t kDefaultConstantBase = 0x4000;
    static bool IsValidConstantName(const char* name)
    {
        if (!name || !name[0])
            return false;
        size_t len = 0;
        for (const char* p = name; *p; ++p, ++len)
        {
            const char c = *p;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_';
            if (!ok)
                return false;
        }
        return len <= 128;
    }

    static std::mutex g_Mutex;
    static std::map<std::string, std::int32_t> g_Constants;

    static void WriteConstantTo(lua_State* L, const char* table, const char* name, std::int32_t value)
    {
        const int top0 = g_lua_gettop(L);
        g_lua_getfield(L, LUA_GLOBALSINDEX_51, const_cast<char*>(table));
        if (g_lua_type(L, -1) == LUA_TTABLE)
        {
            g_lua_pushstring(L, const_cast<char*>(name));
            g_lua_pushnumber(L, static_cast<lua_Number>(value));
            g_lua_settable(L, -3);
        }
        g_lua_settop(L, top0);
    }

    static void WriteConstant(lua_State* L, const char* name, std::int32_t value)
    {
        WriteConstantTo(L, "TppEquip", name, value);
    }

    static std::map<std::string, std::int32_t> g_DamageConstants;
}

bool TppEquipConst_Declare(
    int spaceIndex,
    const char* name,
    std::uint32_t& outValue,
    lua_State* L)
{
    outValue = 0;
    if (spaceIndex < 0 || spaceIndex >= kTppEquipConstSpaceCount)
        return false;
    if (!IsValidConstantName(name))
    {
        Log("[TppEquipConst] ERROR: invalid constant name '%s' (identifiers only, [A-Za-z0-9_], max 128 chars); rejected.\n",
            name ? name : "(null)");
        return false;
    }

    std::int32_t value = 0;
    if (spaceIndex == kSpaceEqp)
    {
        if (!V_FrameWorkState::ResolveOrCreateEquipId(
                name, 0, value, std::strncmp(name, "EQP_WP_", 7) == 0))
        {
            Log("[TppEquipConst] ERROR: no free native equip slot for '%s'.\n", name);
            return false;
        }
    }
    else if (spaceIndex == kSpaceWp)
    {
        const int weaponId = GunBasic_AllocateWeaponIdForName(name);
        if (weaponId > 0)
        {
            value = weaponId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceAm)
    {
        const int ammoId = EquipParam_AllocateMagazineSlotForName(name);
        if (ammoId > 0)
        {
            value = ammoId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceSk)
    {
        const int stockId = EquipParam_AllocateStockSlotForName(name);
        if (stockId > 0)
        {
            value = stockId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceMo)
    {
        const int muzzleId = EquipParam_AllocateMuzzleSlotForName(name);
        if (muzzleId > 0)
        {
            value = muzzleId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceRc)
    {
        const int receiverId = EquipParam_AllocateReceiverSlotForName(name);
        if (receiverId > 0)
        {
            value = receiverId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceSt)
    {
        const int scopeId = EquipParam_AllocateSightSlotForName(name);
        if (scopeId > 0)
        {
            value = scopeId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceBa)
    {
        const int barrelId = EquipParam_AllocateBarrelSlotForName(name);
        if (barrelId > 0)
        {
            value = barrelId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceUb)
    {
        const int underBarrelId = EquipParam_AllocateUnderBarrelSlotForName(name);
        if (underBarrelId > 0)
        {
            value = underBarrelId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceLtls)
    {
        const int optionId = EquipParam_AllocateOptionSlotForName(name);
        if (optionId > 0)
        {
            value = optionId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (spaceIndex == kSpaceBl)
    {
        const int bulletId = EquipParam_AllocateBulletSlotForName(name);
        if (bulletId > 0)
        {
            value = bulletId;
        }
        else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                     kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
        {
            return false;
        }
    }
    else if (!V_FrameWorkState::ResolveOrCreateConstantValue(
                 kSpaceTags[spaceIndex], name, kDefaultConstantBase, value))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Constants[name] = value;
    }

    if (L && ResolveLuaApi())
        WriteConstant(L, name, value);

    outValue = static_cast<std::uint32_t>(value);
#ifdef _DEBUG
    LogDebug("[TppEquipConst] TppEquip.%s = %d (space %s)\n",
        name, value, kSpaceTags[spaceIndex]);
#endif
    return true;
}

void TppEquipConst_SetValue(const char* name, std::uint32_t value, lua_State* L)
{
    if (!name || !name[0])
        return;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Constants[name] = static_cast<std::int32_t>(value);
    }
    if (L && ResolveLuaApi())
        WriteConstant(L, name, value);
}

void TppDamageConst_SetValue(const char* name, std::uint32_t value, lua_State* L)
{
    if (!name || !name[0])
        return;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_DamageConstants[name] = static_cast<std::int32_t>(value);
    }
    if (L && ResolveLuaApi())
        WriteConstantTo(L, "TppDamage", name, value);
}

void TppDamageConst_InjectAll(lua_State* L)
{
    std::map<std::string, std::int32_t> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        snapshot = g_DamageConstants;
    }
    if (snapshot.empty() || !L || !ResolveLuaApi())
        return;

    for (const auto& kv : snapshot)
        WriteConstantTo(L, "TppDamage", kv.first.c_str(), kv.second);
}

void TppEquipConst_InjectAll(lua_State* L)
{
    std::map<std::string, std::int32_t> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        snapshot = g_Constants;
    }
    if (snapshot.empty() || !L || !ResolveLuaApi())
        return;

    for (const auto& kv : snapshot)
        WriteConstant(L, kv.first.c_str(), kv.second);

#ifdef _DEBUG
    LogDebug("[TppEquipConst] injected %u custom constant(s) into L=%p\n",
        static_cast<unsigned>(snapshot.size()), L);
#endif
}

void TppEquipConst_ResetRuntimeState()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Constants.clear();
    g_DamageConstants.clear();
}

int TppEquipDeclareForSpace(lua_State* L, int spaceIndex)
{
    if (g_lua_type && g_lua_type(L, 1) == LUA_TTABLE && g_lua_objlen && g_lua_rawgeti && g_lua_settop)
    {
        const int n = static_cast<int>(g_lua_objlen(L, 1));
        int declared = 0;
        for (int i = 1; i <= n; ++i)
        {
            g_lua_rawgeti(L, 1, i);
            const char* name = GetLuaString(L, -1);
            if (name && name[0])
            {
                std::uint32_t value = 0;
                if (TppEquipConst_Declare(spaceIndex, name, value, L))
                    ++declared;
            }
            g_lua_settop(L, -2);
        }
        if (g_lua_pushnumber)
            g_lua_pushnumber(L, static_cast<lua_Number>(declared));
        else
            PushLuaBool(L, declared > 0);
        return 1;
    }

    const char* name = GetLuaString(L, 1);

    std::uint32_t value = 0;
    if (!name || !name[0] ||
        !TppEquipConst_Declare(spaceIndex, name, value, L))
    {
        PushLuaBool(L, false);
        return 1;
    }

    if (g_lua_pushnumber)
        g_lua_pushnumber(L, static_cast<lua_Number>(value));
    else
        PushLuaBool(L, true);
    return 1;
}
