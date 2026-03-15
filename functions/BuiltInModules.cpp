#include "pch.h"

#include <Windows.h>
#include <mutex>

#include "BuiltInModules.h"
#include "FeatureModule.h"

// Existing feature entry points.
bool Install_SetLuaFunctions_Hook();
bool Uninstall_SetLuaFunctions_Hook();

bool Install_UiTextureOverrides_Hook();
bool Uninstall_UiTextureOverrides_Hook();

bool Install_State_StandHoldupCancelLookToPlayer_Hook(HMODULE hGame);
bool Uninstall_State_StandHoldupCancelLookToPlayer_Hook();

bool Install_CautionStepNormalTimerHook();
bool Uninstall_CautionStepNormalTimerHook();

bool Install_PlayerVoiceFpk_Hook();
bool Uninstall_PlayerVoiceFpk_Hook();

bool Install_VIPSleepWakeReaction_Hook();
bool Uninstall_VIPSleepWakeReaction_Hook();

bool Install_State_EnterDownHoldupForceVoice_Hook();
bool Uninstall_State_EnterDownHoldupForceVoice_Hook();

bool Install_StateRadioSpeechLabelLog_Hook();
bool Uninstall_StateRadioSpeechLabelLog_Hook();

namespace
{
    class LuaBridgeModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "LuaBridge";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_SetLuaFunctions_Hook();
        }

        void Uninstall() override
        {
            Uninstall_SetLuaFunctions_Hook();
        }
    };

    class UiTextureOverridesModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "UiTextureOverrides";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_UiTextureOverrides_Hook();
        }
        void Uninstall() override
        {
            Uninstall_UiTextureOverrides_Hook();
        }
    };

    class HoldupCancelLookToPlayerModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "HoldupCancelLookToPlayer";
        }
        bool Install(HMODULE hGame) override
        {
            return Install_State_StandHoldupCancelLookToPlayer_Hook(hGame);
        }
        void Uninstall() override
        {
            Uninstall_State_StandHoldupCancelLookToPlayer_Hook();
        }
    };
    class CautionTimerModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "CautionTimer";
        }
        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_CautionStepNormalTimerHook();
        }
        void Uninstall() override
        {
            Uninstall_CautionStepNormalTimerHook();
        }
    };

    class PlayerVoiceFpkModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "PlayerVoiceFpk";
        }
        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_PlayerVoiceFpk_Hook();
        }
        void Uninstall() override
        {
            Uninstall_PlayerVoiceFpk_Hook();
        }
    };
    class EnterDownHoldupForceVoiceModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "EnterDownHoldupForceVoice";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_State_EnterDownHoldupForceVoice_Hook();
        }

        void Uninstall() override
        {
            Uninstall_State_EnterDownHoldupForceVoice_Hook();
        }
    };

}

void RegisterBuiltInFeatureModules()
{
    static LuaBridgeModule s_LuaBridgeModule;
    static UiTextureOverridesModule s_UiTextureOverridesModule;
    static HoldupCancelLookToPlayerModule s_HoldupCancelLookToPlayerModule;
    static CautionTimerModule s_CautionTimerModule;
    static PlayerVoiceFpkModule s_PlayerVoiceFpkModule;
    static EnterDownHoldupForceVoiceModule s_EnterDownHoldupForceVoiceModule;

    static std::once_flag s_Once;
    std::call_once(s_Once, []()
        {
            FeatureModuleRegistry::Instance().Register(&s_LuaBridgeModule);
            FeatureModuleRegistry::Instance().Register(&s_UiTextureOverridesModule);
            FeatureModuleRegistry::Instance().Register(&s_HoldupCancelLookToPlayerModule);
            FeatureModuleRegistry::Instance().Register(&s_CautionTimerModule);
            FeatureModuleRegistry::Instance().Register(&s_PlayerVoiceFpkModule);
            FeatureModuleRegistry::Instance().Register(&s_EnterDownHoldupForceVoiceModule);
        });
}