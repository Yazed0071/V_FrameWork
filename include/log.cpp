#include "pch.h"

#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static FILE* g_LogFile = nullptr;

void InitLog()
{
    #if _DEBUG

    #endif // _DEBUG

    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    SetConsoleTitleA("MGSV Arabic Hook Console");

    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    strcat_s(path, "MGSV_ArabicHook.log");

    fopen_s(&g_LogFile, path, "w");
    if (g_LogFile)
        fprintf(g_LogFile, "[LOG] Log file created successfully.\n");
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    vprintf(fmt, args);

    if (g_LogFile)
    {
        va_list args2;
        va_start(args2, fmt);
        vfprintf(g_LogFile, fmt, args2);
        va_end(args2);
        fflush(g_LogFile);
    }

    va_end(args);
}

void CloseLog()
{
    if (g_LogFile)
    {
        fprintf(g_LogFile, "[LOG] Closing log.\n");
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
    FreeConsole();
}
