#include "pch.h"
#include "HeliVoice.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    using FNVHash32_t = unsigned int(__fastcall*)(const char* strToHash);
    static FNVHash32_t g_FNVHash32 = nullptr;

    unsigned int GetFNVHash32(const char* strToHash)
    {
        if (!g_FNVHash32 && gAddr.FNVHash32)
        {
            g_FNVHash32 = reinterpret_cast<FNVHash32_t>(
                ResolveGameAddress(gAddr.FNVHash32));
        }

        if (!g_FNVHash32 || !strToHash)
            return 0;

        const unsigned int ret = g_FNVHash32(strToHash);
        return ret;
    }

    bool TogglePatch(bool isEnable,
                     uintptr_t pointer,
                     SIZE_T dwSize,
                     const std::uint8_t* originalBytes,
                     const std::uint8_t* enabledBytes)
    {
        if (!pointer)
        {
            LogDebug("[HeliVoice] TogglePatch(%s): address not set for current build\n",
                isEnable ? "true" : "false");
            return false;
        }

        void* target = ResolveGameAddress(pointer);
        if (!target)
        {
            LogDebug("[HeliVoice] TogglePatch(%s): ResolveGameAddress @0x%llx null\n",
                isEnable ? "true" : "false", static_cast<unsigned long long>(pointer));
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, dwSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("[HeliVoice] TogglePatch(%s): VirtualProtect failed @0x%llx (err=%lu)\n",
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

    static const char* kOriginalVoiceEvent = "DD_vox_SH_voice";
    static const char* kOriginalRadioEvent = "DD_vox_SH_radio";
}

bool SetEnableHeliVoice(bool isEnable,
                        const char* DD_vox_SH_voice_new,
                        const char* DD_vox_SH_radio_new)
{
    unsigned int origVoiceHash = GetFNVHash32(kOriginalVoiceEvent);
    unsigned int origRadioHash = GetFNVHash32(kOriginalRadioEvent);
    unsigned int newVoiceHash  = GetFNVHash32(DD_vox_SH_voice_new);
    unsigned int newRadioHash  = GetFNVHash32(DD_vox_SH_radio_new);

    auto* origVoiceBytes = reinterpret_cast<std::uint8_t*>(&origVoiceHash);
    auto* origRadioBytes = reinterpret_cast<std::uint8_t*>(&origRadioHash);
    auto* newVoiceBytes  = reinterpret_cast<std::uint8_t*>(&newVoiceHash);
    auto* newRadioBytes  = reinterpret_cast<std::uint8_t*>(&newRadioHash);

    const SIZE_T dwSize = sizeof(unsigned int);

    const bool ok0 = TogglePatch(isEnable, gAddr.DD_vox_SH_voice,  dwSize, origVoiceBytes, newVoiceBytes);
    const bool ok1 = TogglePatch(isEnable, gAddr.DD_vox_SH_radio,  dwSize, origRadioBytes, newRadioBytes);
    const bool ok2 = TogglePatch(isEnable, gAddr.DD_vox_SH_radio2, dwSize, origRadioBytes, newRadioBytes);
    const bool ok3 = TogglePatch(isEnable, gAddr.DD_vox_SH_radio3, dwSize, origRadioBytes, newRadioBytes);

    return ok0 && ok1 && ok2 && ok3;
}
