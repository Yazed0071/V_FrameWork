#include "pch.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>

#include "MinHook.h"
#include "log.h"
#include "BuiltInModules.h"
#include "FeatureModule.h"

namespace
{
    static std::atomic_bool gStarted{ false };
    static std::atomic_bool gConsoleReady{ false };
}


static void SetupConsole()
{
    if (gConsoleReady.load())
        return;

    if (!AllocConsole())
        AttachConsole(ATTACH_PARENT_PROCESS);

    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    SetConsoleTitleW(L"V_FrameWork");
    gConsoleReady.store(true);

    printf("[DLL] Console ready\n");
    fflush(stdout);
}

static DWORD WINAPI InitThread(LPVOID)
{
    #ifdef _DEBUG
    SetupConsole();
    #endif

    Log("[DLL] InitThread started.\n");

    const MH_STATUS st = MH_Initialize();
    Log("[DLL] MH_Initialize -> %d\n", static_cast<int>(st));
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return 0;

    RegisterBuiltInFeatureModules();



    HMODULE hGame = GetModuleHandleW(nullptr);
    const bool allOk = FeatureModuleRegistry::Instance().InstallAll(hGame);
    Log("[DLL] FeatureModuleRegistry::InstallAll -> %s\n", allOk ? "OK" : "PARTIAL/FAIL");

    Log("[DLL] InitThread done.\n");
    return 0;
}

static void UninstallAll(bool processTerminating)
{
    if (processTerminating)
        return;

    FeatureModuleRegistry::Instance().UninstallAll();
    MH_Uninitialize();
    Log("[DLL] UninstallAll done.\n");

    fflush(stdout);
    fflush(stderr);
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
        if (hThread)
            CloseHandle(hThread);

        return TRUE;
    }

    case DLL_PROCESS_DETACH:
    {
        UninstallAll(lpReserved != nullptr);
        return TRUE;
    }
    }

    return TRUE;
}