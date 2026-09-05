#include "pch.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AddressSet.h"
#include "FoxHashes.h"
#include "HookUtils.h"
#include "log.h"
#include "MbDvcCustomPopupHook.h"
#include "RewardPopupBgTexture.h"


namespace
{
    constexpr std::uint32_t kVPopupMagic = 0x56465043u;

    constexpr std::uintptr_t kImpl_TitleBufOffset       = 0x60;
    constexpr std::size_t    kImpl_TitleBufSize         = 0x80;
    constexpr std::uintptr_t kImpl_BodyBufOffset        = 0xe0;
    constexpr std::size_t    kImpl_BodyBufSize          = 0x400;
    constexpr std::uintptr_t kImpl_InnerStateOffset     = 0x558;
    constexpr std::uintptr_t kImpl_QuarkRootOffset      = 0x38;
    constexpr std::uintptr_t kQuarkRoot_PopupCtrlOffset = 0x80;
    constexpr std::uintptr_t kQuarkRoot_LangMgrOffset   = 0x20;
    constexpr std::uintptr_t kQuarkRoot_GateSvcOffset   = 0x48;

    constexpr std::uintptr_t kCtrl_SlotsBaseOffset    = 0x08;
    constexpr std::size_t    kCtrl_SlotSize           = 0x14;
    constexpr std::size_t    kCtrl_NumSlots           = 0x10;
    constexpr std::uintptr_t kSlot_CommonValue1Offset = 0x00;
    constexpr std::uintptr_t kSlot_ReserveIdOffset    = 0x10;
    constexpr std::uint8_t   kReserveId_Empty         = 0x0E;
    constexpr std::uint8_t   kReserveId_NormalSlot0   = 0x00;

    constexpr std::size_t kCtrlVtableIndex_ReserveAnnouncePopup = 0x08 / sizeof(void*);

    constexpr std::size_t kLangVtableIndex_GetLangText = 0x750 / sizeof(void*);

    constexpr std::size_t kGateVtableIndex = 0x610 / sizeof(void*);

    using UpdateAnnounceNormal_t    = std::int32_t (__fastcall*)(void* self);
    using UpdateAnnounceEmergency_t = std::int32_t (__fastcall*)(void* self);
    using ReserveAnnouncePopup_t    = void (__fastcall*)(void* ctrl, const void* param);
    using GetLangText_t             = const char* (__fastcall*)(void* langMgr, std::uint64_t hash);
    using GateFn_t                  = char (__fastcall*)(void* svc);

    static UpdateAnnounceNormal_t    g_OrigUpdateAnnounceNormal    = nullptr;
    static UpdateAnnounceNormal_t    g_OrigUpdateAnnounceServer    = nullptr;
    static UpdateAnnounceEmergency_t g_OrigUpdateAnnounceEmergency = nullptr;
    static ReserveAnnouncePopup_t g_OrigReserveAnnouncePopup = nullptr;
    static GateFn_t               g_OrigGateFn               = nullptr;
    static void*                  g_ReserveHookTarget        = nullptr;
    static void*                  g_GateHookTarget           = nullptr;
    static std::once_flag         g_ReserveHookInstallFlag;
    static std::once_flag         g_GateHookInstallFlag;

    static thread_local bool      g_BypassPopupGate          = false;

    static std::atomic<void*> g_PopupController{ nullptr };
    static std::atomic<void*> g_LangManager{ nullptr };

    static std::atomic<bool> g_DiagDumpDone{ false };

    struct PopupTextSource
    {
        bool          isHash = false;
        std::string   literal;
        std::uint64_t hash = 0;
    };

    struct PendingPopup
    {
        PopupTextSource title;
        PopupTextSource body;
        std::uint64_t   bgTexture = 0;
        std::uint8_t    reserveId = kReserveId_NormalSlot0;
        bool            reserved  = false;
    };
    static std::deque<PendingPopup> g_PendingQueue;
    static std::mutex               g_PendingMutex;


    struct EmergencyPopupOverride
    {
        bool          hasTitle    = false;
        bool          titleIsHash = false;
        std::string   titleLiteral;
        std::uint64_t titleHash   = 0;

        bool          hasBody    = false;
        bool          bodyIsHash = false;
        std::string   bodyLiteral;
        std::uint64_t bodyHash   = 0;
    };
    static std::mutex                       g_EmergencyOverrideMutex;
    static EmergencyPopupOverride           g_ActiveEmergencyOverride;
    static bool                             g_HasActiveEmergencyOverride = false;
    static void*                            g_EmergencyHookTarget        = nullptr;

#pragma pack(push, 1)
    struct ReserveParam
    {
        std::uint32_t commonValue1;
        std::uint32_t commonValue2;
        std::uint32_t commonValue3;
        std::uint32_t commonValue4;
        std::uint8_t  reserveId;
        std::uint8_t  pad[3];
    };
#pragma pack(pop)
    static_assert(sizeof(ReserveParam) == kCtrl_SlotSize, "ReserveParam matches slot stride");


    inline std::uint8_t* SlotPtr(void* ctrl, std::size_t i)
    {
        return reinterpret_cast<std::uint8_t*>(ctrl)
             + kCtrl_SlotsBaseOffset
             + i * kCtrl_SlotSize;
    }

    static bool SafeReadIncomingParam(const void* param,
                                      std::uint32_t* outCv1,
                                      std::uint8_t*  outReserveId)
    {
        *outCv1 = 0;
        *outReserveId = 0xFF;
        if (!param)
            return false;

        __try
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(param);
            *outCv1       = *reinterpret_cast<const std::uint32_t*>(bytes + kSlot_CommonValue1Offset);
            *outReserveId = *(bytes + kSlot_ReserveIdOffset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void* SafeReadControllerFromSelf(void* self)
    {
        if (!self)
            return nullptr;

        __try
        {
            const auto selfBytes = reinterpret_cast<std::uintptr_t>(self);
            void* quarkRoot = *reinterpret_cast<void**>(selfBytes + kImpl_QuarkRootOffset);
            if (!quarkRoot)
                return nullptr;

            return *reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(quarkRoot) + kQuarkRoot_PopupCtrlOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static std::uint32_t SafeReadInnerState(void* self)
    {
        if (!self)
            return 0;

        __try
        {
            return *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(self) + kImpl_InnerStateOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static bool SafeAnyOurSlotInRing(void* ctrl,
                                     const std::uint8_t* eligibleIds,
                                     std::size_t numEligibleIds)
    {
        if (!ctrl || !eligibleIds || numEligibleIds == 0)
            return false;

        __try
        {
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                const std::uint8_t* slot = SlotPtr(ctrl, i);
                const std::uint8_t  rid  = *(slot + kSlot_ReserveIdOffset);
                bool eligible = false;
                for (std::size_t k = 0; k < numEligibleIds; ++k)
                {
                    if (rid == eligibleIds[k]) { eligible = true; break; }
                }
                if (!eligible) continue;
                const std::uint32_t cv1 = *reinterpret_cast<const std::uint32_t*>(
                    slot + kSlot_CommonValue1Offset);
                if (cv1 == kVPopupMagic)
                    return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return false;
    }

    static bool SafeFindFirstSlotIsOurs(void* ctrl,
                                        std::uint8_t wantedReserveId,
                                        bool* outIsOurs)
    {
        *outIsOurs = false;
        if (!ctrl)
            return false;

        __try
        {
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                const std::uint8_t* slot = SlotPtr(ctrl, i);
                if (*(slot + kSlot_ReserveIdOffset) == wantedReserveId)
                {
                    const std::uint32_t cv1 = *reinterpret_cast<const std::uint32_t*>(
                        slot + kSlot_CommonValue1Offset);
                    *outIsOurs = (cv1 == kVPopupMagic);
                    return true;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return false;
    }

    static std::size_t SafeCountEmptySlots(void* ctrl)
    {
        if (!ctrl)
            return 0;

        __try
        {
            std::size_t count = 0;
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                const std::uint8_t* slot = SlotPtr(ctrl, i);
                if (*(slot + kSlot_ReserveIdOffset) == kReserveId_Empty)
                    ++count;
            }
            return count;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    static bool SafeEvictOldestOurSlot(void* ctrl)
    {
        if (!ctrl)
            return false;

        __try
        {
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                std::uint8_t* slot = SlotPtr(ctrl, i);
                const std::uint8_t reserveId = *(slot + kSlot_ReserveIdOffset);
                if (reserveId == kReserveId_Empty)
                    continue;

                const std::uint32_t cv1 = *reinterpret_cast<std::uint32_t*>(
                    slot + kSlot_CommonValue1Offset);
                if (cv1 == kVPopupMagic)
                {
                    std::memset(slot, 0, kCtrl_SlotSize);
                    *(slot + kSlot_ReserveIdOffset) = kReserveId_Empty;
                    return true;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return false;
    }

    static void SafeWriteImplByte(void* self, std::uintptr_t off, std::uint8_t val)
    {
        if (!self) return;
        __try
        {
            *(std::uint8_t*)((std::uintptr_t)self + off) = val;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void SafeWriteImplDword(void* self, std::uintptr_t off, std::uint32_t val)
    {
        if (!self) return;
        __try
        {
            *(std::uint32_t*)((std::uintptr_t)self + off) = val;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static bool SafeWriteTitleBody(void* self,
                                   const char* title,
                                   const char* body)
    {
        if (!self || !title || !body)
            return false;

        __try
        {
            char* titleBuf = reinterpret_cast<char*>(
                reinterpret_cast<std::uintptr_t>(self) + kImpl_TitleBufOffset);
            char* bodyBuf = reinterpret_cast<char*>(
                reinterpret_cast<std::uintptr_t>(self) + kImpl_BodyBufOffset);

            strncpy_s(titleBuf, kImpl_TitleBufSize, title, _TRUNCATE);
            strncpy_s(bodyBuf,  kImpl_BodyBufSize,  body,  _TRUNCATE);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool SafeCallReserveAnnouncePopup(void* ctrl, const ReserveParam* p)
    {
        if (!ctrl || !p)
            return false;

        __try
        {
            void** vtbl = *reinterpret_cast<void***>(ctrl);
            if (!vtbl)
                return false;

            auto reserveFn = reinterpret_cast<ReserveAnnouncePopup_t>(
                vtbl[kCtrlVtableIndex_ReserveAnnouncePopup]);
            if (!reserveFn)
                return false;

            reserveFn(ctrl, p);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool SafeVerifyOurReservationLanded(void* ctrl, std::uint8_t reserveId)
    {
        if (!ctrl)
            return false;

        __try
        {
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                const std::uint8_t* slot = SlotPtr(ctrl, i);
                if (*(slot + kSlot_ReserveIdOffset) == reserveId)
                {
                    const std::uint32_t cv1 = *reinterpret_cast<const std::uint32_t*>(
                        slot + kSlot_CommonValue1Offset);
                    if (cv1 == kVPopupMagic)
                        return true;
                }
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    static void* SafeReadGateSvcFromSelf(void* self)
    {
        if (!self)
            return nullptr;

        __try
        {
            const auto selfBytes = reinterpret_cast<std::uintptr_t>(self);
            void* quarkRoot = *reinterpret_cast<void**>(selfBytes + kImpl_QuarkRootOffset);
            if (!quarkRoot)
                return nullptr;

            return *reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(quarkRoot) + kQuarkRoot_GateSvcOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* SafeReadLangManagerFromSelf(void* self)
    {
        if (!self)
            return nullptr;

        __try
        {
            const auto selfBytes = reinterpret_cast<std::uintptr_t>(self);
            void* quarkRoot = *reinterpret_cast<void**>(selfBytes + kImpl_QuarkRootOffset);
            if (!quarkRoot)
                return nullptr;

            return *reinterpret_cast<void**>(
                reinterpret_cast<std::uintptr_t>(quarkRoot) + kQuarkRoot_LangMgrOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static const char* SafeResolveLangText(void* langMgr, std::uint64_t hash)
    {
        if (!langMgr)
            return nullptr;

        __try
        {
            void** vtbl = *reinterpret_cast<void***>(langMgr);
            if (!vtbl)
                return nullptr;

            auto fn = reinterpret_cast<GetLangText_t>(vtbl[kLangVtableIndex_GetLangText]);
            if (!fn)
                return nullptr;

            return fn(langMgr, hash);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static std::uint32_t SafeReadFirstSlotCv1(void* ctrl, std::uint8_t wantedReserveId)
    {
        if (!ctrl)
            return 0;

        __try
        {
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                const std::uint8_t* slot = SlotPtr(ctrl, i);
                if (*(slot + kSlot_ReserveIdOffset) == wantedReserveId)
                {
                    return *reinterpret_cast<const std::uint32_t*>(
                        slot + kSlot_CommonValue1Offset);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return 0;
    }

    static bool SafeReserveOurSlot(void* ctrl, std::uint8_t reserveId)
    {
        if (!ctrl)
            return false;

        __try
        {
            void** vtbl = *reinterpret_cast<void***>(ctrl);
            if (!vtbl)
                return false;

            auto reserveFn = reinterpret_cast<ReserveAnnouncePopup_t>(
                vtbl[kCtrlVtableIndex_ReserveAnnouncePopup]);
            if (!reserveFn)
                return false;

            std::size_t ourBefore = 0;
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                const std::uint8_t* slot = SlotPtr(ctrl, i);
                if (*(slot + kSlot_ReserveIdOffset) != kReserveId_Empty
                    && *reinterpret_cast<const std::uint32_t*>(
                           slot + kSlot_CommonValue1Offset) == kVPopupMagic)
                    ++ourBefore;
            }

            ReserveParam p{};
            p.commonValue1 = kVPopupMagic;
            p.reserveId    = reserveId;

            reserveFn(ctrl, &p);

            std::size_t ourAfter = 0;
            for (std::size_t i = 0; i < kCtrl_NumSlots; ++i)
            {
                const std::uint8_t* slot = SlotPtr(ctrl, i);
                if (*(slot + kSlot_ReserveIdOffset) != kReserveId_Empty
                    && *reinterpret_cast<const std::uint32_t*>(
                           slot + kSlot_CommonValue1Offset) == kVPopupMagic)
                    ++ourAfter;
            }
            return ourAfter > ourBefore;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void* SafeGetGateFnPtr(void* gateSvc)
    {
        if (!gateSvc)
            return nullptr;

        __try
        {
            void** vtbl = *reinterpret_cast<void***>(gateSvc);
            if (!vtbl)
                return nullptr;
            return vtbl[kGateVtableIndex];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* SafeGetReserveFnPtr(void* ctrl)
    {
        if (!ctrl)
            return nullptr;

        __try
        {
            void** vtbl = *reinterpret_cast<void***>(ctrl);
            if (!vtbl)
                return nullptr;
            return vtbl[kCtrlVtableIndex_ReserveAnnouncePopup];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void SafeDiagDumpSlots(void* ctrl, const void* incomingParam)
    {
        if (!ctrl)
            return;

        __try
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(ctrl);

            const std::uintptr_t vtbl = *reinterpret_cast<const std::uintptr_t*>(base);

            if (incomingParam)
            {
                const auto* pb = reinterpret_cast<const std::uint8_t*>(incomingParam);
            }

            for (int i = 0; i < 8; ++i)
            {
                const auto* slot = base + 0x08 + (std::size_t)i * 0x0c;
            }

            for (int i = 0; i < 8; ++i)
            {
                const auto* slot = base + 0x08 + (std::size_t)i * 0x14;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
#ifdef _DEBUG
            LogDebug("[MbDvcCustomPopup][DIAG] dump raised; aborting diag\n");
#endif
        }
    }

    static void __fastcall hk_ReserveAnnouncePopup(void* ctrl, const void* param)
    {
        if (!ctrl || !param || !g_OrigReserveAnnouncePopup)
        {
            if (g_OrigReserveAnnouncePopup)
                g_OrigReserveAnnouncePopup(ctrl, param);
            return;
        }

        if (!g_DiagDumpDone.exchange(true, std::memory_order_relaxed))
        {
            SafeDiagDumpSlots(ctrl, param);
        }

        std::uint32_t incomingCv1 = 0;
        std::uint8_t  incomingReserveId = 0xFF;
        if (!SafeReadIncomingParam(param, &incomingCv1, &incomingReserveId))
        {
            LogDebug("[MbDvcCustomPopup] Reserve hook: param read raised; passing through\n");
            g_OrigReserveAnnouncePopup(ctrl, param);
            return;
        }

        const bool isOurs = (incomingCv1 == kVPopupMagic);

        const bool isNormalSlot = (incomingReserveId == 0x00 || incomingReserveId == 0x01);

        const std::size_t empty = SafeCountEmptySlots(ctrl);

        if (empty == 0 && isNormalSlot)
        {
            if (isOurs)
            {
                return;
            }
            else
            {
                const bool evicted = SafeEvictOldestOurSlot(ctrl);
                if (evicted)
                {
                    {
                        std::lock_guard<std::mutex> lock(g_PendingMutex);
                        if (!g_PendingQueue.empty())
                            g_PendingQueue.pop_front();
                    }
                }
                else
                {
                    LogDebug("[MbDvcCustomPopup] Reserve hook: ring full + game popup but "
                        "no V_FrameWork slot to evict; game reservation will silently fail\n");
                }
            }
        }

        g_OrigReserveAnnouncePopup(ctrl, param);
    }

    static void TryInstallReserveHook(void* ctrl)
    {
        if (!ctrl)
            return;

        std::call_once(g_ReserveHookInstallFlag, [ctrl]()
        {
            void* reserveFn = SafeGetReserveFnPtr(ctrl);
            if (!reserveFn)
            {
                LogDebug("[MbDvcCustomPopup] ReserveHook install: vtable[+8] not resolvable\n");
                return;
            }

            const bool ok = CreateAndEnableHook(
                reserveFn,
                reinterpret_cast<void*>(&hk_ReserveAnnouncePopup),
                reinterpret_cast<void**>(&g_OrigReserveAnnouncePopup));
            if (ok)
            {
                g_ReserveHookTarget = reserveFn;
#ifdef _DEBUG
                LogDebug("[MbDvcCustomPopup] ReserveHook install: OK (target=%p)\n", reserveFn);
#endif
            }
            else
            {
                Log("[MbDvcCustomPopup] ReserveHook install: MinHook FAILED (target=%p)\n", reserveFn);
            }
        });
    }

    static char __fastcall hk_GateFn(void* svc)
    {
        if (g_BypassPopupGate)
            return 1;
        return g_OrigGateFn ? g_OrigGateFn(svc) : 0;
    }

    static void TryInstallGateHook(void* self)
    {
        if (!self)
            return;

        std::call_once(g_GateHookInstallFlag, [self]()
        {
            void* gateSvc = SafeReadGateSvcFromSelf(self);
            if (!gateSvc)
            {
                LogDebug("[MbDvcCustomPopup] GateHook install: gate svc not resolvable\n");
                return;
            }

            void* fn = SafeGetGateFnPtr(gateSvc);
            if (!fn)
            {
                LogDebug("[MbDvcCustomPopup] GateHook install: vtable[+0x610] not resolvable\n");
                return;
            }

            const bool ok = CreateAndEnableHook(
                fn,
                reinterpret_cast<void*>(&hk_GateFn),
                reinterpret_cast<void**>(&g_OrigGateFn));
            if (ok)
            {
                g_GateHookTarget = fn;
#ifdef _DEBUG
                LogDebug("[MbDvcCustomPopup] GateHook install: OK (target=%p)\n", fn);
#endif
            }
            else
            {
                Log("[MbDvcCustomPopup] GateHook install: MinHook FAILED (target=%p)\n", fn);
            }
        });
    }

    static void DrainUnreservedReservations(void* ctrl)
    {
        if (!ctrl)
            return;

        for (;;)
        {
            bool         haveCandidate = false;
            std::uint8_t candidateRid  = 0;
            {
                std::lock_guard<std::mutex> lock(g_PendingMutex);
                for (auto& entry : g_PendingQueue)
                {
                    if (!entry.reserved)
                    {
                        haveCandidate = true;
                        candidateRid  = entry.reserveId;
                        break;
                    }
                }
            }
            if (!haveCandidate)
                return;

            const bool ok = SafeReserveOurSlot(ctrl, candidateRid);

            {
                std::lock_guard<std::mutex> lock(g_PendingMutex);
                for (auto it = g_PendingQueue.begin(); it != g_PendingQueue.end(); ++it)
                {
                    if (!it->reserved)
                    {
                        if (ok)
                        {
                            it->reserved = true;
                        }
                        else
                        {
                            g_PendingQueue.erase(it);
                        }
                        break;
                    }
                }
            }

            if (!ok)
            {
                Log("[MbDvcCustomPopup] DrainUnreserved: Reserve failed (ring may be full); "
                    "dropped one queued entry\n");
                return;
            }
        }
    }

    static std::int32_t RunPopupOverrideHook(
        void*                      self,
        UpdateAnnounceNormal_t     origFn,
        const std::uint8_t*        eligibleIds,
        std::size_t                numEligibleIds)
    {
        if (!self || !origFn)
        {
            return origFn ? origFn(self) : 0;
        }

        const std::uint32_t prevState = SafeReadInnerState(self);

        void* ctrl = SafeReadControllerFromSelf(self);
        if (ctrl)
        {
            g_PopupController.store(ctrl, std::memory_order_relaxed);
            TryInstallReserveHook(ctrl);
            DrainUnreservedReservations(ctrl);
        }

        void* lang = SafeReadLangManagerFromSelf(self);
        if (lang)
        {
            g_LangManager.store(lang, std::memory_order_relaxed);
        }

        TryInstallGateHook(self);

        bool nextSlotIsOurs = false;
        if (ctrl && prevState != 1)
        {
            for (std::size_t i = 0; i < numEligibleIds && !nextSlotIsOurs; ++i)
            {
                bool isOurs = false;
                SafeFindFirstSlotIsOurs(ctrl, eligibleIds[i], &isOurs);
                if (isOurs) nextSlotIsOurs = true;
            }
        }

        const bool anyOurSlotPending = SafeAnyOurSlotInRing(
            ctrl, eligibleIds, numEligibleIds);

        const bool prevBypass = g_BypassPopupGate;
        if (anyOurSlotPending)
            g_BypassPopupGate = true;

        const std::int32_t  result    = origFn(self);

        g_BypassPopupGate = prevBypass;

        const std::uint32_t currState = SafeReadInnerState(self);

        if (nextSlotIsOurs && prevState != 1 && currState == 1)
        {
            bool            havePopped = false;
            PopupTextSource title;
            PopupTextSource body;
            std::uint64_t   bgTexture  = 0;
            {
                std::lock_guard<std::mutex> lock(g_PendingMutex);
                for (auto it = g_PendingQueue.begin(); it != g_PendingQueue.end(); ++it)
                {
                    if (!it->reserved) continue;
                    bool eligible = false;
                    for (std::size_t i = 0; i < numEligibleIds; ++i)
                    {
                        if (eligibleIds[i] == it->reserveId)
                        {
                            eligible = true;
                            break;
                        }
                    }
                    if (eligible)
                    {
                        title     = std::move(it->title);
                        body      = std::move(it->body);
                        bgTexture = it->bgTexture;
                        g_PendingQueue.erase(it);
                        havePopped = true;
                        break;
                    }
                }
            }

            RewardPopupBg_SetCurrentPopupTexture(havePopped ? bgTexture : 0);

            if (havePopped)
            {
                const char* titleText = "";
                const char* bodyText  = "";

                if (title.isHash)
                    titleText = SafeResolveLangText(lang, title.hash);
                else
                    titleText = title.literal.c_str();
                if (!titleText) titleText = "";

                if (body.isHash)
                    bodyText = SafeResolveLangText(lang, body.hash);
                else
                    bodyText = body.literal.c_str();
                if (!bodyText) bodyText = "";

                if (SafeWriteTitleBody(self, titleText, bodyText))
                {
                }
                else
                {
                    Log("[MbDvcCustomPopup] override write failed (self=%p)\n", self);
                }
            }
        }
        else if (prevState != 1 && currState == 1)
        {
            RewardPopupBg_SetCurrentPopupTexture(0);
        }

        return result;
    }

    static std::int32_t __fastcall hk_UpdateAnnounceNormal(void* self)
    {
        static const std::uint8_t kNormalIds[] = { 0, 1 };
        const std::int32_t result = RunPopupOverrideHook(
            self, g_OrigUpdateAnnounceNormal, kNormalIds, sizeof(kNormalIds));

        if (result == 2)
        {
            void* ctrl = g_PopupController.load(std::memory_order_relaxed);
            static const std::uint8_t kServerIds[] = { 2, 3, 4, 7, 8 };
            if (SafeAnyOurSlotInRing(ctrl, kServerIds, sizeof(kServerIds)))
            {
                SafeWriteImplByte (self, 0x50, 1);
                SafeWriteImplDword(self, 0x5c, 0);
            }
        }
        return result;
    }

    static std::int32_t __fastcall hk_UpdateAnnounceServer(void* self)
    {
        static const std::uint8_t kServerIds[] = { 2, 3, 4, 7, 8 };
        return RunPopupOverrideHook(self, g_OrigUpdateAnnounceServer,
                                    kServerIds, sizeof(kServerIds));
    }


    static bool SafeWriteEmergencyBuffers(void* self,
                                          const char* titleOrNull,
                                          const char* bodyOrNull)
    {
        if (!self) return false;
        __try
        {
            if (titleOrNull)
            {
                char* titleBuf = reinterpret_cast<char*>(
                    reinterpret_cast<std::uintptr_t>(self) + kImpl_TitleBufOffset);
                strncpy_s(titleBuf, kImpl_TitleBufSize, titleOrNull, _TRUNCATE);
            }
            if (bodyOrNull)
            {
                char* bodyBuf = reinterpret_cast<char*>(
                    reinterpret_cast<std::uintptr_t>(self) + kImpl_BodyBufOffset);
                strncpy_s(bodyBuf, kImpl_BodyBufSize, bodyOrNull, _TRUNCATE);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }


    static std::int32_t __fastcall hk_UpdateAnnounceEmergency(void* self)
    {
        const std::int32_t result = g_OrigUpdateAnnounceEmergency
            ? g_OrigUpdateAnnounceEmergency(self) : 0;

        if (!self)
            return result;

        EmergencyPopupOverride snap;
        bool                   haveSnap = false;
        {
            std::lock_guard<std::mutex> lock(g_EmergencyOverrideMutex);
            if (g_HasActiveEmergencyOverride)
            {
                snap     = g_ActiveEmergencyOverride;
                haveSnap = true;
            }
        }

        if (!haveSnap || (!snap.hasTitle && !snap.hasBody))
            return result;

        void* lang = SafeReadLangManagerFromSelf(self);
        if (lang)
            g_LangManager.store(lang, std::memory_order_relaxed);
        else
            lang = g_LangManager.load(std::memory_order_relaxed);

        std::string titleResolved;
        std::string bodyResolved;
        const char* titleArg = nullptr;
        const char* bodyArg  = nullptr;

        if (snap.hasTitle)
        {
            if (snap.titleIsHash)
            {
                const char* r = SafeResolveLangText(lang, snap.titleHash);
                if (r) { titleResolved = r; titleArg = titleResolved.c_str(); }
            }
            else
            {
                titleArg = snap.titleLiteral.c_str();
            }
        }

        if (snap.hasBody)
        {
            if (snap.bodyIsHash)
            {
                const char* r = SafeResolveLangText(lang, snap.bodyHash);
                if (r) { bodyResolved = r; bodyArg = bodyResolved.c_str(); }
            }
            else
            {
                bodyArg = snap.bodyLiteral.c_str();
            }
        }

        if (titleArg || bodyArg)
            SafeWriteEmergencyBuffers(self, titleArg, bodyArg);

        return result;
    }
}


bool Set_MbDvcEmergencyPopup(const char* title, const char* body)
{
    std::lock_guard<std::mutex> lock(g_EmergencyOverrideMutex);

    EmergencyPopupOverride entry{};

    if (title)
    {
        entry.titleLiteral = title;
        entry.titleHash    = 0;
        entry.titleIsHash  = false;
        entry.hasTitle     = true;
    }
    if (body)
    {
        entry.bodyLiteral = body;
        entry.bodyHash    = 0;
        entry.bodyIsHash  = false;
        entry.hasBody     = true;
    }

    if (!entry.hasTitle && !entry.hasBody)
    {
        g_HasActiveEmergencyOverride = false;
        g_ActiveEmergencyOverride    = {};
#ifdef _DEBUG
        LogDebug("[MbDvcCustomPopup] Emergency popup override cleared (Set called with both fields null)\n");
#endif
        return true;
    }

    g_ActiveEmergencyOverride    = std::move(entry);
    g_HasActiveEmergencyOverride = true;

#ifdef _DEBUG
    LogDebug("[MbDvcCustomPopup] Emergency popup override set title=%s body=%s\n",
        g_ActiveEmergencyOverride.hasTitle ? "(literal)" : "(engine default)",
        g_ActiveEmergencyOverride.hasBody  ? "(literal)" : "(engine default)");
#endif
    return true;
}


bool Set_MbDvcEmergencyPopupLangId(const char* titleLabel, const char* bodyLabel)
{
    std::lock_guard<std::mutex> lock(g_EmergencyOverrideMutex);

    EmergencyPopupOverride entry{};

    if (titleLabel && *titleLabel)
    {
        entry.titleHash   = FoxHashes::StrCode64(titleLabel);
        entry.titleIsHash = true;
        entry.hasTitle    = true;
    }
    if (bodyLabel && *bodyLabel)
    {
        entry.bodyHash   = FoxHashes::StrCode64(bodyLabel);
        entry.bodyIsHash = true;
        entry.hasBody    = true;
    }

    if (!entry.hasTitle && !entry.hasBody)
    {
        g_HasActiveEmergencyOverride = false;
        g_ActiveEmergencyOverride    = {};
#ifdef _DEBUG
        LogDebug("[MbDvcCustomPopup] Emergency popup override cleared (SetLangId called with both labels empty)\n");
#endif
        return true;
    }

    g_ActiveEmergencyOverride    = std::move(entry);
    g_HasActiveEmergencyOverride = true;

#ifdef _DEBUG
    LogDebug("[MbDvcCustomPopup] Emergency popup override set titleHash=0x%016llX bodyHash=0x%016llX\n",
        static_cast<unsigned long long>(g_ActiveEmergencyOverride.titleIsHash
            ? g_ActiveEmergencyOverride.titleHash : 0),
        static_cast<unsigned long long>(g_ActiveEmergencyOverride.bodyIsHash
            ? g_ActiveEmergencyOverride.bodyHash : 0));
#endif
    return true;
}


void Clear_MbDvcEmergencyPopupOverride()
{
    std::lock_guard<std::mutex> lock(g_EmergencyOverrideMutex);
    if (g_HasActiveEmergencyOverride)
    {
        g_HasActiveEmergencyOverride = false;
        g_ActiveEmergencyOverride    = {};
#ifdef _DEBUG
        LogDebug("[MbDvcCustomPopup] Emergency popup override cleared\n");
#endif
    }
}


bool Install_MbDvcCustomPopup_Hook()
{
    void* targetN = ResolveGameAddress(
        gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceNormal);
    if (!targetN)
    {
        Log("[Hook] MbDvcCustomPopup: Normal target resolve failed (addr=%llX)\n",
            static_cast<unsigned long long>(
                gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceNormal));
        return false;
    }

    const bool okN = CreateAndEnableHook(
        targetN,
        reinterpret_cast<void*>(&hk_UpdateAnnounceNormal),
        reinterpret_cast<void**>(&g_OrigUpdateAnnounceNormal));
    if (!okN)
        Log("[Hook] MbDvcCustomPopup Normal: FAIL (target=%p)\n", targetN);
#ifdef _DEBUG
    else
        LogDebug("[Hook] MbDvcCustomPopup Normal: OK (target=%p)\n", targetN);
#endif

    if (gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceServer)
    {
        void* targetS = ResolveGameAddress(
            gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceServer);
        if (targetS)
        {
            const bool okS = CreateAndEnableHook(
                targetS,
                reinterpret_cast<void*>(&hk_UpdateAnnounceServer),
                reinterpret_cast<void**>(&g_OrigUpdateAnnounceServer));
            if (!okS)
                Log("[Hook] MbDvcCustomPopup Server: FAIL (target=%p)\n", targetS);
#ifdef _DEBUG
            else
                LogDebug("[Hook] MbDvcCustomPopup Server: OK (target=%p)\n", targetS);
#endif
        }
    }
#ifdef _DEBUG
    else
    {
        LogDebug("[Hook] MbDvcCustomPopup Server: skipped (no address for current build)\n");
    }
#endif

    if (gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceEmergency)
    {
        void* targetE = ResolveGameAddress(
            gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceEmergency);
        if (targetE)
        {
            const bool okE = CreateAndEnableHook(
                targetE,
                reinterpret_cast<void*>(&hk_UpdateAnnounceEmergency),
                reinterpret_cast<void**>(&g_OrigUpdateAnnounceEmergency));
            if (okE) g_EmergencyHookTarget = targetE;
            if (!okE)
                Log("[Hook] MbDvcCustomPopup Emergency: FAIL (target=%p)\n", targetE);
#ifdef _DEBUG
            else
                LogDebug("[Hook] MbDvcCustomPopup Emergency: OK (target=%p)\n", targetE);
#endif
        }
    }
#ifdef _DEBUG
    else
    {
        LogDebug("[Hook] MbDvcCustomPopup Emergency: skipped (no address for current build)\n");
    }
#endif

    if (gAddr.MbDvcReserveAnnouncePopup)
    {
        void* reserveFn = ResolveGameAddress(gAddr.MbDvcReserveAnnouncePopup);
        if (reserveFn)
        {
            const bool okR = CreateAndEnableHook(
                reserveFn,
                reinterpret_cast<void*>(&hk_ReserveAnnouncePopup),
                reinterpret_cast<void**>(&g_OrigReserveAnnouncePopup));
            if (okR)
            {
                g_ReserveHookTarget = reserveFn;
                std::call_once(g_ReserveHookInstallFlag, []() {});
            }
            if (!okR)
                Log("[MbDvcCustomPopup] ReserveHook install: FAIL (target=%p)\n", reserveFn);
#ifdef _DEBUG
            else
                LogDebug("[MbDvcCustomPopup] ReserveHook install: OK (target=%p)\n", reserveFn);
#endif
        }
    }

    if (gAddr.MbDvcPopupGateFn)
    {
        void* gateFn = ResolveGameAddress(gAddr.MbDvcPopupGateFn);
        if (gateFn)
        {
            const bool okG = CreateAndEnableHook(
                gateFn,
                reinterpret_cast<void*>(&hk_GateFn),
                reinterpret_cast<void**>(&g_OrigGateFn));
            if (okG)
            {
                g_GateHookTarget = gateFn;
                std::call_once(g_GateHookInstallFlag, []() {});
            }
            if (!okG)
                Log("[MbDvcCustomPopup] GateHook install: FAIL (target=%p)\n", gateFn);
#ifdef _DEBUG
            else
                LogDebug("[MbDvcCustomPopup] GateHook install: OK (target=%p)\n", gateFn);
#endif
        }
    }

    return okN;
}


bool Uninstall_MbDvcCustomPopup_Hook()
{
    void* targetN = ResolveGameAddress(
        gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceNormal);
    DisableAndRemoveHook(targetN);
    g_OrigUpdateAnnounceNormal = nullptr;

    if (gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceServer)
    {
        void* targetS = ResolveGameAddress(
            gAddr.MbDvcAnnouncePopupCallbackImpl_UpdateAnnounceServer);
        DisableAndRemoveHook(targetS);
        g_OrigUpdateAnnounceServer = nullptr;
    }

    if (g_EmergencyHookTarget)
    {
        DisableAndRemoveHook(g_EmergencyHookTarget);
        g_EmergencyHookTarget = nullptr;
        g_OrigUpdateAnnounceEmergency = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_EmergencyOverrideMutex);
        g_HasActiveEmergencyOverride = false;
        g_ActiveEmergencyOverride    = {};
    }

    if (g_ReserveHookTarget)
    {
        DisableAndRemoveHook(g_ReserveHookTarget);
        g_ReserveHookTarget = nullptr;
        g_OrigReserveAnnouncePopup = nullptr;
    }

    if (g_GateHookTarget)
    {
        DisableAndRemoveHook(g_GateHookTarget);
        g_GateHookTarget = nullptr;
        g_OrigGateFn = nullptr;
    }

    g_PopupController.store(nullptr, std::memory_order_relaxed);
    g_LangManager.store(nullptr, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        g_PendingQueue.clear();
    }

#ifdef _DEBUG
    LogDebug("[Hook] MbDvcCustomPopup: removed\n");
#endif
    return true;
}


static std::uint64_t PopupBgHashFromPath(const char* bgTexturePath)
{
    if (!bgTexturePath || !*bgTexturePath)
        return 0;
    return FoxHashes::PathCode64Ext(bgTexturePath);
}


static bool Show_MbDvcAnnouncePopup_Impl(std::uint8_t  reserveId,
                                         const char*   titleLiteral,
                                         std::uint64_t titleHash,
                                         const char*   bodyLiteral,
                                         std::uint64_t bodyHash,
                                         std::uint64_t bgTexture)
{
    PopupTextSource titleSrc;
    if (titleLiteral)
    {
        titleSrc.isHash  = false;
        titleSrc.literal = titleLiteral;
    }
    else
    {
        titleSrc.isHash = true;
        titleSrc.hash   = titleHash;
    }

    PopupTextSource bodySrc;
    if (bodyLiteral)
    {
        bodySrc.isHash  = false;
        bodySrc.literal = bodyLiteral;
    }
    else
    {
        bodySrc.isHash = true;
        bodySrc.hash   = bodyHash;
    }

    void* ctrl = g_PopupController.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        if (g_PendingQueue.size() >= 256)
        {
            LogDebug("[MbDvcCustomPopup] Show: %zu popups already pending; rejecting "
                "new entry (runaway-caller backstop)\n",
                g_PendingQueue.size());
            return false;
        }
        PendingPopup p;
        p.title     = std::move(titleSrc);
        p.body      = std::move(bodySrc);
        p.bgTexture = bgTexture;
        p.reserveId = reserveId;
        p.reserved  = false;
        g_PendingQueue.push_back(std::move(p));
    }

    if (!ctrl)
    {
        return true;
    }

    const bool reserveOk = SafeReserveOurSlot(ctrl, reserveId);
    if (!reserveOk)
    {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        if (!g_PendingQueue.empty())
            g_PendingQueue.pop_back();
        LogDebug("[MbDvcCustomPopup] Show: SafeReserveOurSlot raised; entry rolled back\n");
        return false;
    }

    const bool landed = SafeVerifyOurReservationLanded(ctrl, reserveId);
    {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        if (!g_PendingQueue.empty())
        {
            if (landed)
                g_PendingQueue.back().reserved = true;
            else
                g_PendingQueue.pop_back();
        }
    }

    if (!landed)
    {
        LogDebug("[MbDvcCustomPopup] Show: post-reserve verification found no "
                 "V_FrameWork slot (the ring was likely full) - entry rolled back\n");
        return false;
    }

    return true;
}


bool Show_MbDvcAnnouncePopupReport(const char* title, const char* body,
                                   const char* bgTexturePath)
{
    return Show_MbDvcAnnouncePopup_Impl(
        kReserveId_NormalSlot0,
        title ? title : "", 0,
        body ? body : "",  0,
        PopupBgHashFromPath(bgTexturePath));
}


bool Show_MbDvcAnnouncePopupByLangId(const char* titleLabel, const char* bodyLabel,
                                     const char* bgTexturePath)
{
    const char*  titleLit  = nullptr;
    std::uint64_t titleHash = 0;
    if (titleLabel && *titleLabel)
        titleHash = FoxHashes::StrCode64(titleLabel);
    else
        titleLit = "";

    const char*  bodyLit  = nullptr;
    std::uint64_t bodyHash = 0;
    if (bodyLabel && *bodyLabel)
        bodyHash = FoxHashes::StrCode64(bodyLabel);
    else
        bodyLit = "";

    return Show_MbDvcAnnouncePopup_Impl(
        kReserveId_NormalSlot0,
        titleLit, titleHash,
        bodyLit,  bodyHash,
        PopupBgHashFromPath(bgTexturePath));
}


bool Show_MbDvcAnnouncePopupReward(const char* title, const char* body,
                                   const char* bgTexturePath)
{
    constexpr std::uint8_t kServerSlot = 2;
    return Show_MbDvcAnnouncePopup_Impl(
        kServerSlot,
        title ? title : "", 0,
        body ? body : "",  0,
        PopupBgHashFromPath(bgTexturePath));
}


bool Show_MbDvcAnnouncePopupRewardLangId(const char* titleLabel,
                                         const char* bodyLabel,
                                         const char* bgTexturePath)
{
    constexpr std::uint8_t kServerSlot = 2;

    const char*  titleLit  = nullptr;
    std::uint64_t titleHash = 0;
    if (titleLabel && *titleLabel)
        titleHash = FoxHashes::StrCode64(titleLabel);
    else
        titleLit = "";

    const char*  bodyLit  = nullptr;
    std::uint64_t bodyHash = 0;
    if (bodyLabel && *bodyLabel)
        bodyHash = FoxHashes::StrCode64(bodyLabel);
    else
        bodyLit = "";

    return Show_MbDvcAnnouncePopup_Impl(
        kServerSlot,
        titleLit, titleHash,
        bodyLit,  bodyHash,
        PopupBgHashFromPath(bgTexturePath));
}


const char* MbDvcCustom_TryResolveLangText(std::uint64_t hash)
{
    void* lang = g_LangManager.load(std::memory_order_relaxed);
    if (!lang)
        return nullptr;
    return SafeResolveLangText(lang, hash);
}
