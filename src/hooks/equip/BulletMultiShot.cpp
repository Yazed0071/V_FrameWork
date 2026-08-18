#include "pch.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "AddressSet.h"
#include "BulletLockOn.h"
#include "BulletMultiShot.h"
#include "HookUtils.h"
#include "log.h"

namespace
{
    struct MultiShotSpec
    {
        int ammoPerShot = 0;
        int lockAmmoPerShot = 0;
    };

    std::recursive_mutex g_Mutex;
    std::unordered_map<int, MultiShotSpec> g_SpecByBulletId;

    using Fire_t = void(__fastcall*)(void*, std::uint8_t*, std::uint32_t);
    Fire_t g_OrigFire = nullptr;

    int SehKeepAvOnly(unsigned long code)
    {
        return (code == EXCEPTION_ACCESS_VIOLATION
                || code == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    bool ReadWorkIdsSEH(std::uint8_t* work, int* outBulletId)
    {
        __try
        {
            if (work[0x3df] == 0)
                return false;
            *outBulletId = work[0x22c];
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    bool LookupForWork(std::uint8_t* work, MultiShotSpec* out)
    {
        int bulletId = 0;
        if (!ReadWorkIdsSEH(work, &bulletId))
            return false;
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
        auto it = g_SpecByBulletId.find(bulletId);
        if (it == g_SpecByBulletId.end())
            return false;
        *out = it->second;
        return true;
    }

    void ApplyCountSEH(std::uint8_t* work, int rounds)
    {
        __try
        {
            if (work[0x3df] != 0)
                work[0x3df] = static_cast<std::uint8_t>(rounds);
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
        }
    }

    bool ArmCountSEH(std::uint8_t* work, int rounds)
    {
        __try
        {
            work[0x3df] = static_cast<std::uint8_t>(rounds);
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    int ReadMagSEH(std::uint8_t* work)
    {
        __try
        {
            return *reinterpret_cast<std::uint16_t*>(work + 0x25c);
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return -1;
        }
    }

    bool ReadFlags27cSEH(std::uint8_t* work, std::uint32_t* out)
    {
        __try
        {
            *out = *reinterpret_cast<std::uint32_t*>(work + 0x27c);
            return true;
        }
        __except (SehKeepAvOnly(GetExceptionCode()))
        {
            return false;
        }
    }

    void __fastcall hkFire(void* self, std::uint8_t* work, std::uint32_t idx)
    {
        MultiShotSpec spec{};
        if (!LookupForWork(work, &spec))
        {
            g_OrigFire(self, work, idx);
            return;
        }
        int n = spec.ammoPerShot;
        if (spec.lockAmmoPerShot > 0 && equip::LockOn_IsLockedNow())
            n = spec.lockAmmoPerShot;
        if (n <= 1)
        {
            g_OrigFire(self, work, idx);
            return;
        }
        if (n > 1023)
            n = 1023;

        std::uint32_t flags = 0;
        const bool haveFlags = ReadFlags27cSEH(work, &flags);
        const bool singleOnly = !haveFlags || (flags & 0x400000) != 0;

        int chunk = n < 255 ? n : 255;
        ApplyCountSEH(work, chunk);
        g_OrigFire(self, work, idx);

        int remaining = n - chunk;
        if (remaining <= 0)
            return;
        if (singleOnly)
        {
#ifdef _DEBUG
            LogDebug("[MultiShot] engine forces 1 round/call for this weapon state "
                "(work+0x27c bit 0x400000) - %d of %d rounds not fired\n",
                remaining, n);
#endif
            return;
        }
        while (remaining > 0)
        {
            const int mag = ReadMagSEH(work);
            if (mag == 0)
            {
#ifdef _DEBUG
                LogDebug("[MultiShot] mag empty - %d of %d rounds dropped\n",
                    remaining, n);
#endif
                break;
            }
            if (mag < 0)
            {
                Log("[MultiShot] work read failed mid-burst - stopping "
                    "re-trigger (%d remaining)\n", remaining);
                break;
            }
            chunk = remaining < 255 ? remaining : 255;
            if (!ArmCountSEH(work, chunk))
            {
                Log("[MultiShot] counter re-arm failed - stopping re-trigger "
                    "(%d remaining)\n", remaining);
                break;
            }
            g_OrigFire(self, work, idx);
            remaining -= chunk;
        }
    }
}

namespace equip
{
    void MultiShot_RegisterBullet(int bulletId, int ammoPerShot,
                                  int lockAmmoPerShot)
    {
        if (bulletId <= 0 || bulletId > 255)
            return;
        MultiShotSpec spec{};
        spec.ammoPerShot = ammoPerShot;
        if (spec.ammoPerShot < 0) spec.ammoPerShot = 0;
        if (spec.ammoPerShot > 1023) spec.ammoPerShot = 1023;
        spec.lockAmmoPerShot = lockAmmoPerShot;
        if (spec.lockAmmoPerShot < 0) spec.lockAmmoPerShot = 0;
        if (spec.lockAmmoPerShot > 1023) spec.lockAmmoPerShot = 1023;
        {
            std::lock_guard<std::recursive_mutex> lock(g_Mutex);
            g_SpecByBulletId[bulletId] = spec;
        }
        LogDebug("[MultiShot] bulletId=%d ammoPerShot=%d lockAmmoPerShot=%d "
            "registered\n", bulletId, spec.ammoPerShot, spec.lockAmmoPerShot);
    }

    bool Install_BulletMultiShot_Hooks()
    {
        void* fn = ResolveGameAddress(gAddr.AttackAction_Fire);
        if (!fn)
        {
            LogDebug("[MultiShot] AttackAction_Fire unresolved on this build "
                "- SetBullet ammoPerShot inactive.\n");
            return true;
        }
        const bool ok = CreateAndEnableHook(
            fn, reinterpret_cast<void*>(&hkFire),
            reinterpret_cast<void**>(&g_OrigFire));
        Log("[MultiShot] Fire hook Install -> %s\n", ok ? "OK" : "FAIL");
        return ok;
    }

    void Uninstall_BulletMultiShot_Hooks()
    {
        void* fn = ResolveGameAddress(gAddr.AttackAction_Fire);
        if (fn && g_OrigFire)
            DisableAndRemoveHook(fn);
        g_OrigFire = nullptr;
        std::lock_guard<std::recursive_mutex> lock(g_Mutex);
        g_SpecByBulletId.clear();
    }
}
