#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"
#include "InterrogationVoiceEvent.h"
#include "SoldierAkObjIdMap.h"
#include "SoldierObjectRtpc.h"
#include "SoldierVoiceTypeQuery.h"
#include "../sound/VoicePitchOverride.h"
#include "MissionCodeGuard.h"


namespace
{


    using ObjectActivate_t = void* (__fastcall*)(
        void* self,
        void* errOut,
        std::uint64_t a3,
        void* a4,
        void* a5,
        void* a6,
        std::uint64_t a7,
        std::uint64_t a8,
        std::uint64_t a9,
        std::uint64_t a10);


    using RegisterGameObject_t = std::uint32_t (__fastcall*)(
        void* self,
        void* errOut,
        void* audioGameObject,
        const char* name,
        std::uint32_t* outId,
        std::uint64_t a6);


    using CallVoiceImpl_t = void (__fastcall*)(
        void*         param_1,
        void*         param_2,
        std::uint32_t slot);


    static ObjectActivate_t        g_OrigObjectActivate     = nullptr;
    static RegisterGameObject_t    g_OrigRegisterGameObject = nullptr;
    static CallVoiceImpl_t         g_OrigCallVoiceImpl      = nullptr;
    static void*                   g_HookTargetActivate     = nullptr;
    static void*                   g_HookTargetRegister     = nullptr;
    static void*                   g_HookTargetCallVoice    = nullptr;
    static std::atomic<bool>       g_Installed{ false };


    static thread_local std::uint32_t t_CurrentSpeakingSlot = 0xFFFFFFFFu;

    static std::mutex                                            g_GoIdAkObjMutex;
    static std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_AkObjIdsByGoId;
    static std::unordered_map<std::uint32_t, float>              g_DesiredCentsByGoId;


    static std::mutex                                                 g_MapMutex;
    static std::unordered_map<void*, std::vector<std::uint32_t>>      g_AkObjIdsByObject;

    static std::mutex                  g_SoldierVoiceMutex;
    static std::vector<std::uint32_t>  g_SoldierVoiceAkObjIds;
    static std::atomic<float>          g_ActiveSoldierVoiceCents{ 0.0f };
    static std::atomic<bool>           g_HaveActiveSoldierVoiceCents{ false };

    static std::mutex                                            g_ControlLinkMutex;
    static std::unordered_map<void*, void*>                      g_SelfToParentControl;
    static std::unordered_map<void*, std::vector<std::uint32_t>> g_AkObjIdsByControl;
    static std::unordered_map<void*, float>                      g_PitchByControl;


    static thread_local void* t_CurrentParentControl = nullptr;


    static constexpr std::size_t kActiveStackCap = 16;
    static thread_local void*    t_ActiveObjectStack[kActiveStackCap] = {};
    static thread_local std::size_t t_ActiveObjectDepth = 0;


    static void* SehCallObjectActivate(void* self, void* errOut,
                                       std::uint64_t a3, void* a4, void* a5,
                                       void* a6, std::uint64_t a7,
                                       std::uint64_t a8, std::uint64_t a9,
                                       std::uint64_t a10)
    {
        if (!g_OrigObjectActivate) return nullptr;
        __try
        {
            return g_OrigObjectActivate(self, errOut, a3, a4, a5, a6,
                                        a7, a8, a9, a10);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }


    static void* __fastcall hk_ObjectActivate(void* self,
                                              void* errOut,
                                              std::uint64_t a3,
                                              void* a4,
                                              void* a5,
                                              void* a6,
                                              std::uint64_t a7,
                                              std::uint64_t a8,
                                              std::uint64_t a9,
                                              std::uint64_t a10)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return SehCallObjectActivate(self, errOut, a3, a4, a5, a6, a7, a8, a9, a10);

        const bool pushed = (self && t_ActiveObjectDepth < kActiveStackCap);
        if (pushed)
            t_ActiveObjectStack[t_ActiveObjectDepth++] = self;

        void* prevParentControl = t_CurrentParentControl;
        t_CurrentParentControl = a4;

        if (self && a4)
        {
            std::lock_guard<std::mutex> lock(g_ControlLinkMutex);
            g_SelfToParentControl[self] = a4;
        }

        void* result = SehCallObjectActivate(self, errOut, a3, a4, a5, a6,
                                             a7, a8, a9, a10);

        if (pushed
            && t_ActiveObjectDepth > 0
            && t_ActiveObjectStack[t_ActiveObjectDepth - 1] == self)
        {
            --t_ActiveObjectDepth;
        }
        t_CurrentParentControl = prevParentControl;

        try { TryApplyAllPendingSoldierPitches(); } catch (...) {}

        return result;
    }


    static void SehCallCallVoiceImpl(void* param_1, void* param_2,
                                     std::uint32_t slot)
    {
        if (!g_OrigCallVoiceImpl) return;
        __try
        {
            g_OrigCallVoiceImpl(param_1, param_2, slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }


    static void __fastcall hk_CallVoiceImpl(void* param_1, void* param_2,
                                            std::uint32_t slot)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
        {
            SehCallCallVoiceImpl(param_1, param_2, slot);
            return;
        }

        const std::uint32_t soldierIndex = GetSoldierIndexFromSoundSlot(slot);

        Debug_InterrogationVoice_NoteDequeue(param_2, slot);

        const std::uint32_t prev = t_CurrentSpeakingSlot;
        t_CurrentSpeakingSlot = soldierIndex;

        SehCallCallVoiceImpl(param_1, param_2, slot);

        t_CurrentSpeakingSlot = prev;
    }


    static std::uint32_t SehReadAkObjIdAtOffsetZero(const void* p)
    {
        if (!p) return 0;
        __try
        {
            return *reinterpret_cast<const std::uint32_t*>(p);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }


    static std::uint32_t __fastcall hk_RegisterGameObject(
        void*           self,
        void*           errOut,
        void*           audioGameObject,
        const char*     name,
        std::uint32_t*  outId,
        std::uint64_t   a6)
    {
        if (MissionCodeGuard::ShouldBypassHooks())
            return g_OrigRegisterGameObject
                ? g_OrigRegisterGameObject(self, errOut, audioGameObject, name, outId, a6)
                : 0u;

        const std::uint32_t result = g_OrigRegisterGameObject
            ? g_OrigRegisterGameObject(self, errOut, audioGameObject,
                                       name, outId, a6)
            : 0;

        std::uint32_t akObjId = SehReadAkObjIdAtOffsetZero(outId);
        if (!akObjId)
            akObjId = SehReadAkObjIdAtOffsetZero(audioGameObject);

        void* owner = (t_ActiveObjectDepth > 0)
            ? t_ActiveObjectStack[t_ActiveObjectDepth - 1]
            : nullptr;

        if (akObjId && owner)
        {
            bool isNewMapping = false;
            try
            {
                std::lock_guard<std::mutex> lock(g_MapMutex);
                auto& list = g_AkObjIdsByObject[owner];
                bool exists = false;
                for (auto v : list) { if (v == akObjId) { exists = true; break; } }
                if (!exists) { list.push_back(akObjId); isNewMapping = true; }
            }
            catch (...) {}

            const bool isSoldierVoice =
                (name && std::strcmp(name, "SoldierSdObj") == 0);

            if (isNewMapping && isSoldierVoice)
            {
                try
                {
                    std::lock_guard<std::mutex> lock(g_SoldierVoiceMutex);
                    g_SoldierVoiceAkObjIds.push_back(akObjId);
                }
                catch (...) {}

                const std::uint32_t currentSlot = t_CurrentSpeakingSlot;
                if (currentSlot != 0xFFFFFFFFu)
                {
                    const std::uint32_t goId = 0x0400u | (currentSlot & 0x01FFu);
                    float desiredCents = 0.0f;
                    bool  haveDesired  = false;
                    try
                    {
                        std::lock_guard<std::mutex> lock(g_GoIdAkObjMutex);
                        g_AkObjIdsByGoId[goId].push_back(akObjId);
                        const auto it = g_DesiredCentsByGoId.find(goId);
                        if (it != g_DesiredCentsByGoId.end())
                        {
                            desiredCents = it->second;
                            haveDesired  = true;
                        }
                    }
                    catch (...) {}

                    if (haveDesired && desiredCents != 0.0f)
                    {
                        Set_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId), desiredCents);
                    }
                }
            }

            if (isNewMapping)
                try { TryApplyAllPendingSoldierPitches(); } catch (...) {}
        }

        return result;
    }


}


namespace SoldierAkObjIdMap
{


    bool Install()
    {
        if (g_Installed.load(std::memory_order_relaxed))
            return true;

        const auto regAddr       = gAddr.Fox_Sd_Ad_AudioSoundEngine_RegisterGameObject;
        const auto activateAddr  = gAddr.Fox_Sd_Object_Activate;
        const auto callVoiceAddr = gAddr.SoundControllerImpl_CallInternal;

        if (!regAddr || !activateAddr)
            return false;

        void* regTarget      = ResolveGameAddress(regAddr);
        void* activateTarget = ResolveGameAddress(activateAddr);
        if (!regTarget || !activateTarget)
            return false;

        const bool okReg = CreateAndEnableHook(
            regTarget,
            reinterpret_cast<void*>(&hk_RegisterGameObject),
            reinterpret_cast<void**>(&g_OrigRegisterGameObject));
        if (!okReg)
            return false;
        g_HookTargetRegister = regTarget;

        const bool okAct = CreateAndEnableHook(
            activateTarget,
            reinterpret_cast<void*>(&hk_ObjectActivate),
            reinterpret_cast<void**>(&g_OrigObjectActivate));
        if (!okAct)
        {
            DisableAndRemoveHook(g_HookTargetRegister);
            g_HookTargetRegister     = nullptr;
            g_OrigRegisterGameObject = nullptr;
            return false;
        }
        g_HookTargetActivate = activateTarget;

        if (callVoiceAddr)
        {
            void* callVoiceTarget = ResolveGameAddress(callVoiceAddr);
            if (callVoiceTarget)
            {
                const bool okCall = CreateAndEnableHook(
                    callVoiceTarget,
                    reinterpret_cast<void*>(&hk_CallVoiceImpl),
                    reinterpret_cast<void**>(&g_OrigCallVoiceImpl));
                if (okCall)
                    g_HookTargetCallVoice = callVoiceTarget;
            }
        }

        g_Installed.store(true, std::memory_order_relaxed);
        return true;
    }


    bool Uninstall()
    {
        if (!g_Installed.load(std::memory_order_relaxed)) return true;

        if (g_HookTargetActivate)
        {
            DisableAndRemoveHook(g_HookTargetActivate);
            g_HookTargetActivate  = nullptr;
            g_OrigObjectActivate  = nullptr;
        }
        if (g_HookTargetRegister)
        {
            DisableAndRemoveHook(g_HookTargetRegister);
            g_HookTargetRegister     = nullptr;
            g_OrigRegisterGameObject = nullptr;
        }
        if (g_HookTargetCallVoice)
        {
            DisableAndRemoveHook(g_HookTargetCallVoice);
            g_HookTargetCallVoice  = nullptr;
            g_OrigCallVoiceImpl    = nullptr;
        }

        {
            std::lock_guard<std::mutex> lock(g_MapMutex);
            g_AkObjIdsByObject.clear();
        }

        g_Installed.store(false, std::memory_order_relaxed);
        return true;
    }


    std::vector<std::uint32_t> GetAkObjIdsForObject(void* object)
    {
        if (!object) return {};
        std::lock_guard<std::mutex> lock(g_MapMutex);
        const auto it = g_AkObjIdsByObject.find(object);
        if (it == g_AkObjIdsByObject.end()) return {};
        return it->second;
    }


    std::vector<std::uint32_t> GetAllSoldierVoiceAkObjIds()
    {
        std::lock_guard<std::mutex> lock(g_SoldierVoiceMutex);
        return g_SoldierVoiceAkObjIds;
    }


    void SetActiveSoldierVoiceCents(float cents)
    {
        g_ActiveSoldierVoiceCents.store(cents, std::memory_order_relaxed);
        g_HaveActiveSoldierVoiceCents.store(true, std::memory_order_relaxed);
    }


    void ClearActiveSoldierVoiceCents()
    {
        g_HaveActiveSoldierVoiceCents.store(false, std::memory_order_relaxed);
        g_ActiveSoldierVoiceCents.store(0.0f, std::memory_order_relaxed);
    }


    std::vector<std::uint32_t> GetAkObjIdsForControl(void* control)
    {
        if (!control) return {};
        std::lock_guard<std::mutex> lock(g_ControlLinkMutex);
        const auto it = g_AkObjIdsByControl.find(control);
        if (it == g_AkObjIdsByControl.end()) return {};
        return it->second;
    }


    void SetPitchForControl(void* control, float cents)
    {
        if (!control) return;
        std::vector<std::uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_ControlLinkMutex);
            g_PitchByControl[control] = cents;
            const auto it = g_AkObjIdsByControl.find(control);
            if (it != g_AkObjIdsByControl.end()) ids = it->second;
        }
        for (std::uint32_t akObjId : ids)
            Set_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId), cents);
    }


    void ClearPitchForControl(void* control)
    {
        if (!control) return;
        std::vector<std::uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_ControlLinkMutex);
            g_PitchByControl.erase(control);
            const auto it = g_AkObjIdsByControl.find(control);
            if (it != g_AkObjIdsByControl.end()) ids = it->second;
        }
        for (std::uint32_t akObjId : ids)
            Clear_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId));
    }


    void SetDesiredPitchForGoId(std::uint32_t goId, float cents)
    {
        std::vector<std::uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_GoIdAkObjMutex);
            g_DesiredCentsByGoId[goId] = cents;
            const auto it = g_AkObjIdsByGoId.find(goId);
            if (it != g_AkObjIdsByGoId.end()) ids = it->second;
        }
        for (std::uint32_t akObjId : ids)
            Set_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId), cents);
    }


    void ClearDesiredPitchForGoId(std::uint32_t goId)
    {
        std::vector<std::uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_GoIdAkObjMutex);
            g_DesiredCentsByGoId.erase(goId);
            const auto it = g_AkObjIdsByGoId.find(goId);
            if (it != g_AkObjIdsByGoId.end()) ids = it->second;
        }
        for (std::uint32_t akObjId : ids)
            Clear_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId));
    }


    void ClearAllDesiredPitches()
    {
        {
            std::lock_guard<std::mutex> lock(g_GoIdAkObjMutex);
            g_DesiredCentsByGoId.clear();
        }
        Clear_AllPerAkObjIdPitchBiases();
    }


    std::vector<std::uint32_t> GetAkObjIdsForGoId(std::uint32_t goId)
    {
        std::lock_guard<std::mutex> lock(g_GoIdAkObjMutex);
        const auto it = g_AkObjIdsByGoId.find(goId);
        if (it == g_AkObjIdsByGoId.end()) return {};
        return it->second;
    }


}
