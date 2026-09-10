#pragma once
#include <windows.h>
#include <unknwn.h>
#include "../common/types.h"
namespace Lightency {
class DockAnimation {
public:
    static bool Initialize();
    static void Shutdown();
    static void UpdateSettings(const SharedHookConfig& config);
    static void RefreshSettings();
    static void OnWindowCreated(HWND hWnd);
    static void AttachXamlElement(IUnknown* element);
};
}
