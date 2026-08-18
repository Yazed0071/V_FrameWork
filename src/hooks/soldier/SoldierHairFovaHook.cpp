#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "HookUtils.h"
#include "MissionCodeGuard.h"
#include "AddressSet.h"
#include "log.h"
#include "SoldierHairFovaHook.h"

namespace
{
    using FnFaceUpdate_t    = void(__fastcall*)(void* self, std::uint32_t soldierIndex, std::uint32_t faceParam, void* soldierData);
    using FnHeadEquipType_t = std::uint64_t(__fastcall*)(void* self, std::uint32_t soldierIndex, void* group, std::uint32_t groupCount);
    using FnGetFovaResMgr_t = void* (__fastcall*)(void* fovaController, std::uint32_t faceParam, std::uint32_t type);
    using FnGetFovaModel_t  = void* (__fastcall*)(void* resMgr, int a2, std::uint32_t a3, std::uint32_t a4, bool a5);

    FnFaceUpdate_t    g_OrigFaceUpdate   = nullptr;
    FnHeadEquipType_t g_GetHeadEquipType = nullptr;
    FnGetFovaResMgr_t g_GetFovaResMgr    = nullptr;
    FnGetFovaModel_t  g_GetFovaModel     = nullptr;

    constexpr std::uint64_t  kNoHeadEquipModelType = 0x2e;
    constexpr std::uint32_t  kHairFovaType         = 2;
    constexpr std::uintptr_t kModelVisMaskOffset   = 0x1a4;

    bool ResolveHelpers()
    {
        if (!g_GetHeadEquipType && gAddr.RealizedSoldier2Impl_ConvertHeadEquipModelType)
            g_GetHeadEquipType = reinterpret_cast<FnHeadEquipType_t>(
                ResolveGameAddress(gAddr.RealizedSoldier2Impl_ConvertHeadEquipModelType));
        if (!g_GetFovaResMgr && gAddr.FovaController_GetActiveFovaResourceManager)
            g_GetFovaResMgr = reinterpret_cast<FnGetFovaResMgr_t>(
                ResolveGameAddress(gAddr.FovaController_GetActiveFovaResourceManager));
        if (!g_GetFovaModel && gAddr.Fv2ResourceManager_GetModel)
            g_GetFovaModel = reinterpret_cast<FnGetFovaModel_t>(
                ResolveGameAddress(gAddr.Fv2ResourceManager_GetModel));
        return g_GetHeadEquipType && g_GetFovaResMgr && g_GetFovaModel;
    }

    bool SoldierHasHeadEquip(void* self, std::uint32_t soldierIndex)
    {
        if (soldierIndex == 0x1ff)
            return false;
        __try { return g_GetHeadEquipType(self, soldierIndex, nullptr, 0) != kNoHeadEquipModelType; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void __fastcall hk_UpdateFaceFovaMesh(void* self, std::uint32_t soldierIndex, std::uint32_t faceParam, void* soldierData)
    {
        if (g_OrigFaceUpdate)
            g_OrigFaceUpdate(self, soldierIndex, faceParam, soldierData);

        if (MissionCodeGuard::ShouldBypassHooks() || !ResolveHelpers())
            return;

        __try
        {
            void* fovaController = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(self) + 0x260);
            if (!fovaController)
                return;
            void* hairMgr = g_GetFovaResMgr(fovaController, faceParam, kHairFovaType);
            if (!hairMgr)
                return;
            void* hairModel = g_GetFovaModel(hairMgr, -1, 0, 0, true);
            if (!hairModel)
                return;

            const bool helmet = SoldierHasHeadEquip(self, soldierIndex);
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(hairModel) + kModelVisMaskOffset) =
                helmet ? 0xFFFFFFFFu : 0u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    bool HookOne(std::uintptr_t addr, void* detour, void** orig)
    {
        if (!addr)
            return true;
        void* target = ResolveGameAddress(addr);
        return target && CreateAndEnableHook(target, detour, orig);
    }
}

bool Install_SoldierHairFova_Hook()
{
    ResolveHelpers();
    const bool ok = HookOne(gAddr.RealizedSoldier2Impl_UpdateHeadEquipMesh,
                            reinterpret_cast<void*>(&hk_UpdateFaceFovaMesh),
                            reinterpret_cast<void**>(&g_OrigFaceUpdate));

#ifdef _DEBUG
    LogDebug("[HairFova] install: FaceUpdate=%s GetHeadEquip=%s GetFovaMgr=%s GetFovaModel=%s\n",
        gAddr.RealizedSoldier2Impl_UpdateHeadEquipMesh ? "set" : "0",
        g_GetHeadEquipType ? "ok" : "NULL",
        g_GetFovaResMgr ? "ok" : "NULL",
        g_GetFovaModel ? "ok" : "NULL");
#endif
    return ok;
}

bool Uninstall_SoldierHairFova_Hook()
{
    if (gAddr.RealizedSoldier2Impl_UpdateHeadEquipMesh)
        DisableAndRemoveHook(ResolveGameAddress(gAddr.RealizedSoldier2Impl_UpdateHeadEquipMesh));
    g_OrigFaceUpdate = nullptr;
    return true;
}
