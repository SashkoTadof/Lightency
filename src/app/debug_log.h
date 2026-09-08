#pragma once
#include <windows.h>
#include <string>
inline void LogDebug(const std::string& msg) {
#ifdef _DEBUG
    OutputDebugStringA((msg + "\n").c_str());
#else
    (void)msg;
#endif
}
