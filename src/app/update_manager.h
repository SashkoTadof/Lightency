#pragma once
#include <windows.h>
#include <string>

namespace Lightency {
class UpdateManager {
public:
    static void Start(HWND owner);
    static void CheckNow(HWND owner);
    static void Shutdown();
    static bool ApplyStagedUpdate(const std::wstring& tempDir, DWORD ownerPid,
        const std::wstring& targetExe, std::wstring& errorMessage);
};
}
