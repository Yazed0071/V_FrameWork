#include "pch.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "MinHook.h"
#include "HookUtils.h"
#include "log.h"
#include "AddressSet.h"
#include "NoticeControllerImpl_GetOccasionalChat.h"
#include "MissionCodeGuard.h"

namespace
{
    using GetOccasionalChat_t =
        std::uint32_t(__fastcall*)(std::uintptr_t self, std::uint32_t param1, std::uint32_t param2);
    using ConvertType_t = std::uint8_t(__fastcall*)(std::uint32_t speechLabel);

    static GetOccasionalChat_t g_OrigGet     = nullptr;
    static ConvertType_t       g_OrigConvert = nullptr;
    static bool                g_Installed   = false;

    enum class Mode { Off, Set, Insert };

    static std::mutex                 g_Mtx;
    static Mode                       g_Mode = Mode::Off;
    static std::vector<std::uint32_t> g_Labels;
    static std::vector<std::uint32_t> g_Removed;

    static thread_local const std::uint32_t* tl_hashes = nullptr;
    static thread_local std::size_t          tl_count  = 0;
    static thread_local bool                 tl_active = false;

    static constexpr std::uintptr_t kListPtr = 0x58;
    static constexpr std::size_t    kCap     = 255;
    static constexpr std::size_t    kStride  = 8;

    static std::uint8_t __fastcall hk_ConvertType(std::uint32_t speechLabel)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigConvert ? g_OrigConvert(speechLabel) : 0;

        if (tl_active && tl_hashes)
        {
            for (std::size_t i = 0; i < tl_count; ++i)
                if (tl_hashes[i] == speechLabel) return 0;
        }
        return g_OrigConvert ? g_OrigConvert(speechLabel) : 0;
    }

    static bool Contains(std::uint32_t h, const std::uint32_t* arr, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i)
            if (arr[i] == h) return true;
        return false;
    }

    static std::uint8_t g_FactionNopOriginal[5] = {};
    static bool         g_FactionNopApplied     = false;

    static void ApplyFactionTestNop(bool enable)
    {
        if (enable && MissionCodeGuard::ShouldBypassHooks())
            enable = false;

        if (!gAddr.OccasionalChat_FactionTestNop)
            return;
        void* target = ResolveGameAddress(gAddr.OccasionalChat_FactionTestNop);
        if (!target || enable == g_FactionNopApplied)
            return;

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(g_FactionNopOriginal), PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            Log("[OccasionalChatList] faction-NOP VirtualProtect failed (err=%lu)\n", GetLastError());
            return;
        }

        if (enable)
        {
            std::memcpy(g_FactionNopOriginal, target, sizeof(g_FactionNopOriginal));
            const std::uint8_t nops[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
            std::memcpy(target, nops, sizeof(nops));
            g_FactionNopApplied = true;
        }
        else
        {
            std::memcpy(target, g_FactionNopOriginal, sizeof(g_FactionNopOriginal));
            g_FactionNopApplied = false;
        }

        DWORD restored = 0;
        VirtualProtect(target, sizeof(g_FactionNopOriginal), oldProtect, &restored);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(g_FactionNopOriginal));
    }

    static std::uint32_t DoInjectedCall(std::uintptr_t self, std::uint32_t p1, std::uint32_t p2,
                                        int mode, const std::uint32_t* hashes, std::size_t count,
                                        const std::uint32_t* removed, std::size_t removedCount)
    {
        __try
        {
            const std::uintptr_t list = *reinterpret_cast<std::uintptr_t*>(self + kListPtr);
            if (!list)
                return g_OrigGet(self, p1, p2);

            std::uint8_t save[kCap * kStride];
            std::memcpy(save, reinterpret_cast<const void*>(list), sizeof(save));

            if (mode == 1)
                std::memset(reinterpret_cast<void*>(list), 0, sizeof(save));

            for (std::size_t k = 0; k < count; ++k)
            {
                const std::uint32_t lbl = hashes[k];
                std::size_t empty = kCap;
                bool exists = false;
                for (std::size_t i = 0; i < kCap; ++i)
                {
                    const std::uint32_t cur = *reinterpret_cast<std::uint32_t*>(list + i * kStride);
                    if (cur == 0) { empty = i; break; }
                    if (cur == lbl) { exists = true; break; }
                }
                if (!exists && empty < kCap)
                {
                    *reinterpret_cast<std::uint32_t*>(list + empty * kStride)     = lbl;
                    *reinterpret_cast<std::uint8_t*>(list + empty * kStride + 4) &= 0xFE;
                }
            }

            if (removedCount > 0)
            {
                std::size_t w = 0;
                for (std::size_t r = 0; r < kCap; ++r)
                {
                    const std::uint32_t h = *reinterpret_cast<std::uint32_t*>(list + r * kStride);
                    if (h == 0) break;
                    if (Contains(h, removed, removedCount)) continue;
                    if (w != r)
                    {
                        *reinterpret_cast<std::uint32_t*>(list + w * kStride) = h;
                        *reinterpret_cast<std::uint8_t*>(list + w * kStride + 4) =
                            *reinterpret_cast<std::uint8_t*>(list + r * kStride + 4);
                    }
                    ++w;
                }
                if (w < kCap)
                    *reinterpret_cast<std::uint32_t*>(list + w * kStride) = 0;
            }

            tl_hashes = hashes; tl_count = count; tl_active = true;
            const std::uint32_t result = g_OrigGet(self, p1, p2);
            tl_active = false; tl_hashes = nullptr; tl_count = 0;

            std::memcpy(reinterpret_cast<void*>(list), save, sizeof(save));
            return result;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tl_active = false; tl_hashes = nullptr; tl_count = 0;
            return 0;
        }
    }

    static std::uint32_t __fastcall hk_GetOccasionalChat(std::uintptr_t self, std::uint32_t p1, std::uint32_t p2)
    {
        if (!g_OrigGet)
            return 0;

        MISSION_GUARD_ORIGINAL_RET(g_OrigGet, self, p1, p2);

        int mode = 0;
        std::vector<std::uint32_t> hashes;
        std::vector<std::uint32_t> removed;
        {
            std::lock_guard<std::mutex> lk(g_Mtx);
            if (g_Mode == Mode::Set)         mode = 1;
            else if (g_Mode == Mode::Insert) mode = 2;
            if (mode != 0) hashes = g_Labels;
            removed = g_Removed;
        }

        if (self == 0 || (mode == 0 && removed.empty()))
            return g_OrigGet(self, p1, p2);

        return DoInjectedCall(self, p1, p2, mode, hashes.data(), hashes.size(),
                              removed.data(), removed.size());
    }
}

bool Install_OccasionalChatList_Hook()
{
    if (g_Installed) return true;

    if (!gAddr.NoticeControllerImpl_GetOccasionalChat)
    {
        LogDebug("[OccasionalChatList] GetOccasionalChat address is 0 (unsupported build)\n");
        return false;
    }

    void* getTarget = ResolveGameAddress(gAddr.NoticeControllerImpl_GetOccasionalChat);
    if (!getTarget)
    {
        Log("[OccasionalChatList] GetOccasionalChat resolve failed\n");
        return false;
    }

    if (!CreateAndEnableHook(getTarget, reinterpret_cast<void*>(&hk_GetOccasionalChat),
                             reinterpret_cast<void**>(&g_OrigGet)))
    {
        Log("[OccasionalChatList] GetOccasionalChat hook FAILED @ %p\n", getTarget);
        return false;
    }

    if (gAddr.SoldierConversationService_ConvertSpeechLabelToConversationType)
    {
        void* convTarget =
            ResolveGameAddress(gAddr.SoldierConversationService_ConvertSpeechLabelToConversationType);
        if (convTarget &&
            !CreateAndEnableHook(convTarget, reinterpret_cast<void*>(&hk_ConvertType),
                                 reinterpret_cast<void**>(&g_OrigConvert)))
        {
            Log("[OccasionalChatList] classifier hook FAILED\n");
        }
    }

    g_Installed = true;
#ifdef _DEBUG
    LogDebug("[OccasionalChatList] installed (get=%p convert=%p)\n",
        getTarget, reinterpret_cast<void*>(g_OrigConvert));
#endif
    return true;
}

bool Uninstall_OccasionalChatList_Hook()
{
    if (!g_Installed) return true;

    void* getTarget = ResolveGameAddress(gAddr.NoticeControllerImpl_GetOccasionalChat);
    if (getTarget) DisableAndRemoveHook(getTarget);

    if (gAddr.SoldierConversationService_ConvertSpeechLabelToConversationType)
    {
        void* convTarget =
            ResolveGameAddress(gAddr.SoldierConversationService_ConvertSpeechLabelToConversationType);
        if (convTarget && g_OrigConvert) DisableAndRemoveHook(convTarget);
    }

    ApplyFactionTestNop(false);

    g_OrigGet     = nullptr;
    g_OrigConvert = nullptr;
    g_Installed   = false;
    {
        std::lock_guard<std::mutex> lk(g_Mtx);
        g_Mode = Mode::Off;
        g_Labels.clear();
        g_Removed.clear();
    }
    return true;
}

void SetOccasionalChatList(const std::uint32_t* labels, std::size_t count)
{
    std::lock_guard<std::mutex> lk(g_Mtx);
    g_Labels.clear();
    for (std::size_t i = 0; i < count; ++i)
        if (labels[i] != 0) g_Labels.push_back(labels[i]);
    g_Mode = g_Labels.empty() ? Mode::Off : Mode::Set;
    ApplyFactionTestNop(g_Mode == Mode::Set);
}

void InsertToOccasionalChatList(const std::uint32_t* labels, std::size_t count)
{
    std::lock_guard<std::mutex> lk(g_Mtx);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint32_t lbl = labels[i];
        if (lbl == 0) continue;
        bool dup = false;
        for (std::uint32_t e : g_Labels) if (e == lbl) { dup = true; break; }
        if (!dup) g_Labels.push_back(lbl);
    }
    if (g_Mode == Mode::Off && !g_Labels.empty())
        g_Mode = Mode::Insert;
    ApplyFactionTestNop(g_Mode == Mode::Set);
}

void RemoveFromOccasionalChatList(const std::uint32_t* labels, std::size_t count)
{
    std::lock_guard<std::mutex> lk(g_Mtx);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint32_t lbl = labels[i];
        if (lbl == 0) continue;
        bool dup = false;
        for (std::uint32_t e : g_Removed) if (e == lbl) { dup = true; break; }
        if (!dup) g_Removed.push_back(lbl);
    }
    ApplyFactionTestNop(g_Mode == Mode::Set);
}

void ClearOccasionalChatListOverride()
{
    std::lock_guard<std::mutex> lk(g_Mtx);
    g_Labels.clear();
    g_Removed.clear();
    g_Mode = Mode::Off;
    ApplyFactionTestNop(false);
}
