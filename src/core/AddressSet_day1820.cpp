#include "pch.h"

#include "AddressSet.h"

namespace AddressSetRuntime
{
    const AddressSet& Get_mst_en_day1820_AddressSet()   // 1.0.15.3 english
    {
        static const AddressSet value =
        {
            0x14147F240ull, // AddNoise
            0x1414DCB60ull, // AddNoticeInfo
            0x140015EF0ull, // ArrayBaseFree
            0x140989340ull, // BeginSoundSystem
            0x1473CFCD0ull, // CallImpl
            0x1473CFF10ull, // CallWithRadioType
            0x142285780ull, // CassettePlayerVtable
            0x149310440ull, // CassetteStart
            0x1414E1090ull, // CheckSightNoticeHostage
            0x140D685C0ull, // ConvertRadioTypeToLabel
            0x140FB9000ull, // CopyAndAdjustInfo
            0x140D6EAA0ull, // DecrementPhaseCounter
            0x14006B6D0ull, // FoxLuaRegisterLibrary
            0x1400855B0ull, // FoxPath_Path
            0x142ECE7F0ull, // FoxStrHash32
            0x14C1BD310ull, // FoxStrHash64
            0x145CB8890ull, // GameOverSetVisible
            0x145E5EE70ull, // GetCurrentMissionCode
            0x146C98180ull, // GetNameIdWithGameObjectId
            0x146C96520ull, // GetGameObjectIdWithIndex
            0x14614A4E0ull, // GetPlayingTime
            0x14614AA30ull, // GetPlayingTrackId
            0x140BFF3F0ull, // GetQuarkSystemTable
            0x14614C0C0ull, // GetTrackInfoByName
            0x14050B2A0ull, // GetUixUtilityToFeedQuarkEnvironment
            0x1404D2AD0ull, // GetVoiceLanguage
            0x140DA3170ull, // GetVoiceParamWithCallSign
            0x140015F20ull, // KernelAllocAligned
            0x146867240ull, // LoadPlayerVoiceFpk
            0x145CD0630ull, // LoadingScreenOrGameOverSplash2
            0x142BFFAC8ull, // MusicManager_s_instance
            0x14006B2E0ull, // PathHashCode
            0x140972C70ull, // PauseMusicPlayer
            0x140EF6BD0ull, // PlayOrPauseSelectedTrack
            0x140A69070ull, // RequestCorpse
            0x1409739E0ull, // ResumeMusicPlayer
            0x145F236F0ull, // SetEquipBackgroundTexture
            0x1408D78A0ull, // SetLuaFunctions
            0x141DC78F0ull, // SetTextureName
            0x140989120ull, // SoundSystemCtor
            0x14A2ACC00ull, // StateRadioRequest
            0x1414B8D20ull, // State_ComradeAction
            0x14A140940ull, // State_EnterDownHoldup
            0x14A140C00ull, // State_EnterStandHoldup1
            0x14A141500ull, // State_EnterStandHoldupUnarmed
            0x1414BC600ull, // State_RecoveryKick
            0x1414BCEF0ull, // State_RecoveryTouch
            0x1414BC7B0ull, // State_StandEnterRecoverySleepFaintHoldupComradeBySound
            0x14A141910ull, // State_StandHoldupCancelLookToPlayer
            0x1414BCA10ull, // State_StandRecoveryHoldup
            0x14150F2C0ull, // StepRadioDiscovery
            0x146150970ull, // StopMusicPlayer
            0x1404D2770ull, // SubtitleManager_Get
            0x149F65330ull, // UpdateOptCamo
            0x142C009F0ull, // g_SoundSystem
            0x14C1D7320ull, // lua_getfield
            0x14C1D7D40ull, // lua_gettop
            0x14C1D8C90ull, // lua_isnumber
            0x14C1D9250ull, // lua_isstring
            0x14C1DA960ull, // lua_objlen
            0x14C1DB230ull, // lua_pushboolean
            0x141A11BC0ull, // lua_pushnumber
            0x14C1E9320ull, // lua_rawgeti
            0x14C1EBBE0ull, // lua_settop
            0x141A12330ull, // lua_toboolean
            0x141A12390ull, // lua_tointeger
            0x141A123C0ull, // lua_tolstring
            0x141A12460ull, // lua_tonumber
            0x14C1ED760ull, // lua_type
            0x14C1E7EE0ull, // lua_pushstring
            0x14C1D6320ull, // lua_createtable
            0x14C1E9CF0ull, // lua_rawset
            0x14C1EB2B0ull, // lua_settable
            0x14C1E7CC0ull, // lua_pushnil
            0x14C1DA770ull, // lua_next
            0x14C1D7C10ull, // lua_gettable
            0x14C1E87E0ull, // lua_pushvalue
            0x141A11930ull, // lua_pcall
            0x14C1E67B0ull, // lua_pushcclosure
            0x145E62540ull, // GetIconFtexPath
            0x145CCFCC0ull, // LoadingTipsEv_UpdateActPhase
            0x14032ADF0ull, // Fox_Sd_ConvertParameterID
            0x143F42540ull, // Fox_Sd_Ad_AudioSoundEngine_RegisterGameObject
            0x14032B040ull, // Fox_Sd_Object_Activate
            0x140329C80ull, // Fox_Sd_Daemon_GetObject
            0x142B9E8B0ull, // Fox_Sd_Daemon_Singleton
            0x1468EDD50ull, // SoundControllerImpl_CallInternal
            0x149CFBA54ull, // TornadoDualPatch
            0x146ACC210ull, // RealizedSahelan2Impl_Realize
            0x146ACC650ull, // RealizedSahelan2Impl_SetFovaImpl
            0x144A3CBD0ull, // FormVariationFile2_ApplyOnlyMeshAndTextureVariation
            0x14BBC3630ull, // Sahelan_ActionCoreImpl_UpdateEyeLampColor
            0x14BBC37A0ull, // Sahelan_ActionCoreImpl_UpdateHeartLight
            0x14BCA4b70ull, // Sahelan_PhaseSneakAi_PushEyeColor
            0x140EF2EE0ull, // MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceNormal
            0x140EF32A0ull, // MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceServer
            0x140EF2CC0ull, // MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceEmergency
            0x145C0A890ull, // HudCommonDataManager_GetInstance
            0x14158B520ull, // Soldier2SoundController_Activate
            0x1441DBB80ull, // CAkResampler_SetPitch
            0x143F33A20ull, // FNVHash32
            0x145CB70BDull, // Play_bgm_gameover
            0x145CB70C4ull, // Play_bgm_gameover_paradox
            0x145CB70B6ull, // Play_bgm_gameover_perfectstealth
            0x145CB70CBull, // Play_bgm_s10010_gameover
            0x145CB8EF5ull, // Stop_bgm_gameover
            0x145CB8F06ull, // Stop_bgm_gameover_paradox
            0x145CB8EFAull, // Stop_bgm_gameover_perfectstealth
            0x145CB8F0Dull, // Stop_bgm_s10010_gameover
            0x14226bFC8ull, // Play_bgm_gameover_paradox_soundId
            0x14226BFCCull, // Stop_bgm_gameover_paradox_soundId
            0x140e2470full, // DD_vox_SH_voice
            0x140e24682ull, // DD_vox_SH_radio
            0x140e246ffull, // DD_vox_SH_radio2
            0x140e24707ull, // DD_vox_SH_radio3
            0x140921EC0ull, // MotherBaseMapCommonDataImpl_GetEnemyInformationLangId
            0x1415E4FE0ull, // TppUIBinoSubjectiveImpl_GetEnemyUnitName
            0x1410A9520ull, // BasicActionImpl_StateCrawlSideRoll
            0x1418FF8D0ull, // Sahelan_PhaseSneakAiImpl_PreUpdate
            0x142C69AA0ull, // Sahelan_PhaseSneakAiImpl_StepFuncsTable
            0x142C1fD24ull, // MessageResendCounter
            0x145e642a0ull, // GetMissionCodeCategory
            0x14162136Bull, // IconTitleGetLangTextCall
            0x146CC7170ull, // GameObject_SendCommand
            0x1496A8070ull, // UiControllerImpl_InitEquipHudData
            0x1414E6110ull, // NoticeControllerImpl_GetOccasionalChat
            0x140D83480ull, // SoldierConversationService_ConvertSpeechLabelToConversationType
            0x1414E6229ull, // OccasionalChat_FactionTestNop
            0x140932BE0ull, // MbDvcReserveAnnouncePopup
            0x140939EE0ull, // MbDvcPopupGateFn
            0x14151FE30ull, // NoticeNoiseAiImpl_StepAware
            0x141513B00ull, // NoticeIndisAiImpl_StepAware
            0x1414E5830ull, // NoticeControllerImpl_DoCheckSpreadNotice
            0x1414E3070ull, // NoticeControllerImpl_CheckSightNoticeSoldier
            0x140E46630ull, // NoticeNearThreatAiImpl_Wake
            0x140E47000ull, // NoticeNoiseAlertAiImpl_Wake
            0x14A8B9420ull, // NoticeNearGameObjectAiImpl_Wolf_Wake
            0x14A8BD650ull, // NoticeNoiseSneakAiImpl_Wolf_Wake
            0x146D89810ull, // NoticeNearGameObjectAiImpl_Bear_Wake (far body; thunk 0x140C35360)
            0x140C36BE0ull, // NoticeNoiseAiImpl_Bear_Wake
            0x140B15B00ull, // RealizedSoldier2Impl_ConvertHeadEquipModelType
            0x140B18ED0ull, // RealizedSoldier2Impl_UpdateHeadEquipMesh
            0x140B1AE60ull, // FovaController_GetActiveFovaResourceManager
            0x144A49BF0ull, // Fv2ResourceManager_GetModel
            0x149CD7DD0ull, // TimeCigaretteActionPluginImpl_ShowTimeCigaretteUi
            0x1408E5DB0ull, // HeliTaxi_CanHeliTaxi
            0x140F0D8D0ull, // HeliTaxi_CallRescueHeli
            0x140E0CD90ull, // HeliTaxi_StepWithdraw
            0x140910890ull, // HeliTaxi_GetLocationId
            0x145C468F0ull, // HeliTaxi_RequestMapPhase
            0x140E096E0ull, // HeliTaxi_StepGoToNav
            0x140E0A2F0ull, // HeliTaxi_StepTaxiCurrentCluster
            0x148F8CB80ull, // HeliTaxi_PassengerUpdate
            0x140E143C0ull, // MechaActionImpl_StateOff
            0x140E14800ull, // MechaActionImpl_StateOn
            0x148F8A4A0ull, // PassengerControllerImpl_IsPassengerClosingDoor
            0x1463FC140ull, // PlacedSystemImpl_BindResource
            0x140935760ull, // UiMarkerCommonDataImpl_RegisterLZMarkerInUpdate
            0x140E242C0ull, // HeliSoundControllerImpl_Update
            0x148FABEA0ull, // HeliSoundControllerImpl_CallVoice
            0x140DFD240ull, // HeliFlightControllerImpl_Update
            0x145C096B0ull, // Hud_GetAnnounceLogSE
            0x1408A2EA0ull, // Hud_TypingLogActUpdate
            0x141DC9ED0ull, // Ui_SoundControlStart
            0x14085F0C0ull, // Ui_UiCommonDataManagerGetInstance
            0x14085F190ull, // Ui_EventNodeBodyGetGraphState
            0x145F804D0ull, // VoiceParam_PlayDialogue
            0x149B603F0ull, // RideHeliActionPluginImpl_ExecPreMotionGraph
            0x14121EB90ull, // RideHeliActionPluginImpl_GetStateFn
            0x142BFF960ull, // SoundDaemon_Instance
            0x146142730ull, // SoundDaemon_PostEventQueue
            0x1420A72B0ull, // Sd_kW_Select
            0x140d7c7d0ull, // UpdateAntiAir
            0x147362370ull, // ClearAntiAir
            0x145F785D0ull, // GetChangeLocationMenuParameterByLocationId
            0x145F78B90ull, // GetMbFreeChangeLocationMenuParameter
            0x140925ef0ull, // GetPhotoAdditionalTextLangId
            0x140fe7db0ull, // UiControllerImpl_HideBinocle
            0x1466A5770ull, // AddCassetteTapeTrack
            0x149309EA0ull, // CollectGotTapes
            0x1466EC350ull, // IsGotCassetteTapeTrack
            0x140AAC670ull, // SetCassetteTapeTrackNewFlag
            0x140EF7A50ull, // SetCurrentAlbum
            0x140974880ull, // SetupMusicInfos
            0x1450487C0ull, // RadioCassette_SearchCasseteInfo
            0x1450475C0ull, // RadioCassette_GetCassetteMusic
            0x145047E90ull, // RadioCassette_IsGotCassette
            0x145047B10ull, // RadioCassette_GetCassetteSaveIndex
            0x1450C73E0ull, // RadioCassette_SdPostEvent
            0x140641CA0ull, // RadioCassette_RadioUpdate
            0x1466A5A50ull, // AddCassetteTapeTrackByIndex
            0x1451FCC50ull, // RadioCassette_ActivateUnit
            0x1405D56D0ull, // RadioCassette_IsSameSaveIndexFromName
            0x14126C060ull, // SearchLightActionPluginImpl_StateDoorStart
            0x14126B560ull, // SearchLightActionPluginImpl_StateDoorEnd
            0x140338B60ull, // SoundControl_PostExternalEvent
            0x140989B20ull, // MusicPlayerPlayWrapper
            0x145FA1510ull, // FUN_145fa1510
            0x1408B1A0Dull, // SetEquipItem
            0x141000780ull, // FUN_141000780
            0x14504E090ull, // FUN_14504e090
            0x1409C3AF5ull, // Barrier_LoadGate0
            0x1409C3B5Full, // Barrier_LoadGate1
            0x1409C3BC5ull, // Barrier_LoadGate2
            0x140FC3CC0ull, // FUN_140fc3cc0
            0x140A2A480ull, // FUN_140a2a480
            0x141B18900ull, // FUN_141b18900
            0x140C0C0B0ull, // FUN_140c0c0b0
            0x140BFF480ull, // FUN_140bff480
            0x140AF36D0ull, // FUN_140af36d0
            0x1404ECF80ull, // FUN_1404ecf80
            0x145D0B630ull, // EquipCrossEvCall_IsItemNoUse
            0x1497396C0ull, // AttackActionImpl_IsWeaponNoUseInPlaceAction
            0x1408B1A48ull, // EquipCrossSetEquipItem_Site1
            0x1408B1AA2ull, // EquipCrossSetEquipItem_Site2
            0x1408B1AE4ull, // EquipCrossSetEquipItem_Site3
            0x1466f3b10ull, // TppMotherBaseManagement_RegCstDev
            0x1466f4600ull, // TppMotherBaseManagement_RegFlwDev
            0x140f6b660ull, // EquipDevCtrl_GetSuitDevelopInfoIndex
            0x140f69f20ull, // EquipDevCtrl_GetFaceEquipDevelopInfoIndex
            0x1416a1aa0ull, // AddListSuit
            0x14691b460ull, // CamoufParamInfo_GetCamoufValue
            0x140fdc5d0ull, // CamouflageController_ExecSuitCorrect
            0x1411b0d10ull, // EquipController_SetHandSlotEnabled
            0x141675600ull, // EquipDevelopCallbackImpl_SetSupplyCBoxInfo
            0x14a56bfa0ull, // IsEnableCurrentSuit
            0x1416a4280ull, // ItemSelectorCallbackImpl_DecideActMissionPreparationSetEquipMode
            0x1416a51c0ull, // ItemSelectorCallbackImpl_DecideActMotherBaseDeviceSupportDropMode
            0x1416a9b80ull, // ItemSelectorCallbackImpl_SetupPrefabListElement
            0x1416af270ull, // ItemSelectorRecordCallFunc_UpdateRecords
            0x14a53c210ull, // ItemSelector_AddListBandana
            0x140ae90f0ull, // LoadPlayerBionicArmFpk
            0x140ae9040ull, // LoadPlayerBionicArmFv2
            0x146864180ull, // LoadPlayerCamoFpk
            0x146863f80ull, // LoadPlayerCamoFv2
            0x146866c80ull, // LoadPlayerPartsFpk
            0x146865f80ull, // LoadPlayerPartsParts
            0x146864e30ull, // LoadPlayerSnakeBlackDiamondFpk
            0x146864c80ull, // LoadPlayerSnakeBlackDiamondFv2
            0x140ae8df0ull, // LoadPlayerSnakeFaceFpk
            0x140ae8ce0ull, // LoadPlayerSnakeFaceFv2
            0x1460b9fa0ull, // MissionPrepSystem_IsEnableHeadOptionSuit
            0x14a56ba20ull, // MissionPrep_IsEnableCurrentHeadOption
            0x146239160ull, // Player2BlockController_LoadPartsNew
            0x1409c5d00ull, // Player2GameObjectImpl_ProcessSignal
            0x1409ca560ull, // Player2Impl_SetUpParts
            0x1462c93f0ull, // Player2UtilityImpl_LoadoutApplyAfterSetSuit
            0x1462c7670ull, // Player2UtilityImpl_SetInitialConditionWithLoadoutInfo
            0x1409defe0ull, // Player2UtilityImpl_SetSuitAndHandConditionWithLoadoutInfo
            0x14622a3b0ull, // Player_ConverFaceIdWithFaceEquipId
            0x140ae84b0ull, // ResourceTable_DoesNeedFaceFova
            0x140ae8500ull, // ResourceTable_DoesNeedFaceFovaForAvatar
            0x1409c45c0ull, // Sys_IsArtificialHandEnabled
            0x141e02d80ull, // Sys_IsArtificialHandEnabledForCurrentPlayerType
            0x1409cc380ull, // UpdatePartsStatus
            0x1416a3670ull, // ItemSelectorCallbackImpl_DecideActMotherBaseCustomize
            0x143232490ull, // Fox_Path_Exists
            0x14319eb70ull, // Fox_Path_Dtor
            0x14d9aa3d0ull, // PluginFacial_ApplyMotion
            0x140f6d8e0ull, // EquipDevCtrl_IsEquipVisile
            0x14950ed50ull, // EquipDevCtrl_GetHeadBadgeCategory
            0x1460b33f0ull, // MissionPrep_GetWornHeadCategory
            0x14a544d40ull, // MissionPrep_SetInitialSelectRecord
            0x1464a0e00ull, // TppEquip_RegisterConstant
            0x141e02930ull, // PlayerInfoInterfaceImpl_GetPartsTypeAtCamoType
            0x142c20fb0ull, // EquipIdTable_InfoList
            0x142a70928ull, // EquipIdTable_TypeWords
            0x1464b6740ull, // EquipIdTable_ReloadEquipIdTable
            0x14950d190ull, // EquipDevelopCtrl_GetEquipDevelopIndex
            0x149527750ull, // EquipDevelopCtrl_SetEquipUndeveloped
            0x149519770ull, // EquipDevCtrl_GetSuitCamoType
            0x149519990ull, // EquipDevCtrl_GetSuitLevel
            0x140f6d7a0ull, // EquipDevCtrl_IsEquipSuit
            0x140f6ed90ull, // EquipDevelopCtrl_SetEquipDeveloped
            0x1412a2f80ull, // SupplyCboxActionPluginImpl_StateInBox
            0x140863050ull, // HudCommonDataManager_AnnounceLogView
            0x145e6ab60ull, // Ui_LangIdToKey
            0x1409122d0ull, // Ui_GetLangText
            0x141393B8Eull, // Soldier_ShootOneBullet_GroupMaskCall
            0x140D2DB6Dull, // Soldier_ActivateBulletAtEmptyWork_SameArmyJnz
            0x1408A88E0ull, // TelopStartTitleEvCall_SetBgTexture
            0x141DAEF60ull, // Layout_GetLayout
            0x141DAEFA0ull, // Layout_GetModel
            0x146081DB0ull, // GetCassetteTapeUnreadInfo
            0x1466EE5A0ull, // IsNewCassetteTapeTrack
            0x149308D60ull, // CassetteMenuCheckNewFlag
            0x1493087C0ull, // CassetteAlbumCheckNewFlag
            0x149309C50ull, // CassetteCheckUnreadInfo
            0x144A06660ull, // SubtitlesObjectSendMessage
            0x14951ed70ull, // EquipDevCtrl_IsEquipDevelopable
            0x146860380ull, // LoadAvatarFaceFv2
            0x1468606e0ull, // LoadAvatarFaceFpk
            0x1409B56D0ull, // AvatarFaceEditUpdate
            0x144A4A350ull, // Fox_ModelFromHandle
            0x14685DA20ull, // LoadAvatarHeadOptionFv2
            0x14685DD50ull, // LoadAvatarHeadOptionFpk
            0x1495264F0ull, // EquipDevelopCtrl_SetEnableDevelop
            0x1464C1180ull, // ReloadEquipParameterTables2
            0x142C25C30ull, // GunBasicParameters2Buffer
            0x142A711F0ull, // EquipParameterTablesImpl_Instance
            0x1423495C0ull, // MotionLoaderImpl_ReceiverTypeTable
            0x147413D10ull, // MotionLoaderImpl_GetReceiverType
            0x1423495A8ull, // MotionLoaderImpl_UnderBarrelTypeTable (Ghidra EN153; =ReceiverTable-0x18, read by GetUnderBarrelType)
            0x147414460ull, // MotionLoaderImpl_GetUnderBarrelType (Ghidra EN153; return (&DAT_1423495a8)[id])
            0x14951AC10ull, // EquipDevelopControllerImpl_GetSuppressorAmount
            0x142BDCFE0ull, // DamageParameterTable_Instance
            0x145000FF0ull, // DamageParameterTable_ReloadDamageParameter
            0x144FFB300ull, // DamageParameterTable_GetDamageParameter
            0x149500A30ull, // EquipDevCtrl_GetBaseDevelopId
            0x141679d50ull, // MenuDevelopGrid_FillGrid
            0x1416796d0ull, // MenuDevelopGrid_CopyGrid
            0x14a4ef340ull, // MenuDevelopGrid_CountBadge
            0x14167a2f0ull, // MenuDevelopGrid_FillFlat
            0x14a4f7130ull, // MenuDevelopGrid_CopyFlat
            0x1412829a0ull, // SightManager_UpdateMissileLockOn
            0x140d2fd20ull, // Bullet3_DoSimulation
            0x147215650ull, // Bullet3_ActivateBulletAtEmptyWork
            0x149c6cd60ull, // SightManager_Update
            0x149c6e5c0ull, // SightManager_UpdateMissileLockOnUi
            0x145debb40ull, // LockOnReticleFactory_CreateWindow
            0x140A3B5E0ull, // EquipParams_GetAttackIdByEquipId
            0x140dc35e0ull, // EquipSystem_SetUpGunInfoFromGunPartsDesc
            0x141daba30ull, // UiWindowFunction_FindWindow
            0x141dabd70ull, // UiWindowFunction_PostShowAndStartMessage
            0x141dabab0ull, // UiWindowFunction_GetLayout
            0x141041950ull, // AttackAction_Fire
            0x141303a40ull, // EquipObject_DoFire
            0x1411574f0ull, // CorePlugin_UpdateLoadoutRequest
            0x140dc0120ull, // WeaponSystem_DefineWeaponFireSound
            0x142C93480ull, // EquipSystem_ChimeraPartsSetWork
            0x140EFC010ull, // MbDvcUpdateTrackListCallFuncs
            0x140EF7680ull, // MbDvcRefreshTrackListPrefabParameter
            0x140EF6F30ull, // MbDvcTrackListRecordRefresh
            0x140F31E50ull, // MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter
            0x149459D50ull, // MbDvcMissionListCallbackImpl_SetMenuHelp 
            0x1464B6340ull, // Equip_ReloadChimeraPartsInfoTable 
            0x142C207C0ull, // Equip_ChimeraPartsPackageInfos
            0x141A75720ull, // Mtar_GetAnimFile
            0x141A762F0ull, // Mtar_GetDataInfo
            0x142A6B400ull, // Equip_MotionEntryTable
            0x142A71328ull, // Equip_MotionMtarResolver
            0x1422E24F0ull, // SimplePartsControllerImpl_Vtable 
            0x146826550ull, // SimplePartsControllerImpl_SetMotionData 
            0x146826900ull, // SimplePartsControllerImpl_SetMotionDataByPath 
            0x140DBFA70ull, // EquipSystemImpl_HideMagazine
            0x140DBEF70ull, // EquipSystemImpl_GetPartsController
            0x140BFF3F0ull, // Fox_GetQuarkSystemTable
            0x140071030ull, // Fox_ArrayBaseExtend
            0x1420BCAC8ull, // Fox_ArrayOperatorVtbl
            0x141AB7100ull, // Animx_GetControlSize
            0x141AB6B20ull, // Animx_SimpleControlCtorPool
            0x141AB6EA0ull, // Animx_SimpleControlCtorHeap
            0x141AB6D00ull, // Animx_SimpleControlDtorPool
            0x141AB6F10ull, // Animx_SimpleControlDtorHeap
            0x141AB6E90ull, // Animx_SimpleControlAux
            0x145045590ull, // Equip_MotionEntrySlotHookA
            0x1450464F0ull, // Equip_MotionEntrySlotHookB
            0x140A29CB0ull, // EquipIdTable_GetEquipTypeId
            0x1414404D0ull, // Soldier2InterrogateUtil_UpdateInterrogation
            0x141441EB0ull, // Soldier2InterrogateUtil_UpdateInterrogationMarker
            0x1468F0540ull, // SoundControllerImpl_CallVoice
            0x14930D910ull, // MbDvcCassetteTapeCallbackImpl_SeekMusic
            0x14742D6D0ull, // EquipSystem_GetGunInfoById
            0x1460257C0ull, // Reticle_InitHandGunAsset
            0x149CD4AA0ull, // TimeCigaretteActionPluginImpl_HideTimeCigaretteUi
            0x143173d10ull, // Fox_EntityInfoMapTeardown
            0x142AC6D10ull, // WeaponEnhanceLangIdArray
            0x149581170ull, // WeaponEnhance_GetIndexForRegist
            0x140fa0260ull, // WeaponEnhance_GetMessageIdArray
            0x1495812F0ull, // WeaponEnhance_GetLangIdById
            0x1495814B0ull, // WeaponEnhance_GetLangIdBound
            0x140b3b690ull, // BulletEffectController_CreateEffect
            0x1416A2160ull, // ItemSelectorCallbackImpl_AddRecord
            0x140AE8960ull, // LoadPlayerLilChickCapFv2
            0x140AE8A00ull, // LoadPlayerLilChickCapFpk
            0x140AE8AA0ull, // LoadPlayerChickenCapFv2
            0x140AE8B40ull, // LoadPlayerChickenCapFpk
            0x140AE3C40ull, // AdditionalMotionTable_GetMtarPathId
            0x141C2EA80ull, // ModelVertexColorChange_GetModelTotalVertexNum
            0x1416A33F0ull, // ItemSelector_AddDevelopWeaponList
            0x1409B6F40ull, // BlockControllerImpl_UpdateAdditionalMotionBlock
            0x14006D440ull, // Fox_Block_GetFileForPathId
            0x14006D230ull, // Fox_BlockGroup_GetBlockAtIndex
            0x140DB9630ull, // EquipPreviewSystem_RequestLoadEquip
            0x1416AD560ull, // ItemSelector_StartEquipPreviewImpl
            0ull,           // EquipBlockController2_RequestLoad TODO EN 15.3
            0ull,           // EquipBlockController2_Instance TODO EN 15.3
            0ull,           // ItemSelector_UpdateSelect TODO EN 15.3
            0x141682D50ull, // UiEquipPreviewController_ScrollNext
            0x141682BA0ull, // UiEquipPreviewController_ScrollPrev
            0ull,           // UiEquipPreviewController_StopEquipPreview TODO EN 15.3
            0ull,           // Player2Impl_AddAdditionalMtarAll TODO EN 15.3
            0ull,           // Player2Impl_RemoveAdditionalMtarAll TODO EN 15.3
            0x1423496B0ull, // MotionLoaderImpl_BarrelTypeTable (Ghidra EN153; =ReceiverTable+0xF0)
            0x147410F10ull, // MotionLoaderImpl_GetBarrelType (Ghidra EN153; sole xref of 1423496b0)
            0x142349730ull, // MotionLoaderImpl_MagazineTypeTable (Ghidra EN153; =ReceiverTable+0x170)
            0x147412460ull, // MotionLoaderImpl_GetMagazineType (Ghidra EN153; sole xref of 142349730)
            0x1423497F0ull, // MotionLoaderImpl_SightTypeTable (Ghidra EN153; =ReceiverTable+0x230)
            0x147413F80ull, // MotionLoaderImpl_GetSightType (Ghidra EN153; sole xref of 1423497f0)
            0x14120B025ull, // TppPickable_ItemWindowBoundSite (EN15.3; CMP EBX,0x209, JG -> +0xA9)
            0x1409147F0ull, // UiUtility_GetWeaponItemNameLangId (EN15.3; 3 call sites converge)
        };
        return value;
    }
    const AddressSet& Get_mst_jp_day1820_AddressSet()   // 1.0.15.3 japanese
    {
        static const AddressSet value =
        {
            0x14147F210ull, // AddNoise
            0x1414DCB30ull, // AddNoticeInfo
            0x140015EE0ull, // ArrayBaseFree
            0x140988E20ull, // BeginSoundSystem
            0x149531DB0ull, // CallImpl
            0x149532190ull, // CallWithRadioType
            0x142285700ull, // CassettePlayerVtable
            0x149CD67B0ull, // CassetteStart
            0x1414E1060ull, // CheckSightNoticeHostage
            0x140D68330ull, // ConvertRadioTypeToLabel
            0x140FB90D0ull, // CopyAndAdjustInfo
            0x140D6E810ull, // DecrementPhaseCounter
            0x1431CC520ull, // FoxLuaRegisterLibrary
            0x143227580ull, // FoxPath_Path
            0x142EB6C10ull, // FoxStrHash32
            0x14C96BF00ull, // FoxStrHash64
            0x1477CFCB0ull, // GameOverSetVisible
            0x147A691E0ull, // GetCurrentMissionCode
            0x148A58CB0ull, // GetNameIdWithGameObjectId
            0x148A57620ull, // GetGameObjectIdWithIndex
            0x147DE8FA0ull, // GetPlayingTime
            0x147DE93E0ull, // GetPlayingTrackId
            0x140BFEF80ull, // GetQuarkSystemTable
            0x147DEA880ull, // GetTrackInfoByName
            0x14050ae60ull, // GetUixUtilityToFeedQuarkEnvironment
            0x1404D25C0ull, // GetVoiceLanguage
            0x140DA30D0ull, // GetVoiceParamWithCallSign
            0x140015F10ull, // KernelAllocAligned
            0x14844E550ull, // LoadPlayerVoiceFpk
            0x1477ED2F0ull, // LoadingScreenOrGameOverSplash2
            0x142BFFAC8ull, // MusicManager_s_instance
            0x14C96C160ull, // PathHashCode
            0x147DEAC30ull, // PauseMusicPlayer
            0x140EF6D40ull, // PlayOrPauseSelectedTrack
            0x140A68B60ull, // RequestCorpse
            0x1409734D0ull, // ResumeMusicPlayer
            0x147A8C170ull, // SetEquipBackgroundTexture
            0x1408D72B0ull, // SetLuaFunctions
            0x141DC7930ull, // SetTextureName
            0x140988C00ull, // SoundSystemCtor
            0x14ACA5E60ull, // StateRadioRequest
            0x1414B8CF0ull, // State_ComradeAction
            0x14AB05B80ull, // State_EnterDownHoldup
            0x14AB05D90ull, // State_EnterStandHoldup1
            0x14AB06770ull, // State_EnterStandHoldupUnarmed
            0x1414BC5D0ull, // State_RecoveryKick
            0x1414BCEC0ull, // State_RecoveryTouch
            0x1414BC780ull, // State_StandEnterRecoverySleepFaintHoldupComradeBySound
            0x14AB06C40ull, // State_StandHoldupCancelLookToPlayer
            0x1414BC9E0ull, // State_StandRecoveryHoldup
            0x14150F290ull, // StepRadioDiscovery
            0x147DEF390ull, // StopMusicPlayer
            0x1404D2260ull, // SubtitleManager_Get
            0x14A96AF20ull, // UpdateOptCamo
            0x142C009F0ull, // g_SoundSystem
            0x14C987300ull, // lua_getfield
            0x14C987CB0ull, // lua_gettop
            0x14C988960ull, // lua_isnumber
            0x14C988CA0ull, // lua_isstring
            0x14C98A230ull, // lua_objlen
            0x14C98B310ull, // lua_pushboolean
            0x14C98D800ull, // lua_pushnumber
            0x14C98Ebc0ull, // lua_rawgeti
            0x14C990ED0ull, // lua_settop
            0x14C991120ull, // lua_toboolean
            0x14C991B80ull, // lua_tointeger
            0x14C992060ull, // lua_tolstring
            0x14C9924D0ull, // lua_tonumber
            0x14C9935F0ull, // lua_type
            0x14C98DCB0ull, // lua_pushstring
            0x14C986520ull, // lua_createtable
            0x14C98ED50ull, // lua_rawset
            0x14C990BD0ull, // lua_settable
            0x14C98D570ull, // lua_pushnil
            0x14C98A010ull, // lua_next
            0x14C987B90ull, // lua_gettable
            0x14C98E1D0ull, // lua_pushvalue
            0x141A11A50ull, // lua_pcall
            0x14C98C080ull, // lua_pushcclosure


            0x147A6BD40ull, // GetIconFtexPath
            0x1477EC6F0ull, // LoadingTipsEv_UpdateActPhase
            0x14032A870ull, // Fox_Sd_ConvertParameterID
            0x143F7BCA0ull, // Fox_Sd_Ad_AudioSoundEngine_RegisterGameObject
            0x14032AAC0ull, // Fox_Sd_Object_Activate
            0x140329710ull, // Fox_Sd_Daemon_GetObject
            0x142B9E8B0ull, // Fox_Sd_Daemon_Singleton
            0x1484D84E0ull, // SoundControllerImpl_CallInternal


            0x14A6C34B4ull, // TornadoDualPatch


            0x148655E70ull, // RealizedSahelan2Impl_Realize
            0x148656360ull, // RealizedSahelan2Impl_SetFovaImpl
            0x1448A0190ull, // FormVariationFile2_ApplyOnlyMeshAndTextureVariation
            0x14B7801D0ull, // Sahelan_ActionCoreImpl_UpdateEyeLampColor
            0x14B7807B0ull, // Sahelan_ActionCoreImpl_UpdateHeartLight
            0x14B84DEC0ull, // Sahelan_PhaseSneakAi_PushEyeColor


            0x140EF3050ull, // MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceNormal
            0x140EF3410ull, // MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceServer
            0x140EF2E30ull, // MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceEmergency

            0x147719930ull, // HudCommonDataManager_GetInstance

            0x14158B4F0ull, // Soldier2SoundController_Activate
            0x14418D980ull, // CAkResampler_SetPitch


            0x143F6EE50ull, // FNVHash32
            0x1477CD50Cull, // Play_bgm_gameover
            0x1477CD513ull, // Play_bgm_gameover_paradox
            0x1477CD505ull, // Play_bgm_gameover_perfectstealth
            0x1477CD51Aull, // Play_bgm_s10010_gameover
            0x1477D0274ull, // Stop_bgm_gameover
            0x1477D0285ull, // Stop_bgm_gameover_paradox
            0x1477D0279ull, // Stop_bgm_gameover_perfectstealth
            0x1477D028Cull, // Stop_bgm_s10010_gameover
            0x14226BF18ull, // Play_bgm_gameover_paradox_soundId
            0x14226BF1Cull, // Stop_bgm_gameover_paradox_soundId


            0x140E247DFull, // DD_vox_SH_voice
            0x140E24752ull, // DD_vox_SH_radio
            0x140E247CFull, // DD_vox_SH_radio2
            0x140E247D7ull, // DD_vox_SH_radio3

            0x1409218E0ull, // MotherBaseMapCommonDataImpl_GetEnemyInformationLangId
            0x1415E5150ull, // TppUIBinoSubjectiveImpl_GetEnemyUnitName

            0x1410A6CA0ull, // BasicActionImpl_StateCrawlSideRoll

            0x1418FFA10ull, // Sahelan_PhaseSneakAiImpl_PreUpdate
            0x142C69AA0ull, // Sahelan_PhaseSneakAiImpl_StepFuncsTable

            0x142C1FD24ull, // MessageResendCounter

            0x147a6cfe0ull, // GetMissionCodeCategory



            0x1416214bbull, // IconTitleGetLangTextCall

            0x148b0e440ull, // GameObject_SendCommand


            0x14A10B530ull, // UiControllerImpl_InitEquipHudData

            0x1414E60E0ull, // NoticeControllerImpl_GetOccasionalChat

            0x140D833D0ull, // SoldierConversationService_ConvertSpeechLabelToConversationType

            0x1414e61f9ull, // OccasionalChat_FactionTestNop

            0x1409325E0ull, // MbDvcReserveAnnouncePopup
            0x140915150ull, // MbDvcPopupGateFn

            0x14151FE00ull, // NoticeNoiseAiImpl_StepAware
            0x141513AD0ull, // NoticeIndisAiImpl_StepAware
            0x1414E5800ull, // NoticeControllerImpl_DoCheckSpreadNotice
            0x1414E3040ull, // NoticeControllerImpl_CheckSightNoticeSoldier
            0x140E46700ull, // NoticeNearThreatAiImpl_Wake
            0x140E470D0ull, // NoticeNoiseAlertAiImpl_Wake
            0x14B23EB90ull, // NoticeNearGameObjectAiImpl_Wolf_Wake
            0x14B243340ull, // NoticeNoiseSneakAiImpl_Wolf_Wake
            0x148C72F50ull, // NoticeNearGameObjectAiImpl_Bear_Wake
            0x140C36760ull, // NoticeNoiseAiImpl_Bear_Wake

            0x140B15650ull, // RealizedSoldier2Impl_ConvertHeadEquipModelType
            0x140B18A20ull, // RealizedSoldier2Impl_UpdateHeadEquipMesh
            0x140B1A9B0ull, // FovaController_GetActiveFovaResourceManager
            0x1448ACA50ull, // Fv2ResourceManager_GetModel

            0x14A6347F0ull, // TimeCigaretteActionPluginImpl_ShowTimeCigaretteUi

            0x1408E57C0ull, // HeliTaxi_CanHeliTaxi
            0x149D09260ull, // HeliTaxi_CallRescueHeli
            0x140E0CE40ull, // HeliTaxi_StepWithdraw
            0x1409102C0ull, // HeliTaxi_GetLocationId
            0x14779D2F0ull, // HeliTaxi_RequestMapPhase
            0x140E09790ull, // HeliTaxi_StepGoToNav
            0x140E0A3A0ull, // HeliTaxi_StepTaxiCurrentCluster
            0x14973F220ull, // HeliTaxi_PassengerUpdate
            0x140E14470ull, // MechaActionImpl_StateOff
            0x140E148B0ull, // MechaActionImpl_StateOn
            0x14973C110ull, // PassengerControllerImpl_IsPassengerClosingDoor

            0x14804A500ull, // PlacedSystemImpl_BindResource
            0x140935170ull, // UiMarkerCommonDataImpl_RegisterLZMarkerInUpdate

            0x140E24390ull, // HeliSoundControllerImpl_Update
            0x14975E670ull, // HeliSoundControllerImpl_CallVoice
            0x140DfD310ull, // HeliFlightControllerImpl_Update
            0x147718790ull, // Hud_GetAnnounceLogSE
            0x1408a29c0ull, // Hud_TypingLogActUpdate
            0x141DC9F10ull, // Ui_SoundControlStart
            0x1476cda70ull, // Ui_UiCommonDataManagerGetInstance
            0x1404b2220ull, // Ui_EventNodeBodyGetGraphState
            0x147b92a30ull, // VoiceParam_PlayDialogue
            0x14a4979a0ull, // RideHeliActionPluginImpl_ExecPreMotionGraph
            0x14121ecb0ull, // RideHeliActionPluginImpl_GetStateFn

            0x142bff960ull, // SoundDaemon_Instance
            0x147ddeb40ull, // SoundDaemon_PostEventQueue
            0x1420a72b0ull, // Sd_kW_Select
            0x140d7c720ull, // UpdateAntiAir
            0x1494bfe40ull, // ClearAntiAir
            0x147b88d00ull, // GetChangeLocationMenuParameterByLocationId
            0x147b897d0ull, // GetMbFreeChangeLocationMenuParameter
            0x140925910ull, // GetPhotoAdditionalTextLangId
            0x140fe7e80ull, // UiControllerImpl_HideBinocle
            0x140A87290ull, // AddCassetteTapeTrack
            0x140EF6540ull, // CollectGotTapes
            0x140A95740ull, // IsGotCassetteTapeTrack
            0x140AAC180ull, // SetCassetteTapeTrackNewFlag
            0x149CD4320ull, // SetCurrentAlbum
            0x140974370ull, // SetupMusicInfos
            0x144EBCCC0ull, // RadioCassette_SearchCasseteInfo
            0x144EBBC20ull, // RadioCassette_GetCassetteMusic
            0x144EBC1E0ull, // RadioCassette_IsGotCassette
            0x144EBBE30ull, // RadioCassette_GetCassetteSaveIndex
            0x145032460ull, // RadioCassette_SdPostEvent
            0x140641AC0ull, // RadioCassette_RadioUpdate
            0x1482D7CB0ull, // AddCassetteTapeTrackByIndex
            0x1451E1DF0ull, // RadioCassette_ActivateUnit
            0x1405D5420ull, // RadioCassette_IsSameSaveIndexFromName
            0x14126C190ull, // SearchLightActionPluginImpl_StateDoorStart
            0x14126B690ull, // SearchLightActionPluginImpl_StateDoorEnd
            0x1403385E0ull, // SoundControl_PostExternalEvent
            0x140989600ull, // MusicPlayerPlayWrapper
            0x147BB2DA0ull, // FUN_147bb2da0
            0x1408B152Dull, // SetEquipItem
            0x1410007D0ull, // FUN_1410007d0
            0x144EC3FF0ull, // FUN_144ec3ff0
            0x1409C35B5ull, // Barrier_LoadGate0
            0x1409C361Full, // Barrier_LoadGate1
            0x1409C3685ull, // Barrier_LoadGate2
            0x140FC3D90ull, // FUN_140fc3d90
            0x140A29F50ull, // FUN_140a29f50
            0x141B18A40ull, // FUN_141b18a40
            0x140C0BC30ull, // FUN_140c0bc30
            0x140BFF010ull, // FUN_140bff010
            0x140AF3210ull, // FUN_140af3210
            0x1404ECA60ull, // FUN_1404eca60
            0x1478375C0ull, // EquipCrossEvCall_IsItemNoUse
            0x14A1AE850ull, // AttackActionImpl_IsWeaponNoUseInPlaceAction
            0x1408B1568ull, // EquipCrossSetEquipItem_Site1
            0x1408B15C2ull, // EquipCrossSetEquipItem_Site2
            0x1408B1604ull, // EquipCrossSetEquipItem_Site3
            0x148332810ull, // TppMotherBaseManagement_RegCstDev
            0x148333ac0ull, // TppMotherBaseManagement_RegFlwDev
            0x140f6b760ull, // EquipDevCtrl_GetSuitDevelopInfoIndex
            0x140f6a020ull, // EquipDevCtrl_GetFaceEquipDevelopInfoIndex
            0x1416a1bf0ull, // AddListSuit
            0x148508540ull, // CamoufParamInfo_GetCamoufValue
            0x140fdc6a0ull, // CamouflageController_ExecSuitCorrect
            0x1411b0e40ull, // EquipController_SetHandSlotEnabled
            0x141675740ull, // EquipDevelopCallbackImpl_SetSupplyCBoxInfo
            0x14b01a880ull, // IsEnableCurrentSuit
            0x1416a43d0ull, // ItemSelectorCallbackImpl_DecideActMissionPreparationSetEquipMode
            0x1416a5320ull, // ItemSelectorCallbackImpl_DecideActMotherBaseDeviceSupportDropMode
            0x1416a9cd0ull, // ItemSelectorCallbackImpl_SetupPrefabListElement
            0x1416af3c0ull, // ItemSelectorRecordCallFunc_UpdateRecords
            0x14afd6ea0ull, // ItemSelector_AddListBandana
            0x140ae8c30ull, // LoadPlayerBionicArmFpk
            0x140ae8b80ull, // LoadPlayerBionicArmFv2
            0x14844b070ull, // LoadPlayerCamoFpk
            0x14844aea0ull, // LoadPlayerCamoFv2
            0x14844de90ull, // LoadPlayerPartsFpk
            0x14844db10ull, // LoadPlayerPartsParts
            0x14844cde0ull, // LoadPlayerSnakeBlackDiamondFpk
            0x14844c6f0ull, // LoadPlayerSnakeBlackDiamondFv2
            0x140ae8930ull, // LoadPlayerSnakeFaceFpk
            0x140ae8820ull, // LoadPlayerSnakeFaceFv2
            0x147cd7780ull, // MissionPrepSystem_IsEnableHeadOptionSuit
            0x14b01a3f0ull, // MissionPrep_IsEnableCurrentHeadOption
            0x147eeb520ull, // Player2BlockController_LoadPartsNew
            0x1409c57c0ull, // Player2GameObjectImpl_ProcessSignal
            0x147f47c60ull, // Player2Impl_SetUpParts
            0x147f89ce0ull, // Player2UtilityImpl_LoadoutApplyAfterSetSuit
            0x147f876f0ull, // Player2UtilityImpl_SetInitialConditionWithLoadoutInfo
            0x1409dead0ull, // Player2UtilityImpl_SetSuitAndHandConditionWithLoadoutInfo
            0x147edbee0ull, // Player_ConverFaceIdWithFaceEquipId
            0x140ae7ff0ull, // ResourceTable_DoesNeedFaceFova
            0x140ae8040ull, // ResourceTable_DoesNeedFaceFovaForAvatar
            0x1409c4080ull, // Sys_IsArtificialHandEnabled
            0x141e02db0ull, // Sys_IsArtificialHandEnabledForCurrentPlayerType
            0x1409cbe40ull, // UpdatePartsStatus
            0x1416a37c0ull, // ItemSelectorCallbackImpl_DecideActMotherBaseCustomize
            0x1432be400ull, // Fox_Path_Exists
            0x143227e40ull, // Fox_Path_Dtor
            0x14e02b500ull, // PluginFacial_ApplyMotion
            0x140f6d9e0ull, // EquipDevCtrl_IsEquipVisile
            0x149f28b70ull, // EquipDevCtrl_GetHeadBadgeCategory
            0x147cd19f0ull, // MissionPrep_GetWornHeadCategory
            0x14afe4630ull, // MissionPrep_SetInitialSelectRecord
            0x1480e0f70ull, // TppEquip_RegisterConstant
            0x141e02960ull, // PlayerInfoInterfaceImpl_GetPartsTypeAtCamoType
            0x142c20fb0ull, // EquipIdTable_InfoList
            0x142a70928ull, // EquipIdTable_TypeWords
            0x1480f8110ull, // EquipIdTable_ReloadEquipIdTable
            0x149f26dd0ull, // EquipDevelopCtrl_GetEquipDevelopIndex
            0x149f44b70ull, // EquipDevelopCtrl_SetEquipUndeveloped
            0x149f32490ull, // EquipDevCtrl_GetSuitCamoType
            0x149f326c0ull, // EquipDevCtrl_GetSuitLevel
            0x140f6d8a0ull, // EquipDevCtrl_IsEquipSuit
            0x149f42df0ull, // EquipDevelopCtrl_SetEquipDeveloped
            0x1412a30a0ull, // SupplyCboxActionPluginImpl_StateInBox
            0x140862ce0ull, // HudCommonDataManager_AnnounceLogView
            0x147a74800ull, // Ui_LangIdToKey
            0x140911d00ull, // Ui_GetLangText
            0x141393B6Eull, // Soldier_ShootOneBullet_GroupMaskCall
            0x140D2D7CDull, // Soldier_ActivateBulletAtEmptyWork_SameArmyJnz
            0x1408A8400ull, // TelopStartTitleEvCall_SetBgTexture
            0x141DAEF70ull, // Layout_GetLayout
            0x141DAEFB0ull, // Layout_GetModel
            0x147CB2BB0ull, // GetCassetteTapeUnreadInfo
            0x140A95BA0ull, // IsNewCassetteTapeTrack
            0x149CCDE50ull, // CassetteMenuCheckNewFlag
            0x149CCD510ull, // CassetteAlbumCheckNewFlag
            0x149CCF740ull, // CassetteCheckUnreadInfo
            0x144868DF0ull, // SubtitlesObjectSendMessage
            0x149f38f70ull, // EquipDevCtrl_IsEquipDevelopable
            0x148446230ull, // LoadAvatarFaceFv2
            0x148446630ull, // LoadAvatarFaceFpk
            0x1409B51B0ull, // AvatarFaceEditUpdate
            0x1448ACC50ull, // Fox_ModelFromHandle
            0x148442AF0ull, // LoadAvatarHeadOptionFv2
            0x148442EF0ull, // LoadAvatarHeadOptionFpk
            0x149F42BE0ull, // EquipDevelopCtrl_SetEnableDevelop
            0x140A40EC0ull, // ReloadEquipParameterTables2
            0x142C25C30ull, // GunBasicParameters2Buffer
            0x142A711F0ull, // EquipParameterTablesImpl_Instance
            0x142349520ull, // MotionLoaderImpl_ReceiverTypeTable
            0x14957D790ull, // MotionLoaderImpl_GetReceiverType
            0x142349508ull, // MotionLoaderImpl_UnderBarrelTypeTable (JP153 dump; .bss =ReceiverTable-0x18, read by accessor 0x14957dcc0)
            0x14957DCC0ull, // MotionLoaderImpl_GetUnderBarrelType (JP153 dump; far body LEA [142349508]+MOVZX[RAX+RCX], reads UnderBarrelTypeTable)
            0x140F6BAF0ull, // EquipDevelopControllerImpl_GetSuppressorAmount
            0x142BDCFE0ull, // DamageParameterTable_Instance
            0x144E87260ull, // DamageParameterTable_ReloadDamageParameter
            0x144E80B60ull, // DamageParameterTable_GetDamageParameter
            0x149F1A710ull, // EquipDevCtrl_GetBaseDevelopId
            0x141679e90ull, // MenuDevelopGrid_FillGrid
            0x141679810ull, // MenuDevelopGrid_CopyGrid
            0x14af88160ull, // MenuDevelopGrid_CountBadge
            0x14167a430ull, // MenuDevelopGrid_FillFlat
            0x14af8ea80ull, // MenuDevelopGrid_CopyFlat
            0x141282ad0ull, // SightManager_UpdateMissileLockOn
            0x140d2f980ull, // Bullet3_DoSimulation
            0x14939a090ull, // Bullet3_ActivateBulletAtEmptyWork
            0x14a506720ull, // SightManager_Update
            0x14a507ed0ull, // SightManager_UpdateMissileLockOnUi
            0x147926060ull, // LockOnReticleFactory_CreateWindow
            0x140A3B0B0ull, // EquipParams_GetAttackIdByEquipId
            0x140dc36c0ull, // EquipSystem_SetUpGunInfoFromGunPartsDesc
            0x141dab8e0ull, // UiWindowFunction_FindWindow
            0x141dabc20ull, // UiWindowFunction_PostShowAndStartMessage
            0x141dab960ull, // UiWindowFunction_GetLayout
            0x1410419a0ull, // AttackAction_Fire
            0x141303b80ull, // EquipObject_DoFire
            0x141157570ull, // CorePlugin_UpdateLoadoutRequest
            0x140dc0200ull, // WeaponSystem_DefineWeaponFireSound
            0x142C93480ull, // EquipSystem_ChimeraPartsSetWork
            0x140EFC180ull, // MbDvcUpdateTrackListCallFuncs
            0x140EF77F0ull, // MbDvcRefreshTrackListPrefabParameter
            0x140EF70A0ull, // MbDvcTrackListRecordRefresh
            0x140F31F50ull, // MbDvcMissionListCallbackImpl_ActivatePrefabPopupParameter
            0x149D5F450ull, // MbDvcMissionListCallbackImpl_SetMenuHelp
            0x1480F7B50ull, // Equip_ReloadChimeraPartsInfoTable
            0x142C207C0ull, // Equip_ChimeraPartsPackageInfos
            0x141A75850ull, // Mtar_GetAnimFile
            0x141A76420ull, // Mtar_GetDataInfo
            0x142A6B400ull, // Equip_MotionEntryTable
            0x142A71328ull, // Equip_MotionMtarResolver
            0x1422E24F0ull, // SimplePartsControllerImpl_Vtable
            0x140ADCE80ull, // SimplePartsControllerImpl_SetMotionData
            0x140ADCF10ull, // SimplePartsControllerImpl_SetMotionDataByPath
            0x140DBFB50ull, // EquipSystemImpl_HideMagazine
            0x140DBF050ull, // EquipSystemImpl_GetPartsController
            0x140BFEF80ull, // Fox_GetQuarkSystemTable
            0x140071160ull, // Fox_ArrayBaseExtend
            0x1420BCAD8ull, // Fox_ArrayOperatorVtbl
            0x141AB7260ull, // Animx_GetControlSize
            0x141AB6C80ull, // Animx_SimpleControlCtorPool
            0x141AB7000ull, // Animx_SimpleControlCtorHeap
            0x141AB6E60ull, // Animx_SimpleControlDtorPool
            0x141AB7070ull, // Animx_SimpleControlDtorHeap
            0x141AB6FF0ull, // Animx_SimpleControlAux
            0x1405D4C10ull, // Equip_MotionEntrySlotHookA
            0x1405D4F70ull, // Equip_MotionEntrySlotHookB
            0x140A29780ull, // EquipIdTable_GetEquipTypeId
            0x1414404B0ull, // Soldier2InterrogateUtil_UpdateInterrogation
            0x141441E90ull, // Soldier2InterrogateUtil_UpdateInterrogationMarker
            0x1484DA340ull, // SoundControllerImpl_CallVoice
            0x149CD3550ull, // MbDvcCassetteTapeCallbackImpl_SeekMusic
            0x14961D670ull, // EquipSystem_GetGunInfoById
            0x147C34E20ull, // Reticle_InitHandGunAsset
            0x14A630E60ull, // TimeCigaretteActionPluginImpl_HideTimeCigaretteUi
            0x1431f89d0ull, // Fox_EntityInfoMapTeardown
            0x142AC6D10ull, // WeaponEnhanceLangIdArray
            0x14A0267B0ull, // WeaponEnhance_GetIndexForRegist
            0x140fa0350ull, // WeaponEnhance_GetMessageIdArray
            0x14A026A20ull, // WeaponEnhance_GetLangIdById
            0x14A026BE0ull, // WeaponEnhance_GetLangIdBound
            0x140b3b210ull, // BulletEffectController_CreateEffect
            0x1416A22B0ull, // ItemSelectorCallbackImpl_AddRecord
            0x140AE84A0ull, // LoadPlayerLilChickCapFv2
            0x140AE8540ull, // LoadPlayerLilChickCapFpk
            0x140AE85E0ull, // LoadPlayerChickenCapFv2
            0x140AE8680ull, // LoadPlayerChickenCapFpk
            0ull,           // AdditionalMotionTable_GetMtarPathId TODO JP 15.3
            0x141C2E8F0ull, // ModelVertexColorChange_GetModelTotalVertexNum
            0x1416A3540ull, // ItemSelector_AddDevelopWeaponList
            0ull,           // BlockControllerImpl_UpdateAdditionalMotionBlock TODO JP 15.3
            0ull,           // Fox_Block_GetFileForPathId TODO JP 15.3
            0ull,           // Fox_BlockGroup_GetBlockAtIndex TODO JP 15.3
            0x140DB9710ull, // EquipPreviewSystem_RequestLoadEquip
            0x1416AD6B0ull, // ItemSelector_StartEquipPreviewImpl
            0ull,           // EquipBlockController2_RequestLoad TODO JP 15.3
            0ull,           // EquipBlockController2_Instance TODO JP 15.3
            0ull,           // ItemSelector_UpdateSelect TODO JP 15.3
            0x141682EA0ull, // UiEquipPreviewController_ScrollNext
            0x141682CF0ull, // UiEquipPreviewController_ScrollPrev
            0ull,           // UiEquipPreviewController_StopEquipPreview TODO JP 15.3
            0ull,           // Player2Impl_AddAdditionalMtarAll TODO JP 15.3
            0ull,           // Player2Impl_RemoveAdditionalMtarAll TODO JP 15.3
            0x142349610ull, // MotionLoaderImpl_BarrelTypeTable (Ghidra JP153; =ReceiverTable+0xF0)
            0x14957B690ull, // MotionLoaderImpl_GetBarrelType (Ghidra JP153; sole xref of 142349610)
            0x142349690ull, // MotionLoaderImpl_MagazineTypeTable (Ghidra JP153; =ReceiverTable+0x170)
            0x14957C9A0ull, // MotionLoaderImpl_GetMagazineType (Ghidra JP153; sole xref of 142349690)
            0x142349750ull, // MotionLoaderImpl_SightTypeTable (Ghidra JP153; =ReceiverTable+0x230)
            0x14957D970ull, // MotionLoaderImpl_GetSightType (Ghidra JP153; sole xref of 142349750)
            0x14120B135ull, // TppPickable_ItemWindowBoundSite (JP15.3; CMP EBX,0x209, JG -> +0xA9)
            0x140914220ull, // UiUtility_GetWeaponItemNameLangId (JP15.3; 3 call sites converge)
        };

        return value;
    }
}
