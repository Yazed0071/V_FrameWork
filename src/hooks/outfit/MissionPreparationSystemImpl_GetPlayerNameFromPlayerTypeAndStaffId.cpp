#include "pch.h"

#include "MissionPreparationSystemImpl_GetPlayerNameFromPlayerTypeAndStaffId.h"

#include "OutfitRegistry.h"

#include <atomic>
#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    using GetPlayerNameFromPlayerTypeAndStaffId_t =
        const char* (__fastcall*)(void* self, int playerType,
                                  std::uint64_t staffId);
    using LangIdToKey_t = void* (__fastcall*)(std::uint64_t* out,
                                              const char* langId);
    using GetLangText_t = const char* (__fastcall*)(std::uint64_t key);

    static GetPlayerNameFromPlayerTypeAndStaffId_t g_OrigGetPlayerName = nullptr;
    static bool g_Installed = false;

    constexpr std::size_t kNameBufferOffset = 0x2A0;
    constexpr std::size_t kNameBufferSize   = 0x36;

    constexpr const char* kSnakeFallbackLangId = "name_chara_snake_e";

    constexpr const char* kOcelotNameLangIds[] = { "name_chara_ocelot_e" };
    constexpr const char* kQuietNameLangIds[]  = { "marker_chara_quiet",
                                                   "marker_quiet",
                                                   "cast_quiet" };

    static const char* SafeGetLangText(GetLangText_t fn, std::uint64_t key)
    {
        __try { return fn(key); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static bool SafeLangIdToKey(LangIdToKey_t fn, const char* langId,
                                std::uint64_t* out)
    {
        __try { fn(out, langId); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool TextEquals(const char* a, const char* b)
    {
        if (!a || !b)
            return false;
        while (*a && *b && *a == *b) { ++a; ++b; }
        return *a == *b;
    }

    static const char* LangText(const char* langId)
    {
        auto toKey = reinterpret_cast<LangIdToKey_t>(
            ResolveGameAddress(gAddr.Ui_LangIdToKey));
        auto getText = reinterpret_cast<GetLangText_t>(
            ResolveGameAddress(gAddr.Ui_GetLangText));
        if (!toKey || !getText || !langId)
            return nullptr;

        std::uint64_t key = 0;
        if (!SafeLangIdToKey(toKey, langId, &key) || !key)
            return nullptr;

        const char* text = SafeGetLangText(getText, key);
        if (!text || !text[0] || TextEquals(text, langId))
            return nullptr;
        return text;
    }

    static const char* ResolveUniqueCharacterName(int playerType)
    {
        const char* const* ids   = nullptr;
        std::size_t        count = 0;

        if (playerType == outfit::kPlayerType_Ocelot)
        {
            ids   = kOcelotNameLangIds;
            count = sizeof(kOcelotNameLangIds) / sizeof(kOcelotNameLangIds[0]);
        }
        else if (playerType == outfit::kPlayerType_Quiet)
        {
            ids   = kQuietNameLangIds;
            count = sizeof(kQuietNameLangIds) / sizeof(kQuietNameLangIds[0]);
        }

        for (std::size_t i = 0; i < count; ++i)
            if (const char* text = LangText(ids[i]))
                return text;

        return nullptr;
    }

    static bool SafeWriteNameBuffer(void* self, const char* text)
    {
        __try
        {
            char* buffer = reinterpret_cast<char*>(self) + kNameBufferOffset;
            std::size_t i = 0;
            for (; i + 1 < kNameBufferSize && text[i]; ++i)
                buffer[i] = text[i];
            buffer[i] = 0;
            return i != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static const char* __fastcall hkGetPlayerNameFromPlayerTypeAndStaffId(
        void* self, int playerType, std::uint64_t staffId)
    {
        const char* vanilla = g_OrigGetPlayerName(self, playerType, staffId);

        if (!self
            || !outfit::IsUniqueCharacterPlayerType(
                   static_cast<std::uint8_t>(playerType)))
            return vanilla;

        const char* fallback = LangText(kSnakeFallbackLangId);
        if (vanilla && vanilla[0] && !TextEquals(vanilla, fallback))
            return vanilla;

        const char* name = ResolveUniqueCharacterName(playerType);
        if (!name || !SafeWriteNameBuffer(self, name))
        {
            static std::atomic<int> s_loggedPlayerType{ -1 };
            if (s_loggedPlayerType.exchange(playerType) != playerType)
                Log("[UniqueCharName] no character name resolved for player "
                    "type %d, so the Sortie Prep CHARACTER row keeps the Snake "
                    "name the staff roster lookup fell back to\n", playerType);
            return vanilla;
        }

        return reinterpret_cast<const char*>(
            reinterpret_cast<std::uint8_t*>(self) + kNameBufferOffset);
    }
}

namespace outfit
{
    bool Install_UniqueCharacterName_Hook()
    {
        if (g_Installed)
            return true;

        void* target = ResolveGameAddress(
            gAddr.MissionPrepSystem_GetPlayerNameFromPlayerTypeAndStaffId);
        if (!target)
        {
            Log("[UniqueCharName] GetPlayerNameFromPlayerTypeAndStaffId is not "
                "ported for %s, so the Sortie Prep CHARACTER row still reads "
                "Snake while playing as Ocelot or Quiet\n",
                GetGameBuildName(gGameBuild));
            return false;
        }

        if (!CreateAndEnableHook(
                target,
                reinterpret_cast<void*>(
                    &hkGetPlayerNameFromPlayerTypeAndStaffId),
                reinterpret_cast<void**>(&g_OrigGetPlayerName)))
        {
            Log("[UniqueCharName] GetPlayerNameFromPlayerTypeAndStaffId hook "
                "FAIL at %p, so the Sortie Prep CHARACTER row still reads Snake "
                "while playing as Ocelot or Quiet\n", target);
            g_OrigGetPlayerName = nullptr;
            return false;
        }

        g_Installed = true;
        return true;
    }

    void Uninstall_UniqueCharacterName_Hook()
    {
        if (!g_Installed)
            return;

        DisableAndRemoveHook(ResolveGameAddress(
            gAddr.MissionPrepSystem_GetPlayerNameFromPlayerTypeAndStaffId));
        g_OrigGetPlayerName = nullptr;
        g_Installed = false;
    }
}
