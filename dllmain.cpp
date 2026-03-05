// DllMain.cpp
#include "pch.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include "MinHook.h"

// Hooks implemented in other .cpp files
bool Install_HoldupCancelLookToPlayerHook(HMODULE hGame);
bool Uninstall_HoldupCancelLookToPlayerHook();

// If you DON'T have the reload hook file right now, keep these stubs.
// If you DO have it, define HAS_RELOAD_HOOK in project settings and link real functions.
#ifndef HAS_RELOAD_HOOK
bool Install_ReloadForLangChangeHook(HMODULE) { return false; }
bool Uninstall_ReloadForLangChangeHook() { return true; }
#else
bool Install_ReloadForLangChangeHook(HMODULE hGame);
bool Uninstall_ReloadForLangChangeHook();
#endif

static HANDLE gConsoleOut = nullptr;
static std::atomic_bool gConsoleReady{ false };
static std::atomic_bool gStarted{ false };

// Make Log() visible to other translation units (NOT static)
void Log(const char* fmt, ...)
{
    char buf[2048];

    va_list va;
    va_start(va, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, va);
    va_end(va);

    OutputDebugStringA(buf);

    if (gConsoleReady.load())
    {
        DWORD written = 0;
        WriteConsoleA(gConsoleOut, buf, (DWORD)strlen(buf), &written, nullptr);
    }
}

// Make SetupConsole() visible too (optional, but handy)
bool SetupConsole()
{
    if (gConsoleReady.load())
        return true;

    if (!AllocConsole())
    {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            return false;
    }

    FILE* fDummy = nullptr;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    freopen_s(&fDummy, "CONIN$", "r", stdin);

    SetConsoleTitleW(L"MGSV Hook Logs");
    gConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);

    gConsoleReady.store(gConsoleOut && gConsoleOut != INVALID_HANDLE_VALUE);
    return gConsoleReady.load();
}

static DWORD WINAPI InitThread(LPVOID)
{
    SetupConsole();
    Log("[DLL] InitThread started.\n");

    HMODULE hGame = GetModuleHandleW(nullptr);
    Log("[DLL] GetModuleHandleW(nullptr) = %p\n", (void*)hGame);

    if (!hGame)
    {
        Log("[DLL] ERROR: hGame is null.\n");
        return 0;
    }

    MH_STATUS st = MH_Initialize();
    Log("[DLL] MH_Initialize -> %d\n", (int)st);
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return 0;

    Sleep(500);

    bool ok1 = Install_ReloadForLangChangeHook(hGame);
    Log("[DLL] Install_ReloadForLangChangeHook: %s\n", ok1 ? "OK" : "FAIL");

    bool ok2 = Install_HoldupCancelLookToPlayerHook(hGame);
    Log("[DLL] Install_HoldupCancelLookToPlayerHook: %s\n", ok2 ? "OK" : "FAIL");

    Log("[DLL] InitThread done.\n");
    return 0;
}

static void UninstallAll(bool processTerminating)
{
    // If process is terminating, keep detach minimal.
    if (processTerminating)
        return;

    Log("[DLL] UninstallAll...\n");

    Uninstall_HoldupCancelLookToPlayerHook();
    Uninstall_ReloadForLangChangeHook();

    MH_Uninitialize();

    Log("[DLL] UninstallAll done.\n");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        bool expected = false;
        if (!gStarted.compare_exchange_strong(expected, true))
            return TRUE;

        HANDLE hThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);

        return TRUE;
    }

    case DLL_PROCESS_DETACH:
    {
        const bool processTerminating = (lpReserved != nullptr);
        UninstallAll(processTerminating);
        return TRUE;
    }
    }
    return TRUE;
}