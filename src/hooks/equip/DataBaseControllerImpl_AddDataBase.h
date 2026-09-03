#pragma once

#include <cstdint>

struct lua_State;

namespace bluePrintDb
{
    struct Documentation
    {
        std::uint64_t nameHash  = 0;
        std::uint64_t iconHash  = 0;
        std::uint64_t imageHash = 0;
        std::uint64_t infoHash  = 0;
        std::uint8_t  tab       = 2;
    };

    void EnsureAddDataBaseHook(void* controller);
    void RemoveAddDataBaseHook();

    bool TryGetDocumentation(std::int32_t publicId, Documentation& out);
    bool SetDocumentation(std::int32_t publicId, const Documentation& doc);

    void ForEachDocumentedId(std::uint8_t tab, void (*fn)(std::int32_t publicId));

    bool AnnounceBluePrintObtained(lua_State* L, std::int32_t publicId);
}
