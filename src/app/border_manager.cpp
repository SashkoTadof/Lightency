#include "border_manager.h"
#include <dwmapi.h>
#include <atomic>
#include <unordered_set>
#include <mutex>
#include <string>

namespace Lightency::BorderManager {
namespace {

constexpr DWORD kDwmBorderAttribute = 34;
constexpr COLORREF kColorInvisible = 0xFFFFFFFE;
constexpr COLORREF kColorDefault = 0xFFFFFFFF;

std::atomic<bool> g_enabled{false};
HWINEVENTHOOK g_hookForeground = nullptr;
HWINEVENTHOOK g_hookShow = nullptr;
HWINEVENTHOOK g_hookDestroy = nullptr;
std::mutex g_windowsMutex;
std::unordered_set<HWND> g_processedWindows;

bool IsEligibleWindow(HWND window) {
    if (!window || !IsWindow(window)) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if ((style & WS_CHILD) || (style & WS_MINIMIZE) || !(style & WS_VISIBLE)) {
        return false;
    }

    const bool hasStandardFrame =
        (style & WS_CAPTION) != 0 ||
        (style & WS_THICKFRAME) != 0;

    if (!hasStandardFrame) {
        return false;
    }

    if ((style & WS_POPUP) && !(style & WS_CAPTION)) {
        return false;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) {
        return false;
    }

    if (GetAncestor(window, GA_ROOT) != window) {
        return false;
    }

    wchar_t className[64]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className)))) {
        const wchar_t c0 = className[0];
        if (c0 == L'S' || c0 == L'P' || c0 == L'W' || c0 == L'#' || c0 == L'L') {
            if (wcscmp(className, L"Shell_TrayWnd") == 0 ||
                wcscmp(className, L"Shell_SecondaryTrayWnd") == 0 ||
                wcscmp(className, L"Progman") == 0 ||
                wcscmp(className, L"WorkerW") == 0 ||
                wcscmp(className, L"#32768") == 0 ||
                wcscmp(className, L"LightencyGenieGhost") == 0 ||
                wcscmp(className, L"Windows.UI.Core.CoreWindow") == 0) {
                return false;
            }
        }
    }

    RECT rc{};
    if (!GetWindowRect(window, &rc)) {
        return false;
    }

    if ((rc.right - rc.left) < 120 || (rc.bottom - rc.top) < 80) {
        return false;
    }

    return true;
}

void SetWindowBorder(HWND window, bool remove) {
    const COLORREF color = remove ? kColorInvisible : kColorDefault;
    DwmSetWindowAttribute(window, kDwmBorderAttribute, &color, sizeof(color));
}

void ApplyToAllWindows(bool remove) {
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        const bool removeBorder = (lParam != 0);
        if (IsEligibleWindow(hwnd)) {
            SetWindowBorder(hwnd, removeBorder);
            std::lock_guard lock(g_windowsMutex);
            if (removeBorder) {
                g_processedWindows.insert(hwnd);
            } else {
                g_processedWindows.erase(hwnd);
            }
        }
        return TRUE;
    }, remove ? 1 : 0);
}

void CALLBACK OnWindowEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }

    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) {
        return;
    }

    if (event == EVENT_OBJECT_DESTROY) {
        std::lock_guard lock(g_windowsMutex);
        g_processedWindows.erase(hwnd);
        return;
    }

    {
        std::lock_guard lock(g_windowsMutex);
        if (g_processedWindows.contains(hwnd)) {
            return;
        }
    }

    if (IsEligibleWindow(hwnd)) {
        SetWindowBorder(hwnd, true);
        std::lock_guard lock(g_windowsMutex);
        g_processedWindows.insert(hwnd);
    }
}

}

void Update(const AppConfig& config) {
    const bool desired = config.hideWindowBorders;
    const bool current = g_enabled.load(std::memory_order_relaxed);

    if (desired == current) {
        return;
    }

    if (desired) {
        g_enabled.store(true, std::memory_order_release);
        g_hookForeground = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, OnWindowEvent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        g_hookShow = SetWinEventHook(
            EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
            nullptr, OnWindowEvent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        g_hookDestroy = SetWinEventHook(
            EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
            nullptr, OnWindowEvent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        ApplyToAllWindows(true);
    } else {
        g_enabled.store(false, std::memory_order_release);
        if (g_hookForeground) {
            UnhookWinEvent(g_hookForeground);
            g_hookForeground = nullptr;
        }
        if (g_hookShow) {
            UnhookWinEvent(g_hookShow);
            g_hookShow = nullptr;
        }
        if (g_hookDestroy) {
            UnhookWinEvent(g_hookDestroy);
            g_hookDestroy = nullptr;
        }
        std::unordered_set<HWND> toRestore;
        {
            std::lock_guard lock(g_windowsMutex);
            toRestore = std::move(g_processedWindows);
            g_processedWindows.clear();
        }
        for (HWND hwnd : toRestore) {
            if (IsWindow(hwnd)) {
                SetWindowBorder(hwnd, false);
            }
        }
        ApplyToAllWindows(false);
    }
}

void Refresh() {
    if (g_enabled.load(std::memory_order_relaxed)) {
        ApplyToAllWindows(true);
    }
}

void Shutdown() {
    AppConfig disabled;
    disabled.hideWindowBorders = false;
    Update(disabled);
}

}
