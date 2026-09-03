#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace V_FrameWorkState
{


    void Load();

    void NoteInstallOutcome(bool allInstalled);


    void Save();

    void FlushPendingSaves();
    void SaveOnProcessExit();
    void AbandonFlusherThread();


    void BeginBatch();
    void EndBatch();

    struct SaveBatch
    {
        SaveBatch() { BeginBatch(); }
        ~SaveBatch() { EndBatch(); }
        SaveBatch(const SaveBatch&) = delete;
        SaveBatch& operator=(const SaveBatch&) = delete;
    };


    bool ResolveOrCreateEquipId(
        const char* key,
        std::int32_t minimumId,
        std::int32_t& outEquipId,
        bool isWeapon = false);

    bool IsClaimedEquipId(std::int32_t equipId);

    bool IsDevelopedByFlowRowDevelopId(std::int32_t developId, bool& developed);

    void SetVanillaIdentityEquipIds(const std::int32_t* equipIds,
                                    std::size_t count);

    void NotePinnedEquipId(std::int32_t equipId);
    void NoteStickyPinnedEquipId(std::int32_t equipId);
    void UnpinEquipId(std::int32_t equipId);


    bool ResolveOrCreateDevelopId(
        const char* key,
        std::int32_t minimumId,
        std::int32_t& outDevelopId,
        bool* outCreated = nullptr);

    std::int32_t GetDevelopIdByKey(const char* key);

    constexpr std::uint8_t kRowKindUnknown = 0;
    constexpr std::uint8_t kRowKindWeapon  = 1;
    constexpr std::uint8_t kRowKindOutfit  = 2;

    void SetRowKind(const char* key, std::uint8_t kind);

    std::int32_t GetDevelopIdAtOldFlowIndex(std::int32_t oldFlowIndex);

    std::int32_t GetFlowIndexByDevelopId(std::int32_t developId);


    std::vector<std::int32_t> TakePendingDevelopedResets();

    bool ResolveDevelopedFlag(const char* key, bool defaultDeveloped);
    bool IsManagedDevelopId(std::int32_t developId);
    void SetDevelopedByDevelopId(std::int32_t developId, bool developed);
    bool GetDevelopedByDevelopId(std::int32_t developId);
    bool IsExplicitlyUndevelopedByDevelopId(std::int32_t developId);

    void ForEachManagedDevelop(
        const std::function<void(std::int32_t developId, bool developed, bool isNew)>& callback);

    void SetNewByDevelopId(std::int32_t developId, bool isNew);
    bool GetNewByDevelopId(std::int32_t developId);

    void ForEachManagedDevelopRow(
        const std::function<void(std::int32_t developId, std::int32_t flowIndex,
                                 bool reqAnnounced)>& callback);

    bool GetDevReqAnnouncedByDevelopId(std::int32_t developId);
    void SetDevReqAnnouncedByDevelopId(std::int32_t developId, bool announced);


    bool ResolveOrCreateFlowIndex(
        const char* key,
        std::int32_t minimumIndex,
        std::int32_t& outFlowIndex);

    void SetSessionFlowIndex(const char* key, std::int32_t flowIndex);
    std::int32_t GetPersistedFlowIndex(const char* key);

    void ReleaseSessionFlowIndex(const char* key);


    bool ResolveOrCreateTapeSaveIndex(
        const char* key,
        std::int16_t minimumIndex,
        std::int16_t& outSaveIndex);

    bool ResolveOrCreateConstantValue(
        const char* spaceTag,
        const char* name,
        std::int32_t minimumValue,
        std::int32_t& outValue);

    std::int32_t GetPersistedConstant(const char* spaceTag, const char* name);
    void         SetPersistedConstant(const char* spaceTag, const char* name,
                                      std::int32_t value);
    void         ForEachPersistedConstant(const char* spaceTag,
                                          void (*fn)(const char* name,
                                                     std::int32_t value));

    std::uint8_t GetPersistedOutfitPartsType(const char* key);
    std::uint8_t GetPersistedOutfitSelector(const char* key);
    void         SetPersistedOutfitIds(const char* key,
                                       std::uint8_t partsType,
                                       std::uint8_t selector);

    constexpr std::size_t kPersistedVariantSelectorSlots = 254;

    std::size_t GetPersistedOutfitVariantSelectors(const char* key,
                                                   std::uint8_t* out,
                                                   std::size_t cap);
    void        SetPersistedOutfitVariantSelectors(const char* key,
                                                   const std::uint8_t* selectors,
                                                   std::size_t count);
    void        ClearPersistedOutfitIds(const char* key);

    void ForEachPersistedOutfit(
        const std::function<void(const std::string& key,
                                 std::uint8_t partsType,
                                 std::uint8_t selector,
                                 const std::uint8_t* variants)>& callback);


    bool ResolveOrCreateBluePrintId(const char* key, std::int32_t& outId);
    std::int32_t GetBluePrintId(const char* key);
    void ForEachBluePrint(void (*fn)(const char* key, std::int32_t id, bool owned));
    void SetBluePrintOwned(const char* key, bool owned);
    bool GetBluePrintOwned(const char* key);
    void SetBluePrintNew(const char* key, bool isNew);
    bool GetBluePrintNew(const char* key);

    void SetTapeOwned(const char* key, bool owned);
    void SetTapeNew(const char* key, bool isNew);


    void SetTapeOwnedBySaveIndex(std::int16_t saveIndex, bool owned);
    void SetTapeNewBySaveIndex(std::int16_t saveIndex, bool isNew);


    bool GetTapeOwned(const char* key);
    bool GetTapeNew(const char* key);


    void ForEachTape(
        const std::function<void(const std::string& key,
                                 std::int16_t saveIndex,
                                 bool owned,
                                 bool isNew)>& callback);


    void Reset();
}
