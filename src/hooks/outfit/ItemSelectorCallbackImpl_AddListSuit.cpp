#include "pch.h"

#include "ItemSelectorCallbackImpl_AddListSuit.h"
#include "OutfitRegistry.h"
#include "EquipDevelopControllerImpl_GetSuitDevelopInfoIndex.h"
#include "CustomHeadRegistry.h"
#include "MissionCodeGuard.h"
#include "UniqueCharacterOwnSuit.h"
#include "UniqueCharacterDefaultOutfit.h"
#include "../equip/DevelopArrayGrow.h"
#include "../equip/EquipDevelop_SetEquipUndeveloped.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"
#include "../../core/V_FrameWorkState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{


    using SetupPrefabListElement_t = void  (__fastcall*)(void* thisPtr);
    using AddListSuit_t            = void  (__fastcall*)(
                                            void* thisPtr,
                                            std::uint32_t* rowCounter,
                                            std::uint16_t flowIndex,
                                            void* entryBuf);


    using AddListBandana_t = void (__fastcall*)(void* thisPtr,
                                                std::uint32_t* count,
                                                std::uint16_t equipId);
    static AddListBandana_t g_AddListBandana = nullptr;
    static std::unordered_set<std::uint16_t> g_UnboundVariantRowLogged;

    static void NoteUnboundVariantRow(std::uint16_t developId,
                                      std::uint8_t variantCount)
    {
        if (!g_UnboundVariantRowLogged.insert(developId).second)
            return;
        Log("[OutfitListInject:AddListSuit] developId=%u declares %u variants but "
            "holds no live bytes - listed as a single-variant row; variants appear "
            "when it scrolls into view or on pick\n",
            static_cast<unsigned>(developId),
            static_cast<unsigned>(variantCount));
    }

    struct NativeRowCell0
    {
        std::uint32_t color  = 0;
        std::uint32_t flags  = 0;
        std::uint8_t  enable = 0;
        bool          used   = false;
    };
    static NativeRowCell0 g_NativeRowCell0[equip::kMaxFlowSlots] = {};
    static std::unordered_set<std::uint16_t> g_RowWindowLogged;

    static void ResetNativeRowCell0()
    {
        std::memset(g_NativeRowCell0, 0, sizeof(g_NativeRowCell0));
    }

#ifdef _DEBUG
    struct ListBuildProbe
    {
        std::uint32_t equipKind = 0xFFFFFFFFu;
        std::uint64_t panelSig  = 0;
    };

    static ListBuildProbe ReadListBuildProbeSEH(void* thisPtr)
    {
        ListBuildProbe p;
        __try
        {
            auto* b = reinterpret_cast<std::uint8_t*>(thisPtr);
            p.equipKind = *reinterpret_cast<std::uint32_t*>(b + 0x4434);
            p.panelSig  = *reinterpret_cast<std::uint64_t*>(b + 0x461b8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return p;
    }

    static std::atomic<DWORD>  g_BuildThreadId{ 0 };
    static std::atomic<DWORD>  g_BuildStartTick{ 0 };
    static std::atomic<bool>   g_BuildActive{ false };
    static std::atomic<int>    g_BuildDumps{ 0 };
    static std::atomic<bool>   g_WatchdogStarted{ false };

    static DWORD64 g_PrevRip = 0;
    static DWORD64 g_PrevRsp = 0;

    static void DumpFrozenBuildStack(unsigned atMs)
    {
        DWORD tid = g_BuildThreadId.load();
        if (!tid) return;
        HANDLE h = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, tid);
        if (!h)
        {
            Log("[BuildWatchdog] OpenThread failed (err=%lu)\n", GetLastError());
            return;
        }

        DWORD64 rip = 0, rsp = 0;
        std::uint64_t stackbuf[192] = {};
        std::size_t   stackn = 0;

        SuspendThread(h);
        CONTEXT ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &ctx))
        {
            rip = ctx.Rip;
            rsp = ctx.Rsp;
            __try
            {
                auto* sp = reinterpret_cast<std::uint64_t*>(rsp);
                for (std::size_t i = 0; i < 192; ++i)
                {
                    stackbuf[i] = sp[i];
                    ++stackn;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        ResumeThread(h);
        CloseHandle(h);

        LogDebug("[BuildWatchdog] FROZEN list build: SetupPrefabListElement has run "
                 ">%ums without returning. rip=0x%llX rsp=0x%llX (no ASLR)\n",
            atMs,
            static_cast<unsigned long long>(rip),
            static_cast<unsigned long long>(rsp));

        if (g_PrevRip)
        {
            LogDebug("[BuildWatchdog]   since the previous sample: rip 0x%llX -> "
                     "0x%llX, rsp 0x%llX -> 0x%llX (both unchanged = wedged on one "
                     "instruction)\n",
                static_cast<unsigned long long>(g_PrevRip),
                static_cast<unsigned long long>(rip),
                static_cast<unsigned long long>(g_PrevRsp),
                static_cast<unsigned long long>(rsp));
        }
        g_PrevRip = rip;
        g_PrevRsp = rsp;

        char phase[32];
        sprintf_s(phase, "frozen build, %ums", atMs);
        equip::LogDevelopScanCounters(phase);

        int shown = 0;
        for (std::size_t i = 0; i < stackn && shown < 32; ++i)
        {
            std::uint64_t v = stackbuf[i];
            if (v >= 0x140000000ull && v < 0x142E00000ull)
            {
                LogDebug("[BuildWatchdog]   ret[rsp+0x%03X] = 0x%llX (game-code return "
                    "address - the frozen call chain)\n",
                    static_cast<unsigned>(i * 8),
                    static_cast<unsigned long long>(v));
                ++shown;
            }
        }
        if (shown == 0)
            LogDebug("[BuildWatchdog]   (no game-code addresses found on the sampled stack)\n");
    }

    static DWORD WINAPI BuildWatchdogThread(LPVOID)
    {
        for (;;)
        {
            Sleep(500);
            if (g_BuildActive.load())
            {
                const DWORD elapsed = GetTickCount() - g_BuildStartTick.load();
                const int   dumps   = g_BuildDumps.load();
                if (dumps == 0 && elapsed > 4000u)
                {
                    g_BuildDumps.store(1);
                    DumpFrozenBuildStack(4000u);
                }
                else if (dumps == 1 && elapsed > 8000u)
                {
                    g_BuildDumps.store(2);
                    DumpFrozenBuildStack(8000u);
                }
            }
        }
    }

    static void EnsureBuildWatchdog()
    {
        bool expected = false;
        if (g_WatchdogStarted.compare_exchange_strong(expected, true))
        {
            HANDLE t = CreateThread(nullptr, 0, BuildWatchdogThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
#endif

#ifdef _DEBUG
    static std::unordered_map<std::uint16_t, std::uint64_t> g_MatchLogged;
    static std::unordered_map<std::uint16_t, std::uint64_t> g_OverrideLogged;

    static bool ShouldLogRowSig(
        std::unordered_map<std::uint16_t, std::uint64_t>& logged,
        std::uint16_t selectedId, std::uint64_t sig)
    {
        auto it = logged.find(selectedId);
        if (it != logged.end() && it->second == sig)
            return false;
        logged[selectedId] = sig;
        return true;
    }
#endif
#ifdef _DEBUG
    static std::atomic<bool> g_HeadOptionInjectFirstFire{ false };
#endif


    using UpdateRecords_t          = void  (__fastcall*)(void* thisPtr);

    static SetupPrefabListElement_t g_OrigSetupPrefab    = nullptr;


    static AddListSuit_t            g_OrigAddListSuit    = nullptr;

    static void*                    g_AddListSuitAddr    = nullptr;

    static UpdateRecords_t          g_OrigUpdateRecords     = nullptr;
    static bool                     g_InstalledUpdateRecords = false;

    using AddRecord_t = void (__fastcall*)(void* thisPtr,
                                           std::uint16_t flowIndex,
                                           std::uint16_t* loadout,
                                           std::uint32_t row,
                                           std::uint32_t cell,
                                           std::uint8_t arg6,
                                           std::uint8_t arg7,
                                           std::uint8_t arg8);
    static AddRecord_t g_OrigAddRecord      = nullptr;
    static bool        g_InstalledAddRecord = false;


    using HeadBadgeCategory_t = std::uint32_t (__fastcall*)(void* self,
                                                            std::uint32_t equipId);
    using WornHeadCategory_t  = std::uint8_t  (__fastcall*)(void* self);
    static HeadBadgeCategory_t g_OrigHeadBadgeCategory = nullptr;
    static WornHeadCategory_t  g_OrigWornHeadCategory  = nullptr;
    static bool g_InstalledHeadBadgeCategory = false;
    static bool g_InstalledWornHeadCategory  = false;
    static bool g_HeadBadgeBuildActive = false;

    static bool g_HeadEquipDecideActive = false;

    static bool       g_Installed       = false;


    thread_local bool t_InsideSetupPrefab = false;


    thread_local std::array<std::uint64_t,
                            (equip::kMaxFlowSlots + 63) / 64>
        t_AddedFlowIxBits = {};

    static bool TestAndSetAddedBit(std::uint16_t flowIndex)
    {
        if (flowIndex >= equip::kMaxFlowSlots) return false;
        auto& word = t_AddedFlowIxBits[flowIndex >> 6];
        const std::uint64_t mask = 1ull << (flowIndex & 63);
        if (word & mask) return true;
        word |= mask;
        return false;
    }

    static void ResetAddedFlowIxBits()
    {
        t_AddedFlowIxBits.fill(0);
    }

    struct VextCellInfo
    {
        std::uint64_t labelHash = 0;
        std::uint8_t  selector  = 0;
        bool          used      = false;
    };
    static VextCellInfo g_VextCellMap[equip::kMaxFlowSlots][15] = {};

    static void StoreVextCellLabel(std::uint16_t flowIndex, std::uint8_t cellPos,
                                   std::uint8_t selector, std::uint64_t labelHash)
    {
        if (flowIndex >= equip::kMaxFlowSlots || cellPos >= 15) return;
        g_VextCellMap[flowIndex][cellPos].labelHash = labelHash;
        g_VextCellMap[flowIndex][cellPos].selector  = selector;
        g_VextCellMap[flowIndex][cellPos].used       = true;
    }

    static std::uint64_t LookupVextCellLabel(std::uint16_t flowIndex,
                                             std::uint8_t cellPos)
    {
        if (flowIndex >= equip::kMaxFlowSlots || cellPos >= 15) return 0;
        const VextCellInfo& e = g_VextCellMap[flowIndex][cellPos];
        return e.used ? e.labelHash : 0;
    }

    static void SeedVextSwatchFlow(std::uint8_t selector, std::uint8_t sourceCamo)
    {
        if (selector < outfit::kCustomSelectorStart) return;
        using GetQuark_t = void* (__fastcall*)();
        auto getQuark = reinterpret_cast<GetQuark_t>(
            ResolveGameAddress(gAddr.GetQuarkSystemTable));
        if (!getQuark) return;
        __try
        {
            auto* quark = static_cast<std::uint8_t*>(getQuark());
            if (!quark) return;
            auto* app = *reinterpret_cast<std::uint8_t**>(quark + 0x98);
            if (!app) return;
            auto* tbl = *reinterpret_cast<std::uint8_t**>(app + 0x10);
            if (!tbl) return;
            auto* camoToFlow = reinterpret_cast<std::int16_t*>(tbl + 0x19ac);
            camoToFlow[selector] = camoToFlow[sourceCamo];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }


    static constexpr int    kMaxInstallAttempts = 16;


    constexpr std::size_t kVtblIx_GetCount = 0x230 / sizeof(void*);
    constexpr std::size_t kVtblIx_Fill     = 0x240 / sizeof(void*);
    constexpr std::size_t kVtblIx_GetTable = 0x718 / sizeof(void*);






    static bool g_VariantInjectEnabled    = true;
    static bool g_HeadOptionInjectEnabled = true;

    static void StampVariantRowCells(std::uint8_t* base, std::uint32_t row,
                                     const outfit::OutfitEntry* entry,
                                     std::uint16_t flowIndex,
                                     std::uint8_t variantsForPT,
                                     std::uint8_t rowEnable)
    {
        if (variantsForPT > 15)
        {
            static std::atomic<int> s_varClamped{ 0 };
            if (s_varClamped.fetch_add(1, std::memory_order_relaxed) < 4)
                Log("[OutfitListInject] an outfit declares %u variants but a list "
                    "row holds 15 cells - clamped, because writing past the row "
                    "overwrites the next outfit's swatches and eventually the "
                    "panel signature this module guards on\n",
                    static_cast<unsigned>(variantsForPT));
            variantsForPT = 15;
        }

        for (std::uint8_t var = 0; var < variantsForPT; ++var)
        {
            const std::size_t cellIndex =
                static_cast<std::size_t>(row) * 15 + var;

            *reinterpret_cast<std::uint16_t*>(
                base + 0x4440 + cellIndex * 2) = flowIndex;

            std::uint8_t* cell = base + 0xCC40 + cellIndex * 12;
            *reinterpret_cast<std::uint32_t*>(cell + 0) =
                static_cast<std::uint32_t>(
                    entry->variantSelectorCodes[var]);
            *reinterpret_cast<std::uint32_t*>(cell + 4) =
                (var == 0) ? 7u : 0u;
            *(cell + 8) = 0;

            *(base + 0x548   + cellIndex) = rowEnable;
            *(base + 0x425a4 + cellIndex) = 0;
        }

        *(base + 0xBC40 + row) = variantsForPT;

        std::uint8_t wornPT = 0, wornSel = 0;
        const bool equipped =
            entry->bound
            && outfit::GetCurrentEquippedSuitBytes(&wornPT, &wornSel)
            && wornPT == entry->partsType;

        std::uint8_t displayVar;
        if (equipped)
        {
            displayVar = outfit::GetActiveVariant(entry->partsType);
        }
        else
        {
            displayVar = entry->defaultVariant;
            if (outfit::PeekCrateDeliveredDevelopId()
                    == entry->developId)
                displayVar = outfit::PeekCrateDeliveredVariantIdx();
            else if (outfit::PeekPendingSupplyDropDevelopId()
                     == entry->developId)
                displayVar = outfit::PeekPendingSupplyDropVariantIdx();
            outfit::SetActiveVariant(entry->partsType, displayVar);
        }
        if (displayVar >= variantsForPT)
            displayVar = static_cast<std::uint8_t>(variantsForPT - 1);
        *(base + 0xC040 + row) = displayVar;
        *(base + 0x425a4 + static_cast<std::size_t>(row) * 15 + displayVar) =
            equipped ? std::uint8_t{1} : std::uint8_t{0};
    }

    static bool IsWornCustomOutfit(const outfit::OutfitEntry* entry)
    {
        if (!entry || !entry->bound) return false;
        std::uint8_t wornPT = 0, wornSel = 0;
        return outfit::GetCurrentEquippedSuitBytes(&wornPT, &wornSel)
            && wornPT == entry->partsType;
    }

    static void __fastcall hkAddRecord(void* thisPtr,
                                       std::uint16_t flowIndex,
                                       std::uint16_t* loadout,
                                       std::uint32_t row,
                                       std::uint32_t cell,
                                       std::uint8_t arg6,
                                       std::uint8_t arg7,
                                       std::uint8_t arg8)
    {
        if (g_OrigAddRecord)
            g_OrigAddRecord(thisPtr, flowIndex, loadout, row, cell,
                            arg6, arg7, arg8);

        if (!thisPtr || row > outfit::kPanelRowMax || cell >= 15) return;

        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);
            if (*reinterpret_cast<std::uint64_t*>(base + 0x461b8)
                    != 0xb8a0bf169f98ull)
                return;
            if (*reinterpret_cast<std::int32_t*>(base + 0x461b0) == 1) return;
            if (MissionCodeGuard::ShouldBypassHooks()) return;

            const std::uint8_t livePT = outfit::ReadLivePlayerType();
            const bool isDefaultRow =
                outfit::IsUniqueCharacterPlayerType(livePT)
                && uniquedefaultoutfit::IsDefaultOutfitRow(livePT, flowIndex);
            if (outfit::IsUniqueCharacterPlayerType(livePT)
                && (isDefaultRow
                    || uniqueownsuit::IsOwnSuitRow(livePT, flowIndex)))
            {
                std::uint8_t ownPT = 0, ownSel = 0;
                const bool haveWorn =
                    outfit::GetCurrentEquippedSuitBytes(&ownPT, &ownSel);
                bool worn = haveWorn && ownPT < outfit::kCustomPartsTypeStart;

                if (!worn && haveWorn && isDefaultRow)
                {
                    const outfit::OutfitEntry* def = nullptr;
                    if (outfit::TryGetOutfitByFlowIndex(flowIndex, &def) && def
                        && def->bound && def->partsType == ownPT)
                        worn = true;
                }

                *(base + 0x425a4 + static_cast<std::size_t>(row) * 15 + cell) =
                    worn ? std::uint8_t{1} : std::uint8_t{0};
                return;
            }

            const outfit::OutfitEntry* entry = nullptr;
            if (!outfit::TryGetOutfitByFlowIndex(flowIndex, &entry) || !entry)
                return;

            *(base + 0x425a4 + static_cast<std::size_t>(row) * 15 + cell) =
                IsWornCustomOutfit(entry) ? std::uint8_t{1} : std::uint8_t{0};
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[OutfitListInject:AddRecord] SEH writing the EQUIPPED mark "
                     "(flowIndex=%u row=%u cell=%u) - that row keeps whatever the "
                     "engine decided\n",
                static_cast<unsigned>(flowIndex),
                static_cast<unsigned>(row),
                static_cast<unsigned>(cell));
        }
    }

    static void __fastcall hkAddListSuit(
        void* thisPtr,
        std::uint32_t* rowCounter,
        std::uint16_t flowIndex,
        void* entryBuf)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
        {
            if (g_OrigAddListSuit)
                g_OrigAddListSuit(thisPtr, rowCounter, flowIndex, entryBuf);
            return;
        }

#ifdef _DEBUG
        if (t_InsideSetupPrefab)
        {
            static std::atomic<int> s_bc{ 0 };
            if (s_bc.fetch_add(1) < 4000)
                LogDebug("[OutfitListInject:AddListSuit] build row flowIndex=%u "
                         "(breadcrumb - the last line before a freeze is the row "
                         "that hangs)\n",
                    static_cast<unsigned>(flowIndex));
        }
#endif

        if (outfit::IsCustomHeadEquipId(flowIndex))
        {
#ifdef _DEBUG
            LogDebug("[OutfitListInject:AddListSuit] suppressed custom-HEAD row "
                     "flowIndex=%u (heads belong in the 0x201 submenu)\n", static_cast<unsigned>(flowIndex));
#endif
            return;
        }

        if (t_InsideSetupPrefab)
        {
            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByFlowIndex(flowIndex, &entry) && entry)
            {
                const std::uint8_t livePT = outfit::ReadLivePlayerType();
                if (livePT != 0xFF
                    && !entry->IsPlayerTypeSupported(livePT))
                {
#ifdef _DEBUG
                    LogDebug("[OutfitListInject:AddListSuit] suppressed PT-unsupported "
                        "flowIndex=%u live-PT=%u (developId=%u partsType=0x%02X)\n",
                        static_cast<unsigned>(flowIndex),
                        static_cast<unsigned>(livePT),
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(entry->partsType));
#endif
                    return;
                }
            }
        }

        if (!g_VariantInjectEnabled)
        {
            if (g_OrigAddListSuit)
                g_OrigAddListSuit(thisPtr, rowCounter, flowIndex, entryBuf);
            return;
        }

        if (t_InsideSetupPrefab && TestAndSetAddedBit(flowIndex))
        {


            return;
        }


        const std::uint32_t rowPre =
            (rowCounter ? *rowCounter : 0xFFFFFFFFu);

        if (g_OrigAddListSuit)
            g_OrigAddListSuit(thisPtr, rowCounter, flowIndex, entryBuf);


        if (!thisPtr || !rowCounter) return;
        const std::uint32_t rowPost = *rowCounter;
        if (rowPost == rowPre)
        {


            return;
        }
        const std::uint32_t row = rowPost - 1;
        if (row > outfit::kPanelRowMax) return;

        const std::uint8_t livePT = outfit::ReadLivePlayerType();
        const outfit::OutfitEntry* entry = nullptr;
        bool isCustom =
            outfit::TryGetOutfitByFlowIndex(flowIndex, &entry) && entry;

        if (isCustom && entry && !entry->bound)
        {
            const std::uint8_t listVariants = (livePT != 0xFF)
                ? entry->GetVariantCountFor(livePT)
                : entry->variantCount;
            if (listVariants >= 2)
            {
                outfit::BindOutfitForVisibleRow(entry->developId);
                isCustom =
                    outfit::TryGetOutfitByFlowIndex(flowIndex, &entry) && entry;
            }
        }
        if (isCustom && entry)
            outfit::NoteOutfitMenuStamp(entry->developId);

        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);

            if (*reinterpret_cast<std::uint64_t*>(base + 0x461b8)
                    != 0xb8a0bf169f98ull)
            {
                LogDebug("[OutfitListInject:AddListSuit] wrong-object guard: "
                         "thisPtr=%p is not the suit panel (+0x461b8 mismatch) - "
                         "skipped variant-cell writes (flowIndex=%u)\n",
                    thisPtr, static_cast<unsigned>(flowIndex));
                return;
            }

            if (isCustom && entry && flowIndex < equip::kMaxFlowSlots)
            {
                NativeRowCell0& snap = g_NativeRowCell0[flowIndex];
                if (!snap.used)
                {
                    const std::size_t c0 =
                        static_cast<std::size_t>(row) * 15;
                    snap.color  = *reinterpret_cast<std::uint32_t*>(
                        base + 0xCC40 + c0 * 12);
                    snap.flags  = *reinterpret_cast<std::uint32_t*>(
                        base + 0xCC40 + c0 * 12 + 4);
                    snap.enable = *(base + 0x548 + c0);
                    snap.used   = true;
                }
            }

            if (isCustom)
            {
                const std::uint8_t variantsForPT = (livePT != 0xFF)
                    ? entry->GetVariantCountFor(livePT)
                    : entry->variantCount;
                if (variantsForPT < 2)
                    return;

                if (!entry->bound)
                {
                    NoteUnboundVariantRow(entry->developId, variantsForPT);
                    return;
                }

                const std::uint8_t rowEnable =
                    row <= 0x3F ? *(base + 0x548 + static_cast<std::size_t>(row) * 15) : 1;

                StampVariantRowCells(base, row, entry, flowIndex,
                                     variantsForPT, rowEnable);
                return;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[OutfitListInject:AddListSuit] SEH writing variant "
                "cells (flowIndex=%u row=%u)\n",
                static_cast<unsigned>(flowIndex),
                static_cast<unsigned>(row));
        }
    }


    static bool LooksLikeValidPtr(const void* p)
    {
        const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
        if (v == 0) return false;
        if (v < 0x10000) return false;
        if ((v & 0x7) != 0) return false;

        if (v >= 0x800000000000ull) return false;
        return true;
    }




    using GetTextByHash_t  = void* (__fastcall*)(void* manager,
                                                  std::uint64_t hash);
    using WriteTextField_t = void  (__fastcall*)(void* manager,
                                                  void* dst1,
                                                  void* dst2,
                                                  void* text);

    using SetNodeTexture_t = void (__fastcall*)(void* manager,
                                                void* node,
                                                std::uint64_t texturePathHash,
                                                std::uint64_t slotNameHash);

    constexpr std::size_t kVtblSlot_PrepIsFobSortie   = 0x4F0 / 8;
    constexpr std::size_t kVtblSlot_DevIsFobAvailable = 0x478 / 8;
    constexpr std::size_t kVtblSlot_SetNodeTexture    = 0x518 / 8;
    constexpr std::uint64_t kIconTextureSlotHash      = 0xCAFB3BBF9889ull;

    using GetUixUtility_t = void** (__fastcall*)();
    static GetUixUtility_t g_GetUixUtility = nullptr;

    static void PrefetchIconTexture(std::uint64_t textureHash)
    {
        if (textureHash == 0 || gAddr.GetUixUtilityToFeedQuarkEnvironment == 0)
            return;
        if (!g_GetUixUtility)
            g_GetUixUtility = reinterpret_cast<GetUixUtility_t>(
                ResolveGameAddress(gAddr.GetUixUtilityToFeedQuarkEnvironment));
        if (!g_GetUixUtility) return;
        __try
        {
            void** util = g_GetUixUtility();
            if (!util) return;
            void** vtbl = *reinterpret_cast<void***>(util);
            if (!vtbl) return;
            auto fn = reinterpret_cast<void(__fastcall*)(void*, std::uint64_t, int)>(
                vtbl[0x548 / sizeof(void*)]);
            if (fn) fn(util, textureHash, 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool IsPanelFobSortie(std::uint8_t* panel)
    {
        __try
        {
            void* sys = *reinterpret_cast<void**>(panel + 0x70);
            if (!sys) return false;
            using CtxFn_t = std::uint8_t (__fastcall*)(void*);
            auto ctx = reinterpret_cast<CtxFn_t>(
                (*reinterpret_cast<void***>(sys))[kVtblSlot_PrepIsFobSortie]);
            return ctx && ctx(sys) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static std::int32_t ReadCamoToFlow(std::uint8_t idx)
    {
        using GetQuark_t = void* (__fastcall*)();
        auto getQuark = reinterpret_cast<GetQuark_t>(
            ResolveGameAddress(gAddr.GetQuarkSystemTable));
        if (!getQuark) return -1;
        __try
        {
            auto* quark = static_cast<std::uint8_t*>(getQuark());
            if (!quark) return -1;
            auto* app = *reinterpret_cast<std::uint8_t**>(quark + 0x98);
            if (!app) return -1;
            auto* tbl = *reinterpret_cast<std::uint8_t**>(app + 0x10);
            if (!tbl) return -1;
            return *reinterpret_cast<std::int16_t*>(tbl + 0x19ac + idx * 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    static void JitRefreshCustomRow(std::uint8_t* base, std::uint32_t row)
    {
        if (*reinterpret_cast<std::uint64_t*>(base + 0x461b8)
                != 0xb8a0bf169f98ull)
            return;
        const std::uint32_t equipKind =
            *reinterpret_cast<std::uint32_t*>(base + 0x4434);
        if (equipKind != 0x80u && equipKind != 0x100u)
            return;

        const std::uint16_t rowFlow = *reinterpret_cast<std::uint16_t*>(
            base + 0x4440 + static_cast<std::size_t>(row) * 15 * 2);
        if (rowFlow == 0 || rowFlow >= equip::kMaxFlowSlots)
            return;

        const outfit::OutfitEntry* entry = nullptr;
        if (!outfit::TryGetOutfitByFlowIndex(rowFlow, &entry) || !entry)
            return;

        if (IsPanelFobSortie(base))
            return;

        outfit::NoteOutfitRowRefresh(entry->developId);

        const std::uint8_t livePT = outfit::ReadLivePlayerType();
        const std::uint8_t variantsForPT = (livePT != 0xFF)
            ? entry->GetVariantCountFor(livePT)
            : entry->variantCount;
        if (variantsForPT < 2)
            return;

        const std::size_t c0 = static_cast<std::size_t>(row) * 15;

        if (!entry->bound)
        {
            if (!outfit::BindOutfitForVisibleRow(entry->developId))
            {
                const std::uint8_t count = *(base + 0xBC40 + row);
                const std::uint8_t sel0  = static_cast<std::uint8_t>(
                    *reinterpret_cast<std::uint32_t*>(
                        base + 0xCC40 + c0 * 12));
                const bool stampedStale =
                    count >= 2
                    || (sel0 >= outfit::kCustomSelectorStart
                        && sel0 <= outfit::kCustomSelectorEnd);
                if (!stampedStale)
                    return;

                const NativeRowCell0& snap = g_NativeRowCell0[rowFlow];
                if (!snap.used)
                {
                    if (g_RowWindowLogged.insert(entry->developId).second)
                        Log("[OutfitListInject:RowWindow] WARN developId=%u has "
                            "stale variant cells but no native cell snapshot - row "
                            "left as-is (stale swatches until the menu rebuilds)\n",
                            static_cast<unsigned>(entry->developId));
                    return;
                }

                std::uint32_t color = snap.color;
                const std::uint8_t nb = static_cast<std::uint8_t>(color);
                if (nb >= outfit::kCustomSelectorStart
                    && nb <= outfit::kCustomSelectorEnd)
                    color &= 0xFFFFFF00u;

                std::uint8_t* cell = base + 0xCC40 + c0 * 12;
                *reinterpret_cast<std::uint32_t*>(cell + 0) = color;
                *reinterpret_cast<std::uint32_t*>(cell + 4) = snap.flags;
                *(cell + 8) = 0;
                *(base + 0x548 + c0) = snap.enable;
                for (std::uint8_t c = 0; c < 15; ++c)
                    *(base + 0x425a4 + c0 + c) = 0;
                for (std::uint8_t c = 1; c < 15; ++c)
                    *(base + 0x548 + c0 + c) = 0;
                *(base + 0xBC40 + row) = 1;
                *(base + 0xC040 + row) = 0;

                if (g_RowWindowLogged.insert(entry->developId).second)
                    Log("[OutfitListInject:RowWindow] WARN developId=%u is visible "
                        "with %u variants but every selector byte is held by other "
                        "outfits - demoted to its native single-variant cell until "
                        "one frees\n",
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(variantsForPT));
                return;
            }
            if (!outfit::TryGetOutfitByFlowIndex(rowFlow, &entry) || !entry
                || !entry->bound)
                return;
        }

        const std::uint8_t count = *(base + 0xBC40 + row);
        bool current = (count == variantsForPT);
        for (std::uint8_t v = 0; current && v < variantsForPT; ++v)
            current = static_cast<std::uint8_t>(
                          *reinterpret_cast<std::uint32_t*>(
                              base + 0xCC40 + (c0 + v) * 12))
                      == entry->variantSelectorCodes[v];
        if (current)
            return;

        const NativeRowCell0& snap = g_NativeRowCell0[rowFlow];
        const std::uint8_t rowEnable = snap.used
            ? snap.enable
            : (row <= 0x3F ? *(base + 0x548 + c0) : std::uint8_t{ 1 });
        StampVariantRowCells(base, row, entry, rowFlow, variantsForPT,
                             rowEnable);
    }

    static void __fastcall hkUpdateRecords(void* thisPtr)
    {
        if (thisPtr && !MissionCodeGuard::ShouldBypassHooks())
        {
            std::uint8_t seedSel[128];
            std::uint8_t seedSrc[128];
            const std::uint8_t seedCount =
                outfit::VanillaExtCollectSelectorSeeds(seedSel, seedSrc, 128);
            for (std::uint8_t i = 0; i < seedCount; ++i)
                SeedVextSwatchFlow(seedSel[i], seedSrc[i]);

            __try
            {
                auto* rb = reinterpret_cast<std::uint8_t*>(thisPtr);
                const std::uint32_t row =
                    *reinterpret_cast<std::uint32_t*>(rb + 0x008);
                const auto flowTable =
                    *reinterpret_cast<std::uint16_t* const*>(rb + 0x1E8);
                if (row <= outfit::kPanelRowMax && flowTable)
                {
                    auto* panel = reinterpret_cast<std::uint8_t*>(
                        reinterpret_cast<std::uintptr_t>(flowTable) - 0x4440);
                    JitRefreshCustomRow(panel, row);
                    {
                        const std::uint8_t ownPT = outfit::ReadLivePlayerType();
                        if (outfit::IsUniqueCharacterPlayerType(ownPT)
                            && uniqueownsuit::IsOwnSuitRow(
                                   ownPT,
                                   flowTable[static_cast<std::size_t>(row) * 15]))
                        {
                            if (*(panel + 0xBC40 + row) != 1)
                                *(panel + 0xBC40 + row) = 1;
                            if (*(panel + 0xC040 + row) != 0)
                                *(panel + 0xC040 + row) = 0;
                        }
                    }
                    std::uint8_t scrubbed = 0;
                    std::uint8_t firstRaw = 0;
                    for (std::uint8_t c = 0; c < 15; ++c)
                    {
                        auto* cell = panel + 0xCC40
                            + (static_cast<std::size_t>(row) * 15 + c) * 12;
                        const std::uint8_t camo = *cell;
                        if (camo < outfit::kCustomSelectorStart
                         || camo > outfit::kCustomSelectorEnd) continue;
                        std::uint8_t svpt = 0, svidx = 0;
                        if (!outfit::TryGetVanillaExtByVariantSelector(
                                camo, &svpt, &svidx)) continue;
                        const std::uint8_t src =
                            outfit::VanillaExtGetVariantSourceCamo(svpt, svidx);
                        if (src == 0xFF) continue;
                        if (scrubbed == 0) firstRaw = camo;
                        *cell = src;
                        ++scrubbed;
                    }
#ifdef _DEBUG
                    if (scrubbed != 0)
                    {
                        static std::atomic<int> s_scrubLog{0};
                        if (int n = s_scrubLog.load(std::memory_order_relaxed);
                            n < 24)
                        {
                            s_scrubLog.store(n + 1, std::memory_order_relaxed);
                            LogDebug("[RedCrossDiag] render-time cell scrub: row=%u "
                                     "%u cell(s) held a vext selector "
                                     "(first=0x%02X) - reset to source camo before "
                                     "the orig render\n",
                                row, static_cast<unsigned>(scrubbed),
                                static_cast<unsigned>(firstRaw));
                        }
                    }
#endif
                    {
                        const std::uint16_t rowFlow =
                            flowTable[static_cast<std::size_t>(row) * 15];
                        bool isVextRow = false;
                        if (rowFlow < equip::kMaxFlowSlots)
                            for (std::uint8_t c = 0; c < 15 && !isVextRow; ++c)
                                if (outfit::VextLookupCellSelector(rowFlow, c)
                                        != 0)
                                    isVextRow = true;
                        if (isVextRow && !IsPanelFobSortie(panel))
                        {
                            const std::uint8_t rowCamo0 =
                                *(panel + 0xCC40
                                  + static_cast<std::size_t>(row) * 15 * 12);
                            const std::uint8_t fvpt =
                                outfit::ResolveVanillaPartsTypeForCamo(rowCamo0);
                            if (fvpt != 0xFF
                                && outfit::GetActiveVariant(fvpt) != 0)
                            {
                                const std::uint8_t factive =
                                    outfit::GetActiveVariant(fvpt);
                                const std::uint8_t fcnt =
                                    *(panel + 0xBC40 + row);
                                for (std::uint8_t c = 0;
                                     c < fcnt && c < 15; ++c)
                                {
                                    const std::size_t ci =
                                        static_cast<std::size_t>(row) * 15 + c;
                                    const std::uint8_t cSel =
                                        outfit::VextLookupCellSelector(
                                            rowFlow, c);
                                    std::uint8_t want = 1;
                                    if (cSel != 0)
                                    {
                                        std::uint8_t cvpt = 0, cvidx = 0;
                                        if (outfit::TryGetVanillaExtByVariantSelector(
                                                cSel, &cvpt, &cvidx)
                                            && cvpt == fvpt
                                            && cvidx == factive)
                                            want = 0;
                                    }
                                    if (*(panel + 0x548 + ci) != want)
                                        *(panel + 0x548 + ci) = want;
                                }
                            }
                            else if (fvpt != 0xFF)
                            {
                                const std::uint8_t fcnt =
                                    *(panel + 0xBC40 + row);
                                for (std::uint8_t c = 0;
                                     c < fcnt && c < 15; ++c)
                                {
                                    if (outfit::VextLookupCellSelector(
                                            rowFlow, c) == 0)
                                        continue;
                                    const std::size_t ci =
                                        static_cast<std::size_t>(row) * 15 + c;
                                    if (*(panel + 0x425a4 + ci) != 0)
                                        *(panel + 0x425a4 + ci) = 0;
                                    if (*(panel + 0x548 + ci) == 0)
                                        *(panel + 0x548 + ci) = 1;
                                }
                            }
#ifdef _DEBUG
                            const std::uint8_t cnt   = *(panel + 0xBC40 + row);
                            const std::uint8_t selC2 =
                                *(panel + 0xC040 + row);
                            char cells[256];
                            int  pos = 0;
                            for (std::uint8_t c = 0;
                                 c < cnt && c < 15 && pos < 200; ++c)
                            {
                                const std::size_t ci =
                                    static_cast<std::size_t>(row) * 15 + c;
                                auto* cd = panel + 0xCC40 + ci * 12;
                                pos += std::snprintf(cells + pos,
                                    sizeof(cells) - pos,
                                    " [%u]c=%02X s=%X e=%u n=%u",
                                    c, *cd,
                                    *reinterpret_cast<std::uint32_t*>(cd + 4),
                                    *(panel + 0x425a4 + ci),
                                    *(panel + 0x548 + ci));
                            }
                            static std::uint64_t s_lastRowKey = 0;
                            std::uint64_t key = 1469598103934665603ull;
                            for (int i = 0; i < pos; ++i)
                                key = (key ^ cells[i]) * 1099511628211ull;
                            key ^= (static_cast<std::uint64_t>(selC2) << 56);
                            if (key != s_lastRowKey)
                            {
                                s_lastRowKey = key;
                                LogDebug("[RedCrossDiag] vextrow row=%u flow=%u "
                                    "count=%u selCell=%u%s\n",
                                    row, rowFlow,
                                    static_cast<unsigned>(cnt),
                                    static_cast<unsigned>(selC2), cells);
                            }
#endif
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (g_OrigUpdateRecords) g_OrigUpdateRecords(thisPtr);

        if (!thisPtr) return;
        if (MissionCodeGuard::ShouldBypassHooks()) return;

        std::uint64_t variantHash = 0;
        std::uint64_t iconHash    = 0;
        std::uint8_t  variantIdx  = 0;
        std::uint16_t selectedId  = 0;

        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);

            const std::uint32_t row = *reinterpret_cast<std::uint32_t*>(base + 0x008);
            if (row > outfit::kPanelRowMax) return;

            const auto variantTable =
                *reinterpret_cast<std::uint8_t* const*>(base + 0x1F0);
            const auto selectedIdTable =
                *reinterpret_cast<std::uint16_t* const*>(base + 0x1E8);
            if (!variantTable || !selectedIdTable) return;

            variantIdx = *(variantTable + row);
            if (variantIdx > 14) return;

            const std::size_t cellIndex =
                static_cast<std::size_t>(row) * 15 + variantIdx;
            selectedId = *(selectedIdTable + cellIndex);


            const outfit::OutfitEntry* entry = nullptr;
            if (outfit::TryGetOutfitByFlowIndex(selectedId, &entry) && entry)
            {
                if (variantIdx >= outfit::kMaxVariantsPerOutfit)
                    return;

                const std::uint8_t livePT = outfit::ReadLivePlayerType();
                const std::uint8_t labelPT =
                    (livePT != 0xFF && entry->IsPlayerTypeSupported(livePT))
                        ? livePT
                        : outfit::kPlayerType_Snake;
                variantHash =
                    entry->GetVariantDisplayNameHash(labelPT, variantIdx);
                iconHash = entry->GetVariantIconPathHash(labelPT, variantIdx);

#ifdef _DEBUG
                const std::uint64_t matchSig =
                    (static_cast<std::uint64_t>(variantIdx) << 56)
                    ^ (variantHash & 0x00FFFFFFFFFFFFFFull);
                if (ShouldLogRowSig(g_MatchLogged, selectedId, matchSig))
                    LogDebug("[OutfitListInject:UpdateRecords] matched custom "
                        "outfit row: selectedId=%u developId=%u variantIdx=%u "
                        "variantHash=0x%016llX %s\n",
                        static_cast<unsigned>(selectedId),
                        static_cast<unsigned>(entry->developId),
                        static_cast<unsigned>(variantIdx),
                        static_cast<unsigned long long>(variantHash),
                        variantHash == 0
                            ? "(no displayName set in Lua - orig label kept)"
                            : "(will override)");
#endif
            }
            else
            {
                const std::uint8_t livePT = outfit::ReadLivePlayerType();
                if (!uniqueownsuit::TryGetLabel(livePT, selectedId,
                                                &variantHash, &iconHash))
                    variantHash = LookupVextCellLabel(selectedId, variantIdx);
            }
            if (variantHash == 0 && iconHash == 0) return;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }


        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);

            void* manager     = *reinterpret_cast<void**>(base + 0x38);
            void* writeTarget1 = *reinterpret_cast<void**>(base + 0x180);
            void* writeTarget2 = *reinterpret_cast<void**>(base + 0x80);

            if (!manager) return;

            void** managerVtable = *reinterpret_cast<void***>(manager);
            if (!managerVtable) return;

            if (iconHash != 0)
            {
                void* iconNode1 = *reinterpret_cast<void**>(base + 0xA8);
                void* iconNode2 = *reinterpret_cast<void**>(base + 0xB0);
                auto setTex = reinterpret_cast<SetNodeTexture_t>(
                    managerVtable[kVtblSlot_SetNodeTexture]);
                if (setTex)
                {
                    PrefetchIconTexture(iconHash);
                    if (iconNode1)
                        setTex(manager, iconNode1, iconHash, kIconTextureSlotHash);
                    if (iconNode2)
                        setTex(manager, iconNode2, iconHash, kIconTextureSlotHash);
                }
            }

            if (variantHash == 0) return;

            auto getText  = reinterpret_cast<GetTextByHash_t>(
                managerVtable[0x750 / 8]);
            auto writeFn  = reinterpret_cast<WriteTextField_t>(
                managerVtable[0x708 / 8]);

            if (!getText || !writeFn) return;

            void* text = getText(manager, variantHash);
            if (!text) return;

            writeFn(manager, writeTarget1, writeTarget2, text);

#ifdef _DEBUG
            const std::uint64_t overrideSig =
                (static_cast<std::uint64_t>(variantIdx) << 56)
                ^ (variantHash & 0x00FFFFFFFFFFFFFFull);
            if (ShouldLogRowSig(g_OverrideLogged, selectedId, overrideSig))
                LogDebug("[OutfitListInject:UpdateRecords] cycle-button label "
                    "override: selectedId=%u variantIdx=%u hash=0x%016llX\n",
                    static_cast<unsigned>(selectedId),
                    static_cast<unsigned>(variantIdx),
                    static_cast<unsigned long long>(variantHash));
#endif
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[OutfitListInject:UpdateRecords] SEH writing variant "
                "label (selectedId=%u variantIdx=%u)\n",
                static_cast<unsigned>(selectedId),
                static_cast<unsigned>(variantIdx));
        }
    }

    static bool TryInjectHeadOptionList(void* thisPtr)
    {
        outfit::DrainPendingHeads();
        if (!thisPtr || !g_AddListBandana) return false;

        const auto base = reinterpret_cast<std::uintptr_t>(thisPtr);

        std::uint32_t equipKind = 0;
        __try
        {
            equipKind = *reinterpret_cast<std::uint32_t*>(base + 0x4434);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (equipKind != 0x201) return false;

        const std::uint8_t pt     = outfit::ReadLivePartsType();
        const std::uint8_t livePT = outfit::ReadLivePlayerType();

        const outfit::OutfitEntry* entry = nullptr;
        const std::uint16_t*       headIds = nullptr;
        std::uint8_t               headCount = 0;
        bool                       isVanillaExt = false;
        std::uint16_t              vextHeadIdBuf[outfit::kMaxHeadOptionsPerOutfit] = {};

        if (pt >= outfit::kCustomPartsTypeStart && pt <= outfit::kCustomPartsTypeEnd)
        {
            if (!outfit::TryGetOutfitByPartsType(pt, &entry) || !entry)
                return false;
            const std::uint8_t variant = outfit::GetActiveVariant(pt);
            entry->GetHeadOptionsForVariant(livePT, variant, &headIds, &headCount);
        }
        else if (pt < outfit::kCustomPartsTypeStart)
        {
            if (!outfit::VanillaExtGetHeadOptions(
                    pt, livePT, outfit::ReadLiveSelectorCode(),
                    vextHeadIdBuf,
                    static_cast<std::uint8_t>(outfit::kMaxHeadOptionsPerOutfit),
                    &headCount))
                return false;
            headIds = vextHeadIdBuf;
            isVanillaExt = true;
        }
        else
        {
            return false;
        }
        if (headCount > 0 && !headIds) headCount = 0;


        std::uint32_t count = 0;
        bool listChanged = false;
        __try
        {
            if (*reinterpret_cast<std::uint64_t*>(base + 0x461b8)
                    != 0xb8a0bf169f98ull)
            {
                LogDebug("[OutfitListInject:HeadOption] wrong-object guard: "
                    "base=%p is not the suit panel (+0x461b8 mismatch) -> "
                    "skipped head-marker writes\n", base);
                return false;
            }

            const std::uint32_t origCount =
                *reinterpret_cast<std::uint32_t*>(base + 0x442c);

            std::uint32_t keep = origCount;
            if (!isVanillaExt
                && origCount >= 1
                && *reinterpret_cast<std::uint16_t*>(base + 0x4440)
                       == outfit::kHeadOption_None)
            {
                keep = 1;
            }


            std::uint8_t* markersA =
                reinterpret_cast<std::uint8_t*>(base + 0xbc40);
            std::uint8_t* markersB =
                reinterpret_cast<std::uint8_t*>(base + 0xc040);
            std::uint8_t* markersC =
                reinterpret_cast<std::uint8_t*>(base + 0xc440);
            std::uint8_t* markersD =
                reinterpret_cast<std::uint8_t*>(base + 0xc840);
            for (std::uint32_t i = keep; i < 256; ++i)
            {
                if (markersA[i] == 0
                    && markersB[i] == 0
                    && markersC[i] == 0
                    && markersD[i] == 0)
                {
                    break;
                }
                markersA[i] = 0;
                markersB[i] = 0;
                markersC[i] = 0;
                markersD[i] = 0;
            }

            count = keep;


            const std::uint32_t startCount = count;

            std::uint16_t origAdded[128] = {};
            std::uint8_t  origAddedCount = 0;
            for (std::uint32_t i = 0; i < startCount && origAddedCount < 128; ++i)
            {
                origAdded[origAddedCount++] =
                    *reinterpret_cast<std::uint16_t*>(base + 0x4440 + i * 0x1E);
            }

            auto isAlreadyInList = [&](std::uint16_t equipId) -> bool {
                for (std::uint8_t k = 0; k < origAddedCount; ++k)
                    if (origAdded[k] == equipId) return true;
                return false;
            };

            for (std::uint8_t i = 0;
                 i < headCount
                 && i < outfit::kMaxHeadOptionsPerOutfit;
                 ++i)
            {
                const std::uint16_t equipId = headIds[i];
                if (equipId == 0) continue;
                if (count >= 128) break;
                if (isAlreadyInList(equipId)) continue;


                if (const auto* head =
                        outfit::TryGetCustomHeadByEquipId(equipId))
                {
                    if (head->developId != 0
                        && V_FrameWorkState::IsExplicitlyUndevelopedByDevelopId(
                               head->developId))
                    {
                        continue;
                    }
                }

                const std::uint32_t addedIdx = count;
                g_AddListBandana(thisPtr, &count, equipId);


                if (origAddedCount < 128)
                    origAdded[origAddedCount++] = equipId;

                if (addedIdx < 128)
                {
                    *reinterpret_cast<std::uint8_t*>(base + 0xc840 + addedIdx) = 0xff;
                    *reinterpret_cast<std::uint8_t*>(base + 0x548 + addedIdx * 0xf) = 1;


                    const std::uint64_t cellOff = addedIdx * 0xb4;
                    *reinterpret_cast<std::uint32_t*>(base + 0xcc40 + cellOff) = 0;
                    *reinterpret_cast<std::uint32_t*>(base + 0xcc44 + cellOff) = 0xff;
                }
            }


            if (count != origCount)
            {
                *reinterpret_cast<std::uint32_t*>(base + 0x442c) = count;
                *reinterpret_cast<std::uint32_t*>(base + 0x104)  = count;
                listChanged = true;
            }

#ifdef _DEBUG
            if (!g_HeadOptionInjectFirstFire.exchange(true))
            {
                LogDebug("[OutfitListInject:HeadOption] FIRST INJECT: "
                    "partsType=0x%02X livePT=%u developId=%u "
                    "declaredCount=%u origCount=%u finalCount=%u - "
                    "committed to this[0x442c] and this[0x104]\n",
                    static_cast<unsigned>(pt),
                    static_cast<unsigned>(livePT),
                    static_cast<unsigned>(entry ? entry->developId : 0),
                    static_cast<unsigned>(headCount),
                    static_cast<unsigned>(startCount),
                    static_cast<unsigned>(count));
            }
#endif
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[OutfitListInject:HeadOption] inject faulted; "
                "partsType=0x%02X count-at-fault=%u\n",
                static_cast<unsigned>(pt),
                static_cast<unsigned>(count));
        }
        return listChanged;
    }

    static bool IsHeadOptionList(void* thisPtr)
    {
        if (!thisPtr) return false;
        __try
        {
            return *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(thisPtr) + 0x4434) == 0x201u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static std::uint32_t __fastcall hkHeadBadgeCategory(void* self,
                                                        std::uint32_t equipId)
    {
        const outfit::CustomHeadEntry* byEquip =
            outfit::TryGetCustomHeadByEquipId(static_cast<std::uint16_t>(equipId));
#ifdef _DEBUG
        if (!g_HeadBadgeBuildActive && !g_HeadEquipDecideActive)
        {
            static int s_passive = 0;
            if (s_passive < 12)
            {
                ++s_passive;
                LogDebug("[HeadSummary] GetFaceEquipId passive: arg=0x%X "
                    "isCustomHeadEquipId=%d livePT=0x%02X\n",
                    equipId, byEquip ? 1 : 0,
                    static_cast<unsigned>(outfit::ReadLivePartsType()));
            }
        }
#endif
        if (byEquip)
            return byEquip->slotByte;
        return g_OrigHeadBadgeCategory
            ? g_OrigHeadBadgeCategory(self, equipId) : 0;
    }

    static std::uint8_t __fastcall hkWornHeadCategory(void* self)
    {
        if (g_HeadBadgeBuildActive)
        {
            const std::uint16_t worn = outfit::GetCurrentWornHeadEquipId();
            if (worn)
                if (const auto* head = outfit::TryGetCustomHeadByEquipId(worn))
                    return head->slotByte;
        }
        return g_OrigWornHeadCategory ? g_OrigWornHeadCategory(self) : 0;
    }

    static void ApplyFobAvailabilityToCustomRows(void* thisPtr)
    {
        __try
        {
            auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);
            void* sys  = *reinterpret_cast<void**>(base + 0x70);
            void* ctrl = *reinterpret_cast<void**>(base + 0x58);
            if (!sys || !ctrl) return;

            using CtxFn_t = std::uint8_t (__fastcall*)(void*);
            using FobFn_t = std::uint8_t (__fastcall*)(void*, std::uint16_t);

            auto ctx = reinterpret_cast<CtxFn_t>(
                (*reinterpret_cast<void***>(sys))[kVtblSlot_PrepIsFobSortie]);
            if (!ctx || !ctx(sys)) return;

            auto fob = reinterpret_cast<FobFn_t>(
                (*reinterpret_cast<void***>(ctrl))[kVtblSlot_DevIsFobAvailable]);
            if (!fob) return;

            const std::uint32_t rows =
                *reinterpret_cast<std::uint32_t*>(base + 0x104);
            if (rows == 0) return;
            if (rows > outfit::kPanelRowMax + 1)
            {
                static std::atomic<int> s_rowsOob{ 0 };
                if (s_rowsOob.fetch_add(1, std::memory_order_relaxed) < 4)
                    Log("[OutfitListInject:Fob] the panel reports %u rows, past the "
                        "%u-row window - FOB availability was not applied, so rows "
                        "the game marks unavailable stay selectable\n",
                        rows, outfit::kPanelRowMax + 1);
                return;
            }

            int disabled = 0;
            for (std::uint32_t row = 0; row < rows; ++row)
            {
                std::uint8_t vars = *(base + 0xBC40 + row);
                if (vars == 0)  vars = 1;
                if (vars > 15)  vars = 15;
                for (std::uint8_t var = 0; var < vars; ++var)
                {
                    const std::size_t cell =
                        static_cast<std::size_t>(row) * 15 + var;
                    const std::uint16_t idx = *reinterpret_cast<std::uint16_t*>(
                        base + 0x4440 + cell * 2);
                    if (!EquipDevelopAdd::IsManagedFlowIndex(idx))
                        continue;
                    if (fob(ctrl, idx))
                        continue;
                    if (*(base + 0x548 + cell))
                    {
                        *(base + 0x548 + cell) = 0;
                        ++disabled;
                    }
                }
            }
#ifdef _DEBUG
            if (disabled > 0)
            {
                static int s_n = 0;
                if (s_n < 12)
                {
                    ++s_n;
                    LogDebug("[OutfitListInject] FOB-sortie context: disabled %d "
                        "custom cell(s) via the game's IsFobAvailable\n",
                        disabled);
                }
            }
#endif
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void __fastcall hkSetupPrefabListElement(void* thisPtr)
    {
#ifdef _DEBUG
        {
            static std::uint16_t s_lastCode = 0xFFFF;
            const std::uint16_t code = MissionCodeGuard::GetCurrentMissionCode();
            if (code != s_lastCode)
            {
                s_lastCode = code;
                LogDebug("[OutfitListInject] SetupPrefab: missionCode=%u fobBypass=%d\n",
                    static_cast<unsigned>(code),
                    MissionCodeGuard::ShouldBypassHooks() ? 1 : 0);
            }
        }
#endif
        if (MissionCodeGuard::ShouldBypassHooks())
        {
            const int suppressed = EquipDevelop_BeginFobListSuppress();
            const bool prevFob = t_InsideSetupPrefab;
            t_InsideSetupPrefab = true;
            equip::BeginDevelopVisibilityCache();
            if (g_OrigSetupPrefab) g_OrigSetupPrefab(thisPtr);
            equip::EndDevelopVisibilityCache();
            t_InsideSetupPrefab = prevFob;
            EquipDevelop_EndFobListSuppress();
            ApplyFobAvailabilityToCustomRows(thisPtr);
#ifdef _DEBUG
            static int s_fobLog = 0;
            if (s_fobLog < 8)
            {
                ++s_fobLog;
                LogDebug("[OutfitListInject] FOB mission: list built with %d custom "
                    "develop row(s) suppressed; custom injection skipped\n",
                    suppressed);
            }
#endif
            return;
        }

        const bool prevBadge = g_HeadBadgeBuildActive;
        g_HeadBadgeBuildActive = IsHeadOptionList(thisPtr);

        const bool prev = t_InsideSetupPrefab;
        if (!prev)
        {
            ResetAddedFlowIxBits();
            outfit::ClearOutfitMenuStamps();
            ResetNativeRowCell0();
        }
        t_InsideSetupPrefab = true;
#ifdef _DEBUG
        {
            static int s_beforeOrig = 0;
            if (s_beforeOrig < 24)
            {
                ++s_beforeOrig;
                const ListBuildProbe pr = ReadListBuildProbeSEH(thisPtr);
                LogDebug("[OutfitListInject] SetupPrefab: pre-resets OK, entering "
                         "the game list build (missionCode=%u equipKind=0x%X "
                         "panelSig=0x%llX) - no AddListSuit breadcrumbs after this "
                         "means the game's own build hung; a [BuildWatchdog] block "
                         "follows in ~4s\n",
                    static_cast<unsigned>(MissionCodeGuard::GetCurrentMissionCode()),
                    static_cast<unsigned>(pr.equipKind),
                    static_cast<unsigned long long>(pr.panelSig));
            }
            EnsureBuildWatchdog();
            g_BuildThreadId.store(GetCurrentThreadId());
            g_BuildStartTick.store(GetTickCount());
            g_BuildDumps.store(0);
            g_PrevRip = 0;
            g_PrevRsp = 0;
            g_BuildActive.store(true);
        }
#endif
        equip::BeginDevelopVisibilityCache();
        if (g_OrigSetupPrefab) g_OrigSetupPrefab(thisPtr);
        equip::EndDevelopVisibilityCache();
#ifdef _DEBUG
        g_BuildActive.store(false);
#endif
        t_InsideSetupPrefab = prev;
#ifdef _DEBUG
        {
            static int s_afterOrig = 0;
            if (s_afterOrig < 24)
            {
                ++s_afterOrig;
                LogDebug("[OutfitListInject] SetupPrefab orig returned "
                         "(missionCode=%u) - entering custom injection; a freeze "
                         "before this line is the game's build, after it is our "
                         "injection\n",
                    static_cast<unsigned>(MissionCodeGuard::GetCurrentMissionCode()));
            }
        }
#endif

        if (!prev && g_VariantInjectEnabled && !g_HeadBadgeBuildActive)
        {
            __try
            {
                auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);
                const std::uint32_t rowCount =
                    *reinterpret_cast<std::uint32_t*>(base + 0x442c);
                if (*reinterpret_cast<std::uint64_t*>(base + 0x461b8)
                        == 0xb8a0bf169f98ull
                    && rowCount != 0 && rowCount <= 0x40)
                {
                    const std::uint8_t livePT = outfit::ReadLivePlayerType();
                    const bool fobSortie = IsPanelFobSortie(base);
#ifdef _DEBUG
                    if (fobSortie)
                    {
                        static int s_fobVextLog = 0;
                        if (s_fobVextLog < 8)
                        {
                            ++s_fobVextLog;
                            LogDebug("[OutfitListInject:vext] FOB-sortie context: "
                                     "vext variant cells not injected\n");
                        }
                    }
#endif
                    for (std::uint32_t row = 0;
                         !fobSortie && row < rowCount; ++row)
                    {
                        const std::size_t row0CellByte =
                            0xCC40 + (static_cast<std::size_t>(row) * 15) * 12;
                        const std::uint32_t colorCode0 =
                            *reinterpret_cast<std::uint32_t*>(base + row0CellByte);
                        const std::uint8_t rawCamo =
                            static_cast<std::uint8_t>(colorCode0 & 0xFF);
                        std::uint8_t rowCamo = rawCamo;
                        if (rowCamo >= outfit::kCustomSelectorStart)
                        {
                            std::uint8_t rvpt = 0, rvidx = 0;
                            if (outfit::TryGetVanillaExtByVariantSelector(
                                    rowCamo, &rvpt, &rvidx))
                                rowCamo = outfit::VanillaExtGetVariantSourceCamo(
                                              rvpt, rvidx);
                        }
                        const std::uint8_t vpt =
                            outfit::ResolveVanillaPartsTypeForCamo(rowCamo);
                        if (vpt == 0xFF) continue;
                        const std::uint8_t slotCount =
                            outfit::VanillaExtVariantSlotCount(vpt);
                        if (slotCount == 0) continue;
                        const std::uint8_t nativeCount = *(base + 0xBC40 + row);
                        if (nativeCount == 0 || nativeCount >= 15) continue;

                        const std::uint16_t flowIndex =
                            *reinterpret_cast<std::uint16_t*>(
                                base + 0x4440
                                + (static_cast<std::size_t>(row) * 15) * 2);

                        if (rawCamo >= outfit::kCustomSelectorStart)
                            *reinterpret_cast<std::uint32_t*>(base + row0CellByte) =
                                (colorCode0 & 0xFFFFFF00u) | rowCamo;

                        if (flowIndex < equip::kMaxFlowSlots)
                            for (std::uint8_t p = 0; p < 15; ++p)
                                g_VextCellMap[flowIndex][p] = VextCellInfo{};

                        const std::uint8_t activeVar = outfit::GetActiveVariant(vpt);
                        const std::uint8_t liveDonor =
                            outfit::VanillaExtResolveVariantDonor(vpt, livePT);
                        std::uint8_t appended   = nativeCount;
                        int          activeCell = -1;
                        for (std::uint8_t v = 1;
                             v <= slotCount && appended < 15; ++v)
                        {
                            if (outfit::VanillaExtGetVariantSourceCamo(vpt, v)
                                    != rowCamo)
                                continue;
                            const outfit::VanillaSuitVariantAsset* vasset =
                                (liveDonor == 0xFF)
                                ? nullptr
                                : outfit::VanillaExtGetVariant(vpt, liveDonor, v);
                            if (!vasset) continue;
                            const std::uint8_t sel =
                                outfit::VanillaExtGetVariantSelector(vpt, v);
                            if (sel == 0) continue;

                            const std::size_t cellIndex =
                                static_cast<std::size_t>(row) * 15 + appended;
                            *reinterpret_cast<std::uint16_t*>(
                                base + 0x4440 + cellIndex * 2) = flowIndex;

                            std::uint8_t* cell = base + 0xCC40 + cellIndex * 12;
                            *reinterpret_cast<std::uint32_t*>(cell + 0) = rowCamo;
                            *reinterpret_cast<std::uint32_t*>(cell + 4) = 0;
                            *(cell + 8) = 0;

                            const bool activeThis = (activeVar == v);
                            *(base + 0x548 + cellIndex) =
                                activeThis ? std::uint8_t{0} : std::uint8_t{1};
                            *(base + 0x425a4 + cellIndex) = activeThis ? 1 : 0;
                            if (activeThis) activeCell = static_cast<int>(appended);
                            StoreVextCellLabel(flowIndex,
                                               static_cast<std::uint8_t>(appended),
                                               sel, vasset->displayNameHash);
                            SeedVextSwatchFlow(sel, rowCamo);
                            ++appended;
                        }

                        if (appended == nativeCount) continue;

                        *(base + 0xBC40 + row) = appended;
                        if (activeVar != 0)
                            for (std::uint8_t nc = 0; nc < nativeCount; ++nc)
                            {
                                *(base + 0x425a4
                                  + static_cast<std::size_t>(row) * 15 + nc) = 0;
                                *(base + 0x548
                                  + static_cast<std::size_t>(row) * 15 + nc) = 1;
                            }
                        if (activeCell >= 0)
                            *(base + 0xC040 + row) =
                                static_cast<std::uint8_t>(activeCell);

                        LogDebug("[OutfitListInject:vext] post-setup row=%u "
                                 "flowIndex=%u rowCamo=0x%02X vpt=0x%02X "
                                 "nativeCount=%u -> total=%u activeVar=%u\n",
                                 static_cast<unsigned>(row),
                                 static_cast<unsigned>(flowIndex),
                                 static_cast<unsigned>(rowCamo),
                                 static_cast<unsigned>(vpt),
                                 static_cast<unsigned>(nativeCount),
                                 static_cast<unsigned>(appended),
                                 static_cast<unsigned>(activeVar));
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[OutfitListInject:PostSetupPrefab] SEH during vext pass\n");
            }
        }

        bool headRowsChanged = false;
        if (!prev && g_HeadOptionInjectEnabled)
        {
            if (IsPanelFobSortie(reinterpret_cast<std::uint8_t*>(thisPtr)))
            {
#ifdef _DEBUG
                static int s_fobHeadLog = 0;
                if (s_fobHeadLog < 8)
                {
                    ++s_fobHeadLog;
                    LogDebug("[OutfitListInject:HeadOption] FOB-sortie context: "
                             "custom head options not injected\n");
                }
#endif
            }
            else
            {
                headRowsChanged = TryInjectHeadOptionList(thisPtr);
            }
        }

        if (!prev && g_HeadBadgeBuildActive)
        {
            __try
            {
                auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);
                if (*reinterpret_cast<std::uint64_t*>(base + 0x461b8)
                        == 0xb8a0bf169f98ull)
                {
                    auto validSlot = [](std::uint8_t s) {
                        return s >= outfit::kCustomHeadSlotBase
                            && outfit::IsCustomHeadSlot(s);
                    };
                    const std::uint8_t srcTracker = outfit::GetWornCustomHeadSlot();
                    const std::uint16_t srcCat = outfit::ReadLiveWornHeadCategory();
                    const std::uint8_t srcHeadSlot = outfit::ReadLiveHeadSlot();
                    std::uint8_t wornSlot = 0;
                    if (validSlot(srcTracker))
                        wornSlot = srcTracker;
                    else if (validSlot(static_cast<std::uint8_t>(srcCat)))
                        wornSlot = static_cast<std::uint8_t>(srcCat);
                    else if (validSlot(srcHeadSlot))
                        wornSlot = srcHeadSlot;
                    const outfit::CustomHeadEntry* worn =
                        validSlot(wornSlot)
                            ? outfit::TryGetCustomHeadBySlot(wornSlot)
                            : nullptr;

                    std::uint16_t wornEquipId = worn ? worn->equipId : 0;
                    if (wornEquipId == 0)
                    {
                        const std::uint16_t wid =
                            outfit::GetCurrentWornHeadEquipId();
                        if (wid != 0 && outfit::TryGetCustomHeadByEquipId(wid))
                            wornEquipId = wid;
                    }

                    if (wornEquipId == 0)
                    {
                        std::uint8_t vslot = 0;
                        if (srcCat >= 1 && srcCat <= 5)
                            vslot = static_cast<std::uint8_t>(srcCat);
                        else if (srcHeadSlot >= 1 && srcHeadSlot <= 5)
                            vslot = srcHeadSlot;
                        if (vslot != 0)
                            wornEquipId =
                                static_cast<std::uint16_t>(0x20D + vslot);
                    }
#ifdef _DEBUG
                    LogDebug("[OutfitListInject:HeadCursor] worn-head sources: "
                        "tracker=0x%02X state[0xFE]=0x%02X state[0xFA]=0x%02X "
                        "wornEquipTracker=0x%X -> equipId=0x%X "
                        "(rowsChanged=%d)\n",
                        static_cast<unsigned>(srcTracker),
                        static_cast<unsigned>(srcCat),
                        static_cast<unsigned>(srcHeadSlot),
                        static_cast<unsigned>(outfit::GetCurrentWornHeadEquipId()),
                        static_cast<unsigned>(wornEquipId),
                        headRowsChanged ? 1 : 0);
#endif
                    const std::uint32_t rowCount =
                        *reinterpret_cast<std::uint32_t*>(base + 0x442c);

                    int targetRow = -1;
                    if (wornEquipId != 0)
                    {
                        for (std::uint32_t r = 0; r < rowCount && r < 64; ++r)
                            if (*reinterpret_cast<std::uint16_t*>(
                                    base + 0x4440 + r * 0x1e) == wornEquipId)
                            { targetRow = static_cast<int>(r); break; }
#ifdef _DEBUG
                        if (targetRow < 0)
                            LogDebug("[OutfitListInject:HeadCursor] equipId=0x%X NOT "
                                "found in %u rows (0x4440 stride 0x1e)\n",
                                static_cast<unsigned>(wornEquipId), rowCount);
#endif
                    }

                    if (targetRow >= 0)
                    {
                        for (std::uint32_t r = 0; r < rowCount && r < 64; ++r)
                            *(base + 0x425a4 + r * 0xf) =
                                (static_cast<int>(r) == targetRow) ? 1 : 0;
                    }

                    if (targetRow >= 0 || headRowsChanged)
                    {
                        if (void* scan = ResolveGameAddress(
                                gAddr.MissionPrep_SetInitialSelectRecord))
                            reinterpret_cast<void(__fastcall*)(
                                void*, std::uint32_t)>(scan)(thisPtr, rowCount);
#ifdef _DEBUG
                        LogDebug("[OutfitListInject:HeadCursor] cursor pass re-run: "
                            "targetRow=%d rowCount=%u\n", targetRow, rowCount);
#endif
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (!prev && !g_HeadBadgeBuildActive)
        {
            __try
            {
                auto* base = reinterpret_cast<std::uint8_t*>(thisPtr);
                const std::uint8_t pt  = outfit::ReadLivePartsType();
                const std::uint8_t ppt = outfit::ReadLivePlayerType();
                const outfit::OutfitEntry* entry = nullptr;
                if (*reinterpret_cast<std::uint64_t*>(base + 0x461b8)
                        == 0xb8a0bf169f98ull
                    && pt >= outfit::kCustomPartsTypeStart
                    && pt <= outfit::kCustomPartsTypeEnd
                    && outfit::TryGetOutfitByPartsType(pt, &entry) && entry)
                {
                    const std::uint32_t scanRows = std::min<std::uint32_t>(
                        *reinterpret_cast<std::uint32_t*>(base + 0x442c),
                        outfit::kPanelRowMax + 1u);

                    int row = -1;
                    for (std::uint32_t r = 0; r < scanRows && row < 0; ++r)
                        if (*reinterpret_cast<std::uint16_t*>(
                                base + 0x4440
                                + (static_cast<std::size_t>(r) * 15) * 2)
                            == entry->flowIndex)
                            row = static_cast<int>(r);

                    const std::uint8_t vcount = (ppt != 0xFF)
                        ? entry->GetVariantCountFor(ppt) : entry->variantCount;
                    if (vcount >= 2)
                    {
                        const std::uint8_t worn =
                            outfit::GetActiveVariant(entry->partsType);
                        if (row >= 0)
                        {
                            for (std::uint8_t v = 0; v < vcount && v < 15; ++v)
                            {
                                const std::size_t c =
                                    static_cast<std::size_t>(row) * 15 + v;
                                const bool on = (v == worn);
                                *(base + 0x425a4 + c) = on ? 1 : 0;
                                *(base + 0x548   + c) = on ? 0 : 1;
                            }
                        }
                    }
                    else if (row >= 0)
                    {
                        const std::size_t c = static_cast<std::size_t>(row) * 15;
                        *(base + 0x425a4 + c) = 1;
                        *(base + 0x548   + c) = 0;
                    }

                    if (void* scan = ResolveGameAddress(
                            gAddr.MissionPrep_SetInitialSelectRecord))
                    {
                        const std::uint32_t rowCount =
                            *reinterpret_cast<std::uint32_t*>(base + 0x442c);
                        reinterpret_cast<void(__fastcall*)(void*, std::uint32_t)>(
                            scan)(thisPtr, rowCount);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        g_HeadBadgeBuildActive = prevBadge;

        if (!prev)
            ApplyFobAvailabilityToCustomRows(thisPtr);
    }
}

namespace outfit
{

    void SetHeadEquipDecideActive(bool active)
    {
        g_HeadEquipDecideActive = active;
    }

    std::uint8_t VextLookupCellSelector(std::uint16_t flowIndex,
                                        std::uint8_t cellPos)
    {
        if (flowIndex >= equip::kMaxFlowSlots || cellPos >= 15) return 0;
        const VextCellInfo& e = g_VextCellMap[flowIndex][cellPos];
        return e.used ? e.selector : 0;
    }



    bool Install_OutfitListInject_Hook()
    {
        if (g_Installed) return true;

        void* target = ResolveGameAddress(
            gAddr.ItemSelectorCallbackImpl_SetupPrefabListElement);
        if (!target)
        {
            LogDebug("[OutfitListInject] target unresolved; module disabled\n");
            return false;
        }


        g_AddListSuitAddr = ResolveGameAddress(gAddr.AddListSuit);
        bool addListSuitHooked = false;
        if (g_AddListSuitAddr)
        {
            addListSuitHooked = CreateAndEnableHook(
                g_AddListSuitAddr,
                reinterpret_cast<void*>(&hkAddListSuit),
                reinterpret_cast<void**>(&g_OrigAddListSuit));
        }

        const bool setupHooked = CreateAndEnableHook(
            target,
            reinterpret_cast<void*>(&hkSetupPrefabListElement),
            reinterpret_cast<void**>(&g_OrigSetupPrefab));
        g_Installed = setupHooked;


        if (void* addBandanaAddr = ResolveGameAddress(
                gAddr.ItemSelector_AddListBandana))
        {
            g_AddListBandana =
                reinterpret_cast<AddListBandana_t>(addBandanaAddr);
#ifdef _DEBUG
            LogDebug("[OutfitListInject:HeadOption] AddListBandana resolved: %p - "
                     "HEAD OPTION (equipKind=0x201) injection enabled for outfits "
                     "with HasHeadOptions()\n",
                addBandanaAddr);
#endif
        }
        else
        {
            LogDebug("[OutfitListInject:HeadOption] AddListBandana unresolved - "
                     "HEAD OPTION submenu not injected for custom outfits (JP "
                     "build?)\n");
        }


        if (void* tCat = ResolveGameAddress(gAddr.EquipDevCtrl_GetHeadBadgeCategory))
        {
            g_InstalledHeadBadgeCategory = CreateAndEnableHook(
                tCat,
                reinterpret_cast<void*>(&hkHeadBadgeCategory),
                reinterpret_cast<void**>(&g_OrigHeadBadgeCategory));
        }
        if (void* tWorn = ResolveGameAddress(gAddr.MissionPrep_GetWornHeadCategory))
        {
            g_InstalledWornHeadCategory = CreateAndEnableHook(
                tWorn,
                reinterpret_cast<void*>(&hkWornHeadCategory),
                reinterpret_cast<void**>(&g_OrigWornHeadCategory));
        }
#ifdef _DEBUG
        LogDebug("[OutfitListInject:HeadBadge] category-feed=%s worn-feed=%s\n",
            g_InstalledHeadBadgeCategory ? "OK" : "skip",
            g_InstalledWornHeadCategory  ? "OK" : "skip");
#endif




        if (void* arTarget = ResolveGameAddress(
                gAddr.ItemSelectorCallbackImpl_AddRecord))
        {
            g_InstalledAddRecord = CreateAndEnableHook(
                arTarget,
                reinterpret_cast<void*>(&hkAddRecord),
                reinterpret_cast<void**>(&g_OrigAddRecord));
            if (!g_InstalledAddRecord)
                Log("[OutfitListInject] AddRecord hook failed - custom rows keep "
                    "the engine's EQUIPPED mark, which matches on a loadout "
                    "equip-id the custom rows share, so the tag lands on the wrong "
                    "rows or none\n");
        }
        else
        {
            LogDebug("[OutfitListInject] AddRecord target unresolved - custom rows "
                     "keep the engine's EQUIPPED mark, which matches on a shared "
                     "loadout equip-id, so the tag lands on the wrong rows or "
                     "none\n");
        }

        if (void* urTarget = ResolveGameAddress(
                gAddr.ItemSelectorRecordCallFunc_UpdateRecords))
        {
            g_InstalledUpdateRecords = CreateAndEnableHook(
                urTarget,
                reinterpret_cast<void*>(&hkUpdateRecords),
                reinterpret_cast<void**>(&g_OrigUpdateRecords));
#ifdef _DEBUG
            Log("[OutfitListInject] UpdateRecords installed: %s "
                "(target=%p)\n",
                g_InstalledUpdateRecords ? "OK" : "FAIL", urTarget);
#endif
        }
        else
        {
            LogDebug("[OutfitListInject] UpdateRecords target unresolved (JP "
                     "build?) - variant cycle-button labels fall back to the "
                     "vanilla mapping\n");
        }

#ifdef _DEBUG
        Log("[OutfitListInject] installed: setup=%s addListSuit=%s "
            "(target=%p addListSuitAddr=%p)\n",
            setupHooked ? "OK" : "FAIL",
            addListSuitHooked ? "OK" : (g_AddListSuitAddr ? "FAIL" : "UNRESOLVED"),
            target, g_AddListSuitAddr);
#endif
        return g_Installed;
    }

    void Uninstall_OutfitListInject_Hook()
    {
        if (!g_Installed) return;



        if (g_AddListSuitAddr) DisableAndRemoveHook(g_AddListSuitAddr);
        g_AddListSuitAddr = nullptr;
        g_OrigAddListSuit = nullptr;

        if (g_InstalledUpdateRecords)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.ItemSelectorRecordCallFunc_UpdateRecords))
                DisableAndRemoveHook(t);
            g_OrigUpdateRecords      = nullptr;
            g_InstalledUpdateRecords = false;
        }

        if (g_InstalledAddRecord)
        {
            if (void* t = ResolveGameAddress(
                    gAddr.ItemSelectorCallbackImpl_AddRecord))
                DisableAndRemoveHook(t);
            g_OrigAddRecord      = nullptr;
            g_InstalledAddRecord = false;
        }

        if (g_InstalledHeadBadgeCategory)
        {
            if (void* t = ResolveGameAddress(gAddr.EquipDevCtrl_GetHeadBadgeCategory))
                DisableAndRemoveHook(t);
            g_OrigHeadBadgeCategory      = nullptr;
            g_InstalledHeadBadgeCategory = false;
        }
        if (g_InstalledWornHeadCategory)
        {
            if (void* t = ResolveGameAddress(gAddr.MissionPrep_GetWornHeadCategory))
                DisableAndRemoveHook(t);
            g_OrigWornHeadCategory      = nullptr;
            g_InstalledWornHeadCategory = false;
        }
        g_HeadBadgeBuildActive = false;

        if (void* t = ResolveGameAddress(
                gAddr.ItemSelectorCallbackImpl_SetupPrefabListElement))
            DisableAndRemoveHook(t);

        g_OrigSetupPrefab  = nullptr;
        g_Installed        = false;

#ifdef _DEBUG
        LogDebug("[OutfitListInject] removed\n");
#endif
    }
}
