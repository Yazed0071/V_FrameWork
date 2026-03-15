#pragma once

#include <cstdint>
#include <string>

namespace FoxHashes
{
    bool Resolve();

    std::string NormalizeAssetPath(const std::string& path);

    uint32_t StrCode32(const char* text);
    uint32_t StrCode32(const std::string& text);

    uint64_t StrCode64(const char* text);
    uint64_t StrCode64(const std::string& text);

    uint64_t PathCode64Ext(const char* path);
    uint64_t PathCode64Ext(const std::string& path);
}