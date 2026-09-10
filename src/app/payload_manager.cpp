#include "payload_manager.h"
#include <windows.h>

namespace Lightency {

static std::wstring GetModuleDir() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return path.substr(0, lastSlash + 1);
    }
    return L"";
}

std::wstring PayloadManager::GetHookDllPath() {
    return GetModuleDir() + L"lightency_hook.dll";
}

}
