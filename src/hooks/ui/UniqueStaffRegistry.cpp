#include "pch.h"
#include "UniqueStaffRegistry.h"

#include "log.h"
#include "V_FrameWorkState.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace uniqueStaff
{
    namespace
    {
        static constexpr const char* kSpaceTag     = "USTAFF";
        static constexpr std::size_t kMaxKeyLength = 96;

        struct FreeInterval
        {
            std::int32_t first;
            std::int32_t last;
        };

        static constexpr FreeInterval kFreeIntervals[] =
        {
            { 200, 208 },
            { 180, 183 },
            { 178, 178 },
            { 167, 176 },
            { 157, 165 },
            { 112, 155 },
        };

        static std::mutex                          g_Mutex;
        static std::map<std::string, std::int32_t> g_IdByKey;
        static std::set<std::int32_t>              g_Claimed;

        static void CollectClaimed(const char*, std::int32_t value)
        {
            g_Claimed.insert(value);
        }

        static std::int32_t g_LookupId = 0;
        static std::string  g_LookupHit;

        static void CollectNameForId(const char* name, std::int32_t value)
        {
            if (value == g_LookupId && name && g_LookupHit.empty())
                g_LookupHit = name;
        }

        static void RefreshClaimed_NoLock()
        {
            g_Claimed.clear();
            V_FrameWorkState::ForEachPersistedConstant(kSpaceTag, &CollectClaimed);
            for (const auto& entry : g_IdByKey)
                g_Claimed.insert(entry.second);
        }

        static std::int32_t Lookup_NoLock(const char* key)
        {
            auto cached = g_IdByKey.find(key);
            if (cached != g_IdByKey.end())
                return cached->second;

            const std::int32_t stored =
                V_FrameWorkState::GetPersistedConstant(kSpaceTag, key);
            if (stored > 0)
            {
                g_IdByKey[key] = stored;
                return stored;
            }
            return 0;
        }
    }

    std::int32_t PoolSize()
    {
        std::int32_t total = 0;
        for (const FreeInterval& interval : kFreeIntervals)
            total += interval.last - interval.first + 1;
        return total;
    }

    bool IsValidKey(const char* key)
    {
        if (!key || !key[0])
            return false;

        const char head = key[0];
        if (!((head >= 'A' && head <= 'Z') || (head >= 'a' && head <= 'z') || head == '_'))
            return false;

        std::size_t length = 0;
        for (const char* p = key; *p; ++p, ++length)
        {
            const char c = *p;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                         || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
            if (!ok)
                return false;
        }
        return length <= kMaxKeyLength;
    }

    std::int32_t Find(const char* key)
    {
        if (!IsValidKey(key))
            return 0;

        std::lock_guard<std::mutex> lock(g_Mutex);
        return Lookup_NoLock(key);
    }

    std::int32_t Register(const char* key)
    {
        if (!IsValidKey(key))
            return 0;

        std::lock_guard<std::mutex> lock(g_Mutex);

        const std::int32_t known = Lookup_NoLock(key);
        if (known > 0)
            return known;

        RefreshClaimed_NoLock();

        for (const FreeInterval& interval : kFreeIntervals)
        {
            for (std::int32_t id = interval.last; id >= interval.first; --id)
            {
                if (g_Claimed.find(id) != g_Claimed.end())
                    continue;

                V_FrameWorkState::SetPersistedConstant(kSpaceTag, key, id);
                g_IdByKey[key] = id;
                return id;
            }
        }
        return 0;
    }

    const char* KeyHoldingId(std::int32_t typeId)
    {
        if (typeId <= 0)
            return nullptr;

        std::lock_guard<std::mutex> lock(g_Mutex);

        g_LookupId = typeId;
        g_LookupHit.clear();

        for (const auto& entry : g_IdByKey)
        {
            if (entry.second == typeId)
            {
                g_LookupHit = entry.first;
                return g_LookupHit.c_str();
            }
        }

        V_FrameWorkState::ForEachPersistedConstant(kSpaceTag, &CollectNameForId);
        return g_LookupHit.empty() ? nullptr : g_LookupHit.c_str();
    }

    std::int32_t FreeSlotCount()
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        RefreshClaimed_NoLock();

        std::int32_t free = 0;
        for (const FreeInterval& interval : kFreeIntervals)
            for (std::int32_t id = interval.first; id <= interval.last; ++id)
                if (g_Claimed.find(id) == g_Claimed.end())
                    ++free;
        return free;
    }
}
