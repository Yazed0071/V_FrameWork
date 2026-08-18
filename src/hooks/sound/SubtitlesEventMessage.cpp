#include "pch.h"
#include "SubtitlesEventMessage.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>

#include "AddressSet.h"
#include "FoxHashes.h"
#include "HookUtils.h"
#include "LuaBroadcaster.h"
#include "log.h"
#include "MissionCodeGuard.h"


namespace
{
    using SendMessage_t = void(__fastcall*)(void* self);

    static SendMessage_t g_OrigSendMessage = nullptr;

    // SubtitlesObject: currentPageIndex @ +0xc, pageCount @ +0x10, Page* array @ +0x40; Page name (char*) @ +8.
    static const char* ReadCurrentPageMessage(void* self)
    {
        if (!self)
            return nullptr;

        __try
        {
            const auto base = reinterpret_cast<std::uintptr_t>(self);
            const std::uint32_t index = *reinterpret_cast<const std::uint32_t*>(base + 0xcull);
            const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(base + 0x10ull);
            if (index >= count)
                return nullptr;

            void* const* pageArray = *reinterpret_cast<void* const* const*>(base + 0x40ull);
            if (!pageArray)
                return nullptr;

            const void* page = pageArray[index];
            if (!page)
                return nullptr;

            const char* message = *reinterpret_cast<const char* const*>(reinterpret_cast<std::uintptr_t>(page) + 0x8ull);
            return (message && message[0]) ? message : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void __fastcall hkSendMessage(void* self)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigSendMessage, self);

        if (g_OrigSendMessage)
            g_OrigSendMessage(self);

        const char* message = ReadCurrentPageMessage(self);
        if (message)
            V_FrameWork::EmitMessage("Subtitles", "SubtitlesEventMessage", FoxHashes::StrCode32(message));
    }
}

bool Install_SubtitlesEventMessage_Hook()
{
    if (!gAddr.SubtitlesObjectSendMessage)
    {
        LogDebug("[SubtitlesEvent] address not set for this build -- skipped\n");
        return false;
    }

    void* target = ResolveGameAddress(gAddr.SubtitlesObjectSendMessage);
    if (!target)
    {
        Log("[SubtitlesEvent] resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target, reinterpret_cast<void*>(&hkSendMessage), reinterpret_cast<void**>(&g_OrigSendMessage));

    if (ok)
        LogDebug("[SubtitlesEvent] SubtitlesObjectSendMessage:OK\n");
    else
        Log("[SubtitlesEvent] SubtitlesObjectSendMessage:FAIL\n");
    return ok;
}

bool Uninstall_SubtitlesEventMessage_Hook()
{
    if (gAddr.SubtitlesObjectSendMessage)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.SubtitlesObjectSendMessage));

    g_OrigSendMessage = nullptr;
    return true;
}
