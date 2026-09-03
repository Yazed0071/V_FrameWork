#include "pch.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "MinHook.h"
#include "log.h"
#include "TppCollectionRuntime.h"
#include "AddressSet.h"
#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "FoxHashes.h"
#include "LuaBroadcaster.h"
#include "../../lua/LuaApi.h"

namespace
{
    constexpr std::uint32_t kAllocAnnotation = 0xd0001;
    constexpr std::uint32_t kMaxLocatorRows  = 0xFFE;

    constexpr std::size_t kOffPositions      = 0x78;
    constexpr std::size_t kOffRotations      = 0x88;
    constexpr std::size_t kOffInfos          = 0x98;
    constexpr std::size_t kOffSegIndices     = 0xA8;
    constexpr std::size_t kOffLocIndices     = 0xB8;
    constexpr std::size_t kOffLocCounts      = 0xC8;
    constexpr std::size_t kOffGroupIds       = 0xD8;
    constexpr std::size_t kOffSegInfoIndices = 0xE8;
    constexpr std::size_t kOffSegCounts      = 0xF8;

    constexpr std::uint32_t kOpSetAt     = 0x9cc6cb50;
    constexpr std::uint32_t kOpDecAt     = 0x469e3b49;
    constexpr std::uint32_t kOpGetAt     = 0x763aed99;
    constexpr std::uint32_t kOpSetAll    = 0xc5443bd6;
    constexpr std::uint32_t kOpDecAll    = 0x2bce4532;
    constexpr std::uint32_t kOpSetByType = 0x0817dbb2;
    constexpr std::uint32_t kOpDecByType = 0xe63b1447;

    struct FoxArrayHdr
    {
        std::uint32_t count;
        std::uint32_t capacity;
        void*         data;
    };

    struct CustomReg
    {
        std::string   name;
        std::uint32_t uid    = 0;
        std::uint8_t  type   = 0;
        std::uint8_t  repop  = 0;
        float         x      = 0.f;
        float         y      = 0.f;
        float         z      = 0.f;
        float         rotY   = 0.f;
    };

    struct TailRow
    {
        std::uint32_t uid       = 0;
        std::int32_t  canonical = -1;
        std::int32_t  regIndex  = -1;
    };

    struct VanillaSnapshot
    {
        std::vector<std::uint8_t>  pos;
        std::vector<std::uint32_t> rot;
        std::vector<std::uint32_t> inf;
        std::vector<std::uint16_t> segIdx;
        std::vector<std::uint16_t> locIdx;
        std::vector<std::uint16_t> locCnt;
        std::vector<std::uint8_t>  groupIds;
        std::vector<std::uint16_t> segInfoIdx;
        std::vector<std::uint16_t> segCnt;
    };

    struct CustomType
    {
        std::string   name;
        std::uint8_t  typeId      = 0;
        std::uint64_t modelPathId = 0;
        std::uint64_t iconPathId  = 0;
        std::uint64_t nameLangId  = 0;
        float         r           = 0.9f;
        float         g           = 0.9f;
        float         b           = 0.5f;
        float         a           = 0.5f;
        float         fxStrengthX = 0.5f;
        float         fxStrengthY = 1.0f;
        float         fxStrengthZ = 0.5f;
        float         fxStrengthW = 1.0f;
        float         effectYOffset    = 0.25f;
        float         groundEffectSize = 0.4f;
        std::uint64_t rootModelPathId  = 0;
        std::uint64_t fovaPathId       = 0;
        std::uint8_t  isHerb           = 0;
        std::uint8_t  isMaterial       = 0;
        std::uint8_t  isDiamond        = 0;
        std::uint8_t  standInLogged   = 0;
        std::uint64_t standInPathId   = 0;
        std::uint64_t drawPathId      = 0;
    };

    static const std::uint64_t kEngineModels[] =
    {
        0x84A308F202AEFA3Bull, 0x84A3A11749C4E6EFull, 0x84A293AC8732231Dull,
        0x84A3650B61BDEB4Cull, 0x84A30859D202DC52ull, 0x84A3E97F47985D22ull,
        0x84A0ACF1437A4414ull, 0x84A13533EDF93EA5ull, 0x84A13696B9E5E84Eull,
        0x84A1BA94772F47E7ull, 0x84A21429EAFD9AFBull, 0x84A21F84483B0E45ull,
        0x84A235FB7FE17DE1ull, 0x84A2A5539DCF4627ull, 0x84A3329346201A68ull,
        0x84A356EB516C4EB5ull, 0x84A02FC6EF0CB9CAull, 0x84A0EC5B2B105B8Aull,
        0x84A109CA0B50B52Dull, 0x84A198FF8FA17073ull, 0x84A2688A5C976165ull,
        0x84A28C3BBA68D4A2ull,
    };

    using KernelAllocAligned_t   = void*(__fastcall*)(std::uint64_t size, std::uint64_t align, std::uint32_t anno);
    using ArrayBaseFree_t        = void(__fastcall*)(void* p, std::uint32_t anno);
    using GetQuarkSystemTable_t  = std::uint8_t*(__fastcall*)();
    using SetUpLinearAccessor_t  = void(__fastcall*)(std::uint8_t* self, void* accessor, const float* pos, float radius, std::uint32_t groupMask);
    using PickUp_t               = void(__fastcall*)(std::uint8_t* iface, std::uint32_t index);
    using RepopCountOperation_t  = int(__fastcall*)(std::uint8_t* execIface, lua_State* L);
    using GetCollectionInfo_t    = std::uint8_t(__fastcall*)(std::uint8_t* iface, std::uint32_t index, std::uint8_t* out, std::uint8_t checkPicked);
    using GetModelFilePath_t     = std::uint64_t*(__fastcall*)(std::uint64_t* sret, std::uint8_t type);
    using GetCatchEffect_t       = void(__fastcall*)(std::uint8_t type, float* color, float* strength, float* yOffset);
    using GetGroundEffectSize_t  = void(__fastcall*)(std::uint8_t type, float* out);
    using DoPickUpCollection_t   = void(__fastcall*)(std::uint8_t* self, std::uint32_t playerId);
    using GetModelFilePathRoot_t = std::uint64_t*(__fastcall*)(std::uint64_t* sret, std::uint8_t type);
    using GetFovaFilePath_t      = std::uint64_t*(__fastcall*)(std::uint64_t* sret, std::uint8_t type);
    using TypePredicate_t        = int(__fastcall*)(lua_State* L);
    using GetIconFtexPath_t      = std::uint64_t*(__fastcall*)(std::uint64_t* sret, std::uint32_t type, std::uint8_t kind);
    using GetNameText_t          = const char*(__fastcall*)(std::uint32_t type, std::uint8_t kind);
    using GetUixUtility_t        = void*(__fastcall*)();
    using LangTextResolve_t      = const char*(__fastcall*)(void* self, std::uint64_t langId);

    static SetUpLinearAccessor_t g_OrigSetUpLinearAccessor = nullptr;
    static PickUp_t              g_OrigPickUp              = nullptr;
    static RepopCountOperation_t g_OrigRepopCountOperation = nullptr;
    static GetCollectionInfo_t   g_OrigGetCollectionInfo   = nullptr;
    static GetModelFilePath_t    g_OrigGetModelFilePath    = nullptr;
    static GetCatchEffect_t      g_OrigGetCatchEffect      = nullptr;
    static GetGroundEffectSize_t g_OrigGetGroundEffectSize = nullptr;
    static DoPickUpCollection_t  g_OrigDoPickUpCollection  = nullptr;
    static GetIconFtexPath_t     g_OrigGetIconFtexPath     = nullptr;
    static GetNameText_t         g_OrigGetNameText         = nullptr;
    static GetModelFilePathRoot_t g_OrigGetModelFilePathRoot = nullptr;
    static GetFovaFilePath_t      g_OrigGetFovaFilePath      = nullptr;
    static TypePredicate_t        g_OrigIsHerbByType         = nullptr;
    static TypePredicate_t        g_OrigIsMaterialByType     = nullptr;
    static TypePredicate_t        g_OrigIsDiamondByType      = nullptr;
    static constexpr std::size_t kQuarkTableApplicationSystem = 0x98;
    static constexpr std::size_t kApplicationSystemCollection = 0x1F8;
    static constexpr std::size_t kUixUtilityLangTextVtbl      = 0x750;

    static bool g_LangResolveFaultLogged = false;
    static const char* ResolveLangText(std::uint64_t langId)
    {
        if (!langId || !gAddr.UixUtility_GetUixUtility)
            return nullptr;

        auto getter = reinterpret_cast<GetUixUtility_t>(
            ResolveGameAddress(gAddr.UixUtility_GetUixUtility));
        if (!getter)
            return nullptr;

        void* self = getter();
        if (!self)
            return nullptr;

        void** vtbl = *reinterpret_cast<void***>(self);
        if (!vtbl)
            return nullptr;

        auto fn = reinterpret_cast<LangTextResolve_t>(
            vtbl[kUixUtilityLangTextVtbl / sizeof(void*)]);
        if (!fn)
            return nullptr;

        return fn(self, langId);
    }

    enum class InstallState
    {
        NotAttempted,
        Installed,
        Failed,
    };

    static std::recursive_mutex g_Mutex;

    static bool g_HooksInstalled = false;
    static InstallState g_TypeHooks = InstallState::NotAttempted;

    static std::vector<CustomReg> g_Regs;
    static std::vector<CustomType> g_Types;
    static bool g_RegsDirty = false;

    static std::uint8_t*        g_Arr = nullptr;
    static void*                g_SeenInfoData = nullptr;
    static bool                 g_HaveVanilla = false;
    static bool                 g_CaptureFailed = false;
    static std::uint32_t        g_VanillaCount = 0;
    static VanillaSnapshot      g_Van;
    static std::vector<TailRow> g_Tail;
    static std::uint8_t         g_MergeGroupId = 0;
    static std::uint32_t        g_SeenMasks[8] = {};
    static std::uint32_t        g_SeenMaskCount = 0;
    static bool g_CustomsInstalled = false;
    static bool g_CustomsBlocked = false;
    static bool g_MirrorFaultLogged = false;

    static FoxArrayHdr* Hdr(std::uint8_t* arr, std::size_t off)
    {
        return reinterpret_cast<FoxArrayHdr*>(arr + off);
    }

    static std::uint8_t* GetCollectionSystemRaw()
    {
        auto getTable = reinterpret_cast<GetQuarkSystemTable_t>(ResolveGameAddress(gAddr.GetQuarkSystemTable));
        if (!getTable)
            return nullptr;

        std::uint8_t* table = getTable();
        if (!table)
            return nullptr;

        std::uint8_t* app = *reinterpret_cast<std::uint8_t**>(table + kQuarkTableApplicationSystem);
        if (!app)
            return nullptr;

        std::uint8_t* iface = *reinterpret_cast<std::uint8_t**>(app + kApplicationSystemCollection);
        if (!iface)
            return nullptr;

        return iface - 0x20;
    }

    enum class Residency { Unavailable, Missing, Resident };

    static Residency QueryModelResidency(std::uint64_t pathId, std::uint64_t* outHandle)
    {
        if (outHandle)
            *outHandle = 0;
        if (!pathId)
            return Residency::Unavailable;

        std::uint8_t* self = GetCollectionSystemRaw();
        if (!self)
            return Residency::Unavailable;

        __try
        {
            std::uint8_t* iface = *reinterpret_cast<std::uint8_t**>(self + 0x68);
            if (!iface)
                return Residency::Unavailable;

            void** ivt = *reinterpret_cast<void***>(iface);
            if (!ivt || !ivt[2] || !ivt[3])
                return Residency::Unavailable;

            using GetProvider_t = std::uint8_t* (__fastcall*)(std::uint8_t*);
            using GetBlock_t    = std::uint64_t (__fastcall*)(std::uint8_t*);
            using Lookup_t      = std::uint64_t (__fastcall*)(std::uint8_t*, std::uint64_t, std::uint64_t);

            std::uint8_t* provider = reinterpret_cast<GetProvider_t>(ivt[2])(iface);
            const std::uint64_t blk = reinterpret_cast<GetBlock_t>(ivt[3])(iface);
            if (!provider)
                return Residency::Unavailable;

            void** pvt = *reinterpret_cast<void***>(provider);
            if (!pvt || !pvt[6])
                return Residency::Unavailable;

            const std::uint64_t handle =
                reinterpret_cast<Lookup_t>(pvt[6])(provider, pathId, blk);
            if (outHandle)
                *outHandle = handle;
            return handle ? Residency::Resident : Residency::Missing;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return Residency::Unavailable;
        }
    }

    static std::uint32_t CompressYawQuat(float yawDeg)
    {
        const float half = yawDeg * 0.01745329252f * 0.5f;
        float y = sinf(half);
        float w = cosf(half);
        if (w < 0.f) { y = -y; w = -w; }

        std::uint32_t signs = 0;
        if (y < 0.f) { y = -y; signs |= 2; }
        if (y < 1e-6f)
            return 1023u;

        const std::uint32_t wOut = static_cast<std::uint32_t>(floorf(acosf(w) * 2.0f * 0.31830987f * 511.0f + 0.5f)) & 0x3FF;
        return 1023u | (wOut << 20) | (signs << 29);
    }

    static bool SegmentFromXZ(float x, float z, std::uint16_t& outSeg)
    {
        const int cx = static_cast<int>(floorf(x * (1.0f / 64.0f))) + 64;
        const int cz = static_cast<int>(floorf(z * (1.0f / 64.0f))) + 64;
        if (cx < 0 || cx > 127 || cz < 0 || cz > 127)
            return false;

        outSeg = static_cast<std::uint16_t>(cx + (cz << 7));
        return true;
    }

    static std::uint64_t ResolvePathArg(const char* s)
    {
        if (!s || !*s)
            return 0;

        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s[2])
        {
            std::uint64_t v = 0;
            for (const char* p = s + 2; *p; ++p)
            {
                std::uint64_t d;
                if (*p >= '0' && *p <= '9')      d = static_cast<std::uint64_t>(*p - '0');
                else if (*p >= 'a' && *p <= 'f') d = static_cast<std::uint64_t>(*p - 'a' + 10);
                else if (*p >= 'A' && *p <= 'F') d = static_cast<std::uint64_t>(*p - 'A' + 10);
                else return FoxHashes::PathCode64Ext(s);
                v = (v << 4) | d;
            }
            return v;
        }

        return FoxHashes::PathCode64Ext(s);
    }

    static const CustomType* FindCustomType(std::uint32_t typeId)
    {
        for (const CustomType& t : g_Types)
            if (t.typeId == typeId)
                return &t;
        return nullptr;
    }

    static CustomType* FindCustomTypeMutable(std::uint32_t typeId)
    {
        for (CustomType& t : g_Types)
            if (t.typeId == typeId)
                return &t;
        return nullptr;
    }

    static CustomType* FindCustomTypeByName(const char* typeName)
    {
        if (!typeName || !*typeName)
            return nullptr;
        for (CustomType& t : g_Types)
            if (t.name == typeName)
                return &t;
        return nullptr;
    }

    static CustomType* ResolveTypeArg(const char* typeName, std::int32_t typeId)
    {
        if (typeName && *typeName)
            return FindCustomTypeByName(typeName);

        if (typeId >= 0)
            for (CustomType& t : g_Types)
                if (t.typeId == static_cast<std::uint32_t>(typeId))
                    return &t;

        return nullptr;
    }

    static void LogTypeArg(char* out, std::size_t cap, const char* typeName, std::int32_t typeId)
    {
        if (typeName && *typeName)
            std::snprintf(out, cap, "'%s'", typeName);
        else
            std::snprintf(out, cap, "id %d", typeId);
    }

    static std::int32_t AllocateTypeId()
    {
        for (std::uint32_t id = 0x37; id <= 0x63; ++id)
            if (!FindCustomType(id))
                return static_cast<std::int32_t>(id);
        for (std::uint32_t id = 0x6E; id <= 0x81; ++id)
            if (!FindCustomType(id))
                return static_cast<std::int32_t>(id);
        return -1;
    }

    static bool IsAllowedType(std::uint32_t typeId)
    {
        if (typeId >= 0x01 && typeId <= 0x36)
            return true;
        if (typeId >= 0x64 && typeId <= 0x6D)
            return true;
        if (FindCustomType(typeId) != nullptr)
            return true;
        return false;
    }

    static bool CaptureVanilla(std::uint8_t* arr)
    {
        FoxArrayHdr* pos = Hdr(arr, kOffPositions);
        FoxArrayHdr* rot = Hdr(arr, kOffRotations);
        FoxArrayHdr* inf = Hdr(arr, kOffInfos);
        FoxArrayHdr* seg = Hdr(arr, kOffSegIndices);
        FoxArrayHdr* li  = Hdr(arr, kOffLocIndices);
        FoxArrayHdr* lc  = Hdr(arr, kOffLocCounts);
        FoxArrayHdr* gi  = Hdr(arr, kOffGroupIds);
        FoxArrayHdr* sii = Hdr(arr, kOffSegInfoIndices);
        FoxArrayHdr* sc  = Hdr(arr, kOffSegCounts);

        const std::uint32_t n = inf->count;
        if (pos->count != n || rot->count != n ||
            seg->count != li->count || seg->count != lc->count ||
            gi->count != sii->count || gi->count != sc->count)
        {
            Log("[TppCollection] locator array %p has inconsistent counts (flat "
                "%u/%u/%u seg %u/%u/%u grp %u/%u/%u) - custom collectibles disabled "
                "here\n",
                arr, pos->count, rot->count, inf->count, seg->count, li->count, lc->count,
                gi->count, sii->count, sc->count);
            return false;
        }

        auto copyBytes = [](std::vector<std::uint8_t>& dst, const FoxArrayHdr* h, std::size_t elem)
        {
            dst.assign(static_cast<std::size_t>(h->count) * elem, 0);
            if (h->count && h->data)
                std::memcpy(dst.data(), h->data, dst.size());
        };
        auto copy16 = [](std::vector<std::uint16_t>& dst, const FoxArrayHdr* h)
        {
            dst.assign(h->count, 0);
            if (h->count && h->data)
                std::memcpy(dst.data(), h->data, static_cast<std::size_t>(h->count) * 2);
        };
        auto copy32 = [](std::vector<std::uint32_t>& dst, const FoxArrayHdr* h)
        {
            dst.assign(h->count, 0);
            if (h->count && h->data)
                std::memcpy(dst.data(), h->data, static_cast<std::size_t>(h->count) * 4);
        };

        copyBytes(g_Van.pos, pos, 16);
        copy32(g_Van.rot, rot);
        copy32(g_Van.inf, inf);
        copy16(g_Van.segIdx, seg);
        copy16(g_Van.locIdx, li);
        copy16(g_Van.locCnt, lc);
        copy16(g_Van.segInfoIdx, sii);
        copy16(g_Van.segCnt, sc);
        g_Van.groupIds.assign(gi->count, 0);
        if (gi->count && gi->data)
            std::memcpy(g_Van.groupIds.data(), gi->data, gi->count);

        for (std::size_t s2 = 0; s2 < g_Van.locIdx.size(); ++s2)
        {
            if (static_cast<std::uint32_t>(g_Van.locIdx[s2]) + g_Van.locCnt[s2] > n)
            {
                Log("[TppCollection] locator array %p segment %zu spans rows %u..%u "
                    "past the flat table (%u rows) - custom collectibles disabled "
                    "here\n",
                    arr, s2, g_Van.locIdx[s2], g_Van.locIdx[s2] + g_Van.locCnt[s2], n);
                return false;
            }
        }
        for (std::size_t g2 = 0; g2 < g_Van.segInfoIdx.size(); ++g2)
        {
            if (static_cast<std::uint32_t>(g_Van.segInfoIdx[g2]) + g_Van.segCnt[g2] > g_Van.segIdx.size())
            {
                Log("[TppCollection] locator array %p group %zu spans segment "
                    "entries past the segment table - custom collectibles disabled "
                    "here\n",
                    arr, g2);
                return false;
            }
        }

        g_VanillaCount = n;
        return true;
    }

    static bool InstallArrays(std::uint8_t* arr,
                              const std::vector<std::uint8_t>& pos,
                              const std::vector<std::uint32_t>& rot,
                              const std::vector<std::uint32_t>& inf,
                              const std::vector<std::uint16_t>& segIdx,
                              const std::vector<std::uint16_t>& locIdx,
                              const std::vector<std::uint16_t>& locCnt,
                              const std::vector<std::uint8_t>& groupIds,
                              const std::vector<std::uint16_t>& segInfoIdx,
                              const std::vector<std::uint16_t>& segCnt)
    {
        auto alloc = reinterpret_cast<KernelAllocAligned_t>(ResolveGameAddress(gAddr.KernelAllocAligned));
        auto release = reinterpret_cast<ArrayBaseFree_t>(ResolveGameAddress(gAddr.ArrayBaseFree));
        if (!alloc || !release)
        {
            Log("[TppCollection] KernelAllocAligned/ArrayBaseFree unavailable - cannot rebuild locator arrays\n");
            return false;
        }

        struct Pending
        {
            std::size_t   off;
            const void*   src;
            std::uint32_t count;
            std::uint32_t elem;
            std::uint32_t align;
            void*         buf;
        };

        Pending items[9] =
        {
            { kOffPositions,      pos.data(),        static_cast<std::uint32_t>(pos.size() / 16), 16, 16, nullptr },
            { kOffRotations,      rot.data(),        static_cast<std::uint32_t>(rot.size()),       4,  4, nullptr },
            { kOffInfos,          inf.data(),        static_cast<std::uint32_t>(inf.size()),       4,  4, nullptr },
            { kOffSegIndices,     segIdx.data(),     static_cast<std::uint32_t>(segIdx.size()),    2,  2, nullptr },
            { kOffLocIndices,     locIdx.data(),     static_cast<std::uint32_t>(locIdx.size()),    2,  2, nullptr },
            { kOffLocCounts,      locCnt.data(),     static_cast<std::uint32_t>(locCnt.size()),    2,  2, nullptr },
            { kOffGroupIds,       groupIds.data(),   static_cast<std::uint32_t>(groupIds.size()),  1,  1, nullptr },
            { kOffSegInfoIndices, segInfoIdx.data(), static_cast<std::uint32_t>(segInfoIdx.size()),2,  2, nullptr },
            { kOffSegCounts,      segCnt.data(),     static_cast<std::uint32_t>(segCnt.size()),    2,  2, nullptr },
        };

        for (Pending& it : items)
        {
            if (!it.count)
                continue;

            it.buf = alloc(static_cast<std::uint64_t>(it.count) * it.elem, it.align, kAllocAnnotation);
            if (!it.buf)
            {
                Log("[TppCollection] engine allocation failed (%u bytes) - locator rebuild aborted\n",
                    it.count * it.elem);
                for (Pending& done : items)
                    if (done.buf)
                        release(done.buf, kAllocAnnotation);
                return false;
            }
            std::memcpy(it.buf, it.src, static_cast<std::size_t>(it.count) * it.elem);
        }

        for (Pending& it : items)
        {
            FoxArrayHdr* h = Hdr(arr, it.off);
            void* old = h->data;
            h->data = it.buf;
            h->count = it.count;
            h->capacity = it.count;
            if (old)
                release(old, kAllocAnnotation);
        }

        g_SeenInfoData = Hdr(arr, kOffInfos)->data;
        return true;
    }

    static bool RebuildArrays(std::uint8_t* arr, bool wantCustoms)
    {
        g_Tail.clear();

        if (!wantCustoms)
        {
            const bool ok = InstallArrays(arr, g_Van.pos, g_Van.rot, g_Van.inf,
                                          g_Van.segIdx, g_Van.locIdx, g_Van.locCnt,
                                          g_Van.groupIds, g_Van.segInfoIdx, g_Van.segCnt);
            g_CustomsInstalled = false;
            return ok;
        }

        std::vector<std::size_t> order;
        order.reserve(g_Regs.size());
        for (std::size_t i = 0; i < g_Regs.size(); ++i)
            order.push_back(i);
        std::sort(order.begin(), order.end(), [](std::size_t a, std::size_t b)
        {
            return g_Regs[a].uid < g_Regs[b].uid;
        });

        std::map<std::uint16_t, std::vector<std::size_t>> buckets;
        for (std::size_t ri : order)
        {
            const CustomReg& reg = g_Regs[ri];

            bool uidClash = false;
            for (std::uint32_t v : g_Van.inf)
            {
                if ((v & 0xFFFFFF) == reg.uid)
                {
                    uidClash = true;
                    break;
                }
            }
            if (uidClash)
            {
                Log("[TppCollection] '%s' uniqueId 0x%06X collides with a vanilla collectible - entry skipped\n",
                    reg.name.c_str(), reg.uid);
                continue;
            }

            std::uint16_t seg = 0;
            if (!SegmentFromXZ(reg.x, reg.z, seg))
            {
                Log("[TppCollection] '%s' position (%.1f, %.1f) is outside the 128x128 segment grid - entry skipped\n",
                    reg.name.c_str(), reg.x, reg.z);
                continue;
            }

            buckets[seg].push_back(ri);
        }

        std::vector<std::uint8_t>  outPos = g_Van.pos;
        std::vector<std::uint32_t> outRot = g_Van.rot;
        std::vector<std::uint32_t> outInf = g_Van.inf;
        std::vector<std::uint16_t> outSegIdx, outLocIdx, outLocCnt, outSegInfoIdx, outSegCnt;
        std::vector<std::uint8_t>  outGroups;

        auto appendVanillaRow = [&](std::uint32_t srcIdx)
        {
            const std::uint32_t j = static_cast<std::uint32_t>(outInf.size());
            outPos.insert(outPos.end(),
                          g_Van.pos.begin() + static_cast<std::size_t>(srcIdx) * 16,
                          g_Van.pos.begin() + static_cast<std::size_t>(srcIdx) * 16 + 16);
            outRot.push_back(g_Van.rot[srcIdx]);
            outInf.push_back(g_Van.inf[srcIdx]);
            TailRow t;
            t.uid = g_Van.inf[srcIdx] & 0xFFFFFF;
            t.canonical = static_cast<std::int32_t>(srcIdx);
            g_Tail.push_back(t);
            return j;
        };

        auto appendCustomRow = [&](std::size_t ri)
        {
            const CustomReg& reg = g_Regs[ri];
            const std::uint32_t j = static_cast<std::uint32_t>(outInf.size());
            float v4[4] = { reg.x, reg.y, reg.z, 0.f };
            const std::uint8_t* pb = reinterpret_cast<const std::uint8_t*>(v4);
            outPos.insert(outPos.end(), pb, pb + 16);
            outRot.push_back(CompressYawQuat(reg.rotY));
            outInf.push_back((static_cast<std::uint32_t>(reg.type) << 24) | reg.uid);
            TailRow t;
            t.uid = reg.uid;
            t.regIndex = static_cast<std::int32_t>(ri);
            g_Tail.push_back(t);
            return j;
        };

        const std::size_t groupCount = g_Van.groupIds.size();

        std::size_t mergeGroup = SIZE_MAX;
        std::uint32_t mergeGroupRows = 0;
        for (std::size_t g = 0; g < groupCount; ++g)
        {
            std::uint32_t rows = 0;
            const std::size_t s0 = g_Van.segInfoIdx[g];
            const std::size_t s1 = s0 + g_Van.segCnt[g];
            for (std::size_t s = s0; s < s1; ++s)
                rows += g_Van.locCnt[s];
            if (rows > mergeGroupRows)
            {
                mergeGroupRows = rows;
                mergeGroup = g;
            }
        }
        g_MergeGroupId = (mergeGroup == SIZE_MAX) ? 0 : g_Van.groupIds[mergeGroup];

        auto emitGroup = [&](std::uint8_t groupId, std::size_t vanillaGroupIndex, bool mergeCustoms)
        {
            const std::uint16_t entryStart = static_cast<std::uint16_t>(outSegIdx.size());

            std::size_t vs = 0, ve = 0;
            if (vanillaGroupIndex != SIZE_MAX)
            {
                vs = g_Van.segInfoIdx[vanillaGroupIndex];
                ve = vs + g_Van.segCnt[vanillaGroupIndex];
            }

            auto bucketIt = mergeCustoms ? buckets.begin() : buckets.end();
            const auto bucketEnd = buckets.end();

            std::size_t s = vs;
            while (s < ve || bucketIt != bucketEnd)
            {
                const bool haveVan = s < ve;
                const std::uint16_t vanSeg = haveVan ? g_Van.segIdx[s] : 0;
                const bool haveCus = bucketIt != bucketEnd;
                const std::uint16_t cusSeg = haveCus ? bucketIt->first : 0;

                if (haveVan && (!haveCus || vanSeg < cusSeg))
                {
                    outSegIdx.push_back(vanSeg);
                    outLocIdx.push_back(g_Van.locIdx[s]);
                    outLocCnt.push_back(g_Van.locCnt[s]);
                    ++s;
                }
                else if (haveVan && haveCus && vanSeg == cusSeg)
                {
                    const std::uint32_t newStart = static_cast<std::uint32_t>(outInf.size());
                    const std::uint16_t runStart = g_Van.locIdx[s];
                    const std::uint16_t runCount = g_Van.locCnt[s];
                    for (std::uint16_t r = 0; r < runCount; ++r)
                        appendVanillaRow(static_cast<std::uint32_t>(runStart) + r);
                    for (std::size_t ri : bucketIt->second)
                        appendCustomRow(ri);

                    outSegIdx.push_back(vanSeg);
                    outLocIdx.push_back(static_cast<std::uint16_t>(newStart));
                    outLocCnt.push_back(static_cast<std::uint16_t>(runCount + bucketIt->second.size()));
                    ++s;
                    ++bucketIt;
                }
                else
                {
                    const std::uint32_t newStart = static_cast<std::uint32_t>(outInf.size());
                    for (std::size_t ri : bucketIt->second)
                        appendCustomRow(ri);

                    outSegIdx.push_back(cusSeg);
                    outLocIdx.push_back(static_cast<std::uint16_t>(newStart));
                    outLocCnt.push_back(static_cast<std::uint16_t>(bucketIt->second.size()));
                    ++bucketIt;
                }
            }

            outGroups.push_back(groupId);
            outSegInfoIdx.push_back(entryStart);
            outSegCnt.push_back(static_cast<std::uint16_t>(outSegIdx.size() - entryStart));
        };

        for (std::size_t g = 0; g < groupCount; ++g)
            emitGroup(g_Van.groupIds[g], g, g == mergeGroup);

        if (mergeGroup == SIZE_MAX && !buckets.empty())
            emitGroup(0, SIZE_MAX, true);

        if (outInf.size() > kMaxLocatorRows)
        {
            Log("[TppCollection] rebuilt locator table would hold %zu rows (max %u) - customs dropped, vanilla restored\n",
                outInf.size(), kMaxLocatorRows);
            g_CustomsBlocked = true;
            return RebuildArrays(arr, false);
        }

        const bool ok = InstallArrays(arr, outPos, outRot, outInf,
                                      outSegIdx, outLocIdx, outLocCnt,
                                      outGroups, outSegInfoIdx, outSegCnt);
        if (!ok)
        {
            g_Tail.clear();
            g_CustomsInstalled = false;
            g_CustomsBlocked = true;
            return false;
        }

        g_CustomsInstalled = true;
        for (CustomType& ct : g_Types)
        {
            ct.standInLogged = 0;
            ct.standInPathId = 0;
            ct.drawPathId    = 0;
        }

        LogDebug("[TppCollection] locator array %p rebuilt: %u vanilla + %zu tail rows (%zu custom)\n",
                 arr, g_VanillaCount, g_Tail.size(),
                 static_cast<std::size_t>(std::count_if(g_Tail.begin(), g_Tail.end(),
                     [](const TailRow& t) { return t.regIndex >= 0; })));

        LogDebug("[TppCollection] customs merged into group id %u (%u vanilla locator(s) in it); "
                 "the realizer only walks group N when bit N of its groupMask is set\n",
                 g_MergeGroupId, mergeGroupRows);
        for (std::size_t t = 0; t < g_Tail.size(); ++t)
        {
            if (g_Tail[t].regIndex < 0)
                continue;
            const CustomReg& reg = g_Regs[static_cast<std::size_t>(g_Tail[t].regIndex)];
            std::uint16_t seg = 0;
            SegmentFromXZ(reg.x, reg.z, seg);
            const CustomType* ct = FindCustomType(reg.type);
            LogDebug("[TppCollection]   '%s' type=%u uid=0x%06X row=%zu segment=%u "
                     "pos=(%.1f, %.1f, %.1f) model=0x%016llX\n",
                     reg.name.c_str(), reg.type, reg.uid,
                     g_VanillaCount + t, seg, reg.x, reg.y, reg.z,
                     static_cast<unsigned long long>(ct ? ct->modelPathId : 0));
        }
        return true;
    }

    static void EnsureCustomState(std::uint8_t* arr)
    {
        const FoxArrayHdr* infos = Hdr(arr, kOffInfos);

        bool newInstance = (arr != g_Arr || infos->data != g_SeenInfoData);

        if (!newInstance && g_CustomsInstalled &&
            infos->count != g_VanillaCount + static_cast<std::uint32_t>(g_Tail.size()))
            newInstance = true;

        if (newInstance)
        {
            g_Arr = arr;
            g_SeenInfoData = infos->data;
            g_HaveVanilla = false;
            g_CaptureFailed = false;
            g_Tail.clear();
            g_CustomsInstalled = false;
            g_CustomsBlocked = false;
            g_MirrorFaultLogged = false;
            g_RegsDirty = true;
        }

        if (g_RegsDirty)
            g_CustomsBlocked = false;

        const bool wantCustoms = !g_Regs.empty() && !g_CustomsBlocked && !g_CaptureFailed &&
                                 !MissionCodeGuard::ShouldBypassHooks();

        if (!wantCustoms && !g_CustomsInstalled)
        {
            g_RegsDirty = false;
            return;
        }

        if (!g_HaveVanilla)
        {
            if (!CaptureVanilla(arr))
            {
                g_CaptureFailed = true;
                g_RegsDirty = false;
                return;
            }
            g_HaveVanilla = true;
        }

        if (g_RegsDirty || wantCustoms != g_CustomsInstalled)
        {
            g_RegsDirty = false;
            RebuildArrays(arr, wantCustoms);
        }
    }

    static void MirrorHiddenBitsUnsafe(std::uint8_t* hidden)
    {
        for (std::size_t k = 0; k < g_Tail.size(); ++k)
        {
            const std::uint32_t j = g_VanillaCount + static_cast<std::uint32_t>(k);
            const TailRow& t = g_Tail[k];
            if (t.canonical >= 0)
            {
                const std::uint32_t i = static_cast<std::uint32_t>(t.canonical);
                const bool bi = (hidden[i >> 3] & (1u << (i & 7))) != 0;
                const bool bj = (hidden[j >> 3] & (1u << (j & 7))) != 0;
                if (bi && !bj)
                    hidden[j >> 3] |= static_cast<std::uint8_t>(1u << (j & 7));
                else if (bj && !bi)
                    hidden[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
            }
            else if (t.regIndex >= 0 && t.regIndex < static_cast<std::int32_t>(g_Regs.size()) &&
                     g_Regs[t.regIndex].repop != 0)
            {
                hidden[j >> 3] |= static_cast<std::uint8_t>(1u << (j & 7));
            }
        }
    }

    static void MirrorHiddenBits()
    {
        if (!g_CustomsInstalled || g_Tail.empty())
            return;

        std::uint8_t* raw = GetCollectionSystemRaw();
        if (!raw)
            return;

        __try
        {
            std::uint8_t* hidden = *reinterpret_cast<std::uint8_t**>(raw + 0xA0);
            if (hidden)
                MirrorHiddenBitsUnsafe(hidden);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_MirrorFaultLogged)
            {
                g_MirrorFaultLogged = true;
                Log("[TppCollection] collection state fault while mirroring picked "
                    "bits - picked-state sync suspended here\n");
            }
        }
    }

    static std::int32_t FindRowByUid(std::uint32_t uid)
    {
        if (!g_Arr)
            return -1;

        const FoxArrayHdr* inf = Hdr(g_Arr, kOffInfos);
        const std::uint32_t* data = static_cast<const std::uint32_t*>(inf->data);
        if (!data)
            return -1;

        for (std::uint32_t i = 0; i < inf->count; ++i)
            if ((data[i] & 0xFFFFFF) == uid)
                return static_cast<std::int32_t>(i);

        return -1;
    }

    static void __fastcall Hook_SetUpLinearAccessor(std::uint8_t* self, void* accessor,
                                                    const float* pos, float radius,
                                                    std::uint32_t groupMask)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            EnsureCustomState(self);
            MirrorHiddenBits();

            if (g_CustomsInstalled && g_SeenMaskCount < 8)
            {
                bool known = false;
                for (std::uint32_t i = 0; i < g_SeenMaskCount; ++i)
                    if (g_SeenMasks[i] == groupMask)
                    {
                        known = true;
                        break;
                    }
                if (!known)
                {
                    g_SeenMasks[g_SeenMaskCount++] = groupMask;
                    LogDebug("[TppCollection] realizer groupMask=0x%08X radius=%.1f at "
                             "(%.1f, %.1f, %.1f) - customs are in group id %u, so bit %u must be "
                             "set for them to be enumerated: %s\n",
                             groupMask, radius,
                             pos ? pos[0] : 0.f, pos ? pos[1] : 0.f, pos ? pos[2] : 0.f,
                             g_MergeGroupId, g_MergeGroupId,
                             ((groupMask >> g_MergeGroupId) & 1u) ? "SET (customs are walked)"
                                                                  : "CLEAR (customs are skipped)");
                }
            }
        }
        g_OrigSetUpLinearAccessor(self, accessor, pos, radius, groupMask);
    }

    static void __fastcall Hook_PickUp(std::uint8_t* iface, std::uint32_t index)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        if (!g_CustomsInstalled || index < g_VanillaCount)
        {
            g_OrigPickUp(iface, index);
            return;
        }

        const std::size_t k = index - g_VanillaCount;
        if (k >= g_Tail.size())
        {
            g_OrigPickUp(iface, index);
            return;
        }

        void* svarRepop = *reinterpret_cast<void**>(iface + 0x70);
        void* svarFlags = *reinterpret_cast<void**>(iface + 0x108);
        *reinterpret_cast<void**>(iface + 0x70) = nullptr;
        *reinterpret_cast<void**>(iface + 0x108) = nullptr;
        g_OrigPickUp(iface, index);
        *reinterpret_cast<void**>(iface + 0x70) = svarRepop;
        *reinterpret_cast<void**>(iface + 0x108) = svarFlags;

        const TailRow& t = g_Tail[k];
        if (t.canonical >= 0)
        {
            const std::uint32_t i = static_cast<std::uint32_t>(t.canonical);
            const std::uint8_t type = static_cast<std::uint8_t>(g_Van.inf[i] >> 24);

            if (svarRepop && type != 6)
            {
                const std::uint16_t cnt = *reinterpret_cast<std::uint16_t*>(iface + 0x78);
                const std::uint16_t off = *reinterpret_cast<std::uint16_t*>(iface + 0x7A);
                if (i < cnt)
                    static_cast<std::uint8_t*>(svarRepop)[off + i] = 0xFF;
            }

            if (svarFlags)
            {
                const std::uint16_t cnt = *reinterpret_cast<std::uint16_t*>(iface + 0x110);
                const std::uint16_t off = *reinterpret_cast<std::uint16_t*>(iface + 0x112);
                if (i < cnt)
                {
                    const std::uint32_t bit = static_cast<std::uint32_t>(off) + i;
                    static_cast<std::uint8_t*>(svarFlags)[bit >> 3] |= static_cast<std::uint8_t>(1u << (bit & 7));
                }
            }

            std::uint8_t* hidden = *reinterpret_cast<std::uint8_t**>(iface + 0x80);
            if (hidden)
                hidden[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        }
        else if (t.regIndex >= 0 && t.regIndex < static_cast<std::int32_t>(g_Regs.size()))
        {
            g_Regs[t.regIndex].repop = 0xFF;
        }
    }

    static void ApplyRepopByte(std::uint32_t op, std::uint8_t* b, double v, lua_State* L)
    {
        if (op == kOpSetAt)
        {
            *b = static_cast<std::uint8_t>(static_cast<std::int64_t>(v));
        }
        else if (op == kOpDecAt)
        {
            const std::uint8_t d = static_cast<std::uint8_t>(static_cast<std::int64_t>(v));
            *b = (*b < d) ? 0 : static_cast<std::uint8_t>(*b - d);
        }
        else if (op == kOpGetAt)
        {
            PushLuaNumber(L, static_cast<float>(*b));
        }
    }

    static int __fastcall Hook_RepopCountOperation(std::uint8_t* execIface, lua_State* L)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        if (!g_CustomsInstalled || !g_Arr || !ResolveLuaApi())
            return g_OrigRepopCountOperation(execIface, L);

        const char* opName = GetLuaString(L, 1);
        const std::uint32_t op = opName ? FoxHashes::StrCode32(opName) : 0;

        if (op == kOpSetAt || op == kOpDecAt || op == kOpGetAt)
        {
            std::uint32_t uid = 0;
            if (LuaIsNumber(L, 2))
            {
                uid = static_cast<std::uint32_t>(static_cast<std::int64_t>(g_lua_tonumber(L, 2)));
            }
            else
            {
                const char* s = GetLuaString(L, 2);
                uid = s ? (FoxHashes::StrCode32(s) & 0xFFFFFF) : 0;
            }

            const std::int32_t idx = FindRowByUid(uid);
            if (idx >= 0 && static_cast<std::uint32_t>(idx) < g_VanillaCount)
                return g_OrigRepopCountOperation(execIface, L);

            const double v = (op == kOpGetAt) ? 0.0 : g_lua_tonumber(L, 3);

            if (idx >= 0)
            {
                const std::size_t k = static_cast<std::size_t>(idx) - g_VanillaCount;
                if (k < g_Tail.size())
                {
                    const TailRow& t = g_Tail[k];
                    if (t.canonical >= 0)
                    {
                        std::uint8_t* raw = execIface - 0x28;
                        std::uint8_t* svarRepop = *reinterpret_cast<std::uint8_t**>(raw + 0x90);
                        const std::uint16_t cnt = *reinterpret_cast<std::uint16_t*>(raw + 0x98);
                        const std::uint16_t off = *reinterpret_cast<std::uint16_t*>(raw + 0x9A);
                        const std::uint32_t i = static_cast<std::uint32_t>(t.canonical);
                        if (svarRepop && i < cnt)
                        {
                            ApplyRepopByte(op, &svarRepop[off + i], v, L);
                            return (op == kOpGetAt) ? 1 : 0;
                        }
                    }
                    else if (t.regIndex >= 0 && t.regIndex < static_cast<std::int32_t>(g_Regs.size()))
                    {
                        ApplyRepopByte(op, &g_Regs[t.regIndex].repop, v, L);
                        return (op == kOpGetAt) ? 1 : 0;
                    }
                }
            }

            LogDebug("[TppCollection] RepopCount %s: unknown collectible id 0x%06X - request dropped\n",
                     opName ? opName : "?", uid);
            if (op == kOpGetAt)
            {
                PushLuaNumber(L, 0.f);
                return 1;
            }
            return 0;
        }

        FoxArrayHdr* pos = Hdr(g_Arr, kOffPositions);
        FoxArrayHdr* rot = Hdr(g_Arr, kOffRotations);
        FoxArrayHdr* inf = Hdr(g_Arr, kOffInfos);
        const std::uint32_t savePos = pos->count;
        const std::uint32_t saveRot = rot->count;
        const std::uint32_t saveInf = inf->count;
        pos->count = g_VanillaCount;
        rot->count = g_VanillaCount;
        inf->count = g_VanillaCount;
        const int result = g_OrigRepopCountOperation(execIface, L);
        pos->count = savePos;
        rot->count = saveRot;
        inf->count = saveInf;

        if (op == kOpSetAll || op == kOpDecAll || op == kOpSetByType || op == kOpDecByType)
        {
            const bool byType = (op == kOpSetByType || op == kOpDecByType);
            const std::uint8_t typeArg = byType
                ? static_cast<std::uint8_t>(static_cast<std::int64_t>(g_lua_tonumber(L, 2)))
                : 0;
            const std::uint8_t amount =
                static_cast<std::uint8_t>(static_cast<std::int64_t>(g_lua_tonumber(L, byType ? 3 : 2)));

            for (CustomReg& reg : g_Regs)
            {
                if (byType && reg.type != typeArg)
                    continue;
                if (op == kOpSetAll || op == kOpSetByType)
                    reg.repop = amount;
                else
                    reg.repop = (reg.repop < amount) ? 0 : static_cast<std::uint8_t>(reg.repop - amount);
            }
        }

        return result;
    }

    static std::uint8_t __fastcall Hook_GetCollectionInfo(std::uint8_t* iface, std::uint32_t index,
                                                          std::uint8_t* out, std::uint8_t checkPicked)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        if (!g_CustomsInstalled || index < g_VanillaCount)
            return g_OrigGetCollectionInfo(iface, index, out, checkPicked);

        void* svarFlags = *reinterpret_cast<void**>(iface + 0x108);
        *reinterpret_cast<void**>(iface + 0x108) = nullptr;
        const std::uint8_t result = g_OrigGetCollectionInfo(iface, index, out, checkPicked);
        *reinterpret_cast<void**>(iface + 0x108) = svarFlags;

        if (result && out && svarFlags)
        {
            const std::size_t k = index - g_VanillaCount;
            if (k < g_Tail.size() && g_Tail[k].canonical >= 0)
            {
                const std::uint16_t cnt = *reinterpret_cast<std::uint16_t*>(iface + 0x110);
                const std::uint16_t off = *reinterpret_cast<std::uint16_t*>(iface + 0x112);
                const std::uint32_t i = static_cast<std::uint32_t>(g_Tail[k].canonical);
                if (i < cnt)
                {
                    const std::uint32_t bit = static_cast<std::uint32_t>(off) + i;
                    if (static_cast<std::uint8_t*>(svarFlags)[bit >> 3] & (1u << (bit & 7)))
                        out[0x17] |= 0x2;
                }
            }
        }

        return result;
    }

    static std::uint64_t* __fastcall Hook_GetModelFilePath(std::uint64_t* sret, std::uint8_t type)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        CustomType* t = FindCustomTypeMutable(type);
        if (!t)
            return g_OrigGetModelFilePath(sret, type);

        if (t->drawPathId != t->modelPathId)
        {
            std::uint64_t handle = 0;
            const Residency res = QueryModelResidency(t->modelPathId, &handle);

            if (res == Residency::Resident)
            {
                if (t->standInLogged)
                    Log("[TppCollection] '%s' model 0x%016llX has finished loading - dropping the "
                        "stand-in and drawing the real mesh from here on\n",
                        t->name.c_str(), static_cast<unsigned long long>(t->modelPathId));
                t->drawPathId = t->modelPathId;
            }
            else
            {
                if (t->standInPathId == 0)
                {
                    for (std::uint64_t candidate : kEngineModels)
                    {
                        if (QueryModelResidency(candidate, &handle) == Residency::Resident)
                        {
                            t->standInPathId = candidate;
                            break;
                        }
                    }
                }

                if (!t->standInLogged)
                {
                    t->standInLogged = 1;
                    if (t->standInPathId)
                        Log("[TppCollection] '%s' model 0x%016llX is not loaded yet - drawing loaded "
                            "stand-in 0x%016llX meanwhile; residency is re-checked every realize, so "
                            "the real mesh takes over as soon as its fpk mounts\n",
                            t->name.c_str(), static_cast<unsigned long long>(t->modelPathId),
                            static_cast<unsigned long long>(t->standInPathId));
                    else
                        Log("[TppCollection] '%s' model 0x%016llX is not loaded yet and no engine "
                            "collectible model is loaded either, so no stand-in was available - it "
                            "stays pickable with no mesh until the model loads\n",
                            t->name.c_str(), static_cast<unsigned long long>(t->modelPathId));
                }

                t->drawPathId = t->standInPathId;
            }
        }

        *sret = t->drawPathId ? t->drawPathId : t->modelPathId;
        return sret;
    }

    static void __fastcall Hook_GetCatchEffectColorAndScale(std::uint8_t type, float* color,
                                                            float* strength, float* yOffset)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        const CustomType* t = FindCustomType(type);
        if (!t)
        {
            g_OrigGetCatchEffect(type, color, strength, yOffset);
            return;
        }

        color[0] = t->r;
        color[1] = t->g;
        color[2] = t->b;
        color[3] = t->a;
        strength[0] = t->fxStrengthX;
        strength[1] = t->fxStrengthY;
        strength[2] = t->fxStrengthZ;
        strength[3] = t->fxStrengthW;
        *yOffset = t->effectYOffset;
    }

    static std::uint64_t* __fastcall Hook_GetCollectionIconFtexPath(std::uint64_t* sret,
                                                                    std::uint32_t type,
                                                                    std::uint8_t kind)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        const CustomType* t = FindCustomType(type);
        if (!t || !t->iconPathId)
            return g_OrigGetIconFtexPath(sret, type, kind);

        if (sret)
            *sret = t->iconPathId;
        return sret;
    }

    static const char* __fastcall Hook_GetCollectionNameText(std::uint32_t type, std::uint8_t kind)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        const CustomType* t = FindCustomType(type);
        if (!t)
            return g_OrigGetNameText(type, kind);

        if (t->nameLangId)
        {
            if (const char* resolved = ResolveLangText(t->nameLangId))
                return resolved;

            if (!g_LangResolveFaultLogged)
            {
                g_LangResolveFaultLogged = true;
                Log("[TppCollection] lang id 0x%012llX did not resolve to text for type %u - the "
                    "key is missing from the .lng2, or the UixUtility chain is wrong on %s; "
                    "the collectible falls back to its vanilla label\n",
                    static_cast<unsigned long long>(t->nameLangId), type,
                    GetGameBuildName(gGameBuild));
            }
        }

        return g_OrigGetNameText(type, kind);
    }

    static void __fastcall Hook_GetGroundEffectSize(std::uint8_t type, float* out)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        const CustomType* t = FindCustomType(type);
        if (!t)
        {
            g_OrigGetGroundEffectSize(type, out);
            return;
        }

        *out = t->groundEffectSize;
    }

    static std::uint64_t* __fastcall Hook_GetModelFilePathRoot(std::uint64_t* sret, std::uint8_t type)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        const CustomType* t = FindCustomType(type);
        if (!t)
            return g_OrigGetModelFilePathRoot(sret, type);

        *sret = t->rootModelPathId;
        return sret;
    }

    static std::uint64_t* __fastcall Hook_GetFovaFilePath(std::uint64_t* sret, std::uint8_t type)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        const CustomType* t = FindCustomType(type);
        if (!t)
            return g_OrigGetFovaFilePath(sret, type);

        *sret = t->fovaPathId;
        return sret;
    }

    static int AnswerTypePredicate(lua_State* L, TypePredicate_t orig,
                                   std::uint8_t CustomType::*field)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        if (!ResolveLuaApi() || !LuaIsNumber(L, -1))
            return orig(L);

        const double raw = g_lua_tonumber(L, -1);
        if (raw < 0.0 || raw > 255.0)
            return orig(L);

        const CustomType* t = FindCustomType(static_cast<std::uint8_t>(raw));
        if (!t)
            return orig(L);

        PushLuaBool(L, (t->*field) != 0);
        return 1;
    }

    static int __fastcall Hook_IsHerbByType(lua_State* L)
    {
        return AnswerTypePredicate(L, g_OrigIsHerbByType, &CustomType::isHerb);
    }

    static int __fastcall Hook_IsMaterialByType(lua_State* L)
    {
        return AnswerTypePredicate(L, g_OrigIsMaterialByType, &CustomType::isMaterial);
    }

    static int __fastcall Hook_IsDiamondByType(lua_State* L)
    {
        return AnswerTypePredicate(L, g_OrigIsDiamondByType, &CustomType::isDiamond);
    }

    static void __fastcall Hook_DoPickUpCollection(std::uint8_t* self, std::uint32_t playerId)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);

        const std::uint8_t type = *(self + 0xB6);
        const CustomType* customType = FindCustomType(type);
        if (!customType)
        {
            g_OrigDoPickUpCollection(self, playerId);
            return;
        }

        std::uint8_t* iface = *reinterpret_cast<std::uint8_t**>(self + 0x78);
        const std::uint16_t locIdx = *reinterpret_cast<std::uint16_t*>(self + 0xB4);
        const std::uint32_t uid = *reinterpret_cast<std::uint32_t*>(self + 0xB0);

        if (iface)
        {
            auto pickUp = reinterpret_cast<void(__fastcall*)(std::uint8_t*, std::uint32_t)>(
                (*reinterpret_cast<void***>(iface))[6]);
            pickUp(iface, locIdx);
        }

        std::uint8_t* playerObj = *reinterpret_cast<std::uint8_t**>(self + 0x8);
        if (playerObj)
        {
            auto sendSe = reinterpret_cast<void(__fastcall*)(std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t)>(
                (*reinterpret_cast<void***>(playerObj))[1]);
            sendSe(playerObj, playerId, 0x533eb0daU, 2);
        }

        *reinterpret_cast<std::uint32_t*>(self + 0xB0) = 0xFFFFFF;

        V_FrameWork::EmitMessage("Player", "OnPickUpCollection",
                                 playerId & 0x1FFu,
                                 uid,
                                 static_cast<std::uint32_t>(type),
                                 static_cast<std::uint32_t>(customType->nameLangId & 0xFFFFFFFFu));
    }
}

bool TppCollection_AddCustom(const char* name, std::uint32_t typeId,
                             float x, float y, float z, float rotYDeg)
{
    if (!name || !*name)
        return false;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!IsAllowedType(typeId))
    {
        Log("[TppCollection] AddCollection '%s' rejected: type %u is not a supported collectible type\n",
            name, typeId);
        return false;
    }

    std::uint16_t seg = 0;
    if (!SegmentFromXZ(x, z, seg))
    {
        Log("[TppCollection] AddCollection '%s' rejected: position (%.1f, %.1f) is outside the world segment grid\n",
            name, x, z);
        return false;
    }

    const std::uint32_t uid = FoxHashes::StrCode32(name) & 0xFFFFFF;

    for (CustomReg& reg : g_Regs)
    {
        if (reg.uid == uid)
        {
            reg.name = name;
            reg.type = static_cast<std::uint8_t>(typeId);
            reg.x = x;
            reg.y = y;
            reg.z = z;
            reg.rotY = rotYDeg;
            g_RegsDirty = true;
            return true;
        }
    }

    CustomReg reg;
    reg.name = name;
    reg.uid = uid;
    reg.type = static_cast<std::uint8_t>(typeId);
    reg.x = x;
    reg.y = y;
    reg.z = z;
    reg.rotY = rotYDeg;
    g_Regs.push_back(reg);
    g_RegsDirty = true;
    return true;
}

bool TppCollection_RemoveCustom(const char* name)
{
    if (!name || !*name)
        return false;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    const std::uint32_t uid = FoxHashes::StrCode32(name) & 0xFFFFFF;
    for (std::size_t i = 0; i < g_Regs.size(); ++i)
    {
        if (g_Regs[i].uid == uid)
        {
            g_Regs.erase(g_Regs.begin() + i);
            g_RegsDirty = true;
            return true;
        }
    }
    return false;
}

std::int32_t TppCollection_RegisterType(const char* typeName, const TppCollectionTypeDesc& desc)
{
    if (!typeName || !*typeName)
        return -1;

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (g_TypeHooks == InstallState::Failed)
    {
        Log("[TppCollection] RegisterCollectionType '%s' rejected: the type hooks could not be "
            "installed on %s - custom collection types unavailable\n",
            typeName, GetGameBuildName(gGameBuild));
        return -1;
    }

    if (!desc.modelPath || !*desc.modelPath)
    {
        Log("[TppCollection] RegisterCollectionType '%s' rejected: the spec table has no Model "
            "field, so the type would have no mesh to draw\n", typeName);
        return -1;
    }

    const std::uint64_t pathId = ResolvePathArg(desc.modelPath);
    if (!pathId)
    {
        Log("[TppCollection] RegisterCollectionType '%s' rejected: could not hash model path '%s'\n",
            typeName, desc.modelPath);
        return -1;
    }

    const std::uint64_t rootPathId =
        (desc.rootModelPath && *desc.rootModelPath) ? ResolvePathArg(desc.rootModelPath) : 0;
    const std::uint64_t fovaPathId =
        (desc.fovaPath && *desc.fovaPath) ? ResolvePathArg(desc.fovaPath) : 0;

    if (desc.rootModelPath && *desc.rootModelPath && !rootPathId)
        Log("[TppCollection] '%s': could not hash RootModel '%s' - nothing will be left behind "
            "when the collectible is picked\n", typeName, desc.rootModelPath);
    if (desc.fovaPath && *desc.fovaPath && !fovaPathId)
        Log("[TppCollection] '%s': could not hash Fova '%s' - the base mesh renders with no form "
            "variation\n", typeName, desc.fovaPath);

    if (CustomType* existing = FindCustomTypeByName(typeName))
    {
        existing->modelPathId = pathId;
        existing->rootModelPathId = rootPathId;
        existing->fovaPathId = fovaPathId;
        existing->isHerb = desc.isHerb ? 1u : 0u;
        existing->isMaterial = desc.isMaterial ? 1u : 0u;
        existing->isDiamond = desc.isDiamond ? 1u : 0u;
        existing->r = desc.r;
        existing->g = desc.g;
        existing->b = desc.b;
        existing->a = desc.a;
        existing->fxStrengthX = desc.fxStrengthX;
        existing->fxStrengthY = desc.fxStrengthY;
        existing->fxStrengthZ = desc.fxStrengthZ;
        existing->fxStrengthW = desc.fxStrengthW;
        existing->effectYOffset = desc.effectYOffset;
        existing->groundEffectSize = desc.groundEffectSize;
        existing->standInLogged = 0;
        existing->standInPathId = 0;
        existing->drawPathId = 0;
        return static_cast<std::int32_t>(existing->typeId);
    }

    const std::int32_t typeId = AllocateTypeId();
    if (typeId < 0)
    {
        Log("[TppCollection] RegisterCollectionType '%s' rejected: all %u custom type ids are "
            "in use this mission\n", typeName, 0x63u - 0x37u + 1u + 0x81u - 0x6Eu + 1u);
        return -1;
    }

    CustomType t;
    t.name = typeName;
    t.typeId = static_cast<std::uint8_t>(typeId);
    t.modelPathId = pathId;
    t.standInLogged = 0;
    t.standInPathId = 0;
    t.drawPathId = 0;
    t.r = desc.r;
    t.g = desc.g;
    t.b = desc.b;
    t.a = desc.a;
    t.fxStrengthX = desc.fxStrengthX;
    t.fxStrengthY = desc.fxStrengthY;
    t.fxStrengthZ = desc.fxStrengthZ;
    t.fxStrengthW = desc.fxStrengthW;
    t.effectYOffset = desc.effectYOffset;
    t.groundEffectSize = desc.groundEffectSize;
    t.rootModelPathId = rootPathId;
    t.fovaPathId = fovaPathId;
    t.isHerb = desc.isHerb ? 1u : 0u;
    t.isMaterial = desc.isMaterial ? 1u : 0u;
    t.isDiamond = desc.isDiamond ? 1u : 0u;
    g_Types.push_back(t);
    return typeId;
}

bool TppCollection_SetTypeIcon(const char* typeName, std::int32_t typeId, const char* iconPath)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    CustomType* t = ResolveTypeArg(typeName, typeId);
    if (!t)
    {
        char what[128];
        LogTypeArg(what, sizeof(what), typeName, typeId);
        Log("[TppCollection] SetCollectionTypeIcon %s rejected: no custom type by that name or id "
            "- register the type first\n", what);
        return false;
    }

    if (!iconPath || !*iconPath)
    {
        t->iconPathId = 0;
        return true;
    }

    const std::uint64_t pathId = ResolvePathArg(iconPath);
    if (!pathId)
    {
        Log("[TppCollection] SetCollectionTypeIcon: could not hash icon path '%s' for type %u - "
            "the type keeps the generic key-item icon\n", iconPath, t->typeId);
        return false;
    }

    t->iconPathId = pathId;

    if (g_TypeHooks == InstallState::Installed && !g_OrigGetIconFtexPath)
        Log("[TppCollection] SetCollectionTypeIcon stored for type %u but the icon hook is not "
            "installed on this build - the generic key-item icon is still drawn\n", t->typeId);

    return true;
}

bool TppCollection_SetTypeLangId(const char* typeName, std::int32_t typeId, std::uint64_t langId)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    CustomType* t = ResolveTypeArg(typeName, typeId);
    if (!t)
    {
        char what[128];
        LogTypeArg(what, sizeof(what), typeName, typeId);
        Log("[TppCollection] SetCollectionTypeLangId %s rejected: no custom type by that name or "
            "id - register the type first\n", what);
        return false;
    }

    t->nameLangId = langId & 0xFFFFFFFFFFFFull;
    g_LangResolveFaultLogged = false;

    if (g_TypeHooks == InstallState::Installed && !g_OrigGetNameText)
        Log("[TppCollection] SetCollectionTypeLangId stored for type %u but the name hook is not "
            "installed on this build - the collectible still shows no label\n", t->typeId);

    return true;
}

std::int32_t TppCollection_FindTypeByName(const char* typeName)
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    const CustomType* t = FindCustomTypeByName(typeName);
    return t ? static_cast<std::int32_t>(t->typeId) : -1;
}

bool TppCollection_TypesAvailable()
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    return g_TypeHooks != InstallState::Failed;
}

bool Install_TppCollectionHooks()
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (g_HooksInstalled)
        return true;

    if (!gAddr.CollectionLocatorArray_SetUpLinearAccessor ||
        !gAddr.CollectionSystemImpl_PickUp ||
        !gAddr.CollectionSystemImpl_RepopCountOperation ||
        !gAddr.CollectionSystemImpl_GetCollectionInfo)
    {
        g_TypeHooks = InstallState::Failed;
        Log("[TppCollection] install skipped: collection addresses not ported for %s - "
            "custom collectibles unavailable\n",
            GetGameBuildName(gGameBuild));
        return false;
    }

    void* tAccessor = ResolveGameAddress(gAddr.CollectionLocatorArray_SetUpLinearAccessor);
    void* tPickUp   = ResolveGameAddress(gAddr.CollectionSystemImpl_PickUp);
    void* tRepop    = ResolveGameAddress(gAddr.CollectionSystemImpl_RepopCountOperation);
    void* tInfo     = ResolveGameAddress(gAddr.CollectionSystemImpl_GetCollectionInfo);

    if (!CreateAndEnableHook(tAccessor, Hook_SetUpLinearAccessor,
                             reinterpret_cast<void**>(&g_OrigSetUpLinearAccessor)) ||
        !CreateAndEnableHook(tPickUp, Hook_PickUp,
                             reinterpret_cast<void**>(&g_OrigPickUp)) ||
        !CreateAndEnableHook(tRepop, Hook_RepopCountOperation,
                             reinterpret_cast<void**>(&g_OrigRepopCountOperation)) ||
        !CreateAndEnableHook(tInfo, Hook_GetCollectionInfo,
                             reinterpret_cast<void**>(&g_OrigGetCollectionInfo)))
    {
        g_TypeHooks = InstallState::Failed;
        Log("[TppCollection] install failed: could not hook the collection system - "
            "custom collectibles unavailable\n");
        DisableAndRemoveHook(tAccessor);
        DisableAndRemoveHook(tPickUp);
        DisableAndRemoveHook(tRepop);
        DisableAndRemoveHook(tInfo);
        g_OrigSetUpLinearAccessor = nullptr;
        g_OrigPickUp = nullptr;
        g_OrigRepopCountOperation = nullptr;
        g_OrigGetCollectionInfo = nullptr;
        return false;
    }

    g_HooksInstalled = true;

    if (gAddr.Collection_GetModelFilePath &&
        gAddr.Collection_GetCatchEffectColorAndScale &&
        gAddr.Collection_GetGroundEffectSize &&
        gAddr.PickUpActionPluginImpl_DoPickUpCollection)
    {
        void* tModel  = ResolveGameAddress(gAddr.Collection_GetModelFilePath);
        void* tCatch  = ResolveGameAddress(gAddr.Collection_GetCatchEffectColorAndScale);
        void* tGround = ResolveGameAddress(gAddr.Collection_GetGroundEffectSize);
        void* tDoPick = ResolveGameAddress(gAddr.PickUpActionPluginImpl_DoPickUpCollection);

        if (CreateAndEnableHook(tModel, Hook_GetModelFilePath,
                                reinterpret_cast<void**>(&g_OrigGetModelFilePath)) &&
            CreateAndEnableHook(tCatch, Hook_GetCatchEffectColorAndScale,
                                reinterpret_cast<void**>(&g_OrigGetCatchEffect)) &&
            CreateAndEnableHook(tGround, Hook_GetGroundEffectSize,
                                reinterpret_cast<void**>(&g_OrigGetGroundEffectSize)) &&
            CreateAndEnableHook(tDoPick, Hook_DoPickUpCollection,
                                reinterpret_cast<void**>(&g_OrigDoPickUpCollection)))
        {
            g_TypeHooks = InstallState::Installed;
        }
        else
        {
            g_TypeHooks = InstallState::Failed;
            Log("[TppCollection] type hooks failed to install - custom collection TYPES "
                "unavailable (custom placements of vanilla types still work)\n");
            DisableAndRemoveHook(tModel);
            DisableAndRemoveHook(tCatch);
            DisableAndRemoveHook(tGround);
            DisableAndRemoveHook(tDoPick);
            g_OrigGetModelFilePath = nullptr;
            g_OrigGetCatchEffect = nullptr;
            g_OrigGetGroundEffectSize = nullptr;
            g_OrigDoPickUpCollection = nullptr;
        }

        if (g_TypeHooks == InstallState::Installed)
        {
            if (!gAddr.GetCollectionIconFtexPath)
            {
                Log("[TppCollection] icon address not ported for %s - custom types keep the "
                    "generic key-item pickup icon\n", GetGameBuildName(gGameBuild));
            }
            else
            {
                void* tIcon = ResolveGameAddress(gAddr.GetCollectionIconFtexPath);
                if (!CreateAndEnableHook(tIcon, Hook_GetCollectionIconFtexPath,
                                         reinterpret_cast<void**>(&g_OrigGetIconFtexPath)))
                {
                    DisableAndRemoveHook(tIcon);
                    g_OrigGetIconFtexPath = nullptr;
                    Log("[TppCollection] icon hook failed to install - custom types keep the "
                        "generic key-item pickup icon\n");
                }
            }

            if (!gAddr.GetCollectionNameText)
            {
                Log("[TppCollection] name address not ported for %s - custom types show no "
                    "collectible label\n", GetGameBuildName(gGameBuild));
            }
            else
            {
                void* tName = ResolveGameAddress(gAddr.GetCollectionNameText);
                if (!CreateAndEnableHook(tName, Hook_GetCollectionNameText,
                                         reinterpret_cast<void**>(&g_OrigGetNameText)))
                {
                    DisableAndRemoveHook(tName);
                    g_OrigGetNameText = nullptr;
                    Log("[TppCollection] name hook failed to install - custom types show no "
                        "collectible label\n");
                }
            }

            struct ParityHook
            {
                uintptr_t   addr;
                void*       detour;
                void**      orig;
                const char* miss;
            };

            const ParityHook parity[] =
            {
                { gAddr.Collection_GetModelFilePathRoot, &Hook_GetModelFilePathRoot,
                  reinterpret_cast<void**>(&g_OrigGetModelFilePathRoot),
                  "RootModel is ignored - custom types leave nothing behind when picked" },
                { gAddr.Collection_GetFovaFilePath, &Hook_GetFovaFilePath,
                  reinterpret_cast<void**>(&g_OrigGetFovaFilePath),
                  "Fova is ignored - custom types render their base mesh with no form variation" },
                { gAddr.Collection_IsHerbByType, &Hook_IsHerbByType,
                  reinterpret_cast<void**>(&g_OrigIsHerbByType),
                  "IsHerb is ignored - TppCollection.IsHerbByType answers false for custom types" },
                { gAddr.Collection_IsMaterialByType, &Hook_IsMaterialByType,
                  reinterpret_cast<void**>(&g_OrigIsMaterialByType),
                  "IsMaterial is ignored - TppCollection.IsMaterialByType answers false for custom types" },
                { gAddr.Collection_IsDiamondByType, &Hook_IsDiamondByType,
                  reinterpret_cast<void**>(&g_OrigIsDiamondByType),
                  "IsDiamond is ignored - TppCollection.IsDiamondByType answers false for custom types" },
            };

            for (const ParityHook& h : parity)
            {
                if (!h.addr)
                {
                    Log("[TppCollection] address not ported for %s - %s\n",
                        GetGameBuildName(gGameBuild), h.miss);
                    continue;
                }

                void* target = ResolveGameAddress(h.addr);
                if (!CreateAndEnableHook(target, h.detour, h.orig))
                {
                    DisableAndRemoveHook(target);
                    *h.orig = nullptr;
                    Log("[TppCollection] hook failed to install - %s\n", h.miss);
                }
            }
        }
    }
    else
    {
        g_TypeHooks = InstallState::Failed;
        Log("[TppCollection] type addresses not ported for %s - custom collection TYPES "
            "unavailable (custom placements of vanilla types still work)\n",
            GetGameBuildName(gGameBuild));
    }

    if (g_TypeHooks == InstallState::Failed && !g_Types.empty())
        Log("[TppCollection] %zu custom type(s) were registered before the type hooks failed to "
            "install - those types will not render or reward\n", g_Types.size());

    LogDebug("[TppCollection] Install hooks: OK (custom types: %s)\n",
             g_TypeHooks == InstallState::Installed ? "armed" : "unavailable");
    return true;
}

void Uninstall_TppCollectionHooks()
{
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);

    if (!g_HooksInstalled)
        return;

    DisableAndRemoveHook(ResolveGameAddress(gAddr.CollectionLocatorArray_SetUpLinearAccessor));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.CollectionSystemImpl_PickUp));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.CollectionSystemImpl_RepopCountOperation));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.CollectionSystemImpl_GetCollectionInfo));

    if (g_TypeHooks == InstallState::Installed)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_GetModelFilePath));
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_GetCatchEffectColorAndScale));
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_GetGroundEffectSize));
        DisableAndRemoveHook(ResolveGameAddress(gAddr.PickUpActionPluginImpl_DoPickUpCollection));
    }
    if (g_OrigGetIconFtexPath)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.GetCollectionIconFtexPath));
        g_OrigGetIconFtexPath = nullptr;
    }
    if (g_OrigGetNameText)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.GetCollectionNameText));
        g_OrigGetNameText = nullptr;
    }
    if (g_OrigGetModelFilePathRoot)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_GetModelFilePathRoot));
        g_OrigGetModelFilePathRoot = nullptr;
    }
    if (g_OrigGetFovaFilePath)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_GetFovaFilePath));
        g_OrigGetFovaFilePath = nullptr;
    }
    if (g_OrigIsHerbByType)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_IsHerbByType));
        g_OrigIsHerbByType = nullptr;
    }
    if (g_OrigIsMaterialByType)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_IsMaterialByType));
        g_OrigIsMaterialByType = nullptr;
    }
    if (g_OrigIsDiamondByType)
    {
        DisableAndRemoveHook(ResolveGameAddress(gAddr.Collection_IsDiamondByType));
        g_OrigIsDiamondByType = nullptr;
    }
    g_TypeHooks = InstallState::NotAttempted;

    g_OrigSetUpLinearAccessor = nullptr;
    g_OrigPickUp = nullptr;
    g_OrigRepopCountOperation = nullptr;
    g_OrigGetCollectionInfo = nullptr;
    g_OrigGetModelFilePath = nullptr;
    g_OrigGetCatchEffect = nullptr;
    g_OrigGetGroundEffectSize = nullptr;
    g_OrigDoPickUpCollection = nullptr;
    g_Types.clear();
    g_Arr = nullptr;
    g_SeenInfoData = nullptr;
    g_HaveVanilla = false;
    g_CaptureFailed = false;
    g_Tail.clear();
    g_CustomsInstalled = false;
    g_CustomsBlocked = false;
    g_HooksInstalled = false;
}
