#include "pch.h"

#include "EquipDevelopControllerImpl_GetItemNameMessageId.h"

#include "OutfitRegistry.h"
#include "UniqueCharacterOwnSuit.h"
#include "UniqueCharacterDefaultOutfit.h"
#include "../equip/EquipDevelop_SetEquipUndeveloped.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "HookUtils.h"
#include "log.h"

namespace
{
    using GetLangId_t    = std::uint64_t* (__fastcall*)(void*, std::uint64_t*,
                                                        std::uint16_t);
    using DevelopIndex_t = std::uint16_t (__fastcall*)(void*, std::uint16_t);

    constexpr std::size_t  kVtblScanSlots = 0x400;
    constexpr std::size_t  kPrologueBytes = 0x50;

    constexpr std::uint8_t kDisp_ListRowName = 0x28;
    constexpr std::uint8_t kDisp_DetailsHead = 0x60;
    constexpr std::uint8_t kDisp_DetailsBody = 0x30;

    constexpr int kLabel_ListRowName = 0;
    constexpr int kLabel_DetailsHead = 1;
    constexpr int kLabel_DetailsBody = 2;
    constexpr int kLabelCount        = 3;

    struct LabelSlot
    {
        std::uint8_t disp;
        bool         isName;
        const char*  what;
        void*        target;
        GetLangId_t  orig;
    };

    LabelSlot g_Slots[kLabelCount] = {
        { kDisp_ListRowName, true,  "UNIFORMS row name",   nullptr, nullptr },
        { kDisp_DetailsHead, true,  "details heading",     nullptr, nullptr },
        { kDisp_DetailsBody, false, "details description", nullptr, nullptr },
    };

    std::atomic<std::uint32_t> g_ConverterVtblOffset{ 0 };
    std::atomic<int>           g_ScanAttempts{ 0 };
    std::atomic<bool>          g_GaveUpLogged{ false };
    bool g_Installed = false;

    constexpr int kMaxScanAttempts = 8;

    bool SafeCopyCode(const void* fn, std::uint8_t* dst, std::size_t n)
    {
        __try
        {
            std::memcpy(dst, fn, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool SafeReadVtblEntry(void** vtbl, std::size_t index, void** out)
    {
        __try
        {
            *out = vtbl[index];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool IsExecutableCode(const void* p)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xFF;
        return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ
            || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    }

    bool ClassifyRecordGetter(const void* fn, std::uint8_t* outDisp,
                              std::uint32_t* outConverterOffset)
    {
        std::uint8_t code[kPrologueBytes];
        if (!SafeCopyCode(fn, code, sizeof code)) return false;

        std::uint32_t converter = 0;
        for (std::size_t i = 0; i + 6 <= sizeof code; ++i)
            if (code[i] == 0xFF && code[i + 1] == 0x90)
            {
                std::memcpy(&converter, code + i + 2, sizeof converter);
                break;
            }
        if (converter == 0) return false;

        for (std::size_t i = 0; i + 4 <= sizeof code; ++i)
        {
            if (code[i] != 0x48 || code[i + 1] != 0x6B
             || code[i + 2] != 0xC0 || code[i + 3] != 0x68) continue;

            for (std::size_t j = i + 4; j + 5 <= sizeof code && j <= i + 12; ++j)
                if (code[j] == 0x48 && code[j + 1] == 0x8B
                 && code[j + 2] == 0x44 && code[j + 3] == 0x38)
                {
                    *outDisp = code[j + 4];
                    *outConverterOffset = converter;
                    return true;
                }
            return false;
        }
        return false;
    }

    bool ResolveRowIndex(void* self, std::uint16_t id, std::uint16_t* outRow)
    {
        const std::uint32_t off =
            g_ConverterVtblOffset.load(std::memory_order_relaxed);
        if (off == 0 || (off % sizeof(void*)) != 0) return false;

        __try
        {
            void** vtbl = *reinterpret_cast<void***>(self);
            if (!vtbl) return false;
            auto convert =
                reinterpret_cast<DevelopIndex_t>(vtbl[off / sizeof(void*)]);
            if (!convert) return false;
            *outRow = convert(self, id);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool TryGetOwnSuitLabel(void* self, std::uint16_t id, bool isName,
                            std::uint64_t* outStringId)
    {
        const std::uint8_t livePT = outfit::ReadLivePlayerType();
        if (!outfit::IsUniqueCharacterPlayerType(livePT)) return false;

        std::uint16_t row = 0;
        if (!ResolveRowIndex(self, id, &row)) return false;
        if (!uniqueownsuit::IsOwnSuitRow(livePT, row)
         && !uniquedefaultoutfit::IsDefaultOutfitRow(livePT, row)) return false;

        std::uint64_t name = 0;
        std::uint64_t info = 0;
        if (!uniqueownsuit::TryGetRowLabel(livePT, &name, &info)) return false;

        const std::uint64_t picked = isName ? name : info;
        if (picked == 0) return false;

        *outStringId = picked;
        return true;
    }

    template <int I>
    std::uint64_t* __fastcall hkLabel(void* self, std::uint64_t* out,
                                      std::uint16_t id)
    {
        std::uint64_t overrideId = 0;
        const bool hit = out && self
            && TryGetOwnSuitLabel(self, id, g_Slots[I].isName, &overrideId);
        if (hit)
        {
            *out = overrideId;
            return out;
        }
        return g_Slots[I].orig(self, out, id);
    }

    void* DetourFor(int index)
    {
        return index == kLabel_ListRowName
                   ? reinterpret_cast<void*>(&hkLabel<kLabel_ListRowName>)
             : index == kLabel_DetailsHead
                   ? reinterpret_cast<void*>(&hkLabel<kLabel_DetailsHead>)
                   : reinterpret_cast<void*>(&hkLabel<kLabel_DetailsBody>);
    }
}

namespace outfit
{
    bool Install_OwnSuitRowLabel_Hooks()
    {
        if (g_Installed) return true;

        void* controller = EquipDevelop_ResolveDevelopController();
        if (!controller) return false;

        void** vtbl = nullptr;
        __try { vtbl = *reinterpret_cast<void***>(controller); }
        __except (EXCEPTION_EXECUTE_HANDLER) { vtbl = nullptr; }
        if (!vtbl) return false;

        if (g_ScanAttempts.fetch_add(1, std::memory_order_relaxed)
                >= kMaxScanAttempts)
            return false;

        void*         found[kLabelCount] = {};
        std::uint32_t converter          = 0;
        bool          converterAgrees    = true;

        for (std::size_t k = 0; k < kVtblScanSlots; ++k)
        {
            void* fn = nullptr;
            if (!SafeReadVtblEntry(vtbl, k, &fn)) break;
            if (!IsExecutableCode(fn)) continue;

            std::uint8_t  disp = 0;
            std::uint32_t conv = 0;
            if (!ClassifyRecordGetter(fn, &disp, &conv)) continue;

            for (int i = 0; i < kLabelCount; ++i)
            {
                if (g_Slots[i].disp != disp || found[i]) continue;
                found[i] = fn;
                if (converter == 0) converter = conv;
                else if (converter != conv) converterAgrees = false;
            }
        }

        int located = 0;
        for (int i = 0; i < kLabelCount; ++i) if (found[i]) ++located;

        if (located != kLabelCount || !converterAgrees || converter == 0)
        {
            if (!g_GaveUpLogged.exchange(true))
            Log("[UniqueOwnSuit] found %d of %d develop-record label accessors in "
                "the controller vtable%s - the own-suit row keeps the borrowed "
                "vanilla label, because overriding only some of them would leave "
                "the list and the details panel disagreeing\n",
                located, kLabelCount,
                converterAgrees ? "" : " (and they disagreed on the row-index "
                                       "accessor, so the match is not trustworthy)");
            return false;
        }

        g_ConverterVtblOffset.store(converter, std::memory_order_relaxed);

        int hooked = 0;
        for (int i = 0; i < kLabelCount; ++i)
        {
            g_Slots[i].target = found[i];
            if (!CreateAndEnableHook(g_Slots[i].target, DetourFor(i),
                                     reinterpret_cast<void**>(&g_Slots[i].orig)))
            {
                g_Slots[i].orig = nullptr;
                Log("[UniqueOwnSuit] hook FAIL on the develop-record accessor at "
                    "%p, so the %s keeps the borrowed vanilla label\n",
                    g_Slots[i].target, g_Slots[i].what);
                continue;
            }
            ++hooked;
        }

        if (hooked != kLabelCount)
        {
            Uninstall_OwnSuitRowLabel_Hooks();
            return false;
        }

#ifdef _DEBUG
        LogDebug("[UniqueOwnSuit] own-suit row label armed: listRowName=%p "
                 "detailsHeading=%p detailsDescription=%p (row-index accessor at "
                 "vtbl+0x%03X) - all three were identified by the record field "
                 "each one reads, not by a hard-coded slot\n",
                 g_Slots[kLabel_ListRowName].target,
                 g_Slots[kLabel_DetailsHead].target,
                 g_Slots[kLabel_DetailsBody].target,
                 converter);
#endif

        g_Installed = true;
        return true;
    }

    void Uninstall_OwnSuitRowLabel_Hooks()
    {
        for (LabelSlot& slot : g_Slots)
        {
            if (!slot.orig) continue;
            DisableAndRemoveHook(slot.target);
            slot.orig   = nullptr;
            slot.target = nullptr;
        }
        g_ScanAttempts.store(0, std::memory_order_relaxed);
        g_GaveUpLogged.store(false, std::memory_order_relaxed);
        g_Installed = false;
    }
}
