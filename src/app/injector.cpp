#include "injector.h"
#include "branded_dialog.h"
#include "payload_manager.h"
#include "debug_log.h"
#include <string>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>

namespace Lightency {

namespace {
    std::vector<HHOOK> g_startMenuThreadHooks;
    std::vector<DWORD> g_startMenuThreadIds;
    DWORD g_startMenuHostPid = 0;
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

    bool IsProcessWindow(HWND window, const wchar_t* executable) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (!process) return false;
        wchar_t path[MAX_PATH]{};
        DWORD size = ARRAYSIZE(path);
        const bool queried = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
        CloseHandle(process);
        if (!queried) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, executable) == 0;
    }

    bool ApplyStartAndSearchFrameSize(const SharedHookConfig* config) {
        if (!config || !config->startMenuSizing) return false;
        struct Context { const SharedHookConfig* config; bool changed = false; } context{ config };
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto& context = *reinterpret_cast<Context*>(parameter);
            if (!IsWindowVisible(window) || !IsProcessWindow(window, L"ApplicationFrameHost.exe")) return TRUE;
            wchar_t className[64]{}, title[256]{};
            GetClassNameW(window, className, ARRAYSIZE(className));
            GetWindowTextW(window, title, ARRAYSIZE(title));
            if (_wcsicmp(className, L"ApplicationFrameWindow") != 0) return TRUE;
            const bool isStart = wcsstr(title, L"StartDocked") || wcsstr(title, L"Пуск");
            const bool isSearch = !isStart && (wcsstr(title, L"Search") || wcsstr(title, L"Поиск"));
            if (!isStart && !isSearch) return TRUE;
            RECT rect{}; if (!GetWindowRect(window, &rect)) return TRUE;
            const UINT dpi = GetDpiForWindow(window);
            const int width = MulDiv(isStart ? context.config->startMenuWidth : context.config->searchWidth, dpi, 96);
            const int height = MulDiv(isStart ? context.config->startMenuHeight : context.config->searchHeight, dpi, 96);
            const int centerX = (rect.left + rect.right) / 2;
            const int bottom = rect.bottom;
            const int x = centerX - width / 2;
            const int y = bottom - height;
            if (rect.right - rect.left != width || rect.bottom - rect.top != height) {
                context.changed |= SetWindowPos(window, nullptr, x, y, width, height,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER) != FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return context.changed;
    }

    HWND FindStartMenuWindow() {
        struct Context { HWND best = nullptr; } context;
        EnumWindows([](HWND frame, LPARAM parameter) -> BOOL {
            auto& context = *reinterpret_cast<Context*>(parameter);
            if (!IsProcessWindow(frame, L"ApplicationFrameHost.exe")) return TRUE;
            EnumChildWindows(frame, [](HWND child, LPARAM inner) -> BOOL {
                auto& context = *reinterpret_cast<Context*>(inner);
                wchar_t className[96]{};
                GetClassNameW(child, className, ARRAYSIZE(className));
                if (_wcsicmp(className, L"Windows.UI.Core.CoreWindow") == 0 &&
                    IsProcessWindow(child, L"StartMenuExperienceHost.exe")) {
                    context.best = child;
                    return FALSE;
                }
                return TRUE;
            }, parameter);
            return context.best ? FALSE : TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        if (context.best) return context.best;
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto& context = *reinterpret_cast<Context*>(parameter);
            // Windows 11 moves the XAML island between these hosts depending
            // on the Start/Search version.  The classic host often has no
            // window at all, while ShellExperienceHost owns the visible one.
            const bool startHost = IsProcessWindow(window, L"StartMenuExperienceHost.exe") ||
                IsProcessWindow(window, L"ShellExperienceHost.exe") ||
                IsProcessWindow(window, L"SearchHost.exe");
            if (!startHost) return TRUE;
            wchar_t className[96]{};
            GetClassNameW(window, className, ARRAYSIZE(className));
            context.best = window;
            return _wcsicmp(className, L"Windows.UI.Core.CoreWindow") != 0;
        }, reinterpret_cast<LPARAM>(&context));
        return context.best;
    }

    std::vector<DWORD> FindStartMenuThreads() {
        std::vector<DWORD> result;
        DWORD wanted[3]{}; int count = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return result;
        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, L"StartMenuExperienceHost.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"ShellExperienceHost.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"SearchHost.exe") == 0) {
                wanted[count++] = pe.th32ProcessID;
                if (count == 3) break;
            }
        }
        CloseHandle(snap);
        if (!count) return result;
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return result;
        THREADENTRY32 te{ sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            for (int i = 0; i < count; ++i) if (te.th32OwnerProcessID == wanted[i]) {
                result.push_back(te.th32ThreadID); break;
            }
        }
        CloseHandle(snap);
        return result;
    }

    DWORD FindStartMenuHostPid() {
        DWORD result = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, L"StartMenuExperienceHost.exe") == 0) {
                result = pe.th32ProcessID;
                break;
            }
        }
        CloseHandle(snap);
        return result;
    }

    bool IsThreadAlive(DWORD threadId) {
        if (!threadId) return false;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
        if (!thread) return false;
        DWORD exitCode = 0;
        const bool alive = GetExitCodeThread(thread, &exitCode) && exitCode == STILL_ACTIVE;
        CloseHandle(thread);
        return alive;
    }
}

HANDLE Injector::s_hMapFile = nullptr;
void* Injector::s_pSharedMemory = nullptr;
HMODULE Injector::s_hHookModule = nullptr;
HHOOK Injector::s_hExplorerHook = nullptr;
HHOOK Injector::s_hStartMenuHook = nullptr;
HWND Injector::s_hStartMenuWindow = nullptr;
DWORD Injector::s_startMenuThreadId = 0;

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
            BrandedDialog::Show(nullptr, message, BrandedDialog::Kind::Error);
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

bool Injector::InstallStartMenuHook() {
    const DWORD hostPid = FindStartMenuHostPid();
    if (s_hStartMenuHook && hostPid && g_startMenuHostPid == hostPid &&
        IsThreadAlive(s_startMenuThreadId)) return true;
    HWND window = FindStartMenuWindow();
    auto threads = FindStartMenuThreads();
    if (window) threads.insert(threads.begin(), GetWindowThreadProcessId(window, nullptr));
    std::sort(threads.begin(), threads.end());
    threads.erase(std::unique(threads.begin(), threads.end()), threads.end());
    if (threads.empty()) {
        LogDebug("InstallStartMenuHook: host thread not found");
        return false;
    }
    if (s_hStartMenuHook) UnhookWindowsHookEx(s_hStartMenuHook);
    s_hStartMenuHook = nullptr;
    for (HHOOK hook : g_startMenuThreadHooks) UnhookWindowsHookEx(hook);
    g_startMenuThreadHooks.clear();
    g_startMenuThreadIds.clear();
    g_startMenuHostPid = 0;
    s_hStartMenuWindow = window;
    if (!s_hHookModule && !InstallExplorerHook()) return false;
    auto getProc = reinterpret_cast<HOOKPROC>(GetProcAddress(s_hHookModule,
        "LightencyGetMessageProc"));
    auto callProc = reinterpret_cast<HOOKPROC>(GetProcAddress(s_hHookModule, "LightencyHookProc"));
    for (DWORD threadId : threads) {
        HHOOK callHook = callProc ? SetWindowsHookExW(
            WH_CALLWNDPROC, callProc, s_hHookModule, threadId) : nullptr;
        if (callHook) {
            if (!s_hStartMenuHook) {
                s_hStartMenuHook = callHook;
                s_startMenuThreadId = threadId;
            } else {
                g_startMenuThreadHooks.push_back(callHook);
            }
            g_startMenuThreadIds.push_back(threadId);
        }
        HHOOK getHook = getProc ? SetWindowsHookExW(
            WH_GETMESSAGE, getProc, s_hHookModule, threadId) : nullptr;
        if (getHook) g_startMenuThreadHooks.push_back(getHook);
        PostThreadMessageW(threadId, Lightency::GetLightencyUpdateMsg(), 0, 0);
    }
    if (!s_hStartMenuHook) {
        LogDebug("InstallStartMenuHook failed error=" + std::to_string(GetLastError()));
        return false;
    }
    g_startMenuHostPid = hostPid;
    if (window) SendMessageTimeoutW(window, Lightency::GetLightencyUpdateMsg(), 0, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 750, nullptr);
    LogDebug("Start menu hook installed");
    return true;
}

bool Injector::EnsureStartMenuHook() {
    const bool hooked = InstallStartMenuHook();
    const bool resized = s_pSharedMemory &&
        ApplyStartAndSearchFrameSize(static_cast<const SharedHookConfig*>(s_pSharedMemory));
    return hooked || resized;
}

bool Injector::Initialize() {
    LogDebug("Injector::Initialize start");
    if (!s_hMapFile) {
        s_hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(SharedHookConfig), SHARED_HOOK_CONFIG_MAPPING_NAME);

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
    EnsureStartMenuHook();

    LogDebug("Injector::Initialize end");
    return true;
}

void Injector::Shutdown() {
    if (s_pSharedMemory) {
        auto* pCfg = static_cast<SharedHookConfig*>(s_pSharedMemory);
        pCfg->dockAnimation = false;
        pCfg->startMenuSizing = false;
        pCfg->masterPid = 0;
    }

    BroadcastToTaskbars(Lightency::GetLightencyUpdateMsg(), 0, 0);
    if (s_hStartMenuWindow) SendMessageTimeoutW(s_hStartMenuWindow,
        Lightency::GetLightencyUpdateMsg(), 0, 0, SMTO_ABORTIFHUNG, 500, nullptr);

    Sleep(120);

    if (s_hExplorerHook) {
        UnhookWindowsHookEx(s_hExplorerHook);
        s_hExplorerHook = nullptr;
    }
    if (s_hStartMenuHook) {
        UnhookWindowsHookEx(s_hStartMenuHook);
        s_hStartMenuHook = nullptr;
    }
    for (HHOOK hook : g_startMenuThreadHooks) UnhookWindowsHookEx(hook);
    g_startMenuThreadHooks.clear();
    g_startMenuThreadIds.clear();
    g_startMenuHostPid = 0;
    s_hStartMenuWindow = nullptr;
    s_startMenuThreadId = 0;
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
    if (s_hStartMenuHook) {
        UnhookWindowsHookEx(s_hStartMenuHook);
        s_hStartMenuHook = nullptr;
    }
    for (HHOOK hook : g_startMenuThreadHooks) UnhookWindowsHookEx(hook);
    g_startMenuThreadHooks.clear();
    g_startMenuThreadIds.clear();
    g_startMenuHostPid = 0;
    s_hStartMenuWindow = nullptr;
    s_startMenuThreadId = 0;
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
        pCfg->dragDropAssist = config.dragDropAssist;
        pCfg->clearTaskbar = config.clearTaskbar;
        pCfg->hideTaskbarBorder = config.hideTaskbarBorder;
        pCfg->startMenuSizing = config.startMenuSizing;
        pCfg->startMenuWidth = config.startMenuWidth;
        pCfg->startMenuHeight = config.startMenuHeight;
        pCfg->searchWidth = config.searchWidth;
        pCfg->searchHeight = config.searchHeight;
        pCfg->startHideSearch = config.startHideSearch;
        pCfg->startHidePinned = config.startHidePinned;
        pCfg->startHideRecommended = config.startHideRecommended;
        pCfg->startHideProfile = config.startHideProfile;
        pCfg->startHidePower = config.startHidePower;
        pCfg->startHideViewSelector = config.startHideViewSelector;
        pCfg->startHideFolders = config.startHideFolders;
        pCfg->startIconCustom = config.startIconCustom;
        pCfg->startIconSize = config.startIconSize;
        pCfg->startIconAccentColor = config.startIconAccentColor;
        pCfg->startIconColorHue = config.startIconColorHue;
        pCfg->masterPid = GetCurrentProcessId();
    }

    BroadcastToTaskbars(Lightency::GetLightencyUpdateMsg(), 0, 0);
    if (s_hStartMenuWindow && IsWindow(s_hStartMenuWindow)) {
        SendMessageTimeoutW(s_hStartMenuWindow, Lightency::GetLightencyUpdateMsg(),
            0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, nullptr);
    }
    else if (!g_startMenuThreadIds.empty()) {
        for (DWORD threadId : g_startMenuThreadIds)
            PostThreadMessageW(threadId, Lightency::GetLightencyUpdateMsg(), 0, 0);
    }
    if (s_pSharedMemory)
        ApplyStartAndSearchFrameSize(static_cast<const SharedHookConfig*>(s_pSharedMemory));
    LogDebug("Injector::Update end");
}

void Injector::SendEffect(TaskbarEffect effect, uint32_t tintColor, uint8_t opacity) {
    uint32_t fullColor = (static_cast<uint32_t>(opacity) << 24) | (tintColor & 0x00FFFFFF);
    BroadcastToTaskbars(WM_LIGHTENCY_APPLY, static_cast<WPARAM>(effect), static_cast<LPARAM>(fullColor));
}

}
