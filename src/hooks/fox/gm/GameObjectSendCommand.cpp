#include "pch.h"
#include "GameObjectSendCommand.h"

extern "C" {
    #include "lua.h"
}

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "log.h"
#include "../../../core/AddressSet.h"
#include "../../../core/HookUtils.h"
#include "../../../core/MissionCodeGuard.h"
#include "../../../core/MissionStateReset.h"
#include "../../sahelan/PhaseSneakAiImpl_PreUpdate.h"
#include "../../ui/HeadMarkMarkerEvCall_SetIconSubType.h"
#include "../../sahelan/RealizedSahelanFovaHook.h"
#include "../../sahelan/SetEyeLampColorHook.h"
#include "../../soldier/LostHostageHook.h"
#include "../../soldier/StepRadioDiscovery.h"
#include "../../soldier/HostageGender.h"
#include "../../soldier/VIPSleepFaintHook.h"
#include "../../soldier/VIPHoldupHook.h"
#include "../../soldier/VIPRadioHook.h"
#include "../../soldier/GetVoiceParamWithCallSign.h"
#include "../../soldier/ActionCoreImpl_UpdateOptCamo.h"
#include "../../soldier/NoticeControllerImpl_GetOccasionalChat.h"
#include "../../soldier/CautionStepNormalTimerHook.h"
#include "../../soldier/SoldierObjectRtpc.h"
#include "../../soldier/NoticeControllerImpl_CheckSightNoticePlayer.h"
#include "GetGameObjectIdWithIndex.h"
#include "../../soldier/InterrogationVoiceEvent.h"
#include "../../soldier/SoldierAkObjIdMap.h"
#include "../../bullet/Bullet3Impl_ActivateBulletAtEmptyWorkPatch.h"
#include "../../../core/FoxHashes.h"
#include "../../../lua/LuaApi.h"

namespace
{

    using GameObjectSendCommand_t = int (__fastcall*)(lua_State* L);

    static GameObjectSendCommand_t g_OrigSendCommand = nullptr;
    static bool                    g_Installed       = false;

    static const char* ReadCommandId(lua_State* L, int cmdStackIdx, std::string* out)
    {
        out->clear();
        g_lua_pushstring(L, const_cast<char*>("id"));
        g_lua_gettable(L, cmdStackIdx);
        if (g_lua_type(L, -1) != LUA_TSTRING) return nullptr;
        const char* s = g_lua_tolstring(L, -1, nullptr);
        if (!s) return nullptr;
        *out = s;
        return out->c_str();
    }

    static double ReadCommandNumber(lua_State* L, int cmdStackIdx, const char* key)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        double v = 0.0;
        if (g_lua_type(L, -1) == LUA_TNUMBER)
            v = static_cast<double>(g_lua_tonumber(L, -1));
        return v;
    }

    static double ReadCommandNumberOr(lua_State* L, int cmdStackIdx, const char* key, double def)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        double v = def;
        if (g_lua_type(L, -1) == LUA_TNUMBER)
            v = static_cast<double>(g_lua_tonumber(L, -1));
        return v;
    }

    static bool ReadCommandBool(lua_State* L, int cmdStackIdx, const char* key)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        bool v = false;
        const int t = g_lua_type(L, -1);
        if (t == LUA_TBOOLEAN)
            v = g_lua_toboolean(L, -1) != 0;
        else if (t == LUA_TNUMBER)
            v = static_cast<int>(g_lua_tonumber(L, -1)) != 0;
        return v;
    }

    static std::uint32_t ReadCommandStrCode32(lua_State* L, int cmdStackIdx, const char* key)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        std::uint32_t v = 0;
        const int t = g_lua_type(L, -1);
        if (t == LUA_TNUMBER)
            v = static_cast<std::uint32_t>(static_cast<long long>(g_lua_tonumber(L, -1)));
        else if (t == LUA_TSTRING)
        {
            const char* s = g_lua_tolstring(L, -1, nullptr);
            if (s && s[0])
                v = FoxHashes::StrCode32(s);
        }
        return v;
    }

    static std::uint32_t FnvHash32Of(const char* s)
    {
        using FNV_t = unsigned int(__fastcall*)(const char*);
        static FNV_t fn = nullptr;
        if (!fn && gAddr.FNVHash32)
            fn = reinterpret_cast<FNV_t>(ResolveGameAddress(gAddr.FNVHash32));
        if (!fn || !s || !s[0])
            return 0;
        __try { return static_cast<std::uint32_t>(fn(s)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static std::uint32_t ReadCommandFnvHash(lua_State* L, int cmdStackIdx, const char* key,
                                            bool* present)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);
        std::uint32_t v = 0;
        bool have = false;
        const int t = g_lua_type(L, -1);
        if (t == LUA_TNUMBER)
        {
            v = static_cast<std::uint32_t>(static_cast<long long>(g_lua_tonumber(L, -1)));
            have = true;
        }
        else if (t == LUA_TSTRING)
        {
            const char* s = g_lua_tolstring(L, -1, nullptr);
            if (s && s[0])
                v = FnvHash32Of(s);
            have = true;
        }
        if (present)
            *present = have;
        return v;
    }

    static std::uint32_t ReadCommandTargetId(lua_State* L)
    {
        if (g_lua_type(L, 1) == LUA_TNUMBER)
            return static_cast<std::uint32_t>(g_lua_tointeger(L, 1) & 0xFFFFFFFFLL);
        return 0;
    }

    static std::size_t ReadLabelArray(lua_State* L, int cmdStackIdx, std::uint32_t* out, const char* key)
    {
        std::size_t n = 0;

        if (g_lua_objlen && g_lua_rawgeti)
        {
            g_lua_pushstring(L, const_cast<char*>(key));
            g_lua_gettable(L, cmdStackIdx);
            if (g_lua_type(L, -1) == LUA_TTABLE)
            {
                const int tbl = g_lua_gettop(L);
                const std::size_t len = g_lua_objlen(L, tbl);
                for (std::size_t i = 1; i <= len && n < 255; ++i)
                {
                    g_lua_rawgeti(L, tbl, static_cast<int>(i));
                    const int et = g_lua_type(L, -1);
                    if (et == LUA_TNUMBER)
                        out[n++] = static_cast<std::uint32_t>(static_cast<long long>(g_lua_tonumber(L, -1)));
                    else if (et == LUA_TSTRING)
                    {
                        const char* s = g_lua_tolstring(L, -1, nullptr);
                        if (s) out[n++] = FoxHashes::StrCode32(s);
                    }
                    g_lua_settop(L, tbl);
                }
            }
        }

        return n;
    }

    static std::uint32_t GatherLabelBases(lua_State* L, int tblIdx, std::uint32_t* out,
                                          std::uint32_t cap, std::uint32_t n, int depth)
    {
        if (!g_lua_pushnil || !g_lua_next || depth > 1)
            return n;
        g_lua_pushnil(L);
        while (g_lua_next(L, tblIdx) != 0)
        {
            const char* keyStr = (g_lua_type(L, -2) == LUA_TSTRING)
                                     ? g_lua_tolstring(L, -2, nullptr)
                                     : nullptr;
            const int vt = g_lua_type(L, -1);
            if (vt == LUA_TSTRING)
            {
                const char* s = g_lua_tolstring(L, -1, nullptr);
                const std::uint32_t sc = (s && s[0]) ? FoxHashes::StrCode32(s) : 0;
                const std::uint32_t fv = FnvHash32Of(s);
                if (sc && n < cap)
                    out[n++] = sc;
                if (fv && fv != sc && n < cap)
                    out[n++] = fv;
                LogDebug("[InterrogationVoice] cmd[%d] field '%s' = \"%s\" strcode32=0x%X "
                         "fnv=0x%X\n",
                         depth, keyStr ? keyStr : "#", s ? s : "", sc, fv);
            }
            else if (vt == LUA_TNUMBER)
            {
                const std::uint32_t num =
                    static_cast<std::uint32_t>(static_cast<long long>(g_lua_tonumber(L, -1)));
                if (num >= 0x10000u && n < cap)
                    out[n++] = num;
                LogDebug("[InterrogationVoice] cmd[%d] field '%s' = 0x%X (%u)\n",
                         depth, keyStr ? keyStr : "#", num, num);
            }
            else if (vt == LUA_TTABLE)
            {
                LogDebug("[InterrogationVoice] cmd[%d] field '%s' = table\n",
                         depth, keyStr ? keyStr : "#");
                n = GatherLabelBases(L, g_lua_gettop(L), out, cap, n, depth + 1);
            }
            else
            {
                LogDebug("[InterrogationVoice] cmd[%d] field '%s' type=%d\n",
                         depth, keyStr ? keyStr : "#", vt);
            }
            g_lua_settop(L, g_lua_gettop(L) - 1);
        }
        return n;
    }

    static float SmartScaleA(float a)
    {
        return (a > 1.0f) ? (a * (1.0f / 255.0f)) : a;
    }

    static void SmartScaleRgb(float* r, float* g, float* b)
    {
        if (*r > 1.0f || *g > 1.0f || *b > 1.0f)
        {
            *r *= (1.0f / 255.0f);
            *g *= (1.0f / 255.0f);
            *b *= (1.0f / 255.0f);
        }
    }

    static void ReadColor(lua_State* L, int cmdStackIdx, float* r, float* g, float* b, float* a, float defaultA)
    {
        *r = 0.0f; *g = 0.0f; *b = 0.0f; *a = defaultA;
        g_lua_pushstring(L, const_cast<char*>("color"));
        g_lua_gettable(L, cmdStackIdx);
        if (g_lua_type(L, -1) == LUA_TTABLE)
        {
            const int t = g_lua_gettop(L);
            g_lua_pushstring(L, const_cast<char*>("r")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *r = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
            g_lua_pushstring(L, const_cast<char*>("g")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *g = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
            g_lua_pushstring(L, const_cast<char*>("b")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *b = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
            g_lua_pushstring(L, const_cast<char*>("a")); g_lua_gettable(L, t); if (g_lua_type(L, -1) == LUA_TNUMBER) *a = static_cast<float>(g_lua_tonumber(L, -1)); g_lua_settop(L, t);
        }
        SmartScaleRgb(r, g, b);
        *a = SmartScaleA(*a);
    }

    static int ReadHeadMarkState(lua_State* L, int cmdStackIdx)
    {
        g_lua_pushstring(L, const_cast<char*>("state"));
        g_lua_gettable(L, cmdStackIdx);

        const int t = g_lua_type(L, -1);
        if (t == LUA_TNIL)
            return -1;

        if (t == LUA_TNUMBER)
        {
            const int n = static_cast<int>(g_lua_tonumber(L, -1));
            return (n >= 0 && n <= 4) ? n : -2;
        }

        if (t != LUA_TSTRING)
            return -2;

        const char* v = g_lua_tolstring(L, -1, nullptr);
        if (!v || !v[0])                        return -2;
        if (!_stricmp(v, "neutral"))            return 0;
        if (!_stricmp(v, "enemy"))              return 1;
        if (!_stricmp(v, "friendly"))           return 2;
        if (!_stricmp(v, "friend"))             return 2;
        if (!_stricmp(v, "powerless"))          return 3;
        if (!_stricmp(v, "dying"))              return 4;
        return -2;
    }

    static bool ReadHeadMarkStop(lua_State* L, int cmdStackIdx, const char* key,
                                 HeadMarkColourStop& out)
    {
        g_lua_pushstring(L, const_cast<char*>(key));
        g_lua_gettable(L, cmdStackIdx);

        const int t = g_lua_type(L, -1);

        if (t == LUA_TNUMBER)
        {
            out.isRgb     = false;
            out.paletteId = static_cast<std::uint32_t>(
                static_cast<long long>(g_lua_tonumber(L, -1)));
            return true;
        }

        if (t == LUA_TSTRING)
        {
            const char* v = g_lua_tolstring(L, -1, nullptr);
            if (!v || !v[0])
                return false;
            out.isRgb     = false;
            out.paletteId = ::ResolveHeadMarkColourId(v);
            return out.paletteId != 0u;
        }

        if (t == LUA_TTABLE)
        {
            const int tbl = g_lua_gettop(L);
            float r = 0.0f, g = 0.0f, b = 0.0f;
            g_lua_pushstring(L, const_cast<char*>("r")); g_lua_gettable(L, tbl);
            if (g_lua_type(L, -1) == LUA_TNUMBER) r = static_cast<float>(g_lua_tonumber(L, -1));
            g_lua_settop(L, tbl);
            g_lua_pushstring(L, const_cast<char*>("g")); g_lua_gettable(L, tbl);
            if (g_lua_type(L, -1) == LUA_TNUMBER) g = static_cast<float>(g_lua_tonumber(L, -1));
            g_lua_settop(L, tbl);
            g_lua_pushstring(L, const_cast<char*>("b")); g_lua_gettable(L, tbl);
            if (g_lua_type(L, -1) == LUA_TNUMBER) b = static_cast<float>(g_lua_tonumber(L, -1));
            g_lua_settop(L, tbl);

            SmartScaleRgb(&r, &g, &b);
            out.isRgb = true;
            out.r = r;
            out.g = g;
            out.b = b;
            return true;
        }

        return false;
    }

    static unsigned ReadHeadMarkStops(lua_State* L, int cmdStackIdx,
                                      HeadMarkColourStop* out, unsigned max)
    {
        static const char* const kKeys[6] =
            { "color", "color2", "color3", "color4", "color5", "color6" };
        static const char* const kAlts[6] =
            { "colour", "colour2", "colour3", "colour4", "colour5", "colour6" };

        unsigned n = 0;
        for (unsigned i = 0; i < 6 && n < max; ++i)
        {
            HeadMarkColourStop stop{};
            if (ReadHeadMarkStop(L, cmdStackIdx, kKeys[i], stop) ||
                ReadHeadMarkStop(L, cmdStackIdx, kAlts[i], stop))
            {
                out[n++] = stop;
            }
        }
        return n;
    }

    static bool ReadHeadMarkBlend(lua_State* L, int cmdStackIdx)
    {
        g_lua_pushstring(L, const_cast<char*>("blend"));
        g_lua_gettable(L, cmdStackIdx);
        const int t = g_lua_type(L, -1);
        if (t == LUA_TBOOLEAN)
            return g_lua_toboolean(L, -1) != 0;
        if (t == LUA_TNUMBER)
            return static_cast<int>(g_lua_tonumber(L, -1)) != 0;
        return true;
    }

    static bool CautionPerCpTarget(lua_State* L)
    {
        const int top = g_lua_gettop(L);
        bool isPerCp = false;
        const int a1 = g_lua_type(L, 1);
        if (a1 == LUA_TNUMBER)
        {
            isPerCp = true;
        }
        else if (a1 == LUA_TTABLE)
        {
            g_lua_pushstring(L, const_cast<char*>("index"));
            g_lua_gettable(L, 1);
            isPerCp = (g_lua_type(L, -1) != LUA_TNIL);
        }
        g_lua_settop(L, top);
        return isPerCp;
    }

    static std::atomic<DWORD> g_GameTid{ 0 };
    static std::atomic<DWORD> g_LastTickMs{ 0 };
    static std::atomic<bool>  g_StallWatchStarted{ false };

    static void DescribeAddr(std::uint64_t addr, char* out, std::size_t outLen)
    {
        out[0] = 0;
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &mod) || !mod)
        {
            sprintf_s(out, outLen, "<unmapped>");
            return;
        }
        char path[MAX_PATH]{};
        if (!GetModuleFileNameA(mod, path, MAX_PATH))
        {
            sprintf_s(out, outLen, "<module?>");
            return;
        }
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        sprintf_s(out, outLen, "%s+0x%llX", base,
            static_cast<unsigned long long>(addr - reinterpret_cast<std::uint64_t>(mod)));
    }

    static bool SampleThread(DWORD tid, DWORD64* rip, DWORD64* rsp)
    {
        HANDLE h = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, tid);
        if (!h) return false;
        bool ok = false;
        SuspendThread(h);
        CONTEXT ctx;
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &ctx))
        {
            *rip = ctx.Rip;
            *rsp = ctx.Rsp;
            ok = true;
        }
        ResumeThread(h);
        CloseHandle(h);
        return ok;
    }

    static void DumpFrozenThread(DWORD tid, const char* kind, DWORD64 rip, DWORD64 rsp,
                                 unsigned heldMs, int shot)
    {
        std::uint64_t stackbuf[192] = {};
        std::size_t   stackn = 0;

        HANDLE h = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, tid);
        if (h)
        {
            SuspendThread(h);
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
            ResumeThread(h);
            CloseHandle(h);
        }

        const unsigned long logStall = LogIoStalledMs();
        const unsigned long logOwner = LogIoOwnerThreadId();

        char ripDesc[MAX_PATH + 32];
        DescribeAddr(rip, ripDesc, sizeof(ripDesc));

        Log("[StallWatchdog] %s (report %d): the game thread has been parked at one "
            "instruction for %ums. rip=0x%llX = %s, rsp=0x%llX. ntdll/KERNELBASE "
            "there means a blocking wait; the frames below name what it was doing\n",
            kind, shot, heldMs,
            static_cast<unsigned long long>(rip), ripDesc,
            static_cast<unsigned long long>(rsp));

        if (logStall)
            Log("[StallWatchdog]   the framework logger was already inside a "
                "console/disk write for %lums on thread %lu (game thread %lu) - if "
                "those match, this DLL's logging is the freeze\n",
                logStall, logOwner, static_cast<unsigned long>(tid));
        else
            Log("[StallWatchdog]   the framework logger was not mid-write when "
                "sampled, so the stall is elsewhere\n");

        int shown = 0;
        for (std::size_t i = 0; i < stackn && shown < 20; ++i)
        {
            const std::uint64_t v = stackbuf[i];
            if (v < 0x10000ull) continue;
            char desc[MAX_PATH + 32];
            DescribeAddr(v, desc, sizeof(desc));
            if (desc[0] == '<') continue;
            Log("[StallWatchdog]   stack[rsp+0x%03zX] = 0x%llX = %s\n",
                i * 8, static_cast<unsigned long long>(v), desc);
            ++shown;
        }
        if (!shown)
            Log("[StallWatchdog]   no game-code addresses in the first 1.5KB of "
                "stack - parked deep inside a system DLL (a wait or I/O)\n");
    }

    static void ReportQuietWindow(DWORD tid, DWORD64 rip, DWORD64 rsp,
                                  unsigned quietMs, int shot, DWORD64 prevRsp)
    {
        char ripName[192];
        ripName[0] = 0;
        DescribeAddr(static_cast<std::uint64_t>(rip), ripName, sizeof(ripName));

        unsigned mission = 0xFFFFu;
        __try
        {
            mission = static_cast<unsigned>(MissionCodeGuard::GetCurrentMissionCode());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            mission = 0xFFFFu;
        }

        Log("[LoadStall] #%d quiet=%u.%us mission=%u thread=%s rip=0x%llX = %s rsp=0x%llX\n",
            shot, quietMs / 1000u, (quietMs % 1000u) / 100u, mission,
            (prevRsp && rsp == prevRsp) ? "PARKED" : "RUNNING",
            static_cast<unsigned long long>(rip), ripName,
            static_cast<unsigned long long>(rsp));

        if (shot == 3 || shot == 8 || shot == 15)
            DumpFrozenThread(tid, "LOAD-STALL", rip, rsp, quietMs, shot);
    }

    static DWORD WINAPI StallWatchProc(LPVOID)
    {
        const DWORD kPeriodMs = 250;
        const int   kPinnedToFire = 80;

        DWORD64 lastRip = 0, lastRsp = 0;
        int samePair = 0;
        int sameRsp = 0;
        int shots = 0;
        bool haveLast = false;

        unsigned long lastSerial = LogLineSerial();
        DWORD quietSinceTick = GetTickCount();
        DWORD lastQuietReportTick = 0;
        int quietReports = 0;
        DWORD64 prevReportRsp = 0;

        for (;;)
        {
            Sleep(kPeriodMs);
            const DWORD tid = g_GameTid.load(std::memory_order_relaxed);
            if (!tid) continue;

            DWORD64 rip = 0, rsp = 0;
            if (!SampleThread(tid, &rip, &rsp)) continue;

            if (haveLast && rip == lastRip && rsp == lastRsp) ++samePair; else samePair = 0;
            if (haveLast && rsp == lastRsp)                   ++sameRsp;  else sameRsp  = 0;
            lastRip = rip;
            lastRsp = rsp;
            haveLast = true;

            const DWORD nowTick = GetTickCount();
            const unsigned long serial = LogLineSerial();
            if (serial != lastSerial)
            {
                lastSerial = serial;
                quietSinceTick = nowTick;
                lastQuietReportTick = 0;
                quietReports = 0;
                prevReportRsp = 0;
            }
            else
            {
                const DWORD quietMs = nowTick - quietSinceTick;
                if (quietMs >= 6000u && quietReports < 60 &&
                    (lastQuietReportTick == 0 || nowTick - lastQuietReportTick >= 2000u))
                {
                    lastQuietReportTick = nowTick;
                    ++quietReports;
                    ReportQuietWindow(tid, rip, rsp, quietMs, quietReports, prevReportRsp);
                    prevReportRsp = rsp;
                    lastSerial = LogLineSerial();
                }
            }

            if (samePair == 0 && sameRsp == 0)
            {
                shots = 0;
                continue;
            }
            if (shots >= 6) continue;

            if (samePair >= kPinnedToFire)
            {
                ++shots;
                DumpFrozenThread(tid, "BLOCKED", rip, rsp,
                                 static_cast<unsigned>(samePair) * kPeriodMs, shots);
                samePair = 0;
                sameRsp = 0;
            }
            else if (sameRsp >= kPinnedToFire)
            {
                ++shots;
                DumpFrozenThread(tid, "SPINNING", rip, rsp,
                                 static_cast<unsigned>(sameRsp) * kPeriodMs, shots);
                samePair = 0;
                sameRsp = 0;
            }
        }
    }

    static void NoteGameThreadTick()
    {
        g_GameTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_LastTickMs.store(GetTickCount(), std::memory_order_relaxed);
        (void)&StallWatchProc;
    }

    static int __fastcall hk_SendCommand(lua_State* L)
    {
        if (!g_OrigSendCommand) return 0;
        NoteGameThreadTick();

        MISSION_GUARD_ORIGINAL_RET(g_OrigSendCommand, L);

        MissionStateReset::PollMissionChange();

        if (!ResolveLuaApi()) return g_OrigSendCommand(L);

        MissionStateReset::EnsureFinalizerRegistered(L);

        const int top = g_lua_gettop(L);
        if (top < 2 || g_lua_type(L, 2) != LUA_TTABLE)
            return g_OrigSendCommand(L);

        std::string idStr;
        const char* id = ReadCommandId(L, 2, &idStr);
        if (!id || !*id)
        {
            g_lua_settop(L, top);
            return g_OrigSendCommand(L);
        }
        g_lua_settop(L, top);

        if (idStr == "SetSahelanPhase")
        {
            const std::int32_t phase =
                static_cast<std::int32_t>(ReadCommandNumber(L, 2, "phase"));
            g_lua_settop(L, top);
            ::Set_SahelanForcePhase(phase);
            return 0;
        }
        if (idStr == "GetSahelanPhase")
        {
            const double phase = static_cast<double>(::Get_SahelanCurrentPhase());
            g_lua_pushnumber(L, phase);
            return 1;
        }
        if (idStr == "SetOccasionalChatList")
        {
            std::uint32_t labels[256];
            const std::size_t n = ReadLabelArray(L, 2, labels, "labels");
            g_lua_settop(L, top);
            ::SetOccasionalChatList(labels, n);
            return 0;
        }
        if (idStr == "InsertToOccasionalChatList")
        {
            std::uint32_t labels[256];
            const std::size_t n = ReadLabelArray(L, 2, labels, "labels");
            g_lua_settop(L, top);
            ::InsertToOccasionalChatList(labels, n);
            return 0;
        }
        if (idStr == "RemoveFromOccasionalChatList")
        {
            std::uint32_t labels[256];
            const std::size_t n = ReadLabelArray(L, 2, labels, "labels");
            g_lua_settop(L, top);
            ::RemoveFromOccasionalChatList(labels, n);
            return 0;
        }

        if (idStr == "SetCautionPhaseDuration")
        {
            const double duration = ReadCommandNumber(L, 2, "duration");

            bool isPerCp = false;
            const int a1 = g_lua_type(L, 1);
            if (a1 == LUA_TNUMBER)
            {
                isPerCp = true;
            }
            else if (a1 == LUA_TTABLE)
            {
                g_lua_pushstring(L, const_cast<char*>("index"));
                g_lua_gettable(L, 1);
                isPerCp = (g_lua_type(L, -1) != LUA_TNIL);
            }
            g_lua_settop(L, top);

            if (isPerCp)
            {
                ::Set_PendingCautionDurationForCp(static_cast<float>(duration));
                return g_OrigSendCommand(L);
            }

            ::Set_CautionStepNormalDurationSeconds(static_cast<float>(duration));
            return 0;
        }
        if (idStr == "GetCautionPhaseDuration")
        {
            if (CautionPerCpTarget(L))
            {
                ::Arm_CautionCpCapture();
                g_OrigSendCommand(L);
                const std::uint32_t cp = ::Take_CautionCpIndex();
                g_lua_settop(L, top);
                g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalDurationSecondsForCp(cp)));
                return 1;
            }
            g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalDurationSeconds()));
            return 1;
        }
        if (idStr == "UnsetCautionPhaseDuration")
        {
            if (CautionPerCpTarget(L))
            {
                ::Arm_CautionCpCapture();
                g_OrigSendCommand(L);
                const std::uint32_t cp = ::Take_CautionCpIndex();
                ::Unset_CautionStepNormalDurationSecondsForCp(cp);
                g_lua_settop(L, top);
                return 0;
            }
            g_lua_settop(L, top);
            ::Unset_CautionStepNormalDurationSeconds();
            return 0;
        }
        if (idStr == "GetCautionPhaseRemaining")
        {
            if (CautionPerCpTarget(L))
            {
                ::Arm_CautionCpCapture();
                g_OrigSendCommand(L);
                const std::uint32_t cp = ::Take_CautionCpIndex();
                g_lua_settop(L, top);
                g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalRemainingSecondsForCp(cp)));
                return 1;
            }
            g_lua_pushnumber(L, static_cast<double>(::Get_CautionStepNormalRemainingSeconds()));
            return 1;
        }

        if (idStr == "SetRestrictNotice")
        {
            const std::uint32_t id = ReadCommandTargetId(L);

            std::uint8_t mask = 0;
            if (ReadCommandBool(L, 2, "enabled"))
            {
                if (ReadCommandBool(L, 2, "ignorePlayer"))
                    mask |= SoldierNoticeIgnore::kPlayer;
            }
            g_lua_settop(L, top);

            const int r = g_OrigSendCommand(L);

            if ((id >> 9) == TppGameObjectType::kSoldier2)
                ::Set_SoldierNoticeIgnoreMask(id, mask);
            return r;
        }
        if (idStr == "SetHeadMarkColor")
        {
            const std::uint32_t id    = ReadCommandTargetId(L);
            const int           state = ReadHeadMarkState(L, 2);

            HeadMarkColourStop stops[6]{};
            const unsigned     count = ReadHeadMarkStops(L, 2, stops, 6);
            const float        speed = static_cast<float>(ReadCommandNumberOr(L, 2, "speed", 1.0));
            const bool         blend = ReadHeadMarkBlend(L, 2);
            const float        fade  = static_cast<float>(ReadCommandNumberOr(L, 2, "fade", 0.0));
            g_lua_settop(L, top);

            if (state == -2)
            {
                Log("[HeadMarkDying] SetHeadMarkColor was given an unusable state for game object "
                    "0x%X - pass neutral/alive/friendly/powerless/dying, so that marker keeps its "
                    "vanilla colour\n", id);
                return 0;
            }

            ::SetHeadMarkEntityColour(id, state, stops, count, speed, blend, fade);
            return 0;
        }
        if (idStr == "SetVoicePitch")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const float cents = static_cast<float>(ReadCommandNumber(L, 2, "pitch"));
            g_lua_settop(L, top);

            const std::uint32_t targetType = id >> 9;

            if (targetType == TppGameObjectType::kCommandPost)
            {
                SoldierAkObjIdMap::SetCommandPostVoiceCents(id & 0x1FFu, cents);
                return 0;
            }

            if (targetType != TppGameObjectType::kSoldier2)
            {
                Log("[SoldierVoicePitch] ERROR: SetVoicePitch was aimed at game object "
                    "0x%X, whose type is %u - only a soldier (%u) or a command post (%u) "
                    "owns a voice this can bias, so the pitch %.2f was applied to "
                    "nothing. A type of 0 means the target name resolved to no game "
                    "object at all\n",
                    id, targetType,
                    static_cast<std::uint32_t>(TppGameObjectType::kSoldier2),
                    static_cast<std::uint32_t>(TppGameObjectType::kCommandPost),
                    cents);
                return 0;
            }

            ::Set_SoldierVoicePitch(id, cents);
            return 0;
        }
        if (idStr == "SetVIPImportant")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const bool isOfficer = ReadCommandBool(L, 2, "isOfficer");
            const std::uint32_t deadBodyLabel = ReadCommandStrCode32(L, 2, "deadBodyLabel");
            g_lua_settop(L, top);
            ::Add_VIPSleepFaintImportantGameObjectId(id, isOfficer);
            ::Add_VIPHoldupImportantGameObjectId(id, isOfficer);
            ::Add_VIPRadioImportantGameObjectId(id, isOfficer, deadBodyLabel);
            return 0;
        }
        if (idStr == "RemoveVIPImportant")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            g_lua_settop(L, top);
            ::Remove_VIPSleepFaintImportantGameObjectId(id);
            ::Remove_VIPHoldupImportantGameObjectId(id);
            ::Remove_VIPRadioImportantGameObjectId(id);
            return 0;
        }
        if (idStr == "SetRadioCallSign")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const int callSign = static_cast<int>(ReadCommandNumber(L, 2, "callSign"));
            g_lua_settop(L, top);
            if (callSign <= 0)
                ::Remove_SoldierCallSign(id);
            else
                ::Set_SoldierCallSign(id, static_cast<std::uint8_t>(callSign > 255 ? 255 : callSign));
            return 0;
        }
        if (idStr == "ClearLostHostages")
        {
            g_lua_settop(L, top);
            ::Clear_LostHostagesTrap();
            ::Clear_LostHostageDiscovery();
            return 0;
        }
        if (idStr == "SetLostHostage")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const int hostageType = static_cast<int>(ReadCommandNumber(L, 2, "hostageType"));
            const std::uint32_t customLostLabel = ReadCommandStrCode32(L, 2, "customLostLabel");
            const std::uint32_t customLostLabelTaken =
                ReadCommandStrCode32(L, 2, "customLostLabelTaken");
            g_lua_settop(L, top);
            ::Add_LostHostageTrap(id, hostageType, customLostLabel, customLostLabelTaken);
            ::Add_LostHostageDiscovery(id, hostageType);
            return 0;
        }
        if (idStr == "RemoveLostHostage")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            g_lua_settop(L, top);
            ::Remove_LostHostageTrap(id);
            ::Remove_LostHostageDiscovery(id);
            return 0;
        }
        if (idStr == "SetOpticalCamo")
        {
            const std::uint32_t mappedIndex = ReadCommandTargetId(L);
            const bool enable = ReadCommandBool(L, 2, "enable");
            g_lua_settop(L, top);
            ::Set_UpdateOptCamoEnableMappedIndex(mappedIndex, enable);
            return 0;
        }
        if (idStr == "SetSahelanFova")
        {
            std::string fv2;
            g_lua_pushstring(L, const_cast<char*>("fv2"));
            g_lua_gettable(L, 2);
            if (g_lua_type(L, -1) == LUA_TSTRING)
            {
                const char* s = g_lua_tolstring(L, -1, nullptr);
                if (s) fv2 = s;
            }
            g_lua_settop(L, top);
            ::Set_SahelanFovaPath(fv2.c_str());
            return 0;
        }
        if (idStr == "SetEyeLampColor")
        {
            float r, g, b, a;
            ReadColor(L, 2, &r, &g, &b, &a, 1.0f);
            const int   mode = static_cast<int>(ReadCommandNumberOr(L, 2, "phase", -1.0));
            g_lua_settop(L, top);
            ::Set_EyeLampColor(mode, r, g, b, a);
            return 0;
        }
        if (idStr == "SetEyeLampDisco")
        {
            const bool  enabled = ReadCommandBool(L, 2, "enabled");
            const float speed   = static_cast<float>(ReadCommandNumber(L, 2, "speed"));
            const float a       = SmartScaleA(static_cast<float>(ReadCommandNumberOr(L, 2, "a", 1.0)));
            g_lua_settop(L, top);
            ::Set_EyeLampDisco(enabled, speed, a);
            return 0;
        }
        if (idStr == "SetHeartLightColor")
        {
            float r, g, b, a;
            ReadColor(L, 2, &r, &g, &b, &a, 1.0f);
            const int mode = static_cast<int>(ReadCommandNumberOr(L, 2, "phase", -1.0));
            g_lua_settop(L, top);
            ::Set_HeartLightColor(mode, r, g, b, a);
            return 0;
        }
        if (idStr == "SetHeartLightDisco")
        {
            const bool  enabled = ReadCommandBool(L, 2, "enabled");
            const float speed   = static_cast<float>(ReadCommandNumber(L, 2, "speed"));
            const float a       = SmartScaleA(static_cast<float>(ReadCommandNumberOr(L, 2, "a", 1.0)));
            g_lua_settop(L, top);
            ::Set_HeartLightDisco(enabled, speed, a);
            return 0;
        }

        if (idStr == "SetFriendlyFire")
        {
            const bool enable = ReadCommandBool(L, 2, "enable");
            g_lua_settop(L, top);
            ::Set_FriendlyFire(enable);
            return 0;
        }
        if (idStr == "GetHostageGender")
        {
            const std::uint32_t id = ReadCommandTargetId(L);
            const int gender = HostageGender::Read(id);
            g_lua_settop(L, top);
            if (gender == HostageGender::kUnknown)
                g_lua_pushnil(L);
            else
                g_lua_pushnumber(L, static_cast<double>(gender));
            return 1;
        }
        if (idStr == "IsFriendlyFire")
        {
            g_lua_settop(L, top);
            g_lua_pushnumber(L, ::Get_FriendlyFire() ? 1.0 : 0.0);
            return 1;
        }

        if (idStr == "AssignInterrogationWithVoice")
        {
            bool hasEvent = false;
            bool hasMarkerEvent = false;
            const std::uint32_t ev =
                ReadCommandFnvHash(L, 2, "soundDialogueEvent", &hasEvent);
            const std::uint32_t evMarker =
                ReadCommandFnvHash(L, 2, "soundDialogueEventMarker", &hasMarkerEvent);
            std::uint32_t labelBases[16] = {};
            const std::uint32_t labelBaseCount =
                GatherLabelBases(L, 2, labelBases, 16, 0, 0);
            g_lua_settop(L, top);

            ::Arm_CautionCpCapture();
            const int r = g_OrigSendCommand(L);
            const std::uint32_t cp = ::Take_CautionCpIndex();

            if (hasEvent || hasMarkerEvent)
            {
                if (cp == 0xFFFFFFFFu)
                    Log("[InterrogationVoice] ERROR: AssignInterrogationWithVoice "
                        "never reached the CP dispatcher, so its CP index was not "
                        "captured - soundDialogueEvent 0x%X / marker 0x%X not "
                        "registered; this CP keeps the vanilla voice\n",
                        ev, evMarker);
                else
                {
                    ::Register_InterrogationVoiceEvent(cp, ev, evMarker, labelBases,
                                                       labelBaseCount);
                    LogDebug("[InterrogationVoice] registered cp=%u main=0x%X marker=0x%X "
                             "labelBases=%u\n",
                             cp, ev, evMarker, labelBaseCount);
                }
            }
            return r;
        }

        return g_OrigSendCommand(L);
    }
}

bool Install_GameObjectSendCommand_Hook()
{
    if (g_Installed) return true;

    if (!gAddr.GameObject_SendCommand)
    {
        LogDebug("[GameObjectSendCommand] address is 0 (unsupported build)\n");
        return false;
    }

    void* target = ResolveGameAddress(gAddr.GameObject_SendCommand);
    if (!target)
    {
        Log("[GameObjectSendCommand] resolve failed\n");
        return false;
    }

    if (!ResolveLuaApi())
    {
        Log("[GameObjectSendCommand] lua API resolve failed; aborting install\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hk_SendCommand),
        reinterpret_cast<void**>(&g_OrigSendCommand));

    if (ok)
    {
        g_Installed = true;
#ifdef _DEBUG
        LogDebug("[GameObjectSendCommand] hook installed @ %p (orig=%p)\n",
            target, reinterpret_cast<void*>(g_OrigSendCommand));
#endif
    }
    else
    {
        Log("[GameObjectSendCommand] hook install FAILED @ %p\n", target);
    }
    return ok;
}

bool Uninstall_GameObjectSendCommand_Hook()
{
    if (!g_Installed) return true;
    void* target = ResolveGameAddress(gAddr.GameObject_SendCommand);
    if (target) DisableAndRemoveHook(target);
    g_OrigSendCommand = nullptr;
    g_Installed       = false;
    return true;
}
