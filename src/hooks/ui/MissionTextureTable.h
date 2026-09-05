#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

class MissionTextureTable
{
public:
    void Set(std::uint32_t missionCode, std::uint64_t textureHash)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (missionCode == 0)
            m_global = textureHash;
        else if (textureHash == 0)
            m_perMission.erase(missionCode);
        else
            m_perMission[missionCode] = textureHash;
    }

    void Clear(std::uint32_t missionCode)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (missionCode == 0)
        {
            m_global = 0;
            m_perMission.clear();
        }
        else
        {
            m_perMission.erase(missionCode);
        }
    }

    std::uint64_t Resolve(std::uint32_t missionCode) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_perMission.find(missionCode);
        if (it != m_perMission.end())
            return it->second;
        return m_global;
    }

private:
    mutable std::mutex m_mutex;
    std::uint64_t m_global = 0;
    std::unordered_map<std::uint32_t, std::uint64_t> m_perMission;
};
