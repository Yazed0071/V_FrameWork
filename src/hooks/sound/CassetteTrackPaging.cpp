#include "pch.h"
#include "CassetteTrackPaging.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kWindowCapacity   = 0x80;
    constexpr std::uintptr_t kImpl_AlbumId    = 0xD78;
    constexpr std::uintptr_t kImpl_Table      = 0xD80;
    constexpr std::uintptr_t kImpl_Count      = 0xF80;
    constexpr std::uintptr_t kImpl_Selected   = 0xF84;

    using ImplFn_t = void(__fastcall*)(void*);
    using RowRefresh_t = void(__fastcall*)(void*);

    static ImplFn_t     g_UpdateCallFuncs   = nullptr;
    static ImplFn_t     g_RefreshPrefab     = nullptr;
    static RowRefresh_t g_OrigRowRefresh    = nullptr;
    static bool         g_PagingReady       = false;
    static bool         g_LabelPathWarned   = false;

    static std::mutex                 g_Mutex;
    static void*                      g_PagingCallback = nullptr;
    static std::uint64_t              g_PagingAlbumId  = 0;
    static std::vector<std::uint32_t> g_FullTracks;
    static std::vector<std::uint32_t> g_PageStart;
    static std::uint32_t              g_Page          = 0;
    static std::uint32_t              g_SentinelNext  = 0xFFFFFF01u;
    static std::uint32_t              g_SentinelPrev  = 0xFFFFFF02u;

    static int SehAv(unsigned long code)
    {
        return (code == EXCEPTION_ACCESS_VIOLATION
                || code == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    static int ReadU32SEH(const void* base, std::uintptr_t off, std::uint32_t* out)
    {
        __try
        {
            *out = *reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(base) + off);
            return 1;
        }
        __except (SehAv(GetExceptionCode()))
        {
            return 0;
        }
    }

    static int ReadU64SEH(const void* base, std::uintptr_t off, std::uint64_t* out)
    {
        __try
        {
            *out = *reinterpret_cast<const std::uint64_t*>(
                static_cast<const std::uint8_t*>(base) + off);
            return 1;
        }
        __except (SehAv(GetExceptionCode()))
        {
            return 0;
        }
    }

    static int WriteU32SEH(void* base, std::uintptr_t off, std::uint32_t v)
    {
        __try
        {
            *reinterpret_cast<std::uint32_t*>(
                static_cast<std::uint8_t*>(base) + off) = v;
            return 1;
        }
        __except (SehAv(GetExceptionCode()))
        {
            return 0;
        }
    }

    static int WriteWindowSEH(void* cb, const std::uint32_t* ids, std::uint32_t n)
    {
        __try
        {
            std::uint8_t* p = static_cast<std::uint8_t*>(cb);
            std::memset(p + kImpl_Table, 0, kWindowCapacity * 4);
            std::memcpy(p + kImpl_Table, ids, n * 4);
            *reinterpret_cast<std::uint32_t*>(p + kImpl_Count) = n;
            return 1;
        }
        __except (SehAv(GetExceptionCode()))
        {
            return 0;
        }
    }

    static int CallImplFnSEH(ImplFn_t fn, void* cb)
    {
        __try
        {
            fn(cb);
            return 1;
        }
        __except (SehAv(GetExceptionCode()))
        {
            return 0;
        }
    }

    static int RenderPagerRowSEH(std::uint8_t* rcf, const char* label)
    {
        __try
        {
            char* dst = reinterpret_cast<char*>(rcf + 0xC5);
            int i = 0;
            for (; label[i] && i < 15; ++i) dst[i] = label[i];
            dst[i] = 0;

            std::uint64_t util = *reinterpret_cast<std::uint64_t*>(rcf + 0x38);
            if (!util) return 0;
            std::uint64_t uiComp = *reinterpret_cast<std::uint64_t*>(util + 0x20);
            if (!uiComp) return 0;
            std::uint64_t* vt = *reinterpret_cast<std::uint64_t**>(uiComp);
            if (!vt) return 0;
            const std::uint64_t textHandle = *reinterpret_cast<std::uint64_t*>(rcf + 0x98);
            const std::uint64_t textArg    = *reinterpret_cast<std::uint64_t*>(rcf + 0x40);

            using SetText_t = void(__fastcall*)(std::uint64_t, void*, std::uint64_t,
                                                std::uint64_t, char*, std::uint8_t,
                                                std::uint8_t);
            using SetStyle_t = void(__fastcall*)(std::uint64_t, std::uint64_t, std::uint32_t);
            using Enable_t   = void(__fastcall*)(std::uint64_t, std::uint64_t, std::uint32_t);

            reinterpret_cast<SetText_t>(vt[0x718 / 8])(
                uiComp, rcf, textHandle, textArg, dst, 1, 0);
            reinterpret_cast<SetStyle_t>(vt[0x318 / 8])(uiComp, textHandle, 0xC148B6DDu);
            reinterpret_cast<Enable_t>(vt[0x2A8 / 8])(uiComp, textHandle, 1);

            std::uint64_t src2 = *reinterpret_cast<std::uint64_t*>(rcf + 0x50);
            if (src2)
            {
                std::uint64_t comp2 = *reinterpret_cast<std::uint64_t*>(src2 + 0x20);
                if (comp2)
                {
                    std::uint64_t* vt2 = *reinterpret_cast<std::uint64_t**>(comp2);
                    if (vt2)
                    {
                        reinterpret_cast<Enable_t>(vt2[0x2B0 / 8])(
                            comp2, *reinterpret_cast<std::uint64_t*>(rcf + 0x58), 0);
                        using Tail_t = void(__fastcall*)(std::uint64_t, std::uint64_t);
                        reinterpret_cast<Tail_t>(vt2[0x6B0 / 8])(
                            comp2, *reinterpret_cast<std::uint64_t*>(rcf + 0x80));
                    }
                }
            }
            return 1;
        }
        __except (SehAv(GetExceptionCode()))
        {
            return 0;
        }
    }

    static std::uint32_t PageCount_NoLock()
    {
        return static_cast<std::uint32_t>(g_PageStart.size());
    }

    static void RebuildPageStarts_NoLock()
    {
        g_PageStart.clear();
        const std::uint32_t n = static_cast<std::uint32_t>(g_FullTracks.size());

        bool collide = true;
        while (collide)
        {
            collide = false;
            for (const std::uint32_t id : g_FullTracks)
            {
                if (id == g_SentinelNext) { g_SentinelNext += 0x100; collide = true; }
                if (id == g_SentinelPrev) { g_SentinelPrev += 0x100; collide = true; }
            }
        }

        if (n <= kWindowCapacity)
        {
            g_PageStart.push_back(0);
            return;
        }
        std::uint32_t start = 0;
        while (start < n)
        {
            g_PageStart.push_back(start);
            const bool first = start == 0;
            const std::uint32_t lastCap = kWindowCapacity - (first ? 0u : 1u);
            if (n - start <= lastCap)
                break;
            start += kWindowCapacity - (first ? 1u : 2u);
        }
    }

    static std::uint32_t BuildWindow_NoLock(std::uint32_t* out)
    {
        const std::uint32_t n = static_cast<std::uint32_t>(g_FullTracks.size());
        const std::uint32_t pages = PageCount_NoLock();
        const std::uint32_t start = g_PageStart[g_Page];
        const bool hasPrev = g_Page > 0;
        const bool hasNext = g_Page + 1 < pages;
        std::uint32_t cap = kWindowCapacity - (hasPrev ? 1u : 0u) - (hasNext ? 1u : 0u);
        std::uint32_t k = n - start;
        if (k > cap) k = cap;
        for (std::uint32_t i = 0; i < k; ++i)
            out[i] = g_FullTracks[start + i];
        std::uint32_t w = k;
        if (hasPrev) out[w++] = g_SentinelPrev;
        if (hasNext) out[w++] = g_SentinelNext;
        return w;
    }

    static void __fastcall hkRowRefresh(void* rcf)
    {
        std::uint8_t* r = static_cast<std::uint8_t*>(rcf);
        std::uint32_t idx = 0, count = 0;
        std::uint64_t tabPtr = 0;
        if (g_PagingReady
            && ReadU32SEH(r, 0x8, &idx) && ReadU32SEH(r, 0xC0, &count)
            && ReadU64SEH(r, 0xB8, &tabPtr) && tabPtr && idx < count)
        {
            std::uint32_t id = 0;
            if (ReadU32SEH(reinterpret_cast<void*>(tabPtr),
                           static_cast<std::uintptr_t>(idx) * 4, &id)
                && IsPagerSentinelId(id))
            {
                char label[16];
                std::uint32_t page = 0, pages = 1;
                {
                    std::lock_guard<std::mutex> lock(g_Mutex);
                    page = g_Page;
                    pages = PageCount_NoLock();
                    if (pages == 0) pages = 1;
                }
                if (id == g_SentinelNext)
                    std::snprintf(label, sizeof(label), "NEXT %u/%u",
                                  page + 2, pages);
                else
                    std::snprintf(label, sizeof(label), "PREV %u/%u",
                                  page, pages);
                if (RenderPagerRowSEH(r, label) != 1 && !g_LabelPathWarned)
                {
                    g_LabelPathWarned = true;
                    LogDebug("[CassetteMenu] pager row label path unavailable - "
                        "pager rows work but show stale text\n");
                }
                return;
            }
        }
        g_OrigRowRefresh(rcf);
    }
}

bool CassettePagingAvailable()
{
    return g_PagingReady;
}

void SetCassetteAlbumTracks(void* cassetteCallback, std::uint64_t albumId,
                            const std::uint32_t* trackIds, std::uint32_t trackCount)
{
    std::uint32_t window[kWindowCapacity];
    std::uint32_t w = 0;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_PagingCallback = cassetteCallback;
        g_PagingAlbumId  = albumId;
        g_Page           = 0;
        g_FullTracks.assign(trackIds, trackIds + trackCount);
        RebuildPageStarts_NoLock();
        w = BuildWindow_NoLock(window);
    }
    if (WriteWindowSEH(cassetteCallback, window, w) != 1)
        LogDebug("[CassetteMenu] track window write faulted - album list unchanged\n");
    else if (trackCount > kWindowCapacity)
        LogDebug("[CassetteMenu] album has %u tracks - paged across %u pages\n",
            trackCount, static_cast<unsigned>(g_PageStart.size()));
}

int GetPagerActionForSlot(void* cassetteCallback, std::uint32_t slot)
{
    if (!g_PagingReady || !cassetteCallback)
        return 0;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (cassetteCallback != g_PagingCallback || PageCount_NoLock() <= 1)
            return 0;
    }
    std::uint64_t liveAlbum = 0;
    if (ReadU64SEH(cassetteCallback, kImpl_AlbumId, &liveAlbum)
        && liveAlbum != g_PagingAlbumId)
        return 0;
    std::uint32_t count = 0, id = 0;
    if (ReadU32SEH(cassetteCallback, kImpl_Count, &count) != 1 || slot >= count)
        return 0;
    if (ReadU32SEH(cassetteCallback, kImpl_Table + slot * 4, &id) != 1)
        return 0;
    if (id == g_SentinelNext) return 1;
    if (id == g_SentinelPrev) return -1;
    return 0;
}

bool IsPagerSentinelId(std::uint32_t trackId)
{
    return trackId == g_SentinelNext || trackId == g_SentinelPrev;
}

void FlipCassettePage(void* cassetteCallback, int direction)
{
    std::uint32_t window[kWindowCapacity];
    std::uint32_t w = 0;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        const std::uint32_t pages = PageCount_NoLock();
        if (pages <= 1)
            return;
        std::int64_t p = static_cast<std::int64_t>(g_Page) + direction;
        if (p < 0) p = 0;
        if (p >= pages) p = pages - 1;
        g_Page = static_cast<std::uint32_t>(p);
        w = BuildWindow_NoLock(window);
    }
    if (WriteWindowSEH(cassetteCallback, window, w) != 1)
    {
        LogDebug("[CassetteMenu] page flip window write faulted\n");
        return;
    }
    WriteU32SEH(cassetteCallback, kImpl_Selected, 0);
    if (g_UpdateCallFuncs && CallImplFnSEH(g_UpdateCallFuncs, cassetteCallback) != 1)
        LogDebug("[CassetteMenu] UpdateTrackListCallFuncs faulted on page flip\n");
    if (g_RefreshPrefab && CallImplFnSEH(g_RefreshPrefab, cassetteCallback) != 1)
        LogDebug("[CassetteMenu] RefreshTrackListPrefabParameter faulted on page flip\n");
}

bool Install_CassetteTrackPaging_Hooks()
{
    g_UpdateCallFuncs = reinterpret_cast<ImplFn_t>(
        ResolveGameAddress(gAddr.MbDvcUpdateTrackListCallFuncs));
    g_RefreshPrefab = reinterpret_cast<ImplFn_t>(
        ResolveGameAddress(gAddr.MbDvcRefreshTrackListPrefabParameter));
    void* rowFn = ResolveGameAddress(gAddr.MbDvcTrackListRecordRefresh);

    if (!g_UpdateCallFuncs || !g_RefreshPrefab || !rowFn)
    {
        Log("[CassetteMenu] ERROR: track-list paging unavailable for this build "
            "(update=%p refresh=%p rowRefresh=%p) - albums are capped at 128 "
            "visible tracks\n",
            reinterpret_cast<void*>(g_UpdateCallFuncs),
            reinterpret_cast<void*>(g_RefreshPrefab), rowFn);
        g_PagingReady = false;
        return true;
    }

    const bool ok = CreateAndEnableHook(
        rowFn, reinterpret_cast<void*>(&hkRowRefresh),
        reinterpret_cast<void**>(&g_OrigRowRefresh));
    if (!ok)
    {
        Log("[CassetteMenu] ERROR: pager row hook failed - albums are capped "
            "at 128 visible tracks\n");
        g_PagingReady = false;
        return true;
    }
    g_PagingReady = true;
#ifdef _DEBUG
    LogDebug("[CassetteMenu] track-list paging armed (window %u rows)\n",
        kWindowCapacity);
#endif
    return true;
}

bool Uninstall_CassetteTrackPaging_Hooks()
{
    if (g_OrigRowRefresh && gAddr.MbDvcTrackListRecordRefresh)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.MbDvcTrackListRecordRefresh));
    g_OrigRowRefresh = nullptr;
    g_UpdateCallFuncs = nullptr;
    g_RefreshPrefab = nullptr;
    g_PagingReady = false;
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_PagingCallback = nullptr;
    g_PagingAlbumId = 0;
    g_FullTracks.clear();
    g_PageStart.clear();
    g_Page = 0;
    return true;
}
