#include "pch.h"
#include "DataBaseCallbackImpl_RefreshPrefabList.h"

#include <Windows.h>
#include <cstdint>
#include <vector>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "V_FrameWorkState.h"
#include "../equip/CustomBluePrint.h"
#include "../equip/DataBaseControllerImpl_AddDataBase.h"

namespace
{
    constexpr std::uint8_t kTabDefault    = 2;
    constexpr std::size_t  kRowsOffset    = 0x80;
    constexpr std::size_t  kCountOffset   = 0x284;
    constexpr std::size_t  kRowsCapacity  = 108;

    constexpr std::size_t kRec_ImplOffset = 0x30;
    constexpr std::size_t kRec_NodeOffset = 0x60;
    constexpr std::size_t kRec_IconNodeA  = 0x50;
    constexpr std::size_t kRec_IconNodeB  = 0x58;
    constexpr std::size_t kRec_InfoUi     = 0x28;
    constexpr std::size_t kImpl_UiOffset  = 0x08;
    constexpr std::size_t kImpl_ArgOffset = 0x38;
    constexpr std::size_t kUiVtbl_SetText = 0x708;
    constexpr std::size_t kUiVtbl_SetIcon = 0x518;
    constexpr std::size_t kUiVtbl_LangStr = 0x750;
    constexpr std::size_t kUiVtbl_InfoTxt = 0x738;

    constexpr std::size_t kInfo_Ui       = 0x28;
    constexpr std::size_t kInfo_Arg      = 0x60;
    constexpr std::size_t kInfo_TextNode = 0x6c8;
    constexpr std::size_t kInfo_Cursor   = 0x290;
    constexpr std::size_t kInfo_Scroll   = 0x28c;
    constexpr int         kInfoTextMode  = 8;

    constexpr std::size_t kImageRec_IconA = 0x28;
    constexpr std::size_t kImageRec_IconB = 0x30;

    constexpr std::uint64_t kIconSlotHash    = 0xcafb3bbf9889ull;
    constexpr std::uint64_t kIconSlotHashAlt = 0xec3f8d982b8eull;

    using RefreshPrefabList_t  = void(__fastcall*)(void* self, std::uint8_t tab);
    using RefreshNameText_t    = void(__fastcall*)(void* self, std::uint16_t id, std::uint8_t cat);
    using RefreshIcon_t        = void(__fastcall*)(void* self, std::uint16_t id, std::uint8_t cat);
    using GetInfoText_t        = const char*(__fastcall*)(void* self, std::uint16_t id);
    using RefreshInfoLayout_t  = void(__fastcall*)(void* self);
    using CollectionRate_t     = void(__fastcall*)(void* self, bool show, std::uint32_t got,
                                                  std::uint32_t total);
    using RefreshImageRecord_t = void(__fastcall*)(void* self, void* rec, std::uint16_t row,
                                                  std::uint16_t mode);
    using SetLangText_t        = void(__fastcall*)(void* ui, void* node, void* arg,
                                                  const char* text);
    using SetInfoText_t        = void(__fastcall*)(void* ui, void* node, void* arg, int mode,
                                                  const char* text, bool wrap, bool fit);
    using SetIconTex_t         = void(__fastcall*)(void* ui, void* node,
                                                  std::uint64_t iconHash, std::uint64_t slotHash);
    using LangToString_t       = const char*(__fastcall*)(void* ui, std::uint64_t langHash);

    RefreshPrefabList_t g_Orig = nullptr;
    RefreshNameText_t g_OrigName = nullptr;
    RefreshIcon_t g_OrigIcon = nullptr;
    GetInfoText_t g_OrigInfo = nullptr;
    RefreshInfoLayout_t g_OrigInfoLayout = nullptr;
    RefreshImageRecord_t g_OrigImageRecord = nullptr;
    CollectionRate_t g_OrigCollectionRate = nullptr;

    bool g_Installed = false;
    bool g_NameInstalled = false;
    bool g_IconInstalled = false;
    bool g_InfoInstalled = false;
    bool g_InfoLayoutInstalled = false;
    bool g_ImageRecordInstalled = false;
    bool g_CollectionRateInstalled = false;

    std::uint32_t g_TabCustomRows = 0;
    std::uint32_t g_TabCustomOwned = 0;

    bool g_CapacityLogged = false;
    bool g_NameSetFailedLogged = false;
    bool g_NameMissingLogged = false;
    bool g_IconSetFailedLogged = false;
    bool g_InfoLayoutFailedLogged = false;
    bool g_ImageIconFailedLogged = false;

    std::vector<std::uint16_t>* g_CollectTarget = nullptr;
    std::uint8_t g_CollectTab = kTabDefault;

    std::uint8_t TabForBluePrint(std::int32_t publicId)
    {
        bluePrintDb::Documentation doc;
        return bluePrintDb::TryGetDocumentation(publicId, doc) ? doc.tab : kTabDefault;
    }

    void CollectBluePrint(const char* key, std::int32_t slot, bool owned)
    {
        UNREFERENCED_PARAMETER(key);
        UNREFERENCED_PARAMETER(owned);

        if (!g_CollectTarget || slot <= 0)
            return;

        const std::int32_t publicId = bluePrint::PublicId(slot);
        if (publicId <= 0 || publicId > 0xFFFE)
            return;

        if (TabForBluePrint(publicId) != g_CollectTab)
            return;

        g_CollectTarget->push_back(static_cast<std::uint16_t>(publicId));
    }

    std::size_t AppendRowsSeh(void* self, const std::vector<std::uint16_t>& ids)
    {
        __try
        {
            auto* rows  = reinterpret_cast<std::uint16_t*>(
                static_cast<std::uint8_t*>(self) + kRowsOffset);
            auto* count = reinterpret_cast<std::uint32_t*>(
                static_cast<std::uint8_t*>(self) + kCountOffset);

            std::uint32_t used = *count;
            if (used >= kRowsCapacity)
                return 0;

            std::size_t added = 0;
            for (const std::uint16_t id : ids)
            {
                if (used >= kRowsCapacity)
                    break;
                rows[used++] = id;
                ++added;
            }

            *count = used;
            return added;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool SetRowNameSeh(void* self, std::uint64_t langHash)
    {
        __try
        {
            auto* base = static_cast<std::uint8_t*>(self);
            void* impl = *reinterpret_cast<void**>(base + kRec_ImplOffset);
            void* node = *reinterpret_cast<void**>(base + kRec_NodeOffset);
            if (!impl)
                return false;

            auto* implBytes = static_cast<std::uint8_t*>(impl);
            void* ui  = *reinterpret_cast<void**>(implBytes + kImpl_UiOffset);
            void* arg = *reinterpret_cast<void**>(implBytes + kImpl_ArgOffset);
            if (!ui)
                return false;

            void** vtbl = *reinterpret_cast<void***>(ui);
            if (!vtbl)
                return false;

            auto setText = reinterpret_cast<SetLangText_t>(
                vtbl[kUiVtbl_SetText / sizeof(void*)]);
            auto toString = reinterpret_cast<LangToString_t>(
                vtbl[kUiVtbl_LangStr / sizeof(void*)]);
            if (!setText || !toString)
                return false;

            const char* text = toString(ui, langHash);
            if (!text)
                return false;

            setText(ui, node, arg, text);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    using GetUixUtility_t = void**(*)();
    GetUixUtility_t g_GetUixUtility = nullptr;

    void PrefetchIconTexture(std::uint64_t textureHash)
    {
        if (textureHash == 0 || gAddr.GetUixUtilityToFeedQuarkEnvironment == 0)
            return;
        if (!g_GetUixUtility)
            g_GetUixUtility = reinterpret_cast<GetUixUtility_t>(
                ResolveGameAddress(gAddr.GetUixUtilityToFeedQuarkEnvironment));
        if (!g_GetUixUtility)
            return;

        __try
        {
            void** util = g_GetUixUtility();
            if (!util)
                return;
            void** vtbl = *reinterpret_cast<void***>(util);
            if (!vtbl)
                return;
            auto fn = reinterpret_cast<void(__fastcall*)(void*, std::uint64_t, int)>(
                vtbl[0x548 / sizeof(void*)]);
            if (fn)
                fn(util, textureHash, 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool SetRowIconSeh(void* self, std::uint64_t iconHash)
    {
        PrefetchIconTexture(iconHash);

        __try
        {
            auto* base = static_cast<std::uint8_t*>(self);
            void* impl = *reinterpret_cast<void**>(base + kRec_ImplOffset);
            if (!impl)
                return false;

            void* ui = *reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(impl) + kImpl_UiOffset);
            if (!ui)
                return false;

            void** vtbl = *reinterpret_cast<void***>(ui);
            if (!vtbl)
                return false;

            auto setIcon = reinterpret_cast<SetIconTex_t>(
                vtbl[kUiVtbl_SetIcon / sizeof(void*)]);
            if (!setIcon)
                return false;

            void* nodeA = *reinterpret_cast<void**>(base + kRec_IconNodeA);
            void* nodeB = *reinterpret_cast<void**>(base + kRec_IconNodeB);

            setIcon(ui, nodeA, iconHash, kIconSlotHash);
            setIcon(ui, nodeB, iconHash, kIconSlotHash);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* ResolveLangTextSeh(void* self, std::uint64_t langHash)
    {
        __try
        {
            void* ui = *reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(self) + kRec_InfoUi);
            if (!ui)
                return nullptr;

            void** vtbl = *reinterpret_cast<void***>(ui);
            if (!vtbl)
                return nullptr;

            auto toString = reinterpret_cast<LangToString_t>(
                vtbl[kUiVtbl_LangStr / sizeof(void*)]);
            if (!toString)
                return nullptr;

            return toString(ui, langHash);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool CustomDocumentation(std::uint16_t id, bluePrintDb::Documentation& doc)
    {
        const std::int32_t publicId = static_cast<std::int32_t>(id);
        const std::int32_t slot = bluePrint::SlotFromPublicId(publicId);
        if (slot <= 0 || !bluePrint::Has(slot))
            return false;
        return bluePrintDb::TryGetDocumentation(publicId, doc);
    }

    void __fastcall hkRefreshDataBaseNameText(void* self, std::uint16_t id, std::uint8_t cat)
    {
        bluePrintDb::Documentation doc;

        if (self && CustomDocumentation(id, doc))
        {
            if (doc.nameHash == 0)
            {
                if (!g_NameMissingLogged)
                {
                    g_NameMissingLogged = true;
                    Log("[BluePrint] no langDocName was registered for blueprint dataBaseId %u, so "
                        "its DATABASE row stays '???' even once owned - call "
                        "V_TppMotherBaseManagement.SetDataBaseDisplay{ dataBaseId = %u, "
                        "langDocName = \"<langId>\" }\n",
                        static_cast<unsigned>(id), static_cast<unsigned>(id));
                }
            }
            else if (SetRowNameSeh(self, doc.nameHash))
            {
                return;
            }
            else if (!g_NameSetFailedLogged)
            {
                g_NameSetFailedLogged = true;
                Log("[BluePrint] the DATABASE name node was unreachable for dataBaseId %u, so its "
                    "row stays '???' - the row-record layout differs on this build\n",
                    static_cast<unsigned>(id));
            }
        }

        if (g_OrigName)
            g_OrigName(self, id, cat);
    }

    void __fastcall hkRefreshDataBaseIcon(void* self, std::uint16_t id, std::uint8_t cat)
    {
        bluePrintDb::Documentation doc;
        const bool custom = CustomDocumentation(id, doc);

        if (self && custom && doc.iconHash != 0)
        {
            if (!SetRowIconSeh(self, doc.iconHash) && !g_IconSetFailedLogged)
            {
                g_IconSetFailedLogged = true;
                Log("[BluePrint] the DATABASE icon node was unreachable for dataBaseId %u, so its "
                    "row keeps the '?' icon - the row-record layout differs on this build\n",
                    static_cast<unsigned>(id));
            }
            return;
        }

        if (g_OrigIcon)
            g_OrigIcon(self, id, cat);
    }

    const char* __fastcall hkGetDataBaseInfoText(void* self, std::uint16_t id)
    {
        bluePrintDb::Documentation doc;

        if (self && CustomDocumentation(id, doc) && doc.infoHash != 0)
        {
            if (const char* text = ResolveLangTextSeh(self, doc.infoHash))
                return text;
        }

        return g_OrigInfo ? g_OrigInfo(self, id) : nullptr;
    }

    std::uint16_t RowIdSeh(void* self, std::uint32_t row)
    {
        __try
        {
            if (row >= kRowsCapacity)
                return 0xFFFF;
            return *reinterpret_cast<std::uint16_t*>(
                static_cast<std::uint8_t*>(self) + kRowsOffset + row * sizeof(std::uint16_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0xFFFF;
        }
    }

    std::uint16_t SelectedRowIdSeh(void* self)
    {
        std::uint32_t row = 0;

        __try
        {
            auto* base = static_cast<std::uint8_t*>(self);
            const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(base + kCountOffset);
            if (count == 0)
                return 0xFFFF;

            const std::uint32_t cursor = *reinterpret_cast<std::uint32_t*>(base + kInfo_Cursor);
            const std::uint32_t scroll = *reinterpret_cast<std::uint32_t*>(base + kInfo_Scroll);
            row = (cursor + scroll) % count;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0xFFFF;
        }

        return RowIdSeh(self, row);
    }

    bool SetInfoTextSeh(void* self, std::uint64_t langHash)
    {
        __try
        {
            auto* base = static_cast<std::uint8_t*>(self);
            void* ui = *reinterpret_cast<void**>(base + kInfo_Ui);
            if (!ui)
                return false;

            void** vtbl = *reinterpret_cast<void***>(ui);
            if (!vtbl)
                return false;

            auto toString = reinterpret_cast<LangToString_t>(
                vtbl[kUiVtbl_LangStr / sizeof(void*)]);
            auto setInfo = reinterpret_cast<SetInfoText_t>(
                vtbl[kUiVtbl_InfoTxt / sizeof(void*)]);
            if (!toString || !setInfo)
                return false;

            const char* text = toString(ui, langHash);
            if (!text)
                return false;

            setInfo(ui, *reinterpret_cast<void**>(base + kInfo_TextNode),
                    *reinterpret_cast<void**>(base + kInfo_Arg), kInfoTextMode, text, true, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SetImageIconSeh(void* self, void* rec, std::uint64_t iconHash)
    {
        PrefetchIconTexture(iconHash);

        __try
        {
            void* ui = *reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(self) + kInfo_Ui);
            if (!ui)
                return false;

            void** vtbl = *reinterpret_cast<void***>(ui);
            if (!vtbl)
                return false;

            auto setIcon = reinterpret_cast<SetIconTex_t>(
                vtbl[kUiVtbl_SetIcon / sizeof(void*)]);
            if (!setIcon)
                return false;

            auto* recBytes = static_cast<std::uint8_t*>(rec);
            void* nodeA = *reinterpret_cast<void**>(recBytes + kImageRec_IconA);
            void* nodeB = *reinterpret_cast<void**>(recBytes + kImageRec_IconB);

            setIcon(ui, nodeA, iconHash, kIconSlotHash);
            setIcon(ui, nodeA, iconHash, kIconSlotHashAlt);
            setIcon(ui, nodeB, iconHash, kIconSlotHash);
            setIcon(ui, nodeB, iconHash, kIconSlotHashAlt);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void __fastcall hkRefreshInfoLayout(void* self)
    {
        if (g_OrigInfoLayout)
            g_OrigInfoLayout(self);

        if (!self)
            return;

        bluePrintDb::Documentation doc;
        if (!CustomDocumentation(SelectedRowIdSeh(self), doc) || doc.infoHash == 0)
            return;

        if (!SetInfoTextSeh(self, doc.infoHash) && !g_InfoLayoutFailedLogged)
        {
            g_InfoLayoutFailedLogged = true;
            Log("[BluePrint] the DATABASE description node was unreachable, so the detail panel "
                "keeps '???' for custom blueprints - the screen layout differs on this build\n");
        }
    }

    void __fastcall hkRefreshImageRecord(void* self, void* rec, std::uint16_t row,
                                         std::uint16_t mode)
    {
        if (g_OrigImageRecord)
            g_OrigImageRecord(self, rec, row, mode);

        if (!self || !rec)
            return;

        bluePrintDb::Documentation doc;
        if (!CustomDocumentation(RowIdSeh(self, row), doc))
            return;

        const std::uint64_t imageHash = doc.imageHash ? doc.imageHash : doc.iconHash;
        if (imageHash == 0)
            return;

        if (!SetImageIconSeh(self, rec, imageHash) && !g_ImageIconFailedLogged)
        {
            g_ImageIconFailedLogged = true;
            Log("[BluePrint] the DATABASE detail image node was unreachable, so the panel keeps "
                "the '?' icon for custom blueprints - the record layout differs on this build\n");
        }
    }

    void __fastcall hkRefreshCollectionRate(void* self, bool show, std::uint32_t got,
                                            std::uint32_t total)
    {
        if (g_OrigCollectionRate)
            g_OrigCollectionRate(self, show, got + g_TabCustomOwned, total + g_TabCustomRows);
    }

    void __fastcall hkRefreshPrefabList(void* self, std::uint8_t tab)
    {
        if (g_Orig)
            g_Orig(self, tab);

        if (!self)
            return;

        g_TabCustomRows = 0;
        g_TabCustomOwned = 0;

        std::vector<std::uint16_t> ids;
        g_CollectTarget = &ids;
        g_CollectTab = tab;
        V_FrameWorkState::ForEachBluePrint(&CollectBluePrint);
        g_CollectTarget = nullptr;

        if (ids.empty())
            return;

        const std::size_t added = AppendRowsSeh(self, ids);

        g_TabCustomRows = static_cast<std::uint32_t>(added);
        for (std::size_t i = 0; i < added; ++i)
        {
            const std::int32_t slot = bluePrint::SlotFromPublicId(
                static_cast<std::int32_t>(ids[i]));
            if (slot > 0 && bluePrint::Has(slot))
                ++g_TabCustomOwned;
        }

        if (added < ids.size() && !g_CapacityLogged)
        {
            g_CapacityLogged = true;
            Log("[BluePrint] only %zu of %zu custom blueprint row(s) fit DATABASE tab %u "
                "(it holds %zu rows total), so the rest are missing from that tab\n",
                added, ids.size(), static_cast<unsigned>(tab), kRowsCapacity);
        }
    }

    bool InstallOne(std::uintptr_t address, void* detour, void** orig, const char* what)
    {
        if (!address)
            return false;

        void* target = ResolveGameAddress(address);
        if (CreateAndEnableHook(target, detour, orig))
            return true;

        *orig = nullptr;
        Log("[BluePrint] ERROR: the DATABASE %s hook was refused at %p - custom blueprint rows "
            "keep the vanilla default there\n", what, target);
        return false;
    }

    void RemoveOne(std::uintptr_t address)
    {
        void* target = ResolveGameAddress(address);
        if (!target)
            return;
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }
}

bool Install_DataBaseBluePrintList_Hook()
{
    if (g_Installed)
        return true;

    if (!gAddr.UiMbmDataBase_RefreshPrefabList)
        return true;

    void* target = ResolveGameAddress(gAddr.UiMbmDataBase_RefreshPrefabList);
    if (!CreateAndEnableHook(target, &hkRefreshPrefabList,
                             reinterpret_cast<void**>(&g_Orig)))
    {
        g_Orig = nullptr;
        Log("[BluePrint] ERROR: the DATABASE blueprint-list hook was refused at %p - custom "
            "blueprints will not be listed under iDroid DATABASE\n", target);
        return false;
    }

    g_Installed = true;

    g_NameInstalled = InstallOne(gAddr.UiMbmDataBase_RefreshDataBaseNameText,
                                 &hkRefreshDataBaseNameText,
                                 reinterpret_cast<void**>(&g_OrigName), "name");

    g_IconInstalled = InstallOne(gAddr.UiMbmDataBase_RefreshDataBaseIcon,
                                 &hkRefreshDataBaseIcon,
                                 reinterpret_cast<void**>(&g_OrigIcon), "icon");

    g_InfoInstalled = InstallOne(gAddr.UiMbmDataBase_GetDataBaseInfoText,
                                 &hkGetDataBaseInfoText,
                                 reinterpret_cast<void**>(&g_OrigInfo), "description");

    g_InfoLayoutInstalled = InstallOne(gAddr.UiMbmDataBase_RefreshInfoLayout,
                                       &hkRefreshInfoLayout,
                                       reinterpret_cast<void**>(&g_OrigInfoLayout),
                                       "detail description");

    g_ImageRecordInstalled = InstallOne(gAddr.UiMbmDataBase_RefreshImageRecord,
                                        &hkRefreshImageRecord,
                                        reinterpret_cast<void**>(&g_OrigImageRecord),
                                        "detail image");

    g_CollectionRateInstalled = InstallOne(gAddr.UiMbmDataBase_RefreshCollectionRate,
                                          &hkRefreshCollectionRate,
                                          reinterpret_cast<void**>(&g_OrigCollectionRate),
                                          "collection rate");

    return true;
}

void Uninstall_DataBaseBluePrintList_Hook()
{
    if (!g_Installed)
        return;

    RemoveOne(gAddr.UiMbmDataBase_RefreshPrefabList);

    if (g_NameInstalled)
    {
        RemoveOne(gAddr.UiMbmDataBase_RefreshDataBaseNameText);
        g_OrigName = nullptr;
        g_NameInstalled = false;
    }

    if (g_IconInstalled)
    {
        RemoveOne(gAddr.UiMbmDataBase_RefreshDataBaseIcon);
        g_OrigIcon = nullptr;
        g_IconInstalled = false;
    }

    if (g_InfoInstalled)
    {
        RemoveOne(gAddr.UiMbmDataBase_GetDataBaseInfoText);
        g_OrigInfo = nullptr;
        g_InfoInstalled = false;
    }

    if (g_InfoLayoutInstalled)
    {
        RemoveOne(gAddr.UiMbmDataBase_RefreshInfoLayout);
        g_OrigInfoLayout = nullptr;
        g_InfoLayoutInstalled = false;
    }

    if (g_ImageRecordInstalled)
    {
        RemoveOne(gAddr.UiMbmDataBase_RefreshImageRecord);
        g_OrigImageRecord = nullptr;
        g_ImageRecordInstalled = false;
    }

    if (g_CollectionRateInstalled)
    {
        RemoveOne(gAddr.UiMbmDataBase_RefreshCollectionRate);
        g_OrigCollectionRate = nullptr;
        g_CollectionRateInstalled = false;
    }

    g_Orig = nullptr;
    g_Installed = false;
}
