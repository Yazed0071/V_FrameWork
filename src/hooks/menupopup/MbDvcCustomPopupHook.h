#pragma once

#include <cstdint>

bool Install_MbDvcCustomPopup_Hook();

bool Uninstall_MbDvcCustomPopup_Hook();

bool Show_MbDvcAnnouncePopupReport(const char* title, const char* body,
                                   const char* bgTexturePath = nullptr);

bool Show_MbDvcAnnouncePopupByLangId(const char* titleLabel,
                                     const char* bodyLabel,
                                     const char* bgTexturePath = nullptr);

bool Show_MbDvcAnnouncePopupReward(const char* title, const char* body,
                                   const char* bgTexturePath = nullptr);

bool Show_MbDvcAnnouncePopupRewardLangId(const char* titleLabel,
                                         const char* bodyLabel,
                                         const char* bgTexturePath = nullptr);

const char* MbDvcCustom_TryResolveLangText(std::uint64_t hash);

bool Set_MbDvcEmergencyPopup        (const char* title,      const char* body);
bool Set_MbDvcEmergencyPopupLangId  (const char* titleLabel, const char* bodyLabel);
void Clear_MbDvcEmergencyPopupOverride();
