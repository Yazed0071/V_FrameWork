#pragma once

// Installs the logger hook for NoticeActionImpl::State_ComradeAction.
// Params: none
bool Install_StateComradeActionLog_Hook();

// Removes the logger hook.
// Params: none
bool Uninstall_StateComradeActionLog_Hook();