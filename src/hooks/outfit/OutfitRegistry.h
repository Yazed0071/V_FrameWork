#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

namespace outfit
{
    constexpr std::uint8_t kCustomPartsTypeStart = 0x20;
    constexpr std::uint8_t kCustomPartsTypeEnd   = 0xFE;
    constexpr std::uint8_t kCustomSelectorStart  = 0x75;
    constexpr std::uint8_t kCustomSelectorEnd    = 0xFE;

    constexpr std::uint8_t kVanillaEventCamoStart = 0x83;
    constexpr std::uint8_t kVanillaEventCamoEnd   = 0x88;


    constexpr std::uint64_t kSubAssetDisabled   = 0;
    constexpr std::uint64_t kSubAssetUseVanilla = 1;


    constexpr std::uint8_t kPlayerType_Snake     = 0;
    constexpr std::uint8_t kPlayerType_DDMale    = 1;
    constexpr std::uint8_t kPlayerType_DDFemale  = 2;
    constexpr std::uint8_t kPlayerType_Avatar    = 3;
    constexpr std::uint8_t kPlayerTypeMax        = 4;


    constexpr std::size_t  kCamoMaterialCount       = 82;
    constexpr std::uint8_t kVanillaCamoTypeMax      = 116;
    constexpr std::uint8_t kCamoVirtualIdStart      = kVanillaCamoTypeMax + 1;
    constexpr std::uint8_t kCamoVirtualIdEnd        = 254;
    constexpr std::uint8_t kCamoBonusTypeUnset      = 0xFF;
    constexpr std::uint32_t kPanelRowMax            = 0x3FF;


    constexpr std::size_t  kMaxHeadOptionsPerOutfit = 120;


    constexpr std::size_t  kMaxVariantsPerOutfit    = 255;
    constexpr std::size_t  kNativeVariantCells      = 15;
    constexpr std::uint16_t kHeadOption_None        = 0x400;
    constexpr std::uint16_t kHeadOption_Balaclava   = 0x210;


    struct OutfitVariant
    {
        bool           used               = false;
        std::uint64_t  partsPathCode64    = 0;
        std::uint64_t  fpkPathCode64      = 0;
        std::uint64_t  camoFpk            = kSubAssetUseVanilla;
        std::uint64_t  camoFv2            = kSubAssetDisabled;
        std::uint64_t  diamondFpk         = kSubAssetDisabled;
        std::uint64_t  diamondFv2         = kSubAssetDisabled;
        std::uint64_t  voiceFpk           = kSubAssetUseVanilla;
        std::uint64_t  displayNameHash    = 0;
        std::uint64_t  iconPathHash       = 0;
        bool           hasEnableArm        = false;
        bool           enableArm           = true;
        bool           hasEnableHead       = false;
        bool           enableHead          = false;
        std::uint16_t  headOptionEquipIds[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t   headOptionCount     = 0;
        bool           headOptionsDeclared = false;
        std::uint64_t  pendingHeadNameHashes[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t   pendingHeadCount    = 0;
    };
    struct OutfitPlayerTypeData
    {
        bool           used                = false;
        std::uint64_t  partsPathCode64     = 0;
        std::uint64_t  fpkPathCode64       = 0;
        std::uint64_t  camoFpk             = kSubAssetDisabled;
        std::uint64_t  faceFpk             = kSubAssetUseVanilla;
        std::uint64_t  skinFv2             = kSubAssetUseVanilla;
        std::uint64_t  diamondFpk          = kSubAssetDisabled;
        std::uint64_t  voiceFpk            = kSubAssetUseVanilla;
        std::uint64_t  camoFv2             = kSubAssetDisabled;
        std::uint64_t  diamondFv2          = kSubAssetDisabled;
        std::uint64_t  baseDisplayNameHash = 0;
        std::uint64_t  baseIconPathHash    = 0;
        std::deque<OutfitVariant> variants;
        std::uint8_t   variantCount                    = 0;
        std::uint8_t   defaultVariant                  = 0;
        bool           enableArm              = true;
        bool           enableHead             = false;
        std::uint16_t  headOptionEquipIds[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t   headOptionCount                              = 0;
        bool           supportsHeadOptions                          = false;
        std::uint64_t  pendingHeadNameHashes[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t   pendingHeadCount                             = 0;
        std::uint8_t   camoBonusType                              = kCamoBonusTypeUnset;
        std::uint8_t   camoBonusTypeDeclared                      = kCamoBonusTypeUnset;
        std::int32_t   camoBonusValues[kCamoMaterialCount]        = {};
        bool           hasCamoBonusValues                         = false;

        const OutfitVariant* Var(std::size_t i) const
        {
            return i < variants.size() ? &variants[i] : nullptr;
        }
        OutfitVariant& EnsureVar(std::size_t i)
        {
            while (variants.size() <= i) variants.emplace_back();
            return variants[i];
        }
    };
    struct OutfitDefinition
    {
        const char*    key             = nullptr;

        std::uint16_t  developId       = 0;
        std::uint16_t  flowIndex       = 0;


        std::uint8_t   partsTypeHint    = 0xFF;
        std::uint8_t   selectorCodeHint = 0xFF;

        std::uint8_t   variantSelectorHints[kMaxVariantsPerOutfit] = {};


        OutfitPlayerTypeData perPlayerType[kPlayerTypeMax] = {};
    };


    struct OutfitEntry
    {
        bool           used              = false;
        bool           bound             = false;

        std::uint16_t  developId         = 0;
        std::uint16_t  flowIndex         = 0;

        std::uint8_t   partsType         = 0;
        std::uint8_t   selectorCode      = 0;

        char           key[64]           = {};
        std::uint64_t  keyHash           = 0;
        std::uint64_t  lastBindTouch     = 0;
        std::uint8_t   partsTypeHint     = 0xFF;
        std::uint8_t   selectorCodeHint  = 0xFF;
        std::uint8_t   variantSelectorHints[kMaxVariantsPerOutfit] = {};

        OutfitPlayerTypeData perPlayerType[kPlayerTypeMax] = {};

        std::uint8_t   variantSelectorCodes[kMaxVariantsPerOutfit] = {};
        std::uint8_t   variantCount                                 = 0;
        std::uint8_t   defaultVariant                               = 0;
        std::uint64_t  displaySummaryNameHash                       = 0;
        std::uint64_t  displaySummaryIconHash                       = 0;
        const OutfitPlayerTypeData* GetPTData(std::uint8_t playerType) const;
        bool IsPlayerTypeSupported(std::uint8_t playerType) const;
        std::uint8_t FirstSupportedPlayerType() const;


        bool HasVariants()       const { return variantCount > 0; }
        bool IsArmEnabled(std::uint8_t playerType)        const;
        bool IsHeadEnabled(std::uint8_t playerType)       const;
        bool IsCamoCustom(std::uint8_t playerType)        const;
        bool IsCamoFv2Custom(std::uint8_t playerType)     const;
        bool IsFaceEnabled(std::uint8_t playerType)       const;
        bool IsDiamondEnabled(std::uint8_t playerType)    const;
        bool IsDiamondCustom(std::uint8_t playerType)     const;
        bool IsDiamondFv2Custom(std::uint8_t playerType)  const;
        bool IsVoiceCustom(std::uint8_t playerType)       const;
        bool HasHeadOptions(std::uint8_t playerType)      const;
        bool HasHeadOption(std::uint16_t equipId, std::uint8_t playerType) const;
        bool HasAnyHeadOptions() const;

        bool GetHeadOptionsFor(std::uint8_t playerType,
                               const std::uint16_t** outEquipIds,
                               std::uint8_t* outCount) const;

        bool GetHeadOptionsForVariant(std::uint8_t playerType, std::uint8_t variant,
                                      const std::uint16_t** outEquipIds,
                                      std::uint8_t* outCount) const;
        bool HasHeadOptionsForVariant(std::uint8_t playerType,
                                      std::uint8_t variant) const;
        bool HasHeadOptionForVariant(std::uint16_t equipId, std::uint8_t playerType,
                                     std::uint8_t variant) const;

        bool HasHeadOptionAnyVariant(std::uint16_t equipId,
                                     std::uint8_t playerType) const;


        std::uint64_t GetVariantPartsPath(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantFpkPath(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantCamoFpk(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantCamoFv2(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantDiamondFpk(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantDiamondFv2(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantVoiceFpk(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantDisplayNameHash(std::uint8_t playerType, std::uint8_t variantIdx) const;
        std::uint64_t GetVariantIconPathHash(std::uint8_t playerType, std::uint8_t variantIdx) const;


        std::uint8_t  GetVariantCountFor(std::uint8_t playerType) const;

        std::uint8_t  GetVariantSelectorCode(std::uint8_t variantIdx) const;


        std::uint8_t  GetCamoBonusType(std::uint8_t playerType) const;
        bool          HasCamoBonusValuesFor(std::uint8_t playerType) const;
        const std::int32_t* GetCamoBonusValuesFor(std::uint8_t playerType) const;
    };


    bool RegisterOutfit(const OutfitDefinition& def,
                        std::uint8_t* outAllocatedPartsType);

    void ClearAllOutfits();


    bool BindOutfit(std::uint16_t developId, bool allowEvict, const char* reason);
    bool UnbindOutfit(std::uint16_t developId);
    bool IsOutfitBound(std::uint16_t developId);

    bool CollectOutfitResidencyPins(
        std::unordered_set<std::uint32_t>& allDevelopIds,
        std::unordered_set<std::uint32_t>& livePinnedDevelopIds);

    void NoteOutfitApplied(std::uint16_t developId);
    void NoteOutfitOrdered(std::uint16_t developId);
    void NoteOutfitMenuStamp(std::uint16_t developId);
    void ClearOutfitMenuStamps();

    void NoteOutfitRowRefresh(std::uint16_t developId);
    bool BindOutfitForVisibleRow(std::uint16_t developId);

    bool TryGetOutfitByPartsType(std::uint8_t partsType,
                                 const OutfitEntry** outEntry);

    bool ReleaseOutfitLiveBytesIfIdle(std::uint16_t developId);

    bool TryGetOutfitBySelectorCode(std::uint8_t selectorCode,
                                    const OutfitEntry** outEntry);


    bool TryGetOutfitByVariantSelector(std::uint8_t selectorCode,
                                       const OutfitEntry** outEntry,
                                       std::uint8_t* outVariantIndex);


    std::uint64_t GetVariantDisplayNameHash(std::uint8_t partsType,
                                            std::uint8_t playerType,
                                            std::uint8_t variantIndex);

    bool TryGetOutfitByFlowIndex(std::uint16_t flowIndex,
                                 const OutfitEntry** outEntry);

    bool TryGetOutfitByDevelopId(std::uint16_t developId,
                                 const OutfitEntry** outEntry);


    bool SetOutfitFlowIndexByDevelopId(std::uint16_t developId,
                                       std::uint16_t flowIndex);

    bool SetOutfitSummaryDisplay(std::uint16_t developId,
                                 std::uint64_t nameHash,
                                 std::uint64_t iconHash);

    bool TryGetOutfitByFlowIndexForPlayerType(
            std::uint16_t flowIndex,
            std::uint8_t  playerType,
            const OutfitEntry** outEntry);

    bool TryGetOutfitByDevelopIdForPlayerType(
            std::uint16_t developId,
            std::uint8_t  playerType,
            const OutfitEntry** outEntry);


    bool TryGetOutfitByCamoVirtualId(std::uint8_t virtualId,
                                     const OutfitEntry** outEntry,
                                     std::uint8_t* outPlayerType);


    bool IsSnakeAvatarBridge(std::uint8_t a, std::uint8_t b);


    std::size_t GetAllOutfits(const OutfitEntry** outEntries,
                              std::size_t maxEntries);

    int ResolvePendingHeadName(std::uint64_t nameHash, std::uint16_t equipId);


    constexpr std::uint8_t kHeadOptionAnyCamo = 0xFF;

    struct VanillaSuitHeadExt
    {
        std::uint16_t headOptionEquipIds[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t  headOptionSourceCamo[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t  headOptionCount = 0;
        std::uint64_t pendingHeadNameHashes[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t  pendingHeadSourceCamo[kMaxHeadOptionsPerOutfit] = {};
        std::uint8_t  pendingHeadCount = 0;
        bool          declared = false;
    };

    struct VanillaSuitVariantAsset
    {
        bool          used            = false;
        std::uint64_t partsPathCode64 = 0;
        std::uint64_t fpkPathCode64   = 0;
        std::uint64_t camoFpk         = kSubAssetUseVanilla;
        std::uint64_t camoFv2         = kSubAssetUseVanilla;
        std::uint64_t diamondFpk      = kSubAssetUseVanilla;
        std::uint64_t diamondFv2      = kSubAssetUseVanilla;
        std::uint64_t voiceFpk        = kSubAssetUseVanilla;
        std::uint64_t displayNameHash = 0;
    };

    bool ExtendVanillaSuitVariants(std::uint8_t vanillaPartsType,
                                   std::uint8_t playerType,
                                   std::uint8_t sourceCamo,
                                   const VanillaSuitVariantAsset* variants,
                                   std::uint8_t count);

    std::uint8_t VanillaExtVariantCount(std::uint8_t vanillaPartsType,
                                        std::uint8_t playerType);

    std::uint8_t VanillaExtVariantSlotCount(std::uint8_t vanillaPartsType);

    const VanillaSuitVariantAsset* VanillaExtGetVariant(
        std::uint8_t vanillaPartsType, std::uint8_t playerType,
        std::uint8_t variantIdx);

    const VanillaSuitVariantAsset* VanillaExtGetVariantBridged(
        std::uint8_t vanillaPartsType, std::uint8_t playerType,
        std::uint8_t variantIdx);

    std::uint8_t VanillaExtResolveVariantDonor(std::uint8_t vanillaPartsType,
                                               std::uint8_t playerType);

    std::uint8_t VanillaExtCollectSelectorSeeds(std::uint8_t* outSelectors,
                                                std::uint8_t* outSourceCamos,
                                                std::uint8_t maxCount);

    std::uint8_t VanillaExtGetVariantSelector(std::uint8_t vanillaPartsType,
                                              std::uint8_t variantIdx);

    std::uint8_t VanillaExtGetVariantSourceCamo(std::uint8_t vanillaPartsType,
                                                std::uint8_t variantIdx);

    void ResetAllVanillaExtVariants(std::uint8_t exceptPartsType = 0xFF);

    bool TryGetVanillaExtByVariantSelector(std::uint8_t selector,
                                           std::uint8_t* outPartsType,
                                           std::uint8_t* outVariantIdx);

    bool ExtendVanillaSuitVoice(std::uint8_t vanillaPartsType,
                                std::uint8_t playerType,
                                std::uint8_t sourceCamo,
                                std::uint64_t voiceFpk);

    std::uint64_t VanillaExtGetSuitVoiceFpk(std::uint8_t vanillaPartsType,
                                            std::uint8_t playerType,
                                            std::uint8_t wornCamo);

    bool ExtendVanillaSuitArmHead(std::uint8_t vanillaPartsType,
                                  std::uint8_t playerType,
                                  std::uint8_t sourceCamo,
                                  bool hasArm, bool armVal,
                                  bool hasHead, bool headVal);

    bool VanillaExtGetSuitArm(std::uint8_t vanillaPartsType,
                              std::uint8_t playerType,
                              std::uint8_t wornCamo, bool* outEnable);

    bool VanillaExtGetSuitHead(std::uint8_t vanillaPartsType,
                               std::uint8_t playerType,
                               std::uint8_t wornCamo, bool* outEnable);

    bool ExtendVanillaSuitHeadOptions(std::uint8_t vanillaPartsType,
                                      std::uint8_t playerType,
                                      std::uint8_t sourceCamo,
                                      const std::uint16_t* equipIds,
                                      std::uint8_t idCount,
                                      const std::uint64_t* pendingHashes,
                                      std::uint8_t pendingCount);

    bool VanillaExtHasAnyHeadOptions(std::uint8_t vanillaPartsType,
                                     std::uint8_t playerType,
                                     std::uint8_t wornCamo);
    bool VanillaExtHasHeadOption(std::uint8_t vanillaPartsType,
                                 std::uint16_t equipId,
                                 std::uint8_t playerType,
                                 std::uint8_t wornCamo);
    bool VanillaExtGetHeadOptions(std::uint8_t vanillaPartsType,
                                  std::uint8_t playerType,
                                  std::uint8_t wornCamo,
                                  std::uint16_t* outEquipIds,
                                  std::uint8_t maxEquipIds,
                                  std::uint8_t* outCount);

    std::uint8_t ResolveVanillaPartsTypeForCamo(std::uint8_t camoType);


    std::uint8_t ReadLivePartsType();
    std::uint8_t ReadLiveSelectorCode();
    std::uint8_t ReadLivePlayerType();

    bool BootRestoreScrubActive();
    void EndBootRestoreScrub(const char* reason);


    bool WriteLivePlayerOutfit(std::uint8_t partsType,
                               std::uint8_t selectorCode,
                               std::uint8_t playerType);

    void RememberPlayerTypeOutfit(std::uint8_t playerType,
                                  std::uint8_t partsType, std::uint8_t selector);
    bool GetRememberedPlayerTypeOutfit(std::uint8_t playerType,
                                       std::uint8_t* outPartsType,
                                       std::uint8_t* outSelector);


    bool WriteLiveHeadSlot(std::uint8_t headSlot);

    bool          WriteLiveWornHeadCategory(std::uint16_t category);
    std::uint16_t ReadLiveWornHeadCategory();
    std::uint8_t  ReadLiveHeadSlot();


    void          SetActiveVariant(std::uint8_t partsType, std::uint8_t variantIndex);
    std::uint8_t  GetActiveVariant(std::uint8_t partsType);
    std::uint8_t  GetVariantWindowStart(std::uint16_t developId);
    void          SetVariantWindowStart(std::uint16_t developId, std::uint8_t start);
    void          ClearActiveVariant(std::uint8_t partsType);


    void          SetPendingOutfitDevelopId(std::uint16_t developId);
    std::uint16_t GetPendingOutfitDevelopId();
    void          ClearPendingOutfitDevelopId();


    void          SetPendingHeadOptionEquipId(std::uint16_t equipId);
    std::uint16_t GetPendingHeadOptionEquipId();
    void          ClearPendingHeadOptionEquipId();

    void          SetWornCustomHeadSlot(std::uint8_t slot);
    std::uint8_t  GetWornCustomHeadSlot();
    void          ClearWornCustomHeadSlot();


    void  SetSupplyDropClickLatch();
    bool  ConsumeSupplyDropClickLatch();


    void          SetPendingSupplyDropDevelopId(std::uint16_t developId);
    std::uint16_t ConsumePendingSupplyDropDevelopId();


    std::uint16_t PeekPendingSupplyDropDevelopId();


    void          SetPendingSupplyDropVariantIdx(std::uint8_t variantIndex);
    std::uint8_t  ConsumePendingSupplyDropVariantIdx();
    std::uint8_t  PeekPendingSupplyDropVariantIdx();

    void          SetCrateDeliveredVariant(std::uint16_t developId,
                                           std::uint8_t variantIndex);
    std::uint16_t PeekCrateDeliveredDevelopId();
    std::uint8_t  PeekCrateDeliveredVariantIdx();
    void          ClearCrateDeliveredVariant();
}
