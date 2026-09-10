#include "injector.h"
#include "payload_manager.h"
#include "debug_log.h"
#include <string>
#include <vector>

namespace Lightency {

namespace {
    void NudgeRealPointerOverTaskbar(HWND taskbar) {

        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
            (GetAsyncKeyState(VK_MBUTTON) & 0x8000)) {
            return;
        }

        POINT original{};
        RECT bounds{};
        if (!GetCursorPos(&original) || !GetWindowRect(taskbar, &bounds)) return;
        if (bounds.right - bounds.left < 4 || bounds.bottom <= bounds.top) return;

        const int targetX = bounds.left + (bounds.right - bounds.left) / 2;
        const int targetY = bounds.top + (bounds.bottom - bounds.top) / 2;
        SetCursorPos(targetX, targetY);
        Sleep(8);
        SetCursorPos(targetX + 1, targetY);
        Sleep(8);
        SetCursorPos(original.x, original.y);
    }

    void NotifyTaskbar(HWND taskbar, UINT msg, WPARAM wParam, LPARAM lParam) {


        DWORD_PTR result = 0;
        SendMessageTimeoutW(taskbar, msg, wParam, lParam,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result);
        NudgeRealPointerOverTaskbar(taskbar);
    }

    void BroadcastToTaskbars(UINT msg, WPARAM wParam, LPARAM lParam) {
        if (HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            NotifyTaskbar(hTray, msg, wParam, lParam);
        }
        HWND hSec = nullptr;
        while ((hSec = FindWindowExW(nullptr, hSec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
            NotifyTaskbar(hSec, msg, wParam, lParam);
        }
    }
}

HANDLE Injector::s_hMapFile = nullptr;
void* Injector::s_pSharedMemory = nullptr;
HMODULE Injector::s_hHookModule = nullptr;
HHOOK Injector::s_hExplorerHook = nullptr;

bool Injector::InstallExplorerHook() {
    if (s_hExplorerHook) return true;

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) {
        LogDebug("InstallExplorerHook: taskbar not found");
        return false;
    }
    DWORD explorerThreadId = GetWindowThreadProcessId(taskbar, nullptr);
    LogDebug("Explorer thread=" + std::to_string(explorerThreadId));
    if (!explorerThreadId) {
        LogDebug("InstallExplorerHook: taskbar thread not found");
        return false;
    }

    std::wstring dllPath = PayloadManager::GetHookDllPath();
    s_hHookModule = LoadLibraryW(dllPath.c_str());
    if (!s_hHookModule) {
        const DWORD error = GetLastError();
        LogDebug("LoadLibrary error=" + std::to_string(error));
        LogDebug("InstallExplorerHook: LoadLibrary failed");
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::wstring message = L"Lightency could not load its taskbar extension.\n\n"
                L"Extract all files from the release ZIP into the same folder. "
                L"The extension must be present beside lightency.exe.\n\n";
            message += dllPath + L"\nWindows error: " + std::to_wstring(error);
            MessageBoxW(nullptr, message.c_str(), L"Lightency", MB_OK | MB_ICONERROR);
        }
        return false;
    }

    auto hookProc = reinterpret_cast<HOOKPROC>(
        GetProcAddress(s_hHookModule, "LightencyHookProc"));
    if (!hookProc) {
        FreeLibrary(s_hHookModule);
        s_hHookModule = nullptr;
        return false;
    }

    s_hExplorerHook = SetWindowsHookExW(
        WH_CALLWNDPROC, hookProc, s_hHookModule, explorerThreadId);
    if (!s_hExplorerHook) {
        LogDebug("SetWindowsHookEx error=" + std::to_string(GetLastError()));
        LogDebug("InstallExplorerHook: SetWindowsHookEx failed");
        FreeLibrary(s_hHookModule);
        s_hHookModule = nullptr;
        return false;
    }


    SendMessageTimeoutW(taskbar, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 250, nullptr);
    LogDebug("Explorer hook installed");
    return true;
}

bool Injector::Initialize() {
    LogDebug("Injector::Initialize start");
    if (!s_hMapFile) {
        s_hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(SharedHookConfig), L"Lightency_Shared_Config_v7");

        if (s_hMapFile) {
            s_pSharedMemory = MapViewOfFile(s_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedHookConfig));
            if (s_pSharedMemory) {
                auto* pCfg = static_cast<SharedHookConfig*>(s_pSharedMemory);
                pCfg->masterPid = GetCurrentProcessId();
            }
        }
    }


    LogDebug("Shared mapping=" + std::to_string(s_hMapFile != nullptr) +
        " view=" + std::to_string(s_pSharedMemory != nullptr) +
        " winerr=" + std::to_string(GetLastError()));
    if (!InstallExplorerHook()) return false;

    LogDebug("Injector::Initialize end");
    return true;
}

void Injector::Shutdown() {
    if (s_pSharedMemory) {
        auto* pCfg = static_cast<SharedHookConfig*>(s_pSharedMemory);
        pCfg->dockAnimation = false;
        pCfg->masterPid = 0;
    }

    BroadcastToTaskbars(Lightency::GetLightencyUpdateMsg(), 0, 0);

    Sleep(120);

    if (s_hExplorerHook) {
        UnhookWindowsHookEx(s_hExplorerHook);
        s_hExplorerHook = nullptr;
    }
    if (s_hHookModule) {
        FreeLibrary(s_hHookModule);
        s_hHookModule = nullptr;
    }

    if (s_pSharedMemory) {
        UnmapViewOfFile(s_pSharedMemory);
        s_pSharedMemory = nullptr;
    }
    if (s_hMapFile) {
        CloseHandle(s_hMapFile);
        s_hMapFile = nullptr;
    }
}

void Injector::Invalidate() {
    if (s_hExplorerHook) {
        UnhookWindowsHookEx(s_hExplorerHook);
        s_hExplorerHook = nullptr;
    }
    if (s_hHookModule) {
        FreeLibrary(s_hHookModule);
        s_hHookModule = nullptr;
    }
}

void Injector::Update(const AppConfig& config) {
    LogDebug("Injector::Update start");
    LogDebug("Settings clear=" + std::to_string(config.clearTaskbar) +
        " dock=" + std::to_string(config.dockAnimation) +
        " tray=" + std::to_string(config.trayItems) +
        " start=" + std::to_string(config.layoutEditor));


    if (!s_pSharedMemory || !s_hExplorerHook) {
        Initialize();
    }

    if (s_pSharedMemory) {
        auto* pCfg = static_cast<SharedHookConfig*>(s_pSharedMemory);
        pCfg->dockAnimation = config.dockAnimation;
        pCfg->maxScale = config.dockMaxScale;
        pCfg->effectRadius = config.dockRadius;
        pCfg->spacingFactor = config.dockSpacing;
        pCfg->animationType = 0;
        pCfg->disableBounce = !config.dockBounce;
        pCfg->excludeSystemButtons = config.dockExcludeSystem;
        pCfg->autoRadius = config.dockAutoPhysics;
        pCfg->autoPhysics = config.dockAutoPhysics;
        pCfg->trayItems = config.trayItems;
        pCfg->hideTrayChevron = config.hideTrayChevron;
        pCfg->hideTrayLanguage = config.hideTrayLanguage;
        pCfg->hideTrayNetwork = config.hideTrayNetwork;
        pCfg->hideTrayVolume = config.hideTrayVolume;
        pCfg->hideTrayBattery = config.hideTrayBattery;
        pCfg->hideTrayClock = config.hideTrayClock;
        pCfg->layoutEditor = config.layoutEditor;
        pCfg->clearTaskbar = config.clearTaskbar;
        pCfg->hideTaskbarBorder = config.hideTaskbarBorder;
        pCfg->masterPid = GetCurrentProcessId();
    }

    BroadcastToTaskbars(Lightency::GetLightencyUpdateMsg(), 0, 0);
    LogDebug("Injector::Update end");
}

void Injector::SendEffect(TaskbarEffect effect, uint32_t tintColor, uint8_t opacity) {
    uint32_t fullColor = (static_cast<uint32_t>(opacity) << 24) | (tintColor & 0x00FFFFFF);
    BroadcastToTaskbars(WM_LIGHTENCY_APPLY, static_cast<WPARAM>(effect), static_cast<LPARAM>(fullColor));
}

}
