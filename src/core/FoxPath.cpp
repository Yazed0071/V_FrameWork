#include "pch.h"

#include "FoxPath.h"
#include "FoxPathInternal.h"

#include <cstdint>

#include "AddressSet.h"
#include "HookUtils.h"
#include "FoxHashes.h"

namespace
{
    using FoxPathCtor_t   = void* (__fastcall*)(void* path, std::uint64_t code64ext);
    using FoxPathExists_t = std::uint8_t (__fastcall*)(void* path, std::uint32_t flags);
    using FoxPathDtor_t   = void (__fastcall*)(void* path);
}

namespace fox
{
namespace detail
{
    bool PathExistsByCode(std::uint64_t pathCode64Ext)
    {
        if (pathCode64Ext == 0) return false;

        if (::AddressSetRuntime::IsEn154Family(gGameBuild))
            return true;

        auto ctor = reinterpret_cast<FoxPathCtor_t>(
            ResolveGameAddress(gAddr.FoxPath_Path));
        auto exists = reinterpret_cast<FoxPathExists_t>(
            ResolveGameAddress(gAddr.Fox_Path_Exists));
        auto dtor = reinterpret_cast<FoxPathDtor_t>(
            ResolveGameAddress(gAddr.Fox_Path_Dtor));

        if (!ctor || !exists) return false;

        std::uint64_t pathBuf[8] = {};
        bool result = false;

        __try
        {
            ctor(pathBuf, pathCode64Ext);
            result = exists(pathBuf, 0) != 0;
            if (dtor) dtor(pathBuf);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            result = false;
        }

        return result;
    }
}

    bool PathExists(const char* path)
    {
        if (!path || !path[0]) return false;
        return detail::PathExistsByCode(FoxHashes::PathCode64Ext(path));
    }
}
