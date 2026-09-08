#include "composition.h"
namespace Lightency {
static pfnSetWindowCompositionAttribute GetSetWindowCompositionAttribute() {
    static auto s_pfn = reinterpret_cast<pfnSetWindowCompositionAttribute>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return s_pfn;
}
std::vector<HWND> GetAllTaskbars() {
    std::vector<HWND> windows;
    HWND hPrimary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hPrimary) windows.push_back(hPrimary);
    HWND hSecondary = nullptr;
    while ((hSecondary = FindWindowExW(nullptr, hSecondary, L"SecondaryTrayWnd", nullptr)) != nullptr) {
        windows.push_back(hSecondary);
    }
    return windows;
}
static void ApplyPolicy(HWND hWnd, int accentState) {
    if (!hWnd || !IsWindow(hWnd)) return;
    auto pfn = GetSetWindowCompositionAttribute();
    if (!pfn) return;
    ACCENT_POLICY policy = { accentState, 2, 0x00000000, 0 };
    WINCOMPATTRDATA data = { 19, &policy, sizeof(policy) };
    pfn(hWnd, &data);
    EnumChildWindows(hWnd, [](HWND hChild, LPARAM lParam) -> BOOL {
        auto pfnChild = GetSetWindowCompositionAttribute();
        if (pfnChild) {
            pfnChild(hChild, reinterpret_cast<WINCOMPATTRDATA*>(lParam));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));


    RedrawWindow(hWnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void ApplyClearToAllTaskbars() {
    auto windows = GetAllTaskbars();
    for (HWND hWnd : windows) {
        ApplyPolicy(hWnd, ACCENT_ENABLE_TRANSPARENTGRADIENT);
    }
}
void ApplyNormalToAllTaskbars() {
    auto windows = GetAllTaskbars();
    for (HWND hWnd : windows) {
        ApplyPolicy(hWnd, ACCENT_DISABLED);
    }
}
}
