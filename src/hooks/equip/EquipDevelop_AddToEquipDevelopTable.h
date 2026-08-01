#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct lua_State;

namespace EquipDevelopAdd
{
    struct Deps
    {
        bool (*ResolveLuaApi)() = nullptr;

        int (*GetLuaTop)(lua_State* L) = nullptr;
        int (*LuaType)(lua_State* L, int idx) = nullptr;
        bool (*LuaToBool)(lua_State* L, int idx) = nullptr;
        void (*LuaSetTop)(lua_State* L, int idx) = nullptr;

        const char* (*GetLuaString)(lua_State* L, int idx) = nullptr;
        int (*GetLuaInt)(lua_State* L, int idx) = nullptr;
        void (*PushLuaNumber)(lua_State* L, float value) = nullptr;

        void (*LuaPushString)(lua_State* L, const char* value) = nullptr;
        void (*LuaCreateTable)(lua_State* L, int narr, int nrec) = nullptr;
        void (*LuaGetTable)(lua_State* L, int idx) = nullptr;
        void (*LuaSetTable)(lua_State* L, int idx) = nullptr;
    };

    void Bind(const Deps& deps);

    int __cdecl Lua_AddToEquipDevelopTable(lua_State* L);

    bool IsDevelopIdParked(std::uint32_t developId);

    void MaybeRotateDevelopWindow(std::uint16_t predicateIdx);

    void PumpDevelopMenuWork();

    void NoteMenuTabHint(int typeId);

    void NoteMenuFillEnter();
    void NoteMenuFillLeave();
    struct MenuFillScope
    {
        MenuFillScope()  { NoteMenuFillEnter(); }
        ~MenuFillScope() { NoteMenuFillLeave(); }
    };

    bool TryGetFlowIndexForDevelopId(std::uint16_t developId, std::uint16_t& outFlowIndex);

    bool IsManagedFlowIndex(std::uint16_t flowIndex);

    void MaybeRefreshDynamicGates();

    bool GetDevelopNameLangId(std::int32_t developId,
                              bool& outHasHash,
                              std::uint32_t& outHash,
                              std::string& outStr);


    bool IsDevelopIdReservedByStock(std::uint32_t developId);
    bool IsFlowIndexReservedByStock(std::uint32_t flowIndex);

    bool Install_TppMotherBaseManagement_EquipDevelopHooks();
    bool Uninstall_TppMotherBaseManagement_EquipDevelopHooks();
}