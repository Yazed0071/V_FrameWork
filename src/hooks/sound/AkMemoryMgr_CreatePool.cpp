#include "pch.h"
#include "AkMemoryMgr_CreatePool.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

#include "HookUtils.h"
#include "log.h"

namespace
{
    constexpr std::uint32_t kPoolBlockSize   = 0x400;
    constexpr std::uint32_t kPoolAttributes  = 1;
    constexpr std::uint32_t kStockPoolSize   = 0x1000000;
    constexpr std::uint32_t kGrownPoolSize   = 0x8000000;
    constexpr std::int32_t  kInvalidPoolId   = -1;

    const std::uint8_t kAnchor[] = {
        0x41, 0xB8, 0x00, 0x04, 0x00, 0x00,
        0x8B, 0x00, 0x60,
        0x33, 0xC9,
        0xE8
    };
    const std::uint8_t kAnchorMask[] = {
        1, 1, 1, 1, 1, 1,
        1, 0, 1,
        0, 1,
        1
    };
    const std::uint8_t kXorEcxOpcodes[] = { 0x33, 0x31 };
    constexpr std::size_t kXorEcxOpcodeOff = 9;
    constexpr std::size_t kAnchorLen  = sizeof(kAnchor);
    constexpr std::size_t kCallRelOff = 12;
    constexpr std::size_t kCallEndOff = 16;

    using CreatePool_t = std::int32_t(__fastcall*)(void* start, std::uint32_t size,
                                                   std::uint32_t blockSize,
                                                   std::uint32_t attributes,
                                                   std::uint32_t blockAlign);

    CreatePool_t      g_OrigCreatePool = nullptr;
    void*             g_Target         = nullptr;
    std::atomic<bool> g_Grown{ false };

    bool RegionSpan(const std::uint8_t* p, std::size_t* span, bool* readable)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        const auto* base = static_cast<const std::uint8_t*>(mbi.BaseAddress);
        const std::size_t used = static_cast<std::size_t>(p - base);
        *span = (mbi.RegionSize > used) ? (mbi.RegionSize - used) : 0;
        const DWORD readableBits = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                 | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY
                                 | PAGE_READWRITE | PAGE_WRITECOPY;
        *readable = mbi.State == MEM_COMMIT
                    && (mbi.Protect & readableBits) != 0
                    && (mbi.Protect & PAGE_GUARD) == 0;
        return *span != 0;
    }

    bool IsXorEcxOpcode(std::uint8_t op)
    {
        for (std::uint8_t candidate : kXorEcxOpcodes)
        {
            if (op == candidate)
                return true;
        }
        return false;
    }

    bool MatchesAt(const std::uint8_t* p)
    {
        if (!IsXorEcxOpcode(p[kXorEcxOpcodeOff]))
            return false;
        for (std::size_t i = 0; i < kAnchorLen; ++i)
        {
            if (kAnchorMask[i] && p[i] != kAnchor[i])
                return false;
        }
        return true;
    }

    int ScanForAnchor(const std::uint8_t** out)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base)
            return 0;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;

        int found = 0;
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        {
            if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const DWORD size = sec->Misc.VirtualSize
                ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            if (size < kAnchorLen)
                continue;

            std::uint8_t* p   = base + sec->VirtualAddress;
            std::uint8_t* end = p + size;
            while (p < end)
            {
                std::size_t span     = 0;
                bool        readable = false;
                if (!RegionSpan(p, &span, &readable))
                    break;
                if (span > static_cast<std::size_t>(end - p))
                    span = static_cast<std::size_t>(end - p);

                if (readable && span >= kAnchorLen)
                {
                    const std::uint8_t* last = p + span - kAnchorLen;
                    for (const std::uint8_t* q = p; q <= last; ++q)
                    {
                        if (!MatchesAt(q))
                            continue;
                        ++found;
                        if (found == 1 && out)
                            *out = q;
                    }
                }
                p += span;
            }
        }
        return found;
    }

    std::int32_t __fastcall hkCreatePool(void* start, std::uint32_t size,
                                         std::uint32_t blockSize,
                                         std::uint32_t attributes,
                                         std::uint32_t blockAlign)
    {
        const bool isPrepareEventPool =
            start == nullptr
            && blockSize == kPoolBlockSize
            && (attributes & 3) == kPoolAttributes
            && size == kStockPoolSize;

        if (!isPrepareEventPool
            || g_Grown.exchange(true, std::memory_order_relaxed))
            return g_OrigCreatePool(start, size, blockSize, attributes, blockAlign);

        const std::int32_t grown = g_OrigCreatePool(start, kGrownPoolSize, blockSize,
                                                    attributes, blockAlign);
        if (grown != kInvalidPoolId)
            return grown;

        Log("[AudioBank] ERROR: the prepare-event pool could not be grown to %u MB "
            "(the up-front allocation was refused) - falling back to the stock %u MB, "
            "so bank media past that is still dropped and those sounds stay silent\n",
            kGrownPoolSize >> 20, kStockPoolSize >> 20);
        return g_OrigCreatePool(start, size, blockSize, attributes, blockAlign);
    }
}

bool AkMemoryMgr_InstallPrepareEventPoolGrow()
{
    const std::uint8_t* site = nullptr;
    const int hits = ScanForAnchor(&site);
    if (hits != 1 || !site)
    {
        Log("[AudioBank] ERROR: the prepare-event pool creation site was not "
            "identified (%d anchor match(es)) - the pool stays at its stock %u MB, "
            "so once custom bank media fills it the rest is dropped and those "
            "sounds are silent\n", hits, kStockPoolSize >> 20);
        return false;
    }

    const std::int32_t rel =
        *reinterpret_cast<const std::int32_t*>(site + kCallRelOff);
    void* createPool = const_cast<std::uint8_t*>(site) + kCallEndOff + rel;

    if (!CreateAndEnableHook(createPool, &hkCreatePool,
                             reinterpret_cast<void**>(&g_OrigCreatePool)))
    {
        g_OrigCreatePool = nullptr;
        Log("[AudioBank] ERROR: the prepare-event pool grow hook was refused at %p "
            "- the pool stays at its stock %u MB, so once custom bank media fills "
            "it the rest is dropped and those sounds are silent\n",
            createPool, kStockPoolSize >> 20);
        return false;
    }

    g_Target = createPool;
    return true;
}

void AkMemoryMgr_UninstallPrepareEventPoolGrow()
{
    if (g_Target)
        DisableAndRemoveHook(g_Target);
    g_Target         = nullptr;
    g_OrigCreatePool = nullptr;
    g_Grown.store(false, std::memory_order_relaxed);
}
