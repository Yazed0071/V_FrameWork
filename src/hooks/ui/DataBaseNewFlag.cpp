#include "pch.h"
#include "DataBaseNewFlag.h"

#include <cstdint>
#include <vector>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "../equip/CustomBluePrint.h"
#include "../equip/DataBaseControllerImpl_AddDataBase.h"

namespace dataBaseNewFlag
{
    namespace
    {
        constexpr std::uint16_t kNoDataBaseId  = 0xFFFF;
        constexpr std::uint16_t kFlagByteCount = 0x1CB;
        constexpr std::size_t   kShadowBytes   = 0x10000;

        constexpr std::uintptr_t kFlagArrayOffset = 0x740;
        constexpr std::uintptr_t kRecordCtlOffset = 0x30;
        constexpr std::uintptr_t kRecordCtlToCtrl = 0x28;
        constexpr std::uintptr_t kCallbackCtrl    = 0x48;
        constexpr std::uintptr_t kCallbackCursor  = 0x32C;

        using RefreshNewIcon_t    = void(__fastcall*)(void* self, std::uint16_t id);
        using SetPutCursorFlag_t  = void(__fastcall*)(void* self);

        RefreshNewIcon_t   g_OrigRefreshNewIcon   = nullptr;
        SetPutCursorFlag_t g_OrigSetPutCursorFlag = nullptr;
        SetPutCursorFlag_t g_OrigOnStop           = nullptr;

        std::vector<std::uint8_t> g_Shadow;
        bool g_SwapFailureLogged = false;

        std::uint8_t** FlagArraySlot(void* controller)
        {
            if (!controller)
                return nullptr;
            return reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(controller) + kFlagArrayOffset);
        }

        void* ControllerFromRecord(void* self)
        {
            if (!self)
                return nullptr;
            auto* ctl = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(self) + kRecordCtlOffset);
            if (!ctl)
                return nullptr;
            return *reinterpret_cast<void**>(ctl + kRecordCtlToCtrl);
        }

        void* ControllerFromCallback(void* self)
        {
            if (!self)
                return nullptr;
            return *reinterpret_cast<void**>(
                reinterpret_cast<std::uint8_t*>(self) + kCallbackCtrl);
        }

        class ShadowSwap
        {
        public:
            explicit ShadowSwap(void* controller)
                : m_slot(FlagArraySlot(controller))
            {
                if (!m_slot || !*m_slot)
                {
                    m_slot = nullptr;
                    return;
                }
                m_saved = *m_slot;
                if (g_Shadow.size() < kShadowBytes)
                    g_Shadow.assign(kShadowBytes, 0);
                *m_slot = g_Shadow.data();
            }

            ~ShadowSwap()
            {
                if (m_slot)
                    *m_slot = m_saved;
            }

            ShadowSwap(const ShadowSwap&) = delete;
            ShadowSwap& operator=(const ShadowSwap&) = delete;

            bool ok() const { return m_slot != nullptr; }

        private:
            std::uint8_t** m_slot  = nullptr;
            std::uint8_t*  m_saved = nullptr;
        };

        void NoteSwapFailure(const char* site)
        {
            if (g_SwapFailureLogged)
                return;
            g_SwapFailureLogged = true;
            Log("[DataBaseNew] %s could not reach the DataBase flag array, so a custom entry "
                "falls through to the vanilla path and reads past the %u-entry array into "
                "unrelated save data\n", site, static_cast<unsigned>(kFlagByteCount));
        }

        void __fastcall hkRefreshNewIcon(void* self, std::uint16_t id)
        {
            if (id == kNoDataBaseId || id < kFlagByteCount)
            {
                g_OrigRefreshNewIcon(self, id);
                return;
            }

            const std::int32_t slot =
                bluePrint::SlotFromPublicId(static_cast<std::int32_t>(id));
            const bool isNew = (slot > 0) && bluePrint::GetNew(slot);

            void* controller = ControllerFromRecord(self);
            ShadowSwap swap(controller);
            if (!swap.ok())
            {
                NoteSwapFailure("the NEW badge");
                g_OrigRefreshNewIcon(self, kNoDataBaseId);
                return;
            }

            g_Shadow[id] = isNew ? 1u : 0u;
            g_OrigRefreshNewIcon(self, id);
        }

        void RunClearOnShadow(void* self, SetPutCursorFlag_t orig, const char* site)
        {
            const std::uint16_t id = *reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uint8_t*>(self) + kCallbackCursor);

            if (id == kNoDataBaseId || id < kFlagByteCount)
            {
                orig(self);
                return;
            }

            const std::int32_t slot =
                bluePrint::SlotFromPublicId(static_cast<std::int32_t>(id));

            void* controller = ControllerFromCallback(self);
            ShadowSwap swap(controller);
            if (!swap.ok())
            {
                NoteSwapFailure(site);
                return;
            }

            g_Shadow[id] = (slot > 0 && bluePrint::GetNew(slot)) ? 1u : 0u;
            orig(self);

            if (slot > 0 && g_Shadow[id] == 0)
                bluePrint::SetNew(slot, false);
        }

        using CalcUnread_t = unsigned int(__fastcall*)(void* self);

        CalcUnread_t g_OrigCalcDocumentation = nullptr;
        CalcUnread_t g_OrigCalcEncyclopedia  = nullptr;

        constexpr std::uint8_t kDocumentationTabs[] = { 1, 2, 7, 8 };
        constexpr std::uint8_t kEncyclopediaTabs[]  = { 0, 4, 5 };

        unsigned int g_CustomUnread = 0;

        void CountIfUnread(std::int32_t publicId)
        {
            const std::int32_t slot = bluePrint::SlotFromPublicId(publicId);
            if (slot > 0 && bluePrint::Has(slot) && bluePrint::GetNew(slot))
                ++g_CustomUnread;
        }

        unsigned int CustomUnreadForTabs(const std::uint8_t* tabs, std::size_t count)
        {
            g_CustomUnread = 0;
            for (std::size_t i = 0; i < count; ++i)
                bluePrintDb::ForEachDocumentedId(tabs[i], &CountIfUnread);
            return g_CustomUnread;
        }

        unsigned int __fastcall hkCalcDocumentationUnreadCount(void* self)
        {
            return g_OrigCalcDocumentation(self)
                 + CustomUnreadForTabs(kDocumentationTabs,
                                       sizeof(kDocumentationTabs));
        }

        unsigned int __fastcall hkCalcEncyclopediaUnreadCount(void* self)
        {
            return g_OrigCalcEncyclopedia(self)
                 + CustomUnreadForTabs(kEncyclopediaTabs,
                                       sizeof(kEncyclopediaTabs));
        }

        void __fastcall hkSetPutCursorFlag(void* self)
        {
            RunClearOnShadow(self, g_OrigSetPutCursorFlag, "clearing the NEW badge");
        }

        void __fastcall hkOnStop(void* self)
        {
            RunClearOnShadow(self, g_OrigOnStop, "clearing the NEW badge on close");
        }
    }

    bool Install(HMODULE)
    {
        if (!gAddr.UiMbmDataBase_RefreshNewIcon
            || !gAddr.UiMbmDataBase_SetPutCursorFlag
            || !gAddr.UiMbmDataBase_OnStop)
        {
            LogDebug("[DataBaseNew] not ported for this build - a custom DATABASE row keeps "
                     "the vanilla badge path, which reads past the flag array\n");
            return true;
        }

        g_Shadow.assign(kShadowBytes, 0);

        bool ok = CreateAndEnableHook(
            ResolveGameAddress(gAddr.UiMbmDataBase_RefreshNewIcon),
            &hkRefreshNewIcon,
            reinterpret_cast<void**>(&g_OrigRefreshNewIcon));

        ok = CreateAndEnableHook(
                 ResolveGameAddress(gAddr.UiMbmDataBase_SetPutCursorFlag),
                 &hkSetPutCursorFlag,
                 reinterpret_cast<void**>(&g_OrigSetPutCursorFlag)) && ok;

        ok = CreateAndEnableHook(
                 ResolveGameAddress(gAddr.UiMbmDataBase_OnStop),
                 &hkOnStop,
                 reinterpret_cast<void**>(&g_OrigOnStop)) && ok;

        if (!ok)
        {
            Log("[DataBaseNew] the DATABASE badge hooks did not all install - a custom entry's "
                "NEW badge is read from, and cleared into, save data past the %u-entry flag "
                "array\n", static_cast<unsigned>(kFlagByteCount));
        }

        if (gAddr.MbmImpl_CalcDocumentationUnreadCount
            && gAddr.MbmImpl_CalcEncyclopediaUnreadCount)
        {
            const bool docOk = CreateAndEnableHook(
                ResolveGameAddress(gAddr.MbmImpl_CalcDocumentationUnreadCount),
                &hkCalcDocumentationUnreadCount,
                reinterpret_cast<void**>(&g_OrigCalcDocumentation));

            const bool encOk = CreateAndEnableHook(
                ResolveGameAddress(gAddr.MbmImpl_CalcEncyclopediaUnreadCount),
                &hkCalcEncyclopediaUnreadCount,
                reinterpret_cast<void**>(&g_OrigCalcEncyclopedia));

            if (!docOk || !encOk)
            {
                Log("[DataBaseNew] the unread counters were not hooked, so the DOCUMENTATION "
                    "and ENCYCLOPEDIA menu badges count vanilla entries only - a custom entry "
                    "shows NEW on its row but never on the menu above it\n");
            }
        }
        return ok;
    }
}
