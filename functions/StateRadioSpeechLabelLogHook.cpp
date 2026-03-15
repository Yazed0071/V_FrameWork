#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <intrin.h>

#include "HookUtils.h"
#include "log.h"

namespace
{
    // Hook target:
    // tpp::gm::CpRadioService::ConvertRadioTypeToSpeechLabel
    // Params:
    //   radioType = enum/radio type byte promoted into ECX
    using ConvertRadioTypeToSpeechLabel_t =
        std::uint32_t(__fastcall*)(std::uint32_t radioType);

    // Derived from the call inside StateRadio:
    //   140D695B8  CALL  tpp::gm::CpRadioService::ConvertRadioTypeToSpeechLabel
    static constexpr std::uintptr_t ABS_ConvertRadioTypeToSpeechLabel = 0x140D685C0ull;

    // Return address immediately after that CALL inside StateRadio.
    // We use this to filter logs so we only log StateRadio's conversion call.
    static constexpr std::uintptr_t ABS_StateRadio_Convert_Return = 0x140D695BDull;

    static ConvertRadioTypeToSpeechLabel_t g_OrigConvertRadioTypeToSpeechLabel = nullptr;
}

// Returns true if the current call came from StateRadio's ConvertRadioTypeToSpeechLabel callsite.
// Params: none
static bool IsCalledFromStateRadioConvertSite()
{
    const void* retAddr = _ReturnAddress();
    if (!retAddr)
        return false;

    void* expectedRet = ResolveGameAddress(ABS_StateRadio_Convert_Return);
    if (!expectedRet)
        return false;

    return retAddr == expectedRet;
}

// Hooked ConvertRadioTypeToSpeechLabel.
// Logs the incoming enum only when called from ActionControllerImpl::StateRadio.
// Params: radioType (uint32_t)
static std::uint32_t __fastcall hkConvertRadioTypeToSpeechLabel(std::uint32_t radioType)
{
    const std::uint32_t result = g_OrigConvertRadioTypeToSpeechLabel(radioType);

    if (IsCalledFromStateRadioConvertSite())
    {
        const std::uint8_t enumByte = static_cast<std::uint8_t>(radioType & 0xFFu);

        Log("[StateRadioLabel] enum=0x%02X (%u) resultLabel=0x%08X\n",
            static_cast<unsigned>(enumByte),
            static_cast<unsigned>(enumByte),
            result);
    }

    return result;
}

// Installs the StateRadio speech-label enum logger.
// Params: none
bool Install_StateRadioSpeechLabelLog_Hook()
{
    void* target = ResolveGameAddress(ABS_ConvertRadioTypeToSpeechLabel);
    if (!target)
    {
        Log("[StateRadioLabel] target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkConvertRadioTypeToSpeechLabel),
        reinterpret_cast<void**>(&g_OrigConvertRadioTypeToSpeechLabel));

    Log("[StateRadioLabel] Install: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes the StateRadio speech-label enum logger.
// Params: none
bool Uninstall_StateRadioSpeechLabelLog_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_ConvertRadioTypeToSpeechLabel));
    g_OrigConvertRadioTypeToSpeechLabel = nullptr;

    Log("[StateRadioLabel] Removed\n");
    return true;
}