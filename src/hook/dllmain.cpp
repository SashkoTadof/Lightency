#include <windows.h>
#include <thread>
#include <atomic>
#include "dock_animation.h"
#include "../common/types.h"

static std::atomic<bool> g_shutdownStarted{ false };
static std::atomic<bool> g_hookReady{ false };

extern "C" __declspec(dllexport) LRESULT CALLBACK LightencyHookProc(
    int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam && g_hookReady.load(std::memory_order_acquire)) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message->message == Lightency::GetLightencyUpdateMsg()) {


            Lightency::DockAnimation::RefreshSettings();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static bool IsExplorerProcess() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, ARRAYSIZE(path))) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"explorer.exe") == 0;
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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);


        if (!IsExplorerProcess()) return TRUE;
        std::thread([hModule]() {
            g_hookReady.store(
                Lightency::DockAnimation::Initialize(),
                std::memory_order_release);

            HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Lightency_Shared_Config_v7");
            auto* config = mapping ? static_cast<Lightency::SharedHookConfig*>(
                MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Lightency::SharedHookConfig))) : nullptr;


            while (config) {
                Sleep(750);
                DWORD ownerPid = config->masterPid;
                if (ownerPid == 0 || !IsOwnerProcessAlive(ownerPid)) break;
            }

            if (config) UnmapViewOfFile(config);
            if (mapping) CloseHandle(mapping);

            if (!g_shutdownStarted.exchange(true)) {
                g_hookReady.store(false, std::memory_order_release);
                Lightency::DockAnimation::Shutdown();
            }
            FreeLibraryAndExitThread(hModule, 0);
        }).detach();
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (IsExplorerProcess() && lpReserved == nullptr && !g_shutdownStarted.exchange(true)) {
            g_hookReady.store(false, std::memory_order_release);
            Lightency::DockAnimation::Shutdown();
        }
    }
    return TRUE;
}
