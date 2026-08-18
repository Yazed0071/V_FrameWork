#include "pch.h"

#include <algorithm>
#include <list>

#include "ChangeLocationMenu.h"

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "MissionCodeGuard.h"

using GetChangeLocationMenuParameterByLocationId_t = ChangeLocationMenuParameter* (__thiscall*)(MotherBaseMissionCommonData* This, unsigned short locationCode);
using GetMbFreeChangeLocationMenuParameter_t = ChangeLocationMenuParameter* (__thiscall*)(MotherBaseMissionCommonData* This);

static GetChangeLocationMenuParameterByLocationId_t g_OrigGetChangeLocationMenuParameterByLocationId = nullptr;
static GetMbFreeChangeLocationMenuParameter_t g_OrigGetMbFreeChangeLocationMenuParameter = nullptr;

static std::list<unsigned short> changeLocationMenuIds{};

ChangeLocationMenuParameter* __thiscall hkGetChangeLocationMenuParameterByLocationId(MotherBaseMissionCommonData* This, unsigned short locationCode)
{
    if (MissionCodeGuard::ShouldBypassHooks())
        return g_OrigGetChangeLocationMenuParameterByLocationId(This, locationCode);

    ChangeLocationMenuParameter* params = This->ChangeLocationMenuParams;
    for (int i = 0; i < This->ChangeLocationMenuParamCount; i++)
    {
        if (std::find(changeLocationMenuIds.begin(), changeLocationMenuIds.end(), locationCode) != changeLocationMenuIds.end())
            if (params[i].LocationId == locationCode)
                return params + i;
    }

    return g_OrigGetChangeLocationMenuParameterByLocationId(This, locationCode);
}

bool Install_ChangeLocationMenu_Hook()
{
    void* target = ResolveGameAddress(gAddr.GetChangeLocationMenuParameterByLocationId);

    g_OrigGetMbFreeChangeLocationMenuParameter = reinterpret_cast<GetMbFreeChangeLocationMenuParameter_t>(
        ResolveGameAddress(gAddr.GetMbFreeChangeLocationMenuParameter));

    const bool okTarget = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGetChangeLocationMenuParameterByLocationId),
        reinterpret_cast<void**>(&g_OrigGetChangeLocationMenuParameterByLocationId));

#ifdef _DEBUG
    LogDebug("[Hook] ChangeLocationMenu %d installed at %p\n", okTarget, target);
#endif
    return okTarget;
}

bool Uninstall_ChangeLocationMenu_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.GetChangeLocationMenuParameterByLocationId));
    DisableAndRemoveHook(ResolveGameAddress(gAddr.GetMbFreeChangeLocationMenuParameter));
    g_OrigGetChangeLocationMenuParameterByLocationId = nullptr;
    g_OrigGetMbFreeChangeLocationMenuParameter = nullptr;
    return true;
}

void AddLocationIdToChangeLocationMenu(unsigned short locationId)
{
    if (std::find(changeLocationMenuIds.begin(), changeLocationMenuIds.end(), locationId) == changeLocationMenuIds.end())
    {
        changeLocationMenuIds.push_back(locationId);
    }
    else
    {
    }
}
