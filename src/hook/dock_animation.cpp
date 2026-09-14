#include "dock_animation.h"
#include "xaml_bridge.h"
#include "start_button_style.h"
#include "drag_drop_assist.h"
#include <unknwn.h>
#include <objbase.h>
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <stdio.h>
#include <string>
#include <vector>
#include "../common/diagnostics.h"

static inline void LogDock(const std::wstring&) {}

static Lightency::SharedHookConfig g_lightencyDockConfig;
static HANDLE s_hConfigMap = NULL;
static Lightency::SharedHookConfig* s_pLiveSharedConfig = nullptr;
static void RestoreDefaultLayout();
static void DockEngine_ApplySettings();

static bool IsProcessAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;
    DWORD exitCode = 0;
    bool alive = (GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE);
    CloseHandle(hProc);
    return alive;
}

static void EnsureLiveConfigMapped() {
    if (!s_pLiveSharedConfig) {
        if (!s_hConfigMap) {
            s_hConfigMap = OpenFileMappingW(FILE_MAP_READ, FALSE, Lightency::SHARED_HOOK_CONFIG_MAPPING_NAME);
        }
        if (s_hConfigMap) {
            s_pLiveSharedConfig = (Lightency::SharedHookConfig*)MapViewOfFile(s_hConfigMap, FILE_MAP_READ, 0, 0, sizeof(Lightency::SharedHookConfig));
        }
    }
    if (s_pLiveSharedConfig) {
        bool shouldDisable = false;
        if (s_pLiveSharedConfig->masterPid != 0 && !IsProcessAlive(s_pLiveSharedConfig->masterPid)) {
            shouldDisable = true;
        }
        bool newDockAnim = shouldDisable ? false : s_pLiveSharedConfig->dockAnimation;
        bool newLayoutEditor = shouldDisable ? false : s_pLiveSharedConfig->layoutEditor;

        if (s_pLiveSharedConfig->maxScale != g_lightencyDockConfig.maxScale ||
            s_pLiveSharedConfig->effectRadius != g_lightencyDockConfig.effectRadius ||
            s_pLiveSharedConfig->spacingFactor != g_lightencyDockConfig.spacingFactor ||
            s_pLiveSharedConfig->disableBounce != g_lightencyDockConfig.disableBounce ||
            s_pLiveSharedConfig->excludeSystemButtons != g_lightencyDockConfig.excludeSystemButtons ||
            s_pLiveSharedConfig->autoPhysics != g_lightencyDockConfig.autoPhysics ||
            s_pLiveSharedConfig->trayItems != g_lightencyDockConfig.trayItems ||
            s_pLiveSharedConfig->hideTrayChevron != g_lightencyDockConfig.hideTrayChevron ||
            s_pLiveSharedConfig->hideTrayLanguage != g_lightencyDockConfig.hideTrayLanguage ||
            s_pLiveSharedConfig->hideTrayNetwork != g_lightencyDockConfig.hideTrayNetwork ||
            s_pLiveSharedConfig->hideTrayVolume != g_lightencyDockConfig.hideTrayVolume ||
            s_pLiveSharedConfig->hideTrayBattery != g_lightencyDockConfig.hideTrayBattery ||
            s_pLiveSharedConfig->hideTrayClock != g_lightencyDockConfig.hideTrayClock ||
            s_pLiveSharedConfig->clearTaskbar != g_lightencyDockConfig.clearTaskbar ||
            s_pLiveSharedConfig->hideTaskbarBorder != g_lightencyDockConfig.hideTaskbarBorder ||
            s_pLiveSharedConfig->dragDropAssist != g_lightencyDockConfig.dragDropAssist ||
            newLayoutEditor != g_lightencyDockConfig.layoutEditor ||
            newDockAnim != g_lightencyDockConfig.dockAnimation) {
            bool layoutToggledOff = (g_lightencyDockConfig.layoutEditor && !newLayoutEditor);
            g_lightencyDockConfig = Lightency::SharedHookConfig::Read(s_pLiveSharedConfig);
            g_lightencyDockConfig.dockAnimation = newDockAnim;
            g_lightencyDockConfig.layoutEditor = newLayoutEditor;
            if (layoutToggledOff) {
                RestoreDefaultLayout();
            }
            DockEngine_ApplySettings();
        }
    }
}

#include <winrt/Windows.Foundation.Collections.h>

#include <winrt/Windows.UI.Xaml.h>

#include <winrt/Windows.UI.Xaml.Controls.h>

#include <winrt/Windows.UI.Xaml.Input.h>

#include <winrt/Windows.UI.Xaml.Media.h>

#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <winrt/Windows.UI.Input.h>

#include <vector>

#include <atomic>

#include <functional>

#include <cmath>

#include <map>
#include <set>

#include <algorithm>

#include <chrono>

#include <unordered_map>

#include <unordered_set>

#include <cstdint>
#include <cwctype>
#include <initializer_list>

#include <winrt/Windows.UI.Xaml.Automation.h>


using namespace winrt::Windows::UI::Xaml;

using namespace winrt::Windows::UI::Xaml::Media;

using namespace winrt::Windows::UI::Xaml::Automation;

struct TrayVisibilityState {
    winrt::weak_ref<FrameworkElement> element;
    bool currentlyHiddenByUs = false;
};

static std::unordered_map<void*, TrayVisibilityState> g_trayVisibilityStates;

struct TaskbarAppearanceState {
    winrt::weak_ref<FrameworkElement> element;
    Brush originalFill{nullptr};
    Brush originalStroke{nullptr};
    double originalStrokeThickness = 0.0;
    double originalOpacity = 1.0;
    Visibility originalVisibility = Visibility::Visible;
    Thickness originalBorderThickness{0, 0, 0, 0};
    Brush originalBorderBrush{nullptr};
    bool isBorder = false;
};

static std::unordered_map<void*, TaskbarAppearanceState> g_taskbarAppearanceStates;

static std::wstring TrayToLower(std::wstring value);
static bool ContainsAny(const std::wstring& value,
                        std::initializer_list<const wchar_t*> needles);

static void RestoreTaskbarAppearance() {
    for (auto& [key, state] : g_taskbarAppearanceStates) {
        if (auto el = state.element.get()) {
            try {
                if (auto shape = el.try_as<winrt::Windows::UI::Xaml::Shapes::Shape>()) {
                    shape.Fill(state.originalFill);
                    shape.Stroke(state.originalStroke);
                    shape.StrokeThickness(state.originalStrokeThickness);
                }
                if (auto b = el.try_as<winrt::Windows::UI::Xaml::Controls::Border>()) {
                    b.BorderThickness(state.originalBorderThickness);
                    b.BorderBrush(state.originalBorderBrush);
                }
                el.Opacity(state.originalOpacity);
                el.Visibility(state.originalVisibility);
            } catch (const winrt::hresult_error&) {}
        }
    }
    g_taskbarAppearanceStates.clear();
}

static void RemoveTaskbarHwndBorder() {
    typedef HRESULT (WINAPI *DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
    static auto pfnDwmSetWindowAttribute = (DwmSetWindowAttribute_t)GetProcAddress(
        GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute");
    if (!pfnDwmSetWindowAttribute) {
        HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
        if (hDwm) pfnDwmSetWindowAttribute = (DwmSetWindowAttribute_t)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    }
    if (!pfnDwmSetWindowAttribute) return;

    auto clearBorder = [](HWND hWnd, DwmSetWindowAttribute_t pfn) {
        if (!hWnd) return;
        COLORREF none = 0xFFFFFFFE;
        pfn(hWnd, 34, &none, sizeof(none));
    };

    clearBorder(FindWindowW(L"Shell_TrayWnd", nullptr), pfnDwmSetWindowAttribute);
    HWND hSec = nullptr;
    while ((hSec = FindWindowExW(nullptr, hSec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        clearBorder(hSec, pfnDwmSetWindowAttribute);
    }
}

static void ApplyTaskbarAppearance(FrameworkElement const& taskbarElement) {
    if (!taskbarElement) return;
    try {
        RemoveTaskbarHwndBorder();

        FrameworkElement root = taskbarElement;
        while (auto parent = VisualTreeHelper::GetParent(root).try_as<FrameworkElement>()) {
            root = parent;
        }

        const bool clearTaskbar = g_lightencyDockConfig.clearTaskbar;
        const bool hideBorder = g_lightencyDockConfig.hideTaskbarBorder || clearTaskbar;

        std::vector<FrameworkElement> stack{root};
        while (!stack.empty()) {
            FrameworkElement element = stack.back();
            stack.pop_back();

            const std::wstring name = element.Name().c_str();
            bool isStroke = (name == L"BackgroundStroke" || name == L"TaskbarStroke" ||
                             name == L"TopBorder" || name == L"BackgroundBorder" ||
                             (!name.empty() && name.find(L"Stroke") != std::wstring::npos));
            const bool isFill = (name == L"BackgroundFill" || 
                                 (!isStroke && !name.empty() && name.find(L"Background") != std::wstring::npos));


            if (!isStroke && !isFill) {
                if (auto s = element.try_as<winrt::Windows::UI::Xaml::Shapes::Shape>()) {
                    double h = s.Height();
                    double ah = s.ActualHeight();
                    if ((h > 0.0 && h <= 2.5) || (ah > 0.0 && ah <= 2.5)) {
                        isStroke = true;
                    }
                }
            }

            if (isFill) {
                if (auto shape = element.try_as<winrt::Windows::UI::Xaml::Shapes::Shape>()) {
                    void* key = winrt::get_abi(shape);
                    auto [it, inserted] = g_taskbarAppearanceStates.try_emplace(
                        key, TaskbarAppearanceState{element, shape.Fill(), shape.Stroke(), shape.StrokeThickness(),
                                                   element.Opacity(), element.Visibility(), {}, nullptr, false});
                    if (clearTaskbar) {
                        shape.Fill(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                        shape.Stroke(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                        shape.StrokeThickness(0.0);
                    } else {
                        shape.Fill(it->second.originalFill);
                        shape.Stroke(it->second.originalStroke);
                        shape.StrokeThickness(it->second.originalStrokeThickness);
                    }
                } else if (auto border = element.try_as<winrt::Windows::UI::Xaml::Controls::Border>()) {
                    void* key = winrt::get_abi(border);
                    auto [it, inserted] = g_taskbarAppearanceStates.try_emplace(
                        key, TaskbarAppearanceState{element, nullptr, nullptr, 0.0,
                                                   element.Opacity(), element.Visibility(), border.BorderThickness(), border.Background(), false});
                    if (clearTaskbar) {
                        border.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                    } else {
                        border.Background(it->second.originalBorderBrush);
                    }
                }
            } else if (isStroke) {
                void* key = winrt::get_abi(element);
                auto shape = element.try_as<winrt::Windows::UI::Xaml::Shapes::Shape>();
                auto borderCtrl = element.try_as<winrt::Windows::UI::Xaml::Controls::Border>();
                Brush fill = shape ? shape.Fill() : nullptr;
                Brush stroke = shape ? shape.Stroke() : nullptr;
                double strokeThick = shape ? shape.StrokeThickness() : 0.0;
                Thickness bThick = borderCtrl ? borderCtrl.BorderThickness() : Thickness{0,0,0,0};
                Brush bBrush = borderCtrl ? borderCtrl.BorderBrush() : nullptr;

                auto [it, inserted] = g_taskbarAppearanceStates.try_emplace(
                    key, TaskbarAppearanceState{element, fill, stroke, strokeThick,
                                               element.Opacity(), element.Visibility(), bThick, bBrush, true});

                if (hideBorder) {
                    element.Visibility(Visibility::Collapsed);
                    element.Opacity(0.0);
                    if (shape) {
                        shape.Fill(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                        shape.Stroke(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                        shape.StrokeThickness(0.0);
                    }
                    if (borderCtrl) {
                        borderCtrl.BorderThickness({0, 0, 0, 0});
                        borderCtrl.BorderBrush(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                    }
                } else {
                    element.Visibility(it->second.originalVisibility);
                    element.Opacity(it->second.originalOpacity);
                    if (shape) {
                        shape.Fill(it->second.originalFill);
                        shape.Stroke(it->second.originalStroke);
                        shape.StrokeThickness(it->second.originalStrokeThickness);
                    }
                    if (borderCtrl) {
                        borderCtrl.BorderThickness(it->second.originalBorderThickness);
                        borderCtrl.BorderBrush(it->second.originalBorderBrush);
                    }
                }
            }

            const int childCount = VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < childCount; ++i) {
                if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                    stack.push_back(child);
                }
            }
        }
    } catch (const winrt::hresult_error&) {}
}

static void RestoreTrayVisibility() {
    for (auto& [key, state] : g_trayVisibilityStates) {
        if (auto element = state.element.get()) {
            try {
                if (state.currentlyHiddenByUs) {
                    if (auto dep = element.try_as<DependencyObject>()) {
                        dep.ClearValue(winrt::Windows::UI::Xaml::UIElement::VisibilityProperty());
                    }
                }
            } catch (const winrt::hresult_error&) {}
        }
    }
    g_trayVisibilityStates.clear();
}

struct LayoutElementState {
    winrt::weak_ref<FrameworkElement> element;
    std::wstring id;
    Thickness originalMargin{};
    double offsetX = 0;
    double offsetY = 0;
};

static std::unordered_map<void*, LayoutElementState> g_layoutElements;
static FrameworkElement g_layoutDragElement = nullptr;
static winrt::Windows::Foundation::Point g_layoutDragStart{};
static double g_layoutDragStartX = 0;
static double g_layoutDragStartY = 0;
static winrt::Windows::Foundation::Point g_layoutDragOrigin{};
static std::atomic<LONG> g_startButtonScreenCenter{-1};
static HWINEVENTHOOK g_startMenuWinEventHook = nullptr;

static bool IsStartMenuWindow(HWND hwnd) {
    if (!hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD pathLength = ARRAYSIZE(path);
    bool result = QueryFullProcessImageNameW(process, 0, path, &pathLength) &&
        wcsstr(TrayToLower(path).c_str(), L"startmenuexperiencehost.exe");
    CloseHandle(process);
    return result;
}

static void CALLBACK StartMenuWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                           LONG objectId, LONG, DWORD, DWORD) {
    if ((event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_LOCATIONCHANGE) ||
        objectId != OBJID_WINDOW || !IsWindowVisible(hwnd) || !IsStartMenuWindow(hwnd)) {
        return;
    }
    LONG anchor = g_startButtonScreenCenter.load();
    if (anchor < 0) return;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return;
    int width = rect.right - rect.left;
    POINT anchorPoint{anchor, rect.bottom};
    HMONITOR monitor = MonitorFromPoint(anchorPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;
    int targetX = anchor - width / 2;
    targetX = std::clamp(targetX, static_cast<int>(monitorInfo.rcWork.left),
                         static_cast<int>(monitorInfo.rcWork.right) - width);
    if (std::abs(targetX - rect.left) > 1) {
        SetWindowPos(hwnd, nullptr, targetX, rect.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static std::wstring LayoutConfigPath() {
    wchar_t localAppData[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
    if (len > 0 && len < ARRAYSIZE(localAppData)) {
        std::wstring dir = std::wstring(localAppData) + L"\\Lightency";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\layout.ini";
    }
    return L"layout.ini";
}

static std::wstring GetLayoutId(FrameworkElement const& element) {
    std::wstring identity = TrayToLower(std::wstring(winrt::get_class_name(element).c_str()) + L"|" +
        std::wstring(element.Name().c_str()) + L"|" +
        std::wstring(AutomationProperties::GetAutomationId(element).c_str()));

    std::wstring automationId = TrayToLower(AutomationProperties::GetAutomationId(element).c_str());
    std::wstring className = TrayToLower(winrt::get_class_name(element).c_str());
    if (automationId == L"startbutton" && ContainsAny(className, {L"button"})) return L"start";
    return L"";
}

static double ReadLayoutValue(const std::wstring& id, const wchar_t* axis) {
    wchar_t value[64] = {};
    std::wstring key = id + L"." + axis;
    GetPrivateProfileStringW(L"Layout", key.c_str(), L"0", value, ARRAYSIZE(value),
                             LayoutConfigPath().c_str());
    wchar_t* end = nullptr;
    double result = wcstod(value, &end);
    return (end && end != value && std::isfinite(result)) ? result : 0.0;
}

static void SaveLayoutValue(const std::wstring& id, double x, double y) {
    std::wstring path = LayoutConfigPath();
    std::wstring xKey = id + L".x";
    std::wstring yKey = id + L".y";
    wchar_t xValue[40] = {}, yValue[40] = {};
    swprintf_s(xValue, L"%.2f", x);
    swprintf_s(yValue, L"%.2f", y);
    WritePrivateProfileStringW(L"Layout", xKey.c_str(), xValue, path.c_str());
    WritePrivateProfileStringW(L"Layout", yKey.c_str(), yValue, path.c_str());
}

static void SetLayoutOffset(LayoutElementState& state, double x, double y) {
    state.offsetX = x;
    state.offsetY = y;


    state.element.get().Margin({
        state.originalMargin.Left + x,
        state.originalMargin.Top + y,
        state.originalMargin.Right - x,
        state.originalMargin.Bottom - y,
    });
}

static void RestoreDefaultLayout() {
    for (auto& [key, state] : g_layoutElements) {
        if (auto element = state.element.get()) {
            try {
                element.ReleasePointerCaptures();
                element.Margin(state.originalMargin);
            } catch (const winrt::hresult_error&) {}
        }
    }
    g_layoutElements.clear();
    g_layoutDragElement = nullptr;
    g_startButtonScreenCenter.store(-1);
}

static LayoutElementState* EnsureLayoutElement(FrameworkElement const& element,
                                               const std::wstring& id) {
    void* key = winrt::get_abi(element);
    auto found = g_layoutElements.find(key);
    if (found != g_layoutElements.end() && found->second.element.get()) {
        return &found->second;
    }
    LayoutElementState state{element, id, element.Margin(), 0, 0};
    auto [it, inserted] = g_layoutElements.emplace(key, std::move(state));
    SetLayoutOffset(it->second, ReadLayoutValue(id, L"x"), ReadLayoutValue(id, L"y"));
    return &it->second;
}

static void ApplySavedLayout(FrameworkElement const& root) {
    if (!root || !g_lightencyDockConfig.layoutEditor) return;
    std::unordered_set<std::wstring> seenIds;
    std::vector<FrameworkElement> stack{root};
    while (!stack.empty()) {
        FrameworkElement element = stack.back();
        stack.pop_back();
        std::wstring id = GetLayoutId(element);
        if (!id.empty() && seenIds.insert(id).second) EnsureLayoutElement(element, id);
        int count = VisualTreeHelper::GetChildrenCount(element);
        for (int i = 0; i < count; ++i) {
            if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                stack.push_back(child);
            }
        }
    }


    for (auto& [key, state] : g_layoutElements) {
        auto element = state.element.get();
        if (!element || element.ActualWidth() <= 0 || element.ActualHeight() <= 0) continue;
        try {
            auto origin = element.TransformToVisual(root).TransformPoint({0, 0});
            if (state.id == L"start") {
                HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
                RECT taskbarRect{};
                double rasterScale = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1.0;
                if (taskbar && GetWindowRect(taskbar, &taskbarRect)) {
                    g_startButtonScreenCenter.store(taskbarRect.left + static_cast<LONG>(
                        (origin.X + element.ActualWidth() / 2.0) * rasterScale));
                }
            }
            double correctionX = 0;
            double correctionY = 0;
            if (origin.X < 0) correctionX = -origin.X;
            else if (origin.X + element.ActualWidth() > root.ActualWidth())
                correctionX = root.ActualWidth() - origin.X - element.ActualWidth();
            if (origin.Y < 0) correctionY = -origin.Y;
            else if (origin.Y + element.ActualHeight() > root.ActualHeight())
                correctionY = root.ActualHeight() - origin.Y - element.ActualHeight();
            if (std::abs(correctionX) > 0.25 || std::abs(correctionY) > 0.25) {
                SetLayoutOffset(state, state.offsetX + correctionX, state.offsetY + correctionY);
                SaveLayoutValue(state.id, state.offsetX, state.offsetY);
            }
        } catch (const winrt::hresult_error&) {

        }
    }
}

static FrameworkElement FindLayoutElementAt(FrameworkElement const& root,
                                            winrt::Windows::Foundation::Point point) {
    FrameworkElement best = nullptr;
    double bestArea = DBL_MAX;
    std::vector<FrameworkElement> stack{root};
    while (!stack.empty()) {
        FrameworkElement element = stack.back();
        stack.pop_back();
        std::wstring id = GetLayoutId(element);
        void* elementKey = winrt::get_abi(element);
        auto registered = g_layoutElements.find(elementKey);
        if (!id.empty() && registered != g_layoutElements.end() &&
            registered->second.element.get() &&
            element.ActualWidth() > 1 && element.ActualHeight() > 1) {
            try {
                auto origin = element.TransformToVisual(root).TransformPoint({0, 0});
                if (point.X >= origin.X && point.X <= origin.X + element.ActualWidth() &&
                    point.Y >= origin.Y && point.Y <= origin.Y + element.ActualHeight()) {
                    double area = element.ActualWidth() * element.ActualHeight();
                    if (area < bestArea) { best = element; bestArea = area; }
                }
            } catch (const winrt::hresult_error&) {}
        }
        int count = VisualTreeHelper::GetChildrenCount(element);
        for (int i = 0; i < count; ++i) {
            if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                stack.push_back(child);
            }
        }
    }
    return best;
}

static bool HandleLayoutPointerMove(FrameworkElement const& root,
                                    Input::PointerRoutedEventArgs const& args) {
    if (!root || !args) return false;
    FrameworkElement layoutRoot = root;
    while (auto parent = VisualTreeHelper::GetParent(layoutRoot).try_as<FrameworkElement>()) {
        layoutRoot = parent;
    }
    ApplySavedLayout(layoutRoot);

    bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!g_lightencyDockConfig.layoutEditor || !leftDown) {
        g_layoutDragElement = nullptr;
        return false;
    }
    auto point = args.GetCurrentPoint(layoutRoot).Position();
    if (!g_layoutDragElement) {
        if (!ctrlDown) return false;
        auto target = FindLayoutElementAt(layoutRoot, point);
        if (!target) return false;
        std::wstring id = GetLayoutId(target);
        if (id.empty()) return false;
        g_layoutDragElement = target;
        auto* state = EnsureLayoutElement(target, id);
        if (!state) return false;
        g_layoutDragStart = point;
        g_layoutDragStartX = state->offsetX;
        g_layoutDragStartY = state->offsetY;
        g_layoutDragOrigin = target.TransformToVisual(layoutRoot).TransformPoint({0, 0});
    }

    double deltaX = point.X - g_layoutDragStart.X;
    double deltaY = point.Y - g_layoutDragStart.Y;
    deltaX = std::clamp(deltaX, -static_cast<double>(g_layoutDragOrigin.X),
        layoutRoot.ActualWidth() - g_layoutDragElement.ActualWidth() - g_layoutDragOrigin.X);
    deltaY = std::clamp(deltaY, -static_cast<double>(g_layoutDragOrigin.Y),
        layoutRoot.ActualHeight() - g_layoutDragElement.ActualHeight() - g_layoutDragOrigin.Y);
    double x = g_layoutDragStartX + deltaX;
    double y = g_layoutDragStartY + deltaY;
    void* key = winrt::get_abi(g_layoutDragElement);
    auto it = g_layoutElements.find(key);
    if (it == g_layoutElements.end()) return false;
    SetLayoutOffset(it->second, x, y);
    SaveLayoutValue(it->second.id, x, y);
    args.Handled(true);
    return true;
}

static std::wstring TrayToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

static bool ContainsAny(const std::wstring& value,
                        std::initializer_list<const wchar_t*> needles) {
    for (const wchar_t* needle : needles) {
        if (value.find(needle) != std::wstring::npos) return true;
    }
    return false;
}

enum class TrayItemKind {
    None,
    Chevron,
    Language,
    Network,
    Volume,
    Battery,
    Clock,
};

static bool GlyphInRanges(wchar_t glyph,
                          std::initializer_list<std::pair<wchar_t, wchar_t>> ranges) {
    for (const auto& range : ranges) {
        if (glyph >= range.first && glyph <= range.second) return true;
    }
    return false;
}

static TrayItemKind IdentifyTrayIcon(FrameworkElement const& icon) {
    std::vector<FrameworkElement> stack{ icon };
    while (!stack.empty()) {
        FrameworkElement element = stack.back();
        stack.pop_back();

        std::wstring className = TrayToLower(winrt::get_class_name(element).c_str());
        std::wstring name = TrayToLower(element.Name().c_str());

        if (ContainsAny(className, {L"language", L"inputindicator", L"systemtray.inputindicatorview"}) ||
            ContainsAny(name, {L"language", L"inputindicator"})) {
            return TrayItemKind::Language;
        }
        if (ContainsAny(className, {L"batteryiconcontent"})) {
            return TrayItemKind::Battery;
        }

        if (auto textBlock = element.try_as<Controls::TextBlock>()) {
            std::wstring text = textBlock.Text().c_str();
            if (!text.empty()) {
                if (text.length() >= 2 && text.length() <= 4 &&
                    std::all_of(text.begin(), text.end(), iswupper)) {
                    return TrayItemKind::Language;
                }

                wchar_t glyph = text.front();

                if (GlyphInRanges(glyph, {{0xE992, 0xE995}, {0xEA85, 0xEA85},
                                           {0xEBC5, 0xEBC5}, {0xE74F, 0xE74F}})) {
                    return TrayItemKind::Volume;
                }
                if (GlyphInRanges(glyph, {{0xE839, 0xE839}, {0xE86C, 0xE870},
                                           {0xEC3C, 0xEC3F}, {0xF8C0, 0xF8CC},
                                           {0xE709, 0xE709}, {0xE7F4, 0xE7F4}})) {
                    return TrayItemKind::Network;
                }
                if (GlyphInRanges(glyph, {{0xEBA0, 0xEBC0}, {0xE3C1, 0xE3CB},
                                           {0xE408, 0xE41D}, {0xEB17, 0xEB17},
                                           {0xEC02, 0xEC02}, {0xF1E8, 0xF1E8}})) {
                    return TrayItemKind::Battery;
                }
                if (glyph == 0xE70D || glyph == 0xE70E) {
                    return TrayItemKind::Chevron;
                }
            }
        }

        int childCount = VisualTreeHelper::GetChildrenCount(element);
        for (int i = 0; i < childCount; ++i) {
            if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                stack.push_back(child);
            }
        }
    }
    return TrayItemKind::None;
}

static void ApplyTrayItemVisibility(FrameworkElement const& taskbarElement) {
    if (!taskbarElement) return;

    try {
        FrameworkElement root = taskbarElement;
        while (auto parent = VisualTreeHelper::GetParent(root).try_as<FrameworkElement>()) {
            root = parent;
        }

        if (!g_lightencyDockConfig.trayItems) {
            RestoreTrayVisibility();
            return;
        }

        std::vector<FrameworkElement> stack{ root };
        while (!stack.empty()) {
            FrameworkElement element = stack.back();
            stack.pop_back();

            std::wstring identity = TrayToLower(std::wstring(winrt::get_class_name(element).c_str()) + L"|" +
                std::wstring(element.Name().c_str()) + L"|" +
                std::wstring(AutomationProperties::GetAutomationId(element).c_str()));

            bool hide = false;
            bool recognized = false;
            if (ContainsAny(identity, {L"notificationcenterbutton"})) {
                recognized = true;
                hide = g_lightencyDockConfig.hideTrayClock;
            } else if (ContainsAny(identity, {L"chevronicon", L"overflowbutton", L"notifyiconoverflow"})) {
                recognized = true;
                hide = g_lightencyDockConfig.hideTrayChevron;
            } else if (ContainsAny(identity, {L"language", L"inputindicator", L"keyboardlayout"})) {
                recognized = true;
                hide = g_lightencyDockConfig.hideTrayLanguage;
            } else if (ContainsAny(identity, {L"networkicon", L"networkitem"})) {
                recognized = true;
                hide = g_lightencyDockConfig.hideTrayNetwork;
            } else if (ContainsAny(identity, {L"volumeicon", L"volumeitem"})) {
                recognized = true;
                hide = g_lightencyDockConfig.hideTrayVolume;
            } else if (ContainsAny(identity, {L"batteryicon", L"powericon", L"batteryitem"})) {
                recognized = true;
                hide = g_lightencyDockConfig.hideTrayBattery;
            } else if (ContainsAny(identity, {L"clockbutton", L"clocksystemtray", L"dateandtime"})) {
                recognized = true;
                hide = g_lightencyDockConfig.hideTrayClock;
            } else if (ContainsAny(identity, {L"systemtray.iconview|systemtrayicon"})) {
                switch (IdentifyTrayIcon(element)) {
                    case TrayItemKind::Chevron:
                        recognized = true; hide = g_lightencyDockConfig.hideTrayChevron; break;
                    case TrayItemKind::Language:
                        recognized = true; hide = g_lightencyDockConfig.hideTrayLanguage; break;
                    case TrayItemKind::Network:
                        recognized = true; hide = g_lightencyDockConfig.hideTrayNetwork; break;
                    case TrayItemKind::Volume:
                        recognized = true; hide = g_lightencyDockConfig.hideTrayVolume; break;
                    case TrayItemKind::Battery:
                        recognized = true; hide = g_lightencyDockConfig.hideTrayBattery; break;
                    default:
                        break;
                }
            }

            if (recognized) {
                void* key = winrt::get_abi(element);
                auto [it, inserted] = g_trayVisibilityStates.try_emplace(
                    key, TrayVisibilityState{ element, false });
                
                if (hide) {
                    if (!it->second.currentlyHiddenByUs || element.Visibility() != Visibility::Collapsed) {
                        element.Visibility(Visibility::Collapsed);
                        it->second.currentlyHiddenByUs = true;
                    }
                } else {
                    if (it->second.currentlyHiddenByUs) {
                        if (auto dep = element.try_as<DependencyObject>()) {
                            dep.ClearValue(winrt::Windows::UI::Xaml::UIElement::VisibilityProperty());
                        }
                        it->second.currentlyHiddenByUs = false;
                    }
                }
            }

            int childCount = VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < childCount; ++i) {
                if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                    stack.push_back(child);
                }
            }
        }
    } catch (const winrt::hresult_error&) {}
}

static void HideButtonBackgroundPlate(FrameworkElement const& root) {
    if (!root) return;
    try {
        auto btn = root.try_as<Controls::Button>();
        if (btn) {
            btn.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
            btn.BorderBrush(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
            btn.BorderThickness({ 0, 0, 0, 0 });
        }
        int count = VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < count; i++) {
            auto child = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();
            if (!child) continue;
            auto cn = winrt::get_class_name(child);
            if (cn == L"Windows.UI.Xaml.Controls.Border") {
                auto b = child.try_as<Controls::Border>();
                if (b) {
                    double h = b.ActualHeight();
                    if (h > 10.0 || b.Height() > 10.0 || b.Name() == L"BackgroundBorder" || b.Name() == L"HoverBorder") {
                        b.Opacity(0.0);
                        b.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                        b.BorderBrush(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
                    }
                }
            }
            HideButtonBackgroundPlate(child);
        }
    } catch (const winrt::hresult_error&) {}
}

namespace Lightency {

enum class TaskbarOrientation {
    Bottom,
    Top,
    Left,
    Right
};

struct DockTransformState {
    double scale = 1.0;
    double offset = 0.0;
};

struct DockItem {
    winrt::weak_ref<FrameworkElement> element;
    double anchorPosition = 0.0;
    double dimension = 0.0;
    DockTransformState state;
};

struct DockHierarchyFingerprint {
    int count = -1;
    double span = -1.0;
    uintptr_t hash = 0;
};

struct DockSession {
    bool initialized = false;
    winrt::weak_ref<FrameworkElement> taskbarFrame;
    winrt::weak_ref<FrameworkElement> itemContainer;
    std::vector<DockItem> items;
    DockHierarchyFingerprint fingerprint;
    std::chrono::steady_clock::time_point lastCheckTime{};
    TaskbarOrientation orientation = TaskbarOrientation::Bottom;
    bool isVertical = false;
    HWND windowHandle = nullptr;
    RECT windowBounds = {0, 0, 0, 0};
    double smoothedCursor = 0.0;
    bool cursorInitialized = false;
};

struct XamlPointerSubscription {
    winrt::weak_ref<FrameworkElement> element;
    Input::PointerEventHandler moved{nullptr};
    Input::PointerEventHandler exited{nullptr};
    winrt::Windows::Foundation::EventHandler<winrt::Windows::Foundation::IInspectable> layoutUpdated{nullptr};
    winrt::event_token movedToken{};
    winrt::event_token exitedToken{};
    winrt::event_token layoutUpdatedToken{};
};

}

static winrt::event_token g_renderEventToken;
static std::atomic<bool> g_isRenderLoopActive = false;
static std::atomic<void*> g_currentActiveSessionKey = nullptr;
static std::atomic<bool> g_isCursorPresent = false;
static std::atomic<double> g_fadeIntensity = 0.0;
static std::atomic<double> g_lastInputPosition = -1.0;
static std::atomic<bool> g_bounceActive = false;
static std::atomic<double> g_bounceWeight = 0.0;
static std::chrono::steady_clock::time_point g_bounceStartTimestamp;
static std::chrono::steady_clock::time_point g_lastMotionTimestamp;
static std::chrono::steady_clock::time_point g_previousFrameTimestamp;

static std::map<void*, Lightency::DockSession> g_dockSessions;
static std::map<void*, Lightency::XamlPointerSubscription> g_pointerSubscriptions;

typedef void (*TaskbarWindowProc_t)(PVOID);

static bool DispatchToTaskbarThread(HWND hWnd, TaskbarWindowProc_t proc, PVOID param) {
    static const UINT dispatchMessage = RegisterWindowMessageW(L"Lightency_TaskbarWindowDispatchMsg");
    struct DispatchPayload {
        TaskbarWindowProc_t proc;
        PVOID param;
    };

    DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (threadId == 0) return false;
    if (threadId == GetCurrentThreadId()) {
        proc(param);
        return true;
    }

    HHOOK hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (code == HC_ACTION) {
                const CWPSTRUCT* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (cwp->message == dispatchMessage) {
                    DispatchPayload* payload = reinterpret_cast<DispatchPayload*>(cwp->lParam);
                    payload->proc(payload->param);
                }
            }
            return CallNextHookEx(nullptr, code, wParam, lParam);
        },
        nullptr, threadId);

    if (!hook) return false;

    DispatchPayload payload{proc, param};
    SendMessageW(hWnd, dispatchMessage, 0, reinterpret_cast<LPARAM>(&payload));
    UnhookWindowsHookEx(hook);
    return true;
}

static HWND ResolveTaskbarWindow() {
    DWORD currentThread = GetCurrentThreadId();
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (primary && GetWindowThreadProcessId(primary, nullptr) == currentThread) {
        return primary;
    }
    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        if (GetWindowThreadProcessId(secondary, nullptr) == currentThread) {
            return secondary;
        }
    }
    POINT pt;
    if (GetCursorPos(&pt)) {
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (mon && primary && MonitorFromWindow(primary, MONITOR_DEFAULTTONULL) == mon) {
            return primary;
        }
        secondary = nullptr;
        while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
            if (MonitorFromWindow(secondary, MONITOR_DEFAULTTONULL) == mon) {
                return secondary;
            }
        }
    }
    return primary;
}

static void UpdateSessionOrientation(Lightency::DockSession& session) {
    auto frame = session.taskbarFrame.get();
    if (!frame) return;

    HWND hwnd = session.windowHandle;
    if (!hwnd || !IsWindow(hwnd)) {
        hwnd = ResolveTaskbarWindow();
        session.windowHandle = hwnd;
    }

    if (!hwnd) {
        if (frame.ActualWidth() < frame.ActualHeight()) {
            session.orientation = Lightency::TaskbarOrientation::Left;
        } else {
            session.orientation = Lightency::TaskbarOrientation::Bottom;
        }
        session.isVertical = (session.orientation == Lightency::TaskbarOrientation::Left ||
                              session.orientation == Lightency::TaskbarOrientation::Right);
        return;
    }

    RECT rc = {0};
    if (GetWindowRect(hwnd, &rc)) {
        if (rc.left == session.windowBounds.left &&
            rc.top == session.windowBounds.top &&
            rc.right == session.windowBounds.right &&
            rc.bottom == session.windowBounds.bottom) {
            return;
        }
        session.windowBounds = rc;
        LONG width = rc.right - rc.left;
        LONG height = rc.bottom - rc.top;
        if (width < height) {
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi)) {
                if (rc.left <= mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left) / 2) {
                    session.orientation = Lightency::TaskbarOrientation::Left;
                } else {
                    session.orientation = Lightency::TaskbarOrientation::Right;
                }
            } else {
                session.orientation = Lightency::TaskbarOrientation::Left;
            }
        } else {
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi)) {
                if (rc.top <= mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top) / 2) {
                    session.orientation = Lightency::TaskbarOrientation::Top;
                } else {
                    session.orientation = Lightency::TaskbarOrientation::Bottom;
                }
            } else {
                session.orientation = Lightency::TaskbarOrientation::Bottom;
            }
        }
        session.isVertical = (session.orientation == Lightency::TaskbarOrientation::Left ||
                              session.orientation == Lightency::TaskbarOrientation::Right);
    }
}

static void PrepareHighResolutionIconSources(FrameworkElement const& root, double maxScale) {
    if (!root) return;
    try {
        std::vector<FrameworkElement> stack{ root };
        while (!stack.empty()) {
            FrameworkElement element = stack.back();
            stack.pop_back();

            if (auto image = element.try_as<Controls::Image>()) {
                double width = image.ActualWidth();
                double height = image.ActualHeight();

                if (width >= 16.0 && height >= 16.0 && width <= 64.0 && height <= 64.0) {
                    double rasterScale = 1.0;
                    if (auto xamlRoot = image.XamlRoot()) {
                        rasterScale = std::max(1.0, xamlRoot.RasterizationScale());
                    }
                    int decodePixels = static_cast<int>(std::ceil(
                        std::max(width, height) * maxScale * rasterScale * 1.35));
                    decodePixels = std::clamp(decodePixels, 64, 256);

                    if (auto bitmap = image.Source().try_as<Media::Imaging::BitmapImage>()) {
                        bitmap.DecodePixelType(Media::Imaging::DecodePixelType::Physical);
                        if (bitmap.DecodePixelWidth() < decodePixels ||
                            bitmap.DecodePixelHeight() < decodePixels) {
                            bitmap.DecodePixelWidth(decodePixels);
                            bitmap.DecodePixelHeight(decodePixels);
                        }
                    }
                }
            }

            int childCount = VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < childCount; ++i) {
                if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                    stack.push_back(child);
                }
            }
        }
    } catch (const winrt::hresult_error&) {}
}

enum class DockElementCategory {
    Application,
    Start,
    Search,
    TaskView,
    Widgets,
    SystemOther
};

static std::wstring StringToLower(std::wstring str) {
    for (auto& c : str) c = static_cast<wchar_t>(towlower(c));
    return str;
}

static DockElementCategory ClassifyTaskbarElement(FrameworkElement const& el) {
    if (!el) return DockElementCategory::SystemOther;
    std::wstring type = StringToLower(winrt::get_class_name(el).c_str());
    std::wstring name = StringToLower(el.Name().c_str());
    std::wstring autoId = StringToLower(AutomationProperties::GetAutomationId(el).c_str());
    std::wstring autoName = StringToLower(AutomationProperties::GetName(el).c_str());

    std::wstring token = type + L"|" + name + L"|" + autoId + L"|" + autoName;
    if (token.find(L"startbutton") != std::wstring::npos || token.find(L"start") != std::wstring::npos) return DockElementCategory::Start;
    if (token.find(L"searchbutton") != std::wstring::npos || token.find(L"search") != std::wstring::npos) return DockElementCategory::Search;
    if (token.find(L"taskview") != std::wstring::npos || token.find(L"task view") != std::wstring::npos) return DockElementCategory::TaskView;
    if (token.find(L"widget") != std::wstring::npos || token.find(L"weather") != std::wstring::npos) return DockElementCategory::Widgets;
    if (token.find(L"appid:") != std::wstring::npos || type == L"taskbar.tasklistbutton") return DockElementCategory::Application;
    if (type.size() >= 13 && type.rfind(L".tasklistbutton") == type.size() - 13) return DockElementCategory::Application;
    return DockElementCategory::SystemOther;
}

static bool IsElementEligibleForAnimation(FrameworkElement const& el) {
    if (!g_lightencyDockConfig.excludeSystemButtons) return true;
    DockElementCategory cat = ClassifyTaskbarElement(el);
    return cat == DockElementCategory::Application;
}

static void UpdateDragDropTargets(void* key, FrameworkElement const& taskbarFrame, HWND hWnd) {
    if (!g_lightencyDockConfig.dragDropAssist || !taskbarFrame) return;
    HWND targetHwnd = hWnd ? hWnd : FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!targetHwnd || !IsWindow(targetHwnd)) return;

    RECT taskbarRect{};
    if (!GetWindowRect(targetHwnd, &taskbarRect)) return;

    double rasterScale = taskbarFrame.XamlRoot() ? taskbarFrame.XamlRoot().RasterizationScale() : 1.0;
    std::vector<FrameworkElement> buttons;
    std::vector<FrameworkElement> stack{ taskbarFrame };
    while (!stack.empty()) {
        auto cur = stack.back();
        stack.pop_back();
        if (ClassifyTaskbarElement(cur) == DockElementCategory::Application) {
            buttons.push_back(cur);
            continue;
        }
        int count = VisualTreeHelper::GetChildrenCount(cur);
        for (int i = 0; i < count; ++i) {
            if (auto ch = VisualTreeHelper::GetChild(cur, i).try_as<FrameworkElement>()) {
                stack.push_back(ch);
            }
        }
    }

    struct TargetItem {
        RECT rect;
        double x;
    };
    std::vector<TargetItem> items;
    items.reserve(buttons.size());

    for (auto& btn : buttons) {
        try {
            auto t = btn.TransformToVisual(taskbarFrame);
            auto pt = t.TransformPoint({0, 0});
            LONG left = taskbarRect.left + static_cast<LONG>(pt.X * rasterScale);
            LONG top = taskbarRect.top + static_cast<LONG>(pt.Y * rasterScale);
            LONG width = static_cast<LONG>(btn.ActualWidth() * rasterScale);
            LONG height = static_cast<LONG>(btn.ActualHeight() * rasterScale);
            if (width > 0 && height > 0) {
                items.push_back({ { left, top, left + width, top + height }, pt.X });
            }
        } catch (const winrt::hresult_error&) {}
    }

    std::sort(items.begin(), items.end(), [](const TargetItem& a, const TargetItem& b) {
        return a.x < b.x;
    });

    std::vector<RECT> targets;
    targets.reserve(items.size());
    for (auto& item : items) {
        targets.push_back(item.rect);
    }

    Lightency::DragDropAssist::UpdateTargets(key, targetHwnd, targets);
}

static void ClearElementTransforms(FrameworkElement const& el) {
    if (!el) return;
    auto tg = el.RenderTransform().try_as<TransformGroup>();
    if (!tg || tg.Children().Size() < 5) return;
    auto scale = tg.Children().GetAt(1).try_as<ScaleTransform>();
    auto translate = tg.Children().GetAt(2).try_as<TranslateTransform>();
    auto bounceScale = tg.Children().GetAt(3).try_as<ScaleTransform>();
    auto bounceTranslate = tg.Children().GetAt(4).try_as<TranslateTransform>();
    if (scale) { scale.ScaleX(1.0); scale.ScaleY(1.0); }
    if (translate) { translate.X(0.0); translate.Y(0.0); }
    if (bounceScale) { bounceScale.ScaleX(1.0); bounceScale.ScaleY(1.0); }
    if (bounceTranslate) { bounceTranslate.X(0.0); bounceTranslate.Y(0.0); }
    Controls::Canvas::SetZIndex(el, 0);
}

static void ResetSessionTransforms(std::vector<Lightency::DockItem>& items) {
    for (auto& item : items) {
        if (auto el = item.element.get()) {
            ClearElementTransforms(el);
        }
        item.state.scale = 1.0;
        item.state.offset = 0.0;
    }
}

static FrameworkElement LocateItemHostContainer(FrameworkElement const& taskbarFrame) {
    if (!taskbarFrame) return nullptr;
    std::vector<FrameworkElement> queue{taskbarFrame};
    while (!queue.empty()) {
        FrameworkElement current = queue.back();
        queue.pop_back();
        auto className = winrt::get_class_name(current);
        if (className == L"Taskbar.TaskbarFrameRepeater" ||
            className == L"Taskbar.TaskbarItemHost" ||
            std::wstring_view(className).find(L"TaskbarFrameRepeater") != std::wstring_view::npos) {
            return current;
        }
        int count = VisualTreeHelper::GetChildrenCount(current);
        for (int i = 0; i < count; ++i) {
            if (auto child = VisualTreeHelper::GetChild(current, i).try_as<FrameworkElement>()) {
                queue.push_back(child);
            }
        }
    }
    return nullptr;
}

static FrameworkElement LocateDynamicHostContainer(FrameworkElement const& taskbarFrame) {
    if (!taskbarFrame) return nullptr;
    std::vector<FrameworkElement> candidates;
    std::vector<FrameworkElement> stack{taskbarFrame};
    while (!stack.empty()) {
        FrameworkElement current = stack.back();
        stack.pop_back();
        candidates.push_back(current);
        int count = VisualTreeHelper::GetChildrenCount(current);
        for (int i = 0; i < count; ++i) {
            if (auto child = VisualTreeHelper::GetChild(current, i).try_as<FrameworkElement>()) {
                stack.push_back(child);
            }
        }
    }

    FrameworkElement bestContainer = nullptr;
    int maxAppButtons = 0;
    double maxWidth = 0.0;

    for (const auto& node : candidates) {
        int children = VisualTreeHelper::GetChildrenCount(node);
        if (children < 2 || children > 128) continue;
        int appCount = 0;
        for (int i = 0; i < children; ++i) {
            if (auto child = VisualTreeHelper::GetChild(node, i).try_as<FrameworkElement>()) {
                auto name = winrt::get_class_name(child);
                if (name == L"Taskbar.TaskListButton" || name == L"Taskbar.TaskListButtonPanel") {
                    appCount++;
                }
            }
        }
        if (appCount >= 2) {
            double w = node.ActualWidth();
            if (appCount > maxAppButtons || (appCount == maxAppButtons && w > maxWidth)) {
                maxAppButtons = appCount;
                maxWidth = w;
                bestContainer = node;
            }
        }
    }
    return bestContainer;
}

static Lightency::DockHierarchyFingerprint EvaluateContainerFingerprint(FrameworkElement const& container, bool isVertical) {
    Lightency::DockHierarchyFingerprint fp;
    if (!container) return fp;
    fp.count = VisualTreeHelper::GetChildrenCount(container);
    fp.span = isVertical ? container.ActualHeight() : container.ActualWidth();

    uintptr_t hash = 0x811c9dc5;
    for (int i = 0; i < fp.count; ++i) {
        if (auto child = VisualTreeHelper::GetChild(container, i).try_as<FrameworkElement>()) {
            hash ^= reinterpret_cast<uintptr_t>(winrt::get_abi(child));
            hash *= 0x01000193;
            double sz = isVertical ? child.ActualHeight() : child.ActualWidth();
            hash ^= static_cast<uintptr_t>(sz * 100.0);
            hash *= 0x01000193;
        }
    }
    fp.hash = hash;
    return fp;
}

static FrameworkElement FindButtonInSubtree(FrameworkElement const& root) {
    if (!root) return nullptr;
    if (winrt::get_class_name(root) == L"Taskbar.TaskListButton") return root;
    int count = VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        if (auto child = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>()) {
            if (winrt::get_class_name(child) == L"Taskbar.TaskListButton") return child;
            if (auto deep = FindButtonInSubtree(child)) return deep;
        }
    }
    return nullptr;
}

static void RegisterDockItem(Lightency::DockSession& session,
                             FrameworkElement const& element,
                             std::vector<Lightency::DockItem>& items,
                             const std::unordered_map<void*, Lightency::DockTransformState>& previousStates) {
    if (!element) return;
    auto taskbarFrame = session.taskbarFrame.get();
    if (!taskbarFrame) return;

    PrepareHighResolutionIconSources(element, static_cast<double>(g_lightencyDockConfig.maxScale) / 100.0);

    FrameworkElement targetElement = element;
    if (ClassifyTaskbarElement(element) == DockElementCategory::Application) {
        std::vector<FrameworkElement> scan{element};
        double largestArea = 0.0;
        while (!scan.empty()) {
            FrameworkElement cur = scan.back();
            scan.pop_back();
            if (cur.try_as<Controls::Image>()) {
                double area = cur.ActualWidth() * cur.ActualHeight();
                if (cur.ActualWidth() >= 14.0 && cur.ActualHeight() >= 14.0 && area > largestArea) {
                    targetElement = cur;
                    largestArea = area;
                }
            }
            int childCount = VisualTreeHelper::GetChildrenCount(cur);
            for (int i = 0; i < childCount; ++i) {
                if (auto ch = VisualTreeHelper::GetChild(cur, i).try_as<FrameworkElement>()) {
                    scan.push_back(ch);
                }
            }
        }
    }

    auto existingTransform = targetElement.RenderTransform();
    auto tg = existingTransform.try_as<TransformGroup>();
    bool validChain = tg && tg.Children().Size() >= 5 &&
        tg.Children().GetAt(1).try_as<ScaleTransform>() &&
        tg.Children().GetAt(2).try_as<TranslateTransform>() &&
        tg.Children().GetAt(3).try_as<ScaleTransform>() &&
        tg.Children().GetAt(4).try_as<TranslateTransform>();

    if (!validChain) {
        tg = TransformGroup();
        if (existingTransform) {
            tg.Children().Append(existingTransform);
        } else {
            tg.Children().Append(MatrixTransform());
        }
        tg.Children().Append(ScaleTransform());
        tg.Children().Append(TranslateTransform());
        tg.Children().Append(ScaleTransform());
        tg.Children().Append(TranslateTransform());
        targetElement.RenderTransform(tg);
    }

    float originX = 0.5f;
    float originY = 0.5f;
    if (session.isVertical) {
        originX = (session.orientation == Lightency::TaskbarOrientation::Left) ? 0.0f : 1.0f;
    }

    targetElement.RenderTransformOrigin({originX, originY});

    auto visualTransform = targetElement.TransformToVisual(taskbarFrame);
    auto originPoint = visualTransform.TransformPoint({0, 0});

    Lightency::DockItem item;
    item.element = targetElement;
    item.dimension = session.isVertical ? element.ActualHeight() : element.ActualWidth();
    item.anchorPosition = session.isVertical
        ? (originPoint.Y + targetElement.ActualHeight() * 0.5)
        : (originPoint.X + targetElement.ActualWidth() * 0.5);

    void* key = winrt::get_abi(targetElement);
    auto it = previousStates.find(key);
    if (it != previousStates.end()) {
        item.state = it->second;
        if (auto st = tg.Children().GetAt(1).try_as<ScaleTransform>()) {
            st.ScaleX(item.state.scale);
            st.ScaleY(item.state.scale);
        }
        if (auto tt = tg.Children().GetAt(2).try_as<TranslateTransform>()) {
            if (session.isVertical) {
                tt.Y(item.state.offset);
            } else {
                tt.X(item.state.offset);
            }
        }
    }

    items.push_back(std::move(item));
}

static void RebuildSessionItems(Lightency::DockSession& session) {
    std::unordered_map<void*, Lightency::DockTransformState> preservedStates;
    for (const auto& item : session.items) {
        if (auto el = item.element.get()) {
            preservedStates[winrt::get_abi(el)] = item.state;
        }
    }

    auto frame = session.taskbarFrame.get();
    if (!frame) return;

    auto container = session.itemContainer.get();
    if (!container) {
        container = LocateItemHostContainer(frame);
        if (!container) container = LocateDynamicHostContainer(frame);
        if (container) session.itemContainer = container;
    }

    std::vector<Lightency::DockItem> newItems;
    if (container) {
        int childCount = VisualTreeHelper::GetChildrenCount(container);
        for (int i = 0; i < childCount; ++i) {
            auto child = VisualTreeHelper::GetChild(container, i).try_as<FrameworkElement>();
            if (!child) continue;
            if (!IsElementEligibleForAnimation(child)) {
                ClearElementTransforms(child);
                continue;
            }
            FrameworkElement target = FindButtonInSubtree(child);
            if (!target) target = child;
            RegisterDockItem(session, target, newItems, preservedStates);
        }
        session.fingerprint = EvaluateContainerFingerprint(container, session.isVertical);
    }

    std::sort(newItems.begin(), newItems.end(), [](const Lightency::DockItem& a, const Lightency::DockItem& b) {
        return a.anchorPosition < b.anchorPosition;
    });

    session.items = std::move(newItems);
}

static bool RebaseSessionGeometry(Lightency::DockSession& session) {
    auto frame = session.taskbarFrame.get();
    if (!frame) return false;

    bool layoutChanged = false;
    for (auto it = session.items.begin(); it != session.items.end();) {
        if (!it->element.get()) {
            it = session.items.erase(it);
            layoutChanged = true;
        } else {
            ++it;
        }
    }

    for (auto& item : session.items) {
        auto el = item.element.get();
        if (!el) continue;
        try {
            double previousAnchor = item.anchorPosition;
            double previousDim = item.dimension;

            auto t = el.TransformToVisual(frame);
            auto pt = t.TransformPoint({0, 0});
            double currentDim = session.isVertical ? el.ActualHeight() : el.ActualWidth();
            item.dimension = currentDim;
            item.anchorPosition = session.isVertical
                ? (pt.Y + currentDim * 0.5)
                : (pt.X + currentDim * 0.5);

            if (std::abs(item.anchorPosition - previousAnchor) > 0.5 ||
                std::abs(item.dimension - previousDim) > 0.5) {
                layoutChanged = true;
            }
        } catch (const winrt::hresult_error&) {}
    }

    if (layoutChanged && session.items.size() > 1) {
        std::sort(session.items.begin(), session.items.end(), [](const Lightency::DockItem& a, const Lightency::DockItem& b) {
            return a.anchorPosition < b.anchorPosition;
        });
    }
    return layoutChanged;
}

static double EvaluateMagnificationFactor(double distance, double radius) {
    if (distance >= radius || radius <= 0.0) return 0.0;
    const double ratio = distance / radius;
    return 0.5 * (1.0 + std::cos(ratio * 3.14159265358979323846));
}

static void ExecuteDockLayoutFrame(double cursorCoord, Lightency::DockSession& session, double intensity, double dt) {
    if (!g_lightencyDockConfig.dockAnimation || session.items.empty()) {
        ResetSessionTransforms(session.items);
        return;
    }

    auto frame = session.taskbarFrame.get();
    if (!frame) return;

    ApplyTaskbarAppearance(frame);
    ApplyTrayItemVisibility(frame);

    if (!session.cursorInitialized) {
        session.smoothedCursor = cursorCoord;
        session.cursorInitialized = true;
    } else {
        const double delta = cursorCoord - session.smoothedCursor;
        const double trackingRate = 22.0 + std::min(std::abs(delta) * 0.35, 45.0);
        const double alpha = 1.0 - std::exp(-trackingRate * dt);
        session.smoothedCursor += delta * std::clamp(alpha, 0.0, 1.0);
    }
    cursorCoord = session.smoothedCursor;

    const bool autoPhysics = g_lightencyDockConfig.autoPhysics;
    const double maxScale = autoPhysics ? 1.35 : std::clamp(static_cast<double>(g_lightencyDockConfig.maxScale) / 100.0, 1.0, 2.2);
    const double baseRadius = autoPhysics ? 0.0 : std::max(20.0, static_cast<double>(g_lightencyDockConfig.effectRadius));
    const double spacingCoeff = autoPhysics ? 0.50 : std::clamp(static_cast<double>(g_lightencyDockConfig.spacingFactor) / 100.0, 0.0, 1.5);

    const size_t itemCount = session.items.size();
    std::vector<double> targetScales(itemCount, 1.0);
    std::vector<double> expansionDeltas(itemCount, 0.0);
    double cumulativeExpansion = 0.0;
    size_t activeItemIndex = static_cast<size_t>(-1);
    double closestDistance = 1e9;

    for (size_t i = 0; i < itemCount; ++i) {
        const auto& item = session.items[i];
        if (!item.element.get()) continue;

        const double dist = std::abs(cursorCoord - item.anchorPosition);
        const double itemRadius = autoPhysics
            ? (item.dimension > 10.0 ? item.dimension * 1.15 : 48.0)
            : baseRadius;

        const double factor = EvaluateMagnificationFactor(dist, itemRadius);
        targetScales[i] = 1.0 + (maxScale - 1.0) * factor;

        const double itemSize = item.dimension > 0.0 ? item.dimension : 40.0;
        expansionDeltas[i] = (targetScales[i] - 1.0) * itemSize * spacingCoeff;
        cumulativeExpansion += expansionDeltas[i];

        if (dist < closestDistance) {
            closestDistance = dist;
            activeItemIndex = i;
        }
    }

    double bounceMultiplier = 1.0;
    double bounceVerticalOffset = 0.0;
    const double currentBounceIntensity = g_bounceWeight.load();

    if (currentBounceIntensity > 0.0 && activeItemIndex != static_cast<size_t>(-1)) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_bounceStartTimestamp).count();
        const double cycleProgress = std::fmod(elapsedMs, 1200.0) / 1200.0;
        const double pulse = std::sin(cycleProgress * 3.14159265358979323846);
        bounceMultiplier = 1.0 + (pulse * 0.05 * currentBounceIntensity);
        bounceVerticalOffset = pulse * -4.0 * currentBounceIntensity;

        if (session.isVertical) {
            if (session.orientation == Lightency::TaskbarOrientation::Left) bounceVerticalOffset *= -1.0;
        } else {
            if (session.orientation == Lightency::TaskbarOrientation::Top) bounceVerticalOffset *= -1.0;
        }
    }

    const double filterAlpha = std::clamp(1.0 - std::exp(-60.0 * dt), 0.0, 1.0);
    double runningOffset = 0.0;
    const double halfTotalExpansion = cumulativeExpansion * 0.5;

    for (size_t i = 0; i < itemCount; ++i) {
        auto& item = session.items[i];
        auto el = item.element.get();
        if (!el) continue;

        auto tg = el.RenderTransform().try_as<TransformGroup>();
        if (!tg || tg.Children().Size() < 5) continue;

        auto waveScale = tg.Children().GetAt(1).try_as<ScaleTransform>();
        auto waveTranslate = tg.Children().GetAt(2).try_as<TranslateTransform>();
        auto bounceScale = tg.Children().GetAt(3).try_as<ScaleTransform>();
        auto bounceTranslate = tg.Children().GetAt(4).try_as<TranslateTransform>();
        if (!waveScale || !waveTranslate || !bounceScale || !bounceTranslate) continue;

        const double halfItemExpansion = expansionDeltas[i] * 0.5;
        const double targetShift = (runningOffset + halfItemExpansion - halfTotalExpansion) * intensity;
        runningOffset += expansionDeltas[i];

        const double desiredScale = 1.0 + (targetScales[i] - 1.0) * intensity;

        item.state.scale += (desiredScale - item.state.scale) * filterAlpha;
        item.state.offset += (targetShift - item.state.offset) * filterAlpha;

        waveScale.ScaleX(item.state.scale);
        waveScale.ScaleY(item.state.scale);

        if (session.isVertical) {
            waveTranslate.X(0.0);
            waveTranslate.Y(item.state.offset);
        } else {
            waveTranslate.X(item.state.offset);
            waveTranslate.Y(0.0);
        }

        if (i == activeItemIndex && currentBounceIntensity > 0.0) {
            bounceScale.ScaleX(bounceMultiplier);
            bounceScale.ScaleY(bounceMultiplier);
            if (session.isVertical) {
                bounceTranslate.X(bounceVerticalOffset);
                bounceTranslate.Y(0.0);
            } else {
                bounceTranslate.X(0.0);
                bounceTranslate.Y(bounceVerticalOffset);
            }
            Controls::Canvas::SetZIndex(el, 100);
        } else {
            bounceScale.ScaleX(1.0);
            bounceScale.ScaleY(1.0);
            bounceTranslate.X(0.0);
            bounceTranslate.Y(0.0);
            Controls::Canvas::SetZIndex(el, 0);
        }
    }
}

static void OnCompositionRenderTick(winrt::Windows::Foundation::IInspectable const&,
                                    winrt::Windows::Foundation::IInspectable const&) {
    try {
        const auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - g_previousFrameTimestamp).count();
        dt = std::clamp(dt, 0.0, 0.05);
        g_previousFrameTimestamp = now;

        const double fadeSpeed = dt / 0.15;
        double currentFade = g_fadeIntensity.load();
        if (g_isCursorPresent) {
            currentFade = std::min(1.0, currentFade + fadeSpeed);
        } else {
            currentFade = std::max(0.0, currentFade - fadeSpeed);
        }
        g_fadeIntensity = currentFade;

        EnsureLiveConfigMapped();
        if (!g_lightencyDockConfig.dockAnimation) {
            Media::CompositionTarget::Rendering(g_renderEventToken);
            g_isRenderLoopActive = false;
            for (auto& [key, session] : g_dockSessions) {
                ResetSessionTransforms(session.items);
                session.initialized = false;
                session.cursorInitialized = false;
                session.items.clear();
            }
            g_currentActiveSessionKey = nullptr;
            g_lastInputPosition = -1.0;
            g_bounceActive = false;
            g_fadeIntensity = 0.0;
            return;
        }

        void* activeKey = g_currentActiveSessionKey.load();
        if (!activeKey) {
            if (currentFade <= 0.0) {
                Media::CompositionTarget::Rendering(g_renderEventToken);
                g_isRenderLoopActive = false;
            }
            return;
        }

        auto it = g_dockSessions.find(activeKey);
        if (it == g_dockSessions.end()) return;

        auto& session = it->second;

        if (!g_isCursorPresent && currentFade <= 0.0) {
            Media::CompositionTarget::Rendering(g_renderEventToken);
            g_isRenderLoopActive = false;
            ResetSessionTransforms(session.items);
            session.initialized = false;
            session.cursorInitialized = false;
            session.items.clear();
            g_currentActiveSessionKey = nullptr;
            g_lastInputPosition = -1.0;
            g_bounceActive = false;
            return;
        }

        if (!session.initialized) {
            RebuildSessionItems(session);
            session.initialized = true;
        }

        if (g_isCursorPresent) {
            auto elapsedSinceCheck = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - session.lastCheckTime).count();
            if (elapsedSinceCheck > 120) {
                session.lastCheckTime = now;
                auto frame = session.taskbarFrame.get();
                if (frame) {
                    UpdateSessionOrientation(session);
                    auto container = session.itemContainer.get();
                    if (!container) {
                        container = LocateItemHostContainer(frame);
                        if (!container) container = LocateDynamicHostContainer(frame);
                        if (container) session.itemContainer = container;
                    }
                    if (container) {
                        auto sig = EvaluateContainerFingerprint(container, session.isVertical);
                        if (sig.count != session.fingerprint.count ||
                            std::abs(sig.span - session.fingerprint.span) > 0.5 ||
                            sig.hash != session.fingerprint.hash) {
                            RebuildSessionItems(session);
                        } else {
                            RebaseSessionGeometry(session);
                        }
                        session.fingerprint = sig;
                    }
                }
            }
        }

        if (session.items.empty()) {
            if (session.initialized) RebuildSessionItems(session);
            if (session.items.empty()) return;
        }

        if (g_lightencyDockConfig.disableBounce) {
            g_bounceActive = false;
        } else {
            auto idleDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - g_lastMotionTimestamp).count();
            if (g_isCursorPresent && idleDuration > 100) {
                if (!g_bounceActive.exchange(true)) {
                    g_bounceStartTimestamp = now;
                }
            } else {
                g_bounceActive = false;
            }
        }

        const double targetBounce = g_bounceActive ? 1.0 : 0.0;
        const double bounceDelta = dt / 0.15;
        double currentBounce = g_bounceWeight.load();
        if (targetBounce > currentBounce) {
            currentBounce = std::min(targetBounce, currentBounce + bounceDelta);
        } else {
            currentBounce = std::max(targetBounce, currentBounce - bounceDelta);
        }
        g_bounceWeight = currentBounce;

        double cursorCoord = g_lastInputPosition.load();
        if (cursorCoord < 0.0) cursorCoord = 0.0;

        const double easedFade = currentFade * currentFade * (3.0 - 2.0 * currentFade);
        ExecuteDockLayoutFrame(cursorCoord, session, easedFade, dt);
    } catch (const winrt::hresult_error&) {
        if (g_isRenderLoopActive.exchange(false)) {
            Media::CompositionTarget::Rendering(g_renderEventToken);
        }
    }
}

static void HandleSessionPointerMoved(void* key, Input::PointerRoutedEventArgs const& args) {
    try {
        auto it = g_dockSessions.find(key);
        if (it == g_dockSessions.end()) return;

        auto& session = it->second;
        auto frame = session.taskbarFrame.get();
        if (!frame) return;

        UpdateSessionOrientation(session);
        g_isCursorPresent = true;
        g_currentActiveSessionKey = key;

        auto point = args.GetCurrentPoint(frame).Position();
        double coord = session.isVertical ? point.Y : point.X;

        if (std::abs(coord - g_lastInputPosition.load()) > 0.5) {
            g_lastMotionTimestamp = std::chrono::steady_clock::now();
        }
        g_lastInputPosition = coord;

        if (!g_isRenderLoopActive.exchange(true)) {
            g_previousFrameTimestamp = std::chrono::steady_clock::now();
            g_renderEventToken = Media::CompositionTarget::Rendering(OnCompositionRenderTick);
            g_lastMotionTimestamp = std::chrono::steady_clock::now();
        }
    } catch (const winrt::hresult_error&) {}
}

static void HandleSessionPointerExited(void* key) {
    try {
        if (g_currentActiveSessionKey.load() == key) {
            g_isCursorPresent = false;
            g_bounceActive = false;
        }
    } catch (const winrt::hresult_error&) {}
}

static void InitializeDockSession(void* key, FrameworkElement const& taskbarFrame) {
    try {
        Lightency::DockSession session;
        session.taskbarFrame = taskbarFrame;
        session.initialized = false;
        session.windowHandle = ResolveTaskbarWindow();
        UpdateSessionOrientation(session);
        g_dockSessions[key] = std::move(session);
    } catch (const winrt::hresult_error&) {}
}

static void DockEngine_Cleanup() {
    for (auto& [key, sub] : g_pointerSubscriptions) {
        HWND hwnd = ResolveTaskbarWindow();
        auto it = g_dockSessions.find(key);
        if (it != g_dockSessions.end() && it->second.windowHandle) {
            hwnd = it->second.windowHandle;
        }
        if (hwnd) {
            DispatchToTaskbarThread(hwnd, [](PVOID data) {
                auto* s = static_cast<Lightency::XamlPointerSubscription*>(data);
                try {
                    if (auto el = s->element.get()) {
                        el.PointerMoved(s->movedToken);
                        el.PointerExited(s->exitedToken);
                        if (s->layoutUpdated) {
                            el.LayoutUpdated(s->layoutUpdatedToken);
                        }
                    }
                } catch (const winrt::hresult_error&) {}
            }, &sub);
        }
    }
    g_pointerSubscriptions.clear();

    if (g_startMenuWinEventHook) {
        UnhookWinEvent(g_startMenuWinEventHook);
        g_startMenuWinEventHook = nullptr;
    }

    RestoreDefaultLayout();
    RestoreTaskbarAppearance();

    g_currentActiveSessionKey = nullptr;
    g_bounceActive = false;
    g_bounceWeight = 0.0;
    g_isCursorPresent = false;
    g_fadeIntensity = 0.0;

    try {
        if (g_isRenderLoopActive.exchange(false)) {
            Media::CompositionTarget::Rendering(g_renderEventToken);
        }
    } catch (const winrt::hresult_error&) {}

    std::map<HWND, std::vector<winrt::weak_ref<FrameworkElement>>> windowIcons;
    for (auto& [key, session] : g_dockSessions) {
        HWND hwnd = session.windowHandle ? session.windowHandle : ResolveTaskbarWindow();
        if (hwnd) {
            auto& list = windowIcons[hwnd];
            for (auto& item : session.items) {
                list.push_back(item.element);
            }
        }
    }

    for (auto& [hwnd, elements] : windowIcons) {
        std::function<void()> resetAction = [elems = std::move(elements)]() {
            try {
                for (auto& weakEl : elems) {
                    if (auto el = weakEl.get()) {
                        ClearElementTransforms(el);
                    }
                }
            } catch (const winrt::hresult_error&) {}
        };
        DispatchToTaskbarThread(hwnd, [](PVOID p) {
            auto* fn = static_cast<std::function<void()>*>(p);
            (*fn)();
        }, &resetAction);
    }

    g_dockSessions.clear();
}

static void DockEngine_ApplySettings() {
    if (!g_lightencyDockConfig.layoutEditor) {
        RestoreDefaultLayout();
    }
    if (!g_lightencyDockConfig.trayItems) {
        RestoreTrayVisibility();
    }

    Lightency::DragDropAssist::SetEnabled(g_lightencyDockConfig.dragDropAssist);
    if (g_lightencyDockConfig.dragDropAssist) {
        for (auto& [key, session] : g_dockSessions) {
            if (auto frame = session.taskbarFrame.get()) {
                UpdateDragDropTargets(key, frame, session.windowHandle);
            }
        }
    }

    std::map<HWND, std::vector<Lightency::DockSession*>> windowSessions;
    std::map<HWND, std::vector<winrt::weak_ref<FrameworkElement>>> windowFrames;

    for (auto& [key, session] : g_dockSessions) {
        HWND hwnd = session.windowHandle ? session.windowHandle : ResolveTaskbarWindow();
        if (hwnd) {
            windowSessions[hwnd].push_back(&session);
            if (auto frame = session.taskbarFrame.get()) {
                windowFrames[hwnd].push_back(session.taskbarFrame);
            }
        }
    }

    for (auto& [key, sub] : g_pointerSubscriptions) {
        HWND hwnd = ResolveTaskbarWindow();
        auto it = g_dockSessions.find(key);
        if (it != g_dockSessions.end() && it->second.windowHandle) hwnd = it->second.windowHandle;
        if (hwnd && sub.element) {
            windowFrames[hwnd].push_back(sub.element);
        }
    }

    std::set<HWND> uniqueHwnds;
    for (const auto& [hwnd, sess] : windowSessions) uniqueHwnds.insert(hwnd);
    for (const auto& [hwnd, frames] : windowFrames) uniqueHwnds.insert(hwnd);

    for (HWND hwnd : uniqueHwnds) {
        std::vector<Lightency::DockSession*> sessList;
        auto itS = windowSessions.find(hwnd);
        if (itS != windowSessions.end()) sessList = std::move(itS->second);

        std::vector<winrt::weak_ref<FrameworkElement>> frameList;
        auto itF = windowFrames.find(hwnd);
        if (itF != windowFrames.end()) frameList = std::move(itF->second);

        std::function<void()> updateAction = [sessList = std::move(sessList), frameList = std::move(frameList)]() {
            try {
                for (auto* session : sessList) {
                    ResetSessionTransforms(session->items);
                    if (auto frame = session->taskbarFrame.get()) {
                        ApplyTaskbarAppearance(frame);
                        ApplyTrayItemVisibility(frame);
                    }
                    session->initialized = false;
                    session->items.clear();
                }
                for (auto& weakFrame : frameList) {
                    if (auto frame = weakFrame.get()) {
                        ApplyTaskbarAppearance(frame);
                        ApplyTrayItemVisibility(frame);
                    }
                }
            } catch (const winrt::hresult_error&) {}
        };

        DispatchToTaskbarThread(hwnd, [](PVOID p) {
            auto* fn = static_cast<std::function<void()>*>(p);
            (*fn)();
        }, &updateAction);
    }

    g_bounceActive = false;
}

namespace Lightency {

void DockAnimation::AttachXamlElement(IUnknown* object) {
    if (!object) return;
    try {
        FrameworkElement element{nullptr};
        if (FAILED(object->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element)))) return;
        const auto type = winrt::get_class_name(element);
        const bool taskbar = type == L"Taskbar.TaskbarFrame";
        if (!taskbar && type != L"SystemTray.SystemTrayFrame") return;
        auto key = winrt::get_abi(element);
        if (g_pointerSubscriptions.contains(key)) return;

        EnsureLiveConfigMapped();
        ApplyTaskbarAppearance(element);
        ApplyTrayItemVisibility(element);
        InitializeDockSession(key, element);

        XamlPointerSubscription subscription;
        subscription.element = element;
        subscription.moved = [key, taskbar, weak = winrt::make_weak(element)](
            auto const&, Input::PointerRoutedEventArgs const& args) {
            try {
                auto frame = weak.get();
                if (!frame) return;
                EnsureLiveConfigMapped();
                ApplyTaskbarAppearance(frame);
                ApplyTrayItemVisibility(frame);
                if (HandleLayoutPointerMove(frame, args)) return;
                if (taskbar && g_lightencyDockConfig.dockAnimation) {
                    HandleSessionPointerMoved(key, args);
                } else if (taskbar) {
                    HandleSessionPointerExited(key);
                }
            } catch (const winrt::hresult_error&) {}
        };
        subscription.exited = [key](auto const&, auto const&) {
            HandleSessionPointerExited(key);
        };

        if (taskbar) {
            StartButtonStyle::AttachTaskbar(object);
            HWND hWnd = ResolveTaskbarWindow();
            auto it = g_dockSessions.find(key);
            if (it != g_dockSessions.end() && it->second.windowHandle) hWnd = it->second.windowHandle;
            UpdateDragDropTargets(key, element, hWnd);
            subscription.layoutUpdated = [key, weak = winrt::make_weak(element)](auto const&, auto const&) {
                try {
                    if (auto frame = weak.get()) {
                        EnsureLiveConfigMapped();
                        HWND h = ResolveTaskbarWindow();
                        auto itCtx = g_dockSessions.find(key);
                        if (itCtx != g_dockSessions.end() && itCtx->second.windowHandle) h = itCtx->second.windowHandle;
                        UpdateDragDropTargets(key, frame, h);
                    }
                } catch (const winrt::hresult_error&) {}
            };
        } else {
            subscription.layoutUpdated = [weak = winrt::make_weak(element)](auto const&, auto const&) {
                try {
                    if (auto frame = weak.get()) {
                        EnsureLiveConfigMapped();
                        ApplyTrayItemVisibility(frame);
                    }
                } catch (const winrt::hresult_error&) {}
            };
        }

        subscription.movedToken = element.PointerMoved(subscription.moved);
        try {
            subscription.exitedToken = element.PointerExited(subscription.exited);
            if (subscription.layoutUpdated) {
                subscription.layoutUpdatedToken = element.LayoutUpdated(subscription.layoutUpdated);
            }
        } catch (const winrt::hresult_error&) {
            element.PointerMoved(subscription.movedToken);
            throw;
        }

        g_pointerSubscriptions.emplace(key, std::move(subscription));
    } catch (const winrt::hresult_error&) {}
}

bool DockAnimation::Initialize() {
    if (!g_startMenuWinEventHook) {
        g_startMenuWinEventHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, StartMenuWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    Lightency::DragDropAssist::SetEnabled(g_lightencyDockConfig.dragDropAssist);
    return XamlBridge::Initialize();
}

void DockAnimation::Shutdown() {
    HWND hWnd = ResolveTaskbarWindow();
    if (hWnd) {
        DispatchToTaskbarThread(hWnd, [](PVOID) {
            Lightency::DragDropAssist::Shutdown();
            StartButtonStyle::Shutdown();
            XamlBridge::Shutdown();
            DockEngine_Cleanup();
        }, nullptr);
    } else {
        Lightency::DragDropAssist::Shutdown();
        StartButtonStyle::Shutdown();
        XamlBridge::Shutdown();
        DockEngine_Cleanup();
    }
}

void DockAnimation::UpdateSettings(const SharedHookConfig& config) {
    g_lightencyDockConfig = config;
    StartButtonStyle::UpdateSettings(config);
    DockEngine_ApplySettings();
}

void DockAnimation::RefreshSettings() {
    EnsureLiveConfigMapped();
    StartButtonStyle::RefreshSettings();
    DockEngine_ApplySettings();
}

void DockAnimation::OnWindowCreated(HWND) {}

}
