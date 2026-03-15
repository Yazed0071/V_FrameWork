#pragma once
#include <cstdio>
#include <cstdarg>

void InitLog();
void Log(const char* fmt, ...);
void CloseLog();