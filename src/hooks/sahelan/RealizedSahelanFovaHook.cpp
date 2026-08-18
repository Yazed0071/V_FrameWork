#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>

#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"
#include "FoxHashes.h"
#include "RealizedSahelanFovaHook.h"

namespace
{
    using SetFovaImpl_t = void(__fastcall*)(void* self, std::uint32_t param2);
    using Realize_t = void(__fastcall*)(void* self, std::uint32_t param2);

    using GetFvaByHash_t = std::int64_t(__fastcall*)(
        void* self, std::uint64_t hash, std::int64_t arg3);
    using GetModelForIndex_t = void* (__fastcall*)(void* self, std::uint32_t index);
    using ApplyFv2_t = bool(__fastcall*)(
        void* self, void* model, void* groupSettings, void* method,
        std::uint32_t flags, bool a6, bool a7);

    static SetFovaImpl_t g_OrigSetFovaImpl = nullptr;
    static Realize_t g_OrigRealize = nullptr;

    static constexpr std::uint64_t kVanillaFovaHash = 0x60887FE72AA5C04Bull;
    static constexpr std::uint16_t kVanillaMissionGate = 0x2B8Fu;

    static std::atomic<std::uint64_t> g_OverrideFovaHash{ 0 };
    static std::atomic<bool> g_HasFovaOverride{ false };
    static std::atomic<bool> g_BypassMissionCheck{ false };

    static void ApplyFovaWithHash(void* self, std::uint32_t param2, std::uint64_t hash)
    {
        if (!self)
            return;

        auto thisAsQwords = reinterpret_cast<std::int64_t*>(self);

        __try
        {
            const auto vtbl = reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(self));
            if (!vtbl)
                return;

            const auto getFva = reinterpret_cast<GetFvaByHash_t>(vtbl[0xC0 / sizeof(void*)]);
            if (!getFva)
                return;

            const std::int64_t arg3 = thisAsQwords[0x178 / sizeof(std::int64_t)];

            const std::int64_t fv2 = getFva(self, hash, arg3);

            thisAsQwords[0x190 / sizeof(std::int64_t)] = fv2;

            if (fv2 == 0)
                return;

            const std::int64_t childPtr = thisAsQwords[0x10 / sizeof(std::int64_t)];
            if (!childPtr)
                return;

            const auto childVtblBase = *reinterpret_cast<std::uintptr_t*>(childPtr);
            if (!childVtblBase)
                return;

            const auto childVtbl = reinterpret_cast<std::uintptr_t*>(childVtblBase);
            const auto getModel = reinterpret_cast<GetModelForIndex_t>(childVtbl[0x3F0 / sizeof(void*)]);
            if (!getModel)
                return;

            void* model = getModel(reinterpret_cast<void*>(childPtr), param2);

            const auto applyFn = reinterpret_cast<ApplyFv2_t>(
                ResolveGameAddress(gAddr.FormVariationFile2_ApplyOnlyMeshAndTextureVariation));
            if (!applyFn)
                return;

            applyFn(reinterpret_cast<void*>(fv2), model, nullptr, nullptr, 0u, true, true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogDebug("[SahelanFova] ApplyFovaWithHash: exception (self=%p, param2=%u, hash=0x%016llX)\n",
                self, param2, static_cast<unsigned long long>(hash));
        }
    }

    static void __fastcall hkSetFovaImpl(void* self, std::uint32_t param2)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigSetFovaImpl, self, param2);

        if (!g_HasFovaOverride.load(std::memory_order_relaxed))
        {
            if (g_OrigSetFovaImpl)
                g_OrigSetFovaImpl(self, param2);
            return;
        }

        const std::uint64_t hash = g_OverrideFovaHash.load(std::memory_order_relaxed);
        if (hash == 0)
        {
            if (g_OrigSetFovaImpl)
                g_OrigSetFovaImpl(self, param2);
            return;
        }

        ApplyFovaWithHash(self, param2, hash);
    }

    static void __fastcall hkRealize(void* self, std::uint32_t param2)
    {
        MISSION_GUARD_ORIGINAL_VOID(g_OrigRealize, self, param2);

        if (g_OrigRealize)
            g_OrigRealize(self, param2);

        if (!g_BypassMissionCheck.load(std::memory_order_relaxed))
            return;

        const std::uint16_t missionCode = MissionCodeGuard::GetCurrentMissionCode();
        if (missionCode == kVanillaMissionGate)
            return;

        hkSetFovaImpl(self, param2);
    }
}

bool Install_RealizedSahelanFova_Hook()
{
    void* setFovaTarget = ResolveGameAddress(gAddr.RealizedSahelan2Impl_SetFovaImpl);
    void* realizeTarget = ResolveGameAddress(gAddr.RealizedSahelan2Impl_Realize);

    if (!setFovaTarget || !realizeTarget)
    {
        Log("[Hook] RealizedSahelanFova: target resolve failed (SetFovaImpl=%p, Realize=%p)\n",
            setFovaTarget, realizeTarget);
        return false;
    }

    const bool okSetFova = CreateAndEnableHook(
        setFovaTarget,
        reinterpret_cast<void*>(&hkSetFovaImpl),
        reinterpret_cast<void**>(&g_OrigSetFovaImpl));

    const bool okRealize = CreateAndEnableHook(
        realizeTarget,
        reinterpret_cast<void*>(&hkRealize),
        reinterpret_cast<void**>(&g_OrigRealize));

#ifdef _DEBUG
    Log("[Hook] RealizedSahelanFova: SetFovaImpl=%s (target=%p), Realize=%s (target=%p)\n",
        okSetFova ? "OK" : "FAIL", setFovaTarget,
        okRealize ? "OK" : "FAIL", realizeTarget);
#endif

    return okSetFova && okRealize;
}

bool Uninstall_RealizedSahelanFova_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RealizedSahelan2Impl_SetFovaImpl));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.RealizedSahelan2Impl_Realize));
    g_OrigSetFovaImpl = nullptr;
    g_OrigRealize = nullptr;

#ifdef _DEBUG
    LogDebug("[Hook] RealizedSahelanFova: removed\n");
#endif
    return true;
}

void Set_SahelanFovaHash(std::uint64_t hash)
{
    g_OverrideFovaHash.store(hash, std::memory_order_relaxed);
    g_HasFovaOverride.store(hash != 0, std::memory_order_relaxed);

    if (hash != 0)
        g_BypassMissionCheck.store(true, std::memory_order_relaxed);

#ifdef _DEBUG
    LogDebug("[SahelanFova] hash override set to 0x%016llX (bypassMission=%s)\n",
        static_cast<unsigned long long>(hash),
        g_BypassMissionCheck.load() ? "ON" : "OFF");
#endif
}

void Set_SahelanFovaPath(const char* path)
{
    if (!path || !*path)
    {
        LogDebug("[SahelanFova] SetSahelanFovaPath: ignoring empty path\n");
        return;
    }

    const std::uint64_t hash = FoxHashes::PathCode64Ext(path);

    Set_SahelanFovaHash(hash);
}

void Clear_SahelanFovaOverride()
{
    g_OverrideFovaHash.store(0, std::memory_order_relaxed);
    g_HasFovaOverride.store(false, std::memory_order_relaxed);
    g_BypassMissionCheck.store(false, std::memory_order_relaxed);

#ifdef _DEBUG
    LogDebug("[SahelanFova] override cleared (vanilla restored)\n");
#endif
}

std::uint64_t Get_SahelanFovaHash()
{
    if (!g_HasFovaOverride.load(std::memory_order_relaxed))
        return kVanillaFovaHash;

    return g_OverrideFovaHash.load(std::memory_order_relaxed);
}
