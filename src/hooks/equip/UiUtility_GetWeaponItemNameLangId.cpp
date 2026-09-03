#include "pch.h"
#include <Windows.h>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include "MinHook.h"
#include "log.h"
#include "UiUtility_GetWeaponItemNameLangId.h"
#include "../outfit/OutfitRegistry.h"
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
    static std::uint64_t g_UniqueProbeKeys[48] = {};
    static int           g_UniqueProbeCount  = 0;
    static int           g_UniqueProbeLogged = 0;

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
                    if (e.hasRealName)  { override = e.realName; have = true; }
                    else if (e.hasInfo) { override = e.info;     have = true; }
                }
                else if (e.hasName)
                {
                    override = e.name; have = true;
                }
            }
        }

        {
            const std::uint8_t livePT = outfit::ReadLivePlayerType();
            if (outfit::IsUniqueCharacterPlayerType(livePT))
            {
                std::lock_guard<std::mutex> lock(g_EquipLangMutex);
                if (g_UniqueProbeLogged < 48)
                {
                    const std::uint64_t key =
                        (static_cast<std::uint64_t>(livePT) << 40)
                      ^ (static_cast<std::uint64_t>(variant ? 1u : 0u) << 32)
                      ^ static_cast<std::uint32_t>(equipId);
                    bool seen = false;
                    for (int k = 0; k < g_UniqueProbeCount; ++k)
                        if (g_UniqueProbeKeys[k] == key) { seen = true; break; }
                    if (!seen)
                    {
                        if (g_UniqueProbeCount < 48)
                            g_UniqueProbeKeys[g_UniqueProbeCount++] = key;
                        ++g_UniqueProbeLogged;
                        LogDebug("[EquipLangInfo] unique-character name query: "
                            "playerType=%u equipId=%d variant=%d (0=short name, "
                            "1=proper/full name) -> engine StringId 0x%012llX - this "
                            "is the weapon pickup and HUD path, NOT the UNIFORMS row; "
                            "that row reads the develop record through the equip "
                            "develop controller instead\n",
                            static_cast<unsigned>(livePT), equipId,
                            variant ? 1 : 0,
                            static_cast<unsigned long long>(
                                *reinterpret_cast<std::uint64_t*>(outStringId)
                                & kStringIdMask));
                    }
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

    void* target = ResolveGameAddress(addr);
    if (MH_CreateHook(
            target,
            reinterpret_cast<void*>(&hkGetWeaponItemNameLangId),
            reinterpret_cast<void**>(&g_OrigGetWeaponItemNameLangId)) != MH_OK)
    {
        Log("[EquipLangInfo] GetWeaponItemNameLangId hook FAIL create at 0x%llX - "
            "custom equip names and descriptions stay blank\n", static_cast<unsigned long long>(addr));
        return false;
    }

    if (EnableOrQueueHook(target) != MH_OK)
    {
        MH_RemoveHook(target);
        g_OrigGetWeaponItemNameLangId = nullptr;
        Log("[EquipLangInfo] GetWeaponItemNameLangId hook FAIL enable at 0x%llX - "
            "custom equip names and descriptions stay blank\n", static_cast<unsigned long long>(addr));
        return false;
    }

    g_Installed = true;
    return true;
}

void Uninstall_UiUtility_GetWeaponItemNameLangIdHook()
{
    if (!g_Installed)
        return;
    void* target = ResolveGameAddress(gAddr.UiUtility_GetWeaponItemNameLangId);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    g_OrigGetWeaponItemNameLangId = nullptr;
    g_Installed = false;
}
