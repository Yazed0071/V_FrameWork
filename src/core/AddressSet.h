#pragma once

#include <Windows.h>
#include <cstdint>

namespace AddressSetRuntime
{
    enum class GameBuild
    {
        Unknown,
        En_1_0_15_3,   // day1820
        Jp_1_0_15_3,   // day1820
        En_1_0_15_4,   // day3800
        Jp_1_0_15_4,   // day3800
        En_1_0_15_4a,    // day3900 - exe version resource still reads 1.0.15.4
        Jp_1_0_15_4a     // day3900
    };

    struct AddressSet
    {
        uintptr_t AddNoise = 0;
        uintptr_t AddNoticeInfo = 0;
        uintptr_t ArrayBaseFree = 0;
        uintptr_t BeginSoundSystem = 0;
        uintptr_t CallImpl = 0;
        uintptr_t CallWithRadioType = 0;
        uintptr_t CassettePlayerVtable = 0;
        uintptr_t CassetteStart = 0;
        uintptr_t CheckSightNoticeHostage = 0;
        uintptr_t ConvertRadioTypeToLabel = 0;
        uintptr_t CopyAndAdjustInfo = 0;
        uintptr_t DecrementPhaseCounter = 0;
        uintptr_t FoxLuaRegisterLibrary = 0;
        uintptr_t FoxPath_Path = 0;
        uintptr_t FoxStrHash32 = 0;
        uintptr_t FoxStrHash64 = 0;
        uintptr_t GameOverSetVisible = 0;
        uintptr_t GetCurrentMissionCode = 0;
        uintptr_t GetNameIdWithGameObjectId = 0;
        uintptr_t GetGameObjectIdWithIndex = 0;
        uintptr_t GetPlayingTime = 0;
        uintptr_t GetPlayingTrackId = 0;
        uintptr_t GetQuarkSystemTable = 0;
        uintptr_t GetTrackInfoByName = 0;
        uintptr_t GetUixUtilityToFeedQuarkEnvironment = 0;
        uintptr_t GetVoiceLanguage = 0;
        uintptr_t GetVoiceParamWithCallSign = 0;
        uintptr_t KernelAllocAligned = 0;
        uintptr_t LoadPlayerVoiceFpk = 0;
        uintptr_t LoadingScreenOrGameOverSplash2 = 0;
        uintptr_t MusicManager_s_instance = 0;
        uintptr_t PathHashCode = 0;
        uintptr_t PauseMusicPlayer = 0;
        uintptr_t PlayOrPauseSelectedTrack = 0;
        uintptr_t RequestCorpse = 0;
        uintptr_t ResumeMusicPlayer = 0;
        uintptr_t SetEquipBackgroundTexture = 0;
        uintptr_t SetLuaFunctions = 0;
        uintptr_t SetTextureName = 0;
        uintptr_t SoundSystemCtor = 0;
        uintptr_t StateRadioRequest = 0;
        uintptr_t State_ComradeAction = 0;
        uintptr_t State_EnterDownHoldup = 0;
        uintptr_t State_EnterStandHoldup1 = 0;
        uintptr_t State_EnterStandHoldupUnarmed = 0;
        uintptr_t State_RecoveryKick = 0;
        uintptr_t State_RecoveryTouch = 0;
        uintptr_t State_StandEnterRecoverySleepFaintHoldupComradeBySound = 0;
        uintptr_t State_StandHoldupCancelLookToPlayer = 0;
        uintptr_t State_StandRecoveryHoldup = 0;
        uintptr_t StepRadioDiscovery = 0;
        uintptr_t StopMusicPlayer = 0;
        uintptr_t SubtitleManager_Get = 0;
        uintptr_t UpdateOptCamo = 0;
        uintptr_t g_SoundSystem = 0;
        uintptr_t lua_getfield = 0;
        uintptr_t lua_gettop = 0;
        uintptr_t lua_isnumber = 0;
        uintptr_t lua_isstring = 0;
        uintptr_t lua_objlen = 0;
        uintptr_t lua_pushboolean = 0;
        uintptr_t lua_pushnumber = 0;
        uintptr_t lua_rawgeti = 0;
        uintptr_t lua_settop = 0;
        uintptr_t lua_toboolean = 0;
        uintptr_t lua_tointeger = 0;
        uintptr_t lua_tolstring = 0;
        uintptr_t lua_tonumber = 0;
        uintptr_t lua_type = 0;
        uintptr_t lua_pushstring = 0;
        uintptr_t lua_createtable = 0;
        uintptr_t lua_rawset = 0;
        uintptr_t lua_settable = 0;
        uintptr_t lua_pushnil = 0;
        uintptr_t lua_next = 0;
        uintptr_t lua_gettable = 0;
        uintptr_t lua_pushvalue = 0;
        uintptr_t lua_pcall = 0;
        uintptr_t lua_pushcclosure = 0;
        uintptr_t GetIconFtexPath = 0;
        uintptr_t LoadingTipsEv_UpdateActPhase = 0;
        uintptr_t Fox_Sd_ConvertParameterID = 0;
        uintptr_t Fox_Sd_Ad_AudioSoundEngine_RegisterGameObject = 0;
        uintptr_t Fox_Sd_Object_Activate = 0;
        uintptr_t Fox_Sd_Daemon_GetObject = 0;
        uintptr_t Fox_Sd_Daemon_Singleton = 0;
        uintptr_t SoundControllerImpl_CallInternal = 0;
        uintptr_t TornadoDualPatch                  = 0;
        uintptr_t RealizedSahelan2Impl_Realize      = 0;
        uintptr_t RealizedSahelan2Impl_SetFovaImpl  = 0;
        uintptr_t FormVariationFile2_ApplyOnlyMeshAndTextureVariation = 0;
        uintptr_t Sahelan_ActionCoreImpl_UpdateEyeLampColor = 0;
        uintptr_t Sahelan_ActionCoreImpl_UpdateHeartLight = 0;
        uintptr_t Sahelan_PhaseSneakAi_PushEyeColor = 0;
        uintptr_t MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceNormal    = 0;
        uintptr_t MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceServer    = 0;
        uintptr_t MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceEmergency = 0;
        uintptr_t HudCommonDataManager_GetInstance       = 0;
        uintptr_t Soldier2SoundController_Activate = 0;
        uintptr_t CAkResampler_SetPitch = 0;
        uintptr_t FNVHash32                         = 0;
        uintptr_t Play_bgm_gameover                 = 0;
        uintptr_t Play_bgm_gameover_paradox         = 0;
        uintptr_t Play_bgm_gameover_perfectstealth  = 0;
        uintptr_t Play_bgm_s10010_gameover          = 0;
        uintptr_t Stop_bgm_gameover                 = 0;
        uintptr_t Stop_bgm_gameover_paradox         = 0;
        uintptr_t Stop_bgm_gameover_perfectstealth  = 0;
        uintptr_t Stop_bgm_s10010_gameover          = 0;
        uintptr_t Play_bgm_gameover_paradox_soundId = 0;
        uintptr_t Stop_bgm_gameover_paradox_soundId = 0;
        uintptr_t DD_vox_SH_voice                   = 0;
        uintptr_t DD_vox_SH_radio                   = 0;
        uintptr_t DD_vox_SH_radio2                  = 0;
        uintptr_t DD_vox_SH_radio3                  = 0;

        uintptr_t MotherBaseMapCommonDataImpl_GetEnemyInformationLangId = 0;
        uintptr_t TppUIBinoSubjectiveImpl_GetEnemyUnitName              = 0;

        uintptr_t BasicActionImpl_StateCrawlSideRoll                    = 0;

        uintptr_t Sahelan_PhaseSneakAiImpl_PreUpdate                    = 0;
        uintptr_t Sahelan_PhaseSneakAiImpl_StepFuncsTable               = 0;

        uintptr_t MessageResendCounter                                  = 0;

        uintptr_t GetMissionCodeCategory                                = 0;



        uintptr_t IconTitleGetLangTextCall                              = 0;

        uintptr_t GameObject_SendCommand                                = 0;


        uintptr_t UiControllerImpl_InitEquipHudData                     = 0;

        uintptr_t NoticeControllerImpl_GetOccasionalChat                = 0;

        uintptr_t SoldierConversationService_ConvertSpeechLabelToConversationType = 0;

        uintptr_t OccasionalChat_FactionTestNop                         = 0;

        uintptr_t MbDvcReserveAnnouncePopup                             = 0;
        uintptr_t MbDvcPopupGateFn                                      = 0;

        uintptr_t NoticeNoiseAiImpl_StepAware                           = 0;
        uintptr_t NoticeIndisAiImpl_StepAware                           = 0;
        uintptr_t NoticeControllerImpl_DoCheckSpreadNotice              = 0;
        uintptr_t NoticeControllerImpl_CheckSightNoticeSoldier          = 0;
        uintptr_t NoticeNearThreatAiImpl_Wake                           = 0;
        uintptr_t NoticeNoiseAlertAiImpl_Wake                           = 0;
        uintptr_t NoticeNearGameObjectAiImpl_Wolf_Wake                  = 0;
        uintptr_t NoticeNoiseSneakAiImpl_Wolf_Wake                      = 0;
        uintptr_t NoticeNearGameObjectAiImpl_Bear_Wake                  = 0;
        uintptr_t NoticeNoiseAiImpl_Bear_Wake                           = 0;

        uintptr_t RealizedSoldier2Impl_ConvertHeadEquipModelType        = 0;
        uintptr_t RealizedSoldier2Impl_UpdateHeadEquipMesh              = 0;
        uintptr_t FovaController_GetActiveFovaResourceManager           = 0;
        uintptr_t Fv2ResourceManager_GetModel                           = 0;

        uintptr_t TimeCigaretteActionPluginImpl_ShowTimeCigaretteUi      = 0;

        uintptr_t HeliTaxi_CanHeliTaxi      = 0;
        uintptr_t HeliTaxi_CallRescueHeli   = 0;
        uintptr_t HeliTaxi_StepWithdraw     = 0;
        uintptr_t HeliTaxi_GetLocationId    = 0;
        uintptr_t HeliTaxi_RequestMapPhase  = 0;
        uintptr_t HeliTaxi_StepGoToNav      = 0;
        uintptr_t HeliTaxi_StepTaxiCurrentCluster  = 0;
        uintptr_t HeliTaxi_PassengerUpdate  = 0;
        uintptr_t MechaActionImpl_StateOff  = 0;
        uintptr_t MechaActionImpl_StateOn    = 0;
        uintptr_t PassengerControllerImpl_IsPassengerClosingDoor = 0;

        uintptr_t PlacedSystemImpl_BindResource = 0;
        uintptr_t UiMarkerCommonDataImpl_RegisterLZMarkerInUpdate = 0;

        uintptr_t HeliSoundControllerImpl_Update    = 0;
        uintptr_t HeliSoundControllerImpl_CallVoice = 0;
        uintptr_t HeliFlightControllerImpl_Update   = 0;
        uintptr_t Hud_GetAnnounceLogSE              = 0;

        uintptr_t Hud_TypingLogActUpdate            = 0;
        uintptr_t Ui_SoundControlStart              = 0;
        uintptr_t Ui_UiCommonDataManagerGetInstance = 0;
        uintptr_t Ui_EventNodeBodyGetGraphState     = 0;
        uintptr_t VoiceParam_PlayDialogue                 = 0;

        uintptr_t RideHeliActionPluginImpl_ExecPreMotionGraph = 0;
        uintptr_t RideHeliActionPluginImpl_GetStateFn         = 0;

        uintptr_t SoundDaemon_Instance       = 0;
        uintptr_t SoundDaemon_PostEventQueue = 0;
        uintptr_t Sd_kW_Select               = 0;

        uintptr_t UpdateAntiAir              = 0;
        uintptr_t ClearAntiAir               = 0;

        uintptr_t GetChangeLocationMenuParameterByLocationId = 0;
        uintptr_t GetMbFreeChangeLocationMenuParameter       = 0;
        uintptr_t GetPhotoAdditionalTextLangId               = 0;

        uintptr_t UiControllerImpl_HideBinocle               = 0;

        uintptr_t AddCassetteTapeTrack        = 0;
        uintptr_t CollectGotTapes             = 0;
        uintptr_t IsGotCassetteTapeTrack      = 0;
        uintptr_t SetCassetteTapeTrackNewFlag = 0;
        uintptr_t SetCurrentAlbum             = 0;
        uintptr_t SetupMusicInfos             = 0;

        uintptr_t RadioCassette_SearchCasseteInfo = 0;
        uintptr_t RadioCassette_GetCassetteMusic  = 0;
        uintptr_t RadioCassette_IsGotCassette     = 0;
        uintptr_t RadioCassette_GetCassetteSaveIndex = 0;
        uintptr_t RadioCassette_SdPostEvent          = 0;
        uintptr_t RadioCassette_RadioUpdate          = 0;
        uintptr_t AddCassetteTapeTrackByIndex        = 0;
        uintptr_t RadioCassette_ActivateUnit         = 0;
        uintptr_t RadioCassette_IsSameSaveIndexFromName = 0;

        uintptr_t SearchLightActionPluginImpl_StateDoorStart = 0;
        uintptr_t SearchLightActionPluginImpl_StateDoorEnd   = 0;

        uintptr_t SoundControl_PostExternalEvent = 0;
        uintptr_t MusicPlayerPlayWrapper = 0;

        uintptr_t Barrier_GetItemId        = 0;
        uintptr_t Barrier_EquipItemCallRet = 0;
        uintptr_t Barrier_Updater          = 0;
        uintptr_t Barrier_IsFobMode        = 0;
        uintptr_t Barrier_LoadGate0        = 0; 
        uintptr_t Barrier_LoadGate1        = 0;
        uintptr_t Barrier_LoadGate2        = 0;

        uintptr_t Dm_FireLoop         = 0;
        uintptr_t Dm_Classify         = 0;
        uintptr_t Dm_VfxFactory       = 0;
        uintptr_t Dm_ComponentFactory = 0;
        uintptr_t Dm_Alloc            = 0;
        uintptr_t Dm_BackLinkPool     = 0;
        uintptr_t Dm_OneShot          = 0;

        uintptr_t EquipCrossEvCall_IsItemNoUse = 0;
        uintptr_t AttackActionImpl_IsWeaponNoUseInPlaceAction = 0;
        uintptr_t EquipCrossSetEquipItem_Site1 = 0;
        uintptr_t EquipCrossSetEquipItem_Site2 = 0;
        uintptr_t EquipCrossSetEquipItem_Site3 = 0;

        uintptr_t TppMotherBaseManagement_RegCstDev = 0;
        uintptr_t TppMotherBaseManagement_RegFlwDev = 0;
        uintptr_t EquipDevCtrl_GetSuitDevelopInfoIndex = 0;
        uintptr_t EquipDevCtrl_GetFaceEquipDevelopInfoIndex = 0;

        uintptr_t AddListSuit = 0;
        uintptr_t CamoufParamInfo_GetCamoufValue = 0;
        uintptr_t CamouflageController_ExecSuitCorrect = 0;
        uintptr_t EquipController_SetHandSlotEnabled = 0;
        uintptr_t EquipDevelopCallbackImpl_SetSupplyCBoxInfo = 0;
        uintptr_t IsEnableCurrentSuit = 0;
        uintptr_t ItemSelectorCallbackImpl_DecideActMissionPreparationSetEquipMode = 0;
        uintptr_t ItemSelectorCallbackImpl_DecideActMotherBaseDeviceSupportDropMode = 0;
        uintptr_t ItemSelectorCallbackImpl_SetupPrefabListElement = 0;
        uintptr_t ItemSelectorRecordCallFunc_UpdateRecords = 0;
        uintptr_t ItemSelector_AddListBandana = 0;
        uintptr_t LoadPlayerBionicArmFpk = 0;
        uintptr_t LoadPlayerBionicArmFv2 = 0;
        uintptr_t LoadPlayerCamoFpk = 0;
        uintptr_t LoadPlayerCamoFv2 = 0;
        uintptr_t LoadPlayerPartsFpk = 0;
        uintptr_t LoadPlayerPartsParts = 0;
        uintptr_t LoadPlayerSnakeBlackDiamondFpk = 0;
        uintptr_t LoadPlayerSnakeBlackDiamondFv2 = 0;
        uintptr_t LoadPlayerSnakeFaceFpk = 0;
        uintptr_t LoadPlayerSnakeFaceFv2 = 0;
        uintptr_t MissionPrepSystem_IsEnableHeadOptionSuit = 0;
        uintptr_t MissionPrep_IsEnableCurrentHeadOption = 0;
        uintptr_t Player2BlockController_LoadPartsNew = 0;
        uintptr_t Player2GameObjectImpl_ProcessSignal = 0;
        uintptr_t Player2Impl_SetUpParts = 0;
        uintptr_t Player2UtilityImpl_LoadoutApplyAfterSetSuit = 0;
        uintptr_t Player2UtilityImpl_SetInitialConditionWithLoadoutInfo = 0;
        uintptr_t Player2UtilityImpl_SetSuitAndHandConditionWithLoadoutInfo = 0;
        uintptr_t Player_ConverFaceIdWithFaceEquipId = 0;
        uintptr_t ResourceTable_DoesNeedFaceFova = 0;
        uintptr_t ResourceTable_DoesNeedFaceFovaForAvatar = 0;
        uintptr_t Sys_IsArtificialHandEnabled = 0;
        uintptr_t Sys_IsArtificialHandEnabledForCurrentPlayerType = 0;
        uintptr_t UpdatePartsStatus = 0;
        uintptr_t ItemSelectorCallbackImpl_DecideActMotherBaseCustomize = 0;
        uintptr_t Fox_Path_Exists = 0;
        uintptr_t Fox_Path_Dtor   = 0;
        uintptr_t PluginFacial_ApplyMotion = 0;
        uintptr_t EquipDevCtrl_IsEquipVisile = 0;
        uintptr_t EquipDevCtrl_GetHeadBadgeCategory = 0;
        uintptr_t MissionPrep_GetWornHeadCategory = 0;
        uintptr_t MissionPrep_SetInitialSelectRecord = 0;
        uintptr_t TppEquip_RegisterConstant = 0;
        uintptr_t PlayerInfoInterfaceImpl_GetPartsTypeAtCamoType = 0;
        uintptr_t EquipIdTable_InfoList = 0;
        uintptr_t EquipIdTable_TypeWords = 0;
        uintptr_t EquipIdTable_ReloadEquipIdTable = 0;
        uintptr_t EquipDevelopCtrl_GetEquipDevelopIndex = 0;
        uintptr_t EquipDevelopCtrl_SetEquipUndeveloped = 0;
        uintptr_t EquipDevCtrl_GetSuitCamoType = 0;
        uintptr_t EquipDevCtrl_GetSuitLevel = 0;
        uintptr_t EquipDevCtrl_IsEquipSuit = 0;
        uintptr_t EquipDevelopCtrl_SetEquipDeveloped = 0;
        uintptr_t SupplyCboxActionPluginImpl_StateInBox = 0;
        uintptr_t HudCommonDataManager_AnnounceLogView = 0;
        uintptr_t Ui_LangIdToKey = 0;
        uintptr_t Ui_GetLangText = 0;
        uintptr_t Soldier_ShootOneBullet_GroupMaskCall = 0;
        uintptr_t Soldier_ActivateBulletAtEmptyWork_SameArmyJnz = 0;

        uintptr_t TelopStartTitleEvCall_SetBgTexture = 0;
        uintptr_t Layout_GetLayout = 0;
        uintptr_t Layout_GetModel = 0;

        uintptr_t GetCassetteTapeUnreadInfo = 0;
        uintptr_t IsNewCassetteTapeTrack = 0;
        uintptr_t CassetteMenuCheckNewFlag = 0;
        uintptr_t CassetteAlbumCheckNewFlag = 0;
        uintptr_t CassetteCheckUnreadInfo = 0;
        uintptr_t SubtitlesObjectSendMessage = 0;

        uintptr_t EquipDevCtrl_IsEquipDevelopable = 0;

        uintptr_t LoadAvatarFaceFv2 = 0;
        uintptr_t LoadAvatarFaceFpk = 0;
        uintptr_t AvatarFaceEditUpdate = 0;
        uintptr_t Fox_ModelFromHandle = 0;

        uintptr_t LoadAvatarHeadOptionFv2 = 0;
        uintptr_t LoadAvatarHeadOptionFpk = 0;

        uintptr_t EquipDevelopCtrl_SetEnableDevelop = 0;

        uintptr_t ReloadEquipParameterTables2 = 0;

        uintptr_t GunBasicParameters2Buffer = 0;


        uintptr_t EquipParameterTablesImpl_Instance = 0;

        uintptr_t MotionLoaderImpl_ReceiverTypeTable = 0;
        uintptr_t MotionLoaderImpl_GetReceiverType = 0;
        uintptr_t MotionLoaderImpl_UnderBarrelTypeTable = 0;
        uintptr_t MotionLoaderImpl_GetUnderBarrelType = 0;

        uintptr_t EquipDevelopControllerImpl_GetSuppressorAmount = 0;

        uintptr_t DamageParameterTable_Instance = 0;
        uintptr_t DamageParameterTable_ReloadDamageParameter = 0;
        uintptr_t DamageParameterTable_GetDamageParameter = 0;
        uintptr_t EquipDevCtrl_GetBaseDevelopId = 0;
        uintptr_t MenuDevelopGrid_FillGrid = 0;
        uintptr_t MenuDevelopGrid_CopyGrid = 0;
        uintptr_t MenuDevelopGrid_CountBadge = 0;
        uintptr_t MenuDevelopGrid_FillFlat = 0;
        uintptr_t MenuDevelopGrid_CopyFlat = 0;
        uintptr_t SightManager_UpdateMissileLockOn = 0;
        uintptr_t Bullet3_DoSimulation = 0;
        uintptr_t Bullet3_ActivateBulletAtEmptyWork = 0;

        uintptr_t SightManager_Update = 0;
        uintptr_t SightManager_UpdateMissileLockOnUi = 0;
        uintptr_t LockOnReticleFactory_CreateWindow = 0;
        uintptr_t EquipParams_GetAttackIdByEquipId = 0;
        uintptr_t EquipSystem_SetUpGunInfoFromGunPartsDesc = 0;
        uintptr_t UiWindowFunction_FindWindow = 0;
        uintptr_t UiWindowFunction_PostShowAndStartMessage = 0;
        uintptr_t UiWindowFunction_GetLayout = 0;
        uintptr_t AttackAction_Fire = 0;
        uintptr_t EquipObject_DoFire = 0;
        uintptr_t CorePlugin_UpdateLoadoutRequest = 0;
        uintptr_t WeaponSystem_DefineWeaponFireSound = 0;
        uintptr_t EquipSystem_ChimeraPartsSetWork = 0;
        uintptr_t MbDvcUpdateTrackListCallFuncs = 0;
        uintptr_t MbDvcRefreshTrackListPrefabParameter = 0;
        uintptr_t MbDvcTrackListRecordRefresh = 0;
        uintptr_t MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter = 0;
        uintptr_t MbDvcMissionListCallbackImpl_SetMenuHelp = 0;
        uintptr_t Equip_ReloadChimeraPartsInfoTable = 0;
        uintptr_t Equip_ChimeraPartsPackageInfos = 0;
        uintptr_t Mtar_GetAnimFile = 0;
        uintptr_t Mtar_GetDataInfo = 0;
        uintptr_t Equip_MotionEntryTable = 0;
        uintptr_t Equip_MotionMtarResolver = 0;
        uintptr_t SimplePartsControllerImpl_Vtable = 0;
        uintptr_t SimplePartsControllerImpl_SetMotionData = 0;
        uintptr_t SimplePartsControllerImpl_SetMotionDataByPath = 0;
        uintptr_t EquipSystemImpl_HideMagazine = 0;
        uintptr_t EquipSystemImpl_GetPartsController = 0;
        uintptr_t Fox_GetQuarkSystemTable = 0;
        uintptr_t Fox_ArrayBaseExtend = 0;
        uintptr_t Fox_ArrayOperatorVtbl = 0;
        uintptr_t Animx_GetControlSize = 0;
        uintptr_t Animx_SimpleControlCtorPool = 0;
        uintptr_t Animx_SimpleControlCtorHeap = 0;
        uintptr_t Animx_SimpleControlDtorPool = 0;
        uintptr_t Animx_SimpleControlDtorHeap = 0;
        uintptr_t Animx_SimpleControlAux = 0;
        uintptr_t Equip_MotionEntrySlotHookA = 0;
        uintptr_t Equip_MotionEntrySlotHookB = 0;
        uintptr_t EquipIdTable_GetEquipTypeId = 0;
        uintptr_t Soldier2InterrogateUtil_UpdateInterrogation = 0;
        uintptr_t Soldier2InterrogateUtil_UpdateInterrogationMarker = 0;
        uintptr_t SoundControllerImpl_CallVoice = 0;
        uintptr_t MbDvcCassetteTapeCallbackImpl_SeekMusic = 0;
        uintptr_t EquipSystem_GetGunInfoById = 0;

        uintptr_t Reticle_InitHandGunAsset = 0;

        uintptr_t TimeCigaretteActionPluginImpl_HideTimeCigaretteUi = 0;

        uintptr_t Fox_EntityInfoMapTeardown = 0;


        uintptr_t WeaponEnhanceLangIdArray = 0;
        uintptr_t WeaponEnhance_GetIndexForRegist = 0;
        uintptr_t WeaponEnhance_GetMessageIdArray = 0;
        uintptr_t WeaponEnhance_GetLangIdById = 0;
        uintptr_t WeaponEnhance_GetLangIdBound = 0;
        uintptr_t BulletEffectController_CreateEffect = 0;
        uintptr_t ItemSelectorCallbackImpl_AddRecord = 0;

        uintptr_t LoadPlayerLilChickCapFv2 = 0;
        uintptr_t LoadPlayerLilChickCapFpk = 0;
        uintptr_t LoadPlayerChickenCapFv2 = 0;
        uintptr_t LoadPlayerChickenCapFpk = 0;
        uintptr_t AdditionalMotionTable_GetMtarPathId = 0;
        uintptr_t ModelVertexColorChange_GetModelTotalVertexNum = 0;
        uintptr_t ItemSelector_AddDevelopWeaponList = 0;
        uintptr_t BlockControllerImpl_UpdateAdditionalMotionBlock = 0;
        uintptr_t Fox_Block_GetFileForPathId = 0;
        uintptr_t Fox_BlockGroup_GetBlockAtIndex = 0;
        uintptr_t EquipPreviewSystem_RequestLoadEquip = 0;
        uintptr_t ItemSelector_StartEquipPreviewImpl = 0;
        uintptr_t EquipBlockController2_RequestLoad = 0;
        uintptr_t EquipBlockController2_Instance = 0;
        uintptr_t ItemSelector_UpdateSelect = 0;
        uintptr_t UiEquipPreviewController_ScrollNext = 0;
        uintptr_t UiEquipPreviewController_ScrollPrev = 0;
        uintptr_t UiEquipPreviewController_StopEquipPreview = 0;
        uintptr_t Player2Impl_AddAdditionalMtarAll = 0;
        uintptr_t Player2Impl_RemoveAdditionalMtarAll = 0;
        uintptr_t MotionLoaderImpl_BarrelTypeTable = 0;
        uintptr_t MotionLoaderImpl_GetBarrelType = 0;
        uintptr_t MotionLoaderImpl_MagazineTypeTable = 0;
        uintptr_t MotionLoaderImpl_GetMagazineType = 0;
        uintptr_t MotionLoaderImpl_SightTypeTable = 0;
        uintptr_t MotionLoaderImpl_GetSightType = 0;
        uintptr_t TppPickable_ItemWindowBoundSite = 0;
        uintptr_t UiUtility_GetWeaponItemNameLangId = 0;
    };

    inline GameBuild& GetGameBuild()
    {
        static GameBuild value = GameBuild::Unknown;
        return value;
    }

    inline AddressSet& GetAddressSet()
    {
        static AddressSet value{};
        return value;
    }

    const AddressSet& Get_mst_en_day3800_AddressSet();   // 1.0.15.4 english
    const AddressSet& Get_mst_jp_day3800_AddressSet();   // 1.0.15.4 japanese
    const AddressSet& Get_mst_en_day1820_AddressSet();   // 1.0.15.3 english
    const AddressSet& Get_mst_jp_day1820_AddressSet();   // 1.0.15.3 japanese
    const AddressSet& Get_mst_en_day3900_AddressSet();   // day3900 english
    const AddressSet& Get_mst_jp_day3900_AddressSet();   // day3900 japanese
    GameBuild DetectGameBuildFromVersionInfo(HMODULE hGame);
    bool ResolveAddressSet(HMODULE hGame);
    void InstallCrashHandler();

    inline const char* GetGameBuildName(GameBuild build)
    {
        switch (build)
        {
        case GameBuild::En_1_0_15_3: return "EN 1.0.15.3";
        case GameBuild::Jp_1_0_15_3: return "JP 1.0.15.3";
        case GameBuild::En_1_0_15_4: return "EN 1.0.15.4";
        case GameBuild::Jp_1_0_15_4: return "JP 1.0.15.4";
        case GameBuild::En_1_0_15_4a:  return "EN 1.0.15.4a (day3900)";
        case GameBuild::Jp_1_0_15_4a:  return "JP 1.0.15.4a (day3900)";
        default:                     return "Unknown";
        }
    }

    inline bool IsEnglishBuild(GameBuild b)  { return b == GameBuild::En_1_0_15_3 || b == GameBuild::En_1_0_15_4 || b == GameBuild::En_1_0_15_4a; }
    inline bool IsJapaneseBuild(GameBuild b) { return b == GameBuild::Jp_1_0_15_3 || b == GameBuild::Jp_1_0_15_4 || b == GameBuild::Jp_1_0_15_4a; }

    inline bool IsEn154Family(GameBuild b) { return b == GameBuild::En_1_0_15_4 || b == GameBuild::En_1_0_15_4a; }
    inline bool IsJp154Family(GameBuild b) { return b == GameBuild::Jp_1_0_15_4 || b == GameBuild::Jp_1_0_15_4a; }
}

#define gGameBuild (::AddressSetRuntime::GetGameBuild())
#define gAddr (::AddressSetRuntime::GetAddressSet())
#define ResolveAddressSet (::AddressSetRuntime::ResolveAddressSet)
#define InstallCrashHandler (::AddressSetRuntime::InstallCrashHandler)
#define GetGameBuildName (::AddressSetRuntime::GetGameBuildName)
