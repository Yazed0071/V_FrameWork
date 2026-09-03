#include "pch.h"

#include "CustomHeadRegistry.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../equip/DevelopArrayGrow.h"

#include <HookUtils.h>

#include "log.h"
#include "AddressSet.h"
#include "../../core/V_FrameWorkState.h"
#include "EquipDevelopControllerImpl_GetSuitDevelopInfoIndex.h"
#include "../equip/EquipDevelop_AddToEquipDevelopTable.h"
#include "../equip/MenuPerf.h"
#include "OutfitRegistry.h"
#include "../../core/FoxHashes.h"

namespace outfit
{
    namespace
    {
        std::array<CustomHeadEntry, kMaxCustomHeads> g_Heads{};
        std::mutex                                    g_Mutex;


        struct PendingHead
        {
            char           name[64]                    = { 0 };
            std::uint16_t  faceIds[kPlayerTypeMax]     = {};
            std::uint64_t  faceFv2Code[kPlayerTypeMax] = {};
            std::uint64_t  faceFpkCode[kPlayerTypeMax] = {};
            bool           showInDevelopMenu           = false;
        };
        std::vector<PendingHead>  g_PendingHeads;
        std::atomic<bool>         g_HasPendingHeads{ false };
        std::atomic<bool>         g_HasProvisionalHeads{ false };

        constexpr const char* kHeadPersistSpace = "CUSTOMHEAD";

        std::int32_t ReadPersistedHeadEquipId(const char* name)
        {
            const std::int32_t v =
                V_FrameWorkState::GetPersistedConstant(kHeadPersistSpace, name);
            return (v > 0) ? (v & 0xFFFF) : 0;
        }

        std::uint8_t ReadPersistedHeadSlot(const char* name)
        {
            const std::int32_t v =
                V_FrameWorkState::GetPersistedConstant(kHeadPersistSpace, name);
            return (v > 0) ? static_cast<std::uint8_t>((v >> 16) & 0xFF)
                           : std::uint8_t{0};
        }

        void PersistHeadRegistration(const CustomHeadEntry& e)
        {
            V_FrameWorkState::SetPersistedConstant(
                kHeadPersistSpace, e.name,
                static_cast<std::int32_t>(
                    (static_cast<std::uint32_t>(e.slotByte) << 16) | e.equipId));
        }


        struct SnakeFaceStages
        {
            std::uint64_t fv2[kSnakeFaceStageCount] = {};
            std::uint64_t fpk[kSnakeFaceStageCount] = {};
        };
        std::unordered_map<std::string, SnakeFaceStages> g_SnakeStages;
        std::unordered_set<std::string> g_InfiniteAmmoHeads;

        std::string SnakeStageKey(const char* name)
        {
            std::string key(name);
            if (key.size() > 63)
                key.resize(63);
            return key;
        }

        const SnakeFaceStages* FindSnakeStagesUnlocked(const char* name)
        {
            if (!name || !name[0]) return nullptr;
            const auto it = g_SnakeStages.find(SnakeStageKey(name));
            return it != g_SnakeStages.end() ? &it->second : nullptr;
        }

        using GetQuarkSystemTable_t = void* (__fastcall*)();
        static GetQuarkSystemTable_t g_GetQuarkSystemTable = nullptr;

        std::int32_t FindByNameUnlocked(const char* name)
        {
            for (std::size_t i = 0; i < g_Heads.size(); ++i)
            {
                if (g_Heads[i].used && std::strcmp(g_Heads[i].name, name) == 0)
                    return static_cast<std::int32_t>(i);
            }
            return -1;
        }

        std::int32_t AllocateUnlocked()
        {
            for (std::size_t i = 0; i < g_Heads.size(); ++i)
            {
                if (!g_Heads[i].used)
                    return static_cast<std::int32_t>(i);
            }
            return -1;
        }

        bool IsSlotTakenUnlocked(std::uint8_t slotByte)
        {
            for (const CustomHeadEntry& e : g_Heads)
            {
                if (e.used && e.slotByte == slotByte)
                    return true;
            }
            return false;
        }

        std::uint8_t AllocateSlotUnlocked()
        {
            for (std::uint32_t s = kCustomHeadSlotBase; s < 0x100u; ++s)
            {
                if (!IsSlotTakenUnlocked(static_cast<std::uint8_t>(s)))
                    return static_cast<std::uint8_t>(s);
            }
            return 0;
        }


        static void* WalkToEquipDevelopController()
        {
            if (!g_GetQuarkSystemTable)
            {
                g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
                    ResolveGameAddress(gAddr.GetQuarkSystemTable));
                if (!g_GetQuarkSystemTable) return nullptr;
            }

            __try
            {
                auto* qst = reinterpret_cast<std::uint8_t*>(g_GetQuarkSystemTable());
                if (!qst) return nullptr;
                auto* app = *reinterpret_cast<std::uint8_t**>(qst + 0x98);
                if (!app) return nullptr;
                auto* base = *reinterpret_cast<std::uint8_t**>(app + 0x110);
                if (!base) return nullptr;
                return *reinterpret_cast<void**>(base + 0xac8);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }


        static std::int32_t TranslateDevelopIdToRowIndex(std::uint32_t developId)
        {
            void* edc = WalkToEquipDevelopController();
            if (!edc) return -1;

            __try
            {
                using GetIdx_t = std::uint16_t (__fastcall*)(void*, std::uint16_t);
                void** vt = *reinterpret_cast<void***>(edc);
                if (!vt) return -1;
                auto getIdx = reinterpret_cast<GetIdx_t>(vt[0xE0 / 8]);
                if (!getIdx) return -1;
                const std::uint16_t row = getIdx(edc, static_cast<std::uint16_t>(developId));
                if (row == 0 || row == 0xFFFF) return -1;
                return static_cast<std::int32_t>(row);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }


        static bool IsResolvedRowValidUnlocked(std::int32_t row)
        {
            if (row <= 0 || row >= 0xFFFF) return false;
            if (row == static_cast<std::int32_t>(kHeadOption_None)) return false;
            for (const CustomHeadEntry& e : g_Heads)
                if (e.used && e.equipId == static_cast<std::uint16_t>(row))
                    return false;
            return true;
        }

        static std::uint16_t CompleteHeadRegistrationUnlocked(
            const char* name, const std::uint16_t* faceIds,
            const std::uint64_t* faceFv2Codes, const std::uint64_t* faceFpkCodes,
            std::uint16_t developId, std::uint16_t rowIndex,
            bool showInDevelopMenu, bool provisional)
        {
            const std::int32_t idx = AllocateUnlocked();
            if (idx < 0)
            {
                LogDebug("[CustomHead] complete: registry full (max=%zu, name=%s)\n",
                    kMaxCustomHeads, name);
                return 0;
            }

            std::uint8_t slotByte = ReadPersistedHeadSlot(name);
            if (slotByte < kCustomHeadSlotBase || IsSlotTakenUnlocked(slotByte))
                slotByte = AllocateSlotUnlocked();
            if (slotByte < kCustomHeadSlotBase)
            {
                LogDebug("[CustomHead] complete: head slot space full (name=%s)\n", name);
                return 0;
            }

            CustomHeadEntry& e = g_Heads[idx];
            e.used            = true;
            e.provisional     = provisional;
            e.hiddenInDevelop = !showInDevelopMenu;
            e.equipId      = rowIndex;
            e.developId    = developId;
            e.flowIndex    = 0;
            e.slotByte     = slotByte;
            for (std::size_t i = 0; i < kPlayerTypeMax; ++i)
                e.TppEnemyFaceId[i] = faceIds[i];
            for (std::size_t i = 0; i < kPlayerTypeMax; ++i)
            {
                e.faceFv2Code[i] = faceFv2Codes ? faceFv2Codes[i] : 0;
                e.faceFpkCode[i] = faceFpkCodes ? faceFpkCodes[i] : 0;
            }

            const std::size_t nameLen = std::strlen(name);
            const std::size_t copyLen = nameLen < (sizeof(e.name) - 1)
                ? nameLen : (sizeof(e.name) - 1);
            std::memcpy(e.name, name, copyLen);
            e.name[copyLen] = '\0';

            LogDebug("[CustomHead] registered '%s' equipId=%u (rowIndex=0x%X) "
                "developId=%u slot=0x%02X%s%s\n",
                e.name, static_cast<unsigned>(e.equipId),
                static_cast<unsigned>(e.equipId),
                static_cast<unsigned>(e.developId),
                static_cast<unsigned>(e.slotByte),
                provisional ? " [PROVISIONAL from persisted ids - verified "
                              "against the develop table at the next menu build]"
                            : "",
                showInDevelopMenu ? "" : " (hidden from R&D)");

            if (!showInDevelopMenu)
            {
                SetDevelopHidden(e.equipId);
                equip::SilenceDevelopRowAnnounce(
                    static_cast<std::int32_t>(e.developId));
            }

            if (provisional)
                g_HasProvisionalHeads.store(true, std::memory_order_release);

            PersistHeadRegistration(e);
            ResolvePendingHeadName(FoxHashes::StrCode64(e.name), e.equipId);
            return e.equipId;
        }

        static void StorePendingHeadUnlocked(
            const char* name, const std::uint16_t* faceIds,
            const std::uint64_t* faceFv2Codes, const std::uint64_t* faceFpkCodes,
            bool showInDevelopMenu)
        {
            for (PendingHead& p : g_PendingHeads)
            {
                if (std::strcmp(p.name, name) == 0)
                {
                    for (std::size_t i = 0; i < kPlayerTypeMax; ++i)
                    {
                        p.faceIds[i]     = faceIds[i];
                        p.faceFv2Code[i] = faceFv2Codes ? faceFv2Codes[i] : 0;
                        p.faceFpkCode[i] = faceFpkCodes ? faceFpkCodes[i] : 0;
                    }
                    p.showInDevelopMenu = showInDevelopMenu;
                    return;
                }
            }
            PendingHead p{};
            const std::size_t nameLen = std::strlen(name);
            const std::size_t copyLen = nameLen < (sizeof(p.name) - 1)
                ? nameLen : (sizeof(p.name) - 1);
            std::memcpy(p.name, name, copyLen);
            p.name[copyLen] = '\0';
            for (std::size_t i = 0; i < kPlayerTypeMax; ++i)
            {
                p.faceIds[i]     = faceIds[i];
                p.faceFv2Code[i] = faceFv2Codes ? faceFv2Codes[i] : 0;
                p.faceFpkCode[i] = faceFpkCodes ? faceFpkCodes[i] : 0;
            }
            p.showInDevelopMenu = showInDevelopMenu;
            g_PendingHeads.push_back(p);
            g_HasPendingHeads.store(true, std::memory_order_release);
        }
    }

    std::uint16_t RegisterHeadOption(
        const char* name,
        const std::uint16_t* TppEnemyFaceIdsPerPt,
        const std::uint64_t* faceFv2CodesPerPt,
        const std::uint64_t* faceFpkCodesPerPt,
        bool showInDevelopMenu)
    {
        static_assert(kPlayerTypeMax >= 4,
                      "head registration needs the vanilla player types");

        if (!name || !name[0])
        {
            LogDebug("[CustomHead] RegisterHeadOption: missing name\n");
            return 0;
        }

        std::uint16_t faceIds[kPlayerTypeMax];
        for (std::size_t i = 0; i < kPlayerTypeMax; ++i)
        {
            const std::uint16_t v = TppEnemyFaceIdsPerPt ? TppEnemyFaceIdsPerPt[i] : 0;
            faceIds[i] = (v != 0) ? v : kDefaultTppEnemyFaceId;
        }

        std::lock_guard<std::mutex> lock(g_Mutex);

        if (const std::int32_t existing = FindByNameUnlocked(name); existing >= 0)
        {
            CustomHeadEntry& e = g_Heads[existing];
            for (std::size_t i = 0; i < kPlayerTypeMax; ++i)
            {
                e.TppEnemyFaceId[i] = faceIds[i];
                e.faceFv2Code[i] = faceFv2CodesPerPt ? faceFv2CodesPerPt[i] : 0;
                e.faceFpkCode[i] = faceFpkCodesPerPt ? faceFpkCodesPerPt[i] : 0;
            }
            return e.equipId;
        }

        std::int32_t developId = 0;
        if (!V_FrameWorkState::ResolveOrCreateDevelopId(name, 0, developId)
            || developId <= 0 || developId > 0xFFFF)
        {
            Log("[CustomHead] RegisterHeadOption: developId allocation "
                "failed (name=%s, raw=%d)\n", name, developId);
            return 0;
        }

        const std::int32_t rowIndex =
            TranslateDevelopIdToRowIndex(static_cast<std::uint32_t>(developId));
        if (IsResolvedRowValidUnlocked(rowIndex))
        {
            return CompleteHeadRegistrationUnlocked(
                name, faceIds, faceFv2CodesPerPt, faceFpkCodesPerPt,
                static_cast<std::uint16_t>(developId),
                static_cast<std::uint16_t>(rowIndex), showInDevelopMenu, false);
        }

        std::int32_t provisionalRow = ReadPersistedHeadEquipId(name);
        const char* provisionalSrc = "persisted head ids";
        if (!IsResolvedRowValidUnlocked(provisionalRow))
        {
            provisionalRow = V_FrameWorkState::GetPersistedFlowIndex(name);
            provisionalSrc = "persisted develop flow index";
        }
        if (IsResolvedRowValidUnlocked(provisionalRow))
        {
            const std::uint16_t equipId = CompleteHeadRegistrationUnlocked(
                name, faceIds, faceFv2CodesPerPt, faceFpkCodesPerPt,
                static_cast<std::uint16_t>(developId),
                static_cast<std::uint16_t>(provisionalRow), showInDevelopMenu,
                true);
            if (equipId != 0)
            {
                LogDebug("[CustomHead] '%s' develop row not committed yet "
                         "(developId=%d) - completed from %s so a boot straight "
                         "into a mission still renders the worn head\n",
                    name, developId, provisionalSrc);
                return equipId;
            }
        }

        StorePendingHeadUnlocked(name, faceIds, faceFv2CodesPerPt,
                                 faceFpkCodesPerPt, showInDevelopMenu);
        LogDebug("[CustomHead] '%s' develop row not committed yet (developId=%d "
                 "getIdx=0x%X) - DEFERRED; resolves when an equip or develop menu "
                 "opens\n",
            name, developId, static_cast<unsigned>(rowIndex));
        return 0;
    }

    void SetCustomHeadSnakeFaceStages(const char* name,
                                      const std::uint64_t* fv2ByStage,
                                      const std::uint64_t* fpkByStage)
    {
        if (!name || !name[0]) return;

        std::lock_guard<std::mutex> lock(g_Mutex);
        SnakeFaceStages& slot = g_SnakeStages[SnakeStageKey(name)];
        for (std::uint8_t i = 0; i < kSnakeFaceStageCount; ++i)
        {
            slot.fv2[i] = fv2ByStage ? fv2ByStage[i] : 0;
            slot.fpk[i] = fpkByStage ? fpkByStage[i] : 0;
        }
        LogDebug("[CustomHead] snake demon-stage fova set for '%s': "
            "fv2=[%016llX %016llX %016llX] fpk=[%016llX %016llX %016llX]\n",
            name,
            static_cast<unsigned long long>(slot.fv2[0]),
            static_cast<unsigned long long>(slot.fv2[1]),
            static_cast<unsigned long long>(slot.fv2[2]),
            static_cast<unsigned long long>(slot.fpk[0]),
            static_cast<unsigned long long>(slot.fpk[1]),
            static_cast<unsigned long long>(slot.fpk[2]));
    }

    std::uint64_t GetCustomHeadSnakeStageFv2(const char* name,
                                             std::uint32_t stage)
    {
        if (stage >= kSnakeFaceStageCount) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const SnakeFaceStages* s = FindSnakeStagesUnlocked(name);
        return s ? s->fv2[stage] : 0;
    }

    std::uint64_t GetCustomHeadSnakeStageFpk(const char* name,
                                             std::uint32_t stage)
    {
        if (stage >= kSnakeFaceStageCount) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const SnakeFaceStages* s = FindSnakeStagesUnlocked(name);
        return s ? s->fpk[stage] : 0;
    }

    void SetCustomHeadInfiniteAmmo(const char* name, bool enable)
    {
        if (!name || !name[0]) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (enable)
        {
            if (g_InfiniteAmmoHeads.insert(SnakeStageKey(name)).second)
                LogDebug("[CustomHead] '%s' grants INFINITE AMMO while worn\n",
                    name);
        }
        else
        {
            g_InfiniteAmmoHeads.erase(SnakeStageKey(name));
        }
    }

    bool GetCustomHeadInfiniteAmmo(const char* name)
    {
        if (!name || !name[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        return g_InfiniteAmmoHeads.count(SnakeStageKey(name)) != 0;
    }

    static std::atomic<DWORD> g_PendingHeadsRetryTick{ 0 };

    bool HasPendingCustomHeads()
    {
        return g_HasPendingHeads.load(std::memory_order_acquire);
    }

    int DrainPendingHeads()
    {
        if (!g_HasPendingHeads.load(std::memory_order_acquire)
            && !g_HasProvisionalHeads.load(std::memory_order_acquire))
            return 0;

        const DWORD lastFail =
            g_PendingHeadsRetryTick.load(std::memory_order_relaxed);
        if (lastFail != 0 && (GetTickCount() - lastFail) < 250)
            return 0;

        equip::MenuPerfScope _perf(equip::kPerf_DrainHeads);
        std::lock_guard<std::mutex> lock(g_Mutex);

        bool provisionalLeft = false;
        for (CustomHeadEntry& e : g_Heads)
        {
            if (!e.used || !e.provisional)
                continue;
            const std::int32_t row =
                TranslateDevelopIdToRowIndex(
                    static_cast<std::uint32_t>(e.developId));
            if (row <= 0 || row >= 0xFFFF
                || row == static_cast<std::int32_t>(kHeadOption_None))
            {
                provisionalLeft = true;
                continue;
            }
            if (row == static_cast<std::int32_t>(e.equipId))
            {
                e.provisional = false;
                continue;
            }
            bool rowTaken = false;
            for (const CustomHeadEntry& o : g_Heads)
            {
                if (&o != &e && o.used
                    && o.equipId == static_cast<std::uint16_t>(row))
                {
                    rowTaken = true;
                    break;
                }
            }
            if (rowTaken)
            {
                provisionalLeft = true;
                Log("[CustomHead] provisional '%s' cannot re-home: develop row %d "
                    "already belongs to another head - retried on the next menu "
                    "build\n", e.name, row);
                continue;
            }
            const std::uint16_t oldId = e.equipId;
            e.equipId     = static_cast<std::uint16_t>(row);
            e.provisional = false;
            if (e.hiddenInDevelop)
            {
                SetDevelopHidden(oldId, false);
                SetDevelopHidden(e.equipId, true);
            }
            PersistHeadRegistration(e);
            const int patched = RemapHeadOptionEquipId(oldId, e.equipId);
            Log("[CustomHead] provisional '%s' re-homed: persisted equipId %u no "
                "longer matches its develop row - now equipId=%u (%d headOptions "
                "patched)\n",
                e.name, static_cast<unsigned>(oldId),
                static_cast<unsigned>(e.equipId), patched);
        }
        g_HasProvisionalHeads.store(provisionalLeft, std::memory_order_release);

        int resolved = 0;
        for (auto it = g_PendingHeads.begin(); it != g_PendingHeads.end(); )
        {
            const std::int32_t developId =
                V_FrameWorkState::GetDevelopIdByKey(it->name);
            const std::int32_t rowIndex =
                (developId > 0 && developId <= 0xFFFF)
                    ? TranslateDevelopIdToRowIndex(
                          static_cast<std::uint32_t>(developId))
                    : -1;
            if (IsResolvedRowValidUnlocked(rowIndex))
            {
                CompleteHeadRegistrationUnlocked(
                    it->name, it->faceIds, it->faceFv2Code, it->faceFpkCode,
                    static_cast<std::uint16_t>(developId),
                    static_cast<std::uint16_t>(rowIndex),
                    it->showInDevelopMenu, false);
                it = g_PendingHeads.erase(it);
                ++resolved;
            }
            else
            {
                ++it;
            }
        }

        if (g_PendingHeads.empty())
            g_HasPendingHeads.store(false, std::memory_order_release);

        if (g_PendingHeads.empty() && !provisionalLeft)
            g_PendingHeadsRetryTick.store(0, std::memory_order_relaxed);
        else
            g_PendingHeadsRetryTick.store(GetTickCount(),
                                          std::memory_order_relaxed);

        if (resolved > 0)
            LogDebug("[CustomHead] DrainPendingHeads: resolved %d deferred head(s); "
                "%zu still pending\n", resolved, g_PendingHeads.size());
        return resolved;
    }

    const CustomHeadEntry* TryGetCustomHeadByName(const char* name)
    {
        if (!name || !name[0]) return nullptr;
        std::lock_guard<std::mutex> lock(g_Mutex);
        const std::int32_t idx = FindByNameUnlocked(name);
        return (idx >= 0) ? &g_Heads[idx] : nullptr;
    }

    const CustomHeadEntry* TryGetCustomHeadByEquipId(std::uint16_t equipId)
    {
        if (equipId == 0) return nullptr;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const CustomHeadEntry& e : g_Heads)
        {
            if (e.used && e.equipId == equipId)
                return &e;
        }
        return nullptr;
    }

    const CustomHeadEntry* TryGetCustomHeadBySlot(std::uint8_t slotByte)
    {
        if (slotByte < kCustomHeadSlotBase) return nullptr;
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (const CustomHeadEntry& e : g_Heads)
        {
            if (e.used && e.slotByte == slotByte)
                return &e;
        }
        return nullptr;
    }

    std::uint16_t GetCurrentWornHeadEquipId()
    {
        if (!g_GetQuarkSystemTable)
        {
            g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
                ResolveGameAddress(gAddr.GetQuarkSystemTable));
            if (!g_GetQuarkSystemTable) return 0;
        }

        std::uint8_t slotByte = 0;
        __try
        {
            auto* qt = reinterpret_cast<std::uint8_t*>(g_GetQuarkSystemTable());
            if (!qt) return 0;
            auto* q98 = *reinterpret_cast<std::uint8_t**>(qt + 0x98);
            if (!q98) return 0;
            auto* state = *reinterpret_cast<std::uint8_t**>(q98 + 0x10);
            if (!state) return 0;


            slotByte = state[0xFA];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }

        if (const CustomHeadEntry* head = TryGetCustomHeadBySlot(slotByte))
            return head->equipId;
        return 0;
    }

    bool GetCurrentEquippedSuitBytes(std::uint8_t* outPartsType,
                                     std::uint8_t* outSelector)
    {
        if (outPartsType) *outPartsType = 0;
        if (outSelector)  *outSelector  = 0;

        if (!g_GetQuarkSystemTable)
        {
            g_GetQuarkSystemTable = reinterpret_cast<GetQuarkSystemTable_t>(
                ResolveGameAddress(gAddr.GetQuarkSystemTable));
            if (!g_GetQuarkSystemTable) return false;
        }

        std::uint8_t pt  = 0;
        std::uint8_t sel = 0;
        __try
        {
            auto* qt = reinterpret_cast<std::uint8_t*>(g_GetQuarkSystemTable());
            if (!qt) return false;
            auto* q98 = *reinterpret_cast<std::uint8_t**>(qt + 0x98);
            if (!q98) return false;
            auto* state = *reinterpret_cast<std::uint8_t**>(q98 + 0x10);
            if (!state) return false;

            pt  = state[0xF8];
            sel = state[0xF9];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        if (outPartsType) *outPartsType = pt;
        if (outSelector)  *outSelector  = sel;
        return true;
    }
}
