#include "tray.h"
#include "icon_gen.h"
#include <shellapi.h>

namespace Lightency {

namespace {
    NOTIFYICONDATAW g_nid = {};
    bool g_created = false;
    HICON g_hTrayIcon = nullptr;
}

void TrayManager::Reset() {
    g_created = false;
}

bool TrayManager::Create(HWND hWnd) {
    if (!g_hTrayIcon) {
        int iconSize = GetSystemMetrics(SM_CXSMICON);
        if (iconSize <= 0) iconSize = 16;
        g_hTrayIcon = IconGenerator::CreateMinimalistIcon(iconSize);
    }

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hTrayIcon;

    wcsncpy_s(g_nid.szTip, _countof(g_nid.szTip), L"Lightency", _TRUNCATE);

    if (Shell_NotifyIconW(NIM_ADD, &g_nid) || Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
        g_created = true;
        KillTimer(hWnd, IDT_TRAY_RETRY);
        return true;
    }

    SetTimer(hWnd, IDT_TRAY_RETRY, 1000, nullptr);
    return false;
}

void TrayManager::Destroy(HWND hWnd) {
    KillTimer(hWnd, IDT_TRAY_RETRY);

    if (g_created) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_created = false;
    }

    if (g_hTrayIcon) {
        DestroyIcon(g_hTrayIcon);
        g_hTrayIcon = nullptr;
    }
}

void TrayManager::ShowContextMenu(HWND hWnd, const AppConfig&) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CONSOLE, L"Open Lightency");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessageW(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

}