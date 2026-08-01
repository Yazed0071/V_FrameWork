#include "pch.h"

#include <list>

#include "GetPhotoAdditionalTextLangId.h"
#include "ChangeLocationMenu.h"   // MotherBaseMissionCommonData

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "FoxHashes.h"
#include "MissionCodeGuard.h"

using GetPhotoAdditionalTextLangIdHook_t = unsigned long long(__thiscall*)(MotherBaseMissionCommonData* self, unsigned long long* ret, unsigned short missionCode, unsigned char photoId, unsigned char photoType);

static GetPhotoAdditionalTextLangIdHook_t g_OrigGetPhotoAdditionalTextLangIdHook = nullptr;

// missionCode + photoId + photoType.
static std::list<PhotoInfo> addPhotoInfos{};

unsigned long long __thiscall hkGetPhotoAdditionalTextLangId(MotherBaseMissionCommonData* self, unsigned long long* ret, unsigned short missionCode, unsigned char photoId, unsigned char photoType)
{
    if (MissionCodeGuard::ShouldBypassHooks())
        return g_OrigGetPhotoAdditionalTextLangIdHook(self, ret, missionCode, photoId, photoType);

    for (auto const& i : addPhotoInfos)
    {
        if (i.MissionCode == missionCode && i.PhotoId == photoId && i.PhotoType == photoType)
        {
            *ret = i.TargetTypeLangId;
            return *ret;
        }
    }

    return g_OrigGetPhotoAdditionalTextLangIdHook(self, ret, missionCode, photoId, photoType);
}

bool Install_PhotoAdditionalText_Hook()
{
    void* target = ResolveGameAddress(gAddr.GetPhotoAdditionalTextLangId);

    const bool okTarget = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkGetPhotoAdditionalTextLangId),
        reinterpret_cast<void**>(&g_OrigGetPhotoAdditionalTextLangIdHook));

#ifdef _DEBUG
    Log("[Hook] PhotoAdditionalText %d installed at %p\n", okTarget, target);
#endif
    return okTarget;
}

bool Uninstall_PhotoAdditionalText_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(gAddr.GetPhotoAdditionalTextLangId));
    g_OrigGetPhotoAdditionalTextLangIdHook = nullptr;
    return true;
}

void AddPhotoAdditionalText(unsigned short missionCode, unsigned char photoId, unsigned char photoType, const char* targetTypeLangIdStr)
{
    const unsigned long long hash = FoxHashes::StrCode64(targetTypeLangIdStr);

    for (auto it = addPhotoInfos.begin(); it != addPhotoInfos.end(); ++it)
    {
        if (it->MissionCode == missionCode && it->PhotoId == photoId && it->PhotoType == photoType)
        {
            it->TargetTypeLangId = hash;
            return;
        }
    }

    PhotoInfo photoInfo = { missionCode, photoId, static_cast<PHOTO_TYPE>(photoType), hash };
    addPhotoInfos.push_back(photoInfo);
}
