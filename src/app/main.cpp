#include "tray.h"
#include "injector.h"
#include "config.h"
#include "modern_gui.h"
#include "icon_gen.h"
#include "debug_log.h"
#include "update_manager.h"
#include "../common/types.h"
#include "../common/composition.h"
#include "window_animation.h"
#include "border_manager.h"
#include <string>
#include <shellapi.h>

#define WINDOW_CLASS_NAME L"LightencyMainWindow"
#define IDT_TASKBAR_RETRY_1 6001
#define IDT_TASKBAR_RETRY_2 6002
#define IDT_TASKBAR_RETRY_3 6003
#define WM_LIGHTENCY_APPLY_SETTINGS (WM_USER + 201)
#define WM_LIGHTENCY_OPEN_UI        (WM_USER + 202)
#define WM_LIGHTENCY_CHECK_UPDATES  (WM_USER + 203)

using namespace Lightency;

static AppConfig g_Config;
static HWND g_hMainWnd = nullptr;
static UINT g_wmTaskbarCreated = 0;
static bool g_bQuitting = false;



static void CleanShutdownAndRestoreSystem() {
    LogDebug("CleanShutdownAndRestoreSystem: start");
    BorderManager::Shutdown();
    WindowAnimation::Shutdown();
    ApplyNormalToAllTaskbars();
    Injector::Shutdown();
    LogDebug("CleanShutdownAndRestoreSystem: finish");
}

static void ApplyAllSettings() {
    LogDebug("ApplyAllSettings: start");
    Injector::Update(g_Config);
    WindowAnimation::Update(g_Config);
    BorderManager::Update(g_Config);
    LogDebug("ApplyAllSettings: after Injector::Update");

    if (g_Config.clearTaskbar) {
        ApplyClearToAllTaskbars();
    } else {
        ApplyNormalToAllTaskbars();
    }
    LogDebug("ApplyAllSettings: after clearTaskbar check");

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
    if (uMsg == WM_LIGHTENCY_CHECK_UPDATES) {
        LogDebug("MainWndProc: WM_LIGHTENCY_CHECK_UPDATES received");
        UpdateManager::CheckNow(hWnd);
        return 0;
    }

    switch (uMsg) {
    case WM_CREATE: {
        LogDebug("MainWndProc: WM_CREATE");
        if (g_Config.showTrayIcon) {
            TrayManager::Create(hWnd);
        }
        SetTimer(hWnd, IDT_TASKBAR_RETRY_1, 200, nullptr);
        SetTimer(hWnd, IDT_TASKBAR_RETRY_2, 600, nullptr);
        SetTimer(hWnd, IDT_TASKBAR_RETRY_3, 1500, nullptr);
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
        UINT msg = LOWORD(lParam);
        if (msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP || msg == NIN_SELECT) {
            ModernGUI::Show(GetModuleHandleW(nullptr), g_Config, OnConfigChangedCallback);
        } else if (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU) {
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

static void EnableDarkMode() {
    HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hUxtheme) return;

    enum PreferredAppMode {
        Default = 0,
        AllowDark = 1,
        ForceDark = 2,
        ForceLight = 3,
        Max = 4
    };
    using fnSetPreferredAppMode = PreferredAppMode(WINAPI*)(PreferredAppMode);
    using fnFlushMenuThemes = void(WINAPI*)();

    auto setPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135)));
    if (setPreferredAppMode) {
        setPreferredAppMode(ForceDark);
    }

    auto flushMenuThemes = reinterpret_cast<fnFlushMenuThemes>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136)));
    if (flushMenuThemes) {
        flushMenuThemes();
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int) {
    LogDebug("wWinMain: start");
    DisableProcessWindowsGhosting();
    EnableDarkMode();

    bool bQuit = (pCmdLine && (wcsstr(pCmdLine, L"--quit") || wcsstr(pCmdLine, L"-q")));
    bool bDaemon = (pCmdLine && (wcsstr(pCmdLine, L"--daemon") || wcsstr(pCmdLine, L"-d")));
    bool bCheckUpdates = (pCmdLine && (wcsstr(pCmdLine, L"--check-updates") || wcsstr(pCmdLine, L"-u")));

    HWND hExisting = FindWindowW(WINDOW_CLASS_NAME, nullptr);
    if (hExisting) {
        if (bQuit) {
            LogDebug("wWinMain: existing instance found, sending IDM_EXIT");
            PostMessageW(hExisting, WM_COMMAND, IDM_EXIT, 0);
            return 0;
        }
        if (bCheckUpdates) {
            PostMessageW(hExisting, WM_LIGHTENCY_CHECK_UPDATES, 0, 0);
            return 0;
        }
        LogDebug("wWinMain: existing instance found, posting WM_LIGHTENCY_OPEN_UI");
        PostMessageW(hExisting, WM_LIGHTENCY_OPEN_UI, 0, 0);
        return 0;
    }
    if (bQuit) {
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ConfigManager::Init();
    g_Config = ConfigManager::Load();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(
        0, WINDOW_CLASS_NAME, L"LightencyCore",
        WS_POPUP, 0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );
    if (g_hMainWnd) {
        HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
        if (hUxtheme) {
            using fnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
            auto allowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133)));
            if (allowDarkModeForWindow) {
                allowDarkModeForWindow(g_hMainWnd, true);
            }
            using fnSetWindowTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
            auto setWindowTheme = reinterpret_cast<fnSetWindowTheme>(GetProcAddress(hUxtheme, "SetWindowTheme"));
            if (setWindowTheme) {
                setWindowTheme(g_hMainWnd, L"DarkMode_Explorer", nullptr);
            }
        }
    }
    LogDebug("wWinMain: g_hMainWnd=" + std::to_string((unsigned long long)g_hMainWnd));

    ApplyAllSettings();

    if (bCheckUpdates) {
        UpdateManager::CheckNow(g_hMainWnd);
    } else if (g_Config.automaticUpdates) {
        UpdateManager::Start(g_hMainWnd);
    }

    if (!bDaemon && !bCheckUpdates) {
        LogDebug("wWinMain: showing GUI");
        ModernGUI::Show(hInstance, g_Config, OnConfigChangedCallback);
    }

    LogDebug("wWinMain: entering message loop");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogDebug("wWinMain: exit");
    return static_cast<int>(msg.wParam);
}
