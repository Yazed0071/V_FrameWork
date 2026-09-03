#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
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


    static constexpr std::uint32_t kNoSoundSlot = 0xFFFFFFFFu;

    static thread_local std::uint32_t t_CurrentSpeakingSlot = kNoSoundSlot;
    static thread_local std::uint32_t t_CurrentSoundSlotRaw = kNoSoundSlot;

    struct VoiceLineIdentity
    {
        std::uint32_t event;
        std::uint32_t voiceType;
        std::uint32_t voiceParam;
        std::uint32_t category;
    };

    static constexpr std::uint32_t kVoiceTypeCommandPost         = 0xCC8D2DC8u;
    static constexpr std::uint32_t kVoiceTypeCommandPostSoviet   = 0xA14739A0u;
    static constexpr std::uint32_t kVoiceTypeCommandPostAfrican  = 0x8E471B8Au;

    static bool IsCommandPostOwnVoiceType(std::uint32_t voiceType)
    {
        return voiceType == kVoiceTypeCommandPost
            || voiceType == kVoiceTypeCommandPostSoviet
            || voiceType == kVoiceTypeCommandPostAfrican;
    }

    static thread_local void*             t_CurrentVoiceController = nullptr;
    static thread_local VoiceLineIdentity t_CurrentVoiceLine       = {};

    static std::mutex                                            g_GoIdAkObjMutex;
    static std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_AkObjIdsByGoId;
    static std::unordered_map<std::uint32_t, float>              g_DesiredCentsByGoId;


    static std::mutex                                                 g_MapMutex;
    static std::unordered_map<void*, std::vector<std::uint32_t>>      g_AkObjIdsByObject;

    static std::mutex                                            g_EmitterNameMutex;
    static std::unordered_map<std::uint32_t, std::string>        g_EmitterNameByAkObjId;

    static std::mutex                  g_SoldierVoiceMutex;
    static std::vector<std::uint32_t>  g_SoldierVoiceAkObjIds;
    static std::atomic<float>          g_ActiveSoldierVoiceCents{ 0.0f };
    static std::atomic<bool>           g_HaveActiveSoldierVoiceCents{ false };

    static std::mutex                             g_CommandPostVoiceMutex;
    static std::vector<std::uint32_t>             g_CommandPostAkObjIds;
    static std::unordered_map<std::uint32_t, float> g_CommandPostCentsByCp;
    static std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_AkObjIdsByCp;

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


    static VoiceLineIdentity SehReadVoiceLineIdentity(const void* entry)
    {
        VoiceLineIdentity line = {};
        if (!entry)
            return line;
        __try
        {
            const auto e = reinterpret_cast<const std::uint8_t*>(entry);
            line.event      = *reinterpret_cast<const std::uint32_t*>(e + 0x20);
            line.voiceType  = *reinterpret_cast<const std::uint32_t*>(e + 0x24);
            line.voiceParam = *reinterpret_cast<const std::uint32_t*>(e + 0x28);
            line.category   = *reinterpret_cast<const std::uint16_t*>(e + 0x2c);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            line = VoiceLineIdentity{};
        }
        return line;
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

        const std::uint32_t     prev     = t_CurrentSpeakingSlot;
        const std::uint32_t     prevRaw  = t_CurrentSoundSlotRaw;
        void* const             prevCtl  = t_CurrentVoiceController;
        const VoiceLineIdentity prevLine = t_CurrentVoiceLine;
        t_CurrentSpeakingSlot    = soldierIndex;
        t_CurrentSoundSlotRaw    = slot;
        t_CurrentVoiceController = param_1;
        t_CurrentVoiceLine       = SehReadVoiceLineIdentity(param_2);

        SehCallCallVoiceImpl(param_1, param_2, slot);

        t_CurrentSpeakingSlot    = prev;
        t_CurrentSoundSlotRaw    = prevRaw;
        t_CurrentVoiceController = prevCtl;
        t_CurrentVoiceLine       = prevLine;
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


    static constexpr const char* kCommandPostEmitterName = "CommandPost";
    static constexpr std::size_t kEmitterNameLogCap      = 64;
    static constexpr std::size_t kCommandPostLogCap      = 64;


    static void NoteEmitterName(const char* name, std::uint32_t akObjId,
                                const void* owner)
    {
        if (!name)
            return;

        {
            std::lock_guard<std::mutex> lock(g_EmitterNameMutex);
            g_EmitterNameByAkObjId[akObjId] = name;
        }

        static std::mutex             s_reportMutex;
        static std::set<std::string>  s_namesSeen;
        static std::size_t            s_commandPostsSeen = 0;

        std::lock_guard<std::mutex> lock(s_reportMutex);

        if (std::strcmp(name, kCommandPostEmitterName) == 0
            && s_commandPostsSeen < kCommandPostLogCap)
        {
            ++s_commandPostsSeen;

            char stack[256];
            int  written = 0;
            for (std::size_t i = t_ActiveObjectDepth;
                 i-- > 0 && written < static_cast<int>(sizeof(stack)) - 1; )
            {
                const int n = std::snprintf(
                    stack + written,
                    sizeof(stack) - static_cast<std::size_t>(written),
                    " [%zu]=%p", i, t_ActiveObjectStack[i]);
                if (n < 0)
                    break;
                written += n;
            }
            if (written <= 0)
                stack[0] = '\0';

            LogDebug("[SoldierAkObjIdMap] command post #%zu emitter akObjId %u owner %p "
                     "cpIndex %d voiceType=0x%08X (%s) voiceParam=0x%08X event=0x%08X "
                     "category=%u depth %zu activation stack:%s - the post, HQ and the "
                     "soldier answering all transmit on this one emitter, so the voice "
                     "type is what tells them apart: cp_a* is the post's own voice and "
                     "takes the bias, hq_a* and ene_a* are the far end and keep their "
                     "vanilla pitch; cpIndex -1 is the radio static that brackets a "
                     "transmission rather than a spoken line\n",
                     s_commandPostsSeen, akObjId, owner,
                     static_cast<int>(static_cast<std::int32_t>(t_CurrentSoundSlotRaw)),
                     t_CurrentVoiceLine.voiceType,
                     IsCommandPostOwnVoiceType(t_CurrentVoiceLine.voiceType)
                         ? "command post" : "far end",
                     t_CurrentVoiceLine.voiceParam, t_CurrentVoiceLine.event,
                     t_CurrentVoiceLine.category,
                     t_ActiveObjectDepth, stack);
            return;
        }

        if (s_namesSeen.size() >= kEmitterNameLogCap
            || !s_namesSeen.insert(name).second)
            return;

        LogDebug("[SoldierAkObjIdMap] sound emitter '%s' registered akObjId %u "
                 "(owner %p)\n", name, akObjId, owner);
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
            NoteEmitterName(name, akObjId, owner);

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
                if (currentSlot != kNoSoundSlot)
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

            const std::uint32_t cpIndex = t_CurrentSoundSlotRaw;

            const bool isCommandPostOwnVoiceLine =
                name
                && std::strcmp(name, kCommandPostEmitterName) == 0
                && cpIndex != kNoSoundSlot
                && IsCommandPostOwnVoiceType(t_CurrentVoiceLine.voiceType);

            if (isNewMapping && isCommandPostOwnVoiceLine)
            {
                float desiredCents = 0.0f;
                try
                {
                    std::lock_guard<std::mutex> lock(g_CommandPostVoiceMutex);
                    g_CommandPostAkObjIds.push_back(akObjId);
                    g_AkObjIdsByCp[cpIndex].push_back(akObjId);

                    const auto it = g_CommandPostCentsByCp.find(cpIndex);
                    if (it != g_CommandPostCentsByCp.end())
                        desiredCents = it->second;
                }
                catch (...) {}

                if (desiredCents != 0.0f)
                    Set_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId),
                                            desiredCents);
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
        {
            std::lock_guard<std::mutex> lock(g_EmitterNameMutex);
            g_EmitterNameByAkObjId.clear();
        }

        g_Installed.store(false, std::memory_order_relaxed);
        return true;
    }


    std::string GetEmitterNameForAkObjId(std::uint32_t akObjId)
    {
        std::lock_guard<std::mutex> lock(g_EmitterNameMutex);
        const auto it = g_EmitterNameByAkObjId.find(akObjId);
        if (it == g_EmitterNameByAkObjId.end()) return {};
        return it->second;
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


    void SetCommandPostVoiceCents(std::uint32_t cpIndex, float cents)
    {
        std::vector<std::uint32_t> live;
        {
            std::lock_guard<std::mutex> lock(g_CommandPostVoiceMutex);
            g_CommandPostCentsByCp[cpIndex] = cents;

            const auto it = g_AkObjIdsByCp.find(cpIndex);
            if (it != g_AkObjIdsByCp.end())
                live = it->second;
        }

        for (std::uint32_t akObjId : live)
            Set_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId), cents);
    }


    void ClearCommandPostVoiceCents()
    {
        std::vector<std::uint32_t> ids;
        {
            std::lock_guard<std::mutex> lock(g_CommandPostVoiceMutex);
            g_CommandPostCentsByCp.clear();
            g_AkObjIdsByCp.clear();
            ids.swap(g_CommandPostAkObjIds);
        }
        for (std::uint32_t akObjId : ids)
            Clear_PitchBiasForAkObjId(static_cast<std::uint64_t>(akObjId));
    }


    std::vector<std::uint32_t> GetCommandPostAkObjIds()
    {
        std::lock_guard<std::mutex> lock(g_CommandPostVoiceMutex);
        return g_CommandPostAkObjIds;
    }


}
