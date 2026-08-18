#include "pch.h"
#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include "MinHook.h"
#include "log.h"
#include "UiUtility_GetWeaponItemNameLangId.h"
#include "AddressSet.h"
#include "HookUtils.h"

namespace
{
    constexpr std::uint64_t kStringIdMask = 0x0000FFFFFFFFFFFFull;

    struct EquipLangEntry
    {
        std::uint64_t name = 0;
        std::uint64_t info = 0;
        std::uint64_t realName = 0;
        bool hasName = false;
        bool hasInfo = false;
        bool hasRealName = false;
    };

    static std::unordered_map<int, EquipLangEntry> g_EquipLang;
    static std::mutex g_EquipLangMutex;

    using GetWeaponItemNameLangId_t =
        void* (__fastcall*)(void* outStringId, int equipId, bool variant);

    static GetWeaponItemNameLangId_t g_OrigGetWeaponItemNameLangId = nullptr;
    static bool g_Installed = false;

    static bool g_VariantSeen[2] = { false, false };

    static void* __fastcall hkGetWeaponItemNameLangId(void* outStringId,
                                                     int equipId,
                                                     bool variant)
    {
        void* result = g_OrigGetWeaponItemNameLangId(outStringId, equipId, variant);
        if (!outStringId)
            return result;

        std::uint64_t override = 0;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_EquipLangMutex);
            auto it = g_EquipLang.find(equipId);
            if (it != g_EquipLang.end())
            {
                const EquipLangEntry& e = it->second;
                if (variant)
                {
                    if (e.hasRealName) { override = e.realName; have = true; }
                    else if (e.hasName) { override = e.name; have = true; }
                }
                else if (e.hasName)
                {
                    override = e.name; have = true;
                }

                if (!g_VariantSeen[variant ? 1 : 0])
                {
                    g_VariantSeen[variant ? 1 : 0] = true;
                    LogDebug("[EquipLangInfo] variant=%d observed for custom equipId=%d "
                        "(engine returned StringId 0x%012llX). The bool selects which "
                        "lang slot the UI is asking for; if the name and the real name "
                        "come out swapped, exchange langEquipName and langEquipRealName "
                        "in SetEquipLangInfo - the mapping is inferred, not proven\n",
                        variant ? 1 : 0, equipId,
                        static_cast<unsigned long long>(
                            *reinterpret_cast<std::uint64_t*>(outStringId) & kStringIdMask));
                }
            }
        }

        if (have)
            *reinterpret_cast<std::uint64_t*>(outStringId) = override & kStringIdMask;

        return result;
    }
}

void EquipLangInfo_Set(int equipId,
                       bool hasName,     std::uint64_t nameId,
                       bool hasInfo,     std::uint64_t infoId,
                       bool hasRealName, std::uint64_t realNameId)
{
    std::lock_guard<std::mutex> lock(g_EquipLangMutex);
    EquipLangEntry& e = g_EquipLang[equipId];
    if (hasName)     { e.name = nameId & kStringIdMask;         e.hasName = true; }
    if (hasInfo)     { e.info = infoId & kStringIdMask;         e.hasInfo = true; }
    if (hasRealName) { e.realName = realNameId & kStringIdMask; e.hasRealName = true; }
}

void EquipLangInfo_Clear()
{
    std::lock_guard<std::mutex> lock(g_EquipLangMutex);
    g_EquipLang.clear();
}

bool Install_UiUtility_GetWeaponItemNameLangIdHook()
{
    if (g_Installed)
        return true;

    const uintptr_t addr = gAddr.UiUtility_GetWeaponItemNameLangId;
    if (!addr)
        return false;

    void* target = reinterpret_cast<void*>(addr);
    if (MH_CreateHook(
            target,
            reinterpret_cast<void*>(&hkGetWeaponItemNameLangId),
            reinterpret_cast<void**>(&g_OrigGetWeaponItemNameLangId)) != MH_OK)
    {
        Log("[EquipLangInfo] GetWeaponItemNameLangId hook FAIL create at 0x%llX - custom "
            "equip names and descriptions stay blank in the pickup prompt and the "
            "announce feed\n", static_cast<unsigned long long>(addr));
        return false;
    }

    if (EnableOrQueueHook(target) != MH_OK)
    {
        MH_RemoveHook(target);
        g_OrigGetWeaponItemNameLangId = nullptr;
        Log("[EquipLangInfo] GetWeaponItemNameLangId hook FAIL enable at 0x%llX - custom "
            "equip names and descriptions stay blank in the pickup prompt and the "
            "announce feed\n", static_cast<unsigned long long>(addr));
        return false;
    }

    g_Installed = true;
    return true;
}

void Uninstall_UiUtility_GetWeaponItemNameLangIdHook()
{
    if (!g_Installed)
        return;
    void* target = reinterpret_cast<void*>(gAddr.UiUtility_GetWeaponItemNameLangId);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    g_OrigGetWeaponItemNameLangId = nullptr;
    g_Installed = false;
}
