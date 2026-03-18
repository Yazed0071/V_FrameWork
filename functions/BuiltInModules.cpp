#include "pch.h"

#include <Windows.h>
#include <mutex>

#include "BuiltInModules.h"
#include "FeatureModule.h"


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

bool Install_State_EnterDownHoldupForceVoice_Hook();
bool Uninstall_State_EnterDownHoldupForceVoice_Hook();

bool Install_VIPSleepFaint_Hook();
bool Uninstall_VIPSleepFaint_Hook();

bool Install_VIPHoldup_Hook();
bool Uninstall_VIPHoldup_Hook();

bool Install_VIPRadio_Hook();
bool Uninstall_VIPRadio_Hook();

bool Install_HoldUpReactionCowardlyReactions_Hook();
bool Uninstall_HoldUpReactionCowardlyReactions_Hook();

bool Install_CallSignExtra_Hook();
bool Uninstall_CallSignExtra_Hook();

bool Install_LostHostage_Hooks();
bool Uninstall_LostHostage_Hooks();

bool Install_LostHostageDiscovery_Hooks();
bool Uninstall_LostHostageDiscovery_Hooks();

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

    class VIPSleepFaintModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "VIPSleepFaint";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_VIPSleepFaint_Hook();
        }

        void Uninstall() override
        {
            Uninstall_VIPSleepFaint_Hook();
        }
    };
    class VIPHoldupModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "VIPHoldup";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_VIPHoldup_Hook();
        }

        void Uninstall() override
        {
            Uninstall_VIPHoldup_Hook();
        }
    };
	class VIPRadioModule final : public IFeatureModule
	{
	public:
		const char* GetName() const override
		{
			return "VIPRadio";
		}
		bool Install(HMODULE hGame) override
		{
			UNREFERENCED_PARAMETER(hGame);
			return Install_VIPRadio_Hook();
		}
		void Uninstall() override
		{
			Uninstall_VIPRadio_Hook();
		}
	};
	class HoldUpReactionCowardlyReactionsModule final : public IFeatureModule
	{
	public:
		const char* GetName() const override
		{
			return "HoldUpReactionCowardlyReactions";
		}
		bool Install(HMODULE hGame) override
		{
			UNREFERENCED_PARAMETER(hGame);
			return Install_HoldUpReactionCowardlyReactions_Hook();
		}
		void Uninstall() override
		{
			Uninstall_HoldUpReactionCowardlyReactions_Hook();
		}
	};
	class PerSoldierCallSignOverrideModule final : public IFeatureModule
	{
	public:
		const char* GetName() const override
		{
			return "PerSoldierCallSignOverride";
		}
		bool Install(HMODULE hGame) override
		{
			UNREFERENCED_PARAMETER(hGame);
			return Install_CallSignExtra_Hook();
		}
		void Uninstall() override
		{
			Uninstall_CallSignExtra_Hook();
		}
	};
	class LostHostageModule final : public IFeatureModule
	{
	public:
		const char* GetName() const override
		{
			return "LostHostage";
		}
		bool Install(HMODULE hGame) override
		{
			UNREFERENCED_PARAMETER(hGame);
			return Install_LostHostage_Hooks() && Install_LostHostageDiscovery_Hooks();
		}
		void Uninstall() override
		{
			Uninstall_LostHostage_Hooks();
            Uninstall_LostHostageDiscovery_Hooks();
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
    static VIPSleepFaintModule s_VIPSleepFaintModule;
    static VIPHoldupModule s_VIPHoldupModule;
	static VIPRadioModule s_VIPRadioModule;
	static HoldUpReactionCowardlyReactionsModule s_HoldUpReactionCowardlyReactionsModule;
	static PerSoldierCallSignOverrideModule s_PerSoldierCallSignOverrideModule;
	static LostHostageModule s_LostHostageModule;

    static std::once_flag s_Once;
    std::call_once(s_Once, []()
        {
            FeatureModuleRegistry::Instance().Register(&s_LuaBridgeModule);
            FeatureModuleRegistry::Instance().Register(&s_UiTextureOverridesModule);
            FeatureModuleRegistry::Instance().Register(&s_HoldupCancelLookToPlayerModule);
            FeatureModuleRegistry::Instance().Register(&s_CautionTimerModule);
            FeatureModuleRegistry::Instance().Register(&s_PlayerVoiceFpkModule);
            FeatureModuleRegistry::Instance().Register(&s_EnterDownHoldupForceVoiceModule);
            FeatureModuleRegistry::Instance().Register(&s_VIPSleepFaintModule);
            FeatureModuleRegistry::Instance().Register(&s_VIPHoldupModule);
			FeatureModuleRegistry::Instance().Register(&s_VIPRadioModule);
			FeatureModuleRegistry::Instance().Register(&s_HoldUpReactionCowardlyReactionsModule);
			FeatureModuleRegistry::Instance().Register(&s_PerSoldierCallSignOverrideModule);
			FeatureModuleRegistry::Instance().Register(&s_LostHostageModule);
        });
}