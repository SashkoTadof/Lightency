#include <windows.h>
#include <shellscalingapi.h>
#include <string>
#include <thread>
#include <tlhelp32.h>
#include "config.h"
#include "injector.h"
#include "tray.h"
#include "modern_gui.h"
#include "debug_log.h"
#include "update_manager.h"
#include "../common/composition.h"
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")
using namespace Lightency;
static const wchar_t* WINDOW_CLASS_NAME = L"LightencyMessageWindow";
static constexpr UINT WM_LIGHTENCY_OPEN_UI = WM_APP + 100;
static constexpr UINT WM_LIGHTENCY_APPLY_SETTINGS = WM_APP + 101;
static constexpr UINT_PTR IDT_TASKBAR_RETRY_1 = 101;
static constexpr UINT_PTR IDT_TASKBAR_RETRY_2 = 102;
static constexpr UINT_PTR IDT_TASKBAR_RETRY_3 = 103;
static UINT g_wmTaskbarCreated = 0;
static HWND g_hMainWnd = nullptr;
static AppConfig g_Config;
static bool g_bQuitting = false;
static void InitThemeDarkMode() {
    using pfnSetPreferredAppMode = int(WINAPI*)(int);
    HMODULE hUxTheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxTheme) {
        pfnSetPreferredAppMode fnSetPreferredAppMode =
            reinterpret_cast<pfnSetPreferredAppMode>(GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135)));
        if (fnSetPreferredAppMode) {
            fnSetPreferredAppMode(2);
        }
    }
}
static void CleanLegacyShortcutArrowKey() {
    HKEY hKeyCU;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Icons", 0, KEY_SET_VALUE, &hKeyCU) == ERROR_SUCCESS) {
        RegDeleteValueW(hKeyCU, L"29");
        RegCloseKey(hKeyCU);
    }
    HKEY hKeyLM;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Icons", 0, KEY_SET_VALUE, &hKeyLM) == ERROR_SUCCESS) {
        RegDeleteValueW(hKeyLM, L"29");
        RegCloseKey(hKeyLM);
    }
}
static void CleanShutdownAndRestoreSystem() {
    Injector::Shutdown();
    ApplyNormalToAllTaskbars();
}
static void ApplyAllSettings() {
    LogDebug("ApplyAllSettings: start");
    g_Config.hideTaskbarBorder = g_Config.clearTaskbar;


    if (g_Config.clearTaskbar) ApplyClearToAllTaskbars();
    else ApplyNormalToAllTaskbars();
    Injector::Update(g_Config);
    LogDebug("ApplyAllSettings: after Injector::Update");
    if (g_hMainWnd) {
        if (g_Config.showTrayIcon) {
            TrayManager::Create(g_hMainWnd);
        } else {
            TrayManager::Destroy(g_hMainWnd);
        }
    }
    LogDebug("ApplyAllSettings: finish");
}
static void OnConfigChangedCallback() {
    ApplyAllSettings();
    ConfigManager::Save(g_Config);
}
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == g_wmTaskbarCreated) {
        LogDebug("MainWndProc: g_wmTaskbarCreated received");
        Injector::Invalidate();
        ApplyAllSettings();
        SetTimer(hWnd, IDT_TASKBAR_RETRY_1, 100, nullptr);
        SetTimer(hWnd, IDT_TASKBAR_RETRY_2, 350, nullptr);
        SetTimer(hWnd, IDT_TASKBAR_RETRY_3, 900, nullptr);
        return 0;
    }
    if (uMsg == WM_LIGHTENCY_APPLY_SETTINGS) {
        LogDebug("MainWndProc: WM_LIGHTENCY_APPLY_SETTINGS received");
        ApplyAllSettings();
        SetTimer(hWnd, IDT_TASKBAR_RETRY_1, 100, nullptr);
        SetTimer(hWnd, IDT_TASKBAR_RETRY_2, 350, nullptr);
        SetTimer(hWnd, IDT_TASKBAR_RETRY_3, 900, nullptr);
        return 0;
    }
    if (uMsg == WM_LIGHTENCY_OPEN_UI) {
        LogDebug("MainWndProc: WM_LIGHTENCY_OPEN_UI received");
        ModernGUI::Show(GetModuleHandleW(nullptr), g_Config, OnConfigChangedCallback);
        return 0;
    }
    switch (uMsg) {
    case WM_CREATE: {
        LogDebug("MainWndProc: WM_CREATE");
        if (g_Config.showTrayIcon) {
            TrayManager::Create(hWnd);
        }
        return 0;
    }
    case WM_TIMER: {
        if (wParam == IDT_TRAY_RETRY) {
            if (g_Config.showTrayIcon) {
                TrayManager::Create(hWnd);
            } else {
                KillTimer(hWnd, IDT_TRAY_RETRY);
            }
            return 0;
        }
        if (wParam >= IDT_TASKBAR_RETRY_1 && wParam <= IDT_TASKBAR_RETRY_3) {
            KillTimer(hWnd, wParam);
            LogDebug("MainWndProc: retry timer fired: " + std::to_string(wParam));
            Injector::Update(g_Config);
            if (g_Config.clearTaskbar) ApplyClearToAllTaskbars();
            else ApplyNormalToAllTaskbars();
            return 0;
        }
        return 0;
    }
    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            ModernGUI::Show(GetModuleHandleW(nullptr), g_Config, OnConfigChangedCallback);
        } else if (lParam == WM_RBUTTONUP) {
            TrayManager::ShowContextMenu(hWnd, g_Config);
        }
        return 0;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDM_OPEN_CONSOLE:
            ModernGUI::Show(GetModuleHandleW(nullptr), g_Config, OnConfigChangedCallback);
            break;
        case IDM_TOGGLE_CLEAR:
            g_Config.clearTaskbar = !g_Config.clearTaskbar;
            OnConfigChangedCallback();
            break;
        case IDM_TOGGLE_DOCK:
            g_Config.dockAnimation = !g_Config.dockAnimation;
            OnConfigChangedCallback();
            break;
        case IDM_EXIT:
            g_bQuitting = true;
            CleanShutdownAndRestoreSystem();
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    }
    case WM_CLOSE:
        LogDebug("MainWndProc: WM_CLOSE");
        return 0;
    case WM_DESTROY:
        LogDebug("MainWndProc: WM_DESTROY (g_bQuitting=" + std::to_string(g_bQuitting) + ")");
        if (g_bQuitting) {
            ModernGUI::Close();
            TrayManager::Destroy(hWnd);
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int) {
    LogDebug("wWinMain: start");
    DisableProcessWindowsGhosting();
    HWND hExisting = FindWindowW(WINDOW_CLASS_NAME, nullptr);
    if (hExisting) {
        LogDebug("wWinMain: existing instance found, posting WM_LIGHTENCY_OPEN_UI");
        PostMessageW(hExisting, WM_LIGHTENCY_OPEN_UI, 0, 0);
        return 0;
    }
    InitThemeDarkMode();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ConfigManager::Init();
    g_Config = ConfigManager::Load();
    std::wstring cmd = pCmdLine ? pCmdLine : L"";
    bool daemonMode = (cmd.find(L"--daemon") != std::wstring::npos || cmd.find(L"-d") != std::wstring::npos);
    bool oneShotApply = (cmd.find(L"--apply") != std::wstring::npos || cmd.find(L"-a") != std::wstring::npos);
    if (oneShotApply) {
        LogDebug("wWinMain: oneShotApply mode");
        ApplyAllSettings();
        ConfigManager::Save(g_Config);
        return 0;
    }
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClassExW(&wc);
    g_hMainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, WINDOW_CLASS_NAME, L"LightencyMessageWindow",
                                 WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    LogDebug("wWinMain: g_hMainWnd=" + std::to_string((unsigned long long)g_hMainWnd));
    if (g_hMainWnd) {
        ChangeWindowMessageFilterEx(g_hMainWnd, g_wmTaskbarCreated, MSGFLT_ALLOW, nullptr);
    }
    CleanLegacyShortcutArrowKey();
    ApplyAllSettings();
    if (g_Config.automaticUpdates) UpdateManager::Start(g_hMainWnd);
    if (!daemonMode) {
        LogDebug("wWinMain: showing GUI");
        ModernGUI::Show(hInstance, g_Config, OnConfigChangedCallback);
    }
    LogDebug("wWinMain: entering message loop");
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (bRet == -1) {
            LogDebug("wWinMain: GetMessage error: " + std::to_string(GetLastError()));
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogDebug("wWinMain: exited message loop (bRet=" + std::to_string(bRet) + ")");
    return 0;
}
