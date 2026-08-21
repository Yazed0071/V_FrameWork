#include "pch.h"

#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <atomic>
#include <mutex>
#include <share.h>

static FILE* g_LogFile = nullptr;
static std::mutex g_LogMutex;
static bool g_AtLineStart = true;
static DWORD g_LastFlushTick = 0;
static std::atomic<unsigned long> g_LogIoBeginTick{ 0 };
static std::atomic<unsigned long> g_LogIoTid{ 0 };
static std::atomic<unsigned long> g_LogSerial{ 0 };

static inline void ConsoleWrite(const char* data, int len)
{
    if (GetConsoleWindow())
        fwrite(data, 1, static_cast<size_t>(len), stdout);
}

void InitLog()
{
    char gameDir[MAX_PATH]{};
    GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
    char* lastSlash = strrchr(gameDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    char modDir[MAX_PATH]{};
    strcpy_s(modDir, gameDir);
    strcat_s(modDir, "mod");
    CreateDirectoryA(modDir, nullptr);

    char vfDir[MAX_PATH]{};
    strcpy_s(vfDir, gameDir);
    strcat_s(vfDir, "mod\\V_FrameWork");
    CreateDirectoryA(vfDir, nullptr);

    char logPath[MAX_PATH]{};
    strcpy_s(logPath, gameDir);
    strcat_s(logPath, "mod\\V_FrameWork\\V_FrameWork_log.txt");


    g_LogFile = _fsopen(logPath, "w", _SH_DENYWR);
    if (g_LogFile)
        fprintf(g_LogFile, "[LOG] V_FrameWork log initialized at %s\n", logPath);
}

void Log(const char* fmt, ...)
{
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0)
        return;
    if (len >= static_cast<int>(sizeof(buf)))
        len = static_cast<int>(sizeof(buf)) - 1;

    std::lock_guard<std::mutex> lock(g_LogMutex);

    g_LogIoTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_LogIoBeginTick.store(GetTickCount() | 1u, std::memory_order_release);

    char ts[32];
    SYSTEMTIME st;

    int i = 0;
    while (i < len)
    {
        if (g_AtLineStart && buf[i] != '\n')
        {
            GetLocalTime(&st);
            const int tslen = sprintf_s(
                ts, sizeof(ts),
                "[%02u:%02u:%02u.%03u] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            ConsoleWrite(ts, tslen);
            if (g_LogFile)
                fwrite(ts, 1, tslen, g_LogFile);
            g_AtLineStart = false;
        }

        int j = i;
        while (j < len && buf[j] != '\n')
            ++j;

        if (j > i)
        {
            ConsoleWrite(buf + i, j - i);
            if (g_LogFile)
                fwrite(buf + i, 1, j - i, g_LogFile);
        }

        if (j < len)
        {
            ConsoleWrite("\n", 1);
            if (g_LogFile)
                fputc('\n', g_LogFile);
            g_AtLineStart = true;
            ++j;
        }

        i = j;
    }

    if (g_LogFile)
    {
        const DWORD now = GetTickCount();
        if (now - g_LastFlushTick >= 100u)
        {
            fflush(g_LogFile);
            g_LastFlushTick = now;
        }
    }

    g_LogIoBeginTick.store(0, std::memory_order_release);
    g_LogSerial.fetch_add(1, std::memory_order_relaxed);
}

unsigned long LogLineSerial()
{
    return g_LogSerial.load(std::memory_order_relaxed);
}

unsigned long LogIoStalledMs()
{
    const unsigned long began = g_LogIoBeginTick.load(std::memory_order_acquire);
    if (!began)
        return 0;
    return static_cast<unsigned long>(GetTickCount()) - began;
}

unsigned long LogIoOwnerThreadId()
{
    if (!g_LogIoBeginTick.load(std::memory_order_acquire))
        return 0;
    return g_LogIoTid.load(std::memory_order_relaxed);
}

void CrashLogf(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0)
        return;
    if (len >= static_cast<int>(sizeof(buf)))
        len = static_cast<int>(sizeof(buf)) - 1;

    if (g_LogFile)
    {
        fwrite(buf, 1, static_cast<size_t>(len), g_LogFile);
        fflush(g_LogFile);
    }
    ConsoleWrite(buf, len);
}

void CloseLog()
{
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogFile)
    {
        fprintf(g_LogFile, "[LOG] Closing log.\n");
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

void EnsureConsole()
{
    if (GetConsoleWindow())
        return;

    if (!AllocConsole())
        AttachConsole(ATTACH_PARENT_PROCESS);

    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    SetConsoleOutputCP(CP_UTF8);

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD inMode = 0;
    if (hIn != INVALID_HANDLE_VALUE && GetConsoleMode(hIn, &inMode))
    {
        inMode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT);
        inMode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, inMode);
    }

    SetConsoleTitleW(L"V_FrameWork");
}
