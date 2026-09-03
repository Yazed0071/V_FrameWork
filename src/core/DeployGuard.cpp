#include "pch.h"
#include "DeployGuard.h"
#include "log.h"
#include "V_FrameWorkState.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace DeployGuard
{
    namespace
    {
        constexpr std::uint32_t kSlots = 4;

        std::mutex        g_Mutex;
        std::int32_t      g_Loadout[kSlots][3] = {};
        constexpr std::uint32_t kNoCode      = 0xFFFFFFFFu;
        constexpr std::uint32_t kTitleCode   = 1u;
        constexpr std::uint32_t kMotherBase  = 40010u;

        std::atomic<bool> g_AwaitingPlayer{ false };
        std::atomic<bool> g_DropExtended{ false };
        std::atomic<bool> g_DropPermanent{ false };
        std::atomic<std::uint32_t> g_LastCode{ kNoCode };
        std::atomic<std::uint32_t> g_HungCode{ kNoCode };

        bool MarkerPath(char* out, std::size_t cap)
        {
            char path[MAX_PATH]{};
            if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
                return false;
            char* lastSlash = std::strrchr(path, '\\');
            if (!lastSlash)
                return false;
            *(lastSlash + 1) = '\0';
            if (strcat_s(path, "mod\\V_FrameWork\\deploy_in_progress.txt") != 0)
                return false;
            return strcpy_s(out, cap, path) == 0;
        }

        void WriteMarker(std::uint32_t code)
        {
            char path[MAX_PATH]{};
            if (!MarkerPath(path, sizeof(path)))
                return;

            FILE* f = nullptr;
            if (fopen_s(&f, path, "w") != 0 || !f)
                return;

            {
                std::lock_guard<std::mutex> lock(g_Mutex);
                fprintf(f, "mission=%u\n", code);
                for (std::uint32_t s = 0; s < kSlots; ++s)
                    fprintf(f, "slot%u=%d,%d,%d\n", s,
                        g_Loadout[s][0], g_Loadout[s][1], g_Loadout[s][2]);
            }
            fclose(f);

            static std::atomic<bool> s_said{ false };
            if (!s_said.exchange(true))
                Log("[DeployGuard] armed: %s tracks the live mission code and "
                    "loadout; it is deleted once the player's weapon is set up and "
                    "again on a clean exit, so a leftover copy at boot means the "
                    "game died mid-load\n", path);
        }

        void DeleteMarker()
        {
            char path[MAX_PATH]{};
            if (MarkerPath(path, sizeof(path)))
                DeleteFileA(path);
        }
    }

    void Init()
    {
        char path[MAX_PATH]{};
        if (!MarkerPath(path, sizeof(path)))
            return;

        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f)
            return;

        Log("[DeployGuard] the previous session left a deploy marker - that run "
            "entered a mission load and never reached the player's weapon setup. "
            "The loadout it hung on:\n");

        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            const std::size_t n = std::strlen(line);
            if (n && line[n - 1] == '\n')
                line[n - 1] = '\0';
            unsigned hung = 0;
            if (sscanf_s(line, "mission=%u", &hung) == 1)
                g_HungCode.store(static_cast<std::uint32_t>(hung),
                                 std::memory_order_relaxed);
            Log("[DeployGuard]   %s\n", line);
        }
        fclose(f);
        DeleteFileA(path);

        g_DropExtended.store(true, std::memory_order_relaxed);
        Log("[DeployGuard] extended equipIds are hidden from the loadout the engine "
            "builds so the deploy can complete - those slots stay empty until a "
            "load finishes; the SAVED loadout keeps them and the hide lifts once a "
            "load passes the point the last run died at\n");
    }

    void NoteLoadoutSlot(std::uint32_t slot, const std::int32_t* ids)
    {
        if (slot >= kSlots || !ids)
            return;

        bool changed = false;
        std::int32_t removed[3] = {};
        int nRemoved = 0;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            for (int i = 0; i < 3; ++i)
            {
                if (g_Loadout[slot][i] == ids[i])
                    continue;
                if (g_Loadout[slot][i] > 0)
                    removed[nRemoved++] = g_Loadout[slot][i];
                g_Loadout[slot][i] = ids[i];
                changed = true;
            }
            for (int r = 0; r < nRemoved; ++r)
            {
                for (std::uint32_t s = 0; s < kSlots && removed[r] != 0; ++s)
                    for (int i = 0; i < 3; ++i)
                        if (g_Loadout[s][i] == removed[r])
                        {
                            removed[r] = 0;
                            break;
                        }
            }
        }
        if (!changed)
            return;

        for (int i = 0; i < 3; ++i)
            V_FrameWorkState::NotePinnedEquipId(ids[i]);
        for (int r = 0; r < nRemoved; ++r)
            if (removed[r] > 0)
                V_FrameWorkState::UnpinEquipId(removed[r]);

        const std::uint32_t code = g_LastCode.load(std::memory_order_relaxed);
        if (code != 0xFFFFFFFFu)
            WriteMarker(code);
    }

    void OnMissionCode(std::uint32_t code)
    {
        const std::uint32_t prev = g_LastCode.exchange(code, std::memory_order_relaxed);
        if (prev == code)
            return;
        g_AwaitingPlayer.store(true, std::memory_order_relaxed);
        WriteMarker(code);
    }

    void OnPlayerLive()
    {
        if (g_AwaitingPlayer.exchange(false, std::memory_order_relaxed))
            DeleteMarker();

        if (!g_DropExtended.load(std::memory_order_relaxed))
            return;

        const std::uint32_t code = g_LastCode.load(std::memory_order_relaxed);
        const std::uint32_t hung = g_HungCode.load(std::memory_order_relaxed);
        if (code == kNoCode)
            return;

        const bool clearedTheHang = (hung != kNoCode && code == hung);
        const bool inFieldMission = (code != kTitleCode && code != kMotherBase);
        if (!clearedTheHang && !inFieldMission)
            return;

        if (g_DropExtended.exchange(false, std::memory_order_relaxed))
            Log("[DeployGuard] mission %u reached the player's weapon setup - the "
                "point the previous run died at - so the extended equipId hide is "
                "lifted for this session. It stays armed on the title screen and "
                "Mother Base, where the deploy it covers has not happened yet\n", code);
    }

    void OnCleanExit()
    {
        DeleteMarker();
    }

    void ForceDropExtendedIds()
    {
        g_DropPermanent.store(true, std::memory_order_relaxed);
    }

    bool ShouldDropExtendedIds()
    {
        return g_DropPermanent.load(std::memory_order_relaxed)
            || g_DropExtended.load(std::memory_order_relaxed);
    }
}
