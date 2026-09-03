#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AddressSet.h"
#include "HookUtils.h"
#include "MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter.h"
#include "log.h"

namespace
{
    constexpr std::uint64_t kNativeRedStringId = 0x8b957b8d5e9full;

    constexpr std::uintptr_t kOff_PopupType  = 0x7c1;
    constexpr std::uintptr_t kOff_Count      = 0xac;
    constexpr std::uintptr_t kOff_ScrollB4   = 0xb4;
    constexpr std::uintptr_t kOff_ScrollB8   = 0xb8;
    constexpr std::uintptr_t kOff_RecordBase = 0x7e4;
    constexpr std::uintptr_t kRecordStride   = 0x28;
    constexpr std::uintptr_t kOff_PageData   = 0x38;
    constexpr std::uintptr_t kPage_LangSvc   = 0x20;
    constexpr std::uintptr_t kPage_Provider  = 0x48;

    constexpr std::uint8_t kPopupType_AcceptDeploy = 1;

    constexpr std::size_t kProviderIdx_GateB    = 0x6e8 / sizeof(void*);
    constexpr std::size_t kProviderIdx_GateC    = 0x6c0 / sizeof(void*);
    constexpr std::size_t kLangIdx_GetLangText  = 0x758 / sizeof(void*);

    using Activate_t    = void* (__fastcall*)(void* self);
    using GatePred_t    = char  (__fastcall*)(void* provider, std::uint32_t code);
    using GetLangText_t = const char* (__fastcall*)(void* langsvc, std::uint64_t stringId);
    using LangIdToKey_t = void* (__fastcall*)(std::uint64_t* out, const char* langId);

    struct RedOverride { std::uint64_t stringId; std::string colorName; };

    std::mutex g_ovMutex;
    std::unordered_map<std::uint16_t, RedOverride> g_overrides;

    struct Ctx { bool active; std::uint16_t code; std::uint64_t customId; char colorName[64]; };
    thread_local Ctx g_ctx{ false, 0, 0, {} };

    Activate_t    g_OrigActivate  = nullptr;
    GatePred_t    g_OrigGateB      = nullptr;
    GatePred_t    g_OrigGateC      = nullptr;
    GetLangText_t g_OrigGetLang    = nullptr;

    void* g_GateBTarget = nullptr;
    void* g_GateCTarget = nullptr;
    void* g_LangTarget  = nullptr;
    std::once_flag g_GateBOnce, g_GateCOnce, g_LangOnce;

    template <typename T>
    __forceinline bool SafeRead(const void* base, std::uintptr_t off, T& out)
    {
        __try
        {
            out = *reinterpret_cast<const T*>(
                reinterpret_cast<const std::uint8_t*>(base) + off);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    __forceinline void* SafeDeref(const void* base, std::uintptr_t off)
    {
        void* v = nullptr;
        return SafeRead(base, off, v) ? v : nullptr;
    }

    void* SafeVtableSlot(void* obj, std::size_t index)
    {
        if (!obj)
            return nullptr;
        __try
        {
            void** vtbl = *reinterpret_cast<void***>(obj);
            return vtbl ? vtbl[index] : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void* ProviderFromSelf(void* self)
    {
        void* page = SafeDeref(self, kOff_PageData);
        return page ? SafeDeref(page, kPage_Provider) : nullptr;
    }

    void* LangSvcFromSelf(void* self)
    {
        void* page = SafeDeref(self, kOff_PageData);
        return page ? SafeDeref(page, kPage_LangSvc) : nullptr;
    }

    std::uint64_t LangKeyFromString(const char* langId)
    {
        auto toKey = reinterpret_cast<LangIdToKey_t>(
            ResolveGameAddress(gAddr.Ui_LangIdToKey));
        if (!toKey || !langId)
            return 0;
        std::uint64_t key = 0;
        __try { toKey(&key, langId); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return key;
    }

    char __fastcall hk_GateB(void* p, std::uint32_t code)
    {
        if (g_ctx.active)
            return 1;
        return g_OrigGateB ? g_OrigGateB(p, code) : 0;
    }

    char __fastcall hk_GateC(void* p, std::uint32_t code)
    {
        if (g_ctx.active)
            return 0;
        return g_OrigGateC ? g_OrigGateC(p, code) : 0;
    }

    const char* __fastcall hk_GetLangText(void* svc, std::uint64_t id)
    {
        if (g_ctx.active && id == kNativeRedStringId)
        {
            const char* text = g_OrigGetLang ? g_OrigGetLang(svc, g_ctx.customId) : nullptr;
            if (g_ctx.colorName[0] && text)
            {
                thread_local std::string buf;
                buf.assign("><I=C=");
                buf.append(g_ctx.colorName);
                buf.append("|");
                buf.append(text);
                return buf.c_str();
            }
            return text;
        }
        return g_OrigGetLang ? g_OrigGetLang(svc, id) : nullptr;
    }

    void TryInstallGateB(void* self)
    {
        std::call_once(g_GateBOnce, [self]()
        {
            void* fn = SafeVtableSlot(ProviderFromSelf(self), kProviderIdx_GateB);
            if (!fn ||
                !CreateAndEnableHook(fn, reinterpret_cast<void*>(&hk_GateB),
                                     reinterpret_cast<void**>(&g_OrigGateB)))
            {
                Log("[MissionDeployWarning] ERROR: warn-gate hook install failed - "
                    "override missions will not show a forced warning.\n");
                return;
            }
            g_GateBTarget = fn;
        });
    }

    void TryInstallGateC(void* self)
    {
        std::call_once(g_GateCOnce, [self]()
        {
            void* fn = SafeVtableSlot(ProviderFromSelf(self), kProviderIdx_GateC);
            if (!fn ||
                !CreateAndEnableHook(fn, reinterpret_cast<void*>(&hk_GateC),
                                     reinterpret_cast<void**>(&g_OrigGateC)))
            {
                Log("[MissionDeployWarning] WARN: hard-mission gate hook install failed - "
                    "override may be suppressed for hard-type missions.\n");
                return;
            }
            g_GateCTarget = fn;
        });
    }

    void TryInstallLang(void* self)
    {
        std::call_once(g_LangOnce, [self]()
        {
            void* fn = SafeVtableSlot(LangSvcFromSelf(self), kLangIdx_GetLangText);
            if (!fn ||
                !CreateAndEnableHook(fn, reinterpret_cast<void*>(&hk_GetLangText),
                                     reinterpret_cast<void**>(&g_OrigGetLang)))
            {
                Log("[MissionDeployWarning] ERROR: lang-text hook install failed - "
                    "override warning text will not be substituted.\n");
                return;
            }
            g_LangTarget = fn;
        });
    }

    void* CallActivateRestoringCtx(void* self, const Ctx& saved)
    {
        void* r = nullptr;
        __try   { r = g_OrigActivate(self); }
        __finally { g_ctx = saved; }
        return r;
    }

    void* __fastcall hk_Activate(void* self)
    {
        if (!self || !g_OrigActivate)
            return g_OrigActivate ? g_OrigActivate(self) : nullptr;

        std::uint8_t type = 0;
        if (!SafeRead(self, kOff_PopupType, type) || type != kPopupType_AcceptDeploy)
            return g_OrigActivate(self);

        std::uint16_t code = 0xFFFF;
        std::uint32_t count = 0, b4 = 0, b8 = 0;
        if (SafeRead(self, kOff_Count, count) && count &&
            SafeRead(self, kOff_ScrollB4, b4) && SafeRead(self, kOff_ScrollB8, b8))
        {
            const std::uint32_t idx = (b8 + b4) % count;
            SafeRead(self, idx * kRecordStride + kOff_RecordBase, code);
        }

        std::uint64_t customId = 0;
        char colorName[64] = {};
        {
            std::lock_guard<std::mutex> lk(g_ovMutex);
            const auto it = g_overrides.find(code);
            if (it != g_overrides.end())
            {
                customId = it->second.stringId;
                strncpy_s(colorName, it->second.colorName.c_str(), _TRUNCATE);
            }
        }

        if (!customId)
            return g_OrigActivate(self);

        TryInstallGateB(self);
        TryInstallGateC(self);
        TryInstallLang(self);

        const Ctx saved = g_ctx;
        g_ctx.active = true;
        g_ctx.code = code;
        g_ctx.customId = customId;
        strncpy_s(g_ctx.colorName, colorName, _TRUNCATE);
        return CallActivateRestoringCtx(self, saved);
    }
}

bool Set_MissionDeployWarning(std::uint16_t missionCode, const char* langId, const char* colorName)
{
    std::uint64_t key = kNativeRedStringId;
    if (langId && *langId)
    {
        key = LangKeyFromString(langId);
        if (!key)
        {
            Log("[MissionDeployWarning] WARN: langId '%s' resolved to 0 - override for mission "
                "%u not set.\n", langId, static_cast<unsigned>(missionCode));
            return false;
        }
    }

    std::lock_guard<std::mutex> lk(g_ovMutex);
    RedOverride& ov = g_overrides[missionCode];
    ov.stringId = key;
    ov.colorName = (colorName && *colorName) ? std::string(colorName) : std::string();
    return true;
}

void Clear_AllMissionDeployWarnings()
{
    std::lock_guard<std::mutex> lk(g_ovMutex);
    g_overrides.clear();
}

bool Install_MissionDeployWarning_Hook()
{
    void* target = ResolveGameAddress(gAddr.MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter);
    if (!target)
        return true;

    if (!CreateAndEnableHook(target, reinterpret_cast<void*>(&hk_Activate),
                             reinterpret_cast<void**>(&g_OrigActivate)))
    {
        Log("[MissionDeployWarning] ERROR: builder hook install failed - custom mission "
            "deploy warnings will not appear.\n");
        return false;
    }
    return true;
}

bool Uninstall_MissionDeployWarning_Hook()
{
    if (gAddr.MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter)
        DisableAndRemoveHook(ResolveGameAddress(
            gAddr.MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter));
    if (g_GateBTarget) DisableAndRemoveHook(g_GateBTarget);
    if (g_GateCTarget) DisableAndRemoveHook(g_GateCTarget);
    if (g_LangTarget)  DisableAndRemoveHook(g_LangTarget);

    g_OrigActivate = nullptr;
    g_OrigGateB = nullptr;
    g_OrigGateC = nullptr;
    g_OrigGetLang = nullptr;
    g_GateBTarget = g_GateCTarget = g_LangTarget = nullptr;

    {
        std::lock_guard<std::mutex> lk(g_ovMutex);
        g_overrides.clear();
    }
    return true;
}
