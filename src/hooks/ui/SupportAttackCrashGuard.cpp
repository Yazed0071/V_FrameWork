#include "pch.h"
#include "SupportAttackCrashGuard.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    using FireLoopFn = void* (__fastcall*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
    using ClassifyFn = unsigned char (__fastcall*)(unsigned int, std::uint64_t, std::uint64_t);
    using FactoryFn  = void* (__fastcall*)();
    using AllocFn    = void* (__fastcall*)(std::uint32_t, std::uint32_t, std::uint32_t);
    using InitFn     = void  (__fastcall*)(void*, void*);
    using VfxFactoryFn      = void* (__fastcall*)(void*, void*);
    using BackLinkFactoryFn = void* (__fastcall*)(void*);
    using PoolResetFn       = void  (__fastcall*)(void*);
    using OneShotFn         = void* (__fastcall*)(void*, void*, void*, void*, void*, std::uint64_t);

    static FireLoopFn        g_OrigFireLoop   = nullptr;
    static ClassifyFn        g_OrigClassify   = nullptr;
    static VfxFactoryFn      g_OrigVfxFactory = nullptr;
    static FactoryFn         g_Factory        = nullptr;
    static AllocFn           g_Alloc          = nullptr;
    static InitFn            g_Init           = nullptr;
    static BackLinkFactoryFn g_BLFactory      = nullptr;

    static void* g_FireAddr = nullptr;
    static void* g_ClsAddr  = nullptr;
    static void* g_VfxAddr  = nullptr;

    static OneShotFn g_OrigOneShot  = nullptr;
    static void*     g_DirGuardAddr = nullptr;

    static void* g_DarkMatterRes[2] = { nullptr, nullptr };
    static void* g_RealBackLink     = nullptr;
    static bool  g_RealBLFailed     = false;

    static bool g_EnableCloudRender = false;

    static constexpr std::uint64_t kVfxHash0      = 0x660A8DBEBB9BBF06ull;
    static constexpr std::uint64_t kVfxHash1      = 0x660A668A5B31CE27ull;
    static constexpr std::uint64_t kComponentHash = 0xf87fb991980aull;

    static std::int64_t __fastcall BL_Create (void* /*self*/, void* /*params*/) { return 0; }
    static void         __fastcall BL_Destroy(void* /*self*/, std::uint32_t /*id*/) {}
    static void         __fastcall BL_Frame  (void* /*self*/, void* /*a*/, void* /*b*/) {}
    static void*        __fastcall BL_Generic(void* /*self*/) { return nullptr; }

    static void* g_BackLinkVtable[16];
    struct StubBackLink { void* vtable; };
    static StubBackLink g_StubBackLink;
    static void*        g_StubBackLinkAddr = &g_StubBackLink;

    static thread_local bool g_Suppress = false;

    static bool g_SynthFailed  = false;

    static void* BuildSupportAttackArray(std::uint32_t count)
    {
        if (!g_Factory || !g_Alloc || count == 0 || count > 64)
            return nullptr;

        void** arr = reinterpret_cast<void**>(g_Alloc(count * 8u, 8u, 0x2005eu));
        if (!arr)
            return nullptr;

        for (std::uint32_t i = 0; i < count; ++i)
        {
            void* obj = nullptr;
            __try { obj = g_Factory(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { obj = nullptr; }

            if (!obj)
                return nullptr;

            if (!g_Init)
            {
                __try { g_Init = reinterpret_cast<InitFn>((*reinterpret_cast<void***>(obj))[1]); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_Init = nullptr; }
            }

            arr[i] = reinterpret_cast<std::uint8_t*>(obj) + 0x20;
        }
        return arr;
    }

    static bool EnsureRegistered(void* objArr, std::uint32_t count)
    {
        if (!g_Init || !objArr || count == 0 || count > 64)
            return false;

        void** arr = reinterpret_cast<void**>(objArr);
        bool allReg = true;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            __try
            {
                void* component = arr[i];
                if (component)
                {
                    std::uint8_t* inner = reinterpret_cast<std::uint8_t*>(component) - 0x20;
                    if (*reinterpret_cast<void**>(inner + 0xE8) == nullptr)
                    {
                        g_Init(inner, &g_StubBackLinkAddr);
                        if (*reinterpret_cast<void**>(inner + 0xE8) == nullptr)
                            allReg = false;
                    }
                }
                else
                {
                    allReg = false;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { allReg = false; }
        }
        return allReg;
    }

    static void BuildSource(void* self, void* objArr, std::uint32_t count)
    {
        if (!g_Alloc || !self || !objArr || count == 0 || count > 64)
            return;

        std::uint8_t* w        = reinterpret_cast<std::uint8_t*>(self);
        void*         entBlock = g_Alloc(count * 0x20u, 8u, 0x2005eu);
        void**        srcArr   = reinterpret_cast<void**>(g_Alloc(count * 8u, 8u, 0x2005eu));
        if (!entBlock || !srcArr)
            return;

        void** oa = reinterpret_cast<void**>(objArr);
        __try
        {
            for (std::uint32_t i = 0; i < count; ++i)
            {
                void*          component = oa[i];
                std::uint8_t*  inner     = component ? reinterpret_cast<std::uint8_t*>(component) - 0x20 : nullptr;
                std::uint64_t* ent       = reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uint8_t*>(entBlock) + i * 0x20);

                ent[0] = 0;
                ent[1] = 0;
                ent[2] = reinterpret_cast<std::uint64_t>(inner);
                ent[3] = 0;
                srcArr[i] = ent;

                if (inner)
                {
                    *reinterpret_cast<std::uint64_t*>(inner + 0x08) = kComponentHash;
                    *reinterpret_cast<void**>(inner + 0x10)         = ent;
                    *reinterpret_cast<std::int32_t*>(inner + 0x80)  = -1;
                    *reinterpret_cast<std::uint8_t*>(inner + 0x90)  = 0xFF;
                }
            }

            *reinterpret_cast<void**>(w + 0xF8) = srcArr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void* BuildRealBackLink()
    {
        if (!g_BLFactory || !g_DarkMatterRes[0] || !g_DarkMatterRes[1])
            return nullptr;

        std::uint32_t config[6] = { 12u, 2u, 0u, 0u, 0u, 0u };
        void* bl = nullptr;
        __try { bl = g_BLFactory(config); }
        __except (EXCEPTION_EXECUTE_HANDLER) { bl = nullptr; }
        if (!bl)
            return nullptr;

        bool ok = false;
        __try
        {
            std::uint8_t*  b = reinterpret_cast<std::uint8_t*>(bl);
            std::uint64_t* t = *reinterpret_cast<std::uint64_t**>(b + 0x18);
            if (t)
            {
                t[0] = reinterpret_cast<std::uint64_t>(g_DarkMatterRes[0]); t[1] = kVfxHash0; t[2] = 0;
                t[3] = reinterpret_cast<std::uint64_t>(g_DarkMatterRes[1]); t[4] = kVfxHash1; t[5] = 0;
                if (*reinterpret_cast<std::uint32_t*>(b + 0xC) == 0)
                    *reinterpret_cast<std::uint32_t*>(b + 0xC) = 12;
                auto reset = reinterpret_cast<PoolResetFn>((*reinterpret_cast<void***>(bl))[6]);
                if (reset) reset(bl);
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

        if (!ok)
            Log("[DarkMatter] WARNING: could not build the support-attack effect pool; the cloud may not render.\n");
        return ok ? bl : nullptr;
    }

    static void* __fastcall hk_FireLoop(void* self, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4)
    {
        const bool was = g_Suppress;
        bool suppress = false;
        bool touched  = false;
        std::uint8_t* w = reinterpret_cast<std::uint8_t*>(self);

        if (w)
        {
            void* objArr = nullptr;
            void* srcArr = nullptr;
            std::uint32_t count = 0;
            bool read = false;
            __try
            {
                objArr = *reinterpret_cast<void**>(w + 0x100);
                srcArr = *reinterpret_cast<void**>(w + 0xF8);
                count  = *reinterpret_cast<std::uint32_t*>(w + 0x50);
                read   = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}

            if (read && objArr == nullptr)
            {
                void* built = g_SynthFailed ? nullptr : BuildSupportAttackArray(count);
                if (built)
                {
                    __try { *reinterpret_cast<void**>(w + 0x100) = built; objArr = built; touched = true; }
                    __except (EXCEPTION_EXECUTE_HANDLER) { objArr = nullptr; }
                }
                if (objArr == nullptr)
                    suppress = true;
            }
            else if (read && objArr != nullptr && srcArr == nullptr && !g_SynthFailed)
            {
                touched = true;
                if (EnsureRegistered(objArr, count))
                    BuildSource(self, objArr, count);
            }

            if (g_EnableCloudRender && read && objArr != nullptr)
            {
                if (g_DarkMatterRes[0] && g_DarkMatterRes[1] && !g_RealBackLink && !g_RealBLFailed)
                {
                    g_RealBackLink = BuildRealBackLink();
                    if (!g_RealBackLink) g_RealBLFailed = true;
                }
                if (g_RealBackLink)
                {
                    touched = true;
                    __try
                    {
                        void** oa = reinterpret_cast<void**>(objArr);
                        for (std::uint32_t i = 0; i < count && i < 64; ++i)
                        {
                            void* comp = oa[i];
                            if (!comp) continue;
                            std::uint8_t* inner = reinterpret_cast<std::uint8_t*>(comp) - 0x20;
                            if (*reinterpret_cast<void**>(inner + 0x88) == &g_StubBackLink)
                            {
                                *reinterpret_cast<void**>(inner + 0x88) = g_RealBackLink;
                                if (*reinterpret_cast<std::uint32_t*>(inner + 0x28) >= 2)
                                {
                                    *reinterpret_cast<std::int32_t*>(inner + 0x80) = -1;
                                    *reinterpret_cast<std::uint8_t*>(inner + 0x90) = 0xFF;
                                    *reinterpret_cast<std::uint32_t*>(inner + 0x28) = 1;
                                }
                            }
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
            }
        }

        g_Suppress = suppress;

        void* r = nullptr;
        __try
        {
            r = g_OrigFireLoop ? g_OrigFireLoop(self, a2, a3, a4) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (touched && w)
            {
                __try { *reinterpret_cast<void**>(w + 0x100) = nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                __try { *reinterpret_cast<void**>(w + 0xF8)  = nullptr; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                g_SynthFailed = true;
                Log("[DarkMatter] ERROR: a support-attack weapon's wiring faulted on fire; reverted to a no-crash fallback (no cloud).\n");
            }
        }

        g_Suppress = was;
        return r;
    }

    static unsigned char __fastcall hk_Classify(unsigned int attackType, std::uint64_t a2, std::uint64_t a3)
    {
        if (g_Suppress)
            return 0;
        return g_OrigClassify ? g_OrigClassify(attackType, a2, a3) : 0;
    }

    static void* __fastcall hk_VfxFactory(void* self, void* desc)
    {
        void* res = g_OrigVfxFactory ? g_OrigVfxFactory(self, desc) : nullptr;
        if (desc && res)
        {
            __try
            {
                std::uint64_t* d = reinterpret_cast<std::uint64_t*>(desc);
                for (int i = 0; i < 16; ++i)
                {
                    const int slot = (d[i] == kVfxHash0) ? 0 : (d[i] == kVfxHash1) ? 1 : -1;
                    if (slot < 0)
                        continue;

                    void* rp = *reinterpret_cast<void**>(res);
                    if (!rp)
                        continue;

                    g_DarkMatterRes[slot] = rp;
                    if (g_RealBackLink)
                    {
                        std::uint64_t* t = *reinterpret_cast<std::uint64_t**>(
                            reinterpret_cast<std::uint8_t*>(g_RealBackLink) + 0x18);
                        if (t) t[slot * 3] = reinterpret_cast<std::uint64_t>(rp);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return res;
    }

    static void* __fastcall hk_OneShotDirGuard(void* a1, void* a2, void* a3, void* a4, void* dir, std::uint64_t flag)
    {
        const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(dir);
        if (v != 0 && (v < 0x10000ull || v > 0x00007FFFFFFFFFFFull))
            dir = nullptr;
        return g_OrigOneShot ? g_OrigOneShot(a1, a2, a3, a4, dir, flag) : nullptr;
    }
}

bool Install_SupportAttackCrashGuard()
{
    g_Factory   = reinterpret_cast<FactoryFn>(ResolveGameAddress(gAddr.Dm_ComponentFactory));
    g_BLFactory = reinterpret_cast<BackLinkFactoryFn>(ResolveGameAddress(gAddr.Dm_BackLinkPool));
    g_Alloc     = reinterpret_cast<AllocFn>(ResolveGameAddress(gAddr.Dm_Alloc));

    void* fire = ResolveGameAddress(gAddr.Dm_FireLoop);
    void* cls  = ResolveGameAddress(gAddr.Dm_Classify);
    void* vfx  = ResolveGameAddress(gAddr.Dm_VfxFactory);

    if (!fire || !cls || !vfx || !g_Factory || !g_BLFactory || !g_Alloc)
    {
        Log("[DarkMatter] ERROR: missing hardcoded addresses on %s (fire:%p cls:%p vfx:%p fac:%p pool:%p alloc:%p); DarkMatter support-attack disabled.\n",
            GetGameBuildName(gGameBuild), fire, cls, vfx,
            reinterpret_cast<void*>(g_Factory), reinterpret_cast<void*>(g_BLFactory), reinterpret_cast<void*>(g_Alloc));
        return false;
    }

    for (auto& e : g_BackLinkVtable) e = reinterpret_cast<void*>(&BL_Generic);
    g_BackLinkVtable[3] = reinterpret_cast<void*>(&BL_Create);
    g_BackLinkVtable[4] = reinterpret_cast<void*>(&BL_Destroy);
    g_BackLinkVtable[7] = reinterpret_cast<void*>(&BL_Frame);
    g_StubBackLink.vtable = g_BackLinkVtable;

    const bool fireOk = CreateAndEnableHook(fire, reinterpret_cast<void*>(&hk_FireLoop),   reinterpret_cast<void**>(&g_OrigFireLoop));
    const bool clsOk  = CreateAndEnableHook(cls,  reinterpret_cast<void*>(&hk_Classify),   reinterpret_cast<void**>(&g_OrigClassify));
    const bool vfxOk  = CreateAndEnableHook(vfx,  reinterpret_cast<void*>(&hk_VfxFactory), reinterpret_cast<void**>(&g_OrigVfxFactory));
    g_FireAddr = fireOk ? fire : nullptr;
    g_ClsAddr  = clsOk  ? cls  : nullptr;
    g_VfxAddr  = vfxOk  ? vfx  : nullptr;

    void* oneShot = ResolveGameAddress(gAddr.Dm_OneShot);
    bool dirGuardOk = false;
    if (oneShot)
    {
        dirGuardOk = CreateAndEnableHook(oneShot, reinterpret_cast<void*>(&hk_OneShotDirGuard),
                                         reinterpret_cast<void**>(&g_OrigOneShot));
        g_DirGuardAddr = dirGuardOk ? oneShot : nullptr;
        if (dirGuardOk)
            g_EnableCloudRender = true;
        else
            Log("[DarkMatter] WARNING: could not attach the crash-guard on the effect creator (0x%llX); the DM cloud is left OFF to avoid the random crash.\n",
                static_cast<unsigned long long>(gAddr.Dm_OneShot));
    }
    else
    {
        Log("[DarkMatter] WARNING: no effect-creator address for %s; the crash-guard is OFF and the DarkMatter cloud is disabled to avoid the random crash.\n",
            GetGameBuildName(gGameBuild));
    }

    if (!fireOk || !clsOk)
        Log("[DarkMatter] ERROR: support-attack hooks failed to install (fire:%s cls:%s); DarkMatter weapons disabled.\n",
            fireOk ? "ok" : "FAIL", clsOk ? "ok" : "FAIL");
    else if (!vfxOk)
        Log("[DarkMatter] WARNING: the effect-resource hook failed to install; the DarkMatter cloud may not render.\n");

    return fireOk && clsOk;
}

void Uninstall_SupportAttackCrashGuard()
{
    if (g_FireAddr) DisableAndRemoveHook(g_FireAddr);
    if (g_ClsAddr)  DisableAndRemoveHook(g_ClsAddr);
    if (g_VfxAddr)  DisableAndRemoveHook(g_VfxAddr);
    if (g_DirGuardAddr) DisableAndRemoveHook(g_DirGuardAddr);
    g_OrigFireLoop   = nullptr;
    g_OrigClassify   = nullptr;
    g_OrigVfxFactory = nullptr;
    g_OrigOneShot    = nullptr;
    g_Factory   = nullptr;
    g_Alloc     = nullptr;
    g_Init      = nullptr;
    g_BLFactory = nullptr;
    g_FireAddr = g_ClsAddr = g_VfxAddr = g_DirGuardAddr = nullptr;
}
