#pragma once
#include <windows.h>
#include "../common/types.h"

#define WM_TRAYICON (WM_USER + 100)
#define IDT_TRAY_RETRY 5001

#define IDM_OPEN_CONSOLE       3001
#define IDM_TOGGLE_CLEAR       3002
#define IDM_TOGGLE_DOCK        3003
#define IDM_EXIT               3099

namespace Lightency {

class TrayManager {
public:
    static bool Create(HWND hWnd);
    static void Destroy(HWND hWnd);
    static void Reset();
    static void ShowContextMenu(HWND hWnd, const AppConfig& config);
};

}
