#pragma once
#include <windows.h>
#include <string>
#include "../common/types.h"
namespace Lightency {
class Injector {
public:
    static bool Initialize();
    static void Shutdown();
    static void Invalidate();
    static void Update(const AppConfig& config);
    static bool EnsureStartMenuHook();
    static void SendEffect(TaskbarEffect effect, uint32_t tintColor = 0, uint8_t opacity = 0);
private:
    static bool InstallExplorerHook();
    static bool InstallStartMenuHook();
    static HANDLE s_hMapFile;
    static void* s_pSharedMemory;
    static HMODULE s_hHookModule;
    static HHOOK s_hExplorerHook;
    static HHOOK s_hStartMenuHook;
    static HWND s_hStartMenuWindow;
    static DWORD s_startMenuThreadId;
};
}
