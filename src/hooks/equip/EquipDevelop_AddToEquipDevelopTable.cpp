#include "pch.h"
#include "EquipDevelop_AddToEquipDevelopTable.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AddressSet.h"
#include "DevelopArrayGrow.h"
#include "FoxHashes.h"
#include "HookUtils.h"
#include "log.h"
#include "V_FrameWorkState.h"
#include "../../lua/LuaApi.h"
#include "LuaStackGuard.h"
#include "../../core/LuaBroadcaster.h"
#include "../outfit/OutfitRegistry.h"
#include "../outfit/CustomHeadRegistry.h"
#include "../outfit/EquipDevelopControllerImpl_GetSuitDevelopInfoIndex.h"
#include "../outfit/ShadowState.h"
#include "EquipDevelop_SetEquipUndeveloped.h"

namespace equip { int MenuGridRowCap(); }

namespace
{
    int MenuRootRenderCap() { return equip::MenuGridRowCap(); }
}

namespace
{
    using RegCstDev_t = void(__fastcall*)(lua_State* L);
    using RegFlwDev_t = void(__fastcall*)(lua_State* L);

    struct FieldValue
    {
        enum class Type
        {
            Number,
            String,
            Boolean
        };

        std::string name;
        Type type = Type::Number;
        std::int32_t numberValue = 0;
        std::string stringValue;
        bool boolValue = false;
        bool wide = false;
        std::uint64_t wideValue = 0;
    };

    struct NamedFieldSpec
    {
        const char* canonicalName;
        const char* aliasName;
    };

    struct DevelopKeyIds
    {
        std::uint16_t developId = 0;
        std::uint16_t flowIndex = 0;
    };

    struct PendingDevelopRequest
    {
        std::string key;
        std::vector<FieldValue> constFields;
        std::vector<FieldValue> flowFields;
        bool hasDynamicGate = false;
    };

    EquipDevelopAdd::Deps g_Deps{};

    RegCstDev_t g_OrigRegCstDev = nullptr;
    RegFlwDev_t g_OrigRegFlwDev = nullptr;

    bool g_RegCstDevHookInstalled = false;
    bool g_RegFlwDevHookInstalled = false;

    struct DevelopNameLangId
    {
        bool hasHash = false;
        std::uint32_t hash = 0;
        std::string str;
    };

    std::recursive_mutex g_StateMutex;
    std::unordered_map<std::string, DevelopKeyIds> g_KeyRegistry;
    std::unordered_map<std::int32_t, DevelopNameLangId> g_DevelopNameByDevelopId;
    std::unordered_map<std::uint32_t, int> g_GradeByDevelopId;
    std::unordered_map<std::uint32_t, int> g_SideByDevelopId;
    std::unordered_map<std::uint32_t, int> g_OrdinalByDevelopId;
    std::unordered_map<std::uint32_t, std::uint32_t> g_ManagedBaseByDevelopId;

    constexpr int kMenuRootBufferCap = 120;
    constexpr int kMenuFamilyCap     = 1024;

    #define kMenuRootRenderCap (MenuRootRenderCap())
    std::unordered_map<int, int> g_NativeRootsByType;
    std::unordered_map<int, std::unordered_set<std::uint32_t>> g_CustomRootsByType;
    std::unordered_set<std::string> g_MenuCapRefusalLogged;
    std::unordered_set<std::string> g_PlacementWarnLogged;

    std::unordered_set<std::string> g_ParkedKeys;
    std::unordered_map<std::string, int> g_RotateFailCount;
    constexpr int      kMaxRotateFailsPerKey = 2;
    std::atomic<bool>  g_AnyParkedKeys{ false };
    std::atomic<bool>  g_AnyPendingReg{ false };
    std::atomic<bool>  g_RotationInProgress{ false };
    std::atomic<bool>  g_RotateRequested{ false };
    std::atomic<bool>  g_GateRefreshRequested{ false };
    std::atomic<bool>  g_RotateDeferLogged{ false };
    std::atomic<int>   g_MenuFillDepth{ 0 };
    std::atomic<int>   g_RotateTabHint{ -1 };
    volatile DWORD     g_LastVisPredicateTick = 0;
    std::size_t        g_RotateInCursor  = 0;
    std::size_t        g_RotateOutCursor = 0;
    std::uint64_t      g_PageStampCounter = 0;
    std::unordered_map<std::string, std::uint64_t> g_PageInStamp;
    std::unordered_map<std::string, std::uint64_t> g_PageOutStamp;
    constexpr DWORD    kMenuOpenGapMs        = 5000;
    constexpr int      kMaxSwapsPerRotation  = 32;

    std::unordered_map<std::string, PendingDevelopRequest> g_RequestByKey;

    constexpr int         kLuaRegistryIndex = -10000;
    constexpr const char* kGateTableKey     = "V_FrameWork_DevGates";
    struct DynamicGate { std::uint16_t flowIndex; std::string key; };
    std::vector<DynamicGate> g_DynamicGates;
    std::atomic<bool>        g_AnyDynamicGate{ false };
    volatile DWORD           g_LastGateRefreshTick = 0;
    constexpr DWORD          kGateRefreshThrottleMs = 300;

    static void EnsureGateTable(lua_State* L)
    {
        g_lua_getfield(L, kLuaRegistryIndex, const_cast<char*>(kGateTableKey));
        const bool exists = g_lua_type(L, -1) == LUA_TTABLE;
        g_lua_settop(L, g_lua_gettop(L) - 1);
        if (exists)
            return;
        g_lua_pushstring(L, const_cast<char*>(kGateTableKey));
        g_lua_createtable(L, 0, 0);
        g_lua_rawset(L, kLuaRegistryIndex);
    }

    static bool StoreGatePredicate(lua_State* L, int constTableIdx,
                                   const std::string& key)
    {
        if (!L || !ResolveLuaApi())
            return false;

        const int top = g_lua_gettop(L);
        bool isFunc = false;
        EnsureGateTable(L);

        const char* names[2] = { "bluePrintId", "p05" };
        for (const char* nm : names)
        {
            g_lua_pushstring(L, const_cast<char*>(nm));
            g_lua_gettable(L, constTableIdx);
            if (g_lua_type(L, -1) == LUA_TFUNCTION)
            {
                g_lua_getfield(L, kLuaRegistryIndex,
                               const_cast<char*>(kGateTableKey));
                g_lua_pushstring(L, const_cast<char*>(key.c_str()));
                g_lua_pushvalue(L, -3);
                g_lua_rawset(L, -3);
                isFunc = true;
                break;
            }
            g_lua_settop(L, g_lua_gettop(L) - 1);
        }

        g_lua_settop(L, top);
        return isFunc;
    }

    static void RegisterDynamicGate(std::uint16_t flowIndex,
                                    const std::string& key)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        for (DynamicGate& gt : g_DynamicGates)
        {
            if (gt.key == key)
            {
                gt.flowIndex = flowIndex;
                return;
            }
        }
        g_DynamicGates.push_back({ flowIndex, key });
        g_AnyDynamicGate.store(true, std::memory_order_relaxed);
    }

    static int EvalOneGateSEH(lua_State* L, std::uint16_t flowIndex,
                              const char* key)
    {
        int flippedVisible = 0;
        __try
        {
            const int top = g_lua_gettop(L);
            if (!luaguard::HasStackRoom(L, 4))
            {
                static bool s_logged = false;
                if (!s_logged)
                {
                    s_logged = true;
                    LogDebug("[EquipDevelop] dynamic gate eval skipped (key=%s): the running "
                        "Lua frame is too tight to push safely; it re-evaluates on the next "
                        "R&D menu build. Skipping prevents a Lua stack-overflow crash.\n",
                        key);
                }
                return 0;
            }
            g_lua_getfield(L, kLuaRegistryIndex,
                           const_cast<char*>(kGateTableKey));
            if (g_lua_type(L, -1) != LUA_TTABLE)
            {
                g_lua_settop(L, top);
                return 0;
            }
            g_lua_getfield(L, -1, const_cast<char*>(key));
            if (g_lua_type(L, -1) != LUA_TFUNCTION)
            {
                g_lua_settop(L, top);
                return 0;
            }
            const int err = g_lua_pcall(L, 0, 1, 0);
            if (err == 0)
            {
                const bool visible   = g_lua_toboolean(L, -1) != 0;
                const bool wasHidden = outfit::IsDevelopHidden(flowIndex);
                outfit::SetDevelopHidden(flowIndex, !visible);
                if (wasHidden != !visible)
                {
                    LogDebug("[EquipDevelop] dynamic gate key=%s flowIndex=%u -> %s\n",
                        key, static_cast<unsigned>(flowIndex),
                        visible ? "VISIBLE" : "HIDDEN");
                    if (visible)
                        flippedVisible = 1;
                }
            }
            g_lua_settop(L, top);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return flippedVisible;
    }

    static void RefreshDynamicGatesImpl()
    {
        std::vector<DynamicGate> gates;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            if (g_DynamicGates.empty())
                return;
            gates = g_DynamicGates;
        }
        if (!ResolveLuaApi())
            return;
        lua_State* L = V_FrameWork_AnyLuaState();
        if (!L)
            return;

        bool anyRevealed = false;
        for (const DynamicGate& gt : gates)
            if (EvalOneGateSEH(L, gt.flowIndex, gt.key.c_str()) == 1)
                anyRevealed = true;

        if (anyRevealed)
            EquipDevelop_TriggerRequirementsMetAnnounce();
    }

    struct NativeFlowInfo
    {
        std::uint8_t grade     = 0;
        std::uint8_t sideGrade = 0;
        bool         seen      = false;
    };
    constexpr std::size_t kNativeFlowCap = equip::kMaxFlowSlots;
    NativeFlowInfo g_NativeFlow[kNativeFlowCap] = {};
    std::unordered_map<std::uint32_t, std::uint32_t> g_NativeBaseByDevelopId;

    std::uint32_t g_NativeConstOrder = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> g_NativeOrderDevelopId;
    std::unordered_map<std::uint32_t, NativeFlowInfo> g_NativeFlowByDevelopId;

    std::unordered_map<std::uint32_t, int> g_NativeTypeByDevelopId;
    std::unordered_map<int, std::unordered_set<std::uint64_t>> g_TabRenderRows;
    std::unordered_set<int> g_TabRowsSeeded;

    static std::uint32_t WalkNativeRoot_NoLock(std::uint32_t developId)
    {
        std::uint32_t cur = developId;
        for (int hop = 0; hop < 64; ++hop)
        {
            auto it = g_NativeBaseByDevelopId.find(cur);
            if (it == g_NativeBaseByDevelopId.end() || it->second == 0
                || it->second == cur)
                break;
            cur = it->second;
        }
        return cur;
    }

    constexpr std::size_t kDevelopRecordArrayOffset = 0x8;
    constexpr std::size_t kDevelopRecordStride      = 0x68;

    struct NativeRecordSnap
    {
        std::uint16_t developId = 0;
        std::uint16_t base = 0;
        std::uint8_t  type = 0;
        std::uint8_t  grade = 0;
        std::uint8_t  side = 0;
    };

    static bool ReadRecordSnap(void* controller, std::uint16_t idx,
                               NativeRecordSnap& out)
    {
        __try
        {
            const std::uint8_t* rec =
                reinterpret_cast<const std::uint8_t*>(controller)
                + kDevelopRecordArrayOffset
                + static_cast<std::size_t>(idx) * kDevelopRecordStride;
            out.developId = *reinterpret_cast<const std::uint16_t*>(rec);
            out.base      = *reinterpret_cast<const std::uint16_t*>(rec + 2);
            const std::uint8_t packed = rec[0xD];
            out.grade = packed & 0xF;
            out.side  = packed >> 5;
            out.type  = rec[0x36];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void BuildNativeRecordSnapshot(std::vector<NativeRecordSnap>& out)
    {
        out.clear();
        void* controller = EquipDevelop_ResolveDevelopController();
        if (!controller)
            return;
        out.reserve(equip::kMaxFlowSlots);
        for (std::uint32_t i = 0; i < equip::NativeFlowBound(); ++i)
        {
            if (equip::IsReservedFlowRow(i))
                continue;
            NativeRecordSnap s;
            if (!ReadRecordSnap(controller, static_cast<std::uint16_t>(i), s)
                || s.developId == 0)
                break;
            out.push_back(s);
        }
    }

    static void LogNativeRowBudgetOnce(
        const std::vector<NativeRecordSnap>& snap,
        const std::unordered_map<std::uint32_t, std::uint32_t>& baseById)
    {
        static bool s_logged = false;
        if (s_logged || snap.empty())
            return;
        s_logged = true;
        std::unordered_map<int, std::unordered_set<std::uint64_t>> rowsByTab;
        for (const NativeRecordSnap& s : snap)
        {
            std::uint32_t root = s.developId;
            for (int hop = 0; hop < 64; ++hop)
            {
                auto it = baseById.find(root);
                if (it == baseById.end() || it->second == 0
                    || it->second == root)
                    break;
                root = it->second;
            }
            rowsByTab[s.type].insert(
                (static_cast<std::uint64_t>(root) << 8)
                | static_cast<std::uint64_t>(s.side & 0xFF));
        }
        std::string line;
        line.reserve(256);
        char buf[48];
        for (const auto& kv : rowsByTab)
        {
            std::snprintf(buf, sizeof(buf), "tab %d=%zu rows (%d free)  ",
                          kv.first, kv.second.size(),
                          114 - static_cast<int>(kv.second.size()));
            line += buf;
        }
        LogDebug("[EquipDevelop] native grid budget (114 rows/tab): %s\n",
            line.c_str());
    }

    static std::unordered_set<std::uint64_t>& TabRenderRows_NoLock(int devType)
    {
        std::unordered_set<std::uint64_t>& rows = g_TabRenderRows[devType];
        if (g_TabRowsSeeded.insert(devType).second)
        {
            std::vector<NativeRecordSnap> snap;
            BuildNativeRecordSnapshot(snap);
            if (!snap.empty())
            {
                std::unordered_map<std::uint32_t, std::uint32_t> baseById;
                for (const NativeRecordSnap& s : snap)
                    baseById[s.developId] = s.base;
                LogNativeRowBudgetOnce(snap, baseById);
                int nativeRoots = 0;
                for (const NativeRecordSnap& s : snap)
                {
                    if (s.type != devType)
                        continue;
                    if (g_GradeByDevelopId.find(s.developId)
                        != g_GradeByDevelopId.end())
                        continue;
                    if (s.base == 0)
                        ++nativeRoots;
                    std::uint32_t root = s.developId;
                    for (int hop = 0; hop < 64; ++hop)
                    {
                        auto it = baseById.find(root);
                        if (it == baseById.end() || it->second == 0
                            || it->second == root)
                            break;
                        root = it->second;
                    }
                    rows.insert((static_cast<std::uint64_t>(root) << 8)
                                | static_cast<std::uint64_t>(s.side & 0xFF));
                }
                g_NativeRootsByType[devType] = nativeRoots;
            }
            else
            {
                for (const auto& kv : g_NativeBaseByDevelopId)
                {
                    auto itT = g_NativeTypeByDevelopId.find(kv.first);
                    if (itT == g_NativeTypeByDevelopId.end()
                        || itT->second != devType)
                        continue;
                    const std::uint32_t root = WalkNativeRoot_NoLock(kv.first);
                    int side = 0;
                    auto itF = g_NativeFlowByDevelopId.find(kv.first);
                    if (itF != g_NativeFlowByDevelopId.end() && itF->second.seen)
                        side = itF->second.sideGrade;
                    rows.insert((static_cast<std::uint64_t>(root) << 8)
                                | static_cast<std::uint64_t>(side & 0xFF));
                }
            }
        }
        return rows;
    }

    static std::uint32_t WalkAnyRoot_NoLock(std::uint32_t developId)
    {
        std::uint32_t cur = developId;
        for (int hop = 0; hop < 64; ++hop)
        {
            std::uint32_t base = 0;
            auto itM = g_ManagedBaseByDevelopId.find(cur);
            if (itM != g_ManagedBaseByDevelopId.end())
                base = itM->second;
            else
            {
                auto itN = g_NativeBaseByDevelopId.find(cur);
                if (itN != g_NativeBaseByDevelopId.end())
                    base = itN->second;
            }
            if (base == 0 || base == cur)
                break;
            cur = base;
        }
        return cur;
    }

    static void RebuildTabRenderRows_NoLock(int devType)
    {
        g_TabRowsSeeded.erase(devType);
        g_TabRenderRows[devType].clear();
        auto& rows = TabRenderRows_NoLock(devType);
        for (const auto& kv : g_KeyRegistry)
        {
            if (kv.second.flowIndex == 0)
                continue;
            const std::uint32_t dev = kv.second.developId;
            if (g_GradeByDevelopId.find(dev) == g_GradeByDevelopId.end())
                continue;
            auto itReq = g_RequestByKey.find(kv.first);
            if (itReq == g_RequestByKey.end())
                continue;
            std::int32_t t = 0;
            for (const FieldValue& f : itReq->second.constFields)
            {
                if (f.type == FieldValue::Type::Number && f.name == "p02")
                {
                    t = f.numberValue;
                    break;
                }
            }
            if (t != devType)
                continue;
            int side = 0;
            auto itS = g_SideByDevelopId.find(dev);
            if (itS != g_SideByDevelopId.end())
                side = itS->second;
            rows.insert((static_cast<std::uint64_t>(WalkAnyRoot_NoLock(dev)) << 8)
                        | static_cast<std::uint64_t>(side & 0xFF));
        }
    }

    static void RebuildAllSeededTabRenderRows_NoLock()
    {
        std::vector<int> types(g_TabRowsSeeded.begin(), g_TabRowsSeeded.end());
        for (int t : types)
            RebuildTabRenderRows_NoLock(t);
    }

    using GetDevIndex_t = std::uint16_t (__fastcall*)(void*, std::uint32_t);

    static std::uint16_t SafeGetDevIndexLocal(GetDevIndex_t fn, void* ctrl,
                                              std::uint32_t developId)
    {
        __try { return fn(ctrl, developId); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0x400; }
    }

    static bool TryGetNativeFlowInfo(std::uint32_t developId,
                                     int* outGrade, int* outSideGrade)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            auto it = g_NativeFlowByDevelopId.find(developId);
            if (it != g_NativeFlowByDevelopId.end() && it->second.seen)
            {
                if (outGrade)     *outGrade     = it->second.grade;
                if (outSideGrade) *outSideGrade = it->second.sideGrade;
                return true;
            }
        }

        auto getIndex = reinterpret_cast<GetDevIndex_t>(
            ResolveGameAddress(gAddr.EquipDevelopCtrl_GetEquipDevelopIndex));
        void* ctrl = EquipDevelop_ResolveDevelopController();
        if (!getIndex || !ctrl)
            return false;
        const std::uint16_t idx = SafeGetDevIndexLocal(getIndex, ctrl, developId);
        if (idx >= kNativeFlowCap)
            return false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            if (g_NativeFlow[idx].seen)
            {
                if (outGrade)     *outGrade     = g_NativeFlow[idx].grade;
                if (outSideGrade) *outSideGrade = g_NativeFlow[idx].sideGrade;
                return true;
            }
        }
        NativeRecordSnap s;
        if (ReadRecordSnap(ctrl, idx, s) && s.developId == developId
            && s.grade >= 1)
        {
            if (outGrade)     *outGrade     = s.grade;
            if (outSideGrade) *outSideGrade = s.side;
            return true;
        }
        return false;
    }

    struct FamilyInfo
    {
        std::uint32_t mainGradeMask = 0;
        std::uint32_t allGradeMask  = 0;
        std::uint32_t sideMask      = 0;
        int           parentGrade   = -1;
        int           parentSide    = 0;
        int           maxAnyGrade   = 0;
        int           memberCount   = 0;
        std::uint32_t rootDevelopId = 0;
        std::uint16_t sideUsedAtGrade[16] = {};

        std::uint16_t childOrdinalMask = 0;
        int           forkSpanPrefix[16] = {};
    };

    static FamilyInfo CollectFamilyInfo(std::uint32_t parentDevelopId)
    {
        FamilyInfo out{};

        std::vector<NativeRecordSnap> snap;
        BuildNativeRecordSnapshot(snap);
        std::unordered_map<std::uint32_t, const NativeRecordSnap*> snapById;
        for (const NativeRecordSnap& s : snap)
            snapById[s.developId] = &s;

        std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
        std::unordered_map<std::uint32_t, std::pair<int, int>> managedInfo;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            edges.reserve(snap.size() + g_NativeBaseByDevelopId.size()
                          + g_ManagedBaseByDevelopId.size());
            for (const NativeRecordSnap& s : snap)
                if (s.base != 0)
                    edges.push_back({ s.developId, s.base });
            for (const auto& kv : g_NativeBaseByDevelopId)
                if (kv.second != 0)
                    edges.push_back({ kv.first, kv.second });
            for (const auto& kv : g_ManagedBaseByDevelopId)
                if (kv.second != 0)
                    edges.push_back({ kv.first, kv.second });
            for (const auto& kv : g_GradeByDevelopId)
            {
                int ordinal = 0;
                const auto itO = g_OrdinalByDevelopId.find(kv.first);
                if (itO != g_OrdinalByDevelopId.end())
                    ordinal = itO->second;
                managedInfo[kv.first] = { kv.second, ordinal };
            }
        }

        std::uint32_t root = parentDevelopId;
        for (int hop = 0; hop < 64; ++hop)
        {
            std::uint32_t base = 0;
            for (const auto& e : edges)
                if (e.first == root) { base = e.second; break; }
            if (base == 0 || base == root)
                break;
            root = base;
        }

        std::vector<std::uint32_t> family{ root };
        std::unordered_set<std::uint32_t> seen{ root };
        for (std::size_t i = 0; i < family.size(); ++i)
            for (const auto& e : edges)
                if (e.second == family[i] && seen.insert(e.first).second)
                    family.push_back(e.first);
        out.memberCount   = static_cast<int>(family.size());
        out.rootDevelopId = root;

        std::unordered_map<std::uint32_t, std::uint32_t> parentOf;
        parentOf.reserve(family.size());
        for (std::uint32_t id : family)
            for (const auto& e : edges)
                if (e.first == id) { parentOf[id] = e.second; break; }

        std::unordered_map<std::uint32_t, int> gradeById;
        std::unordered_map<std::uint32_t, int> ordById;
        gradeById.reserve(family.size());
        ordById.reserve(family.size());
        for (std::uint32_t id : family)
        {
            int gr = -1, sg = 0;
            const auto itM = managedInfo.find(id);
            const auto itSnap = snapById.find(id);
            if (itM != managedInfo.end())
            {
                gr = itM->second.first;
                sg = itM->second.second;
            }
            else if (itSnap != snapById.end() && itSnap->second->grade >= 1)
            {
                gr = itSnap->second->grade;
                sg = itSnap->second->side;
            }
            else if (!TryGetNativeFlowInfo(id, &gr, &sg))
            {
                continue;
            }
            if (gr < 0)
                continue;
            gradeById[id] = gr;
            ordById[id]   = (sg >= 0 && sg <= 15) ? sg : 0;
        }

        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> childrenOf;
        for (std::uint32_t id : family)
        {
            if (id == root || ordById.find(id) == ordById.end())
                continue;
            const auto itP = parentOf.find(id);
            if (itP != parentOf.end())
                childrenOf[itP->second].push_back(id);
        }

        std::unordered_map<std::uint32_t, int> spanById;
        spanById.reserve(ordById.size());
        for (auto it = family.rbegin(); it != family.rend(); ++it)
        {
            const std::uint32_t id = *it;
            if (ordById.find(id) == ordById.end())
                continue;
            int span = 1;
            const auto itC = childrenOf.find(id);
            if (itC != childrenOf.end())
                for (std::uint32_t c : itC->second)
                {
                    const auto itS = spanById.find(c);
                    if (itS == spanById.end())
                        continue;
                    span += (ordById[c] == 0) ? (itS->second - 1)
                                              : itS->second;
                }
            spanById[id] = span;
        }

        std::unordered_map<std::uint32_t, int> rowById;
        rowById.reserve(ordById.size());
        for (std::uint32_t id : family)
        {
            const auto itO = ordById.find(id);
            if (itO == ordById.end())
                continue;
            int row = 0;
            if (id == root)
            {
                row = itO->second;
            }
            else
            {
                const auto itP = parentOf.find(id);
                const int prow = (itP != parentOf.end()
                                  && rowById.count(itP->second))
                    ? rowById[itP->second] : 0;
                if (itO->second == 0)
                {
                    row = prow;
                }
                else
                {
                    row = prow + 1;
                    const auto itC = (itP != parentOf.end())
                        ? childrenOf.find(itP->second) : childrenOf.end();
                    if (itC != childrenOf.end())
                        for (std::uint32_t sib : itC->second)
                        {
                            if (sib == id)
                                continue;
                            const int so = ordById.count(sib) ? ordById[sib] : 0;
                            if (so >= 1 && so < itO->second
                                && spanById.count(sib))
                                row += spanById[sib];
                        }
                }
            }
            if (row < 0)  row = 0;
            if (row > 15) row = 15;
            rowById[id] = row;

            const int gr = gradeById[id];
            if (gr >= 1 && gr <= 15)
            {
                out.allGradeMask |= (1u << gr);
                if (gr > out.maxAnyGrade)
                    out.maxAnyGrade = gr;
                if (row == 0)
                    out.mainGradeMask |= (1u << gr);
                out.sideUsedAtGrade[gr] |= static_cast<std::uint16_t>(1u << row);
            }
            if (row >= 1)
                out.sideMask |= (1u << row);
            if (id == parentDevelopId)
            {
                out.parentGrade = gr;
                out.parentSide  = row;
            }
        }

        const auto itPC = childrenOf.find(parentDevelopId);
        if (itPC != childrenOf.end())
        {
            for (std::uint32_t c : itPC->second)
            {
                const int co = ordById.count(c) ? ordById[c] : 0;
                if (co >= 0 && co <= 15)
                    out.childOrdinalMask |=
                        static_cast<std::uint16_t>(1u << co);
            }
            for (int k = 1; k <= 15; ++k)
            {
                int sum = 0;
                for (std::uint32_t c : itPC->second)
                {
                    const int co = ordById.count(c) ? ordById[c] : 0;
                    if (co >= 1 && co < k && spanById.count(c))
                        sum += spanById[c];
                }
                out.forkSpanPrefix[k] = sum;
            }
        }

        return out;
    }
    std::vector<PendingDevelopRequest> g_PendingRequests;
    bool g_IsFlushingPending = false;

    std::uint32_t g_NextDevelopId = 0;
    std::uint32_t g_NextFlowIndex = 0;
    bool g_ObservedAnyDevelopId = false;
    bool g_ObservedAnyFlowIndex = false;


    std::unordered_set<std::uint32_t> g_ObservedStockDevelopIds;
    std::unordered_set<std::uint32_t> g_ObservedStockFlowIndices;

    constexpr int LUA_TNUMBER_CONST = 3;
    constexpr int LUA_TSTRING_CONST = 4;
    constexpr int LUA_TTABLE_CONST = 5;


    constexpr std::uint32_t kBootstrapDevelopIdStart = 51006;
    constexpr std::uint32_t kBootstrapFlowIndexStart = 922;
    constexpr std::uint32_t kMaxAllocId = 0xFFFEu;

    constexpr int kMinGrade = 1;
    constexpr int kMaxGrade = 15;

    constexpr int kMaxSideGrade = 7;

    static const NamedFieldSpec k_ConstFieldSpecs[] =
    {
        { "p01", "equipID" },
        { "p02", "equipDevelopTypeID" },
        { "p03", "baseEquipDevelopId" },
        { "p04", "skill" },
        { "p05", "bluePrintId" },
        { "p06", "langEquipName" },
        { "p07", "langEquipInfo" },
        { "p08", "iconFtexPath" },
        { "p09", "equipDevelopGroupID" },

        { "p10", "langPowerUpInfo0"  },
        { "p11", "langPowerUpInfo1"  },
        { "p12", "langPowerUpInfo2"  },
        { "p13", "langPowerUpInfo3"  },
        { "p14", "langPowerUpInfo4"  },
        { "p15", "langPowerUpInfo5"  },
        { "p16", "langPowerUpInfo6"  },
        { "p17", "langPowerUpInfo7"  },
        { "p18", "langPowerUpInfo8"  },
        { "p19", "langPowerUpInfo9"  },
        { "p20", "langPowerUpInfo10" },
        { "p21", "langPowerUpInfo11" },

        { "p30", "langEquipRealName" },
        { "p31", "isResultRankLimited" },
        { "p32", "isCustomEnable" },
        { "p33", "isColorChangeEnable" },
        { "p34", "unk34" },
        { "p35", "isSecurityStaffEquip" },
        { "p36", "unk36" }
    };

    static const NamedFieldSpec k_FlowFieldSpecs[] =
    {
        { "p51", "sideGrade" },
        { "p52", "grade" },
        { "p53", "developGmpCost" },
        { "p54", "usageGmpCost" },
        { "p55", "sectionLvForDevelop" },
        { "p56", "sectionID2ForDevelop" },
        { "p57", "sectionLv2ForDevelop" },
        { "p58", "resourceType1" },
        { "p59", "resourceType1Count" },
        { "p60", "resourceType2" },
        { "p61", "resourceType2Count" },
        { "p62", "initialAvailable" },
        { "p63", "sectionIDForDevelop" },
        { "p64", "developSectionLv" },
        { "p65", "resourceUsageType1" },
        { "p66", "resourceUsageType1Count" },
        { "p67", "resourceUsageType2" },
        { "p68", "resourceUsageType2Count" },
        { "p69", "displayInfo" },
        { "p70", "unk70" },
        { "p71", "developTimeMinute" },
        { "p72", "isValidMbCoin" },
        { "p73", "intimacyPoint" },
        { "p74", "isFobAvailable" },
    };
}

namespace
{
    static bool ValidateDeps()
    {
        return
            g_Deps.ResolveLuaApi &&
            g_Deps.GetLuaTop &&
            g_Deps.LuaType &&
            g_Deps.LuaSetTop &&
            g_Deps.GetLuaString &&
            g_Deps.GetLuaInt &&
            g_Deps.PushLuaNumber &&
            g_Deps.LuaPushString &&
            g_Deps.LuaCreateTable &&
            g_Deps.LuaGetTable &&
            g_Deps.LuaSetTable;
    }

    static bool EnsureLuaReady()
    {
        return ValidateDeps() && g_Deps.ResolveLuaApi && g_Deps.ResolveLuaApi();
    }

    static int AbsIndex(lua_State* L, int idx)
    {
        if (idx > 0)
            return idx;

        return g_Deps.GetLuaTop(L) + idx + 1;
    }

    static bool IsLuaTable(lua_State* L, int idx)
    {
        return g_Deps.LuaType(L, idx) == LUA_TTABLE_CONST;
    }

    static bool IsLuaNumber(lua_State* L, int idx)
    {
        return g_Deps.LuaType(L, idx) == LUA_TNUMBER_CONST;
    }

    static bool IsLuaString(lua_State* L, int idx)
    {
        return g_Deps.LuaType(L, idx) == LUA_TSTRING_CONST;
    }

    static void PushFieldKey(lua_State* L, const char* key)
    {
        g_Deps.LuaPushString(L, key);
    }

    static bool TryReadIntFieldByName(
        lua_State* L,
        int tableIndex,
        const char* fieldName,
        std::int32_t& outValue)
    {
        outValue = 0;

        const int absIndex = AbsIndex(L, tableIndex);

        PushFieldKey(L, fieldName);
        g_Deps.LuaGetTable(L, absIndex);

        const bool ok = IsLuaNumber(L, -1);
        if (ok)
            outValue = g_Deps.GetLuaInt(L, -1);

        g_Deps.LuaSetTop(L, -2);
        return ok;
    }

    static bool TryReadWideFieldByName(
        lua_State* L,
        int tableIndex,
        const char* fieldName,
        std::uint64_t& outValue)
    {
        outValue = 0;

        const int absIndex = AbsIndex(L, tableIndex);

        PushFieldKey(L, fieldName);
        g_Deps.LuaGetTable(L, absIndex);

        const bool ok = IsLuaNumber(L, -1);
        if (ok)
            outValue = GetLuaInt64(L, -1);

        g_Deps.LuaSetTop(L, -2);
        return ok;
    }

    static bool IsLangIdConstField(const char* name)
    {
        if (!name || name[0] != 'p' || name[1] == '\0' || name[2] == '\0'
            || name[3] != '\0')
            return false;
        if (name[1] == '1' && name[2] >= '0' && name[2] <= '9')
            return true;
        if (name[1] == '2' && (name[2] == '0' || name[2] == '1'))
            return true;
        return false;
    }

    static bool TryReadStringFieldByName(
        lua_State* L,
        int tableIndex,
        const char* fieldName,
        std::string& outValue)
    {
        outValue.clear();

        const int absIndex = AbsIndex(L, tableIndex);

        PushFieldKey(L, fieldName);
        g_Deps.LuaGetTable(L, absIndex);

        const bool ok = IsLuaString(L, -1);
        if (ok)
        {
            const char* value = g_Deps.GetLuaString(L, -1);
            outValue = value ? value : "";
        }

        g_Deps.LuaSetTop(L, -2);
        return ok;
    }

    constexpr int LUA_TBOOLEAN_CONST = 1;

    static bool IsLuaBoolean(lua_State* L, int idx)
    {
        return g_Deps.LuaType(L, idx) == LUA_TBOOLEAN_CONST;
    }

    static bool TryReadBoolFieldByName(
        lua_State* L,
        int tableIndex,
        const char* fieldName,
        bool& outValue)
    {
        outValue = false;

        if (!g_Deps.LuaToBool)
            return false;

        const int absIndex = AbsIndex(L, tableIndex);

        PushFieldKey(L, fieldName);
        g_Deps.LuaGetTable(L, absIndex);

        const bool ok = IsLuaBoolean(L, -1);
        if (ok)
            outValue = g_Deps.LuaToBool(L, -1);

        g_Deps.LuaSetTop(L, -2);
        return ok;
    }

    static void SetIntField(lua_State* L, int tableIndex, const char* key, std::int32_t value)
    {
        const int absIndex = AbsIndex(L, tableIndex);

        PushFieldKey(L, key);
        g_Deps.PushLuaNumber(L, static_cast<float>(value));
        g_Deps.LuaSetTable(L, absIndex);
    }

    static void SetStringField(lua_State* L, int tableIndex, const char* key, const std::string& value)
    {
        const int absIndex = AbsIndex(L, tableIndex);

        PushFieldKey(L, key);
        g_Deps.LuaPushString(L, value.c_str());
        g_Deps.LuaSetTable(L, absIndex);
    }

    static void SetWideField(lua_State* L, int tableIndex, const char* key,
                             std::uint64_t value)
    {
        const int absIndex = AbsIndex(L, tableIndex);

        PushFieldKey(L, key);
        if (g_lua_pushnumber)
            g_lua_pushnumber(L, static_cast<lua_Number>(value));
        else
            g_Deps.PushLuaNumber(L, static_cast<float>(value));
        g_Deps.LuaSetTable(L, absIndex);
    }

    static bool AreStockDevelopTablesReady_NoLock()
    {
        return g_ObservedAnyDevelopId && g_ObservedAnyFlowIndex;
    }

    static bool FindFirstEmptyRecordIndex(void* controller,
                                          std::uint16_t& outIndex);

    constexpr std::uint16_t kVanillaDevelopRowCount = 922;

    static bool CanInjectRowsNow_NoLock()
    {
        if (!g_OrigRegCstDev || !g_OrigRegFlwDev)
            return false;
        void* controller = EquipDevelop_ResolveDevelopController();
        if (!controller)
            return false;
        std::uint16_t firstEmpty = 0;
        if (!FindFirstEmptyRecordIndex(controller, firstEmpty))
            return true;
        return firstEmpty >= kVanillaDevelopRowCount;
    }

    static void RequestRotationIfBandHasRoom()
    {
        void* controller = EquipDevelop_ResolveDevelopController();
        if (!controller)
            return;
        std::uint16_t freeIdx = 0;
        if (FindFirstEmptyRecordIndex(controller, freeIdx)
            && freeIdx >= kVanillaDevelopRowCount)
            g_RotateRequested.store(true, std::memory_order_release);
    }

    static void EnsureAllocatorSeeded_NoLock()
    {
        if (g_NextDevelopId < kBootstrapDevelopIdStart)
            g_NextDevelopId = kBootstrapDevelopIdStart;

        if (g_NextFlowIndex < kBootstrapFlowIndexStart)
            g_NextFlowIndex = kBootstrapFlowIndexStart;
    }

    static bool TryGetIdsForKey_NoLock(
        const std::string& key,
        std::uint16_t& outDevelopId,
        std::uint16_t& outFlowIndex)
    {
        const auto found = g_KeyRegistry.find(key);
        if (found == g_KeyRegistry.end())
            return false;

        outDevelopId = found->second.developId;
        outFlowIndex = found->second.flowIndex;
        return true;
    }

    static bool TryGetIdsForKey(
        const std::string& key,
        std::uint16_t& outDevelopId,
        std::uint16_t& outFlowIndex)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        return TryGetIdsForKey_NoLock(key, outDevelopId, outFlowIndex);
    }

    static PendingDevelopRequest* FindPendingRequest_NoLock(const std::string& key)
    {
        for (auto& req : g_PendingRequests)
        {
            if (req.key == key)
                return &req;
        }
        return nullptr;
    }

    std::atomic<bool> g_SwapInProgress{ false };

    static void ParkKeyForPaging(const std::string& key)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        g_ParkedKeys.insert(key);
        g_AnyParkedKeys.store(true, std::memory_order_relaxed);
        g_PendingRequests.erase(
            std::remove_if(
                g_PendingRequests.begin(), g_PendingRequests.end(),
                [&](const PendingDevelopRequest& r) { return r.key == key; }),
            g_PendingRequests.end());
        g_AnyPendingReg.store(!g_PendingRequests.empty(),
                              std::memory_order_relaxed);

        if (g_SwapInProgress.load(std::memory_order_relaxed))
            return;
        auto itReg = g_KeyRegistry.find(key);
        if (itReg == g_KeyRegistry.end())
            return;
        if (g_GradeByDevelopId.find(itReg->second.developId)
            != g_GradeByDevelopId.end())
            return;
        if (itReg->second.flowIndex == 0)
        {
            V_FrameWorkState::ReleaseSessionFlowIndex(key.c_str());
            return;
        }
        const std::uint16_t idx = itReg->second.flowIndex;
        itReg->second.flowIndex = 0;
        if (static_cast<std::uint32_t>(idx) + 1 == g_NextFlowIndex)
            --g_NextFlowIndex;
        V_FrameWorkState::ReleaseSessionFlowIndex(key.c_str());
        outfit::SetOutfitFlowIndexByDevelopId(itReg->second.developId, 0);
    }

    static bool GetOrCreateIdsForKey(
        const std::string& key,
        std::uint16_t& outDevelopId,
        std::uint16_t& outFlowIndex,
        bool& outCreated)
    {
        outDevelopId = 0;
        outFlowIndex = 0;
        outCreated = false;

        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

        if (key.empty())
            return false;

        const auto found = g_KeyRegistry.find(key);
        if (found != g_KeyRegistry.end())
        {
            outDevelopId = found->second.developId;
            outFlowIndex = found->second.flowIndex;
            return true;
        }

        EnsureAllocatorSeeded_NoLock();

        if (g_NextDevelopId > kMaxAllocId || g_NextFlowIndex > kMaxAllocId)
        {
            LogDebug("[EquipDevelop] ID allocator overflow\n");
            return false;
        }

        DevelopKeyIds ids{};


        std::int32_t persistedDevelopId = 0;
        if (V_FrameWorkState::ResolveOrCreateDevelopId(
                key.c_str(),
                static_cast<std::int32_t>(g_NextDevelopId),
                persistedDevelopId) &&
            persistedDevelopId > 0)
        {
            ids.developId = static_cast<std::uint16_t>(persistedDevelopId);
            if (static_cast<std::uint32_t>(persistedDevelopId) >= g_NextDevelopId)
                g_NextDevelopId = static_cast<std::uint32_t>(persistedDevelopId) + 1;
        }
        else
        {
            ids.developId = static_cast<std::uint16_t>(g_NextDevelopId++);
        }


        ids.flowIndex = 0;

        g_KeyRegistry.emplace(key, ids);

        outDevelopId = ids.developId;
        outFlowIndex = ids.flowIndex;
        outCreated = true;

        return true;
    }

    static void PersistPlacementInRequest_NoLock(
        const std::string& key, int grade, int side)
    {
        auto it = g_RequestByKey.find(key);
        if (it == g_RequestByKey.end())
            return;
        bool hasGrade = false, hasSide = false;
        for (FieldValue& f : it->second.flowFields)
        {
            if (f.name == "p52")
            {
                f.type = FieldValue::Type::Number;
                f.numberValue = grade;
                hasGrade = true;
            }
            else if (f.name == "p51")
            {
                f.type = FieldValue::Type::Number;
                f.numberValue = side;
                hasSide = true;
            }
        }
        if (!hasGrade)
        {
            FieldValue f;
            f.name = "p52";
            f.type = FieldValue::Type::Number;
            f.numberValue = grade;
            it->second.flowFields.push_back(f);
        }
        if (!hasSide)
        {
            FieldValue f;
            f.name = "p51";
            f.type = FieldValue::Type::Number;
            f.numberValue = side;
            it->second.flowFields.push_back(f);
        }
    }

    static void QueueOrUpdatePendingRequest(
        const std::string& key,
        const std::vector<FieldValue>& constFields,
        const std::vector<FieldValue>& flowFields,
        bool hasDynamicGate)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

        if (key.empty())
            return;

        g_RequestByKey[key] =
            PendingDevelopRequest{ key, constFields, flowFields, hasDynamicGate };

        if (PendingDevelopRequest* existing = FindPendingRequest_NoLock(key))
        {
            existing->constFields = constFields;
            existing->flowFields = flowFields;
            existing->hasDynamicGate = hasDynamicGate;
            return;
        }

        PendingDevelopRequest req{};
        req.key = key;
        req.constFields = constFields;
        req.flowFields = flowFields;
        req.hasDynamicGate = hasDynamicGate;
        g_PendingRequests.push_back(std::move(req));
        g_AnyPendingReg.store(true, std::memory_order_relaxed);
    }

    static void PublishSummaryDisplay(
        std::uint16_t developId,
        const std::vector<FieldValue>& constFields)
    {
        std::uint64_t nameHash = 0;
        std::uint64_t iconHash = 0;
        for (const FieldValue& f : constFields)
        {
            if (f.name == "p06")
            {
                if (f.type == FieldValue::Type::String)
                    nameHash = FoxHashes::StrCode64(f.stringValue);
                else if (f.type == FieldValue::Type::Number)
                    nameHash = static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(f.numberValue));
            }
            else if (f.name == "p08"
                     && f.type == FieldValue::Type::String)
            {
                iconHash = FoxHashes::PathCode64Ext(f.stringValue);
            }
        }
        if (nameHash != 0 || iconHash != 0)
            outfit::SetOutfitSummaryDisplay(developId, nameHash, iconHash);
    }

    static void ObserveDevelopId(std::uint32_t developId)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

        g_ObservedAnyDevelopId = true;
        if (developId >= g_NextDevelopId)
            g_NextDevelopId = developId + 1;


        g_ObservedStockDevelopIds.insert(developId);
    }

    static void ObserveFlowIndex(std::uint32_t flowIndex)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

        g_ObservedAnyFlowIndex = true;
        if (flowIndex >= g_NextFlowIndex)
            g_NextFlowIndex = flowIndex + 1;


        g_ObservedStockFlowIndices.insert(flowIndex);
    }

    static void CollectKnownFields(
        lua_State* L,
        int tableIndex,
        const NamedFieldSpec* specs,
        std::size_t specCount,
        std::vector<FieldValue>& outFields)
    {
        outFields.clear();

        for (std::size_t i = 0; i < specCount; ++i)
        {
            const NamedFieldSpec& spec = specs[i];

            if (IsLangIdConstField(spec.canonicalName))
            {
                std::uint64_t wideValue = 0;
                if (TryReadWideFieldByName(L, tableIndex, spec.canonicalName, wideValue) ||
                    (spec.aliasName && TryReadWideFieldByName(L, tableIndex, spec.aliasName, wideValue)))
                {
                    FieldValue value{};
                    value.name = spec.canonicalName;
                    value.type = FieldValue::Type::Number;
                    value.wide = true;
                    value.wideValue = wideValue;
                    outFields.push_back(value);
                    continue;
                }
            }
            else
            {
                std::int32_t numericValue = 0;
                if (TryReadIntFieldByName(L, tableIndex, spec.canonicalName, numericValue) ||
                    (spec.aliasName && TryReadIntFieldByName(L, tableIndex, spec.aliasName, numericValue)))
                {
                    FieldValue value{};
                    value.name = spec.canonicalName;
                    value.type = FieldValue::Type::Number;
                    value.numberValue = numericValue;
                    outFields.push_back(value);
                    continue;
                }
            }

            std::string stringValue;
            if (TryReadStringFieldByName(L, tableIndex, spec.canonicalName, stringValue) ||
                (spec.aliasName && TryReadStringFieldByName(L, tableIndex, spec.aliasName, stringValue)))
            {
                FieldValue value{};
                value.name = spec.canonicalName;
                value.type = FieldValue::Type::String;
                value.stringValue = std::move(stringValue);
                outFields.push_back(value);
                continue;
            }

            bool boolValue = false;
            if (TryReadBoolFieldByName(L, tableIndex, spec.canonicalName, boolValue) ||
                (spec.aliasName && TryReadBoolFieldByName(L, tableIndex, spec.aliasName, boolValue)))
            {
                FieldValue value{};
                value.name = spec.canonicalName;
                value.type = FieldValue::Type::Boolean;
                value.boolValue = boolValue;
                outFields.push_back(value);
                continue;
            }
        }
    }

    static void CollectKnownFields(
        lua_State* L,
        int tableIndex,
        const char* const* names,
        std::size_t nameCount,
        std::vector<FieldValue>& outFields)
    {
        outFields.clear();

        for (std::size_t i = 0; i < nameCount; ++i)
        {
            const char* fieldName = names[i];

            std::int32_t numericValue = 0;
            if (TryReadIntFieldByName(L, tableIndex, fieldName, numericValue))
            {
                FieldValue value{};
                value.name = fieldName;
                value.type = FieldValue::Type::Number;
                value.numberValue = numericValue;
                outFields.push_back(value);
                continue;
            }

            std::string stringValue;
            if (TryReadStringFieldByName(L, tableIndex, fieldName, stringValue))
            {
                FieldValue value{};
                value.name = fieldName;
                value.type = FieldValue::Type::String;
                value.stringValue = std::move(stringValue);
                outFields.push_back(value);
                continue;
            }
        }
    }

    static bool ReadKeyAndPayload(
        lua_State* L,
        std::string& outKey,
        std::vector<FieldValue>& outConstFields,
        std::vector<FieldValue>& outFlowFields,
        bool& outHasDynamicGate)
    {
        outKey.clear();
        outConstFields.clear();
        outFlowFields.clear();
        outHasDynamicGate = false;

        if (!EnsureLuaReady())
            return false;

        if (!IsLuaString(L, 1))
            return false;
        if (!IsLuaTable(L, 2))
            return false;

        {
            const char* keyValue = g_Deps.GetLuaString(L, 1);
            outKey = keyValue ? keyValue : "";
            if (outKey.empty())
                return false;
        }

        const int payloadIndex = 2;

        PushFieldKey(L, "const");
        g_Deps.LuaGetTable(L, payloadIndex);
        if (!IsLuaTable(L, -1))
        {
            g_Deps.LuaSetTop(L, -2);
            return false;
        }

        const int constTableIdx = g_Deps.GetLuaTop(L);
        CollectKnownFields(
            L,
            constTableIdx,
            k_ConstFieldSpecs,
            sizeof(k_ConstFieldSpecs) / sizeof(k_ConstFieldSpecs[0]),
            outConstFields);

        outHasDynamicGate = StoreGatePredicate(L, constTableIdx, outKey);

        g_Deps.LuaSetTop(L, -2);

        PushFieldKey(L, "flow");
        g_Deps.LuaGetTable(L, payloadIndex);
        if (!IsLuaTable(L, -1))
        {
            g_Deps.LuaSetTop(L, -2);
            return false;
        }

        CollectKnownFields(
            L,
            g_Deps.GetLuaTop(L),
            k_FlowFieldSpecs,
            sizeof(k_FlowFieldSpecs) / sizeof(k_FlowFieldSpecs[0]),
            outFlowFields);

        g_Deps.LuaSetTop(L, -2);
        return true;
    }

    static void BuildConstRowTable(
        lua_State* L,
        std::uint16_t developId,
        const std::vector<FieldValue>& fields)
    {
        g_Deps.LuaCreateTable(L, 0, static_cast<int>(fields.size() + 1));
        const int tableIndex = g_Deps.GetLuaTop(L);

        SetIntField(L, tableIndex, "p00", developId);

        for (const FieldValue& field : fields)
        {
            if (field.wide)
                SetWideField(L, tableIndex, field.name.c_str(), field.wideValue);
            else if (field.type == FieldValue::Type::Number)
                SetIntField(L, tableIndex, field.name.c_str(), field.numberValue);
            else if (field.type == FieldValue::Type::String)
                SetStringField(L, tableIndex, field.name.c_str(), field.stringValue);
        }
    }

    static void BuildFlowRowTable(
        lua_State* L,
        std::uint16_t flowIndex,
        const std::vector<FieldValue>& fields)
    {
        g_Deps.LuaCreateTable(L, 0, static_cast<int>(fields.size() + 1));
        const int tableIndex = g_Deps.GetLuaTop(L);

        SetIntField(L, tableIndex, "p50", flowIndex);

        for (const FieldValue& field : fields)
        {
            if (field.type == FieldValue::Type::Number)
                SetIntField(L, tableIndex, field.name.c_str(), field.numberValue);
            else if (field.type == FieldValue::Type::String)
                SetStringField(L, tableIndex, field.name.c_str(), field.stringValue);
        }
    }

    static bool FindFirstEmptyRecordIndex(void* controller,
                                          std::uint16_t& outIndex);
    static bool ReadRecordDevelopId(void* controller, std::uint16_t flowIndex,
                                    std::uint16_t& outDevelopId);
    static bool FindRecordIndexByDevelopId(void* controller,
                                           std::uint16_t developId,
                                           std::uint16_t& outIndex);

    static bool ShouldLogPark(const std::string& key)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        return g_MenuCapRefusalLogged.insert(key).second
            || g_SwapInProgress.load(std::memory_order_relaxed);
    }

    static bool InjectDevelopPairWithIds(
        lua_State* L,
        const PendingDevelopRequest& req,
        std::uint16_t developId,
        std::uint16_t& flowIndex)
    {
        if (!L || !EnsureLuaReady())
            return false;

        void* recController = EquipDevelop_ResolveDevelopController();
        if (!recController)
            return false;
        std::uint16_t appendIdx = 0;
        if (!FindFirstEmptyRecordIndex(recController, appendIdx))
        {
            if (ShouldLogPark(req.key))
                LogDebug("[EquipDevelop] PARKED key=%s: the develop record array "
                    "has no empty slot left - it will page into the R&D "
                    "window when one frees.\n", req.key.c_str());
            ParkKeyForPaging(req.key);
            return false;
        }
        if (appendIdx < kVanillaDevelopRowCount)
        {
            static std::atomic<std::uint32_t> s_HoleLogged{ 0xFFFFFFFFu };
            if (s_HoleLogged.exchange(appendIdx,
                                      std::memory_order_relaxed) != appendIdx)
                LogDebug("[EquipDevelop] inject held: first empty develop row %u "
                    "is inside the vanilla band (native table still "
                    "populating) - pending registrations retry on later "
                    "passes\n", appendIdx);
            return false;
        }

        std::int32_t devType = 0;
        std::uint32_t baseDevelopId = 0;
        for (const FieldValue& f : req.constFields)
        {
            if (f.type != FieldValue::Type::Number)
                continue;
            if (f.name == "p02")
                devType = f.numberValue;
            else if (f.name == "p03" && f.numberValue > 0)
                baseDevelopId = static_cast<std::uint32_t>(f.numberValue);
        }

        int finalGrade = kMinGrade;
        int finalSide  = 0;
        for (const FieldValue& f : req.flowFields)
        {
            if (f.type != FieldValue::Type::Number)
                continue;
            if (f.name == "p52")
                finalGrade = f.numberValue;
            else if (f.name == "p51")
                finalSide = f.numberValue;
        }
        if (finalGrade < kMinGrade) finalGrade = kMinGrade;
        if (finalGrade > kMaxGrade) finalGrade = kMaxGrade;
        if (finalSide < 0)              finalSide = 0;
        if (finalSide > kMaxSideGrade)  finalSide = kMaxSideGrade;

        int parentRowForDelta = -1;
        int finalOrdinal = 0;

        if (baseDevelopId == 0)
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            auto& rootSet = g_CustomRootsByType[devType];
            if (rootSet.find(developId) == rootSet.end())
            {
                const int nativeRoots = g_NativeRootsByType[devType];
                const int total =
                    nativeRoots + static_cast<int>(rootSet.size()) + 1;
                if (total > kMenuRootRenderCap)
                {
                    if (ShouldLogPark(req.key))
                        LogDebug("[EquipDevelop] PARKED key=%s: its R&D tab "
                            "(equipDevelopTypeID=%d) already holds %d top-level "
                            "rows of the menu's %d - it will page into the "
                            "develop window automatically on later menu opens.\n",
                            req.key.c_str(), devType,
                            nativeRoots + static_cast<int>(rootSet.size()),
                            kMenuRootRenderCap);
                    ParkKeyForPaging(req.key);
                    return false;
                }

                auto& rows = TabRenderRows_NoLock(devType);
                const std::uint64_t rowKey =
                    (static_cast<std::uint64_t>(developId) << 8)
                    | static_cast<std::uint64_t>(finalSide & 0xFF);
                if (rows.find(rowKey) == rows.end())
                {
                    if (static_cast<int>(rows.size()) >= kMenuRootRenderCap)
                    {
                        if (ShouldLogPark(req.key))
                            LogDebug("[EquipDevelop] PARKED key=%s: its R&D tab "
                                "(equipDevelopTypeID=%d) already renders %d "
                                "rows of the menu grid's %d - it will page "
                                "into the develop window automatically on "
                                "later menu opens.\n",
                                req.key.c_str(), devType,
                                static_cast<int>(rows.size()),
                                kMenuRootRenderCap);
                        ParkKeyForPaging(req.key);
                        return false;
                    }
                    rows.insert(rowKey);
                }
                rootSet.insert(developId);
            }
        }
        else
        {
            const FamilyInfo fam = CollectFamilyInfo(baseDevelopId);
            bool alreadyMember = false;
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                alreadyMember = g_ManagedBaseByDevelopId.find(developId)
                    != g_ManagedBaseByDevelopId.end();
            }
            if (!alreadyMember && fam.memberCount >= kMenuFamilyCap)
            {
                if (ShouldLogPark(req.key))
                    LogDebug("[EquipDevelop] PARKED key=%s: the develop chain of "
                        "baseEquipDevelopId=%u already has %d members (menu "
                        "ceiling %d per chain) - it will page into the develop "
                        "window automatically on later menu opens.\n",
                        req.key.c_str(), baseDevelopId, fam.memberCount,
                        kMenuFamilyCap - 1);
                ParkKeyForPaging(req.key);
                return false;
            }

            if (fam.parentGrade >= 1 && finalGrade <= fam.parentGrade)
            {
                if (fam.parentGrade >= kMaxGrade)
                {
                    if (ShouldLogPark(req.key))
                        LogDebug("[EquipDevelop] PARKED key=%s: parent (developId=%u) "
                            "is already grade %d - a child's grade must exceed "
                            "its parent's and no higher grade exists.\n",
                            req.key.c_str(), baseDevelopId, fam.parentGrade);
                    ParkKeyForPaging(req.key);
                    return false;
                }
                LogDebug("[EquipDevelop] key=%s: grade %d does not exceed parent "
                    "grade %d - raised to %d (chain grades must ascend).\n",
                    req.key.c_str(), finalGrade, fam.parentGrade,
                    fam.parentGrade + 1);
                finalGrade = fam.parentGrade + 1;
            }

            const int requestedGrade   = finalGrade;
            const int requestedOrdinal = finalSide;

            if (fam.parentGrade < 1)
            {
                std::size_t joinCount = 0, edgeCount = 0;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                    joinCount = g_NativeFlowByDevelopId.size();
                    edgeCount = g_NativeBaseByDevelopId.size();
                }
                if (ShouldLogPark(req.key))
                    LogDebug("[EquipDevelop] PARKED key=%s: parent developId=%u is "
                        "not resolvable yet (parent parked, or native develop "
                        "data not captured: flowJoin=%zu constEdges=%zu) - it "
                        "will page into the develop window once the parent is "
                        "present.\n",
                        req.key.c_str(), baseDevelopId, joinCount, edgeCount);
                ParkKeyForPaging(req.key);
                return false;
            }

            auto cellUsed = [&](int g, int s) -> bool
            {
                if (g < 1 || g > 15 || s < 0 || s > 15) return false;
                return (fam.sideUsedAtGrade[g]
                        & static_cast<std::uint16_t>(1u << s)) != 0;
            };

            int col0 = requestedGrade;
            if (col0 <= fam.parentGrade) col0 = fam.parentGrade + 1;
            if (col0 > kMaxGrade) col0 = kMaxGrade;

            bool placed = false;
            if (requestedOrdinal == 0)
            {
                bool parentIsRowTip = true;
                for (int g = fam.parentGrade + 1; g <= kMaxGrade; ++g)
                {
                    if (fam.sideUsedAtGrade[g]
                        & static_cast<std::uint16_t>(1u << fam.parentSide))
                    {
                        parentIsRowTip = false;
                        break;
                    }
                }
                if (parentIsRowTip && !cellUsed(col0, fam.parentSide))
                {
                    finalGrade   = col0;
                    finalSide    = fam.parentSide;
                    finalOrdinal = 0;
                    placed       = true;
                }
            }

            if (!placed)
            {
                int maxUsed = 0;
                for (int b = 1; b <= kMaxSideGrade; ++b)
                    if ((fam.childOrdinalMask >> b) & 1)
                        maxUsed = b;
                int ordinal = maxUsed + 1;
                if (requestedOrdinal > maxUsed)
                    ordinal = requestedOrdinal;
                else if (requestedOrdinal >= 1 && ordinal <= kMaxSideGrade)
                    LogDebug("[EquipDevelop] key=%s: requested branch %d under "
                        "baseEquipDevelopId=%u is at or below an existing "
                        "branch - moved to branch %d.\n",
                        req.key.c_str(), requestedOrdinal, baseDevelopId,
                        ordinal);
                if (ordinal > kMaxSideGrade)
                {
                    if (ShouldLogPark(req.key))
                        LogDebug("[EquipDevelop] PARKED key=%s: parent developId=%u "
                            "already carries %d branch chains (the record packs "
                            "the branch ordinal in 3 bits) - it will page into "
                            "the develop window when one frees.\n",
                            req.key.c_str(), baseDevelopId, kMaxSideGrade);
                    ParkKeyForPaging(req.key);
                    return false;
                }
                int row = fam.parentSide + 1 + fam.forkSpanPrefix[ordinal];
                if (row > 15) row = 15;
                if (cellUsed(col0, row))
                {
                    if (ShouldLogPark(req.key))
                        LogDebug("[EquipDevelop] PARKED key=%s: branch %d under "
                            "baseEquipDevelopId=%u computes to an occupied "
                            "cell (grade %d, row %d) - family data is "
                            "irregular; it will page into the develop window "
                            "on later menu opens.\n",
                            req.key.c_str(), ordinal, baseDevelopId,
                            col0, row);
                    ParkKeyForPaging(req.key);
                    return false;
                }
                finalGrade   = col0;
                finalSide    = row;
                finalOrdinal = ordinal;
                placed       = true;
            }

            const bool moved = (finalGrade != requestedGrade)
                || (finalOrdinal != requestedOrdinal);
            parentRowForDelta = fam.parentSide;

            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                auto& rows = TabRenderRows_NoLock(devType);
                const std::uint64_t rowKey =
                    (static_cast<std::uint64_t>(fam.rootDevelopId) << 8)
                    | static_cast<std::uint64_t>(finalSide & 0xFF);
                if (rows.find(rowKey) == rows.end())
                {
                    if (static_cast<int>(rows.size()) >= kMenuRootRenderCap)
                    {
                        if (ShouldLogPark(req.key))
                            LogDebug("[EquipDevelop] PARKED key=%s: its R&D tab "
                                "(equipDevelopTypeID=%d) already renders %d "
                                "rows of the menu grid's %d and this item "
                                "would open a new branch row - it will page "
                                "into the develop window automatically on "
                                "later menu opens.\n",
                                req.key.c_str(), devType,
                                static_cast<int>(rows.size()),
                                kMenuRootRenderCap);
                        ParkKeyForPaging(req.key);
                        return false;
                    }
                    rows.insert(rowKey);
                }
            }

            if (moved)
            {
                LogDebug("[EquipDevelop] key=%s: placed under baseEquipDevelopId=%u "
                    "at grade %d (column), branch %d (row %d); parent grade %d, "
                    "requested grade %d.\n",
                    req.key.c_str(), baseDevelopId, finalGrade, finalOrdinal,
                    finalSide, fam.parentGrade, requestedGrade);
            }
        }

        const int persistedP51 =
            (parentRowForDelta >= 0) ? finalOrdinal : finalSide;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            PersistPlacementInRequest_NoLock(req.key, finalGrade, persistedP51);
        }

        if (appendIdx != flowIndex)
        {
            if (flowIndex != 0)
                LogDebug("[EquipDevelop] key=%s: develop record lands at row %u "
                    "(bookkept row was %u) - flow row follows the record and "
                    "the bookkeeping is re-synced.\n",
                    req.key.c_str(), appendIdx, flowIndex);
            flowIndex = appendIdx;
        }

        g_Deps.LuaSetTop(L, 0);
        BuildConstRowTable(L, developId, req.constFields);
        g_OrigRegCstDev(L);
        ObserveDevelopId(developId);

        for (const FieldValue& f : req.constFields)
        {
            if (f.name != "p06")
                continue;
            DevelopNameLangId nm{};
            if (f.type == FieldValue::Type::Number)
            {
                nm.hasHash = true;
                nm.hash = static_cast<std::uint32_t>(f.numberValue);
            }
            else
            {
                nm.str = f.stringValue;
            }
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            g_DevelopNameByDevelopId[static_cast<std::int32_t>(developId)] = std::move(nm);
            break;
        }

        EquipDevelop_SetDevelopParent(developId, baseDevelopId);

        bool defaultDeveloped = false;
        for (const FieldValue& f : req.flowFields)
            if (f.type == FieldValue::Type::Number && f.name == "p62")
                defaultDeveloped = (f.numberValue != 0);
        EquipDevelop_SetDevelopInitiallyAvailable(developId, defaultDeveloped);

        const bool developed =
            V_FrameWorkState::ResolveDevelopedFlag(req.key.c_str(), defaultDeveloped);

        g_Deps.LuaSetTop(L, 0);
        BuildFlowRowTable(L, flowIndex, req.flowFields);

        const int flowRowIndex = g_Deps.GetLuaTop(L);
        SetIntField(L, flowRowIndex, "p62", 0);
        SetIntField(L, flowRowIndex, "p72", 0);
        SetIntField(L, flowRowIndex, "p74", 0);

        int storedP51 = persistedP51;
        if (storedP51 < 0)             storedP51 = 0;
        if (storedP51 > kMaxSideGrade) storedP51 = kMaxSideGrade;
        SetIntField(L, flowRowIndex, "p51", storedP51);
        SetIntField(L, flowRowIndex, "p52", finalGrade);

        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            g_GradeByDevelopId[developId]       = finalGrade;
            g_SideByDevelopId[developId]        = finalSide;
            g_OrdinalByDevelopId[developId]     = storedP51;
            g_ManagedBaseByDevelopId[developId] = baseDevelopId;
        }

        g_OrigRegFlwDev(L);
        ObserveFlowIndex(flowIndex);

        {
            std::uint16_t landedDev = 0;
            if (ReadRecordDevelopId(recController, flowIndex, landedDev)
                && landedDev != developId)
            {
                std::uint16_t actualIdx = 0;
                if (FindRecordIndexByDevelopId(recController, developId,
                                               actualIdx))
                {
                    Log("[EquipDevelop] WARN key=%s: develop record landed at "
                        "row %u instead of the bookkept row %u - bookkeeping "
                        "resynced to the real row\n",
                        req.key.c_str(), actualIdx, flowIndex);
                    flowIndex = actualIdx;
                }
                else
                {
                    Log("[EquipDevelop] WARN key=%s: develop record not found "
                        "after registration (row %u holds developId %u) - the "
                        "native register landed it on the reserved sentinel "
                        "row; the key stays pending and retries later\n",
                        req.key.c_str(), flowIndex, landedDev);
                    g_Deps.LuaSetTop(L, 0);
                    return false;
                }
            }
        }

        if (developed)
            outfit::MarkDeveloped(flowIndex);

        if (req.hasDynamicGate)
        {
            RegisterDynamicGate(flowIndex, req.key);
            outfit::SetDevelopHidden(flowIndex, true);
            RefreshDynamicGatesImpl();
#ifdef _DEBUG
            LogDebug("[EquipDevelop] bluePrintId dynamic gate key=%s flowIndex=%u "
                "(predicate re-evaluated each R&D menu build)\n",
                req.key.c_str(), flowIndex);
#endif
        }
        else
        {
            for (const FieldValue& f : req.constFields)
            {
                if (f.type == FieldValue::Type::Boolean && f.name == "p05")
                {
                    outfit::SetDevelopHidden(flowIndex, !f.boolValue);
#ifdef _DEBUG
                    LogDebug("[EquipDevelop] bluePrintId gate key=%s flowIndex=%u -> %s\n",
                        req.key.c_str(), flowIndex,
                        f.boolValue ? "VISIBLE" : "HIDDEN");
#endif
                    break;
                }
            }
        }

        g_Deps.LuaSetTop(L, 0);

        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            auto itReg = g_KeyRegistry.find(req.key);
            if (itReg != g_KeyRegistry.end())
                itReg->second.flowIndex = flowIndex;
            if (static_cast<std::uint32_t>(flowIndex) >= g_NextFlowIndex)
                g_NextFlowIndex = static_cast<std::uint32_t>(flowIndex) + 1;
        }
        V_FrameWorkState::SetSessionFlowIndex(req.key.c_str(), flowIndex);
        outfit::SetOutfitFlowIndexByDevelopId(developId, flowIndex);

        return true;
    }

    static bool InjectReservedDevelopPair(
        lua_State* L,
        const PendingDevelopRequest& req,
        std::uint16_t* outDevelopId = nullptr,
        std::uint16_t* outFlowIndex = nullptr)
    {
        std::uint16_t developId = 0;
        std::uint16_t flowIndex = 0;
        if (!TryGetIdsForKey(req.key, developId, flowIndex))
            return false;

        if (!InjectDevelopPairWithIds(L, req, developId, flowIndex))
            return false;

        outfit::SetOutfitFlowIndexByDevelopId(developId, flowIndex);

        if (outDevelopId)
            *outDevelopId = developId;
        if (outFlowIndex)
            *outFlowIndex = flowIndex;

        return true;
    }

    static void FlushPendingRegistrations(lua_State* L)
    {
        if (!L || !EnsureLuaReady())
            return;

        std::vector<PendingDevelopRequest> work;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

            if (g_IsFlushingPending)
                return;

            if (!CanInjectRowsNow_NoLock())
            {
                static std::atomic<bool> s_StallLogged{ false };
                if (!g_PendingRequests.empty()
                    && !s_StallLogged.exchange(true,
                                               std::memory_order_relaxed))
                    LogDebug("[EquipDevelop] %zu pending develop registration(s) "
                        "held: rows cannot be injected yet (native table "
                        "still populating or no room below the custom "
                        "band)\n", g_PendingRequests.size());
                return;
            }

            if (g_PendingRequests.empty())
                return;

            g_IsFlushingPending = true;
            work = g_PendingRequests;
        }

        std::size_t developedCount = 0;
        std::stable_partition(
            work.begin(), work.end(),
            [&developedCount](const PendingDevelopRequest& req)
            {
                const std::int32_t dev =
                    V_FrameWorkState::GetDevelopIdByKey(req.key.c_str());
                const bool developed = dev > 0
                    && V_FrameWorkState::GetDevelopedByDevelopId(dev);
                if (developed)
                    ++developedCount;
                return developed;
            });
        if (developedCount > 102)
            Log("[EquipDevelop] WARNING: %zu DEVELOPED custom items exceed "
                "the 102-slot record band - the overflow will page and lose "
                "equip-list visibility while parked.\n", developedCount);

        std::vector<std::string> completedKeys;

        for (const PendingDevelopRequest& req : work)
        {
            std::uint16_t developId = 0;
            std::uint16_t flowIndex = 0;

            if (InjectReservedDevelopPair(L, req, &developId, &flowIndex))
                completedKeys.push_back(req.key);
        }

        std::size_t leftover = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

            if (!completedKeys.empty())
            {
                g_PendingRequests.erase(
                    std::remove_if(
                        g_PendingRequests.begin(),
                        g_PendingRequests.end(),
                        [&](const PendingDevelopRequest& req)
                        {
                            return std::find(
                                completedKeys.begin(),
                                completedKeys.end(),
                                req.key) != completedKeys.end();
                        }),
                    g_PendingRequests.end());
            }

            leftover = g_PendingRequests.size();
            g_AnyPendingReg.store(leftover != 0, std::memory_order_relaxed);
            g_IsFlushingPending = false;
        }

        static std::atomic<std::size_t> s_LeftoverLogged{ 0 };
        if (leftover == 0)
        {
            s_LeftoverLogged.store(0, std::memory_order_relaxed);
        }
        else if (s_LeftoverLogged.exchange(leftover,
                                           std::memory_order_relaxed)
                 != leftover)
        {
            std::uint16_t firstEmpty = 0xFFFF;
            void* controller = EquipDevelop_ResolveDevelopController();
            if (!controller
                || !FindFirstEmptyRecordIndex(controller, firstEmpty))
                firstEmpty = 0xFFFF;
            LogDebug("[EquipDevelop] %zu develop registration(s) still pending "
                "after a flush pass (first empty row %u; 0xFFFF = none) - "
                "they retry on later registrations and menu opens\n",
                leftover, firstEmpty);
        }
    }

    static void __fastcall hkRegCstDev(lua_State* L)
    {
        if (L && EnsureLuaReady() && IsLuaTable(L, 1))
        {
            std::int32_t developId = 0;
            if (TryReadIntFieldByName(L, 1, "p00", developId) && developId >= 0)
            {
                ObserveDevelopId(static_cast<std::uint32_t>(developId));

                std::int32_t base = 0;
                TryReadIntFieldByName(L, 1, "p03", base);
                std::int32_t devType = 0;
                TryReadIntFieldByName(L, 1, "p02", devType);
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                const bool firstSeen =
                    g_NativeBaseByDevelopId.find(static_cast<std::uint32_t>(developId))
                    == g_NativeBaseByDevelopId.end();
                g_NativeBaseByDevelopId[static_cast<std::uint32_t>(developId)] =
                    (base > 0) ? static_cast<std::uint32_t>(base) : 0u;
                g_NativeTypeByDevelopId[static_cast<std::uint32_t>(developId)] =
                    devType;
                if (firstSeen)
                {
                    g_NativeOrderDevelopId[g_NativeConstOrder++] =
                        static_cast<std::uint32_t>(developId);
                    if (base <= 0)
                        ++g_NativeRootsByType[devType];
                }
            }
        }

        if (g_OrigRegCstDev)
            g_OrigRegCstDev(L);
        equip::InvalidateDevelopLookupIndex();

        FlushPendingRegistrations(L);
    }

    static void __fastcall hkRegFlwDev(lua_State* L)
    {
        if (L && EnsureLuaReady() && IsLuaTable(L, 1))
        {
            std::int32_t flowIndex = 0;
            if (TryReadIntFieldByName(L, 1, "p50", flowIndex) && flowIndex >= 0)
            {
                ObserveFlowIndex(static_cast<std::uint32_t>(flowIndex));

                if (flowIndex < static_cast<std::int32_t>(kNativeFlowCap))
                {
                    std::int32_t sg = 0, gr = 0;
                    TryReadIntFieldByName(L, 1, "p51", sg);
                    TryReadIntFieldByName(L, 1, "p52", gr);
                    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                    NativeFlowInfo& nf = g_NativeFlow[flowIndex];
                    nf.grade     = static_cast<std::uint8_t>(
                        (gr < 0) ? 0 : (gr > 15 ? 15 : gr));
                    nf.sideGrade = static_cast<std::uint8_t>(
                        (sg < 0) ? 0 : (sg > 15 ? 15 : sg));
                    nf.seen      = true;

                    auto itOrd = g_NativeOrderDevelopId.find(
                        static_cast<std::uint32_t>(flowIndex));
                    if (itOrd != g_NativeOrderDevelopId.end())
                        g_NativeFlowByDevelopId[itOrd->second] = nf;
                }

                if (EquipDevelopAdd::IsManagedFlowIndex(
                        static_cast<std::uint16_t>(flowIndex)))
                {
                    SetIntField(L, 1, "p74", 0);
#ifdef _DEBUG
                    static int s_fobLog = 0;
                    if (s_fobLog < 8)
                    {
                        ++s_fobLog;
                        LogDebug("[EquipDevelop] hkRegFlwDev: forced isFobAvailable=0 "
                            "(p74) on managed flow row flowIndex=%d\n", flowIndex);
                    }
#endif
                }
            }
        }

        if (g_OrigRegFlwDev)
            g_OrigRegFlwDev(L);
        equip::InvalidateDevelopLookupIndex();

        FlushPendingRegistrations(L);
    }

    std::unordered_set<std::uint32_t> g_ParkedDevelopIds;

    static bool ZeroDevelopRecord(void* controller, std::uint16_t flowIndex)
    {
        __try
        {
            std::uint8_t* rec = reinterpret_cast<std::uint8_t*>(controller)
                + kDevelopRecordArrayOffset
                + static_cast<std::size_t>(flowIndex) * kDevelopRecordStride;
            for (std::size_t i = 0; i < kDevelopRecordStride; ++i)
                rec[i] = 0;
            equip::InvalidateDevelopLookupIndex();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ReadRecordDevelopId(void* controller, std::uint16_t flowIndex,
                                    std::uint16_t& outDevelopId)
    {
        __try
        {
            const std::uint8_t* rec =
                reinterpret_cast<const std::uint8_t*>(controller)
                + kDevelopRecordArrayOffset
                + static_cast<std::size_t>(flowIndex) * kDevelopRecordStride;
            outDevelopId = *reinterpret_cast<const std::uint16_t*>(rec);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool FindFirstEmptyRecordIndex(void* controller,
                                          std::uint16_t& outIndex)
    {
        for (std::uint32_t i = 0; i < equip::NativeFlowBound(); ++i)
        {
            if (equip::IsReservedFlowRow(i))
                continue;
            std::uint16_t dev = 0;
            if (!ReadRecordDevelopId(controller,
                                     static_cast<std::uint16_t>(i), dev))
                return false;
            if (dev == 0)
            {
                outIndex = static_cast<std::uint16_t>(i);
                return true;
            }
        }
        return false;
    }

    static bool FindRecordIndexByDevelopId(void* controller,
                                           std::uint16_t developId,
                                           std::uint16_t& outIndex)
    {
        if (developId == 0)
            return false;
        for (std::uint32_t i = 0; i < equip::NativeFlowBound(); ++i)
        {
            if (equip::IsReservedFlowRow(i))
                continue;
            std::uint16_t dev = 0;
            if (!ReadRecordDevelopId(controller,
                                     static_cast<std::uint16_t>(i), dev))
                return false;
            if (dev == developId)
            {
                outIndex = static_cast<std::uint16_t>(i);
                return true;
            }
        }
        return false;
    }

    static void RemoveDynamicGatesForFlowIndex(std::uint16_t flowIndex)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        for (auto it = g_DynamicGates.begin(); it != g_DynamicGates.end();)
        {
            if (it->flowIndex == flowIndex)
                it = g_DynamicGates.erase(it);
            else
                ++it;
        }
        g_AnyDynamicGate.store(!g_DynamicGates.empty());
    }

    static void ForgetCustomRootMembership(std::uint32_t developId)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        for (auto& kv : g_CustomRootsByType)
            kv.second.erase(developId);
    }
}

namespace EquipDevelopAdd
{
    void Bind(const Deps& deps)
    {
        g_Deps = deps;
    }

    int __cdecl Lua_AddToEquipDevelopTable(lua_State* L)
    {
        V_FrameWorkState::SaveBatch _saveBatch;

        std::string key;
        std::vector<FieldValue> constFields;
        std::vector<FieldValue> flowFields;
        bool hasDynamicGate = false;

        if (!ReadKeyAndPayload(L, key, constFields, flowFields, hasDynamicGate))
        {
            LogDebug("[EquipDevelop] Invalid AddToEquipDevelopTable call\n");
            return 0;
        }

        std::uint16_t developId = 0;
        std::uint16_t flowIndex = 0;
        bool created = false;

        if (!GetOrCreateIdsForKey(key, developId, flowIndex, created))
        {
            std::int32_t dev32 = 0;
            const bool haveDev = V_FrameWorkState::ResolveOrCreateDevelopId(
                key.c_str(), static_cast<std::int32_t>(g_NextDevelopId), dev32)
                && dev32 > 0 && dev32 <= static_cast<std::int32_t>(kMaxAllocId);
            if (!haveDev)
            {
                Log("[EquipDevelop] Failed to reserve ids for key=%s\n",
                    key.c_str());
                return 0;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                if (static_cast<std::uint32_t>(dev32) >= g_NextDevelopId)
                    g_NextDevelopId = static_cast<std::uint32_t>(dev32) + 1;
                g_RequestByKey[key] = PendingDevelopRequest{
                    key, constFields, flowFields, hasDynamicGate };
                g_ParkedKeys.insert(key);
                g_AnyParkedKeys.store(true, std::memory_order_relaxed);
            }
            V_FrameWorkState::ReleaseSessionFlowIndex(key.c_str());
            RequestRotationIfBandHasRoom();
            PublishSummaryDisplay(
                static_cast<std::uint16_t>(dev32), constFields);
            if (g_MenuCapRefusalLogged.insert(key).second)
                LogDebug("[EquipDevelop] PARKED key=%s (developId %d): no free "
                    "develop record slot right now - it will page into the "
                    "R&D window automatically on later menu opens.\n",
                    key.c_str(), dev32);
            g_Deps.PushLuaNumber(L, static_cast<float>(dev32));
            return 1;
        }


        if (!created)
        {
            bool stillPending = false;
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                stillPending = (FindPendingRequest_NoLock(key) != nullptr);
            }

            if (!stillPending)
            {
                g_Deps.PushLuaNumber(L, static_cast<float>(developId));
                return 1;
            }
        }


        QueueOrUpdatePendingRequest(key, constFields, flowFields, hasDynamicGate);

        PublishSummaryDisplay(developId, constFields);

        FlushPendingRegistrations(L);

        g_Deps.PushLuaNumber(L, static_cast<float>(developId));
        return 1;
    }

    bool IsDevelopIdParked(std::uint32_t developId)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        return g_ParkedDevelopIds.find(developId) != g_ParkedDevelopIds.end();
    }

    struct SwapScope
    {
        SwapScope()  { g_SwapInProgress.store(true,  std::memory_order_relaxed); }
        ~SwapScope() { g_SwapInProgress.store(false, std::memory_order_relaxed); }
    };

    static bool HasResidentChildren_NoLock(std::uint32_t developId);

    static bool SwapDevelopRowCore(
        lua_State* L, const std::string& outKey, const std::string& inKey)
    {
        SwapScope _swapScope;
        std::uint16_t outDev = 0, outIdx = 0;
        PendingDevelopRequest inReq;
        PendingDevelopRequest outReq;
        bool haveOutReq = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            auto itOut = g_KeyRegistry.find(outKey);
            if (itOut == g_KeyRegistry.end() || itOut->second.flowIndex == 0)
            {
                LogDebug("[EquipDevelop] SwapDevelopRow refused: outKey=%s does not "
                    "own a live develop row.\n", outKey.c_str());
                return false;
            }
            if (g_GradeByDevelopId.find(itOut->second.developId)
                == g_GradeByDevelopId.end())
            {
                LogDebug("[EquipDevelop] SwapDevelopRow refused: outKey=%s has a "
                    "reserved slot but no materialized record - not "
                    "swappable.\n", outKey.c_str());
                return false;
            }
            auto itIn = g_KeyRegistry.find(inKey);
            if (itIn != g_KeyRegistry.end() && itIn->second.flowIndex != 0)
            {
                LogDebug("[EquipDevelop] SwapDevelopRow refused: inKey=%s already "
                    "owns a live develop row.\n", inKey.c_str());
                return false;
            }
            auto itReq = g_RequestByKey.find(inKey);
            if (itReq == g_RequestByKey.end())
            {
                LogDebug("[EquipDevelop] SwapDevelopRow refused: inKey=%s has no "
                    "retained registration (it must have called "
                    "AddToEquipDevelopTable this session).\n", inKey.c_str());
                return false;
            }
            outDev = itOut->second.developId;
            outIdx = itOut->second.flowIndex;
            inReq  = itReq->second;
            auto itOutReq = g_RequestByKey.find(outKey);
            if (itOutReq != g_RequestByKey.end())
            {
                outReq = itOutReq->second;
                haveOutReq = true;
            }
        }

        std::int32_t inDev32 = 0;
        if (!V_FrameWorkState::ResolveOrCreateDevelopId(
                inKey.c_str(), static_cast<std::int32_t>(g_NextDevelopId),
                inDev32) ||
            inDev32 <= 0 || inDev32 > static_cast<std::int32_t>(kMaxAllocId))
        {
            LogDebug("[EquipDevelop] SwapDevelopRow refused: could not resolve a "
                "developId for inKey=%s.\n", inKey.c_str());
            return false;
        }
        const std::uint16_t inDev = static_cast<std::uint16_t>(inDev32);

        void* controller = EquipDevelop_ResolveDevelopController();
        if (!controller)
        {
            LogDebug("[EquipDevelop] SwapDevelopRow refused: develop controller not "
                "resolved (game not booted?).\n");
            return false;
        }

        {
            std::uint16_t trueIdx = 0;
            if (!FindRecordIndexByDevelopId(controller, outDev, trueIdx))
            {
                LogDebug("[EquipDevelop] SwapDevelopRow: outKey=%s (developId %u) "
                    "has no develop record in the band despite being marked "
                    "live at row %u - healing it to parked.\n",
                    outKey.c_str(), outDev, outIdx);
                RemoveDynamicGatesForFlowIndex(outIdx);
                ForgetCustomRootMembership(outDev);
                {
                    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                    g_GradeByDevelopId.erase(outDev);
                    g_SideByDevelopId.erase(outDev);
                    g_OrdinalByDevelopId.erase(outDev);
                    g_ManagedBaseByDevelopId.erase(outDev);
                    g_KeyRegistry[outKey] = DevelopKeyIds{ outDev, 0 };
                    g_ParkedDevelopIds.insert(outDev);
                    g_ParkedKeys.insert(outKey);
                    g_AnyParkedKeys.store(true, std::memory_order_relaxed);
                    RebuildAllSeededTabRenderRows_NoLock();
                }
                EquipDevelop_SetDevelopParent(outDev, 0);
                outfit::SetOutfitFlowIndexByDevelopId(outDev, 0);
                V_FrameWorkState::ReleaseSessionFlowIndex(outKey.c_str());
                return false;
            }
            if (trueIdx != outIdx)
            {
                LogDebug("[EquipDevelop] SwapDevelopRow: outKey=%s record sits at "
                    "row %u (bookkeeping said %u) - using the real row.\n",
                    outKey.c_str(), trueIdx, outIdx);
                outIdx = trueIdx;
            }
        }

        if (EquipDevelop_IsDevelopTimerActive(outIdx))
        {
            LogDebug("[EquipDevelop] SwapDevelopRow refused: an R&D development is "
                "in flight on outKey=%s (flow row %u) - complete or cancel it "
                "first.\n", outKey.c_str(), outIdx);
            return false;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            if (HasResidentChildren_NoLock(outDev))
            {
                LogDebug("[EquipDevelop] SwapDevelopRow refused: outKey=%s "
                    "(developId %u) still has live chain members under it - "
                    "page those out first.\n", outKey.c_str(), outDev);
                return false;
            }
        }

        if (!ZeroDevelopRecord(controller, outIdx))
        {
            Log("[EquipDevelop] SwapDevelopRow FAILED: fault while clearing "
                "record %u - nothing changed.\n", outIdx);
            return false;
        }

        RemoveDynamicGatesForFlowIndex(outIdx);
        outfit::SetDevelopHidden(outIdx, false);
        ForgetCustomRootMembership(outDev);
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            g_GradeByDevelopId.erase(outDev);
            g_SideByDevelopId.erase(outDev);
            g_OrdinalByDevelopId.erase(outDev);
            g_ManagedBaseByDevelopId.erase(outDev);
            g_KeyRegistry[outKey] = DevelopKeyIds{ outDev, 0 };
            g_ParkedDevelopIds.insert(outDev);
            g_ParkedDevelopIds.erase(inDev);
            g_KeyRegistry[inKey] = DevelopKeyIds{ inDev, outIdx };
            g_ParkedKeys.insert(outKey);
            RebuildAllSeededTabRenderRows_NoLock();
        }
        EquipDevelop_SetDevelopParent(outDev, 0);
        outfit::SetOutfitFlowIndexByDevelopId(outDev, 0);

        if (!InjectDevelopPairWithIds(L, inReq, inDev, outIdx))
        {
            Log("[EquipDevelop] SwapDevelopRow: inKey=%s injection failed - "
                "rolling the slot back to outKey=%s.\n",
                inKey.c_str(), outKey.c_str());
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                g_KeyRegistry[inKey] = DevelopKeyIds{ inDev, 0 };
                g_ParkedDevelopIds.insert(inDev);
                g_ParkedKeys.insert(inKey);
                g_AnyParkedKeys.store(true, std::memory_order_relaxed);
            }
            V_FrameWorkState::ReleaseSessionFlowIndex(inKey.c_str());
            bool rolledBack = false;
            if (haveOutReq && ZeroDevelopRecord(controller, outIdx))
            {
                {
                    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                    g_ParkedDevelopIds.erase(outDev);
                    g_KeyRegistry[outKey] = DevelopKeyIds{ outDev, outIdx };
                }
                rolledBack = InjectDevelopPairWithIds(L, outReq, outDev, outIdx);
            }
            if (rolledBack)
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                g_ParkedKeys.erase(outKey);
                outfit::SetOutfitFlowIndexByDevelopId(outDev, outIdx);
            }
            else
                LogDebug("[EquipDevelop] SwapDevelopRow CRITICAL: slot %u is now "
                    "EMPTY mid-band - custom rows above it will not show until "
                    "the game is restarted.\n", outIdx);
            return false;
        }

        {
            std::uint16_t landed = 0;
            if (ReadRecordDevelopId(controller, outIdx, landed)
                && landed != inDev)
                LogDebug("[EquipDevelop] SwapDevelopRow CRITICAL: slot %u holds "
                    "developId %u after injecting %u - the const registration "
                    "landed in a different slot (an empty record exists below "
                    "%u). Restart the game before developing anything.\n",
                    outIdx, landed, inDev, outIdx);
        }

        V_FrameWorkState::ReleaseSessionFlowIndex(outKey.c_str());

        if (V_FrameWorkState::GetDevelopedByDevelopId(inDev32))
            EquipDevelop_DevelopByDevelopId(static_cast<std::uint32_t>(inDev));
        else
            EquipDevelop_UndevelopByDevelopId(static_cast<std::uint32_t>(inDev));

        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            g_ParkedKeys.erase(inKey);
            g_AnyParkedKeys.store(!g_ParkedKeys.empty(),
                                  std::memory_order_relaxed);
        }

        LogDebug("[EquipDevelop] SwapDevelopRow: flow row %u now holds key=%s "
            "(developId %u); key=%s (developId %u) parked.\n",
            outIdx, inKey.c_str(), inDev, outKey.c_str(), outDev);

        return true;
    }

    static bool DirectMaterializeCore(lua_State* L, const std::string& inKey)
    {
        SwapScope _swapScope;
        PendingDevelopRequest inReq;
        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            auto itIn = g_KeyRegistry.find(inKey);
            if (itIn != g_KeyRegistry.end() && itIn->second.flowIndex != 0)
                return false;
            auto itReq = g_RequestByKey.find(inKey);
            if (itReq == g_RequestByKey.end())
                return false;
            inReq = itReq->second;
        }

        void* controller = EquipDevelop_ResolveDevelopController();
        if (!controller)
            return false;
        std::uint16_t freeIdx = 0;
        if (!FindFirstEmptyRecordIndex(controller, freeIdx)
            || freeIdx < kVanillaDevelopRowCount)
            return false;

        std::int32_t inDev32 = 0;
        if (!V_FrameWorkState::ResolveOrCreateDevelopId(
                inKey.c_str(), static_cast<std::int32_t>(g_NextDevelopId),
                inDev32) ||
            inDev32 <= 0 || inDev32 > static_cast<std::int32_t>(kMaxAllocId))
            return false;
        const std::uint16_t inDev = static_cast<std::uint16_t>(inDev32);

        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            g_KeyRegistry[inKey] = DevelopKeyIds{ inDev, 0 };
            g_ParkedDevelopIds.erase(inDev);
        }

        std::uint16_t idx = freeIdx;
        if (!InjectDevelopPairWithIds(L, inReq, inDev, idx))
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                g_ParkedDevelopIds.insert(inDev);
            }
            V_FrameWorkState::ReleaseSessionFlowIndex(inKey.c_str());
            return false;
        }

        if (V_FrameWorkState::GetDevelopedByDevelopId(inDev32))
            EquipDevelop_DevelopByDevelopId(static_cast<std::uint32_t>(inDev));

        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            g_ParkedKeys.erase(inKey);
            g_AnyParkedKeys.store(!g_ParkedKeys.empty(),
                                  std::memory_order_relaxed);
        }

        LogDebug("[EquipDevelop] develop row materialized into free slot %u: "
            "key=%s (developId %u).\n", idx, inKey.c_str(), inDev);

        return true;
    }

    static std::int32_t RequestConstNumber(const PendingDevelopRequest& req,
                                           const char* name)
    {
        for (const FieldValue& f : req.constFields)
            if (f.type == FieldValue::Type::Number && f.name == name)
                return f.numberValue;
        return 0;
    }

    static bool ParentReadyForRequest_NoLock(const PendingDevelopRequest& req)
    {
        const std::int32_t base = RequestConstNumber(req, "p03");
        if (base <= 0)
            return true;
        const std::uint32_t b = static_cast<std::uint32_t>(base);
        if (g_GradeByDevelopId.find(b) != g_GradeByDevelopId.end())
            return true;
        if (g_NativeBaseByDevelopId.find(b) == g_NativeBaseByDevelopId.end())
            return false;
        int parentGrade = 0;
        return TryGetNativeFlowInfo(b, &parentGrade, nullptr)
            && parentGrade >= 1;
    }

    static bool HasResidentChildren_NoLock(std::uint32_t developId)
    {
        for (const auto& kv : g_ManagedBaseByDevelopId)
            if (kv.second == developId)
                return true;
        return false;
    }

    static std::int32_t RequestFlowNumber(const PendingDevelopRequest& req,
                                          const char* name)
    {
        for (const FieldValue& f : req.flowFields)
            if (f.type == FieldValue::Type::Number && f.name == name)
                return f.numberValue;
        return 0;
    }

    static std::uint32_t FamilyRootForRequest_NoLock(
        const PendingDevelopRequest& req,
        const std::unordered_map<std::uint32_t, const PendingDevelopRequest*>&
            reqByDev,
        std::uint32_t selfDev)
    {
        std::uint32_t cur = selfDev;
        std::int32_t base = RequestConstNumber(req, "p03");
        for (int hop = 0; hop < 64 && base > 0; ++hop)
        {
            cur = static_cast<std::uint32_t>(base);
            auto itR = reqByDev.find(cur);
            if (itR != reqByDev.end())
            {
                base = RequestConstNumber(*itR->second, "p03");
                continue;
            }
            return WalkNativeRoot_NoLock(cur);
        }
        return cur;
    }

    static void RotateDevelopWindow(lua_State* L)
    {
        V_FrameWorkState::SaveBatch _batch;

        int swapped = 0;
        std::size_t parkedLeft = 0;
        std::uint32_t stickyInRoot = 0;
        std::unordered_set<std::string> rotatedIn, rotatedOut;
        for (int i = 0; i < kMaxSwapsPerRotation; ++i)
        {
            std::string inKey, outKey;
            int inType = 0;
            std::unordered_set<std::uint32_t> readyRoots;
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                if (g_ParkedKeys.empty())
                    break;

                std::unordered_map<std::uint32_t,
                                   const PendingDevelopRequest*> reqByDev;
                std::unordered_map<std::string, std::uint32_t> devByKey;
                for (const auto& kv : g_RequestByKey)
                {
                    const std::int32_t dev =
                        V_FrameWorkState::GetDevelopIdByKey(kv.first.c_str());
                    if (dev <= 0)
                        continue;
                    reqByDev[static_cast<std::uint32_t>(dev)] = &kv.second;
                    devByKey[kv.first] = static_cast<std::uint32_t>(dev);
                }

                struct InCand
                {
                    std::string key;
                    std::uint32_t root = 0;
                    int side = 0, grade = 0;
                    int devType = 0;
                    bool developed = false;
                    std::uint64_t outStamp = 0;
                };
                std::vector<InCand> ready;
                for (const std::string& k : std::vector<std::string>(
                         g_ParkedKeys.begin(), g_ParkedKeys.end()))
                {
                    auto itFail = g_RotateFailCount.find(k);
                    if (itFail != g_RotateFailCount.end()
                        && itFail->second >= kMaxRotateFailsPerKey)
                        continue;
                    if (rotatedOut.count(k))
                        continue;
                    auto itReq = g_RequestByKey.find(k);
                    if (itReq == g_RequestByKey.end())
                        continue;
                    if (!ParentReadyForRequest_NoLock(itReq->second))
                        continue;
                    auto itDev = devByKey.find(k);
                    if (itDev == devByKey.end())
                        continue;
                    InCand c;
                    c.key   = k;
                    c.root  = FamilyRootForRequest_NoLock(
                        itReq->second, reqByDev, itDev->second);
                    c.side  = RequestFlowNumber(itReq->second, "p51");
                    c.grade = RequestFlowNumber(itReq->second, "p52");
                    c.devType = RequestConstNumber(itReq->second, "p02");
                    c.developed = V_FrameWorkState::GetDevelopedByDevelopId(
                        static_cast<std::int32_t>(itDev->second));
                    auto itOut = g_PageOutStamp.find(k);
                    if (itOut != g_PageOutStamp.end())
                        c.outStamp = itOut->second;
                    ready.push_back(std::move(c));
                }
                if (ready.empty())
                    break;
                const int tabHint =
                    g_RotateTabHint.load(std::memory_order_relaxed);
                if (tabHint >= 0)
                {
                    bool anyHint = false;
                    for (const InCand& c : ready)
                        if (c.devType == tabHint) { anyHint = true; break; }
                    if (anyHint)
                        ready.erase(
                            std::remove_if(ready.begin(), ready.end(),
                                [&](const InCand& c)
                                { return c.devType != tabHint; }),
                            ready.end());
                }
                std::sort(ready.begin(), ready.end(),
                    [](const InCand& a, const InCand& b)
                    {
                        if (a.developed != b.developed) return !a.developed;
                        if (a.outStamp != b.outStamp) return a.outStamp < b.outStamp;
                        if (a.root  != b.root)  return a.root  < b.root;
                        if (a.side  != b.side)  return a.side  < b.side;
                        if (a.grade != b.grade) return a.grade < b.grade;
                        return a.key < b.key;
                    });
                const InCand* pick = nullptr;
                if (!ready.front().developed && stickyInRoot != 0)
                {
                    for (const InCand& c : ready)
                        if (c.root == stickyInRoot) { pick = &c; break; }
                }
                if (!pick && !ready.front().developed)
                {
                    std::vector<std::uint32_t> roots;
                    std::unordered_set<std::uint32_t> seenRoots;
                    for (const InCand& c : ready)
                        if (seenRoots.insert(c.root).second)
                            roots.push_back(c.root);
                    const std::uint32_t root =
                        roots[g_RotateInCursor % roots.size()];
                    for (const InCand& c : ready)
                        if (c.root == root) { pick = &c; break; }
                }
                if (!pick)
                    pick = &ready.front();
                inKey = pick->key;
                stickyInRoot = pick->root;
                {
                    auto itSel = g_RequestByKey.find(inKey);
                    if (itSel != g_RequestByKey.end())
                        inType = RequestConstNumber(itSel->second, "p02");
                }
                for (const InCand& c : ready)
                    readyRoots.insert(c.root);
            }

            if (DirectMaterializeCore(L, inKey))
            {
                rotatedIn.insert(inKey);
                {
                    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                    g_PageInStamp[inKey] = ++g_PageStampCounter;
                    g_RotateFailCount.erase(inKey);
                }
                ++swapped;
                continue;
            }

            void* bandCtrl = EquipDevelop_ResolveDevelopController();
            std::uint16_t bandFree = 0;
            const bool bandFull = !bandCtrl
                || !FindFirstEmptyRecordIndex(bandCtrl, bandFree);

            bool haveOut = false;
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                std::unordered_map<std::uint32_t,
                                   const PendingDevelopRequest*> reqByDev;
                for (const auto& kv : g_RequestByKey)
                {
                    const std::int32_t dev =
                        V_FrameWorkState::GetDevelopIdByKey(kv.first.c_str());
                    if (dev > 0)
                        reqByDev[static_cast<std::uint32_t>(dev)] = &kv.second;
                }

                struct OutCand
                {
                    std::string key;
                    int side = 0, grade = 0;
                    bool pinned = false;
                    bool developed = false;
                    int devType = 0;
                    std::uint64_t rowKey = 0;
                    std::uint64_t inStamp = 0;
                };
                std::unordered_set<std::uint32_t> wornDevs;
                for (std::size_t s = 0; s < outfit::shadow::kMaxSlots; ++s)
                {
                    outfit::shadow::Slot sh;
                    if (outfit::shadow::Get(s, &sh) && sh.developId != 0)
                        wornDevs.insert(sh.developId);
                }

                std::unordered_map<std::uint64_t, int> rowOccupancy;
                std::vector<std::pair<std::uint32_t, OutCand>> residents;
                for (const auto& kv : g_KeyRegistry)
                {
                    if (kv.second.flowIndex == 0)
                        continue;
                    const std::uint32_t dev = kv.second.developId;
                    if (g_GradeByDevelopId.find(dev)
                        == g_GradeByDevelopId.end())
                        continue;
                    auto itReq = g_RequestByKey.find(kv.first);
                    if (itReq == g_RequestByKey.end())
                        continue;
                    if (g_ParkedKeys.count(kv.first))
                        continue;
                    OutCand c;
                    c.key = kv.first;
                    auto itG = g_GradeByDevelopId.find(dev);
                    auto itS = g_SideByDevelopId.find(dev);
                    c.grade = (itG != g_GradeByDevelopId.end()) ? itG->second : 0;
                    c.side  = (itS != g_SideByDevelopId.end()) ? itS->second : 0;
                    c.devType = RequestConstNumber(itReq->second, "p02");
                    c.pinned =
                        EquipDevelop_IsDevelopTimerActive(
                            kv.second.flowIndex)
                        || HasResidentChildren_NoLock(dev)
                        || wornDevs.count(dev) != 0;
                    c.developed = V_FrameWorkState::GetDevelopedByDevelopId(
                        static_cast<std::int32_t>(dev));
                    const std::uint32_t root = FamilyRootForRequest_NoLock(
                        itReq->second, reqByDev, dev);
                    c.rowKey = (static_cast<std::uint64_t>(root) << 8)
                        | static_cast<std::uint64_t>(c.side & 0xFF);
                    ++rowOccupancy[c.rowKey];
                    if (rotatedIn.count(kv.first))
                        continue;
                    auto itIn = g_PageInStamp.find(kv.first);
                    if (itIn != g_PageInStamp.end())
                        c.inStamp = itIn->second;
                    residents.push_back({ root, std::move(c) });
                }
                std::unordered_map<std::uint32_t, OutCand> tipByRoot;
                for (auto& kv : residents)
                {
                    auto itTip = tipByRoot.find(kv.first);
                    if (itTip == tipByRoot.end()
                        || kv.second.side > itTip->second.side
                        || (kv.second.side == itTip->second.side
                            && kv.second.grade > itTip->second.grade))
                        tipByRoot[kv.first] = std::move(kv.second);
                }

                std::unordered_set<std::uint64_t> nativeRowKeys;
                {
                    std::vector<NativeRecordSnap> snap;
                    BuildNativeRecordSnapshot(snap);
                    std::unordered_map<std::uint32_t, std::uint32_t> baseById;
                    for (const NativeRecordSnap& s : snap)
                        baseById[s.developId] = s.base;
                    for (const NativeRecordSnap& s : snap)
                    {
                        if (g_GradeByDevelopId.find(s.developId)
                            != g_GradeByDevelopId.end())
                            continue;
                        std::uint32_t root = s.developId;
                        for (int hop = 0; hop < 64; ++hop)
                        {
                            auto it = baseById.find(root);
                            if (it == baseById.end() || it->second == 0
                                || it->second == root)
                                break;
                            root = it->second;
                        }
                        nativeRowKeys.insert(
                            (static_cast<std::uint64_t>(root) << 8)
                            | static_cast<std::uint64_t>(s.side & 0xFF));
                    }
                }
                std::vector<std::pair<std::uint32_t, const OutCand*>> pools[16];
                for (const auto& kv : tipByRoot)
                {
                    if (kv.second.pinned || kv.first == stickyInRoot)
                        continue;
                    auto itOcc = rowOccupancy.find(kv.second.rowKey);
                    const bool freesRow =
                        (itOcc != rowOccupancy.end() && itOcc->second == 1)
                        && nativeRowKeys.count(kv.second.rowKey) == 0;
                    const bool prot = readyRoots.count(kv.first) != 0;
                    const bool sameTab = (kv.second.devType == inType);
                    const int poolIdx =
                        (sameTab ? 0 : 8)
                        + (kv.second.developed ? 0 : 4)
                        + ((freesRow && !prot) ? 0
                           : (!prot)           ? 1
                           : (freesRow)        ? 2
                                               : 3);
                    pools[poolIdx].push_back({ kv.first, &kv.second });
                }
                std::vector<std::pair<std::uint32_t, const OutCand*>>* pool =
                    nullptr;
                const int poolLimit = bandFull ? 16 : 4;
                for (int p = 0; p < poolLimit; ++p)
                    if (!pools[p].empty()) { pool = &pools[p]; break; }
                if (pool)
                {
                    std::sort(pool->begin(), pool->end(),
                        [](const auto& a, const auto& b)
                        {
                            if (a.second->inStamp != b.second->inStamp)
                                return a.second->inStamp < b.second->inStamp;
                            return a.first > b.first;
                        });
                    outKey =
                        (*pool)[g_RotateOutCursor++ % pool->size()].second->key;
                    haveOut = true;
                }
            }
            if (!haveOut)
                break;

            if (!SwapDevelopRowCore(L, outKey, inKey))
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                ++g_RotateFailCount[inKey];
                continue;
            }
            rotatedIn.insert(inKey);
            rotatedOut.insert(outKey);
            {
                std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
                g_PageInStamp[inKey]   = ++g_PageStampCounter;
                g_PageOutStamp[outKey] = ++g_PageStampCounter;
                g_RotateFailCount.erase(inKey);
            }
            ++swapped;
        }
        ++g_RotateInCursor;

        {
            std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
            parkedLeft = g_ParkedKeys.size();
            g_AnyParkedKeys.store(!g_ParkedKeys.empty(),
                                  std::memory_order_relaxed);
        }
        if (swapped)
        {
            outfit::InvalidateEquipVisibilityCache();
            LogDebug("[EquipDevelop] develop-window rotation: paged %d row(s) in "
                "(%zu key(s) still parked - they follow on later R&D "
                "opens).\n", swapped, parkedLeft);
            void* controller = EquipDevelop_ResolveDevelopController();
            if (controller)
            {
                std::string map;
                map.reserve(1200);
                char buf[24];
                for (std::uint32_t i = 922; i < equip::NativeFlowBound(); ++i)
                {
                    if (equip::IsReservedFlowRow(i))
                        continue;
                    std::uint16_t dev = 0;
                    if (!ReadRecordDevelopId(
                            controller, static_cast<std::uint16_t>(i), dev)
                        || dev == 0)
                        break;
                    std::snprintf(buf, sizeof(buf), "%u=%u ", i, dev);
                    map += buf;
                }
                LogDebug("[EquipDevelop] band after rotation: %s\n", map.c_str());
            }
        }
        if (swapped && parkedLeft != 0)
            RequestRotationIfBandHasRoom();
    }

    static void TryRunPendingRotation()
    {
        if (!g_RotateRequested.load(std::memory_order_acquire))
            return;
        if (g_RotationInProgress.load(std::memory_order_relaxed))
            return;
        if (g_MenuFillDepth.load(std::memory_order_relaxed) != 0)
            return;
        const unsigned long luaTid = V_FrameWork_LuaOwnerThreadId();
        if (luaTid == 0 || GetCurrentThreadId() != luaTid)
            return;
        if (!ResolveLuaApi())
            return;
        lua_State* L = V_FrameWork_AnyLuaState();
        if (!L || !EnsureLuaReady())
            return;
        g_RotateRequested.store(false, std::memory_order_relaxed);
        g_RotationInProgress.store(true, std::memory_order_relaxed);
        RotateDevelopWindow(L);
        g_RotationInProgress.store(false, std::memory_order_relaxed);
    }

    void MaybeRotateDevelopWindow(std::uint16_t predicateIdx)
    {
        if (!g_AnyParkedKeys.load(std::memory_order_relaxed))
            return;
        if (predicateIdx > 1)
            return;
        const DWORD now  = GetTickCount();
        const DWORD last = g_LastVisPredicateTick;
        g_LastVisPredicateTick = now;
        if (last != 0 && (now - last) < kMenuOpenGapMs)
            return;
        g_RotateRequested.store(true, std::memory_order_release);
        const unsigned long luaTid = V_FrameWork_LuaOwnerThreadId();
        if (luaTid != 0 && GetCurrentThreadId() != luaTid
            && !g_RotateDeferLogged.exchange(true))
            LogDebug("[EquipDevelop] develop-window rotation requested off the Lua "
                "thread (menu-build tid %lu, Lua owner %lu) - deferred to the "
                "game-thread pump.\n",
                GetCurrentThreadId(), luaTid);
        TryRunPendingRotation();
    }

    void NoteMenuTabHint(int typeId)
    {
        g_RotateTabHint.store(typeId, std::memory_order_relaxed);
    }

    void NoteMenuFillEnter()
    {
        g_MenuFillDepth.fetch_add(1, std::memory_order_acquire);
    }

    void NoteMenuFillLeave()
    {
        g_MenuFillDepth.fetch_sub(1, std::memory_order_release);
    }

    void PumpDevelopMenuWork()
    {
        equip::SyncDevelopFlagsWithSave();
        if (g_RotateRequested.load(std::memory_order_relaxed))
            TryRunPendingRotation();
        if (g_GateRefreshRequested.load(std::memory_order_relaxed))
            MaybeRefreshDynamicGates();
        if (g_AnyPendingReg.load(std::memory_order_relaxed))
        {
            static DWORD s_LastPumpFlushTick = 0;
            const DWORD now = GetTickCount();
            if (now - s_LastPumpFlushTick >= 2000)
            {
                s_LastPumpFlushTick = now;
                if (g_MenuFillDepth.load(std::memory_order_relaxed) == 0
                    && ResolveLuaApi())
                {
                    const unsigned long luaTid = V_FrameWork_LuaOwnerThreadId();
                    lua_State* L = V_FrameWork_AnyLuaState();
                    if (luaTid != 0 && GetCurrentThreadId() == luaTid
                        && L && EnsureLuaReady())
                        FlushPendingRegistrations(L);
                }
            }
        }
    }

    bool TryGetFlowIndexForDevelopId(std::uint16_t developId, std::uint16_t& outFlowIndex)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

        for (const auto& kv : g_KeyRegistry)
        {
            if (kv.second.developId == developId)
            {
                outFlowIndex = kv.second.flowIndex;
                return true;
            }
        }

        outFlowIndex = 0xFFFF;
        return false;
    }

    bool IsManagedFlowIndex(std::uint16_t flowIndex)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

        for (const auto& kv : g_KeyRegistry)
            if (kv.second.flowIndex == flowIndex)
                return true;

        return false;
    }

    void MaybeRefreshDynamicGates()
    {
        if (!g_AnyDynamicGate.load(std::memory_order_relaxed))
            return;
        const unsigned long luaTid = V_FrameWork_LuaOwnerThreadId();
        if (luaTid == 0 || GetCurrentThreadId() != luaTid)
        {
            g_GateRefreshRequested.store(true, std::memory_order_release);
            return;
        }
        const DWORD now = GetTickCount();
        if (now - g_LastGateRefreshTick < kGateRefreshThrottleMs)
            return;
        g_LastGateRefreshTick = now;
        g_GateRefreshRequested.store(false, std::memory_order_relaxed);
        RefreshDynamicGatesImpl();
    }

    bool GetDevelopNameLangId(std::int32_t developId,
                              bool& outHasHash,
                              std::uint32_t& outHash,
                              std::string& outStr)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

        auto it = g_DevelopNameByDevelopId.find(developId);
        if (it == g_DevelopNameByDevelopId.end())
            return false;

        outHasHash = it->second.hasHash;
        outHash = it->second.hash;
        outStr = it->second.str;
        return true;
    }

    bool IsDevelopIdReservedByStock(std::uint32_t developId)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        return g_ObservedStockDevelopIds.find(developId)
            != g_ObservedStockDevelopIds.end();
    }

    bool IsFlowIndexReservedByStock(std::uint32_t flowIndex)
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        return g_ObservedStockFlowIndices.find(flowIndex)
            != g_ObservedStockFlowIndices.end();
    }

    bool Install_TppMotherBaseManagement_EquipDevelopHooks()
    {
        if (g_RegCstDevHookInstalled && g_RegFlwDevHookInstalled)
            return true;

        void* regCstTarget = ResolveGameAddress(gAddr.TppMotherBaseManagement_RegCstDev);
        void* regFlwTarget = ResolveGameAddress(gAddr.TppMotherBaseManagement_RegFlwDev);

        if (!regCstTarget || !regFlwTarget)
        {
            Log("[EquipDevelop] Failed to resolve RegCstDev / RegFlwDev\n");
            return false;
        }

        if (!g_RegCstDevHookInstalled)
        {
            if (!CreateAndEnableHook(
                regCstTarget,
                reinterpret_cast<void*>(&hkRegCstDev),
                reinterpret_cast<void**>(&g_OrigRegCstDev)))
            {
                Log("[EquipDevelop] Failed to hook RegCstDev\n");
                return false;
            }

            g_RegCstDevHookInstalled = true;
        }

        if (!g_RegFlwDevHookInstalled)
        {
            if (!CreateAndEnableHook(
                regFlwTarget,
                reinterpret_cast<void*>(&hkRegFlwDev),
                reinterpret_cast<void**>(&g_OrigRegFlwDev)))
            {
                Log("[EquipDevelop] Failed to hook RegFlwDev\n");
                return false;
            }

            g_RegFlwDevHookInstalled = true;
        }

        return true;
    }

    bool Uninstall_TppMotherBaseManagement_EquipDevelopHooks()
    {
        if (g_RegCstDevHookInstalled)
        {
            if (void* target = ResolveGameAddress(gAddr.TppMotherBaseManagement_RegCstDev))
                DisableAndRemoveHook(target);

            g_RegCstDevHookInstalled = false;
            g_OrigRegCstDev = nullptr;
        }

        if (g_RegFlwDevHookInstalled)
        {
            if (void* target = ResolveGameAddress(gAddr.TppMotherBaseManagement_RegFlwDev))
                DisableAndRemoveHook(target);

            g_RegFlwDevHookInstalled = false;
            g_OrigRegFlwDev = nullptr;
        }

        return true;
    }
}