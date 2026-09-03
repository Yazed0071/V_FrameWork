#pragma once

#include <cstdint>

bool RequestToAttachInDemo(const char* ownerName, const char* connectPointName,
                           bool unattachOnSleep);
bool RequestToAttachInDemoById(std::uint32_t ownerObjectId, const char* connectPointName,
                               bool unattachOnSleep);
void ClearAttachInDemo();

void Note_PlayerGameObjectImpl(void* impl);

bool Install_PlayerAttachInDemo_Hook();
bool Uninstall_PlayerAttachInDemo_Hook();
