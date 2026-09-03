#include "pch.h"
#include "V_FrameWorkState.h"
#include "log.h"
#include "AddressSet.h"
#include "../hooks/equip/CustomBluePrint.h"
#include "../hooks/equip/EquipIdCompression.h"
#include "../hooks/equip/DevelopArrayGrow.h"
#include "FeatureModule.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
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
        static constexpr std::int32_t kBluePrintOrphanGraceLaunches = 2;
        static constexpr std::int32_t kMaxCustomConstantValue = 0xFFF0;

        static constexpr char kUniqueStaffKeyPrefix[] = "USTAFF:";
        static constexpr std::size_t kUniqueStaffKeyPrefixLen =
            sizeof(kUniqueStaffKeyPrefix) - 1;

        static bool IsUniqueStaffConstantKey(const std::string& key)
        {
            return key.compare(0, kUniqueStaffKeyPrefixLen, kUniqueStaffKeyPrefix) == 0;
        }

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

            std::uint8_t kind = 0;
        };

        struct TapeEntry
        {
            std::int16_t saveIndex = -1;
            bool owned = false;
            bool isNew = false;
            std::int32_t misses = 0;
        };

        struct BluePrintEntry
        {
            std::int32_t id = 0;
            bool owned = false;
            bool isNew = false;
            std::int32_t misses = 0;
        };

        struct State
        {
            bool loaded = false;
            bool dirty = false;
            bool launchHealthy = false;
            std::unordered_map<std::string, EquipEntry> equips;
            std::unordered_map<std::string, TapeEntry> tapes;
            std::unordered_map<std::string, BluePrintEntry> bluePrints;
            std::unordered_map<std::string, std::int32_t> constants;
            std::unordered_map<std::string, std::int32_t> constantMisses;
            std::unordered_set<std::int32_t> pinnedEquipIds;
        };

        static State g_State;
        static bool g_PrevLaunchHealthy = true;
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
        static bool g_ExitSave = false;

        static bool CanCountConstantMisses()
        {
            if (!g_ExitSave)
                return false;
            if (!g_State.launchHealthy)
            {
                static bool s_said = false;
                if (!s_said)
                {
                    s_said = true;
                    Log("[V_FrameWorkState] this launch did not install cleanly, so no "
                        "mod could register - unreferenced-constant counters frozen; "
                        "counting this launch would expire persisted ids that are still "
                        "in use\n");
                }
                return false;
            }
            if (FeatureAnyDisabled())
            {
                static bool s_said = false;
                if (!s_said)
                {
                    s_said = true;
                    Log("[V_FrameWorkState] disabled_modules.txt is active, so "
                        "absent symbols are by request, not an uninstall - "
                        "unreferenced-constant counters frozen this launch; two "
                        "bisect runs would otherwise expire every persisted "
                        "equipId\n");
                }
                return false;
            }
            return true;
        }
        static std::unordered_set<std::int32_t> g_SessionPinnedIds;
        static std::unordered_set<std::int32_t> g_StickyPinnedIds;
        static bool g_PinSetFreshThisSession = false;

        static std::unordered_map<std::string, std::int32_t> g_SessionEquipIds;

        static std::atomic<std::uint64_t> g_ClaimedEquipBits[0x10000 / 64] = {};

        static void NoteClaimedEquipId_NoLock(std::int32_t equipId)
        {
            if (equipId > 0 && equipId <= 0xFFFF)
                g_ClaimedEquipBits[equipId >> 6].fetch_or(
                    1ull << (equipId & 63), std::memory_order_relaxed);
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


        static std::int32_t ParseStateInt(const std::string& text)
        {
            std::size_t at = 0;
            bool negative = false;
            if (at < text.size() && (text[at] == '-' || text[at] == '+'))
            {
                negative = (text[at] == '-');
                ++at;
            }
            int base = 10;
            if (text.size() - at > 2 && text[at] == '0'
                && (text[at + 1] == 'x' || text[at + 1] == 'X'))
            {
                base = 16;
                at += 2;
            }
            try
            {
                const long v = std::stol(text.substr(at), nullptr, base);
                return static_cast<std::int32_t>(negative ? -v : v);
            }
            catch (...) { return 0; }
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
                const auto pos = line.find(field, rb);
                if (pos == std::string::npos) return 0;
                const auto start = pos + field.size();
                std::string numStr;
                for (auto i = start; i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (c == ',' || c == '}' || c == ' ') break;
                    numStr.push_back(c);
                }
                return ParseStateInt(numStr);
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
                const auto pos = line.find(tag, rb);
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
                            long v = 0;
                            try { v = std::stol(numStr); }
                            catch (...) { v = 0; }
                            if (v > 0 && v <= 0xFF)
                                out.variantSelectors[vi] =
                                    static_cast<std::uint8_t>(v);
                            ++vi;
                            numStr.clear();
                        }
                        if (c == '"') break;
                    }
                }
            }
            if (line.find("developed = true", rb) != std::string::npos)
                out.developed = 1;
            else if (line.find("developed = false", rb) != std::string::npos)
                out.developed = 0;

            out.isNew =
                line.find("new = true", rb) != std::string::npos
                || line.find("seen = true", rb) == std::string::npos;
            out.devReqAnnounced =
                line.find("reqAnnounced = true", rb) != std::string::npos
                || line.find("announcePending = true", rb) == std::string::npos;

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
                const auto pos = line.find(field, rb);
                if (pos == std::string::npos) return 0;
                const auto start = pos + field.size();
                std::string numStr;
                for (auto i = start; i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (c == ',' || c == '}' || c == ' ') break;
                    numStr.push_back(c);
                }
                return ParseStateInt(numStr);
            };

            out.saveIndex = static_cast<std::int16_t>(findIntField("saveIndex"));
            out.owned = line.find("owned = true", rb) != std::string::npos;
            out.isNew = line.find("new = true", rb) != std::string::npos;
            out.misses = findIntField("misses");
            return !outKey.empty() && out.saveIndex > 0;
        }

        static bool ParseBluePrintLine(const std::string& line, std::string& outKey,
                                       BluePrintEntry& out)
        {
            const auto lb = line.find("[\"");
            if (lb == std::string::npos) return false;
            const auto rb = line.find("\"]", lb + 2);
            if (rb == std::string::npos) return false;

            outKey = line.substr(lb + 2, rb - (lb + 2));
            out = {};

            const std::string field = "bluePrintId = ";
            const auto pos = line.find(field, rb);
            if (pos != std::string::npos)
            {
                std::string numStr;
                for (auto i = pos + field.size(); i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (c == ',' || c == '}' || c == ' ') break;
                    numStr.push_back(c);
                }
                out.id = ParseStateInt(numStr);
            }

            out.owned = line.find("owned = true", rb) != std::string::npos;
            out.isNew = line.find("new = true", rb) != std::string::npos;

            const std::string missField = "misses = ";
            const auto mpos = line.find(missField, rb);
            if (mpos != std::string::npos)
            {
                std::string numStr;
                for (auto i = mpos + missField.size(); i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (c == ',' || c == '}' || c == ' ') break;
                    numStr.push_back(c);
                }
                out.misses = ParseStateInt(numStr);
            }

            return !outKey.empty();
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
            outValue = ParseStateInt(numStr);
            return !outKey.empty() && outValue != 0;
        }

        static std::string CanonicalConstantKey(std::string key)
        {
            static const char kLegacyWeaponSlot[] = "WPSLOT:";
            if (key.rfind(kLegacyWeaponSlot, 0) == 0)
                key = "WP:" + key.substr(sizeof(kLegacyWeaponSlot) - 1);
            return key;
        }

        static void SaveToDisk_NoLock();
        static void WriteToDisk_NoLock();

        static void LoadFromDisk_NoLock()
        {
            if (g_State.loaded) return;
            g_State.loaded = true;
            g_State.equips.clear();
            g_State.tapes.clear();
            g_State.bluePrints.clear();
            g_State.constants.clear();
            g_State.constantMisses.clear();
            g_State.pinnedEquipIds.clear();
            g_VanillaIdentityIds.clear();
            g_ConstantsTouched.clear();
            g_TapeSaveIndexInUse.clear();

            MigrateLegacyStateFile_NoLock();

            std::ifstream in(kSavePath);
            if (!in)
            {
                return;
            }

            g_PrevLaunchHealthy = false;

            enum Section { None, Equips, Weapons, Outfits, Tapes, BluePrints,
                           Constants, ConstantMisses, PinnedIds,
                           VanillaIdentity } section = None;

            std::string constantSpace;

            std::string line;
            while (std::getline(in, line))
            {
                const std::string trimmed = Trim(line);

                if ((section == Constants || section == ConstantMisses)
                    && trimmed.find("[\"") == std::string::npos
                    && trimmed.find('{') != std::string::npos)
                {
                    const auto eq = trimmed.find('=');
                    if (eq != std::string::npos)
                        constantSpace = Trim(trimmed.substr(0, eq));
                    continue;
                }

                if (section == None && trimmed.rfind("launchHealthy", 0) == 0)
                {
                    g_PrevLaunchHealthy =
                        trimmed.find("false") == std::string::npos;
                    continue;
                }

                if (trimmed.rfind("equips", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Equips;
                    continue;
                }

                if (trimmed.rfind("develop", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Equips;
                    continue;
                }

                if (trimmed.rfind("weapons", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Weapons;
                    continue;
                }

                if (trimmed.rfind("outfits", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Outfits;
                    continue;
                }

                if (trimmed.rfind("tapes", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = Tapes;
                    continue;
                }

                if ((trimmed.rfind("DataBase", 0) == 0
                     || trimmed.rfind("BluePrint", 0) == 0) &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = BluePrints;
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

                if (trimmed.rfind("vanillaEquipIdentity", 0) == 0 &&
                    trimmed.find('{') != std::string::npos)
                {
                    section = VanillaIdentity;
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
                    if ((section == Constants || section == ConstantMisses)
                        && !constantSpace.empty())
                        constantSpace.clear();
                    else
                        section = None;
                    continue;
                }

                if (section == Equips || section == Weapons
                    || section == Outfits)
                {
                    std::string key;
                    EquipEntry entry;
                    if (ParseEquipLine(trimmed, key, entry))
                    {
                        entry.kind = (section == Weapons) ? kRowKindWeapon
                                   : (section == Outfits) ? kRowKindOutfit
                                                          : kRowKindUnknown;
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
                else if (section == BluePrints)
                {
                    std::string key;
                    BluePrintEntry entry;
                    if (ParseBluePrintLine(trimmed, key, entry))
                        g_State.bluePrints[key] = entry;
                }
                else if (section == Constants)
                {
                    std::string key;
                    std::int32_t value = 0;
                    if (ParseConstantLine(trimmed, key, value))
                    {
                        if (!constantSpace.empty())
                            key = constantSpace + ":" + key;
                        g_State.constants[CanonicalConstantKey(key)] = value;
                    }
                }
                else if (section == ConstantMisses)
                {
                    std::string key;
                    std::int32_t value = 0;
                    if (ParseConstantLine(trimmed, key, value))
                    {
                        if (!constantSpace.empty())
                            key = constantSpace + ":" + key;
                        g_State.constantMisses[CanonicalConstantKey(key)] = value;
                    }
                }
                else if (section == PinnedIds)
                {
                    std::string key;
                    std::int32_t value = 0;
                    if (ParseConstantLine(trimmed, key, value))
                        g_State.pinnedEquipIds.insert(value);
                }
                else if (section == VanillaIdentity)
                {
                    std::size_t pos = 0;
                    while (pos < trimmed.size())
                    {
                        while (pos < trimmed.size()
                               && (trimmed[pos] < '0' || trimmed[pos] > '9'))
                            ++pos;
                        std::size_t start = pos;
                        while (pos < trimmed.size()
                               && trimmed[pos] >= '0' && trimmed[pos] <= '9')
                            ++pos;
                        if (pos > start)
                        {
                            const long v = std::strtol(
                                trimmed.substr(start, pos - start).c_str(),
                                nullptr, 10);
                            if (v > 0 && v < 0x10000)
                                g_VanillaIdentityIds.insert(
                                    static_cast<std::int32_t>(v));
                        }
                    }
                }
            }

            in.close();

            {
                const std::string prefix = "BLUEPRINT:";
                for (auto it = g_State.constants.begin(); it != g_State.constants.end(); )
                {
                    if (it->first.compare(0, prefix.size(), prefix) == 0 && it->second > 0)
                    {
                        const std::string key = it->first.substr(prefix.size());
                        auto bp = g_State.bluePrints.find(key);
                        if (bp == g_State.bluePrints.end() || bp->second.id <= 0)
                            g_State.bluePrints[key].id = it->second;
                        g_State.constantMisses.erase(it->first);
                        it = g_State.constants.erase(it);
                        g_State.dirty = true;
                        continue;
                    }
                    ++it;
                }

                for (auto it = g_State.bluePrints.begin(); it != g_State.bluePrints.end(); )
                {
                    if (it->second.id <= 0)
                        it = g_State.bluePrints.erase(it);
                    else
                        ++it;
                }

                for (auto it = g_State.bluePrints.begin(); it != g_State.bluePrints.end(); )
                {
                    if (!g_PrevLaunchHealthy) { ++it; continue; }
                    if (it->second.misses >= kBluePrintOrphanGraceLaunches)
                    {
                        Log("[V_FrameWorkState] blueprint \"%s\" (id %d) has not registered for "
                            "%d launches - entry removed, its id returns to the pool and any "
                            "ownership of it is lost\n",
                            it->first.c_str(), it->second.id, it->second.misses);
                        it = g_State.bluePrints.erase(it);
                        continue;
                    }
                    ++it->second.misses;
                    ++it;
                }
            }

            static const char kRewardLangKeyPrefix[] = "REWARDLANG32:";
            const std::size_t kRewardLangKeyPrefixLen = sizeof(kRewardLangKeyPrefix) - 1;

            bool gcChanged = false;
            std::size_t evicted = 0;
            for (auto it = g_State.tapes.begin(); it != g_State.tapes.end(); )
            {
                if (!g_PrevLaunchHealthy) { ++it; continue; }
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
                if (!g_PrevLaunchHealthy) { ++it; continue; }
                if (IsUniqueStaffConstantKey(it->first))
                {
                    ++it;
                    continue;
                }

                const auto mit = g_State.constantMisses.find(it->first);
                const std::int32_t misses =
                    (mit != g_State.constantMisses.end()) ? mit->second : 0;
                if (misses >= kConstantOrphanGraceLaunches)
                {
                    LogDebug("[Constants] \"%s\" (value %d) unreferenced for %d "
                             "launches - entry removed, value returned to the free "
                             "pool\n",
                        it->first.c_str(), it->second, misses);
                    if (mit != g_State.constantMisses.end())
                        g_State.constantMisses.erase(mit);
                    if (it->first.compare(0, kRewardLangKeyPrefixLen,
                                          kRewardLangKeyPrefix) != 0)
                        ++evicted;
                    it = g_State.constants.erase(it);
                    gcChanged = true;
                    continue;
                }
                ++it;
            }
            if (evicted != 0)
                Log("[V_FrameWorkState] WARNING: %zu persisted equip constant(s) "
                    "expired and returned their equipIds to the pool - every id "
                    "allocated this boot shifts, so saved loadout and R&D rows now "
                    "name different weapons; re-pick affected slots once\n",
                    evicted);

            for (auto it = g_State.equips.begin(); it != g_State.equips.end(); )
            {
                if (!g_PrevLaunchHealthy) { ++it; continue; }
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
                        LogDebug("[V_FrameWorkState] '%s' is orphaned but its "
                                 "equipId 0x%X is still referenced by a saved "
                                 "loadout - id kept reserved\n",
                            it->first.c_str(), it->second.equipId);
                        ++it;
                        continue;
                    }
                }
                if (it->second.misses >= kEquipOrphanGraceLaunches)
                {
                    LogDebug("[V_FrameWorkState] \"%s\" (developId %d, equipId "
                             "0x%X, partsType 0x%02X, selector 0x%02X) has not "
                             "registered for %d launches - entry removed, ids "
                             "returned to the pool\n",
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
                    || fi >= static_cast<std::int32_t>(equip::kMaxFlowSlots)
                    || fi == kNativeFlowSentinel || IsReservedFlowIndex(fi))
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

            if (g_FlusherStop)
            {
                WriteToDisk_NoLock();
                return;
            }

            ++g_CoalescedCount;
            g_SaveDueTick = GetTickCount64() + kCoalesceMs;

            if (!g_FlusherRunning)
            {
                g_FlusherRunning = true;
                g_FlusherThread = std::thread(&FlusherMain);
            }

            g_FlushCv.notify_one();
        }

        static void EmitAlignedRows(std::ostream& out,
                                    const std::vector<std::string>& keys,
                                    const std::vector<std::vector<std::string>>& rows)
        {
            std::size_t keyWidth = 0;
            std::vector<std::size_t> colWidth;
            for (std::size_t r = 0; r < keys.size(); ++r)
            {
                if (keys[r].size() > keyWidth) keyWidth = keys[r].size();
                if (colWidth.size() < rows[r].size())
                    colWidth.resize(rows[r].size(), 0);
                for (std::size_t c = 0; c < rows[r].size(); ++c)
                    if (rows[r][c].size() > colWidth[c])
                        colWidth[c] = rows[r][c].size();
            }
            if (keyWidth > 56) keyWidth = 56;

            for (std::size_t r = 0; r < keys.size(); ++r)
            {
                std::string body;
                for (std::size_t c = 0; c < rows[r].size(); ++c)
                {
                    if (colWidth[c] == 0) continue;
                    body.append(rows[r][c]);
                    if (rows[r][c].size() < colWidth[c])
                        body.append(colWidth[c] - rows[r][c].size(), ' ');
                    body.push_back(' ');
                }
                while (!body.empty() && body.back() == ' ')
                    body.pop_back();

                std::string key = keys[r];
                if (key.size() < keyWidth)
                    key.append(keyWidth - key.size(), ' ');
                out << "        " << key << " = { " << body << " },\n";
            }
        }

        static const char* ConstantSpaceNote(const std::string& tag)
        {
            if (tag == "WP")           return "gunBasic weapon slot per WP_ name";
            if (tag == "REWARDLANG32") return "TppReward LANG_ENUM index per langId";
            if (tag == "CUSTOMHEAD")   return "(slotByte << 16) | equipId per head name";
            return nullptr;
        }

        static void EmitConstantRows(
            std::ostream& out,
            const std::vector<std::pair<std::string, std::int32_t>>& rows,
            bool withNotes = true)
        {
            auto tagOf = [](const std::string& key)
            {
                const auto colon = key.find(':');
                return (colon == std::string::npos)
                    ? std::string() : key.substr(0, colon);
            };
            auto nameOf = [](const std::string& key)
            {
                const auto colon = key.find(':');
                return (colon == std::string::npos)
                    ? key : key.substr(colon + 1);
            };

            std::string tag;
            std::size_t keyWidth = 0;
            bool first = true;
            bool open = false;

            for (std::size_t r = 0; r < rows.size(); ++r)
            {
                const std::string rowTag = tagOf(rows[r].first);
                if (first || rowTag != tag)
                {
                    if (open)
                    {
                        out << "        },\n";
                        open = false;
                    }
                    if (!first) out << "\n";
                    tag = rowTag;
                    first = false;

                    keyWidth = 0;
                    for (std::size_t s = r; s < rows.size(); ++s)
                    {
                        if (tagOf(rows[s].first) != tag) break;
                        const std::size_t w = nameOf(rows[s].first).size() + 4;
                        if (w > keyWidth) keyWidth = w;
                    }
                    if (keyWidth > 60) keyWidth = 60;

                    if (!tag.empty())
                    {
                        const char* note =
                            withNotes ? ConstantSpaceNote(tag) : nullptr;
                        if (note)
                            out << "        -- " << note << "\n";
                        out << "        " << tag << " = {\n";
                        open = true;
                    }
                }

                std::string key = "[\"" + nameOf(rows[r].first) + "\"]";
                if (key.size() < keyWidth)
                    key.append(keyWidth - key.size(), ' ');
                out << (open ? "            " : "        ")
                    << key << " = " << rows[r].second << ",\n";
            }

            if (open)
                out << "        },\n";
        }

        static void WriteToDisk_NoLock()
        {
            EnsureSaveDirectory();

            const std::string tmpPath = std::string(kSavePath) + ".tmp";

            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                Log("[V_FrameWorkState] ERROR: could not write '%s' - custom-tape "
                    "ownership and save-index state will not persist\n", tmpPath.c_str());
                return;
            }

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

                std::vector<std::string> keys[4];
                std::vector<std::vector<std::string>> rows[4];
                for (const auto& kv : sorted)
                {
                    if (kv.second.developId == 0 && kv.second.flowIndex == 0 &&
                        kv.second.equipId == 0 && kv.second.subId == 0 &&
                        kv.second.partsType == 0 && kv.second.selector == 0 &&
                        kv.second.misses == 0 && kv.second.developed < 0 &&
                        !kv.second.isNew && !kv.second.devReqAnnounced)
                        continue;

                    std::vector<std::string> row(11);
                    auto cell = [&row](std::size_t i, const char* name,
                                       const std::string& value)
                    { row[i] = std::string(name) + " = " + value + ","; };

                    if (kv.second.developId != 0)
                        cell(0, "developId", std::to_string(kv.second.developId));
                    if (kv.second.flowIndex != 0)
                        cell(1, "flowIndex", std::to_string(kv.second.flowIndex));
                    if (kv.second.developed >= 0)
                        cell(2, "developed",
                             kv.second.developed == 1 ? "true" : "false");
                    if (kv.second.developId != 0)
                    {
                        if (!kv.second.isNew)
                            cell(3, "seen", "true");
                        if (!kv.second.devReqAnnounced)
                            cell(4, "announcePending", "true");
                    }
                    if (kv.second.equipId != 0)
                        cell(5, "equipId", std::to_string(kv.second.equipId));
                    if (kv.second.subId != 0)
                        cell(6, "subId", std::to_string(kv.second.subId));
                    if (kv.second.partsType != 0)
                        cell(7, "partsType", std::to_string(kv.second.partsType));
                    if (kv.second.selector != 0)
                        cell(8, "selector", std::to_string(kv.second.selector));
                    if (kv.second.misses != 0)
                        cell(9, "misses", std::to_string(kv.second.misses));
                    {
                        std::size_t last = 0;
                        for (std::size_t vi = 0;
                             vi < V_FrameWorkState::kPersistedVariantSelectorSlots; ++vi)
                            if (kv.second.variantSelectors[vi] != 0) last = vi + 1;
                        if (last != 0)
                        {
                            std::string csv = "\"";
                            for (std::size_t vi = 0; vi < last; ++vi)
                            {
                                if (vi) csv.push_back(',');
                                csv += std::to_string(
                                    static_cast<int>(kv.second.variantSelectors[vi]));
                            }
                            csv.push_back('"');
                            cell(10, "variantSelectors", csv);
                        }
                    }

                    int group = 3;
                    if (kv.second.kind == kRowKindOutfit
                        || kv.second.partsType != 0 || kv.second.selector != 0)
                        group = 2;
                    else if (kv.second.kind == kRowKindWeapon)
                        group = 1;
                    else if (kv.second.developId == 0)
                        group = 0;

                    keys[group].push_back("[\"" + kv.first + "\"]");
                    rows[group].push_back(std::move(row));
                }

                std::size_t liveConstants = 0;
                for (const auto& kv : g_State.constants)
                    if (kv.second != 0) ++liveConstants;

                out <<
"-- V_FrameWork state file - written by V_FrameWork.dll.\n";
                out << "--   equips " << keys[0].size()
                    << "   weapons " << keys[1].size()
                    << "   outfits " << keys[2].size();
                if (!keys[3].empty())
                    out << "   unclassified " << keys[3].size();
                out << "   tapes " << g_State.tapes.size()
                    << "   database " << g_State.bluePrints.size()
                    << "   constants " << liveConstants
                    << "   pinned " << g_State.pinnedEquipIds.size() << "\n\n";

                out << "return {\n\n";

                out << "    launchHealthy = "
                    << (g_State.launchHealthy ? "true" : "false") << ",\n\n";

                static const char* const kEquipTable[4] =
                    { "equips", "weapons", "outfits", "develop" };
                for (int g = 0; g < 4; ++g)
                {
                    if (g == 3 && keys[g].empty())
                        continue;
                    out << "    " << kEquipTable[g] << " = {\n";
                    EmitAlignedRows(out, keys[g], rows[g]);
                    out << "    },\n\n";
                }
            }


            {
                std::vector<std::pair<std::string, TapeEntry>> sorted;
                sorted.reserve(g_State.tapes.size());
                for (const auto& kv : g_State.tapes)
                    sorted.emplace_back(kv.first, kv.second);
                std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b) { return a.second.saveIndex < b.second.saveIndex; });

                std::vector<std::string> keys;
                std::vector<std::vector<std::string>> rows;
                for (const auto& kv : sorted)
                {
                    std::vector<std::string> row(4);
                    row[0] = "saveIndex = "
                           + std::to_string(static_cast<int>(kv.second.saveIndex)) + ",";
                    row[1] = std::string("owned = ")
                           + (kv.second.owned ? "true" : "false") + ",";
                    row[2] = std::string("new = ")
                           + (kv.second.isNew ? "true" : "false") + ",";
                    if (kv.second.misses != 0)
                        row[3] = "misses = " + std::to_string(kv.second.misses) + ",";
                    keys.push_back("[\"" + kv.first + "\"]");
                    rows.push_back(std::move(row));
                }

                out << "    tapes = {\n";
                EmitAlignedRows(out, keys, rows);
                out << "    },\n\n";
            }


            {
                std::vector<std::pair<std::string, BluePrintEntry>> bpSorted;
                bpSorted.reserve(g_State.bluePrints.size());
                for (const auto& kv : g_State.bluePrints)
                    bpSorted.emplace_back(kv.first, kv.second);
                std::sort(bpSorted.begin(), bpSorted.end(),
                    [](const auto& a, const auto& b) { return a.second.id < b.second.id; });

                std::vector<std::string> keys;
                std::vector<std::vector<std::string>> rows;
                for (const auto& kv : bpSorted)
                {
                    std::vector<std::string> row(5);
                    row[0] = "bluePrintId = " + std::to_string(kv.second.id) + ",";
                    row[1] = "pickupNumber = "
                           + std::to_string(bluePrint::PublicId(kv.second.id)) + ",";
                    row[2] = std::string("owned = ")
                           + (kv.second.owned ? "true" : "false") + ",";
                    if (kv.second.isNew)
                        row[3] = "new = true,";
                    if (kv.second.misses != 0)
                        row[4] = "misses = " + std::to_string(kv.second.misses) + ",";
                    keys.push_back("[\"" + kv.first + "\"]");
                    rows.push_back(std::move(row));
                }

                out << "    DataBase = {\n";
                EmitAlignedRows(out, keys, rows);
                out << "    },\n\n";
            }

            {
                std::vector<std::pair<std::string, std::int32_t>> sorted;
                sorted.reserve(g_State.constants.size());
                for (const auto& kv : g_State.constants)
                    if (kv.second != 0)
                        sorted.emplace_back(kv.first, kv.second);
                std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

                out << "    constants = {\n";
                EmitConstantRows(out, sorted);
                out << "    },\n\n";
            }

            {
                const bool countMisses = CanCountConstantMisses();
                std::vector<std::pair<std::string, std::int32_t>> sorted;
                sorted.reserve(g_State.constants.size());
                for (const auto& kv : g_State.constants)
                {
                    if (IsUniqueStaffConstantKey(kv.first))
                        continue;
                    if (g_ConstantsTouched.find(kv.first) != g_ConstantsTouched.end())
                        continue;
                    const auto mit = g_State.constantMisses.find(kv.first);
                    const std::int32_t loaded =
                        (mit != g_State.constantMisses.end()) ? mit->second : 0;
                    const std::int32_t misses = countMisses ? loaded + 1 : loaded;
                    if (misses != 0)
                        sorted.emplace_back(kv.first, misses);
                }
                std::sort(sorted.begin(), sorted.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });

                out << "    constantMisses = {\n";
                EmitConstantRows(out, sorted, false);
                out << "    },\n\n";
            }

            {
                std::vector<std::int32_t> sorted(g_State.pinnedEquipIds.begin(),
                                                 g_State.pinnedEquipIds.end());
                std::sort(sorted.begin(), sorted.end());
                std::size_t idWidth = 0;
                for (const std::int32_t id : sorted)
                {
                    const std::size_t w = std::to_string(id).size() + 4;
                    if (w > idWidth) idWidth = w;
                }

                out << "    loadoutPinnedIds = {\n";
                for (const std::int32_t id : sorted)
                {
                    std::string cell = "[\"" + std::to_string(id) + "\"]";
                    if (cell.size() < idWidth)
                        cell.append(idWidth - cell.size(), ' ');
                    out << "        " << cell << " = " << id << ",";
                    for (const auto& kv : g_State.equips)
                        if (kv.second.equipId == id)
                        {
                            out << "   -- " << kv.first;
                            break;
                        }
                    out << "\n";
                }
                out << "    },\n";
            }

            if (!g_VanillaIdentityIds.empty())
            {
                std::vector<std::int32_t> sorted(g_VanillaIdentityIds.begin(),
                                                 g_VanillaIdentityIds.end());
                std::sort(sorted.begin(), sorted.end());

                out << "\n    vanillaEquipIdentity = {\n";
                for (std::size_t i = 0; i < sorted.size(); ++i)
                {
                    if ((i % 16) == 0)
                        out << "        ";
                    out << sorted[i] << ",";
                    if ((i % 16) == 15 || i + 1 == sorted.size())
                        out << "\n";
                    else
                        out << " ";
                }
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
                if (c == '"' || c == '[' || c == ']' || c == '\\'
                    || c == '\n' || c == '\r')
                    return false;
            }
            return true;
        }

        static bool GuardStateKey(const char* key, const char* who)
        {
            bool bad = false;
            for (const char* p = key; *p; ++p)
                if (*p == '\n' || *p == '\r' || *p == '"' || *p == '\\')
                { bad = true; break; }
            if (!bad)
                return true;
            Log("[V_FrameWorkState] ERROR: %s rejected key '%s' - a line break, a "
                "quote or a backslash inside a key splits the key on reload and "
                "stops the state file parsing as Lua; nothing keyed on it "
                "persists\n",
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
            static bool s_built = false;
            static int s_warned = 0;
            if (s_built)
                return;

            std::vector<std::int32_t> ids(4096);
            const std::size_t n =
                equip::CollectVanillaIdentityEquipIds(ids.data(), ids.size());
            if (n < 64)
            {
                if (s_warned++ < 4)
                Log("[V_FrameWorkState] WARNING: the vanilla equip-identity set is "
                    "still empty at first allocation (develop lookup returned %zu "
                    "id(s), below the %d-id floor) - custom ids can land on rows "
                    "the game's own tables own\n",
                    n, 64);
                return;
            }
            s_built = true;
            for (std::size_t i = 0; i < n; ++i)
                if (ids[i] > 0)
                    g_VanillaIdentityIds.insert(ids[i]);
        }

        static void NoteIdentitySetLive_NoLock()
        {
            static bool s_said = false;
            if (s_said || g_VanillaIdentityIds.empty())
                return;
            s_said = true;
            Log("[V_FrameWorkState] vanilla equip-identity set LIVE: %zu id(s) - "
                "collision checks against vanilla rows are in effect\n",
                g_VanillaIdentityIds.size());
        }

        static bool IsEquipIdHeldByAnotherKey_NoLock(std::int32_t equipId,
                                                     const char* key)
        {
            for (const auto& kv : g_State.equips)
                if (kv.second.equipId == equipId
                    && (!key || kv.first != key))
                    return true;
            return false;
        }


        static std::int32_t AllocateNextFreeEquipId_NoLock(std::int32_t minimum,
                                                           bool isWeapon,
                                                           const char* key)
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
            NoteIdentitySetLive_NoLock();

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
                result = EquipIdCompression::FindLowestFreeExtendedEquipId(
                    [key](std::int32_t id) {
                        return IsEquipIdHeldByAnotherKey_NoLock(id, key);
                    });
                if (result >= 0)
                    LogDebug("[V_FrameWorkState] AllocateNextFreeEquipId: native "
                             "bands full above floor=0x%X - allocated EXTENDED "
                             "equipId 0x%X (DLL table, served via hooked "
                             "accessors)\n",
                        floor, result);
                else
                    LogDebug("[V_FrameWorkState] AllocateNextFreeEquipId: native "
                             "bands and the extended 0x289-0x3FF range are "
                             "exhausted above floor=0x%X - allocation failed\n", floor);
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

        if (!g_PrevLaunchHealthy)
            Log("[V_FrameWorkState] the previous launch did not install cleanly, so "
                "nothing it failed to register counts as an uninstall - orphan "
                "expiry is frozen for this launch and every persisted id is kept\n");

        g_State.launchHealthy = false;
        g_State.dirty = true;
        WriteToDisk_NoLock();
    }

    void NoteInstallOutcome(bool allInstalled)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_State.launchHealthy == allInstalled)
            return;
        g_State.launchHealthy = allInstalled;
        g_State.dirty = true;
        WriteToDisk_NoLock();
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
        {
            Log("[V_FrameWorkState] WARNING: the state file was locked at process "
                "exit, so this launch could not be recorded - a constant whose mod "
                "is gone keeps its id for another launch\n");
            return;
        }

        g_ExitSave = true;
        g_SaveDueTick = 0;
        g_CoalescedCount = 0;
        WriteToDisk_NoLock();
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
            const bool isExtended = EquipIdCompression::IsExtendedEquipId(persisted);
            const std::int32_t slot = EquipIdCompression::ComputeCompressed(persisted);
            const bool bandOk = isExtended || isWeapon
                || slot < EquipIdCompression::kWeaponBandFirst;
            if (!bandOk)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' is in "
                         "the weapon band but the item is not a weapon - native "
                         "GetEquipType would return 0; reallocating into the item "
                         "band\n", persisted, key);
            if (g_VanillaIdentityIds.empty())
                BuildVanillaIdentitySet_NoLock();
            NoteIdentitySetLive_NoLock();
            const bool identityClash =
                g_VanillaIdentityIds.count(persisted) != 0;
            if (identityClash)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' is a "
                         "vanilla equip identity the game still owns - keeping it "
                         "overwrites that row and the prep list renders the vanilla "
                         "entry with this item's damage tag; reallocating\n",
                    persisted, key);
            const bool itemListClash = isWeapon && IsItemCategoryRow(persisted);
            if (itemListClash)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' is a "
                         "weapon in the vanilla item-category span 0x%X-0x%X, which "
                         "the list enumerates without filtering on type - it would "
                         "render as a supply item; reallocating\n",
                    persisted, key, kItemCategoryRowFirst, kItemCategoryRowLast);
            const bool slotFree = isExtended
                ? !EquipIdCompression::IsExtendedEquipIdUsed(persisted)
                : !EquipIdCompression::IsCompressedSlotUsed(slot);
            if (!sessionTaken && bandOk && !identityClash && !itemListClash
                && slotFree)
            {
                g_SessionEquipIds[key] = persisted;
                NoteClaimedEquipId_NoLock(persisted);
                if (isExtended)
                    EquipIdCompression::MarkExtendedEquipIdUsed(persisted);
                if (pit->second.misses != 0)
                {
                    pit->second.misses = 0;
                    g_State.dirty = true;
                    SaveToDisk_NoLock();
                }
                outEquipId = persisted;
                return true;
            }
            if (bandOk)
                LogDebug("[V_FrameWorkState] persisted equipId 0x%X for '%s' is no "
                         "longer free (vanilla layout change or conflict) - "
                         "reallocating; old loadout references are healed or "
                         "blanked\n", persisted, key);
        }

        const std::int32_t newId = AllocateNextFreeEquipId_NoLock(minimumId, isWeapon, key);
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
        if (equipId <= 0 || equipId > 0xFFFF)
            return false;
        return (g_ClaimedEquipBits[equipId >> 6].load(std::memory_order_relaxed)
                & (1ull << (equipId & 63))) != 0;
    }

    void SetVanillaIdentityEquipIds(const std::int32_t* equipIds,
                                    std::size_t count)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        std::unordered_set<std::int32_t> fresh;
        for (std::size_t i = 0; i < count; ++i)
            if (equipIds[i] > 0)
                fresh.insert(equipIds[i]);
        if (fresh.empty() || fresh == g_VanillaIdentityIds)
            return;

        g_VanillaIdentityIds.swap(fresh);

        int cleared = 0;
        std::int32_t firstId = 0;
        std::string  firstKey;
        for (auto& kv : g_State.equips)
        {
            if (kv.second.equipId == 0
                || g_VanillaIdentityIds.count(kv.second.equipId) == 0)
                continue;
            if (cleared == 0)
            {
                firstId  = kv.second.equipId;
                firstKey = kv.first;
            }
            kv.second.equipId = 0;
            ++cleared;
        }

        if (cleared != 0)
            Log("[V_FrameWorkState] %d persisted equipId(s) sit on rows the game's own "
                "develop tree still owns (first: '%s' = 0x%X) - the vanilla row renders "
                "with this item's damage tag, so those ids were dropped and are "
                "reallocated on the next launch\n",
                cleared, firstKey.c_str(), firstId);

        g_State.dirty = true;
        SaveToDisk_NoLock();
    }

    static bool IsOwnedEquipId_NoLock(std::int32_t equipId)
    {
        for (const auto& kv : g_SessionEquipIds)
            if (kv.second == equipId) return true;
        for (const auto& kv : g_State.equips)
            if (kv.second.equipId == equipId) return true;
        return false;
    }

    static void EnsurePinSetFresh_NoLock()
    {
        if (g_PinSetFreshThisSession)
            return;
        g_PinSetFreshThisSession = true;
        g_SessionPinnedIds.clear();
        g_StickyPinnedIds.clear();
        if (!g_State.pinnedEquipIds.empty())
        {
            g_State.pinnedEquipIds.clear();
            g_State.dirty = true;
        }
    }

    static void AddPin_NoLock(std::int32_t equipId, bool sticky)
    {
        if (equipId <= 0) return;
        LoadFromDisk_NoLock();
        if (!IsOwnedEquipId_NoLock(equipId))
            return;
        EnsurePinSetFresh_NoLock();
        if (sticky)
            g_StickyPinnedIds.insert(equipId);
        if (g_SessionPinnedIds.insert(equipId).second)
        {
            g_State.pinnedEquipIds.insert(equipId);
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
    }

    void NotePinnedEquipId(std::int32_t equipId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        AddPin_NoLock(equipId, false);
    }

    void NoteStickyPinnedEquipId(std::int32_t equipId)
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        AddPin_NoLock(equipId, true);
    }

    void UnpinEquipId(std::int32_t equipId)
    {
        if (equipId <= 0) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (!g_PinSetFreshThisSession)
            return;
        if (g_StickyPinnedIds.find(equipId) != g_StickyPinnedIds.end())
            return;
        LoadFromDisk_NoLock();
        bool changed = g_SessionPinnedIds.erase(equipId) != 0;
        if (g_State.pinnedEquipIds.erase(equipId) != 0)
            changed = true;
        if (changed)
        {
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

    void SetRowKind(const char* key, std::uint8_t kind)
    {
        if (!key || !key[0] || kind == kRowKindUnknown)
            return;
        if (!GuardStateKey(key, "SetRowKind"))
            return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto& e = g_State.equips[key];
        if (e.kind != kind)
        {
            e.kind = kind;
            g_State.dirty = true;
            SaveToDisk_NoLock();
        }
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
            if (kv.second.developId != 0)
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

    bool IsDevelopedByFlowRowDevelopId(std::int32_t developId, bool& developed)
    {
        if (developId == 0)
            return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.equips)
        {
            if (kv.second.developId != developId || kv.second.developed < 0)
                continue;
            developed = (kv.second.developed == 1);
            return true;
        }
        return false;
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
            Log("[V_FrameWorkState] WARNING: '%s' held develop flow index %d, which "
                "the loadout commit reads as a gunsmith slot (0x3FD-0x3FF resolve "
                "to equip 871/873/875) - picking it would equip a pseudo-weapon; "
                "reallocating outside the reserved range\n",
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
            LogDebug("[V_FrameWorkState] ResolveOrCreateFlowIndex REFUSED '%s': the "
                     "native flow array holds %d rows and indices %d..%d are all "
                     "allocated - registering would corrupt memory past the array; "
                     "the item stays out of R&D until develop-row paging frees "
                     "space\n",
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
            Log("[CustomTapes] ERROR: the custom-tape save-index pool [300-32000] "
                "is full - uninstall unused tape mods; no more custom tapes can "
                "register\n");
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

    bool ResolveOrCreateBluePrintId(const char* key, std::int32_t& outId)
    {
        outId = 0;
        if (!key || !key[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();

        auto it = g_State.bluePrints.find(key);
        if (it != g_State.bluePrints.end() && it->second.id > 0)
        {
            outId = it->second.id;
            if (it->second.misses != 0)
            {
                it->second.misses = 0;
                g_State.dirty = true;
                SaveToDisk_NoLock();
            }
            return true;
        }

        std::unordered_set<std::int32_t> used;
        for (const auto& kv : g_State.bluePrints)
            if (kv.second.id > 0)
                used.insert(kv.second.id);

        std::int32_t value = 1;
        while (used.find(value) != used.end())
            ++value;

        g_State.bluePrints[key].id = value;
        g_State.dirty = true;
        SaveToDisk_NoLock();
        outId = value;
        return true;
    }

    std::int32_t GetBluePrintId(const char* key)
    {
        if (!key || !key[0]) return 0;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.bluePrints.find(key);
        return (it != g_State.bluePrints.end()) ? it->second.id : 0;
    }

    void ForEachBluePrint(void (*fn)(const char* key, std::int32_t id, bool owned))
    {
        if (!fn) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        for (const auto& kv : g_State.bluePrints)
            fn(kv.first.c_str(), kv.second.id, kv.second.owned);
    }

    void SetBluePrintOwned(const char* key, bool owned)
    {
        if (!key || !key[0]) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.bluePrints.find(key);
        if (it != g_State.bluePrints.end() && it->second.owned == owned)
            return;
        g_State.bluePrints[key].owned = owned;
        g_State.dirty = true;
        SaveToDisk_NoLock();
    }

    bool GetBluePrintOwned(const char* key)
    {
        if (!key || !key[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.bluePrints.find(key);
        return (it != g_State.bluePrints.end()) ? it->second.owned : false;
    }

    void SetBluePrintNew(const char* key, bool isNew)
    {
        if (!key || !key[0]) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.bluePrints.find(key);
        if (it != g_State.bluePrints.end() && it->second.isNew == isNew)
            return;
        g_State.bluePrints[key].isNew = isNew;
        g_State.dirty = true;
        SaveToDisk_NoLock();
    }

    bool GetBluePrintNew(const char* key)
    {
        if (!key || !key[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
        auto it = g_State.bluePrints.find(key);
        return (it != g_State.bluePrints.end()) ? it->second.isNew : false;
    }

    void SetTapeOwned(const char* key, bool owned)
    {
        if (!key || !key[0]) return;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
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
        LoadFromDisk_NoLock();
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
        LoadFromDisk_NoLock();
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
        LoadFromDisk_NoLock();
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
        LoadFromDisk_NoLock();
        auto it = g_State.tapes.find(key);
        return (it != g_State.tapes.end()) ? it->second.owned : false;
    }

    bool GetTapeNew(const char* key)
    {
        if (!key || !key[0]) return false;
        std::lock_guard<std::mutex> lock(g_Mutex);
        LoadFromDisk_NoLock();
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
        g_State.bluePrints.clear();
        g_State.constants.clear();
        g_State.constantMisses.clear();
        g_State.pinnedEquipIds.clear();
        g_ConstantsTouched.clear();
        g_ExitSave = false;
        g_SessionPinnedIds.clear();
        g_StickyPinnedIds.clear();
        g_PinSetFreshThisSession = false;
        g_SessionEquipIds.clear();
        g_SessionFlowIndices.clear();
        g_OldFlowLayout.clear();
        g_PendingDevelopedResets.clear();
    }
}
