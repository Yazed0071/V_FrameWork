#include "pch.h"
#include "GameOverMusic.h"

#include <Windows.h>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    bool TogglePatch(bool isEnable,
                     uintptr_t pointer,
                     SIZE_T dwSize,
                     const std::uint8_t* originalBytes,
                     const std::uint8_t* enabledBytes)
    {
        if (!pointer)
        {
            LogDebug("[GameOverMusic] TogglePatch(%s): address not set for current build\n",
                isEnable ? "true" : "false");
            return false;
        }

        void* target = ResolveGameAddress(pointer);
        if (!target)
        {
            LogDebug("[GameOverMusic] TogglePatch(%s): ResolveGameAddress @0x%llx null\n",
                isEnable ? "true" : "false", static_cast<unsigned long long>(pointer));
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, dwSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("[GameOverMusic] TogglePatch(%s): VirtualProtect failed @0x%llx (err=%lu)\n",
                isEnable ? "true" : "false",
                static_cast<unsigned long long>(pointer),
                GetLastError());
            return false;
        }

        const std::uint8_t* src = isEnable ? enabledBytes : originalBytes;
        std::memcpy(target, src, dwSize);

        DWORD restored = 0;
        VirtualProtect(target, dwSize, oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, dwSize);

        return true;
    }

    static unsigned int Play_bgm_gameover_FNV132              = 2016311311u;
    static unsigned int Stop_bgm_gameover_FNV132              = 1577147537u;
    static unsigned int Play_bgm_gameover_paradox_FNV132      = 2933771421u;
    static unsigned int Stop_bgm_gameover_paradox_FNV132      = 300567547u;
    static unsigned int Play_bgm_gameover_perfectstealth_FNV132 = 0x9ae8c2c2u;
    static unsigned int Stop_bgm_gameover_perfectstealth_FNV132 = 0xfa2c3474u;
    static unsigned int Play_bgm_s10010_gameover_FNV132       = 3354865787u;
    static unsigned int Stop_bgm_s10010_gameover_FNV132       = 1018550225u;
}

bool SetGameOverMusic(bool isEnable,
                      GAME_OVER_TYPE type,
                      unsigned int playEventHash,
                      unsigned int stopEventHash)
{
    auto* playEventBytes = reinterpret_cast<std::uint8_t*>(&playEventHash);
    auto* stopEventBytes = reinterpret_cast<std::uint8_t*>(&stopEventHash);

    const SIZE_T dwSize = sizeof(unsigned int);

    if (type == GAME_OVER_GENERAL)
    {
        auto* oldPlayBytes = reinterpret_cast<std::uint8_t*>(&Play_bgm_gameover_FNV132);
        auto* oldStopBytes = reinterpret_cast<std::uint8_t*>(&Stop_bgm_gameover_FNV132);

        const bool okPlay = TogglePatch(isEnable, gAddr.Play_bgm_gameover, dwSize, oldPlayBytes, playEventBytes);
        const bool okStop = TogglePatch(isEnable, gAddr.Stop_bgm_gameover, dwSize, oldStopBytes, stopEventBytes);
        return okPlay && okStop;
    }
    if (type == GAME_OVER_PARADOX)
    {
        auto* oldPlayBytes = reinterpret_cast<std::uint8_t*>(&Play_bgm_gameover_paradox_FNV132);
        auto* oldStopBytes = reinterpret_cast<std::uint8_t*>(&Stop_bgm_gameover_paradox_FNV132);

        const bool okPlay = TogglePatch(isEnable, gAddr.Play_bgm_gameover_paradox, dwSize, oldPlayBytes, playEventBytes);
        const bool okStop = TogglePatch(isEnable, gAddr.Stop_bgm_gameover_paradox, dwSize, oldStopBytes, stopEventBytes);
        const bool okPlaySnd = TogglePatch(isEnable, gAddr.Play_bgm_gameover_paradox_soundId, dwSize, oldPlayBytes, playEventBytes);
        const bool okStopSnd = TogglePatch(isEnable, gAddr.Stop_bgm_gameover_paradox_soundId, dwSize, oldStopBytes, stopEventBytes);
        return okPlay && okStop && okPlaySnd && okStopSnd;
    }
    if (type == GAME_OVER_STEALTH)
    {
        auto* oldPlayBytes = reinterpret_cast<std::uint8_t*>(&Play_bgm_gameover_perfectstealth_FNV132);
        auto* oldStopBytes = reinterpret_cast<std::uint8_t*>(&Stop_bgm_gameover_perfectstealth_FNV132);

        const bool okPlay = TogglePatch(isEnable, gAddr.Play_bgm_gameover_perfectstealth, dwSize, oldPlayBytes, playEventBytes);
        const bool okStop = TogglePatch(isEnable, gAddr.Stop_bgm_gameover_perfectstealth, dwSize, oldStopBytes, stopEventBytes);
        return okPlay && okStop;
    }
    if (type == GAME_OVER_CYPRUS)
    {
        auto* oldPlayBytes = reinterpret_cast<std::uint8_t*>(&Play_bgm_s10010_gameover_FNV132);
        auto* oldStopBytes = reinterpret_cast<std::uint8_t*>(&Stop_bgm_s10010_gameover_FNV132);

        const bool okPlay = TogglePatch(isEnable, gAddr.Play_bgm_s10010_gameover, dwSize, oldPlayBytes, playEventBytes);
        const bool okStop = TogglePatch(isEnable, gAddr.Stop_bgm_s10010_gameover, dwSize, oldStopBytes, stopEventBytes);
        return okPlay && okStop;
    }
    return false;
}
