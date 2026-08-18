#include "pch.h"
#include "V_FrameWorkState.h"
#include "log.h"
#include "AddressSet.h"
#include "../hooks/equip/EquipIdCompression.h"
#include "../hooks/equip/DevelopArrayGrow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace equip
{
    std::uint32_t NativeFlowBound();
}

namespace V_FrameWorkState
{
    namespace
    {
        static constexpr const char* kSavePath    = "mod\\V_FrameWork\\V_FrameWork_State.lua";
        static constexpr const char* kLegacyPath  = "mod\\saves\\V_FrameWork_State.lua";


        static constexpr std::int32_t kFirstCustomEquipIdMinimum = 1;
        static constexpr std::int32_t kFirstCustomDevelopId = 0x1000;
        static constexpr std::int32_t kFirstCustomFlowIndex = 922;
        static constexpr std::int32_t kGunsmithFlowFirst    = 0x3FD;
        static constexpr std::int32_t kGunsmithFlowLast     = 0x3FF;

        static bool IsReservedFlowIndex(std::int32_t i)
        {
            return i >= kGunsmithFlowFirst && i <= kGunsmithFlowLast;
        }
        static constexpr std::int32_t kNativeFlowSentinel   = 0x400;

        static std::int32_t NativeFlowIndexBound()
        {
            return static_cast<std::int32_t>(equip::NativeFlowBound());
        }
        static constexpr std::int16_t kFirstCustomTapeSaveIndex = 300;
        static constexpr std::int16_t kMaxCustomTapeSaveIndex = 32000;
        static constexpr std::int32_t kTapeOrphanGraceLaunches = 2;
        static constexpr std::int32_t kEquipOrphanGraceLaunches = 2;
        static constexpr std::int32_t kConstantOrphanGraceLaunches = 2;
        static constexpr std::int32_t kMaxCustomConstantValue = 0xFFF0;

        struct EquipEntry
        {


            std::int32_t developId = 0;
            std::int32_t flowIndex = 0;

            std::int32_t equipId = 0;
            std::int32_t subId = 0;

            std::int32_t partsType = 0;
            std::int32_t selector  = 0;
            std::uint8_t variantSelectors[V_FrameWorkState::kPersistedVariantSelectorSlots] = {};

            std::int32_t misses = 0;

            std::int8_t developed = -1;

            bool isNew = false;
            bool devReqAnnounced = false;
        };

        struct TapeEntry
        {
            std::int16_t saveIndex = -1;
            bool owned = false;
            bool isNew = false;
            std::int32_t misses = 0;
        };

        struct State
        {
            bool loaded = false;
            bool dirty = false;
            std::unordered_map<std::string, EquipEntry> equips;
            std::unordered_map<std::string, TapeEntry> tapes;
            std::unordered_map<std::string, std::int32_t> constants;
            std::unordered_map<std::string, std::int32_t> constantMisses;
            std::unordered_set<std::int32_t> pinnedEquipIds;
        };

        static State g_State;
        static std::mutex g_Mutex;
        static std::vector<std::int32_t> g_PendingDevelopedResets;
        static int g_BatchDepth = 0;
        static std::unordered_set<std::int16_t> g_TapeSaveIndexInUse;

        static constexpr unsigned long long kCoalesceMs = 250;

        static std::condition_variable g_FlushCv;
        static std::thread             g_FlusherThread;
        static bool                    g_FlusherRunning = false;
        static bool                    g_FlusherStop    = false;
        static unsigned long long      g_SaveDueTick    = 0;
        static unsigned long long      g_CoalescedCount = 0;


        static std::unordered_set<std::string> g_ConstantsTouched;
        static std::unordered_set<std::int32_t> g_SessionPinnedIds;
        static bool g_PinSetFreshThisSession = false;

        static std::unordered_map<std::string, std::int32_t> g_SessionEquipIds;

        static std::uint64_t g_ClaimedEquipBits[4096 / 64] = {};

        static void NoteClaimedEquipId_NoLock(std::int32_t equipId)
        {
            if (equipId > 0 && equipId < 4096)
                g_ClaimedEquipBits[equipId >> 6] |= 1ull << (equipId & 63);
        }

        static std::unordered_set<std::int32_t> g_VanillaIdentityIds;

        static std::unordered_map<std::string, std::int32_t> g_SessionFlowIndices;

        static std::unordered_map<std::int32_t, std::int32_t> g_OldFlowLayout;

        static std::string Trim(const std::string& s)
        {
            const auto begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }

        static void EnsureSaveDirectory()
        {
            CreateDirectoryA("mod", nullptr);
            CreateDirectoryA("mod\\V_FrameWork", nullptr);
        }


        static void MigrateLegacyStateFile_NoLock()
        {
            const DWORD newAttr = GetFileAttributesA(kSavePath);
            if (newAttr != INVALID_FILE_ATTRIBUTES)
                return;

            const DWORD oldAttr = GetFileAttributesA(kLegacyPath);
            if (oldAttr == INVALID_FILE_ATTRIBUTES)
                return;

            CreateDirectoryA("mod", nullptr);
            CreateDirectoryA("mod\\V_FrameWork", nullptr);

            MoveFileA(kLegacyPath, kSavePath);
        }


        static bool ParseEquipLine(const std::string& line, std::string& outKey, EquipEntry& out)
        {
            const auto lb = line.find("[\"");
            if (lb == std::string::npos) return false;
            const auto rb = line.find("\"]", lb + 2);
            if (rb == std::string::npos) return false;

            outKey = line.substr(lb + 2, rb - (lb + 2));
            out = {};

            auto findField = [&](const char* name) -> std::int32_t
            {
                const std::string field = std::string(name) + " = ";
                const auto pos = line.find(field);
                if (pos == std::string::npos) return 0;
                const auto start = pos + field.size();
                std::string numStr;
                for (auto i = start; i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (c == ',' || c == '}' || c == ' ') break;
                    numStr.push_back(c);
                }
                try { return static_cast<std::int32_t>(std::stol(numStr, nullptr, 0)); }
                catch (...) { return 0; }
            };

            out.developId = findField("developId");
            out.flowIndex = findField("flowIndex");
            out.equipId   = findField("equipId");
            out.subId     = findField("subId");
            out.partsType = findField("partsType");
            out.selector  = findField("selector");
            out.misses    = findField("misses");

            {
                const char* tag = "variantSelectors = \"";
                const auto pos = line.find(tag);
                if (pos != std::string::npos)
                {
                    std::size_t i = pos + std::strlen(tag);
                    std::size_t vi = 0;
                    std::string numStr;
                    for (; i < line.size()
                           && vi < V_FrameWorkState::kPersistedVariantSelectorSlots; ++i)
                    {
                        const char c = line[i];
                        if (c >= '0' && c <= '9') { numStr.push_back(c); continue; }
                        if (!numStr.empty())
                        {
                            try
                            {
                                const long v = std::stol(numStr);
                                if (v > 0 && v <= 0xFF)
                                    out.variantSelectors[vi++] =
                                        static_cast<std::uint8_t>(v);
                            }
                            catch (...) {}
                            numStr.clear();
                        }
                        if (c == '"') break;
                    }
                }
            }
            if (line.find("developed = true") != std::string::npos)
                out.developed = 1;
            else if (line.find("developed = false") != std::string::npos)
                out.developed = 0;

            out.isNew = line.find("new = true") != std::string::npos;
            out.devReqAnnounced =
                line.find("reqAnnounced = true") != std::string::npos;

            return !outKey.empty();
        }


        static bool ParseTapeLine(const std::string& line, std::string& outKey, TapeEntry& out)
        {
            const auto lb = line.find("[\"");
            if (lb == std::string::npos) return false;
            const auto rb = line.find("\"]", lb + 2);
            if (rb == std::string::npos) return false;

            outKey = line.substr(lb + 2, rb - (lb + 2));
            out = {};

            auto findIntField = [&](const char* name) -> std::int32_t
            {
                const std::string field = std::string(name) + " = ";
                const auto pos = line.find(field);
                if (pos == std::string::npos) return 0;
                const auto start = pos + field.size();
                std::string numStr;
                for (auto i = start; i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (c == ',' || c == '}' || c == ' ') break;
                    numStr.push_back(c);
                }
                try { return static_cast<std::int32_t>(std::stol(numStr, nullptr, 0)); }
                catch (...) { return 0; }
            };

            out.saveIndex = static_cast<std::int16_t>(findIntField("saveIndex"));
            out.owned = line.find("owned = true") != std::string::npos;
            out.isNew = line.find("new = true") != std::string::npos;
            out.misses = findIntField("misses");
            return !outKey.empty() && out.saveIndex > 0;
        }

        static bool ParseConstantLine(const std::string& line, std::string& outKey, std::int32_t& outValue)
        {
            const auto lb = line.find("[\"");
            if (lb == std::string::npos) return false;
            const auto rb = line.find("\"]", lb + 2);
            if (rb == std::string::npos) return false;

            outKey = line.substr(lb + 2, rb - (lb + 2));
            outValue = 0;

            const auto eq = line.find('=', rb);
            if (eq == std::string::npos) return false;

            std::string numStr;
            for (auto i = eq + 1; i < line.size(); ++i)
            {
                const char c = line[i];
                if (c == ' ' && numStr.empty()) continue;
                if (c == ',' || c == '}' || c == ' ') break;
                numStr.push_back(c);
            }
            try { outValue = static_cast<std::int32_t>(std::stol(numStr, nullptr, 0)); }
            catch (...) { return false; }
            return !outKey.empty() && outValue != 0;
        }

        static void SaveToDisk_NoLock();
        static void WriteToDisk_NoLock();

        static void LoadFromDisk_NoLock()
        {
            if (g_State.loaded) return;
            g_State.loaded = true;
            g_State.equips.clear();
            g_State.tapes.clear();
            g_State.constants.clear();
            g_State.constantMisses.clear();
            g_State.pinnedEquipIds.clear();
            g_ConstantsTouched.clear();
            g_TapeSaveIndexInUse.clear();

            MigrateLegacyStateFile_NoLock();

            std::ifstream in(kSavePath);
            if (!in)
            {
                return;
            }

            enum Section { None, Equips, Tapes, Constants, ConstantMisses,
                           PinnedIds } section = None;

            std::string line;
            while (std::getline(in, line))
            {
                const std::string trimmed = Trim(line);

                if (trimmed.rfind("equips", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Equips;
                    continue;
                }

                if (trimmed.rfind("tapes", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Tapes;
                    continue;
                }

                if (trimmed.rfind("constantMisses", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = ConstantMisses;
                    continue;
                }

                if (trimmed.rfind("loadoutPinnedIds", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = PinnedIds;
                    continue;
                }

                if (trimmed.rfind("constants", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Constants;
                    continue;
                }


                if (trimmed == "}," || (trimmed == "}" && section != None))
                {
                    section = None;
                    continue;
                }

                if (section == Equips)
                {
                    std::string key;
                    EquipEntry entry;
                    if (ParseEquipLine(trimmed, key, entry))
                    {
                        const auto colon = key.rfind(':');
                        if (colon != std::string::npos && entry.equipId != 0)
                        {
                            const std::string bareKey = key.substr(colon + 1);
                            EquipEntry& bare = g_State.equips[bareKey];
                            bare.equipId = entry.equipId;
                            if (entry.misses != 0)
                                bare.misses = entry.misses;
                            entry.equipId = 0;
                        }
                        g_State.equips[key] = entry;
                    }
                }
                else if (section == Tapes)
                {
                    std::string key;
                    TapeEntry entry;
                    if (ParseTapeLine(trimmed, key, entry))
                    {
                        g_State.tapes[key] = entry;
                        g_TapeSaveIndexInUse.insert(entry.saveIndex);
                        LogDebug("[CustomTapes] tape loaded: '%s' (saveIndex %d)\n", key.c_str(), static_cast<int>(entry.saveIndex));
                    }
                }
                else if (section == Constants)
                {
                    std::string key;
                    std::int32_t value = 0;
                    if (ParseConstantLine(trimmed, key, value))
                        g_State.constants[key] = value;
                }
                else if (section == ConstantMisses)
                {
                    std::string key;
                    std::int32_t value = 0;
                    if (ParseConstantLine(trimmed, key, value))
                        g_State.constantMisses[key] = value;
                }
                else if (section == PinnedIds)
                {
                    std::string key;
                    std::int32_t value = 0;
                    if (ParseConstantLine(trimmed, key, value))
                        g_State.pinnedEquipIds.insert(value);
                }
            }

            in.close();

            bool gcChanged = false;
            for (auto it = g_State.tapes.begin(); it != g_State.tapes.end(); )
            {
                if (it->second.misses >= kTapeOrphanGraceLaunches)
                {
                    LogDebug("[CustomTapes] tape deleted: '%s' (saveIndex %d) - mod uninstalled; freeing the save slot.\n", it->first.c_str(), static_cast<int>(it->second.saveIndex));
                    it = g_State.tapes.erase(it);
                    gcChanged = true;
                }
                else
                {
                    ++it->second.misses;
                    gcChanged = true;
                    ++it;
                }
            }

            for (auto it = g_State.constants.begin(); it != g_State.constants.end(); )
            {
                const auto mit = g_State.constantMisses.find(it->first);
                const std::int32_t misses =
                    (mit != g_State.constantMisses.end()) ? mit->second : 0;
                if (misses >= kConstantOrphanGraceLaunches)
                {
                    LogDebug("[Constants] \"%s\" (value %d) has not been referenced for %d "
                        "launches - entry removed; its value returns to the free pool.\n",
                        it->first.c_str(), it->second, misses);
                    if (mit != g_State.constantMisses.end())
                        g_State.constantMisses.erase(mit);
                    it = g_State.constants.erase(it);
                    gcChanged = true;
                    continue;
                }
                ++it;
            }

            for (auto it = g_State.equips.begin(); it != g_State.equips.end(); )
            {
                if (it->second.misses >= kEquipOrphanGraceLaunches
                    && it->second.equipId != 0)
                {
                    const std::int32_t slot =
                        EquipIdCompression::ComputeCompressed(it->second.equipId);
                    bool pinned = false;
                    for (const std::int32_t p : g_State.pinnedEquipIds)
                        if (EquipIdCompression::ComputeCompressed(p) == slot)
                        {
                            pinned = true;
                            break;
                        }
                    if (pinned)
                    {
                        LogDebug("[V_FrameWorkState] '%s' is orphaned but its equipId "
                            "0x%X is still referenced by a saved loadout - id "
                            "kept reserved.\n",
                            it->first.c_str(), it->second.equipId);
                        ++it;
                        continue;
                    }
                }
                if (it->second.misses >= kEquipOrphanGraceLaunches)
                {
                    LogDebug("[V_FrameWorkState] \"%s\" (developId %d, equipId 0x%X, partsType 0x%02X, "
                        "selector 0x%02X) has not registered for %d launches - entry removed; its ids "
                        "return to the free pool.\n",
                        it->first.c_str(), it->second.developId, it->second.equipId,
                        it->second.partsType, it->second.selector, it->second.misses);
                    it = g_State.equips.erase(it);
                    gcChanged = true;
                    continue;
                }
                ++it->second.misses;
                gcChanged = true;
                ++it;
            }

            g_OldFlowLayout.clear();
            for (const auto& kv : g_State.equips)
            {
                const std::int32_t fi = kv.second.flowIndex;
                const std::int32_t dv = kv.second.developId;
                if (dv == 0 || fi < kFirstCustomFlowIndex
                    || fi >= NativeFlowIndexBound() || fi == kNativeFlowSentinel)
                    continue;
                auto found = g_OldFlowLayout.find(fi);
                if (found == g_OldFlowLayout.end())
                    g_OldFlowLayout[fi] = dv;
                else if (found->second != dv)
                    found->second = -1;
            }

            if (gcChanged)
            {
                g_State.dirty = true;
                SaveToDisk_NoLock();
            }
        }

        static void FlusherMain()
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            for (;;)
            {
                g_FlushCv.wait(lock,
                    [] { return g_FlusherStop || g_SaveDueTick != 0; });

                if (g_FlusherStop)
                    return;

                for (;;)
                {
                    const unsigned long long now = GetTickCount64();
                    if (now >= g_SaveDueTick)
                        break;
                    g_FlushCv.wait_for(lock,
                        std::chrono::milliseconds(g_SaveDueTick - now));
                    if (g_FlusherStop)
                        return;
                }

                g_SaveDueTick = 0;
                const unsigned long long coalesced = g_CoalescedCount;
                g_CoalescedCount = 0;

                WriteToDisk_NoLock();

                if (coalesced > 1)
                    LogDebug("[V_FrameWorkState] coalesced %llu state writes into 1 "
                        "(%llu forced disk commits avoided)\n",
                        coalesced, coalesced - 1);
            }
        }

        static void SaveToDisk_NoLock()
        {
            if (g_BatchDepth > 0)
                return;

            ++g_CoalescedCount;
            g_SaveDueTick = GetTickCount64() + kCoalesceMs;

            if (!g_FlusherRunning)
            {
                g_FlusherRunning = true;
                g_FlusherThread = std::thread(&FlusherMain);
            }

            g_FlushCv.notify_one();
        }

        static void WriteToDisk_NoLock()
        {
            EnsureSaveDirectory();

            const std::string tmpPath = std::string(kSavePath) + ".tmp";

            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                Log("[V_FrameWorkState] ERROR: could not write '%s' - custom-tape ownership/save-index state will not persist.\n", tmpPath.c_str());
                return;
            }

            out << "return {\n";

            {
                std::vector<std::pair<std::string, EquipEntry>> sorted;
                sorted.reserve(g_State.equips.size());
                for (const auto& kv : g_State.equips)
                    sorted.emplace_back(kv.first, kv.second);

                std::unordered_map<std::string, std::int32_t> bareEquipId;
                for (const auto& kv : sorted)
                    if (kv.first.find(':') == std::string::npos
                        && kv.second.equipId != 0)
                        bareEquipId[kv.first] = kv.second.equipId;
                std::unordered_set<std::string> folded;
                for (auto& kv : sorted)
                {
                    const auto colon = kv.first.rfind(':');
                    if (colon == std::string::npos)
                        continue;
                    const auto b = bareEquipId.find(kv.first.substr(colon + 1));
                    if (b != bareEquipId.end())
                    {
                        kv.second.equipId = b->second;
                        folded.insert(b->first);
                    }
                }
                if (!folded.empty())
                    sorted.erase(std::remove_if(sorted.begin(), sorted.end(),
                        [&](const std::pair<std::string, EquipEntry>& kv) {
                            return kv.first.find(':') == std::string::npos
                                && folded.count(kv.first) != 0;
                        }), sorted.end());

                std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

                out << "    equips = {\n";
                for (const auto& kv : sorted)
                {


                    if (kv.second.developId == 0 && kv.second.flowIndex == 0 &&
                        kv.second.equipId == 0 && kv.second.subId == 0 &&
                        kv.second.partsType == 0 && kv.second.selector == 0 &&
                        kv.second.misses == 0 && kv.second.developed < 0 &&
                        !kv.second.isNew && !kv.second.devReqAnnounced)
                        continue;
                    out << "        [\"" << kv.first << "\"] = {";
                    if (kv.second.developId != 0)
                        out << " developId = " << kv.second.developId << ",";
                    if (kv.second.flowIndex != 0)
                        out << " flowIndex = " << kv.second.flowIndex << ",";
                    if (kv.second.equipId != 0)
                        out << " equipId = " << kv.second.equipId << ",";
                    if (kv.second.subId != 0)
                        out << " subId = " << kv.second.subId << ",";
                    if (kv.second.partsType != 0)
                        out << " partsType = " << kv.second.partsType << ",";
                    if (kv.second.selector != 0)
                        out << " selector = " << kv.second.selector << ",";
                    {
                        std::size_t last = 0;
                        for (std::size_t vi = 0;
                             vi < V_FrameWorkState::kPersistedVariantSelectorSlots; ++vi)
                            if (kv.second.variantSelectors[vi] != 0) last = vi + 1;
                        if (last != 0)
                        {
                            out << " variantSelectors = \"";
                            for (std::size_t vi = 0; vi < last; ++vi)
                            {
                                if (vi) out << ",";
                                out << static_cast<int>(kv.second.variantSelectors[vi]);
                            }
                            out << "\",";
                        }
                    }
                    if (kv.second.misses != 0)
                        out << " misses = " << kv.second.misses << ",";
                    if (kv.second.developed >= 0)
                        out << " developed = " << (kv.second.developed == 1 ? "true" : "false") << ",";
                    if (kv.second.isNew)
                        out << " new = true,";
                    if (kv.second.devReqAnnounced)
                        out << " reqAnnounced = true,";
                    out << " },\n";
                }
                out << "    },\n";
            }


            {
                std::vector<std::pair<std::string, TapeEntry>> sorted;
                sorted.reserve(g_State.tapes.size());
                for (const auto& kv : g_State.tapes)
                    sorted.emplace_back(kv.first, kv.second);
                std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b) { return a.second.saveIndex < b.second.saveIndex; });

                out << "    tapes = {\n";
                for (const auto& kv : sorted)
                {
                    out << "        [\"" << kv.first << "\"] = {"
                        << " saveIndex = " << static_cast<int>(kv.second.saveIndex)
                        << ", owned = " << (kv.second.owned ? "true" : "false")
                        << ", new = " << (kv.second.isNew ? "true" : "false");
                    if (kv.second.misses != 0)
                        out << ", misses = " << kv.second.misses;
                    out << " },\n";
                }
                out << "    },\n";
            }


            {
                std::vector<std::pair<std::string, std::int32_t>> sorted;
                sorted.reserve(g_State.constants.size());
                for (const auto& kv : g_State.constants)
                    sorted.emplace_back(kv.first, kv.second);
                std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

                out << "    constants = {\n";
                for (const auto& kv : sorted)
                {
                    if (kv.second == 0)
                        continue;
                    out << "        [\"" << kv.first << "\"] = " << kv.second << ",\n";
                }
                out << "    },\n";
            }

            {
                std::vector<std::pair<std::string, std::int32_t>> sorted;
                sorted.reserve(g_State.constants.size());
                for (const auto& kv : g_State.constants)
                {
                    if (g_ConstantsTouched.find(kv.first) != g_ConstantsTouched.end())
                        continue;
                    const auto mit = g_State.constantMisses.find(kv.first);
                    const std::int32_t loaded =
                        (mit != g_State.constantMisses.end()) ? mit->second : 0;
                    sorted.emplace_back(kv.first, loaded + 1);
                }
                std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

                out << "    constantMisses = {\n";
                for (const auto& kv : sorted)
                    out << "        [\"" << kv.first << "\"] = " << kv.second << ",\n";
                out << "    },\n";
            }

            {
                std::vector<std::int32_t> sorted(g_State.pinnedEquipIds.begin(),
                                                 g_State.pinnedEquipIds.end());
                std::sort(sorted.begin(), sorted.end());
                out << "    loadoutPinnedIds = {\n";
                for (const std::int32_t id : sorted)
                    out << "        [\"" << id << "\"] = " << id << ",\n";
                out << "    },\n";
            }

            out << "}\n";
            out.close();
            if (!out)
            {
                Log("[V_FrameWorkState] ERROR: write to '%s' failed mid-stream - previous state file left untouched.\n", tmpPath.c_str());
                DeleteFileA(tmpPath.c_str());
                return;
            }

            DWORD lastErr = 0;
            bool replaced = false;
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                if (MoveFileExA(tmpPath.c_str(), kSavePath,
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    replaced = true;
                    break;
                }
                lastErr = GetLastError();
                if (lastErr != ERROR_ACCESS_DENIED && lastErr != ERROR_SHARING_VIOLATION)
                    break;
                Sleep(3);
            }
            if (!replaced)
            {
                Log("[V_FrameWorkState] ERROR: could not replace '%s' (err %lu) - state kept in '%s'.\n",
                    kSavePath, lastErr, tmpPath.c_str());
                return;
            }
            g_State.dirty = false;
        }

        static bool IsEquipIdInUse_NoLock(std::int32_t id)
        {
            for (const auto& kv : g_SessionEquipIds)
                if (kv.second == id) return true;
            const std::int32_t slot = EquipIdCompression::ComputeCompressed(id);
            for (const auto& kv : g_State.equips)
                if (kv.second.equipId != 0 &&
                    EquipIdCompression::ComputeCompressed(kv.second.equipId) == slot)
                    return true;
            return false;
        }

        static bool IsDevelopIdInUse_NoLock(std::int32_t id)
        {
            for (const auto& kv : g_State.equips)
                if (kv.second.developId == id) return true;
            return false;
        }

        static bool IsTapeSaveIndexInUse_NoLock(std::int16_t idx)
        {
            return g_TapeSaveIndexInUse.find(idx) != g_TapeSaveIndexInUse.end();
        }

        static bool IsConstantValueInUse_NoLock(const std::string& spacePrefix, std::int32_t value)
        {
            for (const auto& kv : g_State.constants)
                if (kv.second == value &&
                    kv.first.compare(0, spacePrefix.size(), spacePrefix) == 0)
                    return true;
            return false;
        }

        static bool IsSafeConstantNamePart(const char* s)
        {
            for (const char* p = s; *p; ++p)
            {
                const char c = *p;
                if (c == '"' || c == '[' || c == ']' || c == '\n' || c == '\r')
                    return false;
            }
            return true;
        }

        static bool GuardStateKey(const char* key, const char* who)
        {
            bool bad = false;
            for (const char* p = key; *p; ++p)
            {
                if (*p == '\n' || *p == '\r') { bad = true; break; }
                if (*p == '"' && *(p + 1) == ']') { bad = true; break; }
            }
            if (!bad)
                return true;
            Log("[V_FrameWorkState] ERROR: %s rejected key '%s' - a line break or "
                "the sequence \"] inside a key would corrupt the state file.\n",
                who, key);
            return false;
        }


        static bool g_NativeTableSynced = false;

        static constexpr std::int32_t kItemCategoryRowFirst = 0x1F2;
        static constexpr std::int32_t kItemCategoryRowLast  = 0x1FC;

        static bool IsItemCategoryRow(std::int32_t equipId)
        {
            return equipId >= kItemCategoryRowFirst
                && equipId <= kItemCategoryRowLast;
        }

        static void BuildVanillaIdentitySet_NoLock()
        {
            static bool s_attempted = false;
            if (s_attempted)
                return;
            s_attempted = true;

            std::vector<std::int32_t> ids(4096);
            const std::size_t n =
                equip::CollectVanillaIdentityEquipIds(ids.data(), ids.size());
            if (n < 64)
            {
                Log("[V_FrameWorkState] WARNING: the vanilla equip-identity set is "
                    "still empty at first allocation (develop lookup returned %zu "
                    "id(s), below the %d-id sanity floor) - custom ids can land on "
                    "rows the game's own tables own, and a suit that takes a weapon "
                    "row renders in the prep list with a damage tag\n",
                    n, 64);
                return;
            }
            for (std::size_t i = 0; i < n; ++i)
                if (ids[i] > 0)
                    g_VanillaIdentityIds.insert(ids[i]);
        }

        static std::int32_t AllocateNextFreeEquipId_NoLock(std::int32_t minimum,
                                                           bool isWeapon)
        {
            if (!g_NativeTableSynced)
            {
                EquipIdCompression::SyncFromNativeTable();
                g_NativeTableSynced = true;
            }

            const std::int32_t floor =
                (minimum > kFirstCustomEquipIdMinimum)
                    ? minimum
                    : kFirstCustomEquipIdMinimum;

            if (g_VanillaIdentityIds.empty())
                BuildVanillaIdentitySet_NoLock();

            const auto inUse = [isWeapon](std::int32_t equipId) {
                if (isWeapon && IsItemCategoryRow(equipId))
                    return true;
                return IsEquipIdInUse_NoLock(equipId)
                    || g_VanillaIdentityIds.count(equipId) != 0;
            };

            std::int32_t result = -1;
            if (isWeapon)
            {
                const std::int32_t wFloor =
                    floor > EquipIdCompression::kWeaponBandFirst
                        ? floor : EquipIdCompression::kWeaponBandFirst;
                result = EquipIdCompression::FindLowestFreeEquipIdInRange(
                    inUse, wFloor, EquipIdCompression::kWeaponBandLastUsable);
                if (result < 0)
                    result = EquipIdCompression::FindLowestFreeEquipIdInRange(
                        inUse, floor, EquipIdCompression::kItemBandLast);
            }
            else
            {
                result = EquipIdCompression::FindLowestFreeEquipIdInRange(
                    inUse, floor, EquipIdCompression::kItemBandLast);
            }
            if (result < 0)
            {
                result = EquipIdCompression::FindLowestFreeExtendedEquipId();
                if (result >= 0)
                    LogDebug("[V_FrameWorkState] AllocateNextFreeEquipId: native bands "
                        "full above floor=0x%X - allocated EXTENDED equipId 0x%X "
                        "(DLL-side table, served via hooked accessors)\n",
                        floor, result);
                else
                    LogDebug("[V_FrameWorkState] AllocateNextFreeEquipId: native bands "
                        "AND the extended 0x289-0x3FF range are exhausted above "
                        "floor=0x%X; allocation fails.\n", floor);
            }
            return result;
        }


        static std::int32_t AllocateNextFreeDevelopId_NoLock(std::int32_t minimum)
        {
            std::int32_t id = (minimum > kFirstCustomDevelopId) ? minimum : kFirstCustomDevelopId;
            while (IsDevelopIdInUse_NoLock(id))
                ++id;
            return id;
        }

        static std::int16_t AllocateNextFreeTapeSaveIndex_NoLock(std::int16_t minimum)
        {
            std::int16_t idx = (minimum > kFirstCustomTapeSaveIndex) ? minimum : kFirstCustomTapeSaveIndex;
            while (idx <= kMaxCustomTapeSaveIndex && IsTapeSaveIndexInUse_NoLock(idx)) ++idx;
            return (idx <= kMaxCustomTapeSaveIndex) ? idx : static_cast<std::int16_t>(-1);
        }
    }

    void Load()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
    }

    void Save()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_SaveDueTick = 0;
        g_CoalescedCount = 0;
        if (g_State.dirty)
            WriteToDisk_NoLock();
    }

    void SaveOnProcessExit()
    {
        std::unique_lock<std::mutex> lock(g_Mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;

        if (g_SaveDueTick != 0 || g_State.dirty)
        {
            g_SaveDueTick = 0;
            g_CoalescedCount = 0;
            WriteToDisk_NoLock();
        }
    }

    void AbandonFlusherThread()
    {
        if (g_FlusherThread.joinable())
            g_FlusherThread.detach();
    }

    void FlushPendingSaves()
    {
        std::thread joinMe;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (g_SaveDueTick != 0 || g_State.dirty)
            {
                g_SaveDueTick = 0;
                g_CoalescedCount = 0;
                WriteToDisk_NoLock();
            }
            if (g_FlusherRunning)
            {
                g_FlusherStop = true;
                g_FlusherRunning = false;
                joinMe = std::move(g_FlusherThread);
            }
        }

        g_FlushCv.notify_all();
        if (joinMe.joinable())
            joinMe.join();
    }

    void BeginBatch()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        ++g_BatchDepth;
    }

    void EndBatch()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_BatchDepth > 0)
            --g_BatchDepth;
        if (g_BatchDepth == 0 && g_State.dirty)
            SaveToDisk_NoLock();
    }

    bool ResolveOrCreateEquipId(
        const char* key,
        std::int32_t minimumId,
        std::int32_t& outEquipId,
        bool isWeapon)
    {
        outEquipId = 0;
        if (!key || !key[0]) return false;
        if (!GuardStateKey(key, "ResolveOrCreateEquipId")) return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();


        auto it = g_SessionEquipIds.find(key);
        if (it != g_SessionEquipIds.end() && it->second != 0)
        {
            NoteClaimedEquipId_NoLock(it->second);
            outEquipId = it->second;
            return true;
        }

        auto pit = g_State.equips.find(key);
        if (pit != g_State.equips.end() && pit->second.equipId != 0)
        {
            if (!g_NativeTableSynced)
            {
                EquipIdCompression::SyncFromNativeTable();
                g_NativeTableSynced = true;
            }
            const std::int32_t persisted = pit->second.equipId;
            bool sessionTaken = false;
            for (const auto& kv : g_SessionEquipIds)
                if (kv.second == persisted) { sessionTaken = true; break; }
            const std::int32_t slot = EquipIdCompression::ComputeCompressed(persisted);
            const bool bandOk = isWeapon
                || slot < EquipIdCompression::kWeaponBandFirst;
            if (!bandOk)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' sits in "
                    "the weapon band but the item is not a weapon - native "
                    "GetEquipType would return 0 for it; reallocating into the "
                    "item band\n", persisted, key);
            if (g_VanillaIdentityIds.empty())
                BuildVanillaIdentitySet_NoLock();
            const bool identityClash =
                g_VanillaIdentityIds.count(persisted) != 0;
            if (identityClash)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' is a vanilla "
                    "equip identity the game's own tables still own - keeping it "
                    "overwrites that row, and the prep list then renders the vanilla "
                    "entry with this item's damage tag; reallocating\n",
                    persisted, key);
            const bool itemListClash = isWeapon && IsItemCategoryRow(persisted);
            if (itemListClash)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' is a weapon "
                    "sitting in the vanilla item-category row span 0x%X-0x%X - that "
                    "list enumerates the whole span and does not filter on equip "
                    "type, so the weapon renders as a supply-drop item; "
                    "reallocating\n",
                    persisted, key, kItemCategoryRowFirst, kItemCategoryRowLast);
            if (!sessionTaken && bandOk && !identityClash && !itemListClash
                && !EquipIdCompression::IsCompressedSlotUsed(slot))
            {
                g_SessionEquipIds[key] = persisted;
                NoteClaimedEquipId_NoLock(persisted);
                pit->second.misses = 0;
                outEquipId = persisted;
                return true;
            }
            if (bandOk)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' is no longer free "
                    "(vanilla layout change or conflict) - reallocating; loadout references "
                    "to the old id will be healed or blanked.\n", persisted, key);
        }

        const std::int32_t newId = AllocateNextFreeEquipId_NoLock(minimumId, isWeapon);
        if (newId < 0)
        {


            outEquipId = 0;
            return false;
        }

        g_SessionEquipIds[key] = newId;
        NoteClaimedEquipId_NoLock(newId);
        g_State.equips[key].equipId = newId;
        g_State.equips[key].misses = 0;
        g_State.dirty = true;
        SaveToDisk_NoLock();
        outEquipId = newId;

        return true;
    }

    bool IsClaimedEquipId(std::int32_t equipId)
    {
        if (equipId <= 0 || equipId >= 4096)
            return false;
        return (g_ClaimedEquipBits[equipId >> 6]
                & (1ull << (equipId & 63))) != 0;
    }

    void SetVanillaIdentityEquipIds(const std::int32_t* equipIds,
                                    std::size_t count)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_VanillaIdentityIds.clear();
        for (std::size_t i = 0; i < count; ++i)
            if (equipIds[i] > 0)
                g_VanillaIdentityIds.insert(equipIds[i]);
    }

    static bool IsOwnedEquipId_NoLock(std::int32_t equipId)
    {
        for (const auto& kv : g_SessionEquipIds)
            if (kv.second == equipId) return true;
        for (const auto& kv : g_State.equips)
            if (kv.second.equipId == equipId) return true;
        return false;
    }

    void NotePinnedEquipId(std::int32_t equipId)
    {
        if (equipId <= 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        if (!IsOwnedEquipId_NoLock(equipId))
            return;
        if (!g_PinSetFreshThisSession)
        {
            g_PinSetFreshThisSession = true;
            g_SessionPinnedIds.clear();
            if (!g_State.pinnedEquipIds.empty())
            {
                g_State.pinnedEquipIds.clear();
                g_State.dirty = true;
            }
        }
        if (g_SessionPinnedIds.insert(equipId).second)
        {
            g_State.pinnedEquipIds.insert(equipId);
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    void ReplacePinnedEquipIds(const std::int32_t* equipIds, std::size_t count)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        std::unordered_set<std::int32_t> next;
        for (std::size_t i = 0; i < count; ++i)
            if (equipIds[i] > 0)
                next.insert(equipIds[i]);
        if (next != g_State.pinnedEquipIds)
        {
            g_State.pinnedEquipIds = std::move(next);
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    bool ResolveOrCreateDevelopId(
        const char* key,
        std::int32_t minimumId,
        std::int32_t& outDevelopId,
        bool* outCreated)
    {
        outDevelopId = 0;
        if (outCreated) *outCreated = false;
        if (!key || !key[0]) return false;
        if (!GuardStateKey(key, "ResolveOrCreateDevelopId")) return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto it = g_State.equips.find(key);
        if (it != g_State.equips.end() && it->second.developId != 0)
        {
            it->second.misses = 0;
            g_State.dirty = true;
            outDevelopId = it->second.developId;
            if (outCreated) *outCreated = false;
            return true;
        }

        const std::int32_t newId = AllocateNextFreeDevelopId_NoLock(minimumId);
        g_State.equips[key].developId = newId;
        g_State.equips[key].misses = 0;
        g_State.dirty = true;
        outDevelopId = newId;
        if (outCreated) *outCreated = true;

        SaveToDisk_NoLock();

        return true;
    }

    std::int32_t GetDevelopIdByKey(const char* key)
    {
        if (!key || !key[0]) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.equips.find(key);
        return (it != g_State.equips.end()) ? it->second.developId : 0;
    }

    std::int32_t GetDevelopIdAtOldFlowIndex(std::int32_t oldFlowIndex)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_OldFlowLayout.find(oldFlowIndex);
        return (it != g_OldFlowLayout.end()) ? it->second : 0;
    }

    std::int32_t GetFlowIndexByDevelopId(std::int32_t developId)
    {
        if (developId == 0) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId == developId)
                return kv.second.flowIndex;
        return 0;
    }

    std::vector<std::int32_t> TakePendingDevelopedResets()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        std::vector<std::int32_t> out;
        out.swap(g_PendingDevelopedResets);
        return out;
    }

    bool ResolveDevelopedFlag(const char* key, bool defaultDeveloped)
    {
        if (!key || !key[0]) return defaultDeveloped;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.equips.find(key);
        if (it == g_State.equips.end())
            return defaultDeveloped;
        if (it->second.developed < 0)
        {
            it->second.developed = defaultDeveloped ? 1 : 0;
            it->second.isNew = true;
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
        return it->second.developed == 1;
    }

    bool IsManagedDevelopId(std::int32_t developId)
    {
        if (developId == 0) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId == developId)
                return true;
        return false;
    }

    void SetDevelopedByDevelopId(std::int32_t developId, bool developed)
    {
        if (developId == 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        const std::int8_t v = developed ? 1 : 0;
        for (auto& kv : g_State.equips)
        {
            if (kv.second.developId == developId)
            {
                if (kv.second.developed != v)
                {
                    kv.second.developed = v;
                    g_State.dirty = true;
                    SaveToDisk_NoLock();
                }
                return;
            }
        }
    }

    bool GetDevelopedByDevelopId(std::int32_t developId)
    {
        if (developId == 0) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId == developId)
                return kv.second.developed == 1;
        return false;
    }

    bool IsExplicitlyUndevelopedByDevelopId(std::int32_t developId)
    {
        if (developId == 0) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId == developId)
                return kv.second.developed == 0;
        return false;
    }

    void ForEachManagedDevelop(
        const std::function<void(std::int32_t developId, bool developed, bool isNew)>& callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId != 0)
                callback(kv.second.developId, kv.second.developed == 1, kv.second.isNew);
    }

    void ForEachManagedDevelopRow(
        const std::function<void(std::int32_t developId, std::int32_t flowIndex,
                                 bool reqAnnounced)>& callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId != 0 && kv.second.flowIndex != 0)
                callback(kv.second.developId, kv.second.flowIndex,
                         kv.second.devReqAnnounced);
    }

    void SetNewByDevelopId(std::int32_t developId, bool isNew)
    {
        if (developId == 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (auto& kv : g_State.equips)
        {
            if (kv.second.developId == developId)
            {
                if (kv.second.isNew != isNew)
                {
                    kv.second.isNew = isNew;
                    g_State.dirty = true;
                    SaveToDisk_NoLock();
                }
                return;
            }
        }
    }

    bool GetDevReqAnnouncedByDevelopId(std::int32_t developId)
    {
        if (developId == 0) return true;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId == developId)
                return kv.second.devReqAnnounced;
        return false;
    }

    void SetDevReqAnnouncedByDevelopId(std::int32_t developId, bool announced)
    {
        if (developId == 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (auto& kv : g_State.equips)
        {
            if (kv.second.developId == developId)
            {
                if (kv.second.devReqAnnounced != announced)
                {
                    kv.second.devReqAnnounced = announced;
                    g_State.dirty = true;
                    SaveToDisk_NoLock();
                }
                return;
            }
        }
    }

    bool GetNewByDevelopId(std::int32_t developId)
    {
        if (developId == 0) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
            if (kv.second.developId == developId)
                return kv.second.isNew;
        return false;
    }

    bool ResolveOrCreateFlowIndex(
        const char* key,
        std::int32_t minimumIndex,
        std::int32_t& outFlowIndex)
    {
        (void)minimumIndex;
        outFlowIndex = 0;
        if (!key || !key[0]) return false;
        if (!GuardStateKey(key, "ResolveOrCreateFlowIndex")) return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto sit = g_SessionFlowIndices.find(key);
        if (sit != g_SessionFlowIndices.end())
        {
            if (!IsReservedFlowIndex(sit->second))
            {
                outFlowIndex = sit->second;
                return true;
            }
            Log("[V_FrameWorkState] WARNING: '%s' held develop flow index %d, which the "
                "loadout commit reads as a gunsmith slot (flows 0x3FD-0x3FF resolve to "
                "equip 871/873/875) - picking it would equip a gunsmith pseudo-weapon "
                "instead. Reallocating outside the reserved range.\n",
                key, sit->second);
            g_SessionFlowIndices.erase(sit);
        }

        const std::int32_t flowBound = NativeFlowIndexBound();
        std::int32_t newIdx = 0;
        {
            std::vector<bool> used(
                static_cast<std::size_t>(flowBound
                                         - kFirstCustomFlowIndex), false);
            for (const auto& kv : g_SessionFlowIndices)
                if (kv.second >= kFirstCustomFlowIndex
                    && kv.second < flowBound)
                    used[static_cast<std::size_t>(
                        kv.second - kFirstCustomFlowIndex)] = true;
            for (std::int32_t i = kFirstCustomFlowIndex;
                 i < flowBound; ++i)
            {
                if (i == kNativeFlowSentinel || IsReservedFlowIndex(i))
                    continue;
                if (!used[static_cast<std::size_t>(i - kFirstCustomFlowIndex)])
                {
                    newIdx = i;
                    break;
                }
            }
        }
        if (newIdx == 0)
        {
            LogDebug("[V_FrameWorkState] ResolveOrCreateFlowIndex: REFUSED '%s' - the native "
                "develop flow array holds %d rows and indices %d..%d are all allocated. "
                "Registering this row would corrupt memory past the array. The item will "
                "not appear in R&D until develop-row paging frees window space.\n",
                key, flowBound, kFirstCustomFlowIndex,
                flowBound - 1);
            return false;
        }

        g_SessionFlowIndices[key] = newIdx;

        g_State.equips[key].flowIndex = newIdx;
        g_State.dirty = true;
        outFlowIndex = newIdx;

        SaveToDisk_NoLock();

        return true;
    }

    std::int32_t GetPersistedFlowIndex(const char* key)
    {
        if (!key || !key[0]) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.equips.find(key);
        if (it == g_State.equips.end()) return 0;
        return it->second.flowIndex;
    }

    void SetSessionFlowIndex(const char* key, std::int32_t flowIndex)
    {
        if (!key || !key[0] || flowIndex <= 0) return;
        if (!GuardStateKey(key, "SetSessionFlowIndex")) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        bool dirty = false;
        for (auto it = g_SessionFlowIndices.begin();
             it != g_SessionFlowIndices.end();)
        {
            if (it->second == flowIndex && it->first != key)
            {
                auto itE = g_State.equips.find(it->first);
                if (itE != g_State.equips.end() && itE->second.flowIndex != 0)
                {
                    itE->second.flowIndex = 0;
                    dirty = true;
                }
                it = g_SessionFlowIndices.erase(it);
            }
            else
                ++it;
        }

        for (auto& kv : g_State.equips)
        {
            if (kv.first != key && kv.second.flowIndex == flowIndex)
            {
                kv.second.flowIndex = 0;
                dirty = true;
            }
        }

        auto sit = g_SessionFlowIndices.find(key);
        if (sit == g_SessionFlowIndices.end() || sit->second != flowIndex)
            g_SessionFlowIndices[key] = flowIndex;

        auto& e = g_State.equips[key];
        if (e.flowIndex != flowIndex)
        {
            e.flowIndex = flowIndex;
            dirty = true;
        }

        if (dirty)
        {
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    void ReleaseSessionFlowIndex(const char* key)
    {
        if (!key || !key[0]) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto it = g_SessionFlowIndices.find(key);
        if (it != g_SessionFlowIndices.end())
            g_SessionFlowIndices.erase(it);

        auto itE = g_State.equips.find(key);
        if (itE != g_State.equips.end() && itE->second.flowIndex != 0)
        {
            itE->second.flowIndex = 0;
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    bool ResolveOrCreateConstantValue(
        const char* spaceTag,
        const char* name,
        std::int32_t minimumValue,
        std::int32_t& outValue)
    {
        outValue = 0;
        if (!spaceTag || !spaceTag[0] || !name || !name[0]) return false;
        if (!IsSafeConstantNamePart(spaceTag) || !IsSafeConstantNamePart(name))
        {
            Log("[Constants] ERROR: unsafe characters in constant name '%s' (space '%s'); rejected.\n", name, spaceTag);
            return false;
        }

        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        const std::string prefix = std::string(spaceTag) + ":";
        const std::string key = prefix + name;

        auto it = g_State.constants.find(key);
        if (it != g_State.constants.end() && it->second != 0)
        {
            g_ConstantsTouched.insert(key);
            outValue = it->second;
            return true;
        }

        std::int32_t value = (minimumValue > 0) ? minimumValue : 1;
        while (value <= kMaxCustomConstantValue && IsConstantValueInUse_NoLock(prefix, value))
            ++value;
        if (value > kMaxCustomConstantValue)
        {
            Log("[Constants] ERROR: no free value for '%s' - space '%s' pool [%d..%d] is full.\n",
                name, spaceTag, minimumValue, kMaxCustomConstantValue);
            return false;
        }

        g_State.constants[key] = value;
        g_ConstantsTouched.insert(key);
        g_State.dirty = true;
        outValue = value;

        SaveToDisk_NoLock();

#ifdef _DEBUG
        LogDebug("[Constants] allocated %s = %d\n", key.c_str(), value);
#endif
        return true;
    }

    std::int32_t GetPersistedConstant(const char* spaceTag, const char* name)
    {
        if (!spaceTag || !spaceTag[0] || !name || !name[0]) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        const std::string key = std::string(spaceTag) + ":" + name;
        auto it = g_State.constants.find(key);
        if (it == g_State.constants.end())
            return 0;
        g_ConstantsTouched.insert(key);
        return it->second;
    }

    void SetPersistedConstant(const char* spaceTag, const char* name,
                              std::int32_t value)
    {
        if (!spaceTag || !spaceTag[0] || !name || !name[0]) return;
        if (!IsSafeConstantNamePart(spaceTag) || !IsSafeConstantNamePart(name))
            return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        const std::string key = std::string(spaceTag) + ":" + name;
        g_ConstantsTouched.insert(key);
        auto it = g_State.constants.find(key);
        if (it != g_State.constants.end() && it->second == value)
            return;
        g_State.constants[key] = value;
        g_State.dirty = true;
        SaveToDisk_NoLock();
    }

    void ForEachPersistedConstant(const char* spaceTag,
                                  void (*fn)(const char* name, std::int32_t value))
    {
        if (!spaceTag || !spaceTag[0] || !fn) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        const std::string prefix = std::string(spaceTag) + ":";
        for (const auto& kv : g_State.constants)
            if (kv.first.compare(0, prefix.size(), prefix) == 0)
                fn(kv.first.c_str() + prefix.size(), kv.second);
    }

    std::uint8_t GetPersistedOutfitPartsType(const char* key)
    {
        if (!key || !key[0]) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.equips.find(key);
        if (it == g_State.equips.end()) return 0;
        const std::int32_t v = it->second.partsType;
        return (v > 0 && v <= 0xFF) ? static_cast<std::uint8_t>(v) : 0;
    }

    std::uint8_t GetPersistedOutfitSelector(const char* key)
    {
        if (!key || !key[0]) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.equips.find(key);
        if (it == g_State.equips.end()) return 0;
        const std::int32_t v = it->second.selector;
        return (v > 0 && v <= 0xFF) ? static_cast<std::uint8_t>(v) : 0;
    }

    void SetPersistedOutfitIds(const char* key,
                               std::uint8_t partsType,
                               std::uint8_t selector)
    {
        if (!key || !key[0]) return;
        if (!GuardStateKey(key, "SetPersistedOutfitIds")) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto& e = g_State.equips[key];
        bool changed = false;
        if (partsType != 0 && e.partsType != static_cast<std::int32_t>(partsType))
        {
            e.partsType = static_cast<std::int32_t>(partsType);
            changed = true;
        }
        if (selector != 0 && e.selector != static_cast<std::int32_t>(selector))
        {
            e.selector = static_cast<std::int32_t>(selector);
            changed = true;
        }
        if (changed)
        {
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    std::size_t GetPersistedOutfitVariantSelectors(const char* key,std::uint8_t* out,std::size_t cap)
    {
        if (!key || !key[0] || !out || cap == 0) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.equips.find(key);
        if (it == g_State.equips.end()) return 0;

        if (it->second.misses != 0)
        {
            it->second.misses = 0;
            g_State.dirty = true;
        }

        std::size_t nonZero = 0;
        const std::size_t n =
            (cap < V_FrameWorkState::kPersistedVariantSelectorSlots)
                ? cap : V_FrameWorkState::kPersistedVariantSelectorSlots;
        for (std::size_t i = 0; i < n; ++i)
        {
            out[i] = it->second.variantSelectors[i];
            if (out[i] != 0) ++nonZero;
        }
        for (std::size_t i = n; i < cap; ++i) out[i] = 0;
        return nonZero;
    }

    void SetPersistedOutfitVariantSelectors(const char* key, const std::uint8_t* selectors, std::size_t count)
    {
        if (!key || !key[0]) return;
        if (!GuardStateKey(key, "SetPersistedOutfitVariantSelectors")) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto& e = g_State.equips[key];
        bool changed = false;

        if (e.misses != 0)
        {
            e.misses = 0;
            changed = true;
        }
        for (std::size_t i = 0;
             i < V_FrameWorkState::kPersistedVariantSelectorSlots; ++i)
        {
            const std::uint8_t v =
                (selectors && i < count) ? selectors[i] : std::uint8_t{0};
            if (e.variantSelectors[i] != v)
            {
                e.variantSelectors[i] = v;
                changed = true;
            }
        }
        if (changed)
        {
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    void ClearPersistedOutfitIds(const char* key)
    {
        if (!key || !key[0]) return;
        if (!GuardStateKey(key, "ClearPersistedOutfitIds")) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto it = g_State.equips.find(key);
        if (it == g_State.equips.end()) return;
        auto& e = it->second;
        bool changed = false;
        if (e.partsType != 0) { e.partsType = 0; changed = true; }
        if (e.selector != 0)  { e.selector = 0;  changed = true; }
        for (std::size_t i = 0;
             i < V_FrameWorkState::kPersistedVariantSelectorSlots; ++i)
        {
            if (e.variantSelectors[i] != 0)
            {
                e.variantSelectors[i] = 0;
                changed = true;
            }
        }
        if (changed)
        {
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    void ForEachPersistedOutfit(
        const std::function<void(const std::string& key, std::uint8_t partsType, std::uint8_t selector, const std::uint8_t* variants)>& callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
        {
            const auto& e = kv.second;
            const std::uint8_t pt =
                (e.partsType > 0 && e.partsType <= 0xFF)
                    ? static_cast<std::uint8_t>(e.partsType) : std::uint8_t{0};
            const std::uint8_t sel =
                (e.selector > 0 && e.selector <= 0xFF)
                    ? static_cast<std::uint8_t>(e.selector) : std::uint8_t{0};
            bool anyVariant = false;
            for (std::size_t i = 0;
                 i < V_FrameWorkState::kPersistedVariantSelectorSlots; ++i)
                if (e.variantSelectors[i] != 0) { anyVariant = true; break; }
            if (pt == 0 && sel == 0 && !anyVariant) continue;
            callback(kv.first, pt, sel, e.variantSelectors);
        }
    }

    bool ResolveOrCreateTapeSaveIndex(
        const char* key,
        std::int16_t minimumIndex,
        std::int16_t& outSaveIndex)
    {
        outSaveIndex = -1;
        if (!key || !key[0]) return false;
        if (!GuardStateKey(key, "ResolveOrCreateTapeSaveIndex")) return false;

        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto it = g_State.tapes.find(key);
        if (it != g_State.tapes.end() && it->second.saveIndex > 0)
        {
            if (it->second.misses != 0)
            {
                it->second.misses = 0;
                g_State.dirty = true;
                SaveToDisk_NoLock();
            }
            outSaveIndex = it->second.saveIndex;
            return true;
        }

        const std::int16_t newIdx = AllocateNextFreeTapeSaveIndex_NoLock(minimumIndex);
        if (newIdx < 0)
        {
            Log("[CustomTapes] ERROR: custom-tape save-index pool [300-32000] is full - uninstall unused tape mods; no more custom tapes can be registered.\n");
            return false;
        }

        g_State.tapes[key].saveIndex = newIdx;
        g_TapeSaveIndexInUse.insert(newIdx);
        g_State.dirty = true;
        outSaveIndex = newIdx;
#ifdef _DEBUG
        LogDebug("[CustomTapes] tape added: '%s' (saveIndex %d) - first time; saved to V_FrameWork_State.lua.\n", key, static_cast<int>(newIdx));
#endif

        SaveToDisk_NoLock();

        return true;
    }

    void SetTapeOwned(const char* key, bool owned)
    {
        if (!key || !key[0]) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_State.tapes.find(key);
        if (it != g_State.tapes.end() && it->second.owned != owned)
        {
            it->second.owned = owned;
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    void SetTapeNew(const char* key, bool isNew)
    {
        if (!key || !key[0]) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_State.tapes.find(key);
        if (it != g_State.tapes.end() && it->second.isNew != isNew)
        {
            it->second.isNew = isNew;
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    void SetTapeOwnedBySaveIndex(std::int16_t saveIndex, bool owned)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& kv : g_State.tapes)
        {
            if (kv.second.saveIndex == saveIndex && kv.second.owned != owned)
            {
                kv.second.owned = owned;
                g_State.dirty = true;
                SaveToDisk_NoLock();
                return;
            }
        }
    }

    void SetTapeNewBySaveIndex(std::int16_t saveIndex, bool isNew)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        for (auto& kv : g_State.tapes)
        {
            if (kv.second.saveIndex == saveIndex && kv.second.isNew != isNew)
            {
                kv.second.isNew = isNew;
                g_State.dirty = true;
                SaveToDisk_NoLock();
                return;
            }
        }
    }

    bool GetTapeOwned(const char* key)
    {
        if (!key || !key[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_State.tapes.find(key);
        return (it != g_State.tapes.end()) ? it->second.owned : false;
    }

    bool GetTapeNew(const char* key)
    {
        if (!key || !key[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        auto it = g_State.tapes.find(key);
        return (it != g_State.tapes.end()) ? it->second.isNew : false;
    }

    void ForEachTape(
        const std::function<void(const std::string&, std::int16_t, bool, bool)>& callback)
    {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.tapes)
            callback(kv.first, kv.second.saveIndex, kv.second.owned, kv.second.isNew);
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_State.loaded = false;
        g_State.dirty = false;
        g_State.equips.clear();
        g_State.tapes.clear();
        g_State.constants.clear();
        g_State.constantMisses.clear();
        g_State.pinnedEquipIds.clear();
        g_ConstantsTouched.clear();
        g_SessionPinnedIds.clear();
        g_PinSetFreshThisSession = false;
        g_SessionEquipIds.clear();
        g_SessionFlowIndices.clear();
        g_OldFlowLayout.clear();
        g_PendingDevelopedResets.clear();
    }
}
