#include "pch.h"

#include <Windows.h>
#include <intrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "AddressSet.h"
#include "BulletLockOn.h"
#include "EquipPartParams.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    struct LockOnSpec
    {
        std::uint32_t count = 1;
        float time = 1.0f;
        float turnRadPerSec = 1.5708f;
        float minRange = 0.0f;
        float maxRange = 0.0f;
        float speedScale = 0.0f;
        float homingStartDist = 0.0f;
        bool customMasks = false;
        std::uint32_t typeMask = 0x73;
        std::uint8_t zeroAttrTypes = 0;
    };

    struct SlotTag
    {
        bool active = false;
        std::uint16_t targetId = 0xFFFF;
        std::uint16_t bulletId = 0x3FF;
        float lastElapsed = 0.0f;
        float turnRadPerSec = 0.0f;
        float extraBudget = 0.0f;
        float homingStartDist = 0.0f;
        float spawnPos[3] = { 0.0f, 0.0f, 0.0f };
        float lastPos[3] = { 0.0f, 0.0f, 0.0f };
        bool haveLastPos = false;
    };

    constexpr int kMaxSlots = 256;
    constexpr float kStopSteerDistSq = 4.0f;
    constexpr float kExtraFlightBudget = 2.0f;

    std::recursive_mutex g_Mutex;
    std::unordered_map<int, LockOnSpec> g_SpecByBulletId;

    std::uint8_t* g_SightMgr = nullptr;
    unsigned long long g_SightMgrStampMs = 0;
    constexpr unsigned long long kSightMgrFreshMs = 250;
    LockOnSpec g_ActiveSpec{};
    bool g_ActiveValid = false;

    std::unordered_map<std::uintptr_t, std::unique_ptr<SlotTag[]>> g_TagsByImpl;

    int SehKeepAvOnly(unsigned long code)
    {
        return (code == EXCEPTION_ACCESS_VIOLATION
                || code == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    using GetQuarkSystemTable_t = std::uint8_t* (__fastcall*)();
    GetQuarkSystemTable_t g_GetQuarkSystemTable = nullptr;

    using GetLockParam_t = std::uint64_t(__fastcall*)(void*, std::uint32_t,
                                                      std::uint32_t*, float*,
                                                      std::uint32_t);
    using UpdateLock_t = void(__fastcall*)(std::uint8_t*, void*);
    using DoSim_t = void(__fastcall*)(std::uint8_t*);
    using ActivateBullet_t = void(__fastcall*)(std::uint8_t*, std::uint32_t);
    using SightUpdate_t = void(__fastcall*)(std::uint8_t*);
    using UpdateLockUi_t = void(__fastcall*)(std::uint8_t*);

    GetLockParam_t   g_OrigGetLockParam = nullptr;
    UpdateLock_t     g_OrigUpdateLock = nullptr;
    DoSim_t          g_OrigDoSim = nullptr;
    ActivateBullet_t g_OrigActivate = nullptr;
    SightUpdate_t    g_OrigSightUpdate = nullptr;
    UpdateLockUi_t   g_UpdateMissileLockOnUi = nullptr;

    using WinCreate_t = void(__fastcall*)(std::uint8_t*, void*, void*);
    WinCreate_t g_OrigLockWinCreate = nullptr;
    void* g_OurLockWindow = nullptr;

    bool g_WeDrewLockUi = false;
    unsigned long long g_ActiveStampMs = 0;
    constexpr unsigned long long kActiveFreshMs = 400;
    constexpr unsigned long long kVanillaGraceMs = 100;

    std::uint16_t g_LastLockId = 0xFFFF;
    unsigned long long g_LastLockStampMs = 0;
    constexpr unsigned long long kLockLatchMs = 600;

    std::uint16_t g_LockedIds[8] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                                     0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    std::uint32_t g_LockedCount = 0;
    unsigned long long g_LockedStampMs = 0;
    std::uint32_t g_VolleyCursor = 0;

    SlotTag* TagsFor(std::uint8_t* impl)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
        auto it = g_TagsByImpl.find(reinterpret_cast<std::uintptr_t>(impl));
        if (it != g_TagsByImpl.end())
            return it->second.get();
        auto arr = std::make_unique<SlotTag[]>(kMaxSlots);
        SlotTag* raw = arr.get();
        g_TagsByImpl.emplace(reinterpret_cast<std::uintptr_t>(impl),
                             std::move(arr));
        return raw;
    }

    SlotTag* FindTags(std::uint8_t* impl)
    {
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
        auto it = g_TagsByImpl.find(reinterpret_cast<std::uintptr_t>(impl));
        return (it != g_TagsByImpl.end()) ? it->second.get() : nullptr;
    }

    struct SightSave
    {
        std::uint32_t d4 = 0;
        std::uint32_t d8 = 0;
        std::uint32_t e8 = 0;
    };

    bool SwapMissileQuerySEH(std::uint8_t* mgr, float coneInput, float range,
                             SightSave* out)
    {
        __try
        {
            auto* p5d4 = reinterpret_cast<std::uint32_t*>(mgr + 0x5d4);
            auto* p5d8 = reinterpret_cast<std::uint32_t*>(mgr + 0x5d8);
            auto* p5e8 = reinterpret_cast<std::uint32_t*>(mgr + 0x5e8);
            out->d4 = *p5d4;
            out->d8 = *p5d8;
            out->e8 = *p5e8;
            *reinterpret_cast<float*>(p5d4) = coneInput;
            *reinterpret_cast<float*>(p5d8) = coneInput;
            *reinterpret_cast<float*>(p5e8) = range;
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    void RestoreSightSEH(std::uint8_t* mgr, const SightSave* s)
    {
        __try
        {
            *reinterpret_cast<std::uint32_t*>(mgr + 0x5d4) = s->d4;
            *reinterpret_cast<std::uint32_t*>(mgr + 0x5d8) = s->d8;
            *reinterpret_cast<std::uint32_t*>(mgr + 0x5e8) = s->e8;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

    float ReadZoomProductSEH(void* ctx)
    {
        __try
        {
            const float a = *reinterpret_cast<float*>(
                static_cast<std::uint8_t*>(ctx) + 0x2d8);
            const float b = *reinterpret_cast<float*>(
                static_cast<std::uint8_t*>(ctx) + 0x2dc);
            const float z = a * b;
            if (!(z >= 1.0f))
                return 1.0f;
            return z > 40.0f ? 40.0f : z;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return 1.0f;
        }
    }

    bool ReadLockCachedPosSEH(std::uint8_t* mgr, std::uint16_t targetId,
                              float out[3])
    {
        __try
        {
            for (std::uint32_t i = 0; i < 8; ++i)
            {
                const std::uint16_t id =
                    *reinterpret_cast<std::uint16_t*>(mgr + 0x78 + i * 2);
                if (id != targetId)
                    continue;
                const float* p =
                    reinterpret_cast<const float*>(mgr + 0x90 + i * 0x10);
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
                return true;
            }
            return false;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    bool ReadConfirmedLockSEH(std::uint8_t* mgr, std::uint16_t* outId)
    {
        __try
        {
            const std::uint32_t bits =
                *reinterpret_cast<std::uint32_t*>(mgr + 0x11c);
            if ((bits & 0x1e) == 0)
                return false;
            std::uint32_t n = (bits >> 1) & 0xf;
            if (n > 8) n = 8;
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint16_t id =
                    *reinterpret_cast<std::uint16_t*>(mgr + 0x78 + i * 2);
                if (id != 0xFFFF)
                {
                    *outId = id;
                    return true;
                }
            }
            return false;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    int CaptureLockSetSEH(std::uint8_t* mgr, std::uint16_t out[8])
    {
        __try
        {
            if ((*reinterpret_cast<std::uint32_t*>(mgr + 0x11c) & 0x1e) == 0)
                return 0;
            int c = 0;
            for (int i = 0; i < 8; ++i)
            {
                const std::uint16_t id =
                    *reinterpret_cast<std::uint16_t*>(mgr + 0x78 + i * 2);
                if (id != 0xFFFF && (id >> 9) != 0)
                    out[c++] = id;
            }
            return c;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return 0;
        }
    }

    struct BulletDescInfo
    {
        std::uint16_t ownerId;
        std::uint16_t attackId;
        std::uint8_t bulletId;
        float pos[4];
    };

    bool ReadBulletDescSEH(std::uint8_t* impl, std::uint32_t descIdx,
                           BulletDescInfo* out)
    {
        __try
        {
            std::uint8_t* base = *reinterpret_cast<std::uint8_t**>(impl + 0x38);
            if (!base)
                return false;
            std::uint8_t* d = base + static_cast<std::uintptr_t>(descIdx) * 0x40;
            out->ownerId  = *reinterpret_cast<std::uint16_t*>(d + 0x2a);
            out->attackId = *reinterpret_cast<std::uint16_t*>(d + 0x2e);
            out->bulletId = *(d + 0x30);
            std::memcpy(out->pos, d, 16);
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    bool SnapshotMaskSEH(std::uint8_t* impl, std::uint8_t* outMask,
                         std::uint32_t* outCap)
    {
        __try
        {
            std::uint32_t cap = *reinterpret_cast<std::uint32_t*>(impl + 0x28);
            if (cap > kMaxSlots) cap = kMaxSlots;
            const std::uint8_t* mask =
                *reinterpret_cast<std::uint8_t**>(impl + 0x88);
            if (!mask)
                return false;
            std::memcpy(outMask, mask, (cap + 7) / 8);
            *outCap = cap;
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    int FindActivatedSlotSEH(std::uint8_t* impl, const std::uint8_t* before,
                             std::uint32_t cap, const BulletDescInfo* di)
    {
        __try
        {
            const std::uint8_t* mask =
                *reinterpret_cast<std::uint8_t**>(impl + 0x88);
            if (!mask)
                return -1;
            for (std::uint32_t i = 0; i < cap; ++i)
            {
                const std::uint8_t bit = static_cast<std::uint8_t>(1u << (i & 7));
                if ((mask[i >> 3] & bit) != 0 && (before[i >> 3] & bit) == 0)
                    return static_cast<int>(i);
            }
            const std::uint8_t* recs =
                *reinterpret_cast<std::uint8_t**>(impl + 0xb0);
            const float* oldPos =
                *reinterpret_cast<float**>(impl + 0x98);
            if (!recs || !oldPos)
                return -1;
            for (std::uint32_t i = 0; i < cap; ++i)
            {
                const std::uint8_t bit = static_cast<std::uint8_t>(1u << (i & 7));
                if ((mask[i >> 3] & bit) == 0)
                    continue;
                const std::uint8_t* rec = recs + i * 0x30;
                if (*reinterpret_cast<const float*>(rec) != 0.0f)
                    continue;
                if ((*reinterpret_cast<const std::uint32_t*>(rec + 0x2c) & 0xff)
                    != di->bulletId)
                    continue;
                const float* p = oldPos + i * 4;
                if (p[0] == di->pos[0] && p[1] == di->pos[1]
                    && p[2] == di->pos[2])
                    return static_cast<int>(i);
            }
            return -1;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return -1;
        }
    }

    using SendSignal_t = int* (__fastcall*)(void*, void*, std::uint32_t, void*);

    bool ResolveTargetPos(std::uint8_t* dispatcher, std::uint16_t targetId,
                          float out[3])
    {
        SendSignal_t send = *reinterpret_cast<SendSignal_t*>(
            *reinterpret_cast<std::uintptr_t*>(dispatcher) + 0x38);

        alignas(16) std::uint8_t sig[0x30] = {};
        *reinterpret_cast<std::uint64_t*>(sig) = 0x88be348b1460ull;
        std::uint64_t err = 0;
        int* ret = send(dispatcher, &err, targetId, sig);
        if (ret && *ret == 0)
        {
            const float* p = reinterpret_cast<const float*>(sig + 0x10);
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
            return true;
        }

        alignas(16) std::uint8_t sig2[0x40] = {};
        *reinterpret_cast<std::uint64_t*>(sig2) = 0xd0a5d0c7e373ull;
        reinterpret_cast<float*>(sig2 + 0x10)[3] = 1.0f;
        std::uint64_t err2[2] = {};
        send(dispatcher, err2, targetId, sig2);
        if (sig2[0x30] != 0)
        {
            const float* p = reinterpret_cast<const float*>(sig2 + 0x20);
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
            return true;
        }
        return false;
    }

    void SteerSlots(std::uint8_t* impl, SlotTag* tags, std::uint8_t* dispatcher)
    {
        std::uint32_t cap = *reinterpret_cast<std::uint32_t*>(impl + 0x28);
        if (cap > kMaxSlots) cap = kMaxSlots;
        std::uint8_t* mask = *reinterpret_cast<std::uint8_t**>(impl + 0x88);
        std::uint8_t* recs = *reinterpret_cast<std::uint8_t**>(impl + 0xb0);
        float* oldPos = *reinterpret_cast<float**>(impl + 0x98);
        float* dirs = *reinterpret_cast<float**>(impl + 0xa8);
        std::uint8_t* timeSrc = *reinterpret_cast<std::uint8_t**>(impl + 0xe8);
        if (!mask || !recs || !oldPos || !dirs || !timeSrc)
            return;
        const float dtBase =
            static_cast<float>(*reinterpret_cast<double*>(timeSrc + 0x10));
        const float dtReflex =
            static_cast<float>(*reinterpret_cast<double*>(timeSrc + 0x20));
        if (dtBase <= 0.0f)
            return;
        bool reflexOn = false;
        std::uint8_t* reflexIface =
            *reinterpret_cast<std::uint8_t**>(impl + 0xf8);
        if (reflexIface)
        {
            using ReflexQuery_t = char(__fastcall*)(void*);
            ReflexQuery_t q = *reinterpret_cast<ReflexQuery_t*>(
                *reinterpret_cast<std::uintptr_t*>(reflexIface) + 0xa0);
            reflexOn = q(reflexIface) != 0;
        }

        for (std::uint32_t i = 0; i < cap; ++i)
        {
            SlotTag& tag = tags[i];
            if (!tag.active)
                continue;
            if ((mask[i >> 3] & (1u << (i & 7))) == 0)
            {
                tag.active = false;
                continue;
            }
            std::uint8_t* rec = recs + i * 0x30;
            const std::uint16_t recBits =
                *reinterpret_cast<std::uint16_t*>(rec + 0x2a);
            const std::uint32_t recFlags =
                *reinterpret_cast<std::uint32_t*>(rec + 0x2c);
            if ((recFlags & 0xff) != tag.bulletId)
            {
                tag.active = false;
                continue;
            }
            const float elapsed = *reinterpret_cast<float*>(rec + 0x00);
            if (elapsed < tag.lastElapsed)
            {
                tag.active = false;
                continue;
            }
            tag.lastElapsed = elapsed;
            if (recBits & 0x800)
                continue;
            if (recFlags & 0x80000)
            {
                tag.active = false;
                continue;
            }
            const float dt = (reflexOn && (recFlags & 0x40000) != 0)
                ? dtReflex : dtBase;
            if (dt <= 0.0f)
                continue;

            float* p = oldPos + i * 4;
            if (tag.homingStartDist > 0.0f)
            {
                const float fx = p[0] - tag.spawnPos[0];
                const float fy = p[1] - tag.spawnPos[1];
                const float fz = p[2] - tag.spawnPos[2];
                if (fx * fx + fy * fy + fz * fz
                    < tag.homingStartDist * tag.homingStartDist)
                    continue;
            }

            float tp[3];
            bool got = ResolveTargetPos(dispatcher, tag.targetId, tp);
            if (!got && g_SightMgr)
                got = ReadLockCachedPosSEH(g_SightMgr, tag.targetId, tp);
            if (got)
            {
                tag.lastPos[0] = tp[0];
                tag.lastPos[1] = tp[1];
                tag.lastPos[2] = tp[2];
                tag.haveLastPos = true;
            }
            else if (tag.haveLastPos)
            {
                tp[0] = tag.lastPos[0];
                tp[1] = tag.lastPos[1];
                tp[2] = tag.lastPos[2];
            }
            else
            {
                tag.active = false;
                continue;
            }

            float* d = dirs + i * 4;
            float to[3] = { tp[0] - p[0], tp[1] - p[1], tp[2] - p[2] };
            const float dist2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
            if (dist2 < kStopSteerDistSq)
            {
                tag.active = false;
                continue;
            }
            const float invLen = 1.0f / std::sqrt(dist2);
            to[0] *= invLen; to[1] *= invLen; to[2] *= invLen;

            float dn[3];
            const float dl2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (dl2 > 1e-12f)
            {
                const float invD = 1.0f / std::sqrt(dl2);
                dn[0] = d[0] * invD; dn[1] = d[1] * invD; dn[2] = d[2] * invD;
            }
            else
            {
                dn[0] = to[0]; dn[1] = to[1]; dn[2] = to[2];
            }

            const float cosA = dn[0] * to[0] + dn[1] * to[1] + dn[2] * to[2];
            float maxStep = tag.turnRadPerSec * dt;
            if (maxStep > 3.1415f) maxStep = 3.1415f;
            const float cosMax = std::cos(maxStep);
            float u[3] = { dn[0], dn[1], dn[2] };
            if (cosA >= cosMax)
            {
                u[0] = to[0]; u[1] = to[1]; u[2] = to[2];
            }
            else
            {
                float perp[3] = { to[0] - dn[0] * cosA, to[1] - dn[1] * cosA,
                                  to[2] - dn[2] * cosA };
                const float pl2 = perp[0] * perp[0] + perp[1] * perp[1]
                    + perp[2] * perp[2];
                if (pl2 > 1e-12f)
                {
                    const float invP = 1.0f / std::sqrt(pl2);
                    const float sinMax = std::sin(maxStep);
                    float nd[3] = {
                        dn[0] * cosMax + perp[0] * invP * sinMax,
                        dn[1] * cosMax + perp[1] * invP * sinMax,
                        dn[2] * cosMax + perp[2] * invP * sinMax,
                    };
                    const float nl2 = nd[0] * nd[0] + nd[1] * nd[1]
                        + nd[2] * nd[2];
                    if (nl2 > 1e-12f)
                    {
                        const float invN = 1.0f / std::sqrt(nl2);
                        u[0] = nd[0] * invN;
                        u[1] = nd[1] * invN;
                        u[2] = nd[2] * invN;
                    }
                }
            }
            d[0] = u[0];
            d[1] = u[1];
            d[2] = u[2];
            d[3] = 0.0f;

            *reinterpret_cast<float*>(rec + 0x18) = 0.0f;

            float* remain = reinterpret_cast<float*>(rec + 0x04);
            if (tag.extraBudget > 0.0f && *remain < 0.5f)
            {
                float add = 0.5f;
                if (add > tag.extraBudget) add = tag.extraBudget;
                *remain += add;
                tag.extraBudget -= add;
            }
        }
    }

    void SteerPassSEH(std::uint8_t* impl, SlotTag* tags,
                      std::uint8_t* dispatcher)
    {
        __try
        {
            SteerSlots(impl, tags, dispatcher);
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

    void** g_ProviderSlot = nullptr;
    bool g_ProviderHookTried = false;

    std::uint64_t __fastcall hkGetLockParam(void* self, std::uint32_t bulletId,
                                            std::uint32_t* outCount,
                                            float* outTime,
                                            std::uint32_t equipId);

    bool SwapVtableSlotSEH(std::uint8_t* impl, void* newFn,
                           GetLockParam_t* outOrig, void*** outSlot)
    {
        __try
        {
            std::uint8_t* vtable = *reinterpret_cast<std::uint8_t**>(impl);
            if (!vtable)
                return false;
            void** slot = reinterpret_cast<void**>(vtable + 0x80);
            DWORD oldProt = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
                return false;
            *outOrig = reinterpret_cast<GetLockParam_t>(*slot);
            *slot = newFn;
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            *outSlot = slot;
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    bool EnsureProviderVtableHook()
    {
        if (g_OrigGetLockParam)
            return true;
        auto* impl = static_cast<std::uint8_t*>(
            ResolveGameAddress(gAddr.EquipParameterTablesImpl_Instance));
        if (!impl)
            return false;
        GetLockParam_t orig = nullptr;
        void** slot = nullptr;
        if (!SwapVtableSlotSEH(impl, reinterpret_cast<void*>(&hkGetLockParam),
                               &orig, &slot))
            return false;
        g_OrigGetLockParam = orig;
        g_ProviderSlot = slot;
        LogDebug("[BulletLockOn] lock provider hooked via vtable slot +0x80 "
            "(orig=%p)\n", reinterpret_cast<void*>(orig));
        return true;
    }

    using QueryExec_t = std::uint64_t(__fastcall*)(void*, std::uint32_t,
                                                   std::uint8_t*);
    QueryExec_t g_OrigQueryExec = nullptr;
    void** g_QuerySlot = nullptr;
    bool g_InOurLockQuery = false;

    void RewriteQueryDescSEH(std::uint8_t* desc)
    {
        __try
        {
            if (!g_ActiveSpec.customMasks)
                return;
            *reinterpret_cast<std::uint32_t*>(desc + 0x0c) = g_ActiveSpec.typeMask;
            for (int t = 0; t < 8; ++t)
                if (g_ActiveSpec.zeroAttrTypes & (1u << t))
                    *reinterpret_cast<std::uint16_t*>(desc + 0x10 + t * 2) = 0;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

    std::uint64_t __fastcall hkQueryExec(void* self, std::uint32_t mode,
                                         std::uint8_t* desc)
    {
        if (g_InOurLockQuery && mode == 3 && desc)
            RewriteQueryDescSEH(desc);
        return g_OrigQueryExec(self, mode, desc);
    }

    void ScrubSelfLocksSEH(std::uint8_t* mgr)
    {
        __try
        {
            for (int i = 0; i < 8; ++i)
            {
                auto* slot = reinterpret_cast<std::uint16_t*>(mgr + 0x78 + i * 2);
                if (*slot != 0xFFFF && (*slot >> 9) == 0)
                    *slot = 0xFFFF;
            }
            auto* pending = reinterpret_cast<std::uint16_t*>(mgr + 0x54);
            if (*pending != 0xFFFF && (*pending >> 9) == 0)
                *pending = 0xFFFF;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

    void MinRangeScrubSEH(std::uint8_t* mgr, float minRange)
    {
        __try
        {
            if (minRange <= 0.0f || !g_GetQuarkSystemTable)
                return;
            std::uint8_t* qst = g_GetQuarkSystemTable();
            std::uint8_t* dispatcher =
                *reinterpret_cast<std::uint8_t**>(qst + 0x60);
            if (!dispatcher)
                return;
            float pp[3];
            if (!ResolveTargetPos(dispatcher, 0, pp))
                return;
            const float minSq = minRange * minRange;
            for (int i = 0; i < 8; ++i)
            {
                auto* slot =
                    reinterpret_cast<std::uint16_t*>(mgr + 0x78 + i * 2);
                if (*slot == 0xFFFF)
                    continue;
                const float* tp =
                    reinterpret_cast<const float*>(mgr + 0x90 + i * 0x10);
                const float dx = tp[0] - pp[0];
                const float dy = tp[1] - pp[1];
                const float dz = tp[2] - pp[2];
                if (dx * dx + dy * dy + dz * dz < minSq)
                    *slot = 0xFFFF;
            }
            auto* pending = reinterpret_cast<std::uint16_t*>(mgr + 0x54);
            if (*pending != 0xFFFF)
            {
                float tp[3];
                if (ResolveTargetPos(dispatcher, *pending, tp))
                {
                    const float dx = tp[0] - pp[0];
                    const float dy = tp[1] - pp[1];
                    const float dz = tp[2] - pp[2];
                    if (dx * dx + dy * dy + dz * dz < minSq)
                        *pending = 0xFFFF;
                }
            }
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

    bool EnsureQueryVtableHookSEH(std::uint8_t* mgr)
    {
        __try
        {
            if (g_OrigQueryExec)
                return true;
            std::uint8_t* obj = *reinterpret_cast<std::uint8_t**>(mgr + 0x120);
            if (!obj)
                return false;
            std::uint8_t* vtable = *reinterpret_cast<std::uint8_t**>(obj);
            if (!vtable)
                return false;
            void** slot = reinterpret_cast<void**>(vtable);
            DWORD oldProt = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
                return false;
            g_OrigQueryExec = reinterpret_cast<QueryExec_t>(*slot);
            *slot = reinterpret_cast<void*>(&hkQueryExec);
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            g_QuerySlot = slot;
            LogDebug("[BulletLockOn] lock query hooked via executor vtable slot 0 "
                "(orig=%p)\n", reinterpret_cast<void*>(g_OrigQueryExec));
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    struct TypeSwap
    {
        int lockedType = -1;
        int normalType = -1;
    };
    std::unordered_map<int, TypeSwap> g_TypeByBulletId;

    bool IsLockedNowInternal()
    {
        std::uint16_t liveLock = 0xFFFF;
        const bool fresh = g_SightMgr
            && GetTickCount64() - g_SightMgrStampMs <= kSightMgrFreshMs;
        if (fresh && ReadConfirmedLockSEH(g_SightMgr, &liveLock)
            && liveLock != 0xFFFF)
            return true;
        return g_LastLockId != 0xFFFF
            && GetTickCount64() - g_LastLockStampMs <= kLockLatchMs;
    }

    using DoFire_t = void(__fastcall*)(void*, std::uint32_t, std::uint32_t,
                                       void*);
    DoFire_t g_OrigDoFire = nullptr;

    bool ResolveDoFireRecordSEH(void* self, std::uint32_t p2,
                                std::uint8_t** outRec, int* outBid)
    {
        __try
        {
            std::uint8_t* thiz = static_cast<std::uint8_t*>(self);
            std::uint8_t* a = *reinterpret_cast<std::uint8_t**>(thiz + 0x58);
            if (!a) return false;
            std::uint8_t* tbl = *reinterpret_cast<std::uint8_t**>(a + 0x28);
            if (!tbl) return false;
            const std::uint8_t typeIdx =
                *(tbl + static_cast<std::size_t>(p2) * 0xe + 8);
            std::uint8_t* sys = *reinterpret_cast<std::uint8_t**>(thiz + 0x68);
            if (!sys) return false;
            std::uintptr_t vt = *reinterpret_cast<std::uintptr_t*>(sys);
            if (!vt) return false;
            using Getter = void* (__fastcall*)(void*, std::uint32_t);
            Getter g = *reinterpret_cast<Getter*>(vt);
            std::uint8_t* rec =
                static_cast<std::uint8_t*>(g(sys, typeIdx));
            if (!rec) return false;
            *outRec = rec;
            *outBid = rec[0x7c];
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    void WriteRecTypeSEH(std::uint8_t* rec, int type)
    {
        __try
        {
            rec[0x7e] = static_cast<std::uint8_t>(type);
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

#ifdef _DEBUG
    struct DoFireDiag
    {
        std::uint32_t equipId = 0;
        std::uint32_t hw = 0;
        std::uint32_t flags8a = 0;
        std::uint32_t t7e = 0;
        std::uint32_t t7f = 0;
        std::uint32_t fireBits = 0;
        std::uint32_t ammo = 0;
        std::uint32_t st0 = 0;
        std::uint32_t st1 = 0;
        float timer = 0.0f;
        float rate = -1.0f;
        std::uint32_t liveRate = 0;
        std::uint32_t liveTrig = 0;
        std::uint32_t liveShot = 0;
        std::uint32_t liveTrigWord = 0;
    };

    bool ReadDoFireDiagSEH(std::uint8_t* self, std::uint32_t rowIdx,
                           std::uint32_t slot, std::uint8_t* req, DoFireDiag* d)
    {
        __try
        {
            std::uint8_t* tbl = *reinterpret_cast<std::uint8_t**>(
                *reinterpret_cast<std::uint8_t**>(self + 0x58) + 0x28);
            std::uint8_t* row = tbl + static_cast<size_t>(rowIdx) * 0xe;
            d->equipId = *reinterpret_cast<std::uint16_t*>(row);
            d->hw = row[8];
            d->ammo = *reinterpret_cast<std::uint16_t*>(row + 4);
            std::uint8_t* iface = *reinterpret_cast<std::uint8_t**>(self + 0x68);
            std::uint8_t* work =
                *reinterpret_cast<std::uint8_t**>(iface + 0x1a0)
                + static_cast<size_t>(d->hw) * 0x90;
            d->flags8a = *reinterpret_cast<std::uint16_t*>(work + 0x8a);
            d->t7e = work[0x7e];
            d->t7f = work[0x7f];
            d->liveRate = *reinterpret_cast<std::uint16_t*>(work + 0x68);
            d->liveTrigWord = *reinterpret_cast<std::uint32_t*>(work + 0x88);
            d->liveTrig = (d->liveTrigWord >> 10) & 0x7;
            d->liveShot = work[0x7e];
            if (req)
                d->fireBits = *reinterpret_cast<std::uint32_t*>(req + 0x54);
            const std::uint32_t rebased =
                slot - *reinterpret_cast<std::uint32_t*>(self + 0x54);
            std::uint8_t* recs = *reinterpret_cast<std::uint8_t**>(self + 0xe8);
            std::uint8_t* r = recs + static_cast<size_t>(rebased) * 0x14;
            d->st0 = r[0x10];
            d->st1 = r[0x11];
            d->timer = *reinterpret_cast<float*>(r);
            std::uint8_t* i30 = *reinterpret_cast<std::uint8_t**>(self + 0x30);
            if (i30)
            {
                std::uint8_t* rateArr =
                    *reinterpret_cast<std::uint8_t**>(i30 + 0x18);
                if (rateArr)
                    d->rate = *reinterpret_cast<float*>(
                        rateArr + static_cast<size_t>(rebased) * 4);
            }
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }
#endif

    void __fastcall hkDoFire(void* self, std::uint32_t p2, std::uint32_t p3,
                             void* req)
    {
        std::uint8_t* rec = nullptr;
        int bid = 0;
        if (ResolveDoFireRecordSEH(self, p2, &rec, &bid))
        {
            int desired = -1;
            {
                std::lock_guard<std::recursive_mutex> lock(g_Mutex);
                auto it = g_TypeByBulletId.find(bid);
                if (it != g_TypeByBulletId.end())
                    desired = IsLockedNowInternal() ? it->second.lockedType
                                                    : it->second.normalType;
            }
            if (desired >= 0)
                WriteRecTypeSEH(rec, desired);
        }
#ifdef _DEBUG
        DoFireDiag pre{};
        const bool diagOk = ReadDoFireDiagSEH(
            static_cast<std::uint8_t*>(self), p2, p3,
            static_cast<std::uint8_t*>(req), &pre);
#endif
        g_OrigDoFire(self, p2, p3, req);
#ifdef _DEBUG
        if (diagOk)
        {
            DoFireDiag post{};
            if (ReadDoFireDiagSEH(static_cast<std::uint8_t*>(self), p2, p3,
                                  static_cast<std::uint8_t*>(req), &post))
            {
                static std::mutex s_Mx;
                static std::unordered_map<unsigned, int> s_Cnt;
                std::lock_guard<std::mutex> lock(s_Mx);
                int& n = s_Cnt[pre.equipId];
                ++n;
                if (n <= 6)
                    LogDebug("[WeaponKey] DoFire eq=%u obj=%p slot=%u hw=%u "
                             "flags8A=0x%04X ammo=%u state %u/%u -> %u/%u rate=%.4f "
                             "|| live gunInfo fireRate(+0x68)=%u trigger(+0x88)=%u "
                             "shotType(+0x7e)=%u [+0x88=0x%08X] - this is the "
                             "gunInfo the shot itself reads at trigger time\n",
                        pre.equipId, self, p3, pre.hw, pre.flags8a, pre.ammo,
                        pre.st0, pre.st1, post.st0, post.st1, post.rate,
                        pre.liveRate, pre.liveTrig, pre.liveShot, pre.liveTrigWord);
            }
        }
#endif
    }

    std::uint64_t __fastcall hkGetLockParam(void* self, std::uint32_t bulletId,
                                            std::uint32_t* outCount,
                                            float* outTime,
                                            std::uint32_t equipId)
    {
        const std::uint64_t ret =
            g_OrigGetLockParam(self, bulletId, outCount, outTime, equipId);

        LockOnSpec spec{};
        bool found = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            auto it = g_SpecByBulletId.find(static_cast<int>(bulletId & 0x3ff));
            if (it != g_SpecByBulletId.end())
            {
                spec = it->second;
                found = true;
            }
        }
        if (found)
        {
            if (outCount) *outCount = spec.count;
            if (outTime)  *outTime = spec.time;
        }
        g_ActiveSpec = spec;
        g_ActiveValid = found;
        if (found)
            g_ActiveStampMs = GetTickCount64();
        return ret;
    }

    void __fastcall hkUpdateMissileLockOn(std::uint8_t* self, void* ctx)
    {
        g_SightMgr = self;
        g_SightMgrStampMs = GetTickCount64();

        std::uint16_t liveLock = 0xFFFF;
        const bool haveLive = ReadConfirmedLockSEH(self, &liveLock);
        if (haveLive && liveLock != 0xFFFF && (liveLock >> 9) != 0)
        {
            g_LastLockId = liveLock;
            g_LastLockStampMs = GetTickCount64();
        }
        {
            std::uint16_t setIds[8];
            const int setN = CaptureLockSetSEH(self, setIds);
            if (setN > 0)
            {
                for (int i = 0; i < setN; ++i)
                    g_LockedIds[i] = setIds[i];
                g_LockedCount = static_cast<std::uint32_t>(setN);
                g_LockedStampMs = GetTickCount64();
            }
        }
        if (g_ActiveValid)
        {
            if (g_ActiveSpec.customMasks)
                EnsureQueryVtableHookSEH(self);
            const float range =
                g_ActiveSpec.maxRange > 0.0f ? g_ActiveSpec.maxRange : 50.0f;
            const float cone = 0.5f * ReadZoomProductSEH(ctx);
            SightSave saved;
            if (SwapMissileQuerySEH(self, cone, range, &saved))
            {
                g_InOurLockQuery = true;
                g_OrigUpdateLock(self, ctx);
                g_InOurLockQuery = false;
                ScrubSelfLocksSEH(self);
                MinRangeScrubSEH(self, g_ActiveSpec.minRange);
                RestoreSightSEH(self, &saved);
                return;
            }
        }
        g_OrigUpdateLock(self, ctx);
    }

    bool ApplyLockedSpeedSEH(std::uint8_t* impl, int slot, float scale)
    {
        __try
        {
            std::uint8_t* recs = *reinterpret_cast<std::uint8_t**>(impl + 0xb0);
            if (!recs)
                return false;
            std::uint8_t* rec = recs + static_cast<std::uintptr_t>(slot) * 0x30;
            float* speed = reinterpret_cast<float*>(rec + 0x08);
            float* remain = reinterpret_cast<float*>(rec + 0x04);
            const float oldSpeed = *speed;
            if (oldSpeed <= 0.0f)
                return false;
            float newSpeed = oldSpeed * scale;
            if (newSpeed < 1.0f) newSpeed = 1.0f;
            *speed = newSpeed;
            *remain *= oldSpeed / newSpeed;
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    void __fastcall hkActivateBullet(std::uint8_t* self, std::uint32_t descIdx)
    {
        std::uint8_t before[kMaxSlots / 8] = {};
        std::uint32_t cap = 0;
        const bool snapped = SnapshotMaskSEH(self, before, &cap);

        int falloffSwapped = 0;
        {
            BulletDescInfo dpre{};
            if (ReadBulletDescSEH(self, descIdx, &dpre))
                falloffSwapped = EquipParam_BulletFalloffSwapBegin(
                    static_cast<int>(dpre.bulletId));
        }

        g_OrigActivate(self, descIdx);

        if (falloffSwapped)
            EquipParam_BulletFalloffSwapEnd();

        if (!snapped)
            return;
        BulletDescInfo di{};
        if (!ReadBulletDescSEH(self, descIdx, &di))
            return;

        const int slot = FindActivatedSlotSEH(self, before, cap, &di);
        if (slot < 0 || slot >= kMaxSlots)
            return;

        SlotTag* tags = TagsFor(self);
        tags[slot] = SlotTag{};

        LockOnSpec spec{};
        bool registered = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            auto it = g_SpecByBulletId.find(static_cast<int>(di.bulletId));
            if (it != g_SpecByBulletId.end())
            {
                spec = it->second;
                registered = true;
            }
        }

        std::uint16_t liveLock = 0xFFFF;
        const bool haveMgr = g_SightMgr != nullptr;
        const bool fresh = haveMgr
            && GetTickCount64() - g_SightMgrStampMs <= kSightMgrFreshMs;
        const bool haveLive = fresh
            && ReadConfirmedLockSEH(g_SightMgr, &liveLock) && liveLock != 0xFFFF;
        const bool haveLatch = g_LastLockId != 0xFFFF
            && GetTickCount64() - g_LastLockStampMs <= kLockLatchMs;
        std::uint16_t targetId =
            haveLive ? liveLock : (haveLatch ? g_LastLockId : 0xFFFF);

        if (registered && spec.count > 1)
        {
            std::uint16_t setIds[8];
            int setN = 0;
            if (fresh && haveMgr)
                setN = CaptureLockSetSEH(g_SightMgr, setIds);
            if (setN == 0 && g_LockedCount > 0
                && GetTickCount64() - g_LockedStampMs <= kLockLatchMs)
            {
                for (std::uint32_t i = 0; i < g_LockedCount; ++i)
                    setIds[i] = g_LockedIds[i];
                setN = static_cast<int>(g_LockedCount);
            }
            if (setN > 1)
            {
                int nTargets = setN;
                if (nTargets > static_cast<int>(spec.count))
                    nTargets = static_cast<int>(spec.count);
                targetId = setIds[g_VolleyCursor % static_cast<std::uint32_t>(nTargets)];
                ++g_VolleyCursor;
            }
        }

        const bool ownerIsPlayer = (di.ownerId >> 9) == 0;
        const bool willTag = registered && ownerIsPlayer && targetId != 0xFFFF;
        if (!willTag)
            return;

        tags[slot].active = true;
        tags[slot].targetId = targetId;
        tags[slot].bulletId = di.bulletId;
        tags[slot].lastElapsed = 0.0f;
        tags[slot].turnRadPerSec = spec.turnRadPerSec;
        tags[slot].homingStartDist = spec.homingStartDist;
        tags[slot].spawnPos[0] = di.pos[0];
        tags[slot].spawnPos[1] = di.pos[1];
        tags[slot].spawnPos[2] = di.pos[2];
        tags[slot].extraBudget = kExtraFlightBudget;
        if (g_SightMgr
            && ReadLockCachedPosSEH(g_SightMgr, targetId, tags[slot].lastPos))
            tags[slot].haveLastPos = true;
        if (spec.speedScale > 0.0f)
            ApplyLockedSpeedSEH(self, slot, spec.speedScale);
    }

    bool ReadDispatchBit3SEH(std::uint8_t* self, bool* outOn)
    {
        __try
        {
            *outOn = ((*(self + 0xe) >> 3) & 1) != 0;
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    void CallLockUiSEH(std::uint8_t* self)
    {
        __try
        {
            g_UpdateMissileLockOnUi(self);
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

    int ReadReticleLayoutSEH(void** outLayout);

    int ReadLockWindowShownSEH()
    {
        __try
        {
            std::uint8_t* win = static_cast<std::uint8_t*>(g_OurLockWindow);
            if (!win)
                return -1;
            std::uint8_t* root = *reinterpret_cast<std::uint8_t**>(win + 0x28);
            if (!root)
                return -1;
            return (*reinterpret_cast<std::uint16_t*>(root + 0x68)) & 1;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return -1;
        }
    }

    bool ReuseLockWindowSEH(void** outWin, void** outLayout)
    {
        __try
        {
            using FindWindow_t = void* (__fastcall*)(std::uint32_t);
            using PostShow_t = void(__fastcall*)(void*);
            using GetLayout_t = void* (__fastcall*)(void*, std::uint32_t);
            auto find = reinterpret_cast<FindWindow_t>(
                ResolveGameAddress(gAddr.UiWindowFunction_FindWindow));
            auto show = reinterpret_cast<PostShow_t>(
                ResolveGameAddress(gAddr.UiWindowFunction_PostShowAndStartMessage));
            auto getLayout = reinterpret_cast<GetLayout_t>(
                ResolveGameAddress(gAddr.UiWindowFunction_GetLayout));
            if (!find || !show || !getLayout)
                return false;
            void* win = find(0xca7559d7u);
            if (!win)
                return false;
            *outWin = win;
            void* layout = getLayout(win, 0x301e58c0u);
            *outLayout = layout;
            if (layout && g_GetQuarkSystemTable)
            {
                std::uint8_t* qst = g_GetQuarkSystemTable();
                std::uint8_t* a = *reinterpret_cast<std::uint8_t**>(qst + 0x98);
                std::uint8_t* b = *reinterpret_cast<std::uint8_t**>(a + 0x40);
                std::uint8_t* c = *reinterpret_cast<std::uint8_t**>(b + 0x20);
                using HudGet_t = std::uint8_t* (__fastcall*)(void*);
                HudGet_t get = *reinterpret_cast<HudGet_t*>(
                    *reinterpret_cast<std::uintptr_t*>(c));
                std::uint8_t* hud = get(c);
                if (hud && *reinterpret_cast<void**>(hud + 0x558) == nullptr)
                    *reinterpret_cast<void**>(hud + 0x558) = layout;
            }
            show(win);
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    void TryEnsureLockWindow()
    {
        if (!gAddr.UiWindowFunction_FindWindow)
            return;
        static unsigned long long lastTryMs = 0;
        const unsigned long long now = GetTickCount64();
        if (now - lastTryMs < 2000)
            return;
        lastTryMs = now;

        void* layout = nullptr;
        if (ReadReticleLayoutSEH(&layout) != 0)
            return;
        if (layout && g_OurLockWindow && ReadLockWindowShownSEH() != 0)
            return;

        void* win = nullptr;
        void* winLayout = nullptr;
        if (ReuseLockWindowSEH(&win, &winLayout))
        {
            if (g_OurLockWindow != win)
                LogDebug("[BulletLockOn] lock reticle window found and shown "
                    "(window=%p layout=%p)\n", win, winLayout);
            g_OurLockWindow = win;
        }
    }

    int ReadReticleLayoutSEH(void** outLayout)
    {
        __try
        {
            if (!g_GetQuarkSystemTable)
                return -1;
            std::uint8_t* qst = g_GetQuarkSystemTable();
            std::uint8_t* a = *reinterpret_cast<std::uint8_t**>(qst + 0x98);
            std::uint8_t* b = *reinterpret_cast<std::uint8_t**>(a + 0x40);
            std::uint8_t* c = *reinterpret_cast<std::uint8_t**>(b + 0x20);
            using HudGet_t = std::uint8_t* (__fastcall*)(void*);
            HudGet_t get = *reinterpret_cast<HudGet_t*>(
                *reinterpret_cast<std::uintptr_t*>(c));
            std::uint8_t* hud = get(c);
            if (!hud)
                return -1;
            *outLayout = *reinterpret_cast<void**>(hud + 0x558);
            return 0;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return -1;
        }
    }

    void __fastcall hkSightManagerUpdate(std::uint8_t* self)
    {
        g_OrigSightUpdate(self);

        if (!g_UpdateMissileLockOnUi)
            return;

        const bool active = g_ActiveValid
            && (GetTickCount64() - g_ActiveStampMs) <= kActiveFreshMs;
        if (!active && !g_WeDrewLockUi)
            return;

        bool gateOn = false;
        if (!ReadDispatchBit3SEH(self, &gateOn))
            return;

        if (active)
        {
            TryEnsureLockWindow();
            if (GetTickCount64() - g_ActiveStampMs > kVanillaGraceMs)
                CallLockUiSEH(self);
            g_WeDrewLockUi = gateOn;
        }
        else
        {
            if (!gateOn)
                CallLockUiSEH(self);
            g_WeDrewLockUi = false;
        }

    }

    void __fastcall hkLockWinCreate(std::uint8_t* self, void* ctx, void* outArr)
    {
        g_OurLockWindow = nullptr;
        g_OrigLockWinCreate(self, ctx, outArr);
    }

    void __fastcall hkDoSimulation(std::uint8_t* self)
    {
        SlotTag* tags = FindTags(self);
        if (tags && g_GetQuarkSystemTable)
        {
            std::uint8_t* qst = g_GetQuarkSystemTable();
            std::uint8_t* dispatcher =
                qst ? *reinterpret_cast<std::uint8_t**>(qst + 0x60) : nullptr;
            if (dispatcher)
                SteerPassSEH(self, tags, dispatcher);
        }
        g_OrigDoSim(self);
    }
}

namespace
{
    void ApplyCategoryBits(const equip::LockOnCategories& c, std::uint32_t& type,
                           std::uint8_t& zeroAttr)
    {
        auto resolve = [&](int flag, int bit)
        {
            if (flag == 1)
            {
                type |= (1u << bit);
                zeroAttr |= static_cast<std::uint8_t>(1u << bit);
            }
            else if (flag == 0)
            {
                type &= ~(1u << bit);
            }
        };

        resolve(c.soldiers, 1);
        resolve(c.vehicles, 5);
    }

    void TranslateCategories(const equip::LockOnCategories& c, LockOnSpec& s)
    {
        const bool any = c.soldiers >= 0 || c.vehicles >= 0;
        if (!any)
            return;

        std::uint32_t type = 0x73;
        std::uint8_t zeroAttr = 0;
        ApplyCategoryBits(c, type, zeroAttr);

        s.customMasks = true;
        s.typeMask = type;
        s.zeroAttrTypes = zeroAttr;
    }
}

namespace equip
{
    void LockOn_RegisterBullet(int bulletId, int lockCount, double lockTimeSec,
                               double turnRateDeg, double minRangeMeters,
                               double maxRangeMeters,
                               double lockedSpeed, double baseSpeed,
                               double homingStartMeters,
                               const LockOnCategories* categories)
    {
        if (bulletId <= 0 || bulletId > 255)
        {
            LogDebug("[BulletLockOn] bulletId=%d rejected: the lock query reads the "
                     "held-weapon bullet id as one byte, so lockOn bullets must use "
                     "ids 1-255\n", bulletId);
            return;
        }
        LockOnSpec spec{};
        if (categories)
            TranslateCategories(*categories, spec);
        int c = lockCount;
        if (c < 1) c = 1;
        if (c > 8) c = 8;
        spec.count = static_cast<std::uint32_t>(c);
        double t = lockTimeSec;
        if (t < 0.001) t = 0.001;
        if (t > 1.0e6) t = 1.0e6;
        spec.time = static_cast<float>(t);
        double turn = turnRateDeg;
        if (turn < 0.0) turn = 0.0;
        if (turn > 1.0e6) turn = 1.0e6;
        spec.turnRadPerSec = static_cast<float>(turn * 0.0174532925);
        double rMin = minRangeMeters;
        if (rMin < 0.0) rMin = 0.0;
        if (rMin > 1.0e6) rMin = 1.0e6;
        spec.minRange = static_cast<float>(rMin);
        double rMax = maxRangeMeters;
        if (rMax < 0.0) rMax = 0.0;
        if (rMax > 1.0e6) rMax = 1.0e6;
        spec.maxRange = static_cast<float>(rMax);
        if (lockedSpeed > 0.0 && baseSpeed > 0.0)
        {
            double scale = lockedSpeed / baseSpeed;
            if (scale < 0.002) scale = 0.002;
            if (scale > 50.0) scale = 50.0;
            spec.speedScale = static_cast<float>(scale);
        }
        double hs = homingStartMeters;
        if (hs < 0.0) hs = 0.0;
        if (hs > 1.0e6) hs = 1.0e6;
        spec.homingStartDist = static_cast<float>(hs);

        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            g_SpecByBulletId[bulletId] = spec;
        }
        LogDebug("[BulletLockOn] bulletId=%d lock-on registered: count=%d "
            "time=%.2fs turnRate=%.0fdeg/s range=%.0f-%.0fm speed=%s "
            "homingStart=%.1fm\n",
            bulletId, c, t, turn, rMin, rMax > 0.0 ? rMax : 50.0,
            spec.speedScale > 0.0f ? "custom" : "inherit", hs);

        if (!g_ProviderHookTried || !g_OrigGetLockParam)
        {
            g_ProviderHookTried = true;
            if (!EnsureProviderVtableHook())
                LogDebug("[BulletLockOn] lock provider not hookable yet "
                         "(EquipParameterTablesImpl vtable not ready) - retrying on "
                         "the next registration\n");
        }
    }

    void LockOn_RegisterBulletType(int bulletId, int lockedType, int normalType)
    {
        if (bulletId <= 0 || bulletId > 255 || lockedType < 0 || normalType < 0)
            return;
        TypeSwap swap{};
        swap.lockedType = lockedType;
        swap.normalType = normalType;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            g_TypeByBulletId[bulletId] = swap;
        }
        LogDebug("[BulletLockOn] bulletId=%d locked bulletType=%d (normal=%d) "
            "registered\n", bulletId, lockedType, normalType);
    }

    bool LockOn_IsLockedNow()
    {
        return IsLockedNowInternal();
    }

    bool Install_BulletLockOn_Hooks()
    {
        g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
            ResolveGameAddress(gAddr.GetQuarkSystemTable));

        void* fnUpd   = ResolveGameAddress(gAddr.SightManager_UpdateMissileLockOn);
        void* fnSim   = ResolveGameAddress(gAddr.Bullet3_DoSimulation);
        void* fnAct   = ResolveGameAddress(gAddr.Bullet3_ActivateBulletAtEmptyWork);
        if (!gAddr.EquipParameterTablesImpl_Instance
            || !gAddr.SightManager_UpdateMissileLockOn
            || !gAddr.Bullet3_DoSimulation
            || !gAddr.Bullet3_ActivateBulletAtEmptyWork
            || !fnUpd || !fnSim || !fnAct
            || !g_GetQuarkSystemTable)
        {
            LogDebug("[BulletLockOn] not installed (addresses unresolved on this "
                "build) - SetBullet lockOn inactive.\n");
            return true;
        }

        const bool okU = CreateAndEnableHook(
            fnUpd, reinterpret_cast<void*>(&hkUpdateMissileLockOn),
            reinterpret_cast<void**>(&g_OrigUpdateLock));
        const bool okS = CreateAndEnableHook(
            fnSim, reinterpret_cast<void*>(&hkDoSimulation),
            reinterpret_cast<void**>(&g_OrigDoSim));
        const bool okA = CreateAndEnableHook(
            fnAct, reinterpret_cast<void*>(&hkActivateBullet),
            reinterpret_cast<void**>(&g_OrigActivate));

        const bool okP = EnsureProviderVtableHook();

        void* fnSU = ResolveGameAddress(gAddr.SightManager_Update);
        g_UpdateMissileLockOnUi = reinterpret_cast<UpdateLockUi_t>(
            ResolveGameAddress(gAddr.SightManager_UpdateMissileLockOnUi));
        bool okSU = false;
        if (fnSU && g_UpdateMissileLockOnUi)
            okSU = CreateAndEnableHook(
                fnSU, reinterpret_cast<void*>(&hkSightManagerUpdate),
                reinterpret_cast<void**>(&g_OrigSightUpdate));
        else
            g_UpdateMissileLockOnUi = nullptr;

        void* fnWC = ResolveGameAddress(gAddr.LockOnReticleFactory_CreateWindow);
        if (fnWC)
            CreateAndEnableHook(
                fnWC, reinterpret_cast<void*>(&hkLockWinCreate),
                reinterpret_cast<void**>(&g_OrigLockWinCreate));

        void* fnDF = ResolveGameAddress(gAddr.EquipObject_DoFire);
        bool okDF = false;
        if (fnDF)
            okDF = CreateAndEnableHook(
                fnDF, reinterpret_cast<void*>(&hkDoFire),
                reinterpret_cast<void**>(&g_OrigDoFire));

        Log("[BulletLockOn] hooks: lockProvider=%s sightUpdate=%s bulletSim=%s "
            "bulletActivate=%s reticleUi=%s doFire=%s "
            "(SetBullet lockOn={...} active)\n",
            okP ? "OK" : "deferred", okU ? "OK" : "FAIL",
            okS ? "OK" : "FAIL", okA ? "OK" : "FAIL",
            okSU ? "OK" : "off", okDF ? "OK" : "off");
        return okU && okS && okA;
    }

    void Uninstall_BulletLockOn_Hooks()
    {
        void* fnUpd   = ResolveGameAddress(gAddr.SightManager_UpdateMissileLockOn);
        void* fnSim   = ResolveGameAddress(gAddr.Bullet3_DoSimulation);
        void* fnAct   = ResolveGameAddress(gAddr.Bullet3_ActivateBulletAtEmptyWork);
        void* fnSU    = ResolveGameAddress(gAddr.SightManager_Update);
        void* fnWC    = ResolveGameAddress(gAddr.LockOnReticleFactory_CreateWindow);
        void* fnDF    = ResolveGameAddress(gAddr.EquipObject_DoFire);
        if (fnUpd)   DisableAndRemoveHook(fnUpd);
        if (fnSim)   DisableAndRemoveHook(fnSim);
        if (fnAct)   DisableAndRemoveHook(fnAct);
        if (fnSU && g_OrigSightUpdate) DisableAndRemoveHook(fnSU);
        if (fnWC && g_OrigLockWinCreate) DisableAndRemoveHook(fnWC);
        if (fnDF && g_OrigDoFire) DisableAndRemoveHook(fnDF);
        if (g_ProviderSlot && g_OrigGetLockParam)
        {
            DWORD oldProt = 0;
            if (VirtualProtect(g_ProviderSlot, sizeof(void*),
                               PAGE_READWRITE, &oldProt))
            {
                *g_ProviderSlot = reinterpret_cast<void*>(g_OrigGetLockParam);
                VirtualProtect(g_ProviderSlot, sizeof(void*), oldProt, &oldProt);
            }
        }
        if (g_QuerySlot && g_OrigQueryExec)
        {
            DWORD oldProt = 0;
            if (VirtualProtect(g_QuerySlot, sizeof(void*),
                               PAGE_READWRITE, &oldProt))
            {
                *g_QuerySlot = reinterpret_cast<void*>(g_OrigQueryExec);
                VirtualProtect(g_QuerySlot, sizeof(void*), oldProt, &oldProt);
            }
        }
        g_QuerySlot = nullptr;
        g_OrigQueryExec = nullptr;
        g_InOurLockQuery = false;
        g_ProviderSlot = nullptr;
        g_ProviderHookTried = false;
        g_OrigGetLockParam = nullptr;
        g_OrigSightUpdate = nullptr;
        g_UpdateMissileLockOnUi = nullptr;
        g_OrigLockWinCreate = nullptr;
        g_OurLockWindow = nullptr;
        g_WeDrewLockUi = false;
        g_LastLockId = 0xFFFF;
        g_LastLockStampMs = 0;
        g_LockedCount = 0;
        g_LockedStampMs = 0;
        g_VolleyCursor = 0;
        g_SightMgr = nullptr;
        g_SightMgrStampMs = 0;
        g_ActiveValid = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            g_TagsByImpl.clear();
            g_TypeByBulletId.clear();
        }
    }
}
