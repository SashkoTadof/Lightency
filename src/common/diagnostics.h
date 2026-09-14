#pragma once
#include <string>

#ifdef LIGHTENCY_DIAGNOSTICS
#include <windows.h>
#include <mutex>
#include <cstdio>
#include <cstring>

inline void WriteDiagnostic(const char* component, const std::string& message) {
    const DWORD savedError = GetLastError();
    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);
    wchar_t base[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (length && length < MAX_PATH) {
        const std::wstring directory = std::wstring(base) + L"\\Lightency";
        CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring path = directory +
            (strcmp(component, "app") == 0 ? L"\\app.log" : L"\\explorer.log");
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            SYSTEMTIME now{};
            GetSystemTime(&now);
            char prefix[160]{};
            sprintf_s(prefix, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu tid=%lu [%s] ",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                now.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), component);
            const std::string line = std::string(prefix) + message + "\r\n";
            DWORD written = 0;
            WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    SetLastError(savedError);
}
#else
inline void WriteDiagnostic(const char*, const std::string&) {}
#endif
