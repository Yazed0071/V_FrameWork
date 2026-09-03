#include "pch.h"

#include <Windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "DevelopArrayGrow.h"
#include "EquipDevelop_SetEquipUndeveloped.h"
#include "../outfit/EquipDevelopControllerImpl_GetSuitDevelopInfoIndex.h"
#include "V_FrameWorkState.h"
#include "log.h"

namespace equip
{
    namespace
    {
        constexpr std::uint32_t kOldRows        = 1024;
        constexpr std::uint32_t kNewRows        = kMaxFlowSlots;
        constexpr std::uint32_t kRecStride      = 0x68;
        constexpr std::uint32_t kParStride      = 0x10;
        constexpr std::uint32_t kOldAllocSize   = 0x1FFD0;
        constexpr std::uint32_t kOldCtlOff      = 0x1E028;
        constexpr std::uint32_t kNewCtlOff      = 0x700000;
        constexpr std::uint32_t kCtlSize        = kOldAllocSize - kOldCtlOff;
        constexpr std::uint32_t kCtlDelta       = kNewCtlOff - kOldCtlOff;
        constexpr std::uint32_t kNewParallelOff =
            (kNewCtlOff + kCtlSize + 0xF) & ~0xFu;
        constexpr std::uint32_t kNewAllocSize   =
            kNewParallelOff + kNewRows * kParStride;
        constexpr std::uint32_t kNewRecMemset   = kNewRows * kRecStride;
        constexpr std::uint32_t kNewParMemset   = kNewRows * kParStride;
        constexpr std::uint32_t kNewParDisp20   = kNewParallelOff - 0x20;
        constexpr std::uint32_t kWorkBufDisp    = 0x1FEC0;
        constexpr std::uint32_t kNullSinkOff    = kNewCtlOff + kCtlSize;

        static_assert(0x28 + kNewRows * kRecStride <= kNewCtlOff,
                      "records must not overrun the relocated control fields");
        static_assert(kNullSinkOff + 8 <= kNewParallelOff,
                      "the always-zero qword must fit between the control fields "
                      "and the parallel array");
        static_assert((kNullSinkOff & 7) == 0,
                      "the always-zero qword must be 8-byte aligned");
        static_assert(kNewCtlOff + kCtlSize <= kNewParallelOff,
                      "control fields must not overrun the parallel array");

        struct ImmPatch
        {
            std::uintptr_t addr;
            const char*    what;
            std::uint8_t   opLen;
            std::uint8_t   op[5];
            std::uint32_t  oldImm;
            std::uint32_t  newImm;
        };

        const ImmPatch kPatchTemplate[] = {
            { 0, "block alloc size",     1, { 0xB9 },                         0x1FFD0, kNewAllocSize },
            { 0, "ctor loop count",      1, { 0xB9 },                         0x3FF,   kNewRows - 1 },
            { 0, "ctor parallel base",   3, { 0x48, 0x8D, 0x83 },             0x1A028, kNewParallelOff },
            { 0, "reset records size",   2, { 0x41, 0xB8 },                   0x1A000, kNewRecMemset },
            { 0, "reset parallel base",  3, { 0x48, 0x8D, 0x8B },             0x1A028, kNewParallelOff },
            { 0, "reset parallel size",  2, { 0x41, 0xB8 },                   0x4000,  kNewParMemset },
            { 0, "regist scan bound",    1, { 0xB9 },                         0x400,   kNewRows },
            { 0, "regist probe disp",    5, { 0x66, 0x43, 0x83, 0xBC, 0xC1 }, 0x1A008, kNewParDisp20 },
            { 0, "regist store disp+0",  4, { 0x41, 0x89, 0x84, 0xC9 },       0x1A008, kNewParDisp20 },
            { 0, "regist store disp+4",  4, { 0x41, 0x89, 0x84, 0xC9 },       0x1A00C, kNewParDisp20 + 4 },
            { 0, "regist store disp+8",  4, { 0x41, 0x89, 0x84, 0xC9 },       0x1A010, kNewParDisp20 + 8 },
            { 0, "regist store disp+C",  4, { 0x41, 0x89, 0x84, 0xC9 },       0x1A014, kNewParDisp20 + 12 },

            { 0, "equipid-from-idx bound",   1, { 0xB8 },       0x400, kNewRows },
            { 0, "is-visible bound",         1, { 0xB8 },       0x400, kNewRows },
            { 0, "grade getter bound",       1, { 0xB8 },       0x400, kNewRows },
            { 0, "packed getter bound",      1, { 0xB8 },       0x400, kNewRows },
            { 0, "count-by-type scan bound", 2, { 0x41, 0xB8 }, 0x400, kNewRows },
            { 0, "provider count bound",     2, { 0x41, 0xBF }, 0x400, kNewRows },
            { 0, "provider fill bound",      2, { 0x41, 0xBD }, 0x400, kNewRows },
            { 0, "menu list gate bound",     1, { 0xB8 },       0x400, kNewRows },
        };

        constexpr std::size_t kPatchCount =
            sizeof(kPatchTemplate) / sizeof(kPatchTemplate[0]);

        ImmPatch kPatches[kPatchCount] = {};

        struct ByteFix
        {
            std::uintptr_t addr;
            const char*    what;
            std::uint8_t   len;
            std::uint8_t   oldBytes[4];
            std::uint8_t   newBytes[4];
        };

        const ByteFix kSiblingLoopFixTemplate[] = {
            { 0, "sibling retire loop index init", 3,
              { 0x40, 0x32, 0xF6 },       { 0x33, 0xF6, 0x90, 0x00 } },
            { 0, "sibling retire loop index read", 4,
              { 0x40, 0x0F, 0xB6, 0xC6 }, { 0x8B, 0xC6, 0x90, 0x90 } },
            { 0, "sibling retire loop index inc",  3,
              { 0x40, 0xFE, 0xC6 },       { 0xFF, 0xC6, 0x90, 0x00 } },
            { 0, "sibling retire loop index test", 4,
              { 0x40, 0x0F, 0xB6, 0xC6 }, { 0x8B, 0xC6, 0x90, 0x90 } },
        };

        constexpr std::size_t kSiblingLoopFixCount =
            sizeof(kSiblingLoopFixTemplate)
            / sizeof(kSiblingLoopFixTemplate[0]);

        ByteFix kSiblingLoopFixes[kSiblingLoopFixCount] = {};

        constexpr std::uint16_t kSiblingListCap = 1024;

        using SiblingCount_t =
            std::uint16_t (__fastcall*)(void*, std::uint16_t, char);
        SiblingCount_t g_OrigSiblingCount = nullptr;
        bool           g_ClampInstalled   = false;

        using IsVisible_t = bool (__fastcall*)(void*, std::uint16_t);
        IsVisible_t   g_OrigIsVisible      = nullptr;
        bool          g_IsVisibleInstalled = false;

        bool          g_VisArmed  = false;
        bool          g_VisValid  = false;
        std::uint64_t g_VisStamp  = 0;

        constexpr std::uint64_t kVisTtlMs = 250;
        void*         g_VisSelf   = nullptr;
        DWORD         g_VisThread = 0;
        std::uint64_t g_VisCalls  = 0;
        std::uint64_t g_VisHits   = 0;
        std::uint8_t  g_VisCache[kNewRows] = {};

        std::uint64_t g_VisWinCalls = 0;
        std::uint64_t g_VisWinHits  = 0;

        std::uint64_t g_SibCalls = 0;
        std::uint64_t g_SibSum   = 0;
        std::uint32_t g_SibMax   = 0;
        LARGE_INTEGER g_VisT0    = {};

        using CaptureBackTrace_t =
            USHORT(NTAPI*)(ULONG, ULONG, PVOID*, PULONG);
        CaptureBackTrace_t g_CaptureBackTrace = nullptr;
        bool               g_CaptureResolved  = false;

        void LogSiblingBacktrace(unsigned long long nth)
        {
            if (!g_CaptureResolved)
            {
                g_CaptureResolved = true;
                if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
                    g_CaptureBackTrace = reinterpret_cast<CaptureBackTrace_t>(
                        GetProcAddress(nt, "RtlCaptureStackBackTrace"));
            }
            if (!g_CaptureBackTrace)
            {
                LogDebug("[DevelopArrayGrow] RtlCaptureStackBackTrace unresolved - "
                         "develop-scan caller unidentified\n");
                return;
            }

            PVOID frames[40] = {};
            const USHORT n = g_CaptureBackTrace(1, 40, frames, nullptr);

            char line[1200];
            int  len = sprintf_s(line, sizeof(line),
                                 "[DevelopArrayGrow] develop-scan caller chain at "
                                 "sibling-count call #%llu (unwind-accurate, "
                                 "innermost first):", nth);
            for (USHORT i = 0; i < n && len > 0 && len < 1050; ++i)
            {
                const std::uintptr_t a =
                    reinterpret_cast<std::uintptr_t>(frames[i]);
                if (a >= 0x140000000ull && a < 0x142E00000ull)
                {
                    const int w = sprintf_s(line + len, sizeof(line) - len,
                                            " 0x%llX",
                                            static_cast<unsigned long long>(a));
                    if (w <= 0) break;
                    len += w;
                }
            }
            Log("%s\n", line);
        }

        struct CtlDispSite
        {
            std::uintptr_t addr;
            std::uint32_t  oldDisp;
            std::uint8_t   dispOff;
            std::uint8_t   maxOff;
        };

        struct BoundImmSite
        {
            std::uintptr_t addr;
            std::uint8_t   immOff;
        };

        namespace tbl_en154
        {
        static const CtlDispSite kCtlDispSites[] = {
            { 0x140F64993ull, 0x1E028u, 255, 3 },
            { 0x140F6499Aull, 0x1E030u, 255, 3 },
            { 0x140F649A1ull, 0x1E038u, 255, 3 },
            { 0x140F649C3ull, 0x1FE58u, 255, 3 },
            { 0x140F649CAull, 0x1FE60u, 255, 3 },
            { 0x140F649D1ull, 0x1FE68u, 255, 3 },
            { 0x140F649D8ull, 0x1FE70u, 255, 3 },
            { 0x140F649DFull, 0x1FE78u, 255, 3 },
            { 0x140F649E6ull, 0x1FE80u, 255, 3 },
            { 0x140F649EDull, 0x1FE88u, 255, 3 },
            { 0x140F649F4ull, 0x1FE90u, 255, 3 },
            { 0x140F649FBull, 0x1FE98u, 255, 3 },
            { 0x140F64A02ull, 0x1FEA0u, 255, 3 },
            { 0x140F64A09ull, 0x1FEA8u, 255, 3 },
            { 0x140F64A10ull, 0x1FEB0u, 255, 3 },
            { 0x140F64A17ull, 0x1FEB8u, 255, 3 },
            { 0x140F64A1Eull, 0x1FEC0u, 255, 3 },
            { 0x140F64A25ull, 0x1FEC8u, 255, 3 },
            { 0x140F64A2Cull, 0x1FFC5u, 255, 4 },
            { 0x140F64A34ull, 0x1FFC7u, 255, 3 },
            { 0x140F64A3Bull, 0x1FFC8u, 255, 3 },
            { 0x140F64A4Aull, 0x1E038u, 255, 3 },
            { 0x140F64A56ull, 0x1FED0u, 255, 3 },
            { 0x140F64A6Cull, 0x1FF50u, 255, 3 },
            { 0x140F64A73ull, 0x1FF58u, 255, 3 },
            { 0x140F64A7Aull, 0x1FF60u, 255, 3 },
            { 0x140F64A81ull, 0x1FF68u, 255, 3 },
            { 0x140F64A88ull, 0x1FF70u, 255, 3 },
            { 0x140F64A8Full, 0x1FF78u, 255, 3 },
            { 0x140F64A96ull, 0x1FF80u, 255, 3 },
            { 0x140F64A9Dull, 0x1FF88u, 255, 3 },
            { 0x140F64AA4ull, 0x1FF90u, 255, 3 },
            { 0x140F64AABull, 0x1FF98u, 2, 2 },
            { 0x140F64AB1ull, 0x1FF9Cu, 255, 3 },
            { 0x140F64AC2ull, 0x1FF9Eu, 255, 3 },
            { 0x140F64AC9ull, 0x1FFA6u, 255, 3 },
            { 0x140F64AD0ull, 0x1FFAEu, 255, 3 },
            { 0x140F64AD7ull, 0x1FFB6u, 255, 3 },
            { 0x140F64ADEull, 0x1FFBEu, 2, 2 },
            { 0x140F64AE4ull, 0x1FFC2u, 255, 3 },
            { 0x140F64AEBull, 0x1FFC4u, 2, 2 },
            { 0x140F64B11ull, 0x1FEC0u, 255, 3 },
            { 0x140F64B7Bull, 0x1FEC0u, 255, 3 },
            { 0x140F64BF2ull, 0x1FEC0u, 255, 3 },
            { 0x140F64C92ull, 0x1FEC0u, 255, 3 },
            { 0x140F64E10ull, 0x1FFA8u, 255, 3 },
            { 0x140F64E20ull, 0x1E018u, 255, 3 },
            { 0x140F64E6Cull, 0x1E008u, 255, 3 },
            { 0x140F64E81ull, 0x1FE90u, 255, 3 },
            { 0x140F64EC0ull, 0x1FE78u, 255, 3 },
            { 0x140F64EE0ull, 0x1FE78u, 255, 3 },
            { 0x140F64EF0ull, 0x1FE88u, 255, 3 },
            { 0x140F64EF7ull, 0x1FEB0u, 255, 3 },
            { 0x140F64F13ull, 0x1FE90u, 255, 3 },
            { 0x140F64F26ull, 0x1FE90u, 255, 3 },
            { 0x140F64F3Full, 0x1FE90u, 255, 3 },
            { 0x140F64F46ull, 0x1FEB0u, 255, 3 },
            { 0x140F64F64ull, 0x1E008u, 255, 3 },
            { 0x140F6502Cull, 0x1E010u, 255, 3 },
            { 0x140F6532Eull, 0x1E008u, 255, 3 },
            { 0x140F653A5ull, 0x1E028u, 255, 3 },
            { 0x140F65487ull, 0x1E028u, 255, 3 },
            { 0x140F6551Aull, 0x1E028u, 255, 3 },
            { 0x140F65675ull, 0x1E008u, 255, 3 },
            { 0x140F65711ull, 0x1E008u, 255, 3 },
            { 0x140F65783ull, 0x1E008u, 255, 3 },
            { 0x140F65835ull, 0x1E008u, 255, 3 },
            { 0x140F658D5ull, 0x1E008u, 255, 3 },
            { 0x140F65982ull, 0x1E008u, 255, 3 },
            { 0x140F659E1ull, 0x1E008u, 255, 3 },
            { 0x140F661C0ull, 0x1E030u, 255, 3 },
            { 0x140F66200ull, 0x1E030u, 255, 3 },
            { 0x140F66298ull, 0x1FE60u, 255, 4 },
            { 0x140F662B3ull, 0x1FE60u, 255, 3 },
            { 0x140F66DD2ull, 0x1E008u, 255, 3 },
            { 0x140F66E13ull, 0x1E008u, 255, 3 },
            { 0x140F66E76ull, 0x1E008u, 255, 3 },
            { 0x140F66F3Full, 0x1E008u, 255, 3 },
            { 0x140F671B0ull, 0x1FE50u, 255, 3 },
            { 0x140F671E5ull, 0x1FE50u, 255, 3 },
            { 0x140F67213ull, 0x1FE48u, 255, 3 },
            { 0x140F6724Dull, 0x1FE58u, 255, 3 },
            { 0x140F674BAull, 0x1FE40u, 255, 3 },
            { 0x140F674E0ull, 0x1FE40u, 255, 3 },
            { 0x140F6750Cull, 0x1FE48u, 255, 3 },
            { 0x140F67556ull, 0x1FE48u, 255, 3 },
            { 0x140F67566ull, 0x1FE48u, 255, 3 },
            { 0x140F67576ull, 0x1FE48u, 255, 3 },
            { 0x140F67586ull, 0x1FE48u, 255, 3 },
            { 0x140F67596ull, 0x1FE48u, 255, 3 },
            { 0x140F675A6ull, 0x1FE48u, 255, 3 },
            { 0x140F675ECull, 0x1FE48u, 255, 3 },
            { 0x140F675FCull, 0x1FE48u, 255, 3 },
            { 0x140F6760Cull, 0x1FE48u, 255, 3 },
            { 0x140F6761Cull, 0x1FE48u, 255, 3 },
            { 0x140F6762Cull, 0x1FE48u, 255, 3 },
            { 0x140F6763Cull, 0x1FE48u, 255, 3 },
            { 0x140F67675ull, 0x1FE50u, 255, 3 },
            { 0x140F676BAull, 0x1FE50u, 255, 3 },
            { 0x140F676F0ull, 0x1FE48u, 255, 3 },
            { 0x140F67733ull, 0x1FE58u, 255, 3 },
            { 0x140F6778Cull, 0x1FE70u, 255, 3 },
            { 0x140F67D70ull, 0x1FE40u, 255, 4 },
            { 0x140F67D89ull, 0x1FE40u, 255, 3 },
            { 0x140F68040ull, 0x1FEA8u, 255, 4 },
            { 0x140F68091ull, 0x1FE80u, 255, 3 },
            { 0x140F680A7ull, 0x1FFA6u, 2, 2 },
            { 0x140F680ADull, 0x1FF7Eu, 255, 4 },
            { 0x140F680B7ull, 0x1FF30u, 255, 4 },
            { 0x140F681E3ull, 0x1FF30u, 255, 5 },
            { 0x140F681F6ull, 0x1FF7Eu, 255, 5 },
            { 0x140F6823Cull, 0x1E008u, 255, 3 },
            { 0x140F682C7ull, 0x1E008u, 255, 3 },
            { 0x140F68327ull, 0x1E008u, 255, 3 },
            { 0x140F6837Full, 0x1E028u, 255, 3 },
            { 0x140F683D3ull, 0x1E008u, 255, 3 },
            { 0x140F68406ull, 0x1FE40u, 255, 4 },
            { 0x140F68450ull, 0x1FE40u, 255, 3 },
            { 0x140F684D0ull, 0x1E008u, 255, 3 },
            { 0x140F685EBull, 0x1E010u, 255, 3 },
            { 0x140F686ABull, 0x1E008u, 255, 3 },
            { 0x140F68838ull, 0x1E030u, 255, 3 },
            { 0x140F68920ull, 0x1FE48u, 255, 3 },
            { 0x140F6893Dull, 0x1FE48u, 255, 3 },
            { 0x140F6895Aull, 0x1FE48u, 255, 3 },
            { 0x140F68977ull, 0x1FE48u, 255, 3 },
            { 0x140F68994ull, 0x1FE48u, 255, 3 },
            { 0x140F689B1ull, 0x1FE48u, 255, 3 },
            { 0x140F689CEull, 0x1FE48u, 255, 3 },
            { 0x140F689EBull, 0x1FE48u, 255, 3 },
            { 0x140F68E6Cull, 0x1FE40u, 255, 3 },
            { 0x140F68ECCull, 0x1FE48u, 255, 3 },
            { 0x140F68F18ull, 0x1FE48u, 255, 3 },
            { 0x140F68F28ull, 0x1FE48u, 255, 3 },
            { 0x140F68F38ull, 0x1FE48u, 255, 3 },
            { 0x140F68F48ull, 0x1FE48u, 255, 3 },
            { 0x140F68F58ull, 0x1FE48u, 255, 3 },
            { 0x140F68F68ull, 0x1FE48u, 255, 3 },
            { 0x140F68FBAull, 0x1FE48u, 255, 3 },
            { 0x140F68FCAull, 0x1FE48u, 255, 3 },
            { 0x140F68FDAull, 0x1FE48u, 255, 3 },
            { 0x140F68FEAull, 0x1FE48u, 255, 3 },
            { 0x140F68FFAull, 0x1FE48u, 255, 3 },
            { 0x140F6900Aull, 0x1FE48u, 255, 3 },
            { 0x140F6904Full, 0x1FE50u, 255, 3 },
            { 0x140F6909Eull, 0x1FE50u, 255, 3 },
            { 0x140F690DBull, 0x1FE48u, 255, 3 },
            { 0x140F69124ull, 0x1FE58u, 255, 3 },
            { 0x140F69178ull, 0x1FE70u, 255, 3 },
            { 0x140F69340ull, 0x1E008u, 255, 3 },
            { 0x140F6954Cull, 0x1FE50u, 255, 3 },
            { 0x140F69588ull, 0x1FE50u, 255, 3 },
            { 0x140F695C9ull, 0x1FE50u, 255, 3 },
            { 0x140F695F7ull, 0x1FE50u, 255, 3 },
            { 0x140F696A3ull, 0x1FE50u, 255, 3 },
            { 0x140F696E9ull, 0x1FE50u, 255, 3 },
            { 0x140F69725ull, 0x1FE50u, 255, 3 },
            { 0x140F69759ull, 0x1FE50u, 255, 3 },
            { 0x140F69E22ull, 0x1E008u, 255, 3 },
            { 0x140F6A569ull, 0x1E028u, 255, 3 },
            { 0x140F6A5FCull, 0x1E028u, 255, 3 },
            { 0x140F6A692ull, 0x1E028u, 255, 3 },
            { 0x140F6A730ull, 0x1E028u, 255, 3 },
            { 0x140F6A8ADull, 0x1FE40u, 255, 3 },
            { 0x140F6AC4Eull, 0x1E010u, 255, 3 },
            { 0x140F6AC75ull, 0x1E010u, 255, 3 },
            { 0x140F6AD2Eull, 0x1E018u, 255, 3 },
            { 0x140F6ADA0ull, 0x1FE18u, 255, 3 },
            { 0x140F6ADD0ull, 0x1E018u, 255, 3 },
            { 0x140F6AE30ull, 0x1E018u, 255, 3 },
            { 0x140F6BDCEull, 0x1FE50u, 255, 3 },
            { 0x140F6BE16ull, 0x1FE50u, 255, 3 },
            { 0x140F6BE49ull, 0x1FE50u, 255, 3 },
            { 0x140F6BEEEull, 0x1FE50u, 255, 3 },
            { 0x140F6BF33ull, 0x1FE50u, 255, 3 },
            { 0x140F6BF67ull, 0x1FE50u, 255, 3 },
            { 0x140F6C209ull, 0x1FE40u, 255, 3 },
            { 0x140F6C26Dull, 0x1FE50u, 255, 3 },
            { 0x140F6C2A7ull, 0x1FE50u, 255, 3 },
            { 0x140F6C2CDull, 0x1FE50u, 255, 3 },
            { 0x140F6C364ull, 0x1FE50u, 255, 3 },
            { 0x140F6C39Bull, 0x1FE50u, 255, 3 },
            { 0x140F6C3C2ull, 0x1FE50u, 255, 3 },
            { 0x140F6C770ull, 0x1FEACu, 255, 3 },
            { 0x140F6C7B5ull, 0x1E008u, 255, 3 },
            { 0x140F6C869ull, 0x1E030u, 255, 3 },
            { 0x140F6C876ull, 0x1E028u, 255, 3 },
            { 0x140F6C8C3ull, 0x1E008u, 255, 3 },
            { 0x140F6C927ull, 0x1E028u, 255, 3 },
            { 0x140F6CAB3ull, 0x1FE50u, 255, 3 },
            { 0x140F6CAD9ull, 0x1FE50u, 255, 3 },
            { 0x140F6CB19ull, 0x1FE50u, 255, 3 },
            { 0x140F6CB37ull, 0x1FE50u, 255, 3 },
            { 0x140F6CBC0ull, 0x1FEADu, 255, 3 },
            { 0x140F6CC31ull, 0x1E008u, 255, 3 },
            { 0x140F6CD65ull, 0x1FE78u, 255, 3 },
            { 0x140F6CD8Bull, 0x1FE68u, 255, 3 },
            { 0x140F6CDB0ull, 0x1E008u, 255, 3 },
            { 0x140F6CDD0ull, 0x1E008u, 255, 3 },
            { 0x140F6CDF0ull, 0x1E008u, 255, 3 },
            { 0x140F6CE20ull, 0x1FEA0u, 255, 3 },
            { 0x140F6CEFAull, 0x1E008u, 255, 3 },
            { 0x140F6CF4Aull, 0x1FE40u, 255, 3 },
            { 0x140F6CFB1ull, 0x1E008u, 255, 3 },
            { 0x140F6CFF2ull, 0x1E008u, 255, 3 },
            { 0x140F6D130ull, 0x1E008u, 255, 3 },
            { 0x140F6D1B1ull, 0x1FE58u, 255, 3 },
            { 0x140F6D21Aull, 0x1FE58u, 255, 3 },
            { 0x140F6D283ull, 0x1FE48u, 255, 3 },
            { 0x140F6D3B1ull, 0x1FFC6u, 255, 3 },
            { 0x140F6D452ull, 0x1FEA0u, 255, 3 },
            { 0x140F6D4B6ull, 0x1FEA0u, 255, 3 },
            { 0x140F6D4E0ull, 0x1FFA5u, 255, 3 },
            { 0x140F6D5E3ull, 0x1E010u, 255, 3 },
            { 0x140F6D838ull, 0x1E010u, 255, 3 },
            { 0x140F6D9C0ull, 0x1FFA8u, 255, 3 },
            { 0x140F6DA80ull, 0x1E018u, 255, 3 },
            { 0x140F6DAF2ull, 0x1FFA7u, 255, 3 },
            { 0x140F6DAFBull, 0x1E008u, 255, 3 },
            { 0x140F6DB8Dull, 0x1FE40u, 255, 3 },
            { 0x140F6DC8Full, 0x1FE50u, 255, 4 },
            { 0x140F6DEF8ull, 0x1E008u, 255, 3 },
            { 0x140F6E07Cull, 0x1E008u, 255, 3 },
            { 0x140F6E0E2ull, 0x1FEACu, 2, 2 },
            { 0x140F6E0EAull, 0x1FEACu, 2, 2 },
            { 0x140F6E160ull, 0x1FFA8u, 255, 3 },
            { 0x140F6E170ull, 0x1E008u, 255, 3 },
            { 0x140F6E540ull, 0x1FEADu, 2, 2 },
            { 0x140F6E60Full, 0x1E008u, 255, 3 },
            { 0x140F6E65Bull, 0x1FFA8u, 255, 3 },
            { 0x140F6E680ull, 0x1FE68u, 255, 3 },
            { 0x140F6E69Cull, 0x1FE68u, 255, 3 },
            { 0x140F6E6BBull, 0x1FE68u, 255, 3 },
            { 0x140F6E6D0ull, 0x1FE60u, 255, 3 },
            { 0x140F6E6F8ull, 0x1E008u, 255, 3 },
            { 0x140F6E707ull, 0x1E008u, 255, 3 },
            { 0x140F6E720ull, 0x1E008u, 255, 3 },
            { 0x140F6E757ull, 0x1FEA0u, 255, 3 },
            { 0x140F6E798ull, 0x1E008u, 255, 3 },
            { 0x140F6E7A8ull, 0x1E008u, 255, 3 },
            { 0x140F6E7C0ull, 0x1FF7Eu, 255, 3 },
            { 0x140F6E7C7ull, 0x1FF86u, 255, 3 },
            { 0x140F6E7CEull, 0x1FF8Eu, 255, 3 },
            { 0x140F6E7D5ull, 0x1FF96u, 255, 3 },
            { 0x140F6E7DCull, 0x1FF9Eu, 255, 3 },
            { 0x140F6E7E3ull, 0x1FFA2u, 255, 4 },
            { 0x140F6E7EBull, 0x1FFA4u, 255, 3 },
            { 0x140F6E814ull, 0x1E008u, 255, 3 },
            { 0x140F6E849ull, 0x1FFA8u, 255, 3 },
            { 0x140F6E878ull, 0x1E008u, 255, 3 },
            { 0x140F6E887ull, 0x1E008u, 255, 3 },
            { 0x140F6E94Full, 0x1E030u, 255, 3 },
            { 0x140F6EA93ull, 0x1E030u, 255, 3 },
            { 0x140F6EAB2ull, 0x1E030u, 255, 3 },
            { 0x140F6EAE0ull, 0x1FFA5u, 2, 2 },
            { 0x140F6EE30ull, 0x1FE48u, 255, 3 },
            { 0x140F6EE3Aull, 0x1FE38u, 255, 6 },
            { 0x140F6EE4Bull, 0x1FE38u, 255, 3 },
            { 0x140F6EE5Aull, 0x1FE18u, 255, 4 },
            { 0x140F6EE62ull, 0x1FE38u, 255, 3 },
            { 0x140F6EE69ull, 0x1FE48u, 255, 3 },
            { 0x140F6EE77ull, 0x1FE38u, 255, 3 },
            { 0x140F6EE86ull, 0x1FE18u, 255, 4 },
            { 0x140F6EE8Eull, 0x1FE38u, 255, 3 },
            { 0x140F6EE95ull, 0x1FE48u, 255, 3 },
            { 0x140F6EEA3ull, 0x1FE38u, 255, 3 },
            { 0x140F6EEB2ull, 0x1FE18u, 255, 4 },
            { 0x140F6EEBAull, 0x1FE38u, 255, 3 },
            { 0x140F6EEC1ull, 0x1FE48u, 255, 3 },
            { 0x140F6EECFull, 0x1FE38u, 255, 3 },
            { 0x140F6EEDEull, 0x1FE18u, 255, 4 },
            { 0x140F6EEE6ull, 0x1FE38u, 255, 3 },
            { 0x140F6EEEDull, 0x1FE48u, 255, 3 },
            { 0x140F6EEFBull, 0x1FE38u, 255, 3 },
            { 0x140F6EF0Aull, 0x1FE18u, 255, 4 },
            { 0x140F6EF12ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF19ull, 0x1FE48u, 255, 3 },
            { 0x140F6EF27ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF36ull, 0x1FE18u, 255, 4 },
            { 0x140F6EF3Eull, 0x1FE38u, 255, 3 },
            { 0x140F6EF45ull, 0x1FE48u, 255, 3 },
            { 0x140F6EF53ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF62ull, 0x1FE18u, 255, 4 },
            { 0x140F6EF6Aull, 0x1FE38u, 255, 3 },
            { 0x140F6EF71ull, 0x1FE48u, 255, 3 },
            { 0x140F6EF7Full, 0x1FE38u, 255, 3 },
            { 0x140F6EF8Eull, 0x1FE18u, 255, 4 },
            { 0x140F6EF96ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF9Dull, 0x1FE48u, 255, 3 },
            { 0x140F6EFB5ull, 0x1FE3Cu, 255, 3 },
            { 0x140F6F007ull, 0x1E030u, 255, 3 },
            { 0x140F6F0A6ull, 0x1E030u, 255, 3 },
            { 0x140F6F122ull, 0x1E030u, 255, 3 },
            { 0x140F6F1CEull, 0x1E030u, 255, 3 },
            { 0x140F6F671ull, 0x1E028u, 255, 3 },
            { 0x140F6F6C0ull, 0x1FE40u, 255, 3 },
            { 0x140F6F6CEull, 0x1FE48u, 255, 3 },
            { 0x140F6F6DCull, 0x1FE50u, 255, 3 },
            { 0x140F6F6EAull, 0x1FE58u, 255, 3 },
            { 0x140F6F6F8ull, 0x1FE60u, 255, 3 },
            { 0x140F6F703ull, 0x1FE68u, 255, 3 },
            { 0x140F6F711ull, 0x1FE70u, 255, 3 },
            { 0x140F6F72Aull, 0x1FE78u, 255, 3 },
            { 0x140F6F739ull, 0x1FE80u, 255, 3 },
            { 0x140F6F749ull, 0x1FE88u, 255, 3 },
            { 0x140F6F758ull, 0x1FE90u, 255, 3 },
            { 0x140F6F770ull, 0x1FE80u, 255, 3 },
            { 0x140F6F777ull, 0x1FE98u, 255, 3 },
            { 0x140F6F78Cull, 0x1FFA6u, 2, 2 },
            { 0x140F6F7FEull, 0x1FECEu, 255, 3 },
            { 0x140F6F80Eull, 0x1FEC8u, 255, 5 },
            { 0x140F6F824ull, 0x1FEB0u, 255, 3 },
            { 0x140F6F858ull, 0x1FE98u, 255, 3 },
            { 0x140F6F87Dull, 0x1FE98u, 255, 3 },
            { 0x140F6F889ull, 0x1FEA8u, 255, 3 },
            { 0x140F6F890ull, 0x1FED0u, 255, 3 },
            { 0x140F6F8ACull, 0x1FEB0u, 255, 3 },
            { 0x140F6F8C2ull, 0x1FEB0u, 255, 3 },
            { 0x140F6F8DBull, 0x1FEB0u, 255, 3 },
            { 0x140F6F8E2ull, 0x1FED0u, 255, 3 },
            { 0x140F6F90Bull, 0x1FECEu, 255, 3 },
            { 0x140F6F9BFull, 0x1FEA8u, 255, 5 },
            { 0x140F6FA66ull, 0x1FEAEu, 255, 4 },
            { 0x140F6FA76ull, 0x1FE90u, 255, 3 },
            { 0x140F6FA9Dull, 0x1FE78u, 255, 3 },
            { 0x140F6FAC0ull, 0x1FE78u, 255, 3 },
            { 0x140F6FAD0ull, 0x1FE88u, 255, 3 },
            { 0x140F6FAD7ull, 0x1FEB0u, 255, 3 },
            { 0x140F6FAF3ull, 0x1FE90u, 255, 3 },
            { 0x140F6FB06ull, 0x1FE90u, 255, 3 },
            { 0x140F6FB1Full, 0x1FE90u, 255, 3 },
            { 0x140F6FB26ull, 0x1FEB0u, 255, 3 },
            { 0x140F6FB60ull, 0x1FEAEu, 255, 4 },
            { 0x140F6FBA9ull, 0x1FE40u, 255, 3 },
            { 0x140F6FBEAull, 0x1FEA8u, 255, 3 },
            { 0x140F6FC19ull, 0x1FE40u, 255, 3 },
            { 0x140F6FC2Eull, 0x1FFA5u, 255, 3 },
            { 0x140F6FC35ull, 0x1FFA7u, 2, 2 },
            { 0x140F6FC3Dull, 0x1FEADu, 255, 3 },
            { 0x140F6FC44ull, 0x1FEA8u, 255, 4 },
            { 0x140F6FC4Cull, 0x1FEA8u, 255, 4 },
            { 0x140F6FC9Cull, 0x1E010u, 255, 3 },
            { 0x140F6FD1Aull, 0x1E010u, 255, 3 },
            { 0x140F6FD4Bull, 0x1E010u, 255, 3 },
            { 0x140F6FD5Dull, 0x1E010u, 255, 3 },
            { 0x140F6FD7Dull, 0x1E010u, 255, 3 },
            { 0x140F6FD8Cull, 0x1E010u, 255, 3 },
            { 0x140F6FDB3ull, 0x1E010u, 255, 3 },
            { 0x140F6FDC5ull, 0x1E010u, 255, 3 },
            { 0x140F6FDE0ull, 0x1E010u, 255, 3 },
            { 0x140F6FDEFull, 0x1E010u, 255, 3 },
            { 0x140F6FF36ull, 0x1E018u, 255, 3 },
            { 0x140F700F5ull, 0x1FE90u, 255, 3 },
            { 0x140F7010Full, 0x1FE88u, 255, 3 },
            { 0x140F70116ull, 0x1FEB0u, 255, 3 },
            { 0x140F70135ull, 0x1FE90u, 255, 3 },
            { 0x140F70153ull, 0x1FE90u, 255, 3 },
            { 0x140F7017Bull, 0x1FE90u, 255, 3 },
            { 0x140F7018Eull, 0x1FE90u, 255, 3 },
            { 0x140F70195ull, 0x1FEB0u, 255, 3 },
            { 0x147F32D65ull, 0x1E008u, 3, 3 },
            { 0x147F32D87ull, 0x1FE58u, 3, 3 },
            { 0x147F32DA6ull, 0x1FE58u, 3, 3 },
        };

        static const BoundImmSite kBoundImmSites[] = {
            { 0x140F64DBDull, 1 },
            { 0x140F652EBull, 1 },
            { 0x140F65375ull, 1 },
            { 0x140F65509ull, 1 },
            { 0x140F655D0ull, 1 },
            { 0x140F6564Bull, 1 },
            { 0x140F65776ull, 1 },
            { 0x140F657ABull, 1 },
            { 0x140F65809ull, 2 },
            { 0x140F658A9ull, 2 },
            { 0x140F6594Dull, 2 },
            { 0x140F65C80ull, 1 },
            { 0x140F65D94ull, 1 },
            { 0x140F65DF4ull, 1 },
            { 0x140F664B4ull, 1 },
            { 0x140F665C4ull, 1 },
            { 0x140F66624ull, 1 },
            { 0x140F6693Aull, 1 },
            { 0x140F66DA1ull, 2 },
            { 0x140F66F8Cull, 1 },
            { 0x140F6700Aull, 2 },
            { 0x140F670D0ull, 1 },
            { 0x140F672C5ull, 1 },
            { 0x140F6747Eull, 1 },
            { 0x140F6780Aull, 1 },
            { 0x140F6792Bull, 1 },
            { 0x140F67DDAull, 1 },
            { 0x140F68050ull, 1 },
            { 0x140F68227ull, 2 },
            { 0x140F6829Aull, 2 },
            { 0x140F68317ull, 2 },
            { 0x140F68362ull, 2 },
            { 0x140F683BDull, 2 },
            { 0x140F684DEull, 2 },
            { 0x140F6854Aull, 1 },
            { 0x140F68683ull, 2 },
            { 0x140F68857ull, 2 },
            { 0x140F68E1Eull, 1 },
            { 0x140F69202ull, 2 },
            { 0x140F6931Full, 1 },
            { 0x140F694CDull, 1 },
            { 0x140F697D4ull, 1 },
            { 0x140F69B94ull, 1 },
            { 0x140F69CBBull, 2 },
            { 0x140F69E94ull, 2 },
            { 0x140F6A21Dull, 1 },
            { 0x140F6A26Aull, 1 },
            { 0x140F6A33Dull, 1 },
            { 0x140F6A39Dull, 1 },
            { 0x140F6A401ull, 1 },
            { 0x140F6A4D8ull, 1 },
            { 0x140F6A542ull, 1 },
            { 0x140F6A62Aull, 1 },
            { 0x140F6A66Aull, 1 },
            { 0x140F6A74Aull, 1 },
            { 0x140F6AD3Cull, 2 },
            { 0x140F6B21Eull, 1 },
            { 0x140F6B360ull, 1 },
            { 0x140F6B557ull, 1 },
            { 0x140F6B5CCull, 2 },
            { 0x140F6BD55ull, 1 },
            { 0x140F6BFC5ull, 1 },
            { 0x140F6C1E5ull, 1 },
            { 0x140F6C415ull, 1 },
            { 0x140F6C5C5ull, 1 },
            { 0x140F6C79Cull, 2 },
            { 0x140F6C893ull, 2 },
            { 0x140F6C8F8ull, 1 },
            { 0x140F6C993ull, 1 },
            { 0x140F6CBA0ull, 1 },
            { 0x140F6CBF0ull, 2 },
            { 0x140F6CE13ull, 1 },
            { 0x140F6D521ull, 1 },
            { 0x140F6DD95ull, 1 },
            { 0x140F6DE4Full, 1 },
            { 0x140F6E501ull, 1 },
            { 0x140F6E5D0ull, 1 },
            { 0x140F6E73Aull, 1 },
            { 0x140F6EB19ull, 1 },
            { 0x140F6EB67ull, 2 },
            { 0x140F6EBCEull, 1 },
            { 0x140F6EBFFull, 2 },
            { 0x140F6EC74ull, 1 },
            { 0x140F6ECBBull, 2 },
            { 0x140F6ED1Full, 1 },
            { 0x140F6EDA0ull, 1 },
            { 0x140F6F652ull, 1 },
            { 0x140F6FF45ull, 2 },
            { 0x140F71002ull, 3 },
        };
        }

        namespace tbl_jp154
        {
        static const CtlDispSite kCtlDispSites[] = {
            { 0x140F649F3ull, 0x1E028u, 255, 3 },
            { 0x140F649FAull, 0x1E030u, 255, 3 },
            { 0x140F64A01ull, 0x1E038u, 255, 3 },
            { 0x140F64A23ull, 0x1FE58u, 255, 3 },
            { 0x140F64A2Aull, 0x1FE60u, 255, 3 },
            { 0x140F64A31ull, 0x1FE68u, 255, 3 },
            { 0x140F64A38ull, 0x1FE70u, 255, 3 },
            { 0x140F64A3Full, 0x1FE78u, 255, 3 },
            { 0x140F64A46ull, 0x1FE80u, 255, 3 },
            { 0x140F64A4Dull, 0x1FE88u, 255, 3 },
            { 0x140F64A54ull, 0x1FE90u, 255, 3 },
            { 0x140F64A5Bull, 0x1FE98u, 255, 3 },
            { 0x140F64A62ull, 0x1FEA0u, 255, 3 },
            { 0x140F64A69ull, 0x1FEA8u, 255, 3 },
            { 0x140F64A70ull, 0x1FEB0u, 255, 3 },
            { 0x140F64A77ull, 0x1FEB8u, 255, 3 },
            { 0x140F64A7Eull, 0x1FEC0u, 255, 3 },
            { 0x140F64A85ull, 0x1FEC8u, 255, 3 },
            { 0x140F64A8Cull, 0x1FFC5u, 255, 4 },
            { 0x140F64A94ull, 0x1FFC7u, 255, 3 },
            { 0x140F64A9Bull, 0x1FFC8u, 255, 3 },
            { 0x140F64AAAull, 0x1E038u, 255, 3 },
            { 0x140F64AB6ull, 0x1FED0u, 255, 3 },
            { 0x140F64ACCull, 0x1FF50u, 255, 3 },
            { 0x140F64AD3ull, 0x1FF58u, 255, 3 },
            { 0x140F64ADAull, 0x1FF60u, 255, 3 },
            { 0x140F64AE1ull, 0x1FF68u, 255, 3 },
            { 0x140F64AE8ull, 0x1FF70u, 255, 3 },
            { 0x140F64AEFull, 0x1FF78u, 255, 3 },
            { 0x140F64AF6ull, 0x1FF80u, 255, 3 },
            { 0x140F64AFDull, 0x1FF88u, 255, 3 },
            { 0x140F64B04ull, 0x1FF90u, 255, 3 },
            { 0x140F64B0Bull, 0x1FF98u, 2, 2 },
            { 0x140F64B11ull, 0x1FF9Cu, 255, 3 },
            { 0x140F64B22ull, 0x1FF9Eu, 255, 3 },
            { 0x140F64B29ull, 0x1FFA6u, 255, 3 },
            { 0x140F64B30ull, 0x1FFAEu, 255, 3 },
            { 0x140F64B37ull, 0x1FFB6u, 255, 3 },
            { 0x140F64B3Eull, 0x1FFBEu, 2, 2 },
            { 0x140F64B44ull, 0x1FFC2u, 255, 3 },
            { 0x140F64B4Bull, 0x1FFC4u, 2, 2 },
            { 0x140F64B71ull, 0x1FEC0u, 255, 3 },
            { 0x140F64BDBull, 0x1FEC0u, 255, 3 },
            { 0x140F64C52ull, 0x1FEC0u, 255, 3 },
            { 0x140F64CF2ull, 0x1FEC0u, 255, 3 },
            { 0x140F64E70ull, 0x1FFA8u, 255, 3 },
            { 0x140F64E80ull, 0x1E018u, 255, 3 },
            { 0x140F64ECCull, 0x1E008u, 255, 3 },
            { 0x140F64EE1ull, 0x1FE90u, 255, 3 },
            { 0x140F64F20ull, 0x1FE78u, 255, 3 },
            { 0x140F64F40ull, 0x1FE78u, 255, 3 },
            { 0x140F64F50ull, 0x1FE88u, 255, 3 },
            { 0x140F64F57ull, 0x1FEB0u, 255, 3 },
            { 0x140F64F73ull, 0x1FE90u, 255, 3 },
            { 0x140F64F86ull, 0x1FE90u, 255, 3 },
            { 0x140F64F9Full, 0x1FE90u, 255, 3 },
            { 0x140F64FA6ull, 0x1FEB0u, 255, 3 },
            { 0x140F64FC4ull, 0x1E008u, 255, 3 },
            { 0x140F6508Cull, 0x1E010u, 255, 3 },
            { 0x140F6538Eull, 0x1E008u, 255, 3 },
            { 0x140F65405ull, 0x1E028u, 255, 3 },
            { 0x140F654E7ull, 0x1E028u, 255, 3 },
            { 0x140F6557Aull, 0x1E028u, 255, 3 },
            { 0x140F656D5ull, 0x1E008u, 255, 3 },
            { 0x140F65771ull, 0x1E008u, 255, 3 },
            { 0x140F657E3ull, 0x1E008u, 255, 3 },
            { 0x140F65895ull, 0x1E008u, 255, 3 },
            { 0x140F65935ull, 0x1E008u, 255, 3 },
            { 0x140F659E2ull, 0x1E008u, 255, 3 },
            { 0x140F65A41ull, 0x1E008u, 255, 3 },
            { 0x140F66220ull, 0x1E030u, 255, 3 },
            { 0x140F66260ull, 0x1E030u, 255, 3 },
            { 0x140F662F8ull, 0x1FE60u, 255, 4 },
            { 0x140F66313ull, 0x1FE60u, 255, 3 },
            { 0x140F66E32ull, 0x1E008u, 255, 3 },
            { 0x140F66E73ull, 0x1E008u, 255, 3 },
            { 0x140F66ED6ull, 0x1E008u, 255, 3 },
            { 0x140F66F9Full, 0x1E008u, 255, 3 },
            { 0x140F67210ull, 0x1FE50u, 255, 3 },
            { 0x140F67245ull, 0x1FE50u, 255, 3 },
            { 0x140F67273ull, 0x1FE48u, 255, 3 },
            { 0x140F672ADull, 0x1FE58u, 255, 3 },
            { 0x140F6751Aull, 0x1FE40u, 255, 3 },
            { 0x140F67540ull, 0x1FE40u, 255, 3 },
            { 0x140F6756Cull, 0x1FE48u, 255, 3 },
            { 0x140F675B6ull, 0x1FE48u, 255, 3 },
            { 0x140F675C6ull, 0x1FE48u, 255, 3 },
            { 0x140F675D6ull, 0x1FE48u, 255, 3 },
            { 0x140F675E6ull, 0x1FE48u, 255, 3 },
            { 0x140F675F6ull, 0x1FE48u, 255, 3 },
            { 0x140F67606ull, 0x1FE48u, 255, 3 },
            { 0x140F6764Cull, 0x1FE48u, 255, 3 },
            { 0x140F6765Cull, 0x1FE48u, 255, 3 },
            { 0x140F6766Cull, 0x1FE48u, 255, 3 },
            { 0x140F6767Cull, 0x1FE48u, 255, 3 },
            { 0x140F6768Cull, 0x1FE48u, 255, 3 },
            { 0x140F6769Cull, 0x1FE48u, 255, 3 },
            { 0x140F676D5ull, 0x1FE50u, 255, 3 },
            { 0x140F6771Aull, 0x1FE50u, 255, 3 },
            { 0x140F67750ull, 0x1FE48u, 255, 3 },
            { 0x140F67793ull, 0x1FE58u, 255, 3 },
            { 0x140F677ECull, 0x1FE70u, 255, 3 },
            { 0x140F67DD0ull, 0x1FE40u, 255, 4 },
            { 0x140F67DE9ull, 0x1FE40u, 255, 3 },
            { 0x140F680A0ull, 0x1FEA8u, 255, 4 },
            { 0x140F680F1ull, 0x1FE80u, 255, 3 },
            { 0x140F68107ull, 0x1FFA6u, 2, 2 },
            { 0x140F6810Dull, 0x1FF7Eu, 255, 4 },
            { 0x140F68117ull, 0x1FF30u, 255, 4 },
            { 0x140F68243ull, 0x1FF30u, 255, 5 },
            { 0x140F68256ull, 0x1FF7Eu, 255, 5 },
            { 0x140F6829Cull, 0x1E008u, 255, 3 },
            { 0x140F68327ull, 0x1E008u, 255, 3 },
            { 0x140F68387ull, 0x1E008u, 255, 3 },
            { 0x140F683DFull, 0x1E028u, 255, 3 },
            { 0x140F68433ull, 0x1E008u, 255, 3 },
            { 0x140F68466ull, 0x1FE40u, 255, 4 },
            { 0x140F684B0ull, 0x1FE40u, 255, 3 },
            { 0x140F68530ull, 0x1E008u, 255, 3 },
            { 0x140F6864Bull, 0x1E010u, 255, 3 },
            { 0x140F6870Bull, 0x1E008u, 255, 3 },
            { 0x140F68898ull, 0x1E030u, 255, 3 },
            { 0x140F68980ull, 0x1FE48u, 255, 3 },
            { 0x140F6899Dull, 0x1FE48u, 255, 3 },
            { 0x140F689BAull, 0x1FE48u, 255, 3 },
            { 0x140F689D7ull, 0x1FE48u, 255, 3 },
            { 0x140F689F4ull, 0x1FE48u, 255, 3 },
            { 0x140F68A11ull, 0x1FE48u, 255, 3 },
            { 0x140F68A2Eull, 0x1FE48u, 255, 3 },
            { 0x140F68A4Bull, 0x1FE48u, 255, 3 },
            { 0x140F68ECCull, 0x1FE40u, 255, 3 },
            { 0x140F68F2Cull, 0x1FE48u, 255, 3 },
            { 0x140F68F78ull, 0x1FE48u, 255, 3 },
            { 0x140F68F88ull, 0x1FE48u, 255, 3 },
            { 0x140F68F98ull, 0x1FE48u, 255, 3 },
            { 0x140F68FA8ull, 0x1FE48u, 255, 3 },
            { 0x140F68FB8ull, 0x1FE48u, 255, 3 },
            { 0x140F68FC8ull, 0x1FE48u, 255, 3 },
            { 0x140F6901Aull, 0x1FE48u, 255, 3 },
            { 0x140F6902Aull, 0x1FE48u, 255, 3 },
            { 0x140F6903Aull, 0x1FE48u, 255, 3 },
            { 0x140F6904Aull, 0x1FE48u, 255, 3 },
            { 0x140F6905Aull, 0x1FE48u, 255, 3 },
            { 0x140F6906Aull, 0x1FE48u, 255, 3 },
            { 0x140F690AFull, 0x1FE50u, 255, 3 },
            { 0x140F690FEull, 0x1FE50u, 255, 3 },
            { 0x140F6913Bull, 0x1FE48u, 255, 3 },
            { 0x140F69184ull, 0x1FE58u, 255, 3 },
            { 0x140F691D8ull, 0x1FE70u, 255, 3 },
            { 0x140F693A0ull, 0x1E008u, 255, 3 },
            { 0x140F695ACull, 0x1FE50u, 255, 3 },
            { 0x140F695E8ull, 0x1FE50u, 255, 3 },
            { 0x140F69629ull, 0x1FE50u, 255, 3 },
            { 0x140F69657ull, 0x1FE50u, 255, 3 },
            { 0x140F69703ull, 0x1FE50u, 255, 3 },
            { 0x140F69749ull, 0x1FE50u, 255, 3 },
            { 0x140F69785ull, 0x1FE50u, 255, 3 },
            { 0x140F697B9ull, 0x1FE50u, 255, 3 },
            { 0x140F69E82ull, 0x1E008u, 255, 3 },
            { 0x140F6A5C9ull, 0x1E028u, 255, 3 },
            { 0x140F6A65Cull, 0x1E028u, 255, 3 },
            { 0x140F6A6F2ull, 0x1E028u, 255, 3 },
            { 0x140F6A790ull, 0x1E028u, 255, 3 },
            { 0x140F6A90Dull, 0x1FE40u, 255, 3 },
            { 0x140F6ACAEull, 0x1E010u, 255, 3 },
            { 0x140F6ACD5ull, 0x1E010u, 255, 3 },
            { 0x140F6AD8Eull, 0x1E018u, 255, 3 },
            { 0x140F6AE00ull, 0x1FE18u, 255, 3 },
            { 0x140F6AE30ull, 0x1E018u, 255, 3 },
            { 0x140F6AE90ull, 0x1E018u, 255, 3 },
            { 0x140F6BE2Eull, 0x1FE50u, 255, 3 },
            { 0x140F6BE76ull, 0x1FE50u, 255, 3 },
            { 0x140F6BEA9ull, 0x1FE50u, 255, 3 },
            { 0x140F6BF4Eull, 0x1FE50u, 255, 3 },
            { 0x140F6BF93ull, 0x1FE50u, 255, 3 },
            { 0x140F6BFC7ull, 0x1FE50u, 255, 3 },
            { 0x140F6C269ull, 0x1FE40u, 255, 3 },
            { 0x140F6C2CDull, 0x1FE50u, 255, 3 },
            { 0x140F6C307ull, 0x1FE50u, 255, 3 },
            { 0x140F6C32Dull, 0x1FE50u, 255, 3 },
            { 0x140F6C3C4ull, 0x1FE50u, 255, 3 },
            { 0x140F6C3FBull, 0x1FE50u, 255, 3 },
            { 0x140F6C422ull, 0x1FE50u, 255, 3 },
            { 0x140F6C7D0ull, 0x1FEACu, 255, 3 },
            { 0x140F6C815ull, 0x1E008u, 255, 3 },
            { 0x140F6C8C9ull, 0x1E030u, 255, 3 },
            { 0x140F6C8D6ull, 0x1E028u, 255, 3 },
            { 0x140F6C923ull, 0x1E008u, 255, 3 },
            { 0x140F6C987ull, 0x1E028u, 255, 3 },
            { 0x140F6CB13ull, 0x1FE50u, 255, 3 },
            { 0x140F6CB39ull, 0x1FE50u, 255, 3 },
            { 0x140F6CB79ull, 0x1FE50u, 255, 3 },
            { 0x140F6CB97ull, 0x1FE50u, 255, 3 },
            { 0x140F6CC20ull, 0x1FEADu, 255, 3 },
            { 0x140F6CC91ull, 0x1E008u, 255, 3 },
            { 0x140F6CDC5ull, 0x1FE78u, 255, 3 },
            { 0x140F6CDEBull, 0x1FE68u, 255, 3 },
            { 0x140F6CE10ull, 0x1E008u, 255, 3 },
            { 0x140F6CE30ull, 0x1E008u, 255, 3 },
            { 0x140F6CE50ull, 0x1E008u, 255, 3 },
            { 0x140F6CE80ull, 0x1FEA0u, 255, 3 },
            { 0x140F6CF5Aull, 0x1E008u, 255, 3 },
            { 0x140F6CFAAull, 0x1FE40u, 255, 3 },
            { 0x140F6D011ull, 0x1E008u, 255, 3 },
            { 0x140F6D052ull, 0x1E008u, 255, 3 },
            { 0x140F6D190ull, 0x1E008u, 255, 3 },
            { 0x140F6D211ull, 0x1FE58u, 255, 3 },
            { 0x140F6D27Aull, 0x1FE58u, 255, 3 },
            { 0x140F6D2E3ull, 0x1FE48u, 255, 3 },
            { 0x140F6D411ull, 0x1FFC6u, 255, 3 },
            { 0x140F6D4B2ull, 0x1FEA0u, 255, 3 },
            { 0x140F6D516ull, 0x1FEA0u, 255, 3 },
            { 0x140F6D540ull, 0x1FFA5u, 255, 3 },
            { 0x140F6D643ull, 0x1E010u, 255, 3 },
            { 0x140F6D898ull, 0x1E010u, 255, 3 },
            { 0x140F6DA20ull, 0x1FFA8u, 255, 3 },
            { 0x140F6DAE0ull, 0x1E018u, 255, 3 },
            { 0x140F6DB52ull, 0x1FFA7u, 255, 3 },
            { 0x140F6DB5Bull, 0x1E008u, 255, 3 },
            { 0x140F6DBEDull, 0x1FE40u, 255, 3 },
            { 0x140F6DCEFull, 0x1FE50u, 255, 4 },
            { 0x140F6DF58ull, 0x1E008u, 255, 3 },
            { 0x140F6E0DCull, 0x1E008u, 255, 3 },
            { 0x140F6E142ull, 0x1FEACu, 2, 2 },
            { 0x140F6E14Aull, 0x1FEACu, 2, 2 },
            { 0x140F6E1C0ull, 0x1FFA8u, 255, 3 },
            { 0x140F6E1D0ull, 0x1E008u, 255, 3 },
            { 0x140F6E5A0ull, 0x1FEADu, 2, 2 },
            { 0x140F6E66Full, 0x1E008u, 255, 3 },
            { 0x140F6E6BBull, 0x1FFA8u, 255, 3 },
            { 0x140F6E6E0ull, 0x1FE68u, 255, 3 },
            { 0x140F6E6FCull, 0x1FE68u, 255, 3 },
            { 0x140F6E71Bull, 0x1FE68u, 255, 3 },
            { 0x140F6E730ull, 0x1FE60u, 255, 3 },
            { 0x140F6E758ull, 0x1E008u, 255, 3 },
            { 0x140F6E767ull, 0x1E008u, 255, 3 },
            { 0x140F6E780ull, 0x1E008u, 255, 3 },
            { 0x140F6E7B7ull, 0x1FEA0u, 255, 3 },
            { 0x140F6E7F8ull, 0x1E008u, 255, 3 },
            { 0x140F6E808ull, 0x1E008u, 255, 3 },
            { 0x140F6E820ull, 0x1FF7Eu, 255, 3 },
            { 0x140F6E827ull, 0x1FF86u, 255, 3 },
            { 0x140F6E82Eull, 0x1FF8Eu, 255, 3 },
            { 0x140F6E835ull, 0x1FF96u, 255, 3 },
            { 0x140F6E83Cull, 0x1FF9Eu, 255, 3 },
            { 0x140F6E843ull, 0x1FFA2u, 255, 4 },
            { 0x140F6E84Bull, 0x1FFA4u, 255, 3 },
            { 0x140F6E874ull, 0x1E008u, 255, 3 },
            { 0x140F6E8A9ull, 0x1FFA8u, 255, 3 },
            { 0x140F6E8D8ull, 0x1E008u, 255, 3 },
            { 0x140F6E8E7ull, 0x1E008u, 255, 3 },
            { 0x140F6E9AFull, 0x1E030u, 255, 3 },
            { 0x140F6EAF3ull, 0x1E030u, 255, 3 },
            { 0x140F6EB12ull, 0x1E030u, 255, 3 },
            { 0x140F6EB40ull, 0x1FFA5u, 2, 2 },
            { 0x140F6EE90ull, 0x1FE48u, 255, 3 },
            { 0x140F6EE9Aull, 0x1FE38u, 255, 6 },
            { 0x140F6EEABull, 0x1FE38u, 255, 3 },
            { 0x140F6EEBAull, 0x1FE18u, 255, 4 },
            { 0x140F6EEC2ull, 0x1FE38u, 255, 3 },
            { 0x140F6EEC9ull, 0x1FE48u, 255, 3 },
            { 0x140F6EED7ull, 0x1FE38u, 255, 3 },
            { 0x140F6EEE6ull, 0x1FE18u, 255, 4 },
            { 0x140F6EEEEull, 0x1FE38u, 255, 3 },
            { 0x140F6EEF5ull, 0x1FE48u, 255, 3 },
            { 0x140F6EF03ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF12ull, 0x1FE18u, 255, 4 },
            { 0x140F6EF1Aull, 0x1FE38u, 255, 3 },
            { 0x140F6EF21ull, 0x1FE48u, 255, 3 },
            { 0x140F6EF2Full, 0x1FE38u, 255, 3 },
            { 0x140F6EF3Eull, 0x1FE18u, 255, 4 },
            { 0x140F6EF46ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF4Dull, 0x1FE48u, 255, 3 },
            { 0x140F6EF5Bull, 0x1FE38u, 255, 3 },
            { 0x140F6EF6Aull, 0x1FE18u, 255, 4 },
            { 0x140F6EF72ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF79ull, 0x1FE48u, 255, 3 },
            { 0x140F6EF87ull, 0x1FE38u, 255, 3 },
            { 0x140F6EF96ull, 0x1FE18u, 255, 4 },
            { 0x140F6EF9Eull, 0x1FE38u, 255, 3 },
            { 0x140F6EFA5ull, 0x1FE48u, 255, 3 },
            { 0x140F6EFB3ull, 0x1FE38u, 255, 3 },
            { 0x140F6EFC2ull, 0x1FE18u, 255, 4 },
            { 0x140F6EFCAull, 0x1FE38u, 255, 3 },
            { 0x140F6EFD1ull, 0x1FE48u, 255, 3 },
            { 0x140F6EFDFull, 0x1FE38u, 255, 3 },
            { 0x140F6EFEEull, 0x1FE18u, 255, 4 },
            { 0x140F6EFF6ull, 0x1FE38u, 255, 3 },
            { 0x140F6EFFDull, 0x1FE48u, 255, 3 },
            { 0x140F6F015ull, 0x1FE3Cu, 255, 3 },
            { 0x140F6F067ull, 0x1E030u, 255, 3 },
            { 0x140F6F106ull, 0x1E030u, 255, 3 },
            { 0x140F6F182ull, 0x1E030u, 255, 3 },
            { 0x140F6F22Eull, 0x1E030u, 255, 3 },
            { 0x140F6F6D1ull, 0x1E028u, 255, 3 },
            { 0x140F6F720ull, 0x1FE40u, 255, 3 },
            { 0x140F6F72Eull, 0x1FE48u, 255, 3 },
            { 0x140F6F73Cull, 0x1FE50u, 255, 3 },
            { 0x140F6F74Aull, 0x1FE58u, 255, 3 },
            { 0x140F6F758ull, 0x1FE60u, 255, 3 },
            { 0x140F6F763ull, 0x1FE68u, 255, 3 },
            { 0x140F6F771ull, 0x1FE70u, 255, 3 },
            { 0x140F6F78Aull, 0x1FE78u, 255, 3 },
            { 0x140F6F799ull, 0x1FE80u, 255, 3 },
            { 0x140F6F7A9ull, 0x1FE88u, 255, 3 },
            { 0x140F6F7B8ull, 0x1FE90u, 255, 3 },
            { 0x140F6F7D0ull, 0x1FE80u, 255, 3 },
            { 0x140F6F7D7ull, 0x1FE98u, 255, 3 },
            { 0x140F6F7ECull, 0x1FFA6u, 2, 2 },
            { 0x140F6F85Eull, 0x1FECEu, 255, 3 },
            { 0x140F6F86Eull, 0x1FEC8u, 255, 5 },
            { 0x140F6F884ull, 0x1FEB0u, 255, 3 },
            { 0x140F6F8B8ull, 0x1FE98u, 255, 3 },
            { 0x140F6F8DDull, 0x1FE98u, 255, 3 },
            { 0x140F6F8E9ull, 0x1FEA8u, 255, 3 },
            { 0x140F6F8F0ull, 0x1FED0u, 255, 3 },
            { 0x140F6F90Cull, 0x1FEB0u, 255, 3 },
            { 0x140F6F922ull, 0x1FEB0u, 255, 3 },
            { 0x140F6F93Bull, 0x1FEB0u, 255, 3 },
            { 0x140F6F942ull, 0x1FED0u, 255, 3 },
            { 0x140F6F96Bull, 0x1FECEu, 255, 3 },
            { 0x140F6FA1Full, 0x1FEA8u, 255, 5 },
            { 0x140F6FAC6ull, 0x1FEAEu, 255, 4 },
            { 0x140F6FAD6ull, 0x1FE90u, 255, 3 },
            { 0x140F6FAFDull, 0x1FE78u, 255, 3 },
            { 0x140F6FB20ull, 0x1FE78u, 255, 3 },
            { 0x140F6FB30ull, 0x1FE88u, 255, 3 },
            { 0x140F6FB37ull, 0x1FEB0u, 255, 3 },
            { 0x140F6FB53ull, 0x1FE90u, 255, 3 },
            { 0x140F6FB66ull, 0x1FE90u, 255, 3 },
            { 0x140F6FB7Full, 0x1FE90u, 255, 3 },
            { 0x140F6FB86ull, 0x1FEB0u, 255, 3 },
            { 0x140F6FBC0ull, 0x1FEAEu, 255, 4 },
            { 0x140F6FC09ull, 0x1FE40u, 255, 3 },
            { 0x140F6FC4Aull, 0x1FEA8u, 255, 3 },
            { 0x140F6FC79ull, 0x1FE40u, 255, 3 },
            { 0x140F6FC8Eull, 0x1FFA5u, 255, 3 },
            { 0x140F6FC95ull, 0x1FFA7u, 2, 2 },
            { 0x140F6FC9Dull, 0x1FEADu, 255, 3 },
            { 0x140F6FCA4ull, 0x1FEA8u, 255, 4 },
            { 0x140F6FCACull, 0x1FEA8u, 255, 4 },
            { 0x140F6FCFCull, 0x1E010u, 255, 3 },
            { 0x140F6FD7Aull, 0x1E010u, 255, 3 },
            { 0x140F6FDABull, 0x1E010u, 255, 3 },
            { 0x140F6FDBDull, 0x1E010u, 255, 3 },
            { 0x140F6FDDDull, 0x1E010u, 255, 3 },
            { 0x140F6FDECull, 0x1E010u, 255, 3 },
            { 0x140F6FE13ull, 0x1E010u, 255, 3 },
            { 0x140F6FE25ull, 0x1E010u, 255, 3 },
            { 0x140F6FE40ull, 0x1E010u, 255, 3 },
            { 0x140F6FE4Full, 0x1E010u, 255, 3 },
            { 0x140F6FF96ull, 0x1E018u, 255, 3 },
            { 0x140F70155ull, 0x1FE90u, 255, 3 },
            { 0x140F7016Full, 0x1FE88u, 255, 3 },
            { 0x140F70176ull, 0x1FEB0u, 255, 3 },
            { 0x140F70195ull, 0x1FE90u, 255, 3 },
            { 0x140F701B3ull, 0x1FE90u, 255, 3 },
            { 0x140F701DBull, 0x1FE90u, 255, 3 },
            { 0x140F701EEull, 0x1FE90u, 255, 3 },
            { 0x140F701F5ull, 0x1FEB0u, 255, 3 },
            { 0x14801E985ull, 0x1E008u, 3, 3 },
            { 0x14801E9A8ull, 0x1FE58u, 3, 3 },
            { 0x14801E9C7ull, 0x1FE58u, 3, 3 },
        };

        static const BoundImmSite kBoundImmSites[] = {
            { 0x140F64E1Dull, 1 },
            { 0x140F6534Bull, 1 },
            { 0x140F653D5ull, 1 },
            { 0x140F65569ull, 1 },
            { 0x140F65630ull, 1 },
            { 0x140F656ABull, 1 },
            { 0x140F657D6ull, 1 },
            { 0x140F6580Bull, 1 },
            { 0x140F65869ull, 2 },
            { 0x140F65909ull, 2 },
            { 0x140F659ADull, 2 },
            { 0x140F65CE0ull, 1 },
            { 0x140F65DF4ull, 1 },
            { 0x140F65E54ull, 1 },
            { 0x140F66514ull, 1 },
            { 0x140F66624ull, 1 },
            { 0x140F66684ull, 1 },
            { 0x140F6699Aull, 1 },
            { 0x140F66E01ull, 2 },
            { 0x140F66FECull, 1 },
            { 0x140F6706Aull, 2 },
            { 0x140F67130ull, 1 },
            { 0x140F67325ull, 1 },
            { 0x140F674DEull, 1 },
            { 0x140F6786Aull, 1 },
            { 0x140F6798Bull, 1 },
            { 0x140F67E3Aull, 1 },
            { 0x140F680B0ull, 1 },
            { 0x140F68287ull, 2 },
            { 0x140F682FAull, 2 },
            { 0x140F68377ull, 2 },
            { 0x140F683C2ull, 2 },
            { 0x140F6841Dull, 2 },
            { 0x140F6853Eull, 2 },
            { 0x140F685AAull, 1 },
            { 0x140F686E3ull, 2 },
            { 0x140F688B7ull, 2 },
            { 0x140F68E7Eull, 1 },
            { 0x140F69262ull, 2 },
            { 0x140F6937Full, 1 },
            { 0x140F6952Dull, 1 },
            { 0x140F69834ull, 1 },
            { 0x140F69BF4ull, 1 },
            { 0x140F69D1Bull, 2 },
            { 0x140F69EF4ull, 2 },
            { 0x140F6A27Dull, 1 },
            { 0x140F6A2CAull, 1 },
            { 0x140F6A39Dull, 1 },
            { 0x140F6A3FDull, 1 },
            { 0x140F6A461ull, 1 },
            { 0x140F6A538ull, 1 },
            { 0x140F6A5A2ull, 1 },
            { 0x140F6A68Aull, 1 },
            { 0x140F6A6CAull, 1 },
            { 0x140F6A7AAull, 1 },
            { 0x140F6AD9Cull, 2 },
            { 0x140F6B27Eull, 1 },
            { 0x140F6B3C0ull, 1 },
            { 0x140F6B5B7ull, 1 },
            { 0x140F6B62Cull, 2 },
            { 0x140F6BDB5ull, 1 },
            { 0x140F6C025ull, 1 },
            { 0x140F6C245ull, 1 },
            { 0x140F6C475ull, 1 },
            { 0x140F6C625ull, 1 },
            { 0x140F6C7FCull, 2 },
            { 0x140F6C8F3ull, 2 },
            { 0x140F6C958ull, 1 },
            { 0x140F6C9F3ull, 1 },
            { 0x140F6CC00ull, 1 },
            { 0x140F6CC50ull, 2 },
            { 0x140F6CE73ull, 1 },
            { 0x140F6D581ull, 1 },
            { 0x140F6DDF5ull, 1 },
            { 0x140F6DEAFull, 1 },
            { 0x140F6E561ull, 1 },
            { 0x140F6E630ull, 1 },
            { 0x140F6E79Aull, 1 },
            { 0x140F6EB79ull, 1 },
            { 0x140F6EBC7ull, 2 },
            { 0x140F6EC2Eull, 1 },
            { 0x140F6EC5Full, 2 },
            { 0x140F6ECD4ull, 1 },
            { 0x140F6ED1Bull, 2 },
            { 0x140F6ED7Full, 1 },
            { 0x140F6EE00ull, 1 },
            { 0x140F6F6B2ull, 1 },
            { 0x140F6FFA5ull, 2 },
            { 0x140F71062ull, 3 },
        };
        }

        namespace tbl_en153
        {
        static const CtlDispSite kCtlDispSites[] = {
            { 0x140F655FCull, 0x1E008u, 255, 3 },
            { 0x140F65611ull, 0x1FE90u, 255, 3 },
            { 0x140F65650ull, 0x1FE78u, 255, 3 },
            { 0x140F65670ull, 0x1FE78u, 255, 3 },
            { 0x140F65680ull, 0x1FE88u, 255, 3 },
            { 0x140F65687ull, 0x1FEB0u, 255, 3 },
            { 0x140F656A3ull, 0x1FE90u, 255, 3 },
            { 0x140F656B6ull, 0x1FE90u, 255, 3 },
            { 0x140F656CFull, 0x1FE90u, 255, 3 },
            { 0x140F656D6ull, 0x1FEB0u, 255, 3 },
            { 0x140F656F4ull, 0x1E008u, 255, 3 },
            { 0x140F657BCull, 0x1E010u, 255, 3 },
            { 0x140F65B35ull, 0x1E028u, 255, 3 },
            { 0x140F65C17ull, 0x1E028u, 255, 3 },
            { 0x140F65CAAull, 0x1E028u, 255, 3 },
            { 0x140F65E05ull, 0x1E008u, 255, 3 },
            { 0x140F65EA1ull, 0x1E008u, 255, 3 },
            { 0x140F65F13ull, 0x1E008u, 255, 3 },
            { 0x140F67562ull, 0x1E008u, 255, 3 },
            { 0x140F675A3ull, 0x1E008u, 255, 3 },
            { 0x140F67606ull, 0x1E008u, 255, 3 },
            { 0x140F676CFull, 0x1E008u, 255, 3 },
            { 0x140F67940ull, 0x1FE50u, 255, 3 },
            { 0x140F67975ull, 0x1FE50u, 255, 3 },
            { 0x140F679A3ull, 0x1FE48u, 255, 3 },
            { 0x140F679DDull, 0x1FE58u, 255, 3 },
            { 0x140F67C4Aull, 0x1FE40u, 255, 3 },
            { 0x140F67C70ull, 0x1FE40u, 255, 3 },
            { 0x140F67C9Cull, 0x1FE48u, 255, 3 },
            { 0x140F67CE6ull, 0x1FE48u, 255, 3 },
            { 0x140F67CF6ull, 0x1FE48u, 255, 3 },
            { 0x140F67D06ull, 0x1FE48u, 255, 3 },
            { 0x140F67D16ull, 0x1FE48u, 255, 3 },
            { 0x140F67D26ull, 0x1FE48u, 255, 3 },
            { 0x140F67D36ull, 0x1FE48u, 255, 3 },
            { 0x140F67D7Cull, 0x1FE48u, 255, 3 },
            { 0x140F67D8Cull, 0x1FE48u, 255, 3 },
            { 0x140F67D9Cull, 0x1FE48u, 255, 3 },
            { 0x140F67DACull, 0x1FE48u, 255, 3 },
            { 0x140F67DBCull, 0x1FE48u, 255, 3 },
            { 0x140F67DCCull, 0x1FE48u, 255, 3 },
            { 0x140F67E05ull, 0x1FE50u, 255, 3 },
            { 0x140F67E4Aull, 0x1FE50u, 255, 3 },
            { 0x140F67E80ull, 0x1FE48u, 255, 3 },
            { 0x140F67EC3ull, 0x1FE58u, 255, 3 },
            { 0x140F67F1Cull, 0x1FE70u, 255, 3 },
            { 0x140F690B0ull, 0x1FE48u, 255, 3 },
            { 0x140F690CDull, 0x1FE48u, 255, 3 },
            { 0x140F690EAull, 0x1FE48u, 255, 3 },
            { 0x140F69107ull, 0x1FE48u, 255, 3 },
            { 0x140F69124ull, 0x1FE48u, 255, 3 },
            { 0x140F69141ull, 0x1FE48u, 255, 3 },
            { 0x140F6915Eull, 0x1FE48u, 255, 3 },
            { 0x140F6917Bull, 0x1FE48u, 255, 3 },
            { 0x140F695FCull, 0x1FE40u, 255, 3 },
            { 0x140F6965Cull, 0x1FE48u, 255, 3 },
            { 0x140F696A8ull, 0x1FE48u, 255, 3 },
            { 0x140F696B8ull, 0x1FE48u, 255, 3 },
            { 0x140F696C8ull, 0x1FE48u, 255, 3 },
            { 0x140F696D8ull, 0x1FE48u, 255, 3 },
            { 0x140F696E8ull, 0x1FE48u, 255, 3 },
            { 0x140F696F8ull, 0x1FE48u, 255, 3 },
            { 0x140F6974Aull, 0x1FE48u, 255, 3 },
            { 0x140F6975Aull, 0x1FE48u, 255, 3 },
            { 0x140F6976Aull, 0x1FE48u, 255, 3 },
            { 0x140F6977Aull, 0x1FE48u, 255, 3 },
            { 0x140F6978Aull, 0x1FE48u, 255, 3 },
            { 0x140F6979Aull, 0x1FE48u, 255, 3 },
            { 0x140F697DFull, 0x1FE50u, 255, 3 },
            { 0x140F6982Eull, 0x1FE50u, 255, 3 },
            { 0x140F6986Bull, 0x1FE48u, 255, 3 },
            { 0x140F698B4ull, 0x1FE58u, 255, 3 },
            { 0x140F69908ull, 0x1FE70u, 255, 3 },
            { 0x140F69CDCull, 0x1FE50u, 255, 3 },
            { 0x140F69D18ull, 0x1FE50u, 255, 3 },
            { 0x140F69D59ull, 0x1FE50u, 255, 3 },
            { 0x140F69D87ull, 0x1FE50u, 255, 3 },
            { 0x140F69E33ull, 0x1FE50u, 255, 3 },
            { 0x140F69E79ull, 0x1FE50u, 255, 3 },
            { 0x140F69EB5ull, 0x1FE50u, 255, 3 },
            { 0x140F69EE9ull, 0x1FE50u, 255, 3 },
            { 0x140F6A5B2ull, 0x1E008u, 255, 3 },
            { 0x140F6C55Eull, 0x1FE50u, 255, 3 },
            { 0x140F6C5A6ull, 0x1FE50u, 255, 3 },
            { 0x140F6C5D9ull, 0x1FE50u, 255, 3 },
            { 0x140F6C67Eull, 0x1FE50u, 255, 3 },
            { 0x140F6C6C3ull, 0x1FE50u, 255, 3 },
            { 0x140F6C6F7ull, 0x1FE50u, 255, 3 },
            { 0x140F6C999ull, 0x1FE40u, 255, 3 },
            { 0x140F6C9FDull, 0x1FE50u, 255, 3 },
            { 0x140F6CA37ull, 0x1FE50u, 255, 3 },
            { 0x140F6CA5Dull, 0x1FE50u, 255, 3 },
            { 0x140F6CAF4ull, 0x1FE50u, 255, 3 },
            { 0x140F6CB2Bull, 0x1FE50u, 255, 3 },
            { 0x140F6CB52ull, 0x1FE50u, 255, 3 },
            { 0x140F6D053ull, 0x1E008u, 255, 3 },
            { 0x140F6D6DAull, 0x1FE40u, 255, 3 },
            { 0x140F6D741ull, 0x1E008u, 255, 3 },
            { 0x140F6D782ull, 0x1E008u, 255, 3 },
            { 0x140F6D941ull, 0x1FE58u, 255, 3 },
            { 0x140F6D9AAull, 0x1FE58u, 255, 3 },
            { 0x140F6DA13ull, 0x1FE48u, 255, 3 },
            { 0x140F6DD73ull, 0x1E010u, 255, 3 },
            { 0x140F6DFC8ull, 0x1E010u, 255, 3 },
            { 0x140F6E688ull, 0x1E008u, 255, 3 },
            { 0x140F6ED9Full, 0x1E008u, 255, 3 },
            { 0x140F6EDEBull, 0x1FFA8u, 255, 3 },
            { 0x140F6EE10ull, 0x1FE68u, 255, 3 },
            { 0x140F6EE2Cull, 0x1FE68u, 255, 3 },
            { 0x140F6EE4Bull, 0x1FE68u, 255, 3 },
            { 0x140F6EE60ull, 0x1FE60u, 255, 3 },
            { 0x140F6F223ull, 0x1E030u, 255, 3 },
            { 0x140F6F242ull, 0x1E030u, 255, 3 },
            { 0x140F6FF8Eull, 0x1FECEu, 255, 3 },
            { 0x140F6FF9Eull, 0x1FEC8u, 255, 5 },
            { 0x140F6FFB4ull, 0x1FEB0u, 255, 3 },
            { 0x140F6FFE8ull, 0x1FE98u, 255, 3 },
            { 0x140F7000Dull, 0x1FE98u, 255, 3 },
            { 0x140F70019ull, 0x1FEA8u, 255, 3 },
            { 0x140F70020ull, 0x1FED0u, 255, 3 },
            { 0x140F7003Cull, 0x1FEB0u, 255, 3 },
            { 0x140F70052ull, 0x1FEB0u, 255, 3 },
            { 0x140F7006Bull, 0x1FEB0u, 255, 3 },
            { 0x140F70072ull, 0x1FED0u, 255, 3 },
            { 0x140F7009Bull, 0x1FECEu, 255, 3 },
            { 0x140F7014Full, 0x1FEA8u, 255, 5 },
            { 0x140F701F6ull, 0x1FEAEu, 255, 4 },
            { 0x140F70206ull, 0x1FE90u, 255, 3 },
            { 0x140F7022Dull, 0x1FE78u, 255, 3 },
            { 0x140F70250ull, 0x1FE78u, 255, 3 },
            { 0x140F70260ull, 0x1FE88u, 255, 3 },
            { 0x140F70267ull, 0x1FEB0u, 255, 3 },
            { 0x140F70283ull, 0x1FE90u, 255, 3 },
            { 0x140F70296ull, 0x1FE90u, 255, 3 },
            { 0x140F702AFull, 0x1FE90u, 255, 3 },
            { 0x140F702B6ull, 0x1FEB0u, 255, 3 },
            { 0x140F702F0ull, 0x1FEAEu, 255, 4 },
            { 0x140F70339ull, 0x1FE40u, 255, 3 },
            { 0x140F7037Aull, 0x1FEA8u, 255, 3 },
            { 0x140F706C6ull, 0x1E018u, 255, 3 },
            { 0x1494F93D3ull, 0x1E028u, 255, 3 },
            { 0x1494F93DAull, 0x1E030u, 255, 3 },
            { 0x1494F93E1ull, 0x1E038u, 255, 3 },
            { 0x1494F9403ull, 0x1FE58u, 255, 3 },
            { 0x1494F940Aull, 0x1FE60u, 255, 3 },
            { 0x1494F9411ull, 0x1FE68u, 255, 3 },
            { 0x1494F9418ull, 0x1FE70u, 255, 3 },
            { 0x1494F941Full, 0x1FE78u, 255, 3 },
            { 0x1494F9426ull, 0x1FE80u, 255, 3 },
            { 0x1494F942Dull, 0x1FE88u, 255, 3 },
            { 0x1494F9434ull, 0x1FE90u, 255, 3 },
            { 0x1494F943Bull, 0x1FE98u, 255, 3 },
            { 0x1494F9442ull, 0x1FEA0u, 255, 3 },
            { 0x1494F9449ull, 0x1FEA8u, 255, 3 },
            { 0x1494F9450ull, 0x1FEB0u, 255, 3 },
            { 0x1494F9457ull, 0x1FEB8u, 255, 3 },
            { 0x1494F945Eull, 0x1FEC0u, 255, 3 },
            { 0x1494F9465ull, 0x1FEC8u, 255, 3 },
            { 0x1494F946Cull, 0x1FFC5u, 255, 4 },
            { 0x1494F9474ull, 0x1FFC7u, 255, 3 },
            { 0x1494F947Bull, 0x1FFC8u, 255, 3 },
            { 0x1494F948Aull, 0x1E038u, 255, 3 },
            { 0x1494F9496ull, 0x1FED0u, 255, 3 },
            { 0x1494F94ACull, 0x1FF50u, 255, 3 },
            { 0x1494F94B3ull, 0x1FF58u, 255, 3 },
            { 0x1494F94BAull, 0x1FF60u, 255, 3 },
            { 0x1494F94C1ull, 0x1FF68u, 255, 3 },
            { 0x1494F94C8ull, 0x1FF70u, 255, 3 },
            { 0x1494F94CFull, 0x1FF78u, 255, 3 },
            { 0x1494F94D6ull, 0x1FF80u, 255, 3 },
            { 0x1494F94DDull, 0x1FF88u, 255, 3 },
            { 0x1494F94E4ull, 0x1FF90u, 255, 3 },
            { 0x1494F94EBull, 0x1FF98u, 2, 2 },
            { 0x1494F94F1ull, 0x1FF9Cu, 255, 3 },
            { 0x1494F9502ull, 0x1FF9Eu, 255, 3 },
            { 0x1494F9509ull, 0x1FFA6u, 255, 3 },
            { 0x1494F9510ull, 0x1FFAEu, 255, 3 },
            { 0x1494F9517ull, 0x1FFB6u, 255, 3 },
            { 0x1494F951Eull, 0x1FFBEu, 2, 2 },
            { 0x1494F9524ull, 0x1FFC2u, 255, 3 },
            { 0x1494F952Bull, 0x1FFC4u, 2, 2 },
            { 0x1494F9551ull, 0x1FEC0u, 255, 3 },
            { 0x1494FA22Bull, 0x1FEC0u, 255, 3 },
            { 0x1494FA662ull, 0x1FEC0u, 255, 3 },
            { 0x1494FB942ull, 0x1FEC0u, 255, 3 },
            { 0x1494FBECBull, 0x1E008u, 255, 3 },
            { 0x1494FBEE7ull, 0x1FE58u, 255, 3 },
            { 0x1494FBF06ull, 0x1FE58u, 255, 3 },
            { 0x1494FC720ull, 0x1FFA8u, 255, 3 },
            { 0x1494FC960ull, 0x1E018u, 255, 3 },
            { 0x1494FDF31ull, 0x1E008u, 255, 3 },
            { 0x1494FE65Bull, 0x1E008u, 255, 3 },
            { 0x1494FE88Bull, 0x1E008u, 255, 3 },
            { 0x1494FEB88ull, 0x1E008u, 255, 3 },
            { 0x1494FEBE7ull, 0x1E008u, 255, 3 },
            { 0x1495028D0ull, 0x1E030u, 255, 3 },
            { 0x149502913ull, 0x1E030u, 255, 3 },
            { 0x1495029ABull, 0x1FE60u, 255, 4 },
            { 0x1495029C6ull, 0x1FE60u, 255, 3 },
            { 0x149506C80ull, 0x1FE40u, 255, 4 },
            { 0x149506C99ull, 0x1FE40u, 255, 3 },
            { 0x149508CD0ull, 0x1FEA8u, 255, 4 },
            { 0x149509120ull, 0x1FE80u, 255, 3 },
            { 0x149509136ull, 0x1FFA6u, 2, 2 },
            { 0x14950913Cull, 0x1FF7Eu, 255, 4 },
            { 0x149509146ull, 0x1FF30u, 255, 4 },
            { 0x149509273ull, 0x1FF30u, 255, 5 },
            { 0x149509286ull, 0x1FF7Eu, 255, 5 },
            { 0x14950951Full, 0x1E008u, 255, 3 },
            { 0x149509907ull, 0x1E008u, 255, 3 },
            { 0x1495099CAull, 0x1E008u, 255, 3 },
            { 0x149509E45ull, 0x1E028u, 255, 3 },
            { 0x14950A146ull, 0x1E008u, 255, 3 },
            { 0x14950AAE5ull, 0x1FE40u, 255, 4 },
            { 0x14950AB30ull, 0x1FE40u, 255, 3 },
            { 0x14950AD20ull, 0x1E008u, 255, 3 },
            { 0x14950B3C7ull, 0x1E010u, 255, 3 },
            { 0x14950BB8Bull, 0x1E008u, 255, 3 },
            { 0x14950BD48ull, 0x1E030u, 255, 3 },
            { 0x14950E7A0ull, 0x1E008u, 255, 3 },
            { 0x149513B69ull, 0x1E028u, 255, 3 },
            { 0x149513BFCull, 0x1E028u, 255, 3 },
            { 0x149514111ull, 0x1E028u, 255, 3 },
            { 0x1495141B0ull, 0x1E028u, 255, 3 },
            { 0x149515A2Dull, 0x1FE40u, 255, 3 },
            { 0x149517EBDull, 0x1E010u, 255, 3 },
            { 0x1495180E5ull, 0x1E010u, 255, 3 },
            { 0x1495187FEull, 0x1E018u, 255, 3 },
            { 0x149518ED0ull, 0x1FE18u, 255, 3 },
            { 0x149519230ull, 0x1E018u, 255, 3 },
            { 0x1495194E0ull, 0x1E018u, 255, 3 },
            { 0x14951CD90ull, 0x1FEACu, 255, 3 },
            { 0x14951CEA5ull, 0x1E008u, 255, 3 },
            { 0x14951D1D8ull, 0x1E030u, 255, 3 },
            { 0x14951D1E5ull, 0x1E028u, 255, 3 },
            { 0x14951D447ull, 0x1E028u, 255, 3 },
            { 0x14951D773ull, 0x1FE50u, 255, 3 },
            { 0x14951D799ull, 0x1FE50u, 255, 3 },
            { 0x14951D7D9ull, 0x1FE50u, 255, 3 },
            { 0x14951D7F7ull, 0x1FE50u, 255, 3 },
            { 0x14951E4B0ull, 0x1FEADu, 255, 3 },
            { 0x14951EBB1ull, 0x1E008u, 255, 3 },
            { 0x14951F4C5ull, 0x1FE78u, 255, 3 },
            { 0x14951F4EBull, 0x1FE68u, 255, 3 },
            { 0x14951F860ull, 0x1E008u, 255, 3 },
            { 0x14951F8D0ull, 0x1E008u, 255, 3 },
            { 0x14951FC80ull, 0x1E008u, 255, 3 },
            { 0x1495200B0ull, 0x1FEA0u, 255, 3 },
            { 0x1495206BAull, 0x1E008u, 255, 3 },
            { 0x149520A90ull, 0x1E008u, 255, 3 },
            { 0x149521391ull, 0x1FFC6u, 255, 3 },
            { 0x149521432ull, 0x1FEA0u, 255, 3 },
            { 0x149521496ull, 0x1FEA0u, 255, 3 },
            { 0x149521760ull, 0x1FFA5u, 255, 3 },
            { 0x1495222B0ull, 0x1FFA8u, 255, 3 },
            { 0x149522B90ull, 0x1E018u, 255, 3 },
            { 0x149522DA2ull, 0x1FFA7u, 255, 3 },
            { 0x149522DABull, 0x1E008u, 255, 3 },
            { 0x1495231BDull, 0x1FE40u, 255, 3 },
            { 0x1495236FFull, 0x1FE50u, 255, 4 },
            { 0x1495249ECull, 0x1E008u, 255, 3 },
            { 0x149524A5Eull, 0x1FEACu, 2, 2 },
            { 0x149524A66ull, 0x1FEACu, 2, 2 },
            { 0x149525420ull, 0x1FFA8u, 255, 3 },
            { 0x149525610ull, 0x1E008u, 255, 3 },
            { 0x1495261C0ull, 0x1FEADu, 2, 2 },
            { 0x149526A18ull, 0x1E008u, 255, 3 },
            { 0x149526A27ull, 0x1E008u, 255, 3 },
            { 0x149526B50ull, 0x1E008u, 255, 3 },
            { 0x1495270F7ull, 0x1FEA0u, 255, 3 },
            { 0x1495272A8ull, 0x1E008u, 255, 3 },
            { 0x1495272B8ull, 0x1E008u, 255, 3 },
            { 0x1495272D0ull, 0x1FF7Eu, 255, 3 },
            { 0x1495272D7ull, 0x1FF86u, 255, 3 },
            { 0x1495272DEull, 0x1FF8Eu, 255, 3 },
            { 0x1495272E5ull, 0x1FF96u, 255, 3 },
            { 0x1495272ECull, 0x1FF9Eu, 255, 3 },
            { 0x1495272F3ull, 0x1FFA2u, 255, 4 },
            { 0x1495272FBull, 0x1FFA4u, 255, 3 },
            { 0x149527764ull, 0x1E008u, 255, 3 },
            { 0x149527D38ull, 0x1E008u, 255, 3 },
            { 0x149527D47ull, 0x1E008u, 255, 3 },
            { 0x1495280DFull, 0x1E030u, 255, 3 },
            { 0x149528410ull, 0x1FFA5u, 2, 2 },
            { 0x1495289A0ull, 0x1FE48u, 255, 3 },
            { 0x1495289AAull, 0x1FE38u, 255, 6 },
            { 0x1495289BBull, 0x1FE38u, 255, 3 },
            { 0x1495289CAull, 0x1FE18u, 255, 4 },
            { 0x1495289D2ull, 0x1FE38u, 255, 3 },
            { 0x1495289D9ull, 0x1FE48u, 255, 3 },
            { 0x1495289E7ull, 0x1FE38u, 255, 3 },
            { 0x1495289F6ull, 0x1FE18u, 255, 4 },
            { 0x1495289FEull, 0x1FE38u, 255, 3 },
            { 0x149528A05ull, 0x1FE48u, 255, 3 },
            { 0x149528A13ull, 0x1FE38u, 255, 3 },
            { 0x149528A22ull, 0x1FE18u, 255, 4 },
            { 0x149528A2Aull, 0x1FE38u, 255, 3 },
            { 0x149528A31ull, 0x1FE48u, 255, 3 },
            { 0x149528A3Full, 0x1FE38u, 255, 3 },
            { 0x149528A4Eull, 0x1FE18u, 255, 4 },
            { 0x149528A56ull, 0x1FE38u, 255, 3 },
            { 0x149528A5Dull, 0x1FE48u, 255, 3 },
            { 0x149528A6Bull, 0x1FE38u, 255, 3 },
            { 0x149528A7Aull, 0x1FE18u, 255, 4 },
            { 0x149528A82ull, 0x1FE38u, 255, 3 },
            { 0x149528A89ull, 0x1FE48u, 255, 3 },
            { 0x149528A97ull, 0x1FE38u, 255, 3 },
            { 0x149528AA6ull, 0x1FE18u, 255, 4 },
            { 0x149528AAEull, 0x1FE38u, 255, 3 },
            { 0x149528AB5ull, 0x1FE48u, 255, 3 },
            { 0x149528AC3ull, 0x1FE38u, 255, 3 },
            { 0x149528AD2ull, 0x1FE18u, 255, 4 },
            { 0x149528ADAull, 0x1FE38u, 255, 3 },
            { 0x149528AE1ull, 0x1FE48u, 255, 3 },
            { 0x149528AEFull, 0x1FE38u, 255, 3 },
            { 0x149528AFEull, 0x1FE18u, 255, 4 },
            { 0x149528B06ull, 0x1FE38u, 255, 3 },
            { 0x149528B0Dull, 0x1FE48u, 255, 3 },
            { 0x149528B25ull, 0x1FE3Cu, 255, 3 },
            { 0x149529246ull, 0x1E030u, 255, 3 },
            { 0x1495294D6ull, 0x1E030u, 255, 3 },
            { 0x1495298E2ull, 0x1E030u, 255, 3 },
            { 0x149529D12ull, 0x1E030u, 255, 3 },
            { 0x14952A0C1ull, 0x1E028u, 255, 3 },
            { 0x14952A2A0ull, 0x1FE40u, 255, 3 },
            { 0x14952A2AEull, 0x1FE48u, 255, 3 },
            { 0x14952A2BCull, 0x1FE50u, 255, 3 },
            { 0x14952A2CAull, 0x1FE58u, 255, 3 },
            { 0x14952A2D8ull, 0x1FE60u, 255, 3 },
            { 0x14952A2E3ull, 0x1FE68u, 255, 3 },
            { 0x14952A2F1ull, 0x1FE70u, 255, 3 },
            { 0x14952A30Aull, 0x1FE78u, 255, 3 },
            { 0x14952A319ull, 0x1FE80u, 255, 3 },
            { 0x14952A329ull, 0x1FE88u, 255, 3 },
            { 0x14952A338ull, 0x1FE90u, 255, 3 },
            { 0x14952A350ull, 0x1FE80u, 255, 3 },
            { 0x14952A357ull, 0x1FE98u, 255, 3 },
            { 0x14952A36Cull, 0x1FFA6u, 2, 2 },
            { 0x14952A678ull, 0x1FE40u, 255, 3 },
            { 0x14952A68Dull, 0x1FFA5u, 255, 3 },
            { 0x14952A694ull, 0x1FFA7u, 2, 2 },
            { 0x14952A69Cull, 0x1FEADu, 255, 3 },
            { 0x14952A6A3ull, 0x1FEA8u, 255, 4 },
            { 0x14952A6ABull, 0x1FEA8u, 255, 4 },
            { 0x14952A89Bull, 0x1E010u, 255, 3 },
            { 0x14952A919ull, 0x1E010u, 255, 3 },
            { 0x14952A94Aull, 0x1E010u, 255, 3 },
            { 0x14952A95Cull, 0x1E010u, 255, 3 },
            { 0x14952A97Cull, 0x1E010u, 255, 3 },
            { 0x14952A98Bull, 0x1E010u, 255, 3 },
            { 0x14952A9B2ull, 0x1E010u, 255, 3 },
            { 0x14952A9C4ull, 0x1E010u, 255, 3 },
            { 0x14952A9DFull, 0x1E010u, 255, 3 },
            { 0x14952A9EEull, 0x1E010u, 255, 3 },
            { 0x14952AFC4ull, 0x1FE90u, 255, 3 },
            { 0x14952AFDEull, 0x1FE88u, 255, 3 },
            { 0x14952AFE5ull, 0x1FEB0u, 255, 3 },
            { 0x14952B004ull, 0x1FE90u, 255, 3 },
            { 0x14952B022ull, 0x1FE90u, 255, 3 },
            { 0x14952B04Aull, 0x1FE90u, 255, 3 },
            { 0x14952B05Dull, 0x1FE90u, 255, 3 },
            { 0x14952B064ull, 0x1FEB0u, 255, 3 },
        };

        static const BoundImmSite kBoundImmSites[] = {
            { 0x140F65B05ull, 1 },
            { 0x140F65C99ull, 1 },
            { 0x140F65D60ull, 1 },
            { 0x140F65DDBull, 1 },
            { 0x140F65F06ull, 1 },
            { 0x140F65F3Bull, 1 },
            { 0x140F670CAull, 1 },
            { 0x140F67531ull, 2 },
            { 0x140F6771Cull, 1 },
            { 0x140F67860ull, 1 },
            { 0x140F67A55ull, 1 },
            { 0x140F67C0Eull, 1 },
            { 0x140F67F9Aull, 1 },
            { 0x140F680BBull, 1 },
            { 0x140F6856Aull, 1 },
            { 0x140F695AEull, 1 },
            { 0x140F69C5Dull, 1 },
            { 0x140F6B9AEull, 1 },
            { 0x140F6BCE7ull, 1 },
            { 0x140F6C4E5ull, 1 },
            { 0x140F6C755ull, 1 },
            { 0x140F6C975ull, 1 },
            { 0x140F6CBA5ull, 1 },
            { 0x140F6CD55ull, 1 },
            { 0x140F6D023ull, 2 },
            { 0x140F6D123ull, 1 },
            { 0x140F6D8EAull, 1 },
            { 0x140F6DCB1ull, 1 },
            { 0x140F6E5DFull, 1 },
            { 0x140F706D5ull, 2 },
            { 0x1494FC2CDull, 1 },
            { 0x1494FDEEBull, 1 },
            { 0x1494FE629ull, 2 },
            { 0x1494FE859ull, 2 },
            { 0x1494FEB4Dull, 2 },
            { 0x149500D30ull, 1 },
            { 0x149501264ull, 1 },
            { 0x149501474ull, 1 },
            { 0x149503934ull, 1 },
            { 0x149503DE4ull, 1 },
            { 0x149504154ull, 1 },
            { 0x149505E89ull, 2 },
            { 0x149508EA0ull, 1 },
            { 0x1495098DAull, 2 },
            { 0x14950B31Aull, 1 },
            { 0x14950BB63ull, 2 },
            { 0x14950BF87ull, 2 },
            { 0x14950CE80ull, 1 },
            { 0x14950DB70ull, 1 },
            { 0x14950DCA2ull, 2 },
            { 0x14950DE80ull, 1 },
            { 0x14950E77Full, 1 },
            { 0x14950ED54ull, 1 },
            { 0x14950FF24ull, 1 },
            { 0x1495100FAull, 2 },
            { 0x149510BD4ull, 2 },
            { 0x14951222Dull, 1 },
            { 0x14951231Aull, 1 },
            { 0x14951259Dull, 1 },
            { 0x149512A3Dull, 1 },
            { 0x1495132D0ull, 1 },
            { 0x1495135B8ull, 1 },
            { 0x149513B42ull, 1 },
            { 0x149513C2Aull, 1 },
            { 0x1495140E9ull, 1 },
            { 0x1495141CAull, 1 },
            { 0x14951612Bull, 2 },
            { 0x149516692ull, 2 },
            { 0x14951880Cull, 2 },
            { 0x14951AE20ull, 1 },
            { 0x14951B7FBull, 2 },
            { 0x14951CE8Cull, 2 },
            { 0x14951D418ull, 1 },
            { 0x14951E0D0ull, 1 },
            { 0x14951EB70ull, 2 },
            { 0x14951F464ull, 1 },
            { 0x1495200A3ull, 1 },
            { 0x149523C55ull, 1 },
            { 0x149524FC5ull, 1 },
            { 0x149525E81ull, 1 },
            { 0x1495264F0ull, 1 },
            { 0x1495270DAull, 1 },
            { 0x1495284C9ull, 1 },
            { 0x149528517ull, 2 },
            { 0x14952857Eull, 1 },
            { 0x1495285ABull, 2 },
            { 0x149528620ull, 1 },
            { 0x149528667ull, 2 },
            { 0x1495286CFull, 1 },
            { 0x149528750ull, 1 },
            { 0x14952A0A8ull, 1 },
        };
        }

        namespace tbl_jp153
        {
        static const CtlDispSite kCtlDispSites[] = {
            { 0x140F656ECull, 0x1E008u, 255, 3 },
            { 0x140F65701ull, 0x1FE90u, 255, 3 },
            { 0x140F65740ull, 0x1FE78u, 255, 3 },
            { 0x140F65760ull, 0x1FE78u, 255, 3 },
            { 0x140F65770ull, 0x1FE88u, 255, 3 },
            { 0x140F65777ull, 0x1FEB0u, 255, 3 },
            { 0x140F65793ull, 0x1FE90u, 255, 3 },
            { 0x140F657A6ull, 0x1FE90u, 255, 3 },
            { 0x140F657BFull, 0x1FE90u, 255, 3 },
            { 0x140F657C6ull, 0x1FEB0u, 255, 3 },
            { 0x140F657E4ull, 0x1E008u, 255, 3 },
            { 0x140F65C25ull, 0x1E028u, 255, 3 },
            { 0x140F65D07ull, 0x1E028u, 255, 3 },
            { 0x140F65D9Aull, 0x1E028u, 255, 3 },
            { 0x140F65EF5ull, 0x1E008u, 255, 3 },
            { 0x140F65F91ull, 0x1E008u, 255, 3 },
            { 0x140F66003ull, 0x1E008u, 255, 3 },
            { 0x140F67662ull, 0x1E008u, 255, 3 },
            { 0x140F676A3ull, 0x1E008u, 255, 3 },
            { 0x140F67706ull, 0x1E008u, 255, 3 },
            { 0x140F677CFull, 0x1E008u, 255, 3 },
            { 0x140F67A40ull, 0x1FE50u, 255, 3 },
            { 0x140F67A75ull, 0x1FE50u, 255, 3 },
            { 0x140F67AA3ull, 0x1FE48u, 255, 3 },
            { 0x140F67ADDull, 0x1FE58u, 255, 3 },
            { 0x140F67D4Aull, 0x1FE40u, 255, 3 },
            { 0x140F67D70ull, 0x1FE40u, 255, 3 },
            { 0x140F67D9Cull, 0x1FE48u, 255, 3 },
            { 0x140F67DE6ull, 0x1FE48u, 255, 3 },
            { 0x140F67DF6ull, 0x1FE48u, 255, 3 },
            { 0x140F67E06ull, 0x1FE48u, 255, 3 },
            { 0x140F67E16ull, 0x1FE48u, 255, 3 },
            { 0x140F67E26ull, 0x1FE48u, 255, 3 },
            { 0x140F67E36ull, 0x1FE48u, 255, 3 },
            { 0x140F67E7Cull, 0x1FE48u, 255, 3 },
            { 0x140F67E8Cull, 0x1FE48u, 255, 3 },
            { 0x140F67E9Cull, 0x1FE48u, 255, 3 },
            { 0x140F67EACull, 0x1FE48u, 255, 3 },
            { 0x140F67EBCull, 0x1FE48u, 255, 3 },
            { 0x140F67ECCull, 0x1FE48u, 255, 3 },
            { 0x140F67F05ull, 0x1FE50u, 255, 3 },
            { 0x140F67F4Aull, 0x1FE50u, 255, 3 },
            { 0x140F67F80ull, 0x1FE48u, 255, 3 },
            { 0x140F67FC3ull, 0x1FE58u, 255, 3 },
            { 0x140F6801Cull, 0x1FE70u, 255, 3 },
            { 0x140F691B0ull, 0x1FE48u, 255, 3 },
            { 0x140F691CDull, 0x1FE48u, 255, 3 },
            { 0x140F691EAull, 0x1FE48u, 255, 3 },
            { 0x140F69207ull, 0x1FE48u, 255, 3 },
            { 0x140F69224ull, 0x1FE48u, 255, 3 },
            { 0x140F69241ull, 0x1FE48u, 255, 3 },
            { 0x140F6925Eull, 0x1FE48u, 255, 3 },
            { 0x140F6927Bull, 0x1FE48u, 255, 3 },
            { 0x140F696FCull, 0x1FE40u, 255, 3 },
            { 0x140F6975Cull, 0x1FE48u, 255, 3 },
            { 0x140F697A8ull, 0x1FE48u, 255, 3 },
            { 0x140F697B8ull, 0x1FE48u, 255, 3 },
            { 0x140F697C8ull, 0x1FE48u, 255, 3 },
            { 0x140F697D8ull, 0x1FE48u, 255, 3 },
            { 0x140F697E8ull, 0x1FE48u, 255, 3 },
            { 0x140F697F8ull, 0x1FE48u, 255, 3 },
            { 0x140F6984Aull, 0x1FE48u, 255, 3 },
            { 0x140F6985Aull, 0x1FE48u, 255, 3 },
            { 0x140F6986Aull, 0x1FE48u, 255, 3 },
            { 0x140F6987Aull, 0x1FE48u, 255, 3 },
            { 0x140F6988Aull, 0x1FE48u, 255, 3 },
            { 0x140F6989Aull, 0x1FE48u, 255, 3 },
            { 0x140F698DFull, 0x1FE50u, 255, 3 },
            { 0x140F6992Eull, 0x1FE50u, 255, 3 },
            { 0x140F6996Bull, 0x1FE48u, 255, 3 },
            { 0x140F699B4ull, 0x1FE58u, 255, 3 },
            { 0x140F69A08ull, 0x1FE70u, 255, 3 },
            { 0x140F69DDCull, 0x1FE50u, 255, 3 },
            { 0x140F69E18ull, 0x1FE50u, 255, 3 },
            { 0x140F69E59ull, 0x1FE50u, 255, 3 },
            { 0x140F69E87ull, 0x1FE50u, 255, 3 },
            { 0x140F69F33ull, 0x1FE50u, 255, 3 },
            { 0x140F69F79ull, 0x1FE50u, 255, 3 },
            { 0x140F69FB5ull, 0x1FE50u, 255, 3 },
            { 0x140F69FE9ull, 0x1FE50u, 255, 3 },
            { 0x140F6A6B2ull, 0x1E008u, 255, 3 },
            { 0x140F6C65Eull, 0x1FE50u, 255, 3 },
            { 0x140F6C6A6ull, 0x1FE50u, 255, 3 },
            { 0x140F6C6D9ull, 0x1FE50u, 255, 3 },
            { 0x140F6C77Eull, 0x1FE50u, 255, 3 },
            { 0x140F6C7C3ull, 0x1FE50u, 255, 3 },
            { 0x140F6C7F7ull, 0x1FE50u, 255, 3 },
            { 0x140F6CA99ull, 0x1FE40u, 255, 3 },
            { 0x140F6CAFDull, 0x1FE50u, 255, 3 },
            { 0x140F6CB37ull, 0x1FE50u, 255, 3 },
            { 0x140F6CB5Dull, 0x1FE50u, 255, 3 },
            { 0x140F6CBF4ull, 0x1FE50u, 255, 3 },
            { 0x140F6CC2Bull, 0x1FE50u, 255, 3 },
            { 0x140F6CC52ull, 0x1FE50u, 255, 3 },
            { 0x140F6D153ull, 0x1E008u, 255, 3 },
            { 0x140F6D7DAull, 0x1FE40u, 255, 3 },
            { 0x140F6D841ull, 0x1E008u, 255, 3 },
            { 0x140F6D882ull, 0x1E008u, 255, 3 },
            { 0x140F6DA41ull, 0x1FE58u, 255, 3 },
            { 0x140F6DAAAull, 0x1FE58u, 255, 3 },
            { 0x140F6DB13ull, 0x1FE48u, 255, 3 },
            { 0x140F6DE73ull, 0x1E010u, 255, 3 },
            { 0x140F6E0C8ull, 0x1E010u, 255, 3 },
            { 0x140F6E778ull, 0x1E008u, 255, 3 },
            { 0x140F6F313ull, 0x1E030u, 255, 3 },
            { 0x140F6F332ull, 0x1E030u, 255, 3 },
            { 0x140F7007Eull, 0x1FECEu, 255, 3 },
            { 0x140F7008Eull, 0x1FEC8u, 255, 5 },
            { 0x140F700A4ull, 0x1FEB0u, 255, 3 },
            { 0x140F700D8ull, 0x1FE98u, 255, 3 },
            { 0x140F700FDull, 0x1FE98u, 255, 3 },
            { 0x140F70109ull, 0x1FEA8u, 255, 3 },
            { 0x140F70110ull, 0x1FED0u, 255, 3 },
            { 0x140F7012Cull, 0x1FEB0u, 255, 3 },
            { 0x140F70142ull, 0x1FEB0u, 255, 3 },
            { 0x140F7015Bull, 0x1FEB0u, 255, 3 },
            { 0x140F70162ull, 0x1FED0u, 255, 3 },
            { 0x140F7018Bull, 0x1FECEu, 255, 3 },
            { 0x140F7023Full, 0x1FEA8u, 255, 5 },
            { 0x140F702E6ull, 0x1FEAEu, 255, 4 },
            { 0x140F702F6ull, 0x1FE90u, 255, 3 },
            { 0x140F7031Dull, 0x1FE78u, 255, 3 },
            { 0x140F70340ull, 0x1FE78u, 255, 3 },
            { 0x140F70350ull, 0x1FE88u, 255, 3 },
            { 0x140F70357ull, 0x1FEB0u, 255, 3 },
            { 0x140F70373ull, 0x1FE90u, 255, 3 },
            { 0x140F70386ull, 0x1FE90u, 255, 3 },
            { 0x140F7039Full, 0x1FE90u, 255, 3 },
            { 0x140F703A6ull, 0x1FEB0u, 255, 3 },
            { 0x140F703E0ull, 0x1FEAEu, 255, 4 },
            { 0x140F70429ull, 0x1FE40u, 255, 3 },
            { 0x140F7046Aull, 0x1FEA8u, 255, 3 },
            { 0x140F707B6ull, 0x1E018u, 255, 3 },
            { 0x149F14D53ull, 0x1E028u, 255, 3 },
            { 0x149F14D5Aull, 0x1E030u, 255, 3 },
            { 0x149F14D61ull, 0x1E038u, 255, 3 },
            { 0x149F14D83ull, 0x1FE58u, 255, 3 },
            { 0x149F14D8Aull, 0x1FE60u, 255, 3 },
            { 0x149F14D91ull, 0x1FE68u, 255, 3 },
            { 0x149F14D98ull, 0x1FE70u, 255, 3 },
            { 0x149F14D9Full, 0x1FE78u, 255, 3 },
            { 0x149F14DA6ull, 0x1FE80u, 255, 3 },
            { 0x149F14DADull, 0x1FE88u, 255, 3 },
            { 0x149F14DB4ull, 0x1FE90u, 255, 3 },
            { 0x149F14DBBull, 0x1FE98u, 255, 3 },
            { 0x149F14DC2ull, 0x1FEA0u, 255, 3 },
            { 0x149F14DC9ull, 0x1FEA8u, 255, 3 },
            { 0x149F14DD0ull, 0x1FEB0u, 255, 3 },
            { 0x149F14DD7ull, 0x1FEB8u, 255, 3 },
            { 0x149F14DDEull, 0x1FEC0u, 255, 3 },
            { 0x149F14DE5ull, 0x1FEC8u, 255, 3 },
            { 0x149F14DECull, 0x1FFC5u, 255, 4 },
            { 0x149F14DF4ull, 0x1FFC7u, 255, 3 },
            { 0x149F14DFBull, 0x1FFC8u, 255, 3 },
            { 0x149F14E0Aull, 0x1E038u, 255, 3 },
            { 0x149F14E16ull, 0x1FED0u, 255, 3 },
            { 0x149F14E2Cull, 0x1FF50u, 255, 3 },
            { 0x149F14E33ull, 0x1FF58u, 255, 3 },
            { 0x149F14E3Aull, 0x1FF60u, 255, 3 },
            { 0x149F14E41ull, 0x1FF68u, 255, 3 },
            { 0x149F14E48ull, 0x1FF70u, 255, 3 },
            { 0x149F14E4Full, 0x1FF78u, 255, 3 },
            { 0x149F14E56ull, 0x1FF80u, 255, 3 },
            { 0x149F14E5Dull, 0x1FF88u, 255, 3 },
            { 0x149F14E64ull, 0x1FF90u, 255, 3 },
            { 0x149F14E6Bull, 0x1FF98u, 2, 2 },
            { 0x149F14E71ull, 0x1FF9Cu, 255, 3 },
            { 0x149F14E82ull, 0x1FF9Eu, 255, 3 },
            { 0x149F14E89ull, 0x1FFA6u, 255, 3 },
            { 0x149F14E90ull, 0x1FFAEu, 255, 3 },
            { 0x149F14E97ull, 0x1FFB6u, 255, 3 },
            { 0x149F14E9Eull, 0x1FFBEu, 2, 2 },
            { 0x149F14EA4ull, 0x1FFC2u, 255, 3 },
            { 0x149F14EABull, 0x1FFC4u, 2, 2 },
            { 0x149F14ED1ull, 0x1FEC0u, 255, 3 },
            { 0x149F1532Bull, 0x1FEC0u, 255, 3 },
            { 0x149F159D2ull, 0x1FEC0u, 255, 3 },
            { 0x149F15ED2ull, 0x1FEC0u, 255, 3 },
            { 0x149F1613Bull, 0x1E008u, 255, 3 },
            { 0x149F16157ull, 0x1FE58u, 255, 3 },
            { 0x149F16176ull, 0x1FE58u, 255, 3 },
            { 0x149F168D0ull, 0x1FFA8u, 255, 3 },
            { 0x149F16C10ull, 0x1E018u, 255, 3 },
            { 0x149F16F0Cull, 0x1E010u, 255, 3 },
            { 0x149F178C1ull, 0x1E008u, 255, 3 },
            { 0x149F17E7Bull, 0x1E008u, 255, 3 },
            { 0x149F1826Bull, 0x1E008u, 255, 3 },
            { 0x149F18788ull, 0x1E008u, 255, 3 },
            { 0x149F187E7ull, 0x1E008u, 255, 3 },
            { 0x149F1B9F0ull, 0x1E030u, 255, 3 },
            { 0x149F1BA33ull, 0x1E030u, 255, 3 },
            { 0x149F1BACBull, 0x1FE60u, 255, 4 },
            { 0x149F1BAE6ull, 0x1FE60u, 255, 3 },
            { 0x149F1F5E0ull, 0x1FE40u, 255, 4 },
            { 0x149F1F5F9ull, 0x1FE40u, 255, 3 },
            { 0x149F21500ull, 0x1FEA8u, 255, 4 },
            { 0x149F219F0ull, 0x1FE80u, 255, 3 },
            { 0x149F21A06ull, 0x1FFA6u, 2, 2 },
            { 0x149F21A0Cull, 0x1FF7Eu, 255, 4 },
            { 0x149F21A16ull, 0x1FF30u, 255, 4 },
            { 0x149F21B43ull, 0x1FF30u, 255, 5 },
            { 0x149F21B56ull, 0x1FF7Eu, 255, 5 },
            { 0x149F2211Full, 0x1E008u, 255, 3 },
            { 0x149F22E87ull, 0x1E008u, 255, 3 },
            { 0x149F2335Aull, 0x1E008u, 255, 3 },
            { 0x149F23525ull, 0x1E028u, 255, 3 },
            { 0x149F23956ull, 0x1E008u, 255, 3 },
            { 0x149F23B95ull, 0x1FE40u, 255, 4 },
            { 0x149F23BE0ull, 0x1FE40u, 255, 3 },
            { 0x149F24290ull, 0x1E008u, 255, 3 },
            { 0x149F244D7ull, 0x1E010u, 255, 3 },
            { 0x149F24A2Bull, 0x1E008u, 255, 3 },
            { 0x149F24BB8ull, 0x1E030u, 255, 3 },
            { 0x149F285F0ull, 0x1E008u, 255, 3 },
            { 0x149F2CF09ull, 0x1E028u, 255, 3 },
            { 0x149F2CF9Cull, 0x1E028u, 255, 3 },
            { 0x149F2D141ull, 0x1E028u, 255, 3 },
            { 0x149F2D1E0ull, 0x1E028u, 255, 3 },
            { 0x149F2E9CDull, 0x1FE40u, 255, 3 },
            { 0x149F3110Dull, 0x1E010u, 255, 3 },
            { 0x149F315B5ull, 0x1E010u, 255, 3 },
            { 0x149F3186Eull, 0x1E018u, 255, 3 },
            { 0x149F31B60ull, 0x1FE18u, 255, 3 },
            { 0x149F31CC0ull, 0x1E018u, 255, 3 },
            { 0x149F31F40ull, 0x1E018u, 255, 3 },
            { 0x149F35790ull, 0x1FEACu, 255, 3 },
            { 0x149F35A85ull, 0x1E008u, 255, 3 },
            { 0x149F35D88ull, 0x1E030u, 255, 3 },
            { 0x149F35D95ull, 0x1E028u, 255, 3 },
            { 0x149F361C7ull, 0x1E028u, 255, 3 },
            { 0x149F36323ull, 0x1FE50u, 255, 3 },
            { 0x149F36349ull, 0x1FE50u, 255, 3 },
            { 0x149F36389ull, 0x1FE50u, 255, 3 },
            { 0x149F363A7ull, 0x1FE50u, 255, 3 },
            { 0x149F37920ull, 0x1FEADu, 255, 3 },
            { 0x149F384A1ull, 0x1E008u, 255, 3 },
            { 0x149F394E5ull, 0x1FE78u, 255, 3 },
            { 0x149F3950Bull, 0x1FE68u, 255, 3 },
            { 0x149F39670ull, 0x1E008u, 255, 3 },
            { 0x149F398D0ull, 0x1E008u, 255, 3 },
            { 0x149F39CF0ull, 0x1E008u, 255, 3 },
            { 0x149F39EF0ull, 0x1FEA0u, 255, 3 },
            { 0x149F3B1BAull, 0x1E008u, 255, 3 },
            { 0x149F3B7C0ull, 0x1E008u, 255, 3 },
            { 0x149F3C8D1ull, 0x1FFC6u, 255, 3 },
            { 0x149F3C972ull, 0x1FEA0u, 255, 3 },
            { 0x149F3C9D6ull, 0x1FEA0u, 255, 3 },
            { 0x149F3CE30ull, 0x1FFA5u, 255, 3 },
            { 0x149F3E120ull, 0x1FFA8u, 255, 3 },
            { 0x149F3EE10ull, 0x1E018u, 255, 3 },
            { 0x149F3F152ull, 0x1FFA7u, 255, 3 },
            { 0x149F3F15Bull, 0x1E008u, 255, 3 },
            { 0x149F3F64Dull, 0x1FE40u, 255, 3 },
            { 0x149F3FD6Full, 0x1FE50u, 255, 4 },
            { 0x149F404FCull, 0x1E008u, 255, 3 },
            { 0x149F4056Eull, 0x1FEACu, 2, 2 },
            { 0x149F40576ull, 0x1FEACu, 2, 2 },
            { 0x149F40B70ull, 0x1FFA8u, 255, 3 },
            { 0x149F40D00ull, 0x1E008u, 255, 3 },
            { 0x149F41B50ull, 0x1FEADu, 2, 2 },
            { 0x149F42DFFull, 0x1E008u, 255, 3 },
            { 0x149F42E51ull, 0x1FFA8u, 255, 3 },
            { 0x149F42E76ull, 0x1FE68u, 255, 3 },
            { 0x149F42E92ull, 0x1FE68u, 255, 3 },
            { 0x149F42EB1ull, 0x1FE68u, 255, 3 },
            { 0x149F42EC6ull, 0x1FE60u, 255, 3 },
            { 0x149F43188ull, 0x1E008u, 255, 3 },
            { 0x149F43197ull, 0x1E008u, 255, 3 },
            { 0x149F434E0ull, 0x1E008u, 255, 3 },
            { 0x149F43977ull, 0x1FEA0u, 255, 3 },
            { 0x149F44898ull, 0x1E008u, 255, 3 },
            { 0x149F448A8ull, 0x1E008u, 255, 3 },
            { 0x149F448C0ull, 0x1FF7Eu, 255, 3 },
            { 0x149F448C7ull, 0x1FF86u, 255, 3 },
            { 0x149F448CEull, 0x1FF8Eu, 255, 3 },
            { 0x149F448D5ull, 0x1FF96u, 255, 3 },
            { 0x149F448DCull, 0x1FF9Eu, 255, 3 },
            { 0x149F448E3ull, 0x1FFA2u, 255, 4 },
            { 0x149F448EBull, 0x1FFA4u, 255, 3 },
            { 0x149F44B84ull, 0x1E008u, 255, 3 },
            { 0x149F44BC2ull, 0x1FFA8u, 255, 3 },
            { 0x149F44F48ull, 0x1E008u, 255, 3 },
            { 0x149F44F57ull, 0x1E008u, 255, 3 },
            { 0x149F450CFull, 0x1E030u, 255, 3 },
            { 0x149F45230ull, 0x1FFA5u, 2, 2 },
            { 0x149F461C0ull, 0x1FE48u, 255, 3 },
            { 0x149F461CAull, 0x1FE38u, 255, 6 },
            { 0x149F461DBull, 0x1FE38u, 255, 3 },
            { 0x149F461EAull, 0x1FE18u, 255, 4 },
            { 0x149F461F2ull, 0x1FE38u, 255, 3 },
            { 0x149F461F9ull, 0x1FE48u, 255, 3 },
            { 0x149F46207ull, 0x1FE38u, 255, 3 },
            { 0x149F46216ull, 0x1FE18u, 255, 4 },
            { 0x149F4621Eull, 0x1FE38u, 255, 3 },
            { 0x149F46225ull, 0x1FE48u, 255, 3 },
            { 0x149F46233ull, 0x1FE38u, 255, 3 },
            { 0x149F46242ull, 0x1FE18u, 255, 4 },
            { 0x149F4624Aull, 0x1FE38u, 255, 3 },
            { 0x149F46251ull, 0x1FE48u, 255, 3 },
            { 0x149F4625Full, 0x1FE38u, 255, 3 },
            { 0x149F4626Eull, 0x1FE18u, 255, 4 },
            { 0x149F46276ull, 0x1FE38u, 255, 3 },
            { 0x149F4627Dull, 0x1FE48u, 255, 3 },
            { 0x149F4628Bull, 0x1FE38u, 255, 3 },
            { 0x149F4629Aull, 0x1FE18u, 255, 4 },
            { 0x149F462A2ull, 0x1FE38u, 255, 3 },
            { 0x149F462A9ull, 0x1FE48u, 255, 3 },
            { 0x149F462B7ull, 0x1FE38u, 255, 3 },
            { 0x149F462C6ull, 0x1FE18u, 255, 4 },
            { 0x149F462CEull, 0x1FE38u, 255, 3 },
            { 0x149F462D5ull, 0x1FE48u, 255, 3 },
            { 0x149F462E3ull, 0x1FE38u, 255, 3 },
            { 0x149F462F2ull, 0x1FE18u, 255, 4 },
            { 0x149F462FAull, 0x1FE38u, 255, 3 },
            { 0x149F46301ull, 0x1FE48u, 255, 3 },
            { 0x149F4630Full, 0x1FE38u, 255, 3 },
            { 0x149F4631Eull, 0x1FE18u, 255, 4 },
            { 0x149F46326ull, 0x1FE38u, 255, 3 },
            { 0x149F4632Dull, 0x1FE48u, 255, 3 },
            { 0x149F46345ull, 0x1FE3Cu, 255, 3 },
            { 0x149F464A6ull, 0x1E030u, 255, 3 },
            { 0x149F466A6ull, 0x1E030u, 255, 3 },
            { 0x149F46A62ull, 0x1E030u, 255, 3 },
            { 0x149F46CFEull, 0x1E030u, 255, 3 },
            { 0x149F47811ull, 0x1E028u, 255, 3 },
            { 0x149F47E50ull, 0x1FE40u, 255, 3 },
            { 0x149F47E5Eull, 0x1FE48u, 255, 3 },
            { 0x149F47E6Cull, 0x1FE50u, 255, 3 },
            { 0x149F47E7Aull, 0x1FE58u, 255, 3 },
            { 0x149F47E88ull, 0x1FE60u, 255, 3 },
            { 0x149F47E93ull, 0x1FE68u, 255, 3 },
            { 0x149F47EA1ull, 0x1FE70u, 255, 3 },
            { 0x149F47EBAull, 0x1FE78u, 255, 3 },
            { 0x149F47EC9ull, 0x1FE80u, 255, 3 },
            { 0x149F47ED9ull, 0x1FE88u, 255, 3 },
            { 0x149F47EE8ull, 0x1FE90u, 255, 3 },
            { 0x149F47F00ull, 0x1FE80u, 255, 3 },
            { 0x149F47F07ull, 0x1FE98u, 255, 3 },
            { 0x149F47F1Cull, 0x1FFA6u, 2, 2 },
            { 0x149F47FE8ull, 0x1FE40u, 255, 3 },
            { 0x149F47FFDull, 0x1FFA5u, 255, 3 },
            { 0x149F48004ull, 0x1FFA7u, 2, 2 },
            { 0x149F4800Cull, 0x1FEADu, 255, 3 },
            { 0x149F48013ull, 0x1FEA8u, 255, 4 },
            { 0x149F4801Bull, 0x1FEA8u, 255, 4 },
            { 0x149F4818Bull, 0x1E010u, 255, 3 },
            { 0x149F48209ull, 0x1E010u, 255, 3 },
            { 0x149F4823Aull, 0x1E010u, 255, 3 },
            { 0x149F4824Cull, 0x1E010u, 255, 3 },
            { 0x149F4826Cull, 0x1E010u, 255, 3 },
            { 0x149F4827Bull, 0x1E010u, 255, 3 },
            { 0x149F482A2ull, 0x1E010u, 255, 3 },
            { 0x149F482B4ull, 0x1E010u, 255, 3 },
            { 0x149F482CFull, 0x1E010u, 255, 3 },
            { 0x149F482DEull, 0x1E010u, 255, 3 },
            { 0x149F485B4ull, 0x1FE90u, 255, 3 },
            { 0x149F485CEull, 0x1FE88u, 255, 3 },
            { 0x149F485D5ull, 0x1FEB0u, 255, 3 },
            { 0x149F485F4ull, 0x1FE90u, 255, 3 },
            { 0x149F48612ull, 0x1FE90u, 255, 3 },
            { 0x149F4863Aull, 0x1FE90u, 255, 3 },
            { 0x149F4864Dull, 0x1FE90u, 255, 3 },
            { 0x149F48654ull, 0x1FEB0u, 255, 3 },
        };

        static const BoundImmSite kBoundImmSites[] = {
            { 0x140F65BF5ull, 1 },
            { 0x140F65D89ull, 1 },
            { 0x140F65E50ull, 1 },
            { 0x140F65ECBull, 1 },
            { 0x140F65FF6ull, 1 },
            { 0x140F6602Bull, 1 },
            { 0x140F671CAull, 1 },
            { 0x140F67631ull, 2 },
            { 0x140F6781Cull, 1 },
            { 0x140F67960ull, 1 },
            { 0x140F67B55ull, 1 },
            { 0x140F67D0Eull, 1 },
            { 0x140F6809Aull, 1 },
            { 0x140F681BBull, 1 },
            { 0x140F6866Aull, 1 },
            { 0x140F696AEull, 1 },
            { 0x140F69D5Dull, 1 },
            { 0x140F6BAAEull, 1 },
            { 0x140F6BDE7ull, 1 },
            { 0x140F6C5E5ull, 1 },
            { 0x140F6C855ull, 1 },
            { 0x140F6CA75ull, 1 },
            { 0x140F6CCA5ull, 1 },
            { 0x140F6CE55ull, 1 },
            { 0x140F6D123ull, 2 },
            { 0x140F6D223ull, 1 },
            { 0x140F6D9EAull, 1 },
            { 0x140F6DDB1ull, 1 },
            { 0x140F6E6CFull, 1 },
            { 0x140F707C5ull, 2 },
            { 0x149F167ADull, 1 },
            { 0x149F1787Bull, 1 },
            { 0x149F17E49ull, 2 },
            { 0x149F18239ull, 2 },
            { 0x149F1874Dull, 2 },
            { 0x149F1A8E0ull, 1 },
            { 0x149F1AFE4ull, 1 },
            { 0x149F1B3D4ull, 1 },
            { 0x149F1CAC4ull, 1 },
            { 0x149F1CFA4ull, 1 },
            { 0x149F1D0A4ull, 1 },
            { 0x149F1EA49ull, 2 },
            { 0x149F217D0ull, 1 },
            { 0x149F22E5Aull, 2 },
            { 0x149F2442Aull, 1 },
            { 0x149F24A03ull, 2 },
            { 0x149F24E07ull, 2 },
            { 0x149F269F0ull, 1 },
            { 0x149F27330ull, 1 },
            { 0x149F27682ull, 2 },
            { 0x149F27BE0ull, 1 },
            { 0x149F285CFull, 1 },
            { 0x149F28B74ull, 1 },
            { 0x149F2A5E4ull, 1 },
            { 0x149F2A83Aull, 2 },
            { 0x149F2ABA4ull, 2 },
            { 0x149F2BCDDull, 1 },
            { 0x149F2C2DAull, 1 },
            { 0x149F2C4FDull, 1 },
            { 0x149F2CA0Dull, 1 },
            { 0x149F2CBD0ull, 1 },
            { 0x149F2CD08ull, 1 },
            { 0x149F2CEE2ull, 1 },
            { 0x149F2CFCAull, 1 },
            { 0x149F2D119ull, 1 },
            { 0x149F2D1FAull, 1 },
            { 0x149F2F87Bull, 2 },
            { 0x149F2FBC2ull, 2 },
            { 0x149F3187Cull, 2 },
            { 0x149F33BF0ull, 1 },
            { 0x149F348DBull, 2 },
            { 0x149F35A6Cull, 2 },
            { 0x149F36198ull, 1 },
            { 0x149F37690ull, 1 },
            { 0x149F38460ull, 2 },
            { 0x149F39484ull, 1 },
            { 0x149F39EE3ull, 1 },
            { 0x149F401C5ull, 1 },
            { 0x149F407B5ull, 1 },
            { 0x149F41561ull, 1 },
            { 0x149F42BE0ull, 1 },
            { 0x149F4395Aull, 1 },
            { 0x149F45439ull, 1 },
            { 0x149F45487ull, 2 },
            { 0x149F454EEull, 1 },
            { 0x149F4551Bull, 2 },
            { 0x149F45590ull, 1 },
            { 0x149F455D7ull, 2 },
            { 0x149F4563Full, 1 },
            { 0x149F456C0ull, 1 },
            { 0x149F477F8ull, 1 },
        };
        }

        template <std::size_t A, std::size_t B>
        constexpr std::size_t MaxOf() { return A > B ? A : B; }

        constexpr std::size_t kMaxCtlSites =
            MaxOf<MaxOf<sizeof(tbl_en154::kCtlDispSites)
                            / sizeof(tbl_en154::kCtlDispSites[0]),
                        sizeof(tbl_jp154::kCtlDispSites)
                            / sizeof(tbl_jp154::kCtlDispSites[0])>(),
                  MaxOf<sizeof(tbl_en153::kCtlDispSites)
                            / sizeof(tbl_en153::kCtlDispSites[0]),
                        sizeof(tbl_jp153::kCtlDispSites)
                            / sizeof(tbl_jp153::kCtlDispSites[0])>()>();

        const CtlDispSite*  kCtlDispSites  = nullptr;
        const BoundImmSite* kBoundImmSites = nullptr;
        std::size_t         kCtlSiteCount   = 0;
        std::size_t         kBoundSiteCount = 0;

        std::uint8_t g_CtlDispOff[kMaxCtlSites] = {};

        CtlDispSite   g_CtlMut[kMaxCtlSites] = {};
        std::intptr_t g_CloneDelta[4]        = {};
        std::size_t   g_CloneDeltaCount      = 0;

        const std::uintptr_t kSweepFpEn154[] = {
            0x140FE5295, 0x140FFE8C8, 0x14127A055, 0x14127A37E,
            0x141311057, 0x1413110C5, 0x141A582B5, 0x142643B6D,
            0x142687645, 0x1426B4C85, 0x142747999, 0x1427480D5,
            0x14274839D, 0x1430A6147, 0x1430B5DBE, 0x1430BA26F,
            0x1431034EF, 0x14310566B, 0x14310B923, 0x1434C8182,
            0x148C7381E, 0x149A7361C, 0x149A7CBC8,
            0x14308066F, 0x14308C91B, 0x14308F67B, 0x14309FCCF,
            0x1457E24E7, 0x145AAD062, 0x149C8B054, 0x149CAB88C,
            0x149CAE708, 0x149CE09D8, 0x149CF0EF8, 0x149CF9170,
            0x149D7C1C9,
        };

        const std::uintptr_t kSweepFpJp154[] = { 0 };

        struct BuildAddrs
        {
            std::uintptr_t patch[kPatchCount];
            std::uintptr_t sibFix[kSiblingLoopFixCount];
            std::uint8_t   sibFixOld[kSiblingLoopFixCount][4];
            std::uintptr_t blockReset;
            std::uintptr_t setDeveloped;
            std::uintptr_t setUndeveloped;
            std::uintptr_t findByDevId;
            std::uintptr_t findByEquipId;
            std::uintptr_t listDevCount;
            std::uintptr_t listDevFill;
            std::uintptr_t equipIdCount;
            std::uintptr_t equipIdRow;
            std::uintptr_t getBaseId;
            std::uintptr_t siblingCount;
            std::uintptr_t isVisible;
            std::uintptr_t quarkAlloc;
            std::uintptr_t blockVtblA;
            std::uintptr_t blockVtblB;
            std::uintptr_t base20VtblA;
            std::uintptr_t base20VtblB;
            std::uintptr_t edcDtor;

            const CtlDispSite*    ctl;
            std::size_t           ctlCount;
            const BoundImmSite*   bnd;
            std::size_t           bndCount;
            const std::uintptr_t* sweepFp;
            std::size_t           sweepFpCount;
        };

        const BuildAddrs kAddrsEn154 = {
            { 0x140F65084, 0x140F64908, 0x140F6496F, 0x140F6C82B,
              0x140F6C83A, 0x140F6C843, 0x140F6E105, 0x140F6E117,
              0x140F6E135, 0x140F6E140, 0x140F6E14B, 0x140F6E156,
              0x140F69240, 0x140F6D15A, 0x140F68DF0, 0x140F68CB0,
              0x140F68C75, 0x140F6AA62, 0x140F6A9BB, 0x140F6CD04 },
            { 0x140F693DA, 0x140F69400, 0x140F6944C, 0x140F6944F },
            { { 0x40, 0x32, 0xF6 }, { 0x40, 0x0F, 0xB6, 0xC6 },
              { 0x40, 0xFE, 0xC6 },   { 0x40, 0x0F, 0xB6, 0xC6 } },
            0x140F6C820, 0x140F6E600, 0x140F6E800,
            0x140F68CD0, 0x140F68D00,
            0x140F65930, 0x140F657E0,
            0x140F6B020, 0x140F6B430, 0x140F65C80,
            0x140F6A660, 0x140F6D150,
            0x140BFF0E0,
            0x14239B460, 0x14239BCD0, 0x14239B530, 0x14239BDA0,
            0x140F64BC0,
            tbl_en154::kCtlDispSites,
            sizeof(tbl_en154::kCtlDispSites)
                / sizeof(tbl_en154::kCtlDispSites[0]),
            tbl_en154::kBoundImmSites,
            sizeof(tbl_en154::kBoundImmSites)
                / sizeof(tbl_en154::kBoundImmSites[0]),
            kSweepFpEn154,
            sizeof(kSweepFpEn154) / sizeof(kSweepFpEn154[0]),
        };

        const BuildAddrs kAddrsJp154 = {
            { 0x140F650E4, 0x140F64968, 0x140F649CF, 0x140F6C88B,
              0x140F6C89A, 0x140F6C8A3, 0x140F6E165, 0x140F6E177,
              0x140F6E195, 0x140F6E1A0, 0x140F6E1AB, 0x140F6E1B6,
              0x140F692A0, 0x140F6D1BA, 0x140F68E50, 0x140F68D10,
              0x140F68CD5, 0x140F6AAC2, 0x140F6AA1B, 0x140F6CD64 },
            { 0x140F6943A, 0x140F69460, 0x140F694AC, 0x140F694AF },
            { { 0x40, 0x32, 0xF6 }, { 0x40, 0x0F, 0xB6, 0xC6 },
              { 0x40, 0xFE, 0xC6 },   { 0x40, 0x0F, 0xB6, 0xC6 } },
            0x140F6C880, 0x140F6E660, 0x140F6E860,
            0x140F68D30, 0x140F68D60,
            0x140F65990, 0x140F65840,
            0x140F6B080, 0x140F6B490, 0x140F65CE0,
            0x140F6A6C0, 0x140F6D1B0,
            0x140BFF060,
            0x14239B770, 0x14239BFE0, 0x14239B840, 0x14239C0B0,
            0x140F64C20,
            tbl_jp154::kCtlDispSites,
            sizeof(tbl_jp154::kCtlDispSites)
                / sizeof(tbl_jp154::kCtlDispSites[0]),
            tbl_jp154::kBoundImmSites,
            sizeof(tbl_jp154::kBoundImmSites)
                / sizeof(tbl_jp154::kBoundImmSites[0]),
            kSweepFpJp154, 0,
        };

        const std::uintptr_t kSweepFpEn153[] = { 0 };
        const std::uintptr_t kSweepFpJp153[] = { 0 };

        const BuildAddrs kAddrsEn153 = {
            { 0x1494FD523, 0x1494F9348, 0x1494F93AF, 0x14951D19A,
              0x14951D1A9, 0x14951D1B2, 0,           0x149524FD7,
              0x149524FF5, 0x149525000, 0x14952500B, 0x149525016,
              0, 0, 0, 0, 0, 0, 0, 0 },
            { 0x14950E83D, 0x14950E860, 0x14950E8AC, 0x14950E8AF },
            { { 0x40, 0x30, 0xF6 }, { 0x40, 0x0F, 0xB6, 0xC6 },
              { 0x40, 0xFE, 0xC6 },   { 0x40, 0x0F, 0xB6, 0xC6 } },
            0x14951D190, 0x140F6ED90, 0x149527750,
            0x14950D190, 0x14950D350,
            0x1494FEB30, 0x1494FE600,
            0x14951A1E0, 0x14951AF70, 0,
            0x1495140E0, 0x140F6D8E0,
            0x140BFF480,
            0x14239B5C0, 0x14239BE30, 0x14239B690, 0x14239BF00,
            0,
            tbl_en153::kCtlDispSites,
            sizeof(tbl_en153::kCtlDispSites)
                / sizeof(tbl_en153::kCtlDispSites[0]),
            tbl_en153::kBoundImmSites,
            sizeof(tbl_en153::kBoundImmSites)
                / sizeof(tbl_en153::kBoundImmSites[0]),
            kSweepFpEn153, 0,
        };

        const BuildAddrs kAddrsJp153 = {
            { 0x149F17083, 0x149F14CC8, 0x149F14D2F, 0x149F35D4A,
              0x149F35D59, 0x149F35D62, 0,           0x149F407C7,
              0x149F407E5, 0x149F407F0, 0x149F407FB, 0x149F40806,
              0, 0, 0, 0, 0, 0, 0, 0 },
            { 0x149F2868D, 0x149F286B0, 0x149F286FC, 0x149F286FF },
            { { 0x40, 0x30, 0xF6 }, { 0x40, 0x0F, 0xB6, 0xC6 },
              { 0x40, 0xFE, 0xC6 },   { 0x40, 0x0F, 0xB6, 0xC6 } },
            0x149F35D40, 0x149F42DF0, 0x149F44B70,
            0x149F26DD0, 0x140F69590,
            0x149F18730, 0x149F17E20,
            0x149F32A10, 0x149F33EC0, 0,
            0x149F2D110, 0x140F6D9E0,
            0x140BFF010,
            0x14239B5F0, 0x14239BE60, 0x14239B6C0, 0x14239BF30,
            0,
            tbl_jp153::kCtlDispSites,
            sizeof(tbl_jp153::kCtlDispSites)
                / sizeof(tbl_jp153::kCtlDispSites[0]),
            tbl_jp153::kBoundImmSites,
            sizeof(tbl_jp153::kBoundImmSites)
                / sizeof(tbl_jp153::kBoundImmSites[0]),
            kSweepFpJp153, 0,
        };

        const BuildAddrs* g_A = nullptr;

        bool SelectBuildTables()
        {
            switch (gGameBuild)
            {
            case AddressSetRuntime::GameBuild::En_1_0_15_4a:
            case AddressSetRuntime::GameBuild::En_1_0_15_4:
                g_A = &kAddrsEn154;
                break;
            case AddressSetRuntime::GameBuild::Jp_1_0_15_4a:
            case AddressSetRuntime::GameBuild::Jp_1_0_15_4:
                g_A = &kAddrsJp154;
                break;
            case AddressSetRuntime::GameBuild::En_1_0_15_3:
                g_A = &kAddrsEn153;
                break;
            case AddressSetRuntime::GameBuild::Jp_1_0_15_3:
                g_A = &kAddrsJp153;
                break;
            default:
                g_A = nullptr;
                return false;
            }
            for (std::size_t i = 0; i < kPatchCount; ++i)
            {
                kPatches[i]      = kPatchTemplate[i];
                kPatches[i].addr = g_A->patch[i];
            }
            for (std::size_t i = 0; i < kSiblingLoopFixCount; ++i)
            {
                kSiblingLoopFixes[i]      = kSiblingLoopFixTemplate[i];
                kSiblingLoopFixes[i].addr = g_A->sibFix[i];
                std::memcpy(kSiblingLoopFixes[i].oldBytes,
                            g_A->sibFixOld[i], 4);
            }
            for (std::size_t i = 0; i < g_A->ctlCount && i < kMaxCtlSites; ++i)
                g_CtlMut[i] = g_A->ctl[i];
            kCtlDispSites   = g_CtlMut;
            kCtlSiteCount   = g_A->ctlCount;
            kBoundImmSites  = g_A->bnd;
            kBoundSiteCount = g_A->bndCount;
            return true;
        }

        bool g_Active = false;

        enum class PrePatchState : int
        {
            NotAttempted = 0,
            Applied,
            WrongBuild,
            DisabledByMarker,
            BlockExists,
            VerifyFailed,
            ApplyFailed,
            RacedMidPatch,
            RetryPending,
        };
        PrePatchState g_PrePatch       = PrePatchState::NotAttempted;
        int           g_PrePatchDetail = -1;
        bool          g_Migrated       = false;
        std::uintptr_t g_GrownBlock    = 0;
        char          g_PreVerifyDetail[192] = {};

        bool GrowDisabledByMarker()
        {
            wchar_t path[MAX_PATH];
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return false;
            wchar_t* slash = std::wcsrchr(path, L'\\');
            if (!slash)
                return false;
            slash[1] = L'\0';
            if (std::wcslen(path) + 40 >= MAX_PATH)
                return false;
            wcscat_s(path, MAX_PATH, L"V_FrameWork_no_develop_grow.txt");
            return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        }

        void NoteVerifyFail(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(g_PreVerifyDetail, sizeof(g_PreVerifyDetail), fmt, args);
            va_end(args);
            LogDebug("[DevelopArrayGrow] %s\n", g_PreVerifyDetail);
        }

        std::atomic<std::uintptr_t> g_ArmedBase20{ 0 };

        constexpr std::size_t kFlagsShadowSize = 0x10000;
        static_assert(kFlagsShadowSize >= kNewRows, "shadow must cover all rows");
        std::uint8_t  g_FlagsShadow[kFlagsShadowSize] = {};
        std::uint8_t* g_SvarsFlags = nullptr;
        void*         g_BlockSeen  = nullptr;

        std::uint8_t  g_SyncMirror[kOldRows] = {};
        std::uint32_t g_LastSyncTick   = 0;
        bool          g_SyncPullLogged = false;
        bool          g_SyncReassertLogged = false;

        using BlockReset_t     = void (__fastcall*)(std::uintptr_t block);
        using SetDeveloped_t   = void (__fastcall*)(std::uintptr_t base20,
                                                    std::uint16_t idx);
        using SetUndeveloped_t = void (__fastcall*)(std::uintptr_t base20,
                                                    std::uint16_t idx, char notify);
        using FindFlow_t       = std::uint16_t (__fastcall*)(std::uintptr_t base20,
                                                             std::uint16_t key);
        using ListDevCount_t   = std::uint16_t (__fastcall*)(std::uintptr_t base20);
        using ListDevFill_t    = void (__fastcall*)(std::uintptr_t base20,
                                                    std::uint16_t maxCount,
                                                    std::uint16_t* outBuf);
        using RowListGate_t    = char (__fastcall*)(std::uintptr_t block,
                                                    std::uint16_t idx);
        using EquipIdQuery_t   = long long (__fastcall*)(std::uintptr_t base20,
                                                         std::uint16_t equipId);
        using RowQueryTail_t   = long long (__fastcall*)(std::uintptr_t block,
                                                         std::uint32_t equipId,
                                                         std::uint32_t foundIdx);
        using ExtCount_t       = int (__fastcall*)(std::uintptr_t self);
        using GetQuark_t       = std::uint8_t* (__fastcall*)();
        using GetBaseId_t      = std::uint16_t (__fastcall*)(std::uintptr_t base20,
                                                             std::uint16_t row);

        using EdcDtor_t = void* (__fastcall*)(void* self, unsigned int flags);

        BlockReset_t     g_OrigBlockReset     = nullptr;
        EdcDtor_t        g_OrigEdcDtor        = nullptr;
        SetDeveloped_t   g_OrigSetDeveloped   = nullptr;
        SetUndeveloped_t g_OrigSetUndeveloped = nullptr;
        FindFlow_t       g_OrigFindByDevId    = nullptr;
        FindFlow_t       g_OrigFindByEquipId  = nullptr;
        ListDevCount_t   g_OrigListDevCount   = nullptr;
        ListDevFill_t    g_OrigListDevFill    = nullptr;
        EquipIdQuery_t   g_OrigEquipIdCount   = nullptr;
        EquipIdQuery_t   g_OrigEquipIdRow     = nullptr;
        GetBaseId_t      g_OrigGetBaseId      = nullptr;

        void*            g_HookBlockReset     = nullptr;
        void*            g_HookSetDeveloped   = nullptr;
        void*            g_HookSetUndeveloped = nullptr;
        void*            g_HookFindByDevId    = nullptr;
        void*            g_HookFindByEquipId  = nullptr;
        void*            g_HookListDevCount   = nullptr;
        void*            g_HookListDevFill    = nullptr;
        void*            g_HookEquipIdCount   = nullptr;
        void*            g_HookEquipIdRow     = nullptr;
        void*            g_HookGetBaseId      = nullptr;
        void*            g_HookEdcDtor        = nullptr;
        bool             g_DtorGuardInstalled = false;
        bool             g_OrphanDtorSkipped  = false;

        void* __fastcall hkEdcDtorGuard(void* self, unsigned int flags)
        {
            if (g_GrownBlock
                && reinterpret_cast<std::uintptr_t>(self) != g_GrownBlock)
            {
                if (!g_OrphanDtorSkipped)
                {
                    g_OrphanDtorSkipped = true;
                    LogDebug("[DevelopArrayGrow] EquipDevelopController dtor skipped "
                             "on block %p (the grown block is %p) - the relocated "
                             "field displacements are sized for the grown allocation, "
                             "so running the dtor here would read past the end of this "
                             "smaller one; the pre-migration block is orphaned and was "
                             "never freed, so skipping it leaks nothing that was still "
                             "reachable\n",
                        self, reinterpret_cast<void*>(g_GrownBlock));
                }
                return self;
            }
            __try
            {
                return g_OrigEdcDtor ? g_OrigEdcDtor(self, flags) : self;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Log("[DevelopArrayGrow] EquipDevelopController dtor faulted on "
                    "the grown block %p - swallowed so shutdown finishes, but a "
                    "fault here means the teardown stopped part-way and the heap "
                    "may already be inconsistent\n",
                    self);
                return self;
            }
        }

#define kAddr_BlockReset     (g_A->blockReset)
#define kAddr_SetDeveloped   (g_A->setDeveloped)
#define kAddr_SetUndeveloped (g_A->setUndeveloped)
#define kAddr_FindByDevId    (g_A->findByDevId)
#define kAddr_FindByEquipId  (g_A->findByEquipId)
#define kAddr_EdcDtor        (g_A->edcDtor)
#define kAddr_ListDevCount   (g_A->listDevCount)
#define kAddr_ListDevFill    (g_A->listDevFill)
#define kAddr_EquipIdCount   (g_A->equipIdCount)
#define kAddr_EquipIdRow     (g_A->equipIdRow)
#define kAddr_GetBaseId      (g_A->getBaseId)
#define kAddr_SiblingCount   (g_A->siblingCount)
#define kAddr_IsVisible      (g_A->isVisible)

#define kAddr_QuarkBlockHeapAlloc (g_A->quarkAlloc)
#define kAddr_BlockVtblA          (g_A->blockVtblA)
#define kAddr_BlockVtblB          (g_A->blockVtblB)
#define kAddr_Base20VtblA         (g_A->base20VtblA)
#define kAddr_Base20VtblB         (g_A->base20VtblB)

        constexpr std::size_t kBlock_FlagsPtrOff  = kNewCtlOff;
        constexpr std::size_t kBase20_FlagsPtrOff = kNewCtlOff - 0x20;
        constexpr std::size_t kBase20_RecFieldOff = 0x8;

        constexpr std::uint8_t  kReqAnnouncedBit = 0x4;
        constexpr std::uint32_t kFirstCustomRow  = 922;

        std::atomic<std::uint32_t> g_GunsmithClaimMask{ 0 };

        struct AnnouncedRowWork
        {
            std::int32_t developId;
            std::int32_t flowIndex;
            bool         reqAnnounced;
        };

        void CollectAnnouncedRowWork(std::vector<AnnouncedRowWork>& out)
        {
            V_FrameWorkState::ForEachManagedDevelopRow(
                [&out](std::int32_t developId, std::int32_t flowIndex,
                       bool reqAnnounced)
                {
                    out.push_back({ developId, flowIndex, reqAnnounced });
                });
        }

        std::uint32_t ResolveAnnouncedRowSEH(std::int32_t developId)
        {
            if (!g_OrigFindByDevId || developId <= 0 || developId > 0xFFFF)
                return 0;
            const std::uintptr_t base20 =
                g_ArmedBase20.load(std::memory_order_relaxed);
            if (base20 == 0)
                return 0;
            __try
            {
                return g_OrigFindByDevId(
                    base20, static_cast<std::uint16_t>(developId));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        bool RowHoldsDevelopId(std::uint32_t row, std::int32_t developId)
        {
            const std::uintptr_t base20 =
                g_ArmedBase20.load(std::memory_order_relaxed);
            if (base20 == 0 || row >= kNewRows)
                return false;
            std::uint16_t devId = 0;
            __try
            {
                std::memcpy(&devId,
                    reinterpret_cast<const void*>(
                        base20 + kBase20_RecFieldOff
                               + static_cast<std::size_t>(row) * kRecStride),
                    sizeof(devId));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return devId == static_cast<std::uint16_t>(developId);
        }

        std::uint32_t AnnouncedRowOf(std::int32_t developId,
                                     std::int32_t persistedFlow)
        {
            const std::uint32_t persisted =
                static_cast<std::uint32_t>(persistedFlow);
            const bool persistedUsable =
                persistedFlow > 0 && persisted >= kFirstCustomRow
                && persisted < kNewRows && !IsReservedFlowRow(persisted);

            if (persistedUsable && RowHoldsDevelopId(persisted, developId))
                return persisted;

            const std::uint32_t live = ResolveAnnouncedRowSEH(developId);
            if (live >= kFirstCustomRow && live < kNewRows
                && !IsReservedFlowRow(live)
                && RowHoldsDevelopId(live, developId))
            {
                if (persistedUsable && persisted != live)
                {
                    static std::atomic<std::uint32_t> s_moved{ 0 };
                    if (s_moved.fetch_add(1, std::memory_order_relaxed) < 6)
                        LogDebug("[DevelopArrayGrow] developId %d sits at row %u, "
                                 "not the bookkept row %u - the announced bit "
                                 "follows the record\n",
                                 developId, live, persisted);
                }
                return live;
            }

            return persistedUsable ? persisted : 0;
        }

        void RestoreGrownAnnouncedBits()
        {
            std::vector<AnnouncedRowWork> work;
            CollectAnnouncedRowWork(work);

            std::vector<std::int32_t> nowAnnounced;
            for (const AnnouncedRowWork& w : work)
            {
                const std::uint32_t row =
                    AnnouncedRowOf(w.developId, w.flowIndex);
                if (row == 0)
                    continue;

                const bool initial = EquipDevelop_IsDevelopInitiallyAvailable(
                    static_cast<std::uint32_t>(w.developId));
                if (!w.reqAnnounced && !initial)
                {
                    g_FlagsShadow[row] &= static_cast<std::uint8_t>(
                        ~kReqAnnouncedBit);
                    continue;
                }
                g_FlagsShadow[row] |= kReqAnnouncedBit;
                if (!w.reqAnnounced)
                    nowAnnounced.push_back(w.developId);
            }

            for (std::int32_t developId : nowAnnounced)
                V_FrameWorkState::SetDevReqAnnouncedByDevelopId(developId, true);
        }

        void CaptureGrownAnnouncedBits()
        {
            std::vector<AnnouncedRowWork> work;
            CollectAnnouncedRowWork(work);

            std::vector<std::int32_t> justAnnounced;
            for (const AnnouncedRowWork& w : work)
            {
                if (w.reqAnnounced)
                    continue;
                const std::uint32_t row =
                    AnnouncedRowOf(w.developId, w.flowIndex);
                if (row == 0)
                    continue;
                if (!RowHoldsDevelopId(row, w.developId))
                    continue;
                if (g_FlagsShadow[row] & kReqAnnouncedBit)
                    justAnnounced.push_back(w.developId);
            }

            for (std::int32_t developId : justAnnounced)
                V_FrameWorkState::SetDevReqAnnouncedByDevelopId(developId, true);
        }

        void MirrorFlagByte(std::uint32_t idx)
        {
            InvalidateDevelopVisibilityCache();
            if (g_SvarsFlags && idx < kOldRows)
            {
                g_SvarsFlags[idx] = g_FlagsShadow[idx];
                g_SyncMirror[idx] = g_FlagsShadow[idx];
            }
        }

        std::mutex                g_SilencedMutex;
        std::vector<std::int32_t> g_SilencedDevelopIds;

        void MarkSilencedRowsAnnounced()
        {
            std::lock_guard<std::mutex> lock(g_SilencedMutex);
            for (std::int32_t developId : g_SilencedDevelopIds)
            {
                const std::uint32_t row = AnnouncedRowOf(developId, -1);
                if (row == 0)
                    continue;
                g_FlagsShadow[row] |= kReqAnnouncedBit;
                MirrorFlagByte(row);
            }
        }

        void MarkReservedRowsAnnounced()
        {
            const std::uint32_t claimed =
                g_GunsmithClaimMask.load(std::memory_order_relaxed);
            for (std::uint32_t row = kGunsmithFlowFirst;
                 row <= kGunsmithFlowLast; ++row)
            {
                if (((claimed >> (row - kGunsmithFlowFirst)) & 1u) == 0)
                    continue;
                g_FlagsShadow[row] |= kReqAnnouncedBit;
                MirrorFlagByte(row);
            }
        }

        void SyncFlagsWithSvars(bool force)
        {
            if (!g_Active || !g_SvarsFlags)
                return;
            if (!force)
            {
                const std::uint32_t now = GetTickCount();
                if (now == g_LastSyncTick)
                    return;
                g_LastSyncTick = now;
            }
            std::uint32_t pulled = 0;
            std::uint32_t reasserted = 0;
            __try
            {
                for (std::uint32_t i = 0; i < kOldRows; ++i)
                {
                    const std::uint8_t s = g_SvarsFlags[i];
                    const std::uint8_t h = g_FlagsShadow[i];
                    if (s == h)
                    {
                        g_SyncMirror[i] = s;
                        continue;
                    }
                    if (i >= kFirstCustomRow)
                    {
                        if (s != g_SyncMirror[i])
                            ++reasserted;
                        g_SvarsFlags[i] = h;
                        g_SyncMirror[i] = h;
                        continue;
                    }
                    if (s != g_SyncMirror[i])
                    {
                        g_FlagsShadow[i] = s;
                        g_SyncMirror[i]  = s;
                        ++pulled;
                    }
                    else
                    {
                        g_SvarsFlags[i] = h;
                        g_SyncMirror[i] = h;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[DevelopArrayGrow] flags sync: exception touching the "
                         "save develop-flag array at %p - vanilla develop state "
                         "stays stale until the next sync\n", g_SvarsFlags);
                return;
            }
            MarkReservedRowsAnnounced();
            MarkSilencedRowsAnnounced();
            CaptureGrownAnnouncedBits();
            RestoreGrownAnnouncedBits();
            if (pulled)
            {
                InvalidateDevelopVisibilityCache();
                EquipDevelop_RequestDevelopRestore();
            }
            if (pulled && !g_SyncPullLogged)
            {
                g_SyncPullLogged = true;
                LogDebug("[DevelopArrayGrow] flags sync: adopted %u develop flag(s) "
                         "the save load wrote behind the shadow\n",
                    pulled);
            }
            if (reasserted && !g_SyncReassertLogged)
            {
                g_SyncReassertLogged = true;
                LogDebug("[DevelopArrayGrow] flags sync: the save load rewrote %u "
                         "custom-band byte(s) (rows %u..%u) behind the shadow - the "
                         "save cannot carry custom rows, so state-file values were "
                         "re-asserted (adopting them empties the custom loadout "
                         "lists)\n",
                    reasserted, kFirstCustomRow, kOldRows - 1);
            }
        }

        bool WriteImmAt(const ImmPatch& p, std::uint32_t value)
        {
            std::uint8_t* addr = reinterpret_cast<std::uint8_t*>(
                const_cast<void*>(ResolveGameAddress(p.addr)));
            if (!addr)
                return false;
            DWORD oldProt = 0;
            if (!VirtualProtect(addr, p.opLen + 4, PAGE_EXECUTE_READWRITE,
                                &oldProt))
            {
                Log("[DevelopArrayGrow] site %s @%p: VirtualProtect failed "
                    "(%lu)\n", p.what, addr, GetLastError());
                return false;
            }
            std::memcpy(addr + p.opLen, &value, 4);
            DWORD tmp = 0;
            VirtualProtect(addr, p.opLen + 4, oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), addr, p.opLen + 4);
            return true;
        }

        bool IsWorkBufFreeLoad(std::uintptr_t dispVa, std::uint32_t oldDisp)
        {
            if (oldDisp != kWorkBufDisp)
                return false;
            const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(
                ResolveGameAddress(dispVa - 3));
            if (!p)
                return false;
            __try
            {
                return p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x89;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::uint32_t CtlNewDisp(std::uintptr_t dispVa, std::uint32_t oldDisp)
        {
            return IsWorkBufFreeLoad(dispVa, oldDisp)
                       ? kNullSinkOff
                       : oldDisp + kCtlDelta;
        }

        bool WriteU32At(std::uintptr_t siteAddr, std::uint32_t byteOff,
                        std::uint32_t value)
        {
            std::uint8_t* addr = reinterpret_cast<std::uint8_t*>(
                const_cast<void*>(ResolveGameAddress(siteAddr)));
            if (!addr)
                return false;
            addr += byteOff;
            DWORD oldProt = 0;
            if (!VirtualProtect(addr, 4, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                Log("[DevelopArrayGrow] site @%p: VirtualProtect failed "
                    "(%lu)\n", addr, GetLastError());
                return false;
            }
            std::memcpy(addr, &value, 4);
            DWORD tmp = 0;
            VirtualProtect(addr, 4, oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), addr, 4);
            return true;
        }

        bool VerifyCtlSiteAt(std::size_t i, std::uintptr_t va, bool quiet)
        {
            const CtlDispSite& s = kCtlDispSites[i];
            const std::uint8_t* addr = reinterpret_cast<const std::uint8_t*>(
                ResolveGameAddress(va));
            if (!addr)
                return false;
            __try
            {
                if (s.dispOff != 0xFF)
                {
                    std::uint32_t v = 0;
                    std::memcpy(&v, addr + s.dispOff, 4);
                    if (v != s.oldDisp)
                    {
                        if (!quiet)
                            NoteVerifyFail("ctl site %p: disp at +%u is 0x%X, "
                                           "expected 0x%X",
                                           addr, s.dispOff, v, s.oldDisp);
                        return false;
                    }
                    g_CtlDispOff[i] = s.dispOff;
                    return true;
                }
                int found = -1;
                for (int off = 1; off <= s.maxOff; ++off)
                {
                    std::uint32_t v = 0;
                    std::memcpy(&v, addr + off, 4);
                    if (v == s.oldDisp)
                    {
                        if (found >= 0)
                        {
                            if (!quiet)
                                NoteVerifyFail("ctl site %p disp 0x%X: "
                                               "ambiguous (offsets %d and %d)",
                                               addr, s.oldDisp, found, off);
                            return false;
                        }
                        found = off;
                    }
                }
                if (found < 0)
                {
                    if (!quiet)
                        NoteVerifyFail("ctl site %p: disp 0x%X not found in "
                                       "the instruction window",
                                       addr, s.oldDisp);
                    return false;
                }
                g_CtlDispOff[i] = static_cast<std::uint8_t>(found);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                if (!quiet)
                    NoteVerifyFail("ctl site %p: unreadable", addr);
                return false;
            }
        }

        bool VerifyCtlSite(std::size_t i)
        {
            return VerifyCtlSiteAt(i, kCtlDispSites[i].addr, false);
        }

        bool RelocateCloneCtlSites(const std::size_t* failed,
                                   std::size_t nFailed)
        {
            std::size_t order[16];
            for (std::size_t i = 0; i < nFailed; ++i)
                order[i] = failed[i];
            for (std::size_t i = 1; i < nFailed; ++i)
                for (std::size_t j = i;
                     j > 0 && kCtlDispSites[order[j - 1]].addr
                              > kCtlDispSites[order[j]].addr; --j)
                {
                    const std::size_t t = order[j];
                    order[j]            = order[j - 1];
                    order[j - 1]        = t;
                }

            const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(
                GetModuleHandleW(nullptr));
            if (!base)
                return false;

            std::uintptr_t placed[16] = {};
            __try
            {
                const IMAGE_DOS_HEADER* dos =
                    reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const IMAGE_NT_HEADERS* nt =
                    reinterpret_cast<const IMAGE_NT_HEADERS*>(
                        base + dos->e_lfanew);
                const std::uint8_t* end =
                    base + nt->OptionalHeader.SizeOfImage;

                std::size_t g0 = 0;
                while (g0 < nFailed)
                {
                    std::size_t g1 = g0 + 1;
                    while (g1 < nFailed
                           && kCtlDispSites[order[g1]].addr
                              - kCtlDispSites[order[g1 - 1]].addr <= 0x1000)
                        ++g1;

                    const CtlDispSite& f0 = kCtlDispSites[order[g0]];
                    std::uintptr_t best[16] = {};
                    std::size_t    nComplete = 0;

                    MEMORY_BASIC_INFORMATION mbi;
                    const std::uint8_t* p = base;
                    while (p < end
                           && VirtualQuery(p, &mbi, sizeof(mbi))
                              == sizeof(mbi))
                    {
                        const std::uint8_t* rBeg =
                            static_cast<const std::uint8_t*>(
                                mbi.BaseAddress);
                        const std::uint8_t* rEnd = rBeg + mbi.RegionSize;
                        if (rEnd > end)
                            rEnd = end;
                        const bool scannable =
                            mbi.State == MEM_COMMIT
                            && (mbi.Protect
                                & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                                   | PAGE_EXECUTE_READWRITE
                                   | PAGE_EXECUTE_WRITECOPY)) != 0;
                        if (scannable && rEnd - rBeg >= 8)
                        {
                            for (const std::uint8_t* q = rBeg;
                                 q + 4 <= rEnd; ++q)
                            {
                                if (q[2] != 0x01 || q[3] != 0x00)
                                    continue;
                                std::uint32_t v = 0;
                                std::memcpy(&v, q, 4);
                                if (v != f0.oldDisp)
                                    continue;
                                const std::uintptr_t qva =
                                    0x140000000ull
                                    + static_cast<std::uintptr_t>(q - base);
                                const int offLo =
                                    f0.dispOff != 0xFF ? f0.dispOff : 1;
                                const int offHi =
                                    f0.dispOff != 0xFF ? f0.dispOff
                                                       : f0.maxOff;
                                for (int off = offLo; off <= offHi; ++off)
                                {
                                    const std::uintptr_t anchor = qva - off;
                                    if (anchor == f0.addr)
                                        continue;
                                    if (!VerifyCtlSiteAt(order[g0], anchor,
                                                         true))
                                        continue;
                                    std::uintptr_t cur[16];
                                    cur[0] = anchor;
                                    bool ok = true;
                                    for (std::size_t gi = g0 + 1;
                                         gi < g1 && ok; ++gi)
                                    {
                                        const std::uintptr_t expect =
                                            anchor
                                            + (kCtlDispSites[order[gi]].addr
                                               - f0.addr);
                                        std::uintptr_t found = 0;
                                        for (std::uintptr_t c =
                                                 expect - 0x80;
                                             c <= expect + 0x80; ++c)
                                        {
                                            if (c <= cur[gi - g0 - 1])
                                                continue;
                                            if (VerifyCtlSiteAt(order[gi],
                                                                c, true))
                                            {
                                                found = c;
                                                break;
                                            }
                                        }
                                        if (!found)
                                            ok = false;
                                        else
                                            cur[gi - g0] = found;
                                    }
                                    if (!ok)
                                        continue;
                                    if (nComplete == 0)
                                        for (std::size_t k = 0;
                                             k < g1 - g0; ++k)
                                            best[k] = cur[k];
                                    ++nComplete;
                                }
                            }
                        }
                        p = rEnd < end ? rEnd : end;
                    }

                    if (nComplete != 1)
                    {
                        NoteVerifyFail(
                            "clone relocation: %zu candidate placements for "
                            "a %zu-site moved clone block (need exactly 1) "
                            "- the clone table must be re-ported by hand",
                            nComplete, g1 - g0);
                        return false;
                    }
                    for (std::size_t k = 0; k < g1 - g0; ++k)
                        placed[g0 + k] = best[k];
                    g0 = g1;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                NoteVerifyFail("clone relocation: exception while scanning "
                               "the game image");
                return false;
            }

            const std::uintptr_t firstOld = kCtlDispSites[order[0]].addr;
            for (std::size_t i = 0; i < nFailed; ++i)
            {
                const std::intptr_t d =
                    static_cast<std::intptr_t>(placed[i])
                    - static_cast<std::intptr_t>(
                        kCtlDispSites[order[i]].addr);
                g_CtlMut[order[i]].addr = placed[i];
                if (!VerifyCtlSiteAt(order[i],
                                     kCtlDispSites[order[i]].addr, false))
                    return false;
                bool dup = false;
                for (std::size_t k = 0; k < g_CloneDeltaCount; ++k)
                    if (g_CloneDelta[k] == d)
                        dup = true;
                if (!dup && g_CloneDeltaCount < 4)
                    g_CloneDelta[g_CloneDeltaCount++] = d;
            }
            Log("[DevelopArrayGrow] %zu Arxan clone ctl site(s) re-laid by this "
                "update - relocated per-site (0x%llX -> 0x%llX) and byte-verified\n",
                nFailed,
                static_cast<unsigned long long>(firstOld),
                static_cast<unsigned long long>(placed[0]));
            return true;
        }

        constexpr std::uint32_t kSweepValLo = 0x1A008;
        constexpr std::uint32_t kSweepValHi = 0x1FFD0;
        constexpr int           kSweepReportCap = 64;

        struct SweepFind
        {
            std::uintptr_t va;
            std::uint32_t  disp;
        };
        SweepFind g_SweepUnknown[kSweepReportCap];
        int       g_SweepUnknownCount = 0;

        bool           g_SweepValue[kSweepValHi - kSweepValLo] = {};
        std::uintptr_t g_SweepAllow[600];
        std::size_t    g_SweepAllowCount = 0;

        void SweepAllowAdd(std::uintptr_t va)
        {
            if (g_SweepAllowCount < 600)
                g_SweepAllow[g_SweepAllowCount++] = va;
        }

        bool SweepAllowed(std::uintptr_t va)
        {
            for (std::size_t i = 0; i < g_SweepAllowCount; ++i)
                if (g_SweepAllow[i] == va)
                    return true;
            for (std::size_t d = 0; d < g_CloneDeltaCount; ++d)
                for (std::size_t i = 0; i < g_SweepAllowCount; ++i)
                    if (g_SweepAllow[i] + g_CloneDelta[d] == va)
                        return true;
            return false;
        }

        bool SweepImageForUnknownSites(bool quiet = false)
        {
            g_SweepAllowCount = 0;
            g_SweepUnknownCount = 0;
            for (std::size_t i = 0; i < kCtlSiteCount; ++i)
            {
                g_SweepValue[kCtlDispSites[i].oldDisp - kSweepValLo] = true;
                SweepAllowAdd(kCtlDispSites[i].addr + g_CtlDispOff[i]);
            }
            const std::uint32_t parVals[] =
                { 0x1A028, 0x1A008, 0x1A00C, 0x1A010, 0x1A014 };
            for (std::size_t i = 0; i < 5; ++i)
                g_SweepValue[parVals[i] - kSweepValLo] = true;
            for (std::size_t i = 0; i < kPatchCount; ++i)
                if (kPatches[i].addr
                    && kPatches[i].oldImm >= kSweepValLo
                    && kPatches[i].oldImm < kSweepValHi)
                    SweepAllowAdd(kPatches[i].addr + kPatches[i].opLen);
            for (std::size_t i = 0; i < g_A->sweepFpCount; ++i)
                SweepAllowAdd(g_A->sweepFp[i]);

            const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(
                GetModuleHandleW(nullptr));
            if (!base)
            {
                NoteVerifyFail("image sweep: no game module base");
                return false;
            }

            int unknown = 0;
            int benign  = 0;
            __try
            {
                const IMAGE_DOS_HEADER* dos =
                    reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const IMAGE_NT_HEADERS* nt =
                    reinterpret_cast<const IMAGE_NT_HEADERS*>(
                        base + dos->e_lfanew);
                const std::uint8_t* end =
                    base + nt->OptionalHeader.SizeOfImage;

                MEMORY_BASIC_INFORMATION mbi;
                const std::uint8_t* p = base;
                while (p < end
                       && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
                {
                    const std::uint8_t* rBeg =
                        static_cast<const std::uint8_t*>(mbi.BaseAddress);
                    const std::uint8_t* rEnd = rBeg + mbi.RegionSize;
                    if (rEnd > end)
                        rEnd = end;
                    const bool readable =
                        mbi.State == MEM_COMMIT
                        && mbi.Protect != 0
                        && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
                    const bool executable =
                        (mbi.Protect
                         & (PAGE_EXECUTE | PAGE_EXECUTE_READ
                            | PAGE_EXECUTE_READWRITE
                            | PAGE_EXECUTE_WRITECOPY)) != 0;
                    if (readable && rEnd - rBeg >= 8)
                    {
                        for (const std::uint8_t* q = rBeg + 2;
                             q + 4 <= rEnd; ++q)
                        {
                            if (q[2] != 0x01 || q[3] != 0x00)
                                continue;
                            std::uint32_t v = 0;
                            std::memcpy(&v, q, 4);
                            if (v < kSweepValLo || v >= kSweepValHi
                                || !g_SweepValue[v - kSweepValLo])
                                continue;
                            const std::uint8_t b1 = q[-1];
                            const std::uint8_t b2 = q[-2];
                            const bool direct = (b1 & 0xC0) == 0x80
                                                && (b1 & 7) != 4;
                            const bool sib = (b2 & 0xC0) == 0x80
                                             && (b2 & 7) == 4;
                            if (!direct && !sib)
                                continue;
                            const std::uintptr_t va =
                                0x140000000ull
                                + static_cast<std::uintptr_t>(q - base);
                            if (SweepAllowed(va))
                                continue;
                            if (!executable)
                            {
                                ++benign;
                                continue;
                            }
                            bool detourTail = false;
                            for (int k = 1; k <= 4 && !detourTail; ++k)
                            {
                                if (q - k < rBeg)
                                    break;
                                if (q[-k] != 0xE9 && q[-k] != 0xE8)
                                    continue;
                                std::int32_t rel = 0;
                                std::memcpy(&rel, q - k + 1, 4);
                                const std::uintptr_t next =
                                    0x140000000ull
                                    + static_cast<std::uintptr_t>(
                                        (q - k + 5) - base);
                                const std::uintptr_t tgt = next
                                    + static_cast<std::uintptr_t>(
                                        static_cast<std::intptr_t>(rel));
                                if (tgt < 0x140000000ull
                                    || tgt >= 0x140000000ull
                                        + nt->OptionalHeader.SizeOfImage)
                                    detourTail = true;
                            }
                            if (detourTail)
                            {
                                ++benign;
                                continue;
                            }
                            if (unknown == 0 && !quiet)
                                NoteVerifyFail(
                                    "image sweep: unknown develop-field "
                                    "reference at %p (disp 0x%X) - an "
                                    "unpatched code copy exists",
                                    reinterpret_cast<const void*>(va), v);
                            if (unknown < kSweepReportCap)
                            {
                                if (!quiet)
                                    LogDebug("[DevelopArrayGrow] sweep unknown "
                                        "site %d: 0x%llX disp 0x%X prot "
                                        "0x%X\n",
                                        unknown + 1,
                                        static_cast<unsigned long long>(va),
                                        v, static_cast<unsigned>(mbi.Protect));
                                g_SweepUnknown[unknown] = SweepFind{ va, v };
                                g_SweepUnknownCount = unknown + 1;
                            }
                            ++unknown;
                        }
                    }
                    p = rEnd < end ? rEnd : end;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                NoteVerifyFail("image sweep: exception while scanning the "
                               "game image");
                return false;
            }
            if (unknown > kSweepReportCap && !quiet)
                LogDebug("[DevelopArrayGrow] sweep found %d unknown sites, only "
                    "the first %d are listed\n", unknown, kSweepReportCap);
            if (unknown != 0 && benign != 0 && !quiet)
                LogDebug("[DevelopArrayGrow] sweep skipped %d match(es) in "
                         "non-executable memory (data coincidences)\n",
                    benign);
            return unknown == 0;
        }

        bool VerifyByteFix(const ByteFix& f)
        {
            const std::uint8_t* addr = reinterpret_cast<const std::uint8_t*>(
                ResolveGameAddress(f.addr));
            if (!addr)
                return false;
            __try
            {
                if (std::memcmp(addr, f.oldBytes, f.len) == 0
                    || std::memcmp(addr, f.newBytes, f.len) == 0)
                    return true;
                NoteVerifyFail("byte fix %s @%p: opcode bytes are not the "
                               "expected 8-bit-index form", f.what, addr);
                return false;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                NoteVerifyFail("byte fix %s: exception reading the site",
                               f.what);
                return false;
            }
        }

        bool WriteByteFix(const ByteFix& f, const std::uint8_t* bytes)
        {
            std::uint8_t* addr = reinterpret_cast<std::uint8_t*>(
                const_cast<void*>(ResolveGameAddress(f.addr)));
            if (!addr)
                return false;
            DWORD oldProt = 0;
            if (!VirtualProtect(addr, f.len, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                Log("[DevelopArrayGrow] byte fix %s @%p: VirtualProtect failed "
                    "(%lu)\n", f.what, addr, GetLastError());
                return false;
            }
            std::memcpy(addr, bytes, f.len);
            DWORD tmp = 0;
            VirtualProtect(addr, f.len, oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), addr, f.len);
            return true;
        }

        std::uint16_t __fastcall hkSiblingCount(void* self, std::uint16_t row,
                                                char flaggedOnly)
        {
            std::uint16_t n =
                g_OrigSiblingCount ? g_OrigSiblingCount(self, row, flaggedOnly)
                                   : 0;
            if (n > kSiblingListCap)
            {
                LogDebug("[DevelopArrayGrow] develop row %u has %u same-group rows "
                         "- clamped to the engine's 1024-entry stack array; "
                         "siblings past 1024 are not retired, which beats writing "
                         "%u bytes past it\n",
                    static_cast<unsigned>(row), static_cast<unsigned>(n),
                    static_cast<unsigned>((n - kSiblingListCap) * 2u));
                n = kSiblingListCap;
            }
            ++g_SibCalls;
            g_SibSum += n;
            if (n > g_SibMax)
                g_SibMax = n;
            if (g_SibCalls == 200000
                && g_VisArmed
                && g_VisThread == GetCurrentThreadId())
            {
                LogDebug("[DevelopArrayGrow] variant-chain walk hit 200000 "
                         "iterations in one list build (normal is a few hundred) - "
                         "no end sentinel reached; caller chain follows\n");
                LogSiblingBacktrace(g_SibCalls);
            }
            return n;
        }

        bool CustomRowLive(const void* self, std::uint16_t row)
        {
            __try
            {
                const std::uint8_t* rec =
                    static_cast<const std::uint8_t*>(self)
                    + static_cast<std::size_t>(row) * kRecStride;
                return *reinterpret_cast<const std::uint16_t*>(rec + 0x8) != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool __fastcall hkIsVisible(void* self, std::uint16_t row)
        {
            ++g_VisWinCalls;
            if (outfit::IsDevelopHidden(row))
                return false;
            if (row >= kFirstCustomRow && row < kNewRows
                && !IsReservedFlowRow(row)
                && !MissionCodeGuard::ShouldBypassHooks()
                && CustomRowLive(self, row))
                return true;
            if (row >= kNewRows || g_VisThread != GetCurrentThreadId())
                return g_OrigIsVisible ? g_OrigIsVisible(self, row) : false;

            const std::uint64_t now = GetTickCount64();
            if (g_VisValid && !g_VisArmed && now - g_VisStamp > kVisTtlMs)
            {
                g_VisValid = false;
                g_VisSelf  = nullptr;
            }
            if (!g_VisValid)
            {
                std::memset(g_VisCache, 0, sizeof(g_VisCache));
                g_VisSelf  = self;
                g_VisStamp = now;
                g_VisValid = true;
            }
            else if (g_VisSelf != self)
            {
                return g_OrigIsVisible ? g_OrigIsVisible(self, row) : false;
            }

            ++g_VisCalls;
            if (const std::uint8_t c = g_VisCache[row])
            {
                ++g_VisHits;
                ++g_VisWinHits;
                return c == 2;
            }
            const bool v = g_OrigIsVisible ? g_OrigIsVisible(self, row) : false;
            g_VisCache[row] = v ? std::uint8_t{ 2 } : std::uint8_t{ 1 };
            return v;
        }

        bool VerifyBoundSite(const BoundImmSite& s)
        {
            const std::uint8_t* addr = reinterpret_cast<const std::uint8_t*>(
                ResolveGameAddress(s.addr));
            if (!addr)
                return false;
            __try
            {
                std::uint32_t v = 0;
                std::memcpy(&v, addr + s.immOff, 4);
                if (v != 0x400)
                {
                    NoteVerifyFail("bound site %p+%u: imm 0x%X is not 0x400",
                                   addr, s.immOff, v);
                    return false;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                NoteVerifyFail("bound site %p: unreadable", addr);
                return false;
            }
        }

        void RevertAllPatches(const char* why)
        {
            g_Active = false;
            g_ArmedBase20.store(0, std::memory_order_relaxed);
            int reverted = 0;
            for (const ImmPatch& p : kPatches)
                if (p.addr && WriteImmAt(p, p.oldImm))
                    ++reverted;
            for (std::size_t i = 0; i < kCtlSiteCount; ++i)
                if (g_CtlDispOff[i]
                    && WriteU32At(kCtlDispSites[i].addr, g_CtlDispOff[i],
                                  kCtlDispSites[i].oldDisp))
                    ++reverted;
            for (std::size_t i = 0; i < kBoundSiteCount; ++i)
                if (WriteU32At(kBoundImmSites[i].addr,
                               kBoundImmSites[i].immOff, 0x400))
                    ++reverted;
            for (const ByteFix& f : kSiblingLoopFixes)
                if (WriteByteFix(f, f.oldBytes))
                    ++reverted;
            LogDebug("[DevelopArrayGrow] REVERTED (%s): %d site(s) restored - bound "
                     "back at 1024 rows this session\n",
                why, reverted);
        }

        void DeactivateGrow(const char* why)
        {
            g_Active = false;
            g_ArmedBase20.store(0, std::memory_order_relaxed);
            LogDebug("[DevelopArrayGrow] CRITICAL (%s): grown band deactivated - "
                     "rows >= 1024 unusable; restart before developing anything\n", why);
        }

        bool ProbeGrownRowsClean(std::uintptr_t base20)
        {
            for (std::uint32_t i = kOldRows; i < kNewRows; ++i)
            {
                const std::uint16_t w =
                    *reinterpret_cast<const std::uint16_t*>(
                        base20 + 8
                        + static_cast<std::size_t>(i) * kRecStride);
                if (i == kFlowSentinel)
                {
                    if (w != 0 && w != 0xFFFF)
                        return false;
                }
                else if (w != 0)
                    return false;
            }
            return true;
        }

        void ClaimReservedRows(std::uintptr_t block)
        {
            std::uint32_t mask = 0;
            for (std::uint32_t row = kGunsmithFlowFirst;
                 row <= kFlowSentinel; ++row)
            {
                std::uint8_t* rec = reinterpret_cast<std::uint8_t*>(
                    block + 0x28
                    + static_cast<std::size_t>(row) * kRecStride);
                const std::uint16_t word0 =
                    *reinterpret_cast<std::uint16_t*>(rec);
                if (word0 != 0 && word0 != 0xFFFF)
                {
                    static std::atomic<int> s_occupied{ 0 };
                    if (s_occupied.fetch_add(1, std::memory_order_relaxed) < 4)
                        Log("[DevelopArrayGrow] WARNING: develop row %u already "
                            "holds a live record (developId=%u) - rows 0x3FD-0x3FF "
                            "hard-map to gunsmith equips 871/873/875, so this row "
                            "is left to the game; a custom weapon here equips a "
                            "gunsmith pseudo-weapon\n",
                            row, static_cast<unsigned>(word0));
                    continue;
                }
                if (word0 != 0xFFFF)
                {
                    std::memset(rec, 0, kRecStride);
                    *reinterpret_cast<std::uint16_t*>(rec) = 0xFFFF;
                    rec[0x36] = 0xFF;
                }
                if (row < kNewRows)
                {
                    g_FlagsShadow[row] |= kReqAnnouncedBit;
                    MirrorFlagByte(row);
                }
                if (row != kFlowSentinel)
                    mask |= 1u << (row - kGunsmithFlowFirst);
            }
            g_GunsmithClaimMask.store(mask, std::memory_order_relaxed);
        }

        void ArmFromBase20(std::uintptr_t base20)
        {
            __try
            {
                const std::uintptr_t block = base20 - 0x20;
                std::uint8_t** slot = reinterpret_cast<std::uint8_t**>(
                    block + kBlock_FlagsPtrOff);

                if (!ProbeGrownRowsClean(base20))
                {
                    LogDebug("[DevelopArrayGrow] CRITICAL: rows 1024..%u of develop "
                             "block %p hold foreign data (the block predates the "
                             "grow patches) - refusing the grown band\n",
                        kNewRows - 1, reinterpret_cast<void*>(block));
                    DeactivateGrow("pre-existing develop block");
                    return;
                }

                ClaimReservedRows(block);

                std::uint8_t* svarsFlags = *slot;
                if (!svarsFlags)
                    return;
                if (svarsFlags != g_FlagsShadow)
                {
                    if (g_BlockSeen
                        && g_BlockSeen != reinterpret_cast<void*>(block))
                        Log("[DevelopArrayGrow] WARNING: second develop block "
                            "%p (first %p) - shadow re-bound to the newest\n",
                            reinterpret_cast<void*>(block), g_BlockSeen);
                    g_BlockSeen  = reinterpret_cast<void*>(block);
                    g_SvarsFlags = svarsFlags;
                    std::memcpy(g_FlagsShadow, svarsFlags, kOldRows);
                    std::memcpy(g_SyncMirror, svarsFlags, kOldRows);
                    std::memset(g_FlagsShadow + kOldRows, kReqAnnouncedBit,
                                kFlagsShadowSize - kOldRows);
                    MarkReservedRowsAnnounced();
                    MarkSilencedRowsAnnounced();
                    RestoreGrownAnnouncedBits();
                    *slot = g_FlagsShadow;
                    InvalidateDevelopVisibilityCache();
                    InvalidateDevelopLookupIndex();
                    EquipDevelop_RequestDevelopRestore();
                    LogDebug("[DevelopArrayGrow] develop block %p armed at first "
                             "use (its reset ran before the hooks): flags shadow "
                             "bound (svars=%p), rows 0x3FD-0x400 claimed\n",
                        reinterpret_cast<void*>(block), svarsFlags);
                }
                else if (!g_BlockSeen)
                {
                    g_BlockSeen = reinterpret_cast<void*>(block);
                }
                g_ArmedBase20.store(base20, std::memory_order_relaxed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[DevelopArrayGrow] ArmFromBase20: exception probing the "
                         "develop block - grown rows stay unarmed this pass\n");
            }
        }

        bool GrownRowFlagsBound(std::uintptr_t base20)
        {
            __try
            {
                return *reinterpret_cast<std::uint8_t* const*>(
                           base20 + kBase20_FlagsPtrOff) == g_FlagsShadow;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void __fastcall hkBlockReset(std::uintptr_t block)
        {
            g_OrigBlockReset(block);
            __try
            {
                ClaimReservedRows(block);

                std::uint8_t** slot = reinterpret_cast<std::uint8_t**>(
                    block + kBlock_FlagsPtrOff);
                std::uint8_t* svarsFlags = *slot;
                if (!svarsFlags)
                    return;
                if (g_BlockSeen && g_BlockSeen != reinterpret_cast<void*>(block))
                    Log("[DevelopArrayGrow] WARNING: second develop block %p "
                        "(first %p) - shadow re-bound to the newest\n",
                        reinterpret_cast<void*>(block), g_BlockSeen);
                g_BlockSeen  = reinterpret_cast<void*>(block);
                g_SvarsFlags = svarsFlags;
                std::memcpy(g_FlagsShadow, svarsFlags, kOldRows);
                std::memcpy(g_SyncMirror, svarsFlags, kOldRows);
                std::memset(g_FlagsShadow + kOldRows, kReqAnnouncedBit,
                            kFlagsShadowSize - kOldRows);
                MarkReservedRowsAnnounced();
                MarkSilencedRowsAnnounced();
                RestoreGrownAnnouncedBits();
                *slot = g_FlagsShadow;
                g_ArmedBase20.store(block + 0x20, std::memory_order_relaxed);
                InvalidateDevelopVisibilityCache();
                InvalidateDevelopLookupIndex();
                EquipDevelop_RequestDevelopRestore();
                LogDebug("[DevelopArrayGrow] develop block reset: flags shadow "
                         "re-armed (svars=%p rows=%u), rows 0x3FD-0x400 "
                         "pre-claimed\n", svarsFlags, kNewRows);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[DevelopArrayGrow] hkBlockReset: exception re-arming "
                    "the flags shadow\n");
            }
        }

        bool RecordFieldAt(std::uintptr_t base20, std::uint16_t row,
                           bool equipField, std::uint16_t& out);

        void RearmNewBadgeOnDevelop(std::uintptr_t base20, std::uint16_t idx)
        {
            if (idx < kFirstCustomRow)
                return;
            std::uint16_t developId = 0;
            if (!RecordFieldAt(base20, idx, false, developId) || developId == 0)
                return;
            const std::int32_t id = static_cast<std::int32_t>(developId);
            if (!V_FrameWorkState::IsManagedDevelopId(id))
                return;
            V_FrameWorkState::SetNewByDevelopId(id, true);
        }

        void __fastcall hkSetDeveloped(std::uintptr_t base20, std::uint16_t idx)
        {
            if (idx >= equip::NativeFlowBound())
            {
                LogDebug("[DevelopArrayGrow] SetEquipDeveloped REFUSED garbage "
                    "index %u (bound %u)\n", idx, equip::NativeFlowBound());
                return;
            }
            if (idx >= kOldRows && !GrownRowFlagsBound(base20))
            {
                LogDebug("[DevelopArrayGrow] SetEquipDeveloped REFUSED grown index "
                         "%u: flags shadow unbound - the write would land in the "
                         "online develop array\n", idx);
                return;
            }
            void* ctl = reinterpret_cast<void*>(base20);
            if (EquipDevelop_ShouldSuppressNativeDevelop(ctl, idx))
                return;
            RearmNewBadgeOnDevelop(base20, idx);
            g_OrigSetDeveloped(base20, idx);
            MirrorFlagByte(idx);
            EquipDevelop_NotifyNativeDevelopChanged(ctl, idx, true);
        }

        void __fastcall hkSetUndeveloped(std::uintptr_t base20,
                                         std::uint16_t idx, char notify)
        {
            if (idx >= equip::NativeFlowBound())
            {
                LogDebug("[DevelopArrayGrow] SetEquipUndeveloped REFUSED garbage "
                    "index %u (bound %u)\n", idx, equip::NativeFlowBound());
                return;
            }
            if (idx >= kOldRows && !GrownRowFlagsBound(base20))
            {
                LogDebug("[DevelopArrayGrow] SetEquipUndeveloped REFUSED grown "
                         "index %u: flags shadow unbound - the write would land in "
                         "the online develop array\n", idx);
                return;
            }
            void* ctl = reinterpret_cast<void*>(base20);
            g_OrigSetUndeveloped(base20, idx, notify);
            MirrorFlagByte(idx);
            EquipDevelop_NotifyNativeDevelopChanged(ctl, idx, false);
        }

        constexpr std::uint32_t kDevIdSlots   = 0x10000;
        constexpr std::uint32_t kEquipIdSlots = 0x10000;
        constexpr std::uint64_t kLookupTtlMs  = 16;

        std::uint16_t  g_DevIdRow[kDevIdSlots]          = {};
        std::uint16_t  g_EquipIdRow[kEquipIdSlots]      = {};
        std::uint16_t  g_EquipIdRowManaged[kEquipIdSlots] = {};
        bool           g_LookupValid  = false;
        std::uintptr_t g_LookupBase   = 0;
        std::uint32_t  g_LookupBound  = 0;
        std::uint64_t  g_LookupStamp  = 0;
        std::uint64_t  g_LookupBuilds = 0;
        std::uint64_t  g_FindCalls    = 0;
        std::uint64_t  g_FindIndexed  = 0;
        std::uint64_t  g_FindStale    = 0;

        std::uint64_t g_VanIdentityBits[kEquipIdSlots / 64] = {};
        bool          g_VanIdentityDirty = false;

        void BuildLookupIndex(std::uintptr_t base20, std::uint32_t bound)
        {
            std::memset(g_DevIdRow, 0xFF, sizeof(g_DevIdRow));
            std::memset(g_EquipIdRow, 0xFF, sizeof(g_EquipIdRow));
            std::memset(g_EquipIdRowManaged, 0xFF, sizeof(g_EquipIdRowManaged));
            std::uint64_t vanBits[kEquipIdSlots / 64] = {};
            const std::uint8_t* base =
                reinterpret_cast<const std::uint8_t*>(base20);
            for (std::uint32_t i = 0; i < bound; ++i)
            {
                if (IsReservedFlowRow(i))
                    continue;
                const std::uint8_t* rec =
                    base + static_cast<std::size_t>(i) * kRecStride;
                const std::uint16_t dev = *reinterpret_cast<const std::uint16_t*>(
                    rec + kBase20_RecFieldOff);
                if (g_DevIdRow[dev] == 0xFFFF)
                    g_DevIdRow[dev] = static_cast<std::uint16_t>(i);
                const std::uint16_t eqp = static_cast<std::uint16_t>(
                    *reinterpret_cast<const std::uint16_t*>(rec + 0xC) >> 4);
                if (eqp < kEquipIdSlots && g_EquipIdRow[eqp] == 0xFFFF)
                    g_EquipIdRow[eqp] = static_cast<std::uint16_t>(i);
                if (eqp != 0 && eqp < kEquipIdSlots)
                {
                    if (i < kFirstCustomRow)
                        vanBits[eqp >> 6] |= 1ull << (eqp & 63);
                    else if (g_EquipIdRowManaged[eqp] == 0xFFFF)
                        g_EquipIdRowManaged[eqp] = static_cast<std::uint16_t>(i);
                }
            }
            if (std::memcmp(vanBits, g_VanIdentityBits, sizeof(vanBits)) != 0)
            {
                std::memcpy(g_VanIdentityBits, vanBits, sizeof(vanBits));
                g_VanIdentityDirty = true;
            }
            g_LookupBase  = base20;
            g_LookupBound = bound;
            g_LookupStamp = GetTickCount64();
            g_LookupValid = true;
            ++g_LookupBuilds;
        }

        void PushVanillaIdentityIds()
        {
            if (!g_VanIdentityDirty)
                return;
            g_VanIdentityDirty = false;
            std::vector<std::int32_t> ids;
            ids.reserve(512);
            for (std::uint32_t w = 0; w < kEquipIdSlots / 64; ++w)
            {
                if (g_VanIdentityBits[w] == 0)
                    continue;
                for (std::uint32_t b = 0; b < 64; ++b)
                    if (g_VanIdentityBits[w] & (1ull << b))
                        ids.push_back(static_cast<std::int32_t>(w * 64 + b));
            }
            if (ids.size() < 64)
                return;
            V_FrameWorkState::SetVanillaIdentityEquipIds(ids.data(), ids.size());
        }

        std::uint32_t g_LookupProbe = 0;

        bool RecordFieldAt(std::uintptr_t base20, std::uint16_t row,
                           bool equipField, std::uint16_t& out)
        {
            if (row >= equip::NativeFlowBound() || IsReservedFlowRow(row))
                return false;
            __try
            {
                const std::uint8_t* rec =
                    reinterpret_cast<const std::uint8_t*>(base20)
                    + static_cast<std::size_t>(row) * kRecStride;
                const std::uint16_t raw =
                    *reinterpret_cast<const std::uint16_t*>(
                        rec + (equipField ? 0xC : kBase20_RecFieldOff));
                out = equipField ? static_cast<std::uint16_t>(raw >> 4) : raw;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool EnsureLookupIndex(std::uintptr_t base20)
        {
            const std::uint32_t bound = equip::NativeFlowBound();
            if (g_LookupValid
                && g_LookupBase == base20
                && g_LookupBound == bound)
            {
                if ((++g_LookupProbe & 0xFF) != 0)
                    return true;
                if (GetTickCount64() - g_LookupStamp <= kLookupTtlMs)
                    return true;
            }
            __try
            {
                BuildLookupIndex(base20, bound);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_LookupValid = false;
                return false;
            }
            PushVanillaIdentityIds();
            return true;
        }

        bool ServeFromIndex(std::uintptr_t base20, std::uint16_t key,
                            bool equipField, std::uint16_t& out)
        {
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (!EnsureLookupIndex(base20))
                    return false;
                const std::uint16_t row =
                    equipField ? g_EquipIdRow[key] : g_DevIdRow[key];
                if (row == 0xFFFF)
                {
                    out = kFlowSentinel;
                    return true;
                }
                std::uint16_t live = 0;
                if (RecordFieldAt(base20, row, equipField, live) && live == key)
                {
                    out = row;
                    return true;
                }
                ++g_FindStale;
                InvalidateDevelopLookupIndex();
            }
            return false;
        }

        std::uint16_t __fastcall hkFindByDevId(std::uintptr_t base20,
                                               std::uint16_t developId)
        {
            ++g_FindCalls;
            std::uint16_t served = 0;
            if (ServeFromIndex(base20, developId, false, served))
            {
                ++g_FindIndexed;
                return served;
            }
            const std::uint8_t* base =
                reinterpret_cast<const std::uint8_t*>(base20);
            for (std::uint32_t i = 0; i < equip::NativeFlowBound(); ++i)
            {
                if (IsReservedFlowRow(i))
                    continue;
                if (*reinterpret_cast<const std::uint16_t*>(
                        base + static_cast<std::size_t>(i) * kRecStride
                        + kBase20_RecFieldOff) == developId)
                    return static_cast<std::uint16_t>(i);
            }
            return kFlowSentinel;
        }

        std::uint16_t __fastcall hkFindByEquipId(std::uintptr_t base20,
                                                 std::uint16_t equipId)
        {
            if (equipId == 0)
                return kFlowSentinel;
            ++g_FindCalls;
            if (equipId < kEquipIdSlots
                && !MissionCodeGuard::ShouldBypassHooks()
                && V_FrameWorkState::IsClaimedEquipId(
                       static_cast<std::int32_t>(equipId))
                && EnsureLookupIndex(base20))
            {
                const std::uint16_t mrow = g_EquipIdRowManaged[equipId];
                std::uint16_t live = 0;
                if (mrow != 0xFFFF
                    && RecordFieldAt(base20, mrow, true, live)
                    && live == equipId)
                    return mrow;
            }
            std::uint16_t served = 0;
            if (equipId < kEquipIdSlots
                && ServeFromIndex(base20, equipId, true, served))
            {
                ++g_FindIndexed;
                return served;
            }
            const std::uint8_t* base =
                reinterpret_cast<const std::uint8_t*>(base20);
            for (std::uint32_t i = 0; i < equip::NativeFlowBound(); ++i)
            {
                if (IsReservedFlowRow(i))
                    continue;
                const std::uint16_t packed =
                    *reinterpret_cast<const std::uint16_t*>(
                        base + static_cast<std::size_t>(i) * kRecStride + 0xC);
                if (static_cast<std::uint16_t>(packed >> 4) == equipId)
                    return static_cast<std::uint16_t>(i);
            }
            return kFlowSentinel;
        }

        bool IsGrownBase20(std::uintptr_t base20)
        {
            return g_BlockSeen
                && base20 == reinterpret_cast<std::uintptr_t>(g_BlockSeen)
                             + 0x20;
        }

        bool RowQualifiesForList(std::uintptr_t base20, std::uint32_t idx)
        {
            const std::uintptr_t block = base20 - 0x20;
            const std::uintptr_t vtbl =
                *reinterpret_cast<const std::uintptr_t*>(block);
            const RowListGate_t gate =
                *reinterpret_cast<const RowListGate_t*>(vtbl + 0x30);
            if (!gate || gate(block, static_cast<std::uint16_t>(idx)) == 0)
                return false;
            if (*reinterpret_cast<const std::uint16_t*>(
                    base20 + 8
                    + static_cast<std::size_t>(idx) * kRecStride) == 0)
                return false;
            const std::uint8_t* flags =
                *reinterpret_cast<std::uint8_t* const*>(
                    base20 + kBase20_FlagsPtrOff);
            return flags && (flags[idx] & 1) != 0;
        }

        std::uint16_t __fastcall hkListDevCount(std::uintptr_t base20)
        {
            if (!IsGrownBase20(base20))
                return g_OrigListDevCount(base20);
            SyncFlagsWithSvars(true);
            __try
            {
                std::uint32_t n = 0;
                for (std::uint32_t i = 0; i < kNewRows; ++i)
                {
                    if (IsReservedFlowRow(i))
                        continue;
                    if (RowQualifiesForList(base20, i))
                        ++n;
                }
                if (n == 0)
                    return g_OrigListDevCount(base20);
                if (n > kOldRows)
                {
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        LogDebug("[DevelopArrayGrow] menu candidate count %u over "
                                 "the native 1024-entry list buffer - clamped; "
                                 "items past the cap will not list\n", n);
                    }
                    n = kOldRows;
                }
                return static_cast<std::uint16_t>(n);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[DevelopArrayGrow] hkListDevCount: exception scanning "
                         "grown rows - falling back to the native 1024 scan\n");
                return g_OrigListDevCount(base20);
            }
        }

        void __fastcall hkListDevFill(std::uintptr_t base20,
                                      std::uint16_t maxCount,
                                      std::uint16_t* outBuf)
        {
            if (!IsGrownBase20(base20))
            {
                g_OrigListDevFill(base20, maxCount, outBuf);
                return;
            }
            SyncFlagsWithSvars(true);
            __try
            {
                if (!outBuf || maxCount == 0)
                    return;
                std::uint16_t w = 0;
                for (std::uint32_t i = 0; i < kNewRows && w < maxCount; ++i)
                {
                    if (IsReservedFlowRow(i))
                        continue;
                    if (RowQualifiesForList(base20, i))
                        outBuf[w++] = static_cast<std::uint16_t>(i);
                }
                if (w == 0)
                    g_OrigListDevFill(base20, maxCount, outBuf);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                LogDebug("[DevelopArrayGrow] hkListDevFill: exception scanning "
                         "grown rows - falling back to the native 1024 scan\n");
                g_OrigListDevFill(base20, maxCount, outBuf);
            }
        }

        std::uint16_t FindLastRowForEquipId(std::uintptr_t base20,
                                            std::uint16_t equipId)
        {
            std::uint16_t found = 0x400;
            const std::uint32_t bound = equip::NativeFlowBound();
            for (std::uint32_t i = 0; i < bound; ++i)
            {
                const std::uint16_t packed =
                    *reinterpret_cast<const std::uint16_t*>(
                        base20 + 0xC
                        + static_cast<std::size_t>(i) * kRecStride);
                if (static_cast<std::uint16_t>(packed >> 4) == equipId)
                    found = static_cast<std::uint16_t>(i);
            }
            return found;
        }

        long long __fastcall hkEquipIdCount(std::uintptr_t base20,
                                            std::uint16_t equipId)
        {
            __try
            {
                const std::uint16_t found =
                    FindLastRowForEquipId(base20, equipId);
                const std::uintptr_t block = base20 - 0x20;
                const std::uintptr_t vt =
                    *reinterpret_cast<const std::uintptr_t*>(block);
                const RowQueryTail_t tail =
                    *reinterpret_cast<const RowQueryTail_t*>(vt + 0x90);
                const long long a = tail(block, equipId, found);

                const GetQuark_t getQuark = reinterpret_cast<GetQuark_t>(
                    ResolveGameAddress(gAddr.GetQuarkSystemTable));
                if (!getQuark)
                    return a;
                const std::uint8_t* quark = getQuark();
                if (!quark)
                    return a;
                const std::uintptr_t ext =
                    *reinterpret_cast<const std::uintptr_t*>(
                        *reinterpret_cast<const std::uintptr_t*>(quark + 0x98)
                        + 0x180);
                const std::uintptr_t extVt =
                    *reinterpret_cast<const std::uintptr_t*>(ext);
                const ExtCount_t cnt =
                    *reinterpret_cast<const ExtCount_t*>(extVt + 0xA8);
                return a + cnt(ext);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return g_OrigEquipIdCount(base20, equipId);
            }
        }

        long long __fastcall hkEquipIdRow(std::uintptr_t base20,
                                          std::uint16_t equipId)
        {
            __try
            {
                if (equipId == 0)
                    return 0;
                const std::uint16_t found =
                    FindLastRowForEquipId(base20, equipId);
                const std::uintptr_t block = base20 - 0x20;
                const std::uintptr_t vt =
                    *reinterpret_cast<const std::uintptr_t*>(block);
                const RowQueryTail_t tail =
                    *reinterpret_cast<const RowQueryTail_t*>(vt + 0x90);
                return tail(block, equipId, found);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return g_OrigEquipIdRow(base20, equipId);
            }
        }

        std::uint16_t __fastcall hkGetBaseId(std::uintptr_t base20,
                                             std::uint16_t row)
        {
            if (row >= static_cast<std::uint16_t>(kNewRows)
                || IsReservedFlowRow(row))
                return kFlowSentinel;
            const std::uint16_t parent = g_OrigGetBaseId(base20, row);
            if (parent >= static_cast<std::uint16_t>(kNewRows)
                || IsReservedFlowRow(parent))
                return kFlowSentinel;
            if (parent == row)
            {
                static std::atomic<int> s_selfParent{ 0 };
                if (s_selfParent.fetch_add(1, std::memory_order_relaxed) < 4)
                    LogDebug("[DevelopArrayGrow] develop row %u is its own parent - "
                             "reported as no-parent, because the menu's parent walk "
                             "would hang on the self-reference\n",
                        static_cast<unsigned>(row));
                return kFlowSentinel;
            }
            return parent;
        }

        bool VerifyPatchSite(const ImmPatch& p)
        {
            const std::uint8_t* addr = reinterpret_cast<const std::uint8_t*>(
                ResolveGameAddress(p.addr));
            if (!addr)
                return false;
            __try
            {
                if (std::memcmp(addr, p.op, p.opLen) != 0)
                {
                    char have[16] = {};
                    char want[16] = {};
                    for (std::uint8_t i = 0; i < p.opLen && i < 5; ++i)
                    {
                        std::snprintf(have + i * 3, 4, "%02X ", addr[i]);
                        std::snprintf(want + i * 3, 4, "%02X ", p.op[i]);
                    }
                    NoteVerifyFail("site %s @%p: opcode mismatch (have %s- "
                                   "expected %s)", p.what, addr, have, want);
                    return false;
                }
                std::uint32_t imm;
                std::memcpy(&imm, addr + p.opLen, 4);
                if (imm != p.oldImm && imm != p.newImm)
                {
                    NoteVerifyFail("site %s @%p: imm 0x%X is neither orig "
                                   "0x%X nor patched 0x%X", p.what, addr, imm,
                                   p.oldImm, p.newImm);
                    return false;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                NoteVerifyFail("site %s @%p: unreadable", p.what, addr);
                return false;
            }
        }

        bool ApplyPatchSite(const ImmPatch& p)
        {
            std::uint8_t* addr = reinterpret_cast<std::uint8_t*>(
                const_cast<void*>(ResolveGameAddress(p.addr)));
            if (!addr)
                return false;
            DWORD oldProt = 0;
            if (!VirtualProtect(addr, p.opLen + 4, PAGE_EXECUTE_READWRITE,
                                &oldProt))
            {
                Log("[DevelopArrayGrow] site %s @%p: VirtualProtect failed "
                    "(%lu)\n", p.what, addr, GetLastError());
                return false;
            }
            std::memcpy(addr + p.opLen, &p.newImm, 4);
            DWORD tmp = 0;
            VirtualProtect(addr, p.opLen + 4, oldProt, &tmp);
            FlushInstructionCache(GetCurrentProcess(), addr, p.opLen + 4);
            return true;
        }

        int TotalPatchCount()
        {
            std::size_t listed = 0;
            for (std::size_t i = 0; i < kPatchCount; ++i)
                if (kPatches[i].addr)
                    ++listed;
            return static_cast<int>(listed + kCtlSiteCount
                                    + kBoundSiteCount + kSiblingLoopFixCount);
        }

        int ApplyAllPatches()
        {
            int applied = 0;
            for (const ImmPatch& p : kPatches)
                if (p.addr && ApplyPatchSite(p))
                    ++applied;
            for (std::size_t i = 0; i < kCtlSiteCount; ++i)
                if (WriteU32At(kCtlDispSites[i].addr, g_CtlDispOff[i],
                               CtlNewDisp(kCtlDispSites[i].addr + g_CtlDispOff[i],
                                          kCtlDispSites[i].oldDisp)))
                    ++applied;
            for (std::size_t i = 0; i < kBoundSiteCount; ++i)
                if (WriteU32At(kBoundImmSites[i].addr,
                               kBoundImmSites[i].immOff, kNewRows))
                    ++applied;
            for (const ByteFix& f : kSiblingLoopFixes)
                if (WriteByteFix(f, f.newBytes))
                    ++applied;
            return applied;
        }

        constexpr int kMaxSuspendedThreads = 512;

        int SuspendOtherThreads(HANDLE* handles)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return 0;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            int n = 0;
            const DWORD pid = GetCurrentProcessId();
            const DWORD tid = GetCurrentThreadId();
            if (Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID == pid
                        && te.th32ThreadID != tid
                        && n < kMaxSuspendedThreads)
                    {
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE,
                                              te.th32ThreadID);
                        if (h)
                        {
                            if (SuspendThread(h) != static_cast<DWORD>(-1))
                                handles[n++] = h;
                            else
                                CloseHandle(h);
                        }
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            return n;
        }

        void ResumeOtherThreads(HANDLE* handles, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                ResumeThread(handles[i]);
                CloseHandle(handles[i]);
            }
        }

        using QuarkAlloc_t = void* (__fastcall*)(std::uint64_t size,
                                                 std::uint32_t align,
                                                 std::uint32_t flags);

        QuarkAlloc_t g_OrigQuarkAlloc      = nullptr;
        void*        g_HookQuarkAlloc      = nullptr;
        bool         g_AllocGuardInstalled = false;

        void* __fastcall hkQuarkBlockAlloc(std::uint64_t size,
                                           std::uint32_t align,
                                           std::uint32_t flags)
        {
            std::uint64_t realSize = size;
            if (size == kOldAllocSize && align == 0x10 && flags == 0x3002C)
            {
                realSize = kNewAllocSize;
                LogDebug("[DevelopArrayGrow] CRITICAL: a develop-block allocation "
                         "still asked for the OLD size 0x%X after the size patch - "
                         "the protector restored original bytes; grown to 0x%X here "
                         "so a 4096-row init cannot overrun it\n",
                    kOldAllocSize, kNewAllocSize);
            }
            void* p = g_OrigQuarkAlloc(realSize, align, flags);
            if (p && realSize == kNewAllocSize && align == 0x10
                && flags == 0x3002C)
                std::memset(p, 0, kNewAllocSize);
            return p;
        }

        bool TryReadU32(std::uintptr_t va, std::uint32_t off,
                        std::uint32_t& out)
        {
            const std::uint8_t* addr = reinterpret_cast<const std::uint8_t*>(
                ResolveGameAddress(va));
            if (!addr)
                return false;
            __try
            {
                std::memcpy(&out, addr + off, 4);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool TryReadBytes(std::uintptr_t va, void* out, std::size_t n)
        {
            const std::uint8_t* addr = reinterpret_cast<const std::uint8_t*>(
                ResolveGameAddress(va));
            if (!addr)
                return false;
            __try
            {
                std::memcpy(out, addr, n);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        struct HealedSite
        {
            std::uint8_t  cls;
            std::uint16_t idx;
        };
        constexpr std::size_t kMaxHealed = 600;

        constexpr std::size_t kDetourWindow = 5;

        bool SiteInsideOurDetour(std::uintptr_t va)
        {
            void* const site = ResolveGameAddress(va);
            if (!site)
                return false;

            void* const hooks[] = {
                g_HookBlockReset,    g_HookSetDeveloped,  g_HookSetUndeveloped,
                g_HookFindByDevId,   g_HookFindByEquipId, g_HookListDevCount,
                g_HookListDevFill,   g_HookEquipIdCount,  g_HookEquipIdRow,
                g_HookGetBaseId,     g_HookEdcDtor,
            };

            const std::uintptr_t s = reinterpret_cast<std::uintptr_t>(site);
            for (void* h : hooks)
            {
                if (!h)
                    continue;
                const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(h);
                if (s >= b && s < b + kDetourWindow)
                    return true;
            }
            return false;
        }

        void RepairHealedPatches()
        {
            static HealedSite healed[kMaxHealed];
            std::size_t nHealed        = 0;
            int         nForeign       = 0;
            int         nDetoured      = 0;
            std::uintptr_t foreignAddr = 0;
            std::uint32_t  foreignVal  = 0;
            const char*    foreignWhat = nullptr;

            for (std::size_t i = 0; i < kPatchCount && nHealed < kMaxHealed;
                 ++i)
            {
                const ImmPatch& p = kPatches[i];
                if (!p.addr)
                    continue;
                std::uint32_t v = 0;
                if (!TryReadU32(p.addr, p.opLen, v))
                    continue;
                if (v == p.oldImm)
                    healed[nHealed++] =
                        { 0, static_cast<std::uint16_t>(i) };
                else if (v != p.newImm)
                {
                    if (nForeign++ == 0)
                    {
                        foreignAddr = p.addr;
                        foreignVal  = v;
                        foreignWhat = p.what;
                    }
                }
            }
            for (std::size_t i = 0; i < kCtlSiteCount && nHealed < kMaxHealed;
                 ++i)
            {
                if (SiteInsideOurDetour(kCtlDispSites[i].addr))
                {
                    ++nDetoured;
                    continue;
                }
                std::uint32_t v = 0;
                if (!TryReadU32(kCtlDispSites[i].addr, g_CtlDispOff[i], v))
                    continue;
                if (v == kCtlDispSites[i].oldDisp)
                    healed[nHealed++] =
                        { 1, static_cast<std::uint16_t>(i) };
                else if (v != CtlNewDisp(kCtlDispSites[i].addr + g_CtlDispOff[i],
                                         kCtlDispSites[i].oldDisp))
                {
                    if (nForeign++ == 0)
                    {
                        foreignAddr = kCtlDispSites[i].addr;
                        foreignVal  = v;
                        foreignWhat = "ctl disp";
                    }
                }
            }
            for (std::size_t i = 0;
                 i < kBoundSiteCount && nHealed < kMaxHealed; ++i)
            {
                if (SiteInsideOurDetour(kBoundImmSites[i].addr))
                {
                    ++nDetoured;
                    continue;
                }
                std::uint32_t v = 0;
                if (!TryReadU32(kBoundImmSites[i].addr,
                                kBoundImmSites[i].immOff, v))
                    continue;
                if (v == 0x400)
                    healed[nHealed++] =
                        { 2, static_cast<std::uint16_t>(i) };
                else if (v != kNewRows)
                {
                    if (nForeign++ == 0)
                    {
                        foreignAddr = kBoundImmSites[i].addr;
                        foreignVal  = v;
                        foreignWhat = "scan bound";
                    }
                }
            }
            for (std::size_t i = 0;
                 i < kSiblingLoopFixCount && nHealed < kMaxHealed; ++i)
            {
                const ByteFix& f = kSiblingLoopFixes[i];
                std::uint8_t cur[4] = {};
                if (!f.addr || f.len > 4
                    || !TryReadBytes(f.addr, cur, f.len))
                    continue;
                if (std::memcmp(cur, f.oldBytes, f.len) == 0)
                    healed[nHealed++] =
                        { 3, static_cast<std::uint16_t>(i) };
            }

            static bool detourNoteLogged = false;
            if (nDetoured != 0 && !detourNoteLogged)
            {
                detourNoteLogged = true;
                LogDebug("[DevelopArrayGrow] %d patched site(s) sit in the first "
                         "bytes of a function this module also detours - MinHook "
                         "copied the patched instruction into its trampoline, so "
                         "the bound is live through it; excluded from the integrity "
                         "re-check\n", nDetoured);
            }

            static int foreignLogged = 0;
            if (nForeign != 0 && foreignLogged < 3)
            {
                ++foreignLogged;
                Log("[DevelopArrayGrow] WARNING: %d patched site(s) hold neither "
                    "the original nor the patched value (first %s @0x%llX = 0x%X) - "
                    "a detour on the same bytes or an encrypted clone; left "
                    "untouched, rechecked next pass\n",
                    nForeign,
                    foreignWhat ? foreignWhat : "?",
                    static_cast<unsigned long long>(foreignAddr),
                    foreignVal);
            }
            if (nHealed == 0)
                return;

            static HANDLE suspended[kMaxSuspendedThreads];
            const int nSuspended = SuspendOtherThreads(suspended);
            std::size_t repaired = 0;
            for (std::size_t i = 0; i < nHealed; ++i)
            {
                bool ok = false;
                switch (healed[i].cls)
                {
                case 0:
                {
                    const ImmPatch& p = kPatches[healed[i].idx];
                    ok = WriteImmAt(p, p.newImm);
                    break;
                }
                case 1:
                {
                    const std::size_t k = healed[i].idx;
                    ok = WriteU32At(kCtlDispSites[k].addr, g_CtlDispOff[k],
                                    CtlNewDisp(kCtlDispSites[k].addr
                                                   + g_CtlDispOff[k],
                                               kCtlDispSites[k].oldDisp));
                    break;
                }
                case 2:
                {
                    const std::size_t k = healed[i].idx;
                    ok = WriteU32At(kBoundImmSites[k].addr,
                                    kBoundImmSites[k].immOff, kNewRows);
                    break;
                }
                case 3:
                {
                    const ByteFix& f = kSiblingLoopFixes[healed[i].idx];
                    ok = WriteByteFix(f, f.newBytes);
                    break;
                }
                }
                if (ok)
                    ++repaired;
            }
            ResumeOtherThreads(suspended, nSuspended);

            Log("[DevelopArrayGrow] WARNING: the protector restored %zu patched "
                "site(s) to original bytes - %zu re-applied; develop data touched "
                "in that window may be inconsistent until the next R&D rebuild\n", nHealed, repaired);
            InvalidateDevelopVisibilityCache();
            InvalidateDevelopLookupIndex();
        }

        std::uintptr_t g_LateCloneLogged[16] = {};
        std::size_t    g_LateCloneLoggedCount = 0;

        bool LateCloneAlreadyLogged(std::uintptr_t va)
        {
            for (std::size_t i = 0; i < g_LateCloneLoggedCount; ++i)
                if (g_LateCloneLogged[i] == va)
                    return true;
            if (g_LateCloneLoggedCount
                < sizeof(g_LateCloneLogged) / sizeof(g_LateCloneLogged[0]))
                g_LateCloneLogged[g_LateCloneLoggedCount++] = va;
            return false;
        }

        void RecheckForLateClones()
        {
            if (SweepImageForUnknownSites(true))
                return;
            const int nFind = g_SweepUnknownCount;
            if (nFind == 0)
                return;

            static std::uint16_t order[kMaxCtlSites];
            for (std::size_t i = 0; i < kCtlSiteCount; ++i)
                order[i] = static_cast<std::uint16_t>(i);
            for (std::size_t a = 1; a < kCtlSiteCount; ++a)
            {
                const std::uint16_t key = order[a];
                const std::uintptr_t kv =
                    kCtlDispSites[key].addr + g_CtlDispOff[key];
                std::size_t b = a;
                while (b > 0
                       && kCtlDispSites[order[b - 1]].addr
                              + g_CtlDispOff[order[b - 1]] > kv)
                {
                    order[b] = order[b - 1];
                    --b;
                }
                order[b] = key;
            }

            int g0 = 0;
            while (g0 < nFind)
            {
                int g1 = g0 + 1;
                while (g1 < nFind
                       && g_SweepUnknown[g1].va - g_SweepUnknown[g1 - 1].va
                              <= 0x1000)
                    ++g1;
                const int n = g1 - g0;

                std::size_t matchStart = 0;
                int matches = 0;
                if (n >= 2 && static_cast<std::size_t>(n) <= kCtlSiteCount)
                {
                    for (std::size_t j = 0;
                         j + static_cast<std::size_t>(n) <= kCtlSiteCount;
                         ++j)
                    {
                        bool fit = true;
                        for (int i = 0; i < n && fit; ++i)
                        {
                            const CtlDispSite& s =
                                kCtlDispSites[order[j + i]];
                            if (s.oldDisp != g_SweepUnknown[g0 + i].disp)
                                fit = false;
                        }
                        for (int i = 0; i + 1 < n && fit; ++i)
                        {
                            const std::intptr_t fg = static_cast<std::intptr_t>(
                                g_SweepUnknown[g0 + i + 1].va
                                - g_SweepUnknown[g0 + i].va);
                            const std::intptr_t kg = static_cast<std::intptr_t>(
                                (kCtlDispSites[order[j + i + 1]].addr
                                 + g_CtlDispOff[order[j + i + 1]])
                                - (kCtlDispSites[order[j + i]].addr
                                   + g_CtlDispOff[order[j + i]]));
                            if (fg - kg > 0x80 || kg - fg > 0x80)
                                fit = false;
                        }
                        if (fit)
                        {
                            ++matches;
                            matchStart = j;
                        }
                    }
                }

                if (matches == 1)
                {
                    static HANDLE suspended[kMaxSuspendedThreads];
                    const int nSuspended = SuspendOtherThreads(suspended);
                    int patched = 0;
                    for (int i = 0; i < n; ++i)
                        if (WriteU32At(g_SweepUnknown[g0 + i].va, 0,
                                       CtlNewDisp(g_SweepUnknown[g0 + i].va,
                                                  g_SweepUnknown[g0 + i].disp)))
                            ++patched;
                    ResumeOtherThreads(suspended, nSuspended);
                    Log("[DevelopArrayGrow] WARNING: a late-materialized clone "
                        "surfaced with %d unpatched develop-field ref(s) at 0x%llX "
                        "(clone of the %zu-site run near 0x%llX) - %d patched in "
                        "place\n",
                        n,
                        static_cast<unsigned long long>(
                            g_SweepUnknown[g0].va),
                        static_cast<std::size_t>(n),
                        static_cast<unsigned long long>(
                            kCtlDispSites[order[matchStart]].addr),
                        patched);
                }
                else if (!LateCloneAlreadyLogged(g_SweepUnknown[g0].va))
                {
                    LogDebug("[DevelopArrayGrow] CRITICAL: %d unpatched "
                             "develop-field ref(s) at 0x%llX match %d known-site "
                             "runs (need exactly 1%s) - NOT patched; code running "
                             "through that copy reads the OLD control offsets, "
                             "which overlap grown rows 1024..1259\n",
                        n,
                        static_cast<unsigned long long>(
                            g_SweepUnknown[g0].va),
                        matches,
                        n < 2 ? "; a lone site is never matched, the run "
                                "matcher needs two or more" : "");
                    for (int i = 0; i < n; ++i)
                    {
                        const SweepFind& f = g_SweepUnknown[g0 + i];
                        std::size_t owners = 0;
                        std::uintptr_t firstOwner = 0;
                        for (std::size_t k = 0; k < kCtlSiteCount; ++k)
                            if (kCtlDispSites[k].oldDisp == f.disp)
                            {
                                if (owners == 0)
                                    firstOwner = kCtlDispSites[k].addr;
                                ++owners;
                            }
                        char bytes[80];
                        bytes[0] = '\0';
                        __try
                        {
                            const std::uint8_t* p =
                                reinterpret_cast<const std::uint8_t*>(
                                    f.va - 6);
                            for (int b = 0; b < 14; ++b)
                                std::snprintf(bytes + b * 3, 4, "%02X ", p[b]);
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER)
                        {
                            std::snprintf(bytes, sizeof(bytes),
                                          "<unreadable>");
                        }
                        LogDebug("[DevelopArrayGrow]   site %d: 0x%llX disp 0x%X "
                                 "held by %zu known site(s), first 0x%llX | bytes "
                                 "%s\n",
                            i + 1,
                            static_cast<unsigned long long>(f.va),
                            f.disp, owners,
                            static_cast<unsigned long long>(firstOwner),
                            bytes);
                    }
                }
                g0 = g1;
            }
        }

        std::atomic<bool> g_IntegrityThreadStarted{ false };

        DWORD WINAPI GrowIntegrityThread(LPVOID)
        {
            SetThreadPriority(GetCurrentThread(),
                              THREAD_PRIORITY_BELOW_NORMAL);
            for (int pass = 0; ; ++pass)
            {
                Sleep(pass < 36 ? 5000 : 30000);
                if (!g_Active)
                    continue;
                RepairHealedPatches();
                if ((pass & 3) == 3)
                    RecheckForLateClones();
            }
        }

        bool MigrateExistingBlock()
        {
            const GetQuark_t getQuark = reinterpret_cast<GetQuark_t>(
                ResolveGameAddress(gAddr.GetQuarkSystemTable));
            const QuarkAlloc_t quarkAlloc = reinterpret_cast<QuarkAlloc_t>(
                ResolveGameAddress(kAddr_QuarkBlockHeapAlloc));
            if (!getQuark || !quarkAlloc)
            {
                NoteVerifyFail("migrate: quark resolver addresses missing");
                return false;
            }

            static HANDLE suspended[kMaxSuspendedThreads];
            volatile int nSuspended = -1;

            __try
            {
                std::uint8_t* quark = getQuark();
                if (!quark)
                {
                    NoteVerifyFail("migrate: quark table is null");
                    return false;
                }
                const std::uintptr_t app =
                    *reinterpret_cast<const std::uintptr_t*>(quark + 0x98);
                if (!app)
                {
                    NoteVerifyFail("migrate: app object (quark+0x98) is null");
                    return false;
                }
                const std::uintptr_t f110 =
                    *reinterpret_cast<const std::uintptr_t*>(app + 0x110);
                if (!f110)
                {
                    NoteVerifyFail("migrate: holder (app+0x110) is null");
                    return false;
                }
                std::uintptr_t* slot =
                    reinterpret_cast<std::uintptr_t*>(f110 + 0xAC8);
                const std::uintptr_t base20 = *slot;
                if (!base20)
                {
                    NoteVerifyFail("migrate: develop slot (+0xAC8) is null");
                    return false;
                }
                const std::uintptr_t block = base20 - 0x20;

                const std::uintptr_t vt0 =
                    *reinterpret_cast<const std::uintptr_t*>(block);
                const std::uintptr_t vt20 =
                    *reinterpret_cast<const std::uintptr_t*>(block + 0x20);
                const std::uintptr_t vtblA = reinterpret_cast<std::uintptr_t>(
                    ResolveGameAddress(kAddr_BlockVtblA));
                const std::uintptr_t vtblB = reinterpret_cast<std::uintptr_t>(
                    ResolveGameAddress(kAddr_BlockVtblB));
                const std::uintptr_t vt20A = reinterpret_cast<std::uintptr_t>(
                    ResolveGameAddress(kAddr_Base20VtblA));
                const std::uintptr_t vt20B = reinterpret_cast<std::uintptr_t>(
                    ResolveGameAddress(kAddr_Base20VtblB));
                if ((vt0 != vtblA && vt0 != vtblB)
                    || (vt20 != vt20A && vt20 != vt20B))
                {
                    NoteVerifyFail("migrate: block %p vtables %p/%p do not "
                                   "match the develop block class",
                                   reinterpret_cast<void*>(block),
                                   reinterpret_cast<void*>(vt0),
                                   reinterpret_cast<void*>(vt20));
                    return false;
                }

                std::uint8_t* nb = static_cast<std::uint8_t*>(
                    quarkAlloc(kNewAllocSize, 0x10, 0x3002C));
                if (!nb)
                {
                    NoteVerifyFail("migrate: QuarkBlockHeapAlloc(0x%X) "
                                   "returned null", kNewAllocSize);
                    return false;
                }
                std::memset(nb, 0, kNewAllocSize);
                std::memcpy(nb, reinterpret_cast<const void*>(block), 0x28);
                std::memcpy(nb + 0x28,
                            reinterpret_cast<const void*>(block + 0x28),
                            static_cast<std::size_t>(kOldRows) * kRecStride);
                std::memcpy(nb + kNewCtlOff,
                            reinterpret_cast<const void*>(block + kOldCtlOff),
                            kCtlSize);
                std::memcpy(nb + kNewParallelOff,
                            reinterpret_cast<const void*>(block + 0x1A028),
                            static_cast<std::size_t>(kOldRows) * kParStride);

                nSuspended = SuspendOtherThreads(suspended);
                const int applied = ApplyAllPatches();
                if (applied != TotalPatchCount())
                {
                    RevertAllPatches("partial apply during migration");
                    ResumeOtherThreads(suspended, nSuspended);
                    nSuspended = -1;
                    NoteVerifyFail("migrate: only %d of %d patches applied - "
                                   "reverted", applied, TotalPatchCount());
                    return false;
                }
                *slot = reinterpret_cast<std::uintptr_t>(nb + 0x20);
                g_GrownBlock = reinterpret_cast<std::uintptr_t>(nb);
                ResumeOtherThreads(suspended, nSuspended);
                nSuspended = -1;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                if (nSuspended >= 0)
                    ResumeOtherThreads(suspended, nSuspended);
                NoteVerifyFail("migrate: exception during block copy or "
                               "pointer swap");
                return false;
            }
        }
    }

    bool DevelopArrayGrowActive()
    {
        return g_Active;
    }


    void SilenceDevelopRowAnnounce(std::int32_t developId)
    {
        if (developId <= 0)
            return;
        {
            std::lock_guard<std::mutex> lock(g_SilencedMutex);
            for (std::int32_t known : g_SilencedDevelopIds)
            {
                if (known == developId)
                    return;
            }
            g_SilencedDevelopIds.push_back(developId);
        }
        if (!g_Active)
            return;
        const std::uint32_t row = AnnouncedRowOf(developId, -1);
        if (row == 0)
            return;
        g_FlagsShadow[row] |= kReqAnnouncedBit;
        MirrorFlagByte(row);
    }

    void AssertDevelopRowAnnounced(std::int32_t developId, std::int32_t flowIndex)
    {
        if (!g_Active || developId <= 0)
            return;

        const std::uint32_t row = AnnouncedRowOf(developId, flowIndex);
        if (row == 0)
            return;

        const bool persisted =
            V_FrameWorkState::GetDevReqAnnouncedByDevelopId(developId);
        const bool initial = EquipDevelop_IsDevelopInitiallyAvailable(
            static_cast<std::uint32_t>(developId));

        if (!persisted && !initial)
        {
            g_FlagsShadow[row] &= static_cast<std::uint8_t>(~kReqAnnouncedBit);
            MirrorFlagByte(row);
            return;
        }

        g_FlagsShadow[row] |= kReqAnnouncedBit;
        MirrorFlagByte(row);
        if (!persisted)
            V_FrameWorkState::SetDevReqAnnouncedByDevelopId(developId, true);
    }

    void InvalidateDevelopVisibilityCache()
    {
        g_VisValid = false;
        g_VisSelf  = nullptr;
    }

    void InvalidateDevelopLookupIndex()
    {
        g_LookupValid = false;
    }

    void DevelopLookupTakeCounters(unsigned long long& calls,
                                   unsigned long long& indexed,
                                   unsigned long long& builds,
                                   unsigned long long& stale)
    {
        calls   = g_FindCalls;
        indexed = g_FindIndexed;
        builds  = g_LookupBuilds;
        stale   = g_FindStale;
        g_FindCalls    = 0;
        g_FindIndexed  = 0;
        g_LookupBuilds = 0;
        g_FindStale    = 0;
    }

    void BeginDevelopVisibilityCache()
    {
        if (!g_IsVisibleInstalled)
            return;
        g_VisValid  = false;
        g_VisSelf   = nullptr;
        g_VisCalls  = 0;
        g_VisHits   = 0;
        g_SibCalls  = 0;
        g_SibSum    = 0;
        g_SibMax    = 0;
        g_VisThread = GetCurrentThreadId();
        QueryPerformanceCounter(&g_VisT0);
        g_VisArmed  = true;
    }

    void LogDevelopScanCounters(const char* phase)
    {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const double ms = (g_VisT0.QuadPart && f.QuadPart)
            ? (double)(t1.QuadPart - g_VisT0.QuadPart) * 1000.0 / (double)f.QuadPart
            : 0.0;
        LogDebug("[DevelopArrayGrow] list build took %.1f ms\n", ms);
        LogDebug("[DevelopArrayGrow] scan counters (%s): visibility predicate %llu "
                 "calls / %llu cached; sibling-count %llu calls, %llu collected "
                 "rows, largest group %u (collected rows = the retire loop's total "
                 "iterations)\n",
            phase,
            static_cast<unsigned long long>(g_VisCalls),
            static_cast<unsigned long long>(g_VisHits),
            static_cast<unsigned long long>(g_SibCalls),
            static_cast<unsigned long long>(g_SibSum),
            static_cast<unsigned>(g_SibMax));
    }

    void EndDevelopVisibilityCache()
    {
        if (!g_VisArmed)
            return;
        g_VisArmed = false;
        LogDevelopScanCounters("build finished");
    }

    DevelopVisibilityScope::DevelopVisibilityScope()
        : owned(false)
    {
        if (!g_IsVisibleInstalled || g_VisArmed)
            return;
        g_VisValid  = false;
        g_VisSelf   = nullptr;
        g_VisCalls  = 0;
        g_VisHits   = 0;
        g_SibCalls  = 0;
        g_SibSum    = 0;
        g_SibMax    = 0;
        g_VisThread = GetCurrentThreadId();
        QueryPerformanceCounter(&g_VisT0);
        g_VisArmed  = true;
        owned       = true;
    }

    DevelopVisibilityScope::~DevelopVisibilityScope()
    {
        if (owned)
            g_VisArmed = false;
    }

    void DevelopVisibilityTakeCounters(unsigned long long& calls,
                                       unsigned long long& hits)
    {
        calls = g_VisWinCalls;
        hits  = g_VisWinHits;
        g_VisWinCalls = 0;
        g_VisWinHits  = 0;
    }

    std::uint32_t NativeFlowBound()
    {
        return g_Active ? kNewRows : kOldRows;
    }

    bool GunsmithFlowRowClaimed(std::uint32_t row)
    {
        if (row < kGunsmithFlowFirst || row > kGunsmithFlowLast)
            return false;
        return ((g_GunsmithClaimMask.load(std::memory_order_relaxed)
                 >> (row - kGunsmithFlowFirst)) & 1u) != 0;
    }

    std::size_t DevFlagsPtrOffsetBase20()
    {
        return (g_Active ? kNewCtlOff : kOldCtlOff) - 0x20;
    }

    void DevFlagsWriteByte(void* controller, std::uint32_t index,
                           std::uint8_t value)
    {
        if (!controller || index >= NativeFlowBound())
            return;
        __try
        {
            std::uint8_t* bits = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(controller)
                + DevFlagsPtrOffsetBase20());
            if (!bits)
                return;
            if (index >= kOldRows && bits != g_FlagsShadow)
                return;
            bits[index] = value;
            InvalidateDevelopVisibilityCache();
            if (bits == g_FlagsShadow)
                MirrorFlagByte(index);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    std::uint32_t FirstCustomFlowIndex()
    {
        return kFirstCustomRow;
    }

    bool TryReadRowDevelopId(std::uint32_t index, std::uint32_t& out)
    {
        if (index >= NativeFlowBound())
            return false;
        const std::uintptr_t base20 =
            g_ArmedBase20.load(std::memory_order_relaxed);
        if (!base20)
            return false;
        std::uint16_t dev = 0;
        if (!RecordFieldAt(base20, static_cast<std::uint16_t>(index), false, dev))
            return false;
        out = dev;
        return true;
    }

    bool DevFlagsTryReadByte(void* controller, std::uint32_t index,
                             std::uint8_t& out)
    {
        if (!controller || index >= NativeFlowBound())
            return false;
        __try
        {
            std::uint8_t* bits = *reinterpret_cast<std::uint8_t**>(
                reinterpret_cast<std::uint8_t*>(controller)
                + DevFlagsPtrOffsetBase20());
            if (!bits)
                return false;
            if (index >= kOldRows && bits != g_FlagsShadow)
                return false;
            out = bits[index];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void EnsureDevelopBlockArmed(void* base20)
    {
        if (!g_Active || !base20)
            return;
        if (g_ArmedBase20.load(std::memory_order_relaxed)
            != reinterpret_cast<std::uintptr_t>(base20))
            ArmFromBase20(reinterpret_cast<std::uintptr_t>(base20));
        SyncFlagsWithSvars(false);
    }

    void SyncDevelopFlagsWithSave()
    {
        SyncFlagsWithSvars(false);
    }

    std::size_t CollectVanillaIdentityEquipIds(std::int32_t* out,
                                               std::size_t capacity)
    {
        if (!out || capacity == 0)
            return 0;
        const std::uintptr_t base20 =
            g_ArmedBase20.load(std::memory_order_relaxed);
        if (!base20)
            return 0;
        __try
        {
            BuildLookupIndex(base20, equip::NativeFlowBound());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_LookupValid = false;
            return 0;
        }

        std::size_t n = 0;
        for (std::uint32_t w = 0; w < kEquipIdSlots / 64 && n < capacity; ++w)
        {
            if (g_VanIdentityBits[w] == 0)
                continue;
            for (std::uint32_t b = 0; b < 64 && n < capacity; ++b)
                if (g_VanIdentityBits[w] & (1ull << b))
                    out[n++] = static_cast<std::int32_t>(w * 64 + b);
        }
        return n;
    }

    void PreApplyDevelopArrayGrowPatches()
    {
        if (g_PrePatch != PrePatchState::NotAttempted)
            return;
        if (!SelectBuildTables())
        {
            g_PrePatch = PrePatchState::WrongBuild;
            return;
        }
        if (GrowDisabledByMarker())
        {
            g_PrePatch = PrePatchState::DisabledByMarker;
            return;
        }
        for (std::size_t i = 0; i < kPatchCount; ++i)
            if (kPatches[i].addr && !VerifyPatchSite(kPatches[i]))
            {
                g_PrePatch       = PrePatchState::VerifyFailed;
                g_PrePatchDetail = static_cast<int>(i);
                return;
            }
        {
            std::size_t ctlFailed[16];
            std::size_t nCtlFailed   = 0;
            std::size_t ctlFirstFail = 0;
            for (std::size_t i = 0; i < kCtlSiteCount; ++i)
                if (!VerifyCtlSiteAt(i, kCtlDispSites[i].addr, true))
                {
                    if (nCtlFailed == 0)
                        ctlFirstFail = i;
                    if (nCtlFailed < 16)
                        ctlFailed[nCtlFailed] = i;
                    ++nCtlFailed;
                }
            if (nCtlFailed > 16)
            {
                VerifyCtlSite(ctlFirstFail);
                g_PrePatch       = PrePatchState::RetryPending;
                g_PrePatchDetail = 1000 + static_cast<int>(ctlFirstFail);
                return;
            }
            if (nCtlFailed > 0
                && !RelocateCloneCtlSites(ctlFailed, nCtlFailed))
            {
                g_PrePatch       = PrePatchState::RetryPending;
                g_PrePatchDetail = 1000 + static_cast<int>(ctlFirstFail);
                return;
            }
        }
        for (std::size_t i = 0; i < kBoundSiteCount; ++i)
            if (!VerifyBoundSite(kBoundImmSites[i]))
            {
                g_PrePatch       = PrePatchState::VerifyFailed;
                g_PrePatchDetail = 2000 + static_cast<int>(i);
                return;
            }
        for (std::size_t i = 0; i < kSiblingLoopFixCount; ++i)
            if (!VerifyByteFix(kSiblingLoopFixes[i]))
            {
                g_PrePatch       = PrePatchState::VerifyFailed;
                g_PrePatchDetail = 4000 + static_cast<int>(i);
                return;
            }

        if (!SweepImageForUnknownSites())
        {
            g_PrePatch       = PrePatchState::VerifyFailed;
            g_PrePatchDetail = 3000;
            return;
        }

        if (EquipDevelop_ResolveDevelopController())
        {
            if (!MigrateExistingBlock())
            {
                g_PrePatch = PrePatchState::BlockExists;
                return;
            }
            g_Migrated = true;
            g_PrePatch = PrePatchState::Applied;
            return;
        }

        const int applied = ApplyAllPatches();
        if (applied != TotalPatchCount())
        {
            RevertAllPatches("partial apply");
            g_PrePatch       = PrePatchState::ApplyFailed;
            g_PrePatchDetail = applied;
            return;
        }

        if (EquipDevelop_ResolveDevelopController())
        {
            RevertAllPatches("develop block appeared mid-patch");
            g_PrePatch = PrePatchState::RacedMidPatch;
            return;
        }

        g_PrePatch = PrePatchState::Applied;
    }

    namespace
    {
        bool LogGrowInstallGate()
        {
        switch (g_PrePatch)
        {
        case PrePatchState::Applied:
            return true;
        case PrePatchState::WrongBuild:
            LogDebug("[DevelopArrayGrow] no relocation table for this build (only "
                     "EN/JP 1.0.15.4 are ported) - develop array stays 1024 rows\n");
            return false;
        case PrePatchState::DisabledByMarker:
            LogDebug("[DevelopArrayGrow] DISABLED by "
                     "V_FrameWork_no_develop_grow.txt - bound stays 1024 rows; "
                     "delete that file to re-enable\n");
            return false;
        case PrePatchState::BlockExists:
            LogDebug("[DevelopArrayGrow] REFUSING install: the develop block "
                     "predates the DLL and could not be migrated (%s) - bound stays "
                     "1024\n",
                g_PreVerifyDetail[0] ? g_PreVerifyDetail : "no detail");
            return false;
        case PrePatchState::VerifyFailed:
            Log("[DevelopArrayGrow] REFUSING install: patch site %d failed "
                "verification at DllMain (0=core, 1000+=ctl disp, 2000+=scan bound, "
                "3000=image sweep, 4000+=loop byte fix; %s) - nothing applied, "
                "bound stays 1024\n", g_PrePatchDetail,
                g_PreVerifyDetail[0] ? g_PreVerifyDetail : "no detail");
            return false;
        case PrePatchState::ApplyFailed:
            LogDebug("[DevelopArrayGrow] REFUSING install: only %d patch sites "
                     "applied at DllMain - all reverted, bound stays 1024\n", g_PrePatchDetail);
            return false;
        case PrePatchState::RacedMidPatch:
            LogDebug("[DevelopArrayGrow] REFUSING install: the develop block was "
                     "constructed mid-patch at DllMain - all reverted, bound stays "
                     "1024\n");
            return false;
        default:
            return false;
        }
        }

        bool InstallGrowHooksAndFinalize()
        {
        const bool hooks =
            CreateAndEnableHook(ResolveGameAddress(kAddr_BlockReset),
                                reinterpret_cast<void*>(&hkBlockReset),
                                reinterpret_cast<void**>(&g_OrigBlockReset))
            && (g_HookBlockReset = ResolveGameAddress(kAddr_BlockReset)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_SetDeveloped),
                                   reinterpret_cast<void*>(&hkSetDeveloped),
                                   reinterpret_cast<void**>(&g_OrigSetDeveloped))
            && (g_HookSetDeveloped = ResolveGameAddress(kAddr_SetDeveloped)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_SetUndeveloped),
                                   reinterpret_cast<void*>(&hkSetUndeveloped),
                                   reinterpret_cast<void**>(&g_OrigSetUndeveloped))
            && (g_HookSetUndeveloped = ResolveGameAddress(kAddr_SetUndeveloped)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_FindByDevId),
                                   reinterpret_cast<void*>(&hkFindByDevId),
                                   reinterpret_cast<void**>(&g_OrigFindByDevId))
            && (g_HookFindByDevId = ResolveGameAddress(kAddr_FindByDevId)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_FindByEquipId),
                                   reinterpret_cast<void*>(&hkFindByEquipId),
                                   reinterpret_cast<void**>(&g_OrigFindByEquipId))
            && (g_HookFindByEquipId = ResolveGameAddress(kAddr_FindByEquipId)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_ListDevCount),
                                   reinterpret_cast<void*>(&hkListDevCount),
                                   reinterpret_cast<void**>(&g_OrigListDevCount))
            && (g_HookListDevCount = ResolveGameAddress(kAddr_ListDevCount)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_ListDevFill),
                                   reinterpret_cast<void*>(&hkListDevFill),
                                   reinterpret_cast<void**>(&g_OrigListDevFill))
            && (g_HookListDevFill = ResolveGameAddress(kAddr_ListDevFill)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_EquipIdCount),
                                   reinterpret_cast<void*>(&hkEquipIdCount),
                                   reinterpret_cast<void**>(&g_OrigEquipIdCount))
            && (g_HookEquipIdCount = ResolveGameAddress(kAddr_EquipIdCount)) != nullptr
            && CreateAndEnableHook(ResolveGameAddress(kAddr_EquipIdRow),
                                   reinterpret_cast<void*>(&hkEquipIdRow),
                                   reinterpret_cast<void**>(&g_OrigEquipIdRow))
            && (g_HookEquipIdRow = ResolveGameAddress(kAddr_EquipIdRow)) != nullptr
            && (kAddr_GetBaseId == 0
                || CreateAndEnableHook(ResolveGameAddress(kAddr_GetBaseId),
                                   reinterpret_cast<void*>(&hkGetBaseId),
                                   reinterpret_cast<void**>(&g_OrigGetBaseId))
            && (g_HookGetBaseId = ResolveGameAddress(kAddr_GetBaseId))
                   != nullptr);
        if (!hooks)
        {
            Log("[DevelopArrayGrow] REFUSING install: hook installation "
                "failed - reverting all patches, bound stays 1024\n");
            RevertAllPatches("hook installation failed");
            Uninstall_DevelopArrayGrow();
            return false;
        }

        g_ClampInstalled = CreateAndEnableHook(
            ResolveGameAddress(kAddr_SiblingCount),
            reinterpret_cast<void*>(&hkSiblingCount),
            reinterpret_cast<void**>(&g_OrigSiblingCount));
        if (!g_ClampInstalled)
            Log("[DevelopArrayGrow] sibling-count clamp hook failed - the collector "
                "now scans %u rows but still gathers into a 1024-entry stack array, "
                "so a bigger group overruns the menu thread's stack\n", kNewRows);

        g_IsVisibleInstalled = CreateAndEnableHook(
            ResolveGameAddress(kAddr_IsVisible),
            reinterpret_cast<void*>(&hkIsVisible),
            reinterpret_cast<void**>(&g_OrigIsVisible));
        if (!g_IsVisibleInstalled)
            Log("[DevelopArrayGrow] row-visibility cache hook failed - the "
                "predicate stays uncached, so every list build re-walks the "
                "QuarkSystemTable per row and weapon lists take seconds\n");

        g_AllocGuardInstalled = CreateAndEnableHook(
            ResolveGameAddress(kAddr_QuarkBlockHeapAlloc),
            reinterpret_cast<void*>(&hkQuarkBlockAlloc),
            reinterpret_cast<void**>(&g_OrigQuarkAlloc));
        if (g_AllocGuardInstalled)
            g_HookQuarkAlloc = ResolveGameAddress(kAddr_QuarkBlockHeapAlloc);
        else
            Log("[DevelopArrayGrow] block-alloc guard hook failed - a fresh develop "
                "block keeps an uninitialized grown tail, so the relocated control "
                "fields hold garbage and the dtor frees it\n");

        if (kAddr_EdcDtor != 0)
        {
            g_DtorGuardInstalled = CreateAndEnableHook(
                ResolveGameAddress(kAddr_EdcDtor),
                reinterpret_cast<void*>(&hkEdcDtorGuard),
                reinterpret_cast<void**>(&g_OrigEdcDtor));
            if (g_DtorGuardInstalled)
                g_HookEdcDtor = ResolveGameAddress(kAddr_EdcDtor);
            else
                Log("[DevelopArrayGrow] destructor guard hook failed - a teardown "
                    "fault in the EquipDevelopController dtor now crashes the game "
                    "at quit\n");
        }

        g_Active = true;

        if (!g_IntegrityThreadStarted.exchange(true))
        {
            HANDLE h = CreateThread(nullptr, 0, &GrowIntegrityThread,
                                    nullptr, 0, nullptr);
            if (h)
                CloseHandle(h);
            else
            {
                g_IntegrityThreadStarted.store(false);
                Log("[DevelopArrayGrow] patch-integrity watchdog failed to start - "
                    "protector byte-restores and late clones go undetected this "
                    "session\n");
            }
        }

        {
            int workBufSites = 0;
            int sunk = 0;
            for (std::size_t i = 0; i < kCtlSiteCount; ++i)
            {
                if (kCtlDispSites[i].oldDisp != kWorkBufDisp)
                    continue;
                ++workBufSites;
                if (IsWorkBufFreeLoad(kCtlDispSites[i].addr + g_CtlDispOff[i],
                                      kCtlDispSites[i].oldDisp))
                    ++sunk;
            }
            if (workBufSites != 0 && sunk == 0)
                Log("[DevelopArrayGrow] WARNING: none of the %d develop-controller "
                    "work-buffer site(s) matched the free-load form, so every "
                    "teardown still frees that buffer once per destructor - the "
                    "repeat free corrupts the heap and the process dies at exit\n",
                    workBufSites);
        }
        LogDebug("[DevelopArrayGrow] INSTALLED: develop array 1024 -> %u rows (%s: "
                 "%zu core + %zu ctl disps + %zu scan bounds + %zu loop/sentinel "
                 "fixes; 10 hooks + %s); custom flow band %u..%u minus rows "
                 "0x3FD-0x400; ctl fields 0x%X -> 0x%X; flags rows >= 1024 "
                 "shadow-backed\n",
            kNewRows,
            g_Migrated ? "pre-existing block MIGRATED + patches applied"
                       : "patches pre-applied",
            kPatchCount,
            kCtlSiteCount, kBoundSiteCount, kSiblingLoopFixCount,
            g_ClampInstalled ? "sibling-count clamp" : "NO clamp",
            922, kNewRows - 1, kOldCtlOff, kNewCtlOff);
        return true;
        }

        DWORD WINAPI GrowRetryThread(LPVOID)
        {
            for (int i = 0; i < 60; ++i)
            {
                Sleep(250);
                g_PrePatch = PrePatchState::NotAttempted;
                PreApplyDevelopArrayGrowPatches();
                if (g_PrePatch != PrePatchState::RetryPending)
                    break;
            }
            if (g_PrePatch == PrePatchState::RetryPending)
            {
                g_PrePatch = PrePatchState::VerifyFailed;
                Log("[DevelopArrayGrow] REFUSING install: patch site %d still "
                    "failed verification after 15s of retries (%s) - the protector "
                    "never materialized that region; bound stays 1024\n", g_PrePatchDetail,
                    g_PreVerifyDetail[0] ? g_PreVerifyDetail : "no detail");
                return 0;
            }
            if (LogGrowInstallGate())
                InstallGrowHooksAndFinalize();
            return 0;
        }
    }

    bool Install_DevelopArrayGrow()
    {
        if (g_Active)
            return true;

        if (g_PrePatch == PrePatchState::NotAttempted)
            PreApplyDevelopArrayGrowPatches();

        if (g_PrePatch == PrePatchState::RetryPending)
        {
            LogDebug("[DevelopArrayGrow] ctl site %d not verifiable yet - the "
                     "protector has not materialized that clone region (%s); "
                     "retrying in the background for up to 15s\n",
                g_PrePatchDetail >= 1000 ? g_PrePatchDetail - 1000
                                         : g_PrePatchDetail,
                g_PreVerifyDetail[0] ? g_PreVerifyDetail : "no detail");
            HANDLE h = CreateThread(nullptr, 0, &GrowRetryThread, nullptr,
                                    0, nullptr);
            if (h)
            {
                CloseHandle(h);
                return true;
            }
            g_PrePatch = PrePatchState::VerifyFailed;
            LogDebug("[DevelopArrayGrow] REFUSING install: could not start the "
                     "background retry thread - bound stays 1024\n");
            return false;
        }

        if (!LogGrowInstallGate())
            return false;
        return InstallGrowHooksAndFinalize();
    }

    void Uninstall_DevelopArrayGrow()
    {
        if (g_AllocGuardInstalled)
        {
            DisableAndRemoveHook(g_HookQuarkAlloc
                                     ? g_HookQuarkAlloc
                                     : ResolveGameAddress(
                                           kAddr_QuarkBlockHeapAlloc));
            g_AllocGuardInstalled = false;
            g_OrigQuarkAlloc     = nullptr;
            g_HookQuarkAlloc     = nullptr;
        }
        if (g_IsVisibleInstalled)
        {
            DisableAndRemoveHook(ResolveGameAddress(kAddr_IsVisible));
            g_IsVisibleInstalled = false;
            g_VisArmed           = false;
            g_OrigIsVisible      = nullptr;
        }
        if (g_ClampInstalled)
        {
            DisableAndRemoveHook(ResolveGameAddress(kAddr_SiblingCount));
            g_ClampInstalled   = false;
            g_OrigSiblingCount = nullptr;
        }
        if (g_HookEdcDtor)
        {
            DisableAndRemoveHook(g_HookEdcDtor);
            g_HookEdcDtor        = nullptr;
            g_OrigEdcDtor        = nullptr;
            g_DtorGuardInstalled = false;
        }
        if (g_HookGetBaseId)      DisableAndRemoveHook(g_HookGetBaseId);
        if (g_HookEquipIdRow)     DisableAndRemoveHook(g_HookEquipIdRow);
        if (g_HookEquipIdCount)   DisableAndRemoveHook(g_HookEquipIdCount);
        if (g_HookListDevFill)    DisableAndRemoveHook(g_HookListDevFill);
        if (g_HookListDevCount)   DisableAndRemoveHook(g_HookListDevCount);
        if (g_HookFindByEquipId)  DisableAndRemoveHook(g_HookFindByEquipId);
        if (g_HookFindByDevId)    DisableAndRemoveHook(g_HookFindByDevId);
        if (g_HookSetUndeveloped) DisableAndRemoveHook(g_HookSetUndeveloped);
        if (g_HookSetDeveloped)   DisableAndRemoveHook(g_HookSetDeveloped);
        if (g_HookBlockReset)     DisableAndRemoveHook(g_HookBlockReset);
        g_HookGetBaseId = nullptr;
        g_HookEquipIdRow = g_HookEquipIdCount = nullptr;
        g_HookListDevFill = g_HookListDevCount = nullptr;
        g_HookFindByEquipId = g_HookFindByDevId = nullptr;
        g_HookSetUndeveloped = g_HookSetDeveloped = g_HookBlockReset = nullptr;
    }
}
