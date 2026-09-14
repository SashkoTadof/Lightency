#include <windows.h>
#include <thread>
#include <atomic>
#include "dock_animation.h"
#include "start_menu_size.h"
#include "../common/types.h"
#include "../common/diagnostics.h"

static std::atomic<bool> g_shutdownStarted{ false };
static std::atomic<bool> g_hookReady{ false };
static std::atomic<bool> g_workerRunning{ false };

static bool IsExplorerProcess() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"explorer.exe") == 0;
}

static bool IsStartMenuProcess() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"StartMenuExperienceHost.exe") == 0 ||
        _wcsicmp(name, L"ShellExperienceHost.exe") == 0 ||
        _wcsicmp(name, L"SearchHost.exe") == 0;
}

static bool IsOwnerProcessAlive(DWORD pid) {
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD exitCode = 0;
    bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

static std::thread g_workerThread;

static void StartExtensionWorker(HMODULE hModule) {
    if (g_workerRunning.exchange(true, std::memory_order_acq_rel)) {
        if (hModule) FreeLibrary(hModule);
        return;
    }

    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }

    g_workerThread = std::thread([hModule]() {
        WriteDiagnostic("explorer", "Extension worker started");
        const bool ready = Lightency::DockAnimation::Initialize();
        g_hookReady.store(ready, std::memory_order_release);
        WriteDiagnostic("explorer", "Initialize ready=" + std::to_string(ready));

        if (!ready) {
            g_workerRunning.store(false, std::memory_order_release);
            if (hModule) FreeLibraryAndExitThread(hModule, 0);
            return;
        }

        if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            SendMessageTimeoutW(taskbar, Lightency::GetLightencyUpdateMsg(),
                0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
        }

        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, Lightency::SHARED_HOOK_CONFIG_MAPPING_NAME);
        WriteDiagnostic("explorer", "Owner mapping=" + std::to_string(mapping != nullptr) +
            " winerr=" + std::to_string(GetLastError()));
        auto* config = mapping ? static_cast<Lightency::SharedHookConfig*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Lightency::SharedHookConfig))) : nullptr;

        if (config) {
            UINT_PTR timerId = SetTimer(nullptr, 0, 250, nullptr);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_TIMER && msg.wParam == timerId) {
                    const DWORD ownerPid = config->masterPid;
                    if (ownerPid == 0 || !IsOwnerProcessAlive(ownerPid)) break;
                }
            }
            KillTimer(nullptr, timerId);
        }

        g_hookReady.store(false, std::memory_order_release);
        Lightency::DockAnimation::Shutdown();
        if (config) UnmapViewOfFile(config);
        if (mapping) CloseHandle(mapping);
        g_workerRunning.store(false, std::memory_order_release);
        WriteDiagnostic("explorer", "Extension worker stopped");
        if (hModule) FreeLibraryAndExitThread(hModule, 0);
    });
}

extern "C" __declspec(dllexport) LRESULT CALLBACK LightencyHookProc(
    int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (IsStartMenuProcess()) {
            thread_local bool initialized = false;
            if (!initialized || message->message == Lightency::GetLightencyUpdateMsg())
                initialized = Lightency::StartMenuSize::RefreshSettings();
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        if (message->message == Lightency::GetLightencyUpdateMsg()) {
            if (g_hookReady.load(std::memory_order_acquire)) {
                Lightency::DockAnimation::RefreshSettings();
            } else {
                HMODULE hSelf = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(StartExtensionWorker), &hSelf);
                StartExtensionWorker(hSelf);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

extern "C" __declspec(dllexport) LRESULT CALLBACK LightencyGetMessageProc(
    int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam) {
        const auto* message = reinterpret_cast<const MSG*>(lParam);
        if (message->message == Lightency::GetLightencyUpdateMsg() && IsStartMenuProcess()) {
            Lightency::StartMenuSize::RefreshSettings();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

extern "C" __declspec(dllexport) DWORD WINAPI LightencyInitializeStartMenu(void*) {
    return Lightency::StartMenuSize::RefreshSettings() ? 1 : 0;
}

extern "C" __declspec(dllexport) LRESULT CALLBACK LightencyCbtProc(
    int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_MINMAX && wParam &&
        (lParam == SW_MINIMIZE || lParam == SW_SHOWMINIMIZED ||
         lParam == SW_SHOWMINNOACTIVE)) {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE,
            L"Lightency_WindowAnimation_v1");
        if (mapping) {
            auto* ipc = static_cast<Lightency::WindowAnimationIpc*>(MapViewOfFile(
                mapping, FILE_MAP_READ, 0, 0, sizeof(Lightency::WindowAnimationIpc)));
            if (ipc && ipc->enabled && IsWindow(ipc->receiverWindow)) {
                PostMessageW(ipc->receiverWindow,
                    Lightency::GetWindowAnimationEarlyMsg(), wParam, 0);
            }
            if (ipc) UnmapViewOfFile(ipc);
            CloseHandle(mapping);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (IsStartMenuProcess()) {
            Lightency::StartMenuSize::Shutdown();
        }
    }
    return TRUE;
}
