#include "pch.h"

#include <Windows.h>
#include <cstdint>

#include "HookUtils.h"
#include "log.h"

namespace
{
    // Target function type:
    // RCX = NoticeActionImpl*
    // RDX = actorId / slot id
    // R8D = proc
    // R9  = event object
    using State_ComradeAction_t =
        void(__fastcall*)(void* self, unsigned int actorId, unsigned int proc, void* evt);

    // Absolute address of:
    // tpp::gm::soldier::impl::NoticeActionImpl::State_ComradeAction
    static constexpr std::uintptr_t ABS_State_ComradeAction = 0x1414B8D20ull;

    static State_ComradeAction_t g_OrigState_ComradeAction = nullptr;

    // Used to suppress duplicate spam for the same event object.
    static void* g_LastLoggedEvt = nullptr;
}

// Safely reads a qword from memory.
// Params: addr (uintptr_t), outValue (uint64_t&)
static bool SafeReadQword(std::uintptr_t addr, std::uint64_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint64_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Safely reads a dword from memory.
// Params: addr (uintptr_t), outValue (uint32_t&)
static bool SafeReadDword(std::uintptr_t addr, std::uint32_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint32_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Safely reads a word from memory.
// Params: addr (uintptr_t), outValue (uint16_t&)
static bool SafeReadWord(std::uintptr_t addr, std::uint16_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint16_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Safely reads a byte from memory.
// Params: addr (uintptr_t), outValue (uint8_t&)
static bool SafeReadByte(std::uintptr_t addr, std::uint8_t& outValue)
{
    if (!addr)
        return false;

    __try
    {
        outValue = *reinterpret_cast<const std::uint8_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Calls the first virtual on the event object to get the event hash.
// Params: evt (void*)
static std::uint32_t GetEventHash(void* evt)
{
    if (!evt)
        return 0;

    __try
    {
        const std::uintptr_t objectAddr = reinterpret_cast<std::uintptr_t>(evt);
        const std::uintptr_t vtbl = *reinterpret_cast<const std::uintptr_t*>(objectAddr);
        if (!vtbl)
            return 0;

        const std::uintptr_t fnAddr = *reinterpret_cast<const std::uintptr_t*>(vtbl + 0x0ull);
        if (!fnAddr)
            return 0;

        using GetHashFn_t = std::uint32_t(__fastcall*)(void*);
        auto fn = reinterpret_cast<GetHashFn_t>(fnAddr);
        return fn(evt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Resolves the per-action entry used by State_ComradeAction.
// Formula matches the function body:
//   entry = ((actorId - *(int*)(self+0x98)) * 0x68) + *(qword*)(self+0x90)
// Params: self (void*), actorId (unsigned int)
static std::uintptr_t GetComradeActionEntry(void* self, unsigned int actorId)
{
    if (!self)
        return 0;

    const std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);

    std::uint32_t baseIndex = 0;
    std::uint64_t tableBase = 0;

    if (!SafeReadDword(selfAddr + 0x98ull, baseIndex))
        return 0;

    if (!SafeReadQword(selfAddr + 0x90ull, tableBase))
        return 0;

    const std::uint32_t slot = actorId - baseIndex;
    return static_cast<std::uintptr_t>(tableBase + (static_cast<std::uint64_t>(slot) * 0x68ull));
}

// Logs the target-info area and a few related fields from the comrade-action entry.
// Focus is on the block starting at entry+0x18, since that is passed to
// Soldier2ActionUtil::GetPositionFromTargetInfoParam.
// Params: actorId (unsigned int), evt (void*), entry (uintptr_t)
static void LogComradeActionTargetInfo(unsigned int actorId, void* evt, std::uintptr_t entry)
{
    std::uint64_t q18 = 0;
    std::uint64_t q20 = 0;
    std::uint64_t q28 = 0;
    std::uint64_t q30 = 0;
    std::uint32_t d38 = 0;
    std::uint32_t d3C = 0;
    std::uint16_t w54 = 0;
    std::uint8_t b56 = 0;
    std::uint8_t b57 = 0;
    std::uint8_t b5D = 0;
    std::uint8_t b5E = 0;

    SafeReadQword(entry + 0x18ull, q18);
    SafeReadQword(entry + 0x20ull, q20);
    SafeReadQword(entry + 0x28ull, q28);
    SafeReadQword(entry + 0x30ull, q30);
    SafeReadDword(entry + 0x38ull, d38);
    SafeReadDword(entry + 0x3Cull, d3C);
    SafeReadWord(entry + 0x54ull, w54);
    SafeReadByte(entry + 0x56ull, b56);
    SafeReadByte(entry + 0x57ull, b57);
    SafeReadByte(entry + 0x5Dull, b5D);
    SafeReadByte(entry + 0x5Eull, b5E);

    Log("[ComradeAction] actorId=%u evt=%p entry=%p "
        "+18=0x%016llX +20=0x%016llX +28=0x%016llX +30=0x%016llX "
        "+38=0x%08X +3C=0x%08X +54=0x%04X +56=0x%02X +57=0x%02X +5D=0x%02X +5E=0x%02X\n",
        actorId,
        evt,
        reinterpret_cast<void*>(entry),
        static_cast<unsigned long long>(q18),
        static_cast<unsigned long long>(q20),
        static_cast<unsigned long long>(q28),
        static_cast<unsigned long long>(q30),
        d38,
        d3C,
        static_cast<unsigned>(w54),
        static_cast<unsigned>(b56),
        static_cast<unsigned>(b57),
        static_cast<unsigned>(b5D),
        static_cast<unsigned>(b5E));
}

// Hooked State_ComradeAction.
// Logs only the relevant voice-notify event with minimal spam, then calls original.
// Params: self (void*), actorId (unsigned int), proc (unsigned int), evt (void*)
static void __fastcall hkState_ComradeAction(
    void* self,
    unsigned int actorId,
    unsigned int proc,
    void* evt)
{
    if (proc == 6 && evt)
    {
        const std::uint32_t eventHash = GetEventHash(evt);

        if (eventHash == 0x1077DB8Du)
        {
            if (evt != g_LastLoggedEvt)
            {
                g_LastLoggedEvt = evt;

                const std::uintptr_t entry = GetComradeActionEntry(self, actorId);
                if (entry)
                {
                    LogComradeActionTargetInfo(actorId, evt, entry);
                }
                else
                {
                    Log("[ComradeAction] actorId=%u evt=%p entry=<resolve failed>\n", actorId, evt);
                }
            }
        }
    }

    g_OrigState_ComradeAction(self, actorId, proc, evt);
}

// Installs the logger hook for NoticeActionImpl::State_ComradeAction.
// Params: none
bool Install_StateComradeActionLog_Hook()
{
    void* target = ResolveGameAddress(ABS_State_ComradeAction);
    if (!target)
    {
        Log("[Hook] StateComradeActionLog: target resolve failed\n");
        return false;
    }

    const bool ok = CreateAndEnableHook(
        target,
        reinterpret_cast<void*>(&hkState_ComradeAction),
        reinterpret_cast<void**>(&g_OrigState_ComradeAction));

    Log("[Hook] StateComradeActionLog: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

// Removes the logger hook.
// Params: none
bool Uninstall_StateComradeActionLog_Hook()
{
    DisableAndRemoveHook(ResolveGameAddress(ABS_State_ComradeAction));
    g_OrigState_ComradeAction = nullptr;
    g_LastLoggedEvt = nullptr;

    Log("[Hook] StateComradeActionLog: removed\n");
    return true;
}