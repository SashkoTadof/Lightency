#include "start_menu_size.h"
#include "../common/types.h"
#include "../common/diagnostics.h"
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Input.h>
#include <optional>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cwctype>

using namespace winrt::Windows::UI::Xaml;

namespace Lightency::StartMenuSize {
namespace {
bool ContainsAny(std::wstring_view text, std::initializer_list<std::wstring_view> words) {
    for (auto word : words) {
        if (text.find(word) != std::wstring_view::npos) return true;
    }
    return false;
}
struct Dimensions {
    bool enabled = false;
    bool layoutEditor = false;
    int width = 640;
    int height = 720;
    bool hideSearch = false;
    bool hidePinned = false;
    bool hideRecommended = false;
    bool hideProfile = false;
    bool hidePower = false;
    bool hideViewSelector = false;
    bool hideFolders = false;
};
bool IsSearchHost() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    const wchar_t* name = wcsrchr(path, L'\\');
    return name && _wcsicmp(name + 1, L"SearchHost.exe") == 0;
}
struct LayoutBoundsSnapshot {
    double width = 0.0;
    double height = 0.0;
    double minWidth = 0.0;
    double minHeight = 0.0;
    double maxWidth = 0.0;
    double maxHeight = 0.0;
    Thickness margin{};
    bool hasCustomMargin = false;
    bool captured = false;

    void Record(const FrameworkElement& el, bool trackMargin = false) {
        if (!el) return;
        width = el.Width();
        height = el.Height();
        minWidth = el.MinWidth();
        minHeight = el.MinHeight();
        maxWidth = el.MaxWidth();
        maxHeight = el.MaxHeight();
        if (trackMargin) {
            margin = el.Margin();
            hasCustomMargin = true;
        }
        captured = true;
    }

    void Restore(const FrameworkElement& el) const {
        if (!el || !captured) return;
        auto dep = el.as<DependencyObject>();
        if (std::isnan(width)) dep.ClearValue(FrameworkElement::WidthProperty()); else el.Width(width);
        if (std::isnan(height)) dep.ClearValue(FrameworkElement::HeightProperty()); else el.Height(height);
        if (minWidth <= 0.0) dep.ClearValue(FrameworkElement::MinWidthProperty()); else el.MinWidth(minWidth);
        if (minHeight <= 0.0) dep.ClearValue(FrameworkElement::MinHeightProperty()); else el.MinHeight(minHeight);
        if (std::isinf(maxWidth)) dep.ClearValue(FrameworkElement::MaxWidthProperty()); else el.MaxWidth(maxWidth);
        if (std::isinf(maxHeight)) dep.ClearValue(FrameworkElement::MaxHeightProperty()); else el.MaxHeight(maxHeight);
        if (hasCustomMargin) el.Margin(margin);
    }
};

Dimensions g_settings;
FrameworkElement g_activeContentHost{nullptr};
FrameworkElement g_activeBoundaryHost{nullptr};
LayoutBoundsSnapshot g_contentSnapshot;
LayoutBoundsSnapshot g_boundarySnapshot;
winrt::event_token g_layoutToken{};
Input::PointerEventHandler g_pointerMovedHandler{nullptr};
Input::PointerEventHandler g_pointerPressedHandler{nullptr};
Input::PointerEventHandler g_pointerReleasedHandler{nullptr};
bool g_applying = false;
bool g_applyPending = false;

struct StartLayoutElementState {
    winrt::weak_ref<FrameworkElement> element;
    std::wstring id;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

std::unordered_map<void*, StartLayoutElementState> g_startLayoutElements;
FrameworkElement g_startDragElement{nullptr};
winrt::Windows::Foundation::Point g_startDragStart{};
double g_startDragStartX = 0.0;
double g_startDragStartY = 0.0;

std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), towlower);
    return text;
}

std::wstring LayoutConfigPath() {
    wchar_t userProfile[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", userProfile, ARRAYSIZE(userProfile));
    if (len > 0 && len < ARRAYSIZE(userProfile)) {
        std::wstring dir = std::wstring(userProfile) + L"\\AppData\\Local\\Lightency";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\layout.ini";
    }
    return L"layout.ini";
}

double ReadLayoutValue(const std::wstring& id, const wchar_t* axis) {
    wchar_t value[64] = {};
    std::wstring key = id + L"." + axis;
    GetPrivateProfileStringW(L"Layout", key.c_str(), L"0", value, ARRAYSIZE(value),
                             LayoutConfigPath().c_str());
    wchar_t* end = nullptr;
    double result = wcstod(value, &end);
    return (end && end != value && std::isfinite(result)) ? result : 0.0;
}

void SaveLayoutValue(const std::wstring& id, double x, double y) {
    std::wstring path = LayoutConfigPath();
    std::wstring xKey = id + L".x";
    std::wstring yKey = id + L".y";
    wchar_t xValue[40] = {}, yValue[40] = {};
    swprintf_s(xValue, L"%.2f", x);
    swprintf_s(yValue, L"%.2f", y);
    WritePrivateProfileStringW(L"Layout", xKey.c_str(), xValue, path.c_str());
    WritePrivateProfileStringW(L"Layout", yKey.c_str(), yValue, path.c_str());
}

Media::TranslateTransform EnsureTranslateTransform(const FrameworkElement& element) {
    if (!element) return nullptr;
    auto currentTransform = element.RenderTransform();
    if (auto direct = currentTransform.try_as<Media::TranslateTransform>()) {
        return direct;
    }
    if (auto group = currentTransform.try_as<Media::TransformGroup>()) {
        for (uint32_t i = 0; i < group.Children().Size(); ++i) {
            if (auto t = group.Children().GetAt(i).try_as<Media::TranslateTransform>()) {
                return t;
            }
        }
        auto t = Media::TranslateTransform();
        group.Children().Append(t);
        return t;
    }
    auto t = Media::TranslateTransform();
    if (currentTransform) {
        auto group = Media::TransformGroup();
        group.Children().Append(currentTransform);
        group.Children().Append(t);
        element.RenderTransform(group);
    } else {
        element.RenderTransform(t);
    }
    return t;
}

void SetStartLayoutOffset(StartLayoutElementState& state, double x, double y) {
    state.offsetX = x;
    state.offsetY = y;
    if (auto el = state.element.get()) {
        try {
            auto t = EnsureTranslateTransform(el);
            if (t) {
                t.X(x);
                t.Y(y);
            }
        } catch (const winrt::hresult_error&) {}
    }
}

void RestoreStartDefaultLayout() {
    for (auto& [key, state] : g_startLayoutElements) {
        if (auto element = state.element.get()) {
            try {
                auto t = EnsureTranslateTransform(element);
                if (t) {
                    t.X(0);
                    t.Y(0);
                }
            } catch (const winrt::hresult_error&) {}
        }
    }
    g_startLayoutElements.clear();
    g_startDragElement = nullptr;
}

std::wstring GetElementIdentityText(const FrameworkElement& element) {
    std::wstring result = std::wstring(element.Name()) + L" " +
        std::wstring(winrt::get_class_name(element)) + L" " +
        std::wstring(Automation::AutomationProperties::GetAutomationId(element)) + L" " +
        std::wstring(Automation::AutomationProperties::GetName(element));

    try {
        auto tooltip = Controls::ToolTipService::GetToolTip(element);
        if (tooltip) {
            if (auto str = tooltip.try_as<winrt::hstring>()) {
                result += L" " + std::wstring(*str);
            } else if (auto tb = tooltip.try_as<Controls::TextBlock>()) {
                result += L" " + std::wstring(tb.Text());
            }
        }
    } catch (const winrt::hresult_error&) {}

    return ToLower(result);
}

std::wstring ClassifyPlaceId(const FrameworkElement& element, int fallbackIndex) {
    std::wstring text = GetElementIdentityText(element);
    if (text.find(L"download") != std::wstring::npos) return L"start_place_downloads";
    if (text.find(L"setting") != std::wstring::npos) return L"start_place_settings";
    if (text.find(L"explorer") != std::wstring::npos || text.find(L"file") != std::wstring::npos) return L"start_place_explorer";
    if (text.find(L"doc") != std::wstring::npos) return L"start_place_documents";
    if (text.find(L"pict") != std::wstring::npos || text.find(L"photo") != std::wstring::npos) return L"start_place_pictures";
    if (text.find(L"music") != std::wstring::npos) return L"start_place_music";
    if (text.find(L"vid") != std::wstring::npos) return L"start_place_videos";
    if (text.find(L"net") != std::wstring::npos) return L"start_place_network";
    if (text.find(L"pers") != std::wstring::npos || text.find(L"user") != std::wstring::npos) return L"start_place_personal";

    auto autoId = std::wstring(Automation::AutomationProperties::GetAutomationId(element));
    if (!autoId.empty()) {
        std::wstring clean;
        for (wchar_t c : autoId) {
            if (iswalnum(c) || c == L'_') clean += towlower(c);
        }
        if (!clean.empty()) return L"start_place_" + clean;
    }
    return L"start_place_" + std::to_wstring(fallbackIndex);
}

bool IsPlacesContainer(const FrameworkElement& element) {
    const auto name = std::wstring(element.Name());
    const auto cls = std::wstring(winrt::get_class_name(element));
    return ContainsAny(name, {L"SystemPlaces", L"FoldersList", L"SystemFolders", L"PlacesView", L"PlaceList", L"PlacesContainer", L"QuickActions", L"NavigationPanePlaces"}) ||
           ContainsAny(cls, {L"SystemPlaces", L"FoldersList", L"SystemFolders", L"PlacesView", L"PlaceList", L"PlacesContainer", L"QuickActions", L"NavigationPanePlaces"});
}

std::wstring GetStartElementLayoutId(const FrameworkElement& element) {
    if (!element) return L"";
    const auto name = std::wstring(element.Name());
    const auto cls = std::wstring(winrt::get_class_name(element));
    const auto autoId = std::wstring(Automation::AutomationProperties::GetAutomationId(element));

    auto hasAny = [&](std::initializer_list<std::wstring_view> words) {
        return ContainsAny(name, words) || ContainsAny(cls, words) || ContainsAny(autoId, words);
    };

    if (hasAny({L"UserTile"})) return L"start_profile";
    if (hasAny({L"PowerOption", L"PowerButton"})) return L"start_power";

    return L"";
}

void ApplyStartSavedLayout(const FrameworkElement& root) {
    if (!root || !g_settings.layoutEditor) return;
    std::unordered_set<std::wstring> seenIds;
    std::vector<FrameworkElement> stack{root};
    while (!stack.empty()) {
        auto element = stack.back();
        stack.pop_back();

        if (IsPlacesContainer(element)) {
            int childCount = Media::VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < childCount; ++i) {
                if (auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                    std::wstring placeId = ClassifyPlaceId(child, i);
                    if (seenIds.insert(placeId).second) {
                        void* key = winrt::get_abi(child);
                        auto it = g_startLayoutElements.find(key);
                        if (it == g_startLayoutElements.end() || !it->second.element.get()) {
                            StartLayoutElementState st{child, placeId, 0, 0};
                            double x = ReadLayoutValue(placeId, L"x");
                            double y = ReadLayoutValue(placeId, L"y");
                            SetStartLayoutOffset(st, x, y);
                            g_startLayoutElements[key] = std::move(st);
                        }
                    }
                }
            }
        }

        std::wstring id = GetStartElementLayoutId(element);
        if (!id.empty() && seenIds.insert(id).second) {
            void* key = winrt::get_abi(element);
            auto it = g_startLayoutElements.find(key);
            if (it == g_startLayoutElements.end() || !it->second.element.get()) {
                StartLayoutElementState st{element, id, 0, 0};
                double x = ReadLayoutValue(id, L"x");
                double y = ReadLayoutValue(id, L"y");
                SetStartLayoutOffset(st, x, y);
                g_startLayoutElements[key] = std::move(st);
            }
        }

        const int count = Media::VisualTreeHelper::GetChildrenCount(element);
        for (int i = count - 1; i >= 0; --i) {
            if (auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>())
                stack.push_back(child);
        }
    }
}

FrameworkElement FindStartLayoutElementAt(const FrameworkElement& root, winrt::Windows::Foundation::Point point) {
    FrameworkElement best = nullptr;
    double bestArea = DBL_MAX;
    for (auto& [key, state] : g_startLayoutElements) {
        auto element = state.element.get();
        if (!element || element.ActualWidth() <= 1 || element.ActualHeight() <= 1) continue;
        try {
            auto origin = element.TransformToVisual(root).TransformPoint({0, 0});
            if (point.X >= origin.X && point.X <= origin.X + element.ActualWidth() &&
                point.Y >= origin.Y && point.Y <= origin.Y + element.ActualHeight()) {
                double area = element.ActualWidth() * element.ActualHeight();
                if (area < bestArea) {
                    best = element;
                    bestArea = area;
                }
            }
        } catch (const winrt::hresult_error&) {}
    }
    return best;
}

bool HandleStartLayoutPointer(const FrameworkElement& root, const Input::PointerRoutedEventArgs& args) {
    if (!root || !args || !g_settings.layoutEditor) {
        g_startDragElement = nullptr;
        return false;
    }
    ApplyStartSavedLayout(root);

    bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (!leftDown) {
        g_startDragElement = nullptr;
        return false;
    }

    auto point = args.GetCurrentPoint(root).Position();
    if (!g_startDragElement) {
        if (!ctrlDown) return false;
        auto target = FindStartLayoutElementAt(root, point);
        if (!target) return false;
        void* key = winrt::get_abi(target);
        auto it = g_startLayoutElements.find(key);
        if (it == g_startLayoutElements.end()) return false;
        g_startDragElement = target;
        g_startDragStart = point;
        g_startDragStartX = it->second.offsetX;
        g_startDragStartY = it->second.offsetY;
    }

    double deltaX = point.X - g_startDragStart.X;
    double deltaY = point.Y - g_startDragStart.Y;
    double x = g_startDragStartX + deltaX;
    double y = g_startDragStartY + deltaY;

    void* key = winrt::get_abi(g_startDragElement);
    auto it = g_startLayoutElements.find(key);
    if (it == g_startLayoutElements.end()) return false;

    SetStartLayoutOffset(it->second, x, y);
    SaveLayoutValue(it->second.id, x, y);
    args.Handled(true);
    return true;
}

void EnsureStartLayoutHandlers(const FrameworkElement& content) {
    if (!content || !g_settings.layoutEditor || content.ActualWidth() <= 1 || content.ActualHeight() <= 1) return;
    if (!g_pointerMovedHandler) {
        g_pointerMovedHandler = Input::PointerEventHandler([](const winrt::Windows::Foundation::IInspectable& sender, const Input::PointerRoutedEventArgs& args) {
            if (auto el = sender.try_as<FrameworkElement>()) HandleStartLayoutPointer(el, args);
        });
        content.AddHandler(UIElement::PointerMovedEvent(), winrt::box_value(g_pointerMovedHandler), true);
    }
    if (!g_pointerPressedHandler) {
        g_pointerPressedHandler = Input::PointerEventHandler([](const winrt::Windows::Foundation::IInspectable& sender, const Input::PointerRoutedEventArgs& args) {
            if (auto el = sender.try_as<FrameworkElement>()) HandleStartLayoutPointer(el, args);
        });
        content.AddHandler(UIElement::PointerPressedEvent(), winrt::box_value(g_pointerPressedHandler), true);
    }
    if (!g_pointerReleasedHandler) {
        g_pointerReleasedHandler = Input::PointerEventHandler([](const winrt::Windows::Foundation::IInspectable&, const Input::PointerRoutedEventArgs&) {
            g_startDragElement = nullptr;
        });
        content.AddHandler(UIElement::PointerReleasedEvent(), winrt::box_value(g_pointerReleasedHandler), true);
    }
}

struct HiddenElement {
    winrt::weak_ref<FrameworkElement> element;
    Visibility visibility;
};
std::vector<HiddenElement> g_hiddenElements;
size_t g_lastHiddenCount = 0;
uint32_t g_appliedHideMask = UINT32_MAX;

enum class HideKind { None, Search, Pinned, Recommended, Profile, Power, ViewSelector, Folders, Footer };

HideKind ClassifyElement(const FrameworkElement& element) {
    const auto name = std::wstring(element.Name());
    const auto cls = std::wstring(winrt::get_class_name(element));
    
    auto hasAny = [&](std::initializer_list<std::wstring_view> words) {
        return ContainsAny(name, words) || ContainsAny(cls, words);
    };
    
    if (hasAny({L"SearchBox"})) return HideKind::Search;
    if (hasAny({L"Pinned"})) return HideKind::Pinned;
    if (hasAny({L"Suggestion"})) return HideKind::Recommended;
    if (hasAny({L"UserTile"})) return HideKind::Profile;
    if (hasAny({L"PowerOption", L"PowerButton"})) return HideKind::Power;
    if (hasAny({L"ViewSelection", L"AllListHeading"})) return HideKind::ViewSelector;
    if (hasAny({L"SystemPlaces", L"FoldersList", L"SystemFolders", L"PlacesView", L"PlaceList", L"PlacesContainer", L"QuickActions", L"NavigationPanePlaces"})) return HideKind::Folders;
    
    if (hasAny({L"BottomStrip", L"Footer", L"BottomPanel", L"NavPanePlaceholder", L"NavigationPaneView"})) return HideKind::Footer;
    
    return HideKind::None;
}

bool ShouldHide(HideKind kind) {
    switch (kind) {
    case HideKind::Search: return g_settings.hideSearch;
    case HideKind::Pinned: return g_settings.hidePinned;
    case HideKind::Recommended: return g_settings.hideRecommended;
    case HideKind::Profile: return g_settings.hideProfile;
    case HideKind::Power: return g_settings.hidePower;
    case HideKind::ViewSelector: return g_settings.hideViewSelector;
    case HideKind::Folders: return g_settings.hideFolders;
    case HideKind::Footer: return g_settings.hideProfile && g_settings.hidePower && g_settings.hideFolders;
    default: return false;
    }
}

void RestoreHiddenElements() {
    for (auto& saved : g_hiddenElements) {
        if (auto element = saved.element.get()) element.Visibility(saved.visibility);
    }
    g_hiddenElements.clear();
}

bool IsInAllAppsGrid(const FrameworkElement& element) {
    for (auto current = element; current;) {
        if (current.Name() == L"AllAppsGrid" || current.Name() == L"AppsList" ||
            winrt::get_class_name(current) == L"StartDocked.AllAppsGridListView") {
            return true;
        }
        current = Media::VisualTreeHelper::GetParent(current).try_as<FrameworkElement>();
    }
    return false;
}

void HideCategoryCardRows(const std::vector<FrameworkElement>& cards) {
    if (!g_settings.hideFolders || cards.empty()) return;

    for (const auto& card : cards) {
        g_hiddenElements.push_back({ winrt::make_weak(card), card.Visibility() });
        card.Visibility(Visibility::Collapsed);
    }
}

void DumpVisualTree(const FrameworkElement&) {
}

void ApplyHiddenElements(FrameworkElement root) {
    DumpVisualTree(root);
    
    uint32_t mask = 0;
    if (g_settings.hideSearch) mask |= 1u << 0;
    if (g_settings.hidePinned) mask |= 1u << 1;
    if (g_settings.hideRecommended) mask |= 1u << 2;
    if (g_settings.hideProfile) mask |= 1u << 3;
    if (g_settings.hidePower) mask |= 1u << 4;
    if (g_settings.hideViewSelector) mask |= 1u << 5;
    if (g_settings.hideFolders) mask |= 1u << 6;
    if (mask == 0) {
        RestoreHiddenElements();
        g_appliedHideMask = 0;
        g_lastHiddenCount = 0;
        return;
    }
    if (mask == g_appliedHideMask && (mask == 0 || !g_hiddenElements.empty())) return;
    RestoreHiddenElements();
    g_appliedHideMask = mask;
    std::vector<FrameworkElement> stack{ root };
    std::vector<FrameworkElement> categoryCards;
    while (!stack.empty()) {
        auto element = stack.back();
        stack.pop_back();
        if (element.try_as<Controls::GridViewItem>() && IsInAllAppsGrid(element) &&
            element.ActualWidth() >= 90 && element.ActualWidth() <= 280 &&
            element.ActualHeight() >= 90 && element.ActualHeight() <= 280) {
            categoryCards.push_back(element);
        }
        if (ShouldHide(ClassifyElement(element))) {
            g_hiddenElements.push_back({ winrt::make_weak(element), element.Visibility() });
            element.Visibility(Visibility::Collapsed);
            continue;
        }
        const int count = Media::VisualTreeHelper::GetChildrenCount(element);
        for (int i = count - 1; i >= 0; --i) {
            if (auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>())
                stack.push_back(child);
        }
    }
    HideCategoryCardRows(categoryCards);
    g_lastHiddenCount = g_hiddenElements.size();
}

struct SizingHosts {
    FrameworkElement boundaryHost{nullptr};
    FrameworkElement contentHost{nullptr};
    bool legacyMode = false;
};

SizingHosts LocateSizingHosts(const FrameworkElement& root) {
    SizingHosts hosts;
    if (!root) return hosts;

    if (root.try_as<Controls::Canvas>()) {
        const int childCount = Media::VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < childCount; ++i) {
            if (auto child = Media::VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>()) {
                const std::wstring_view typeName = winrt::get_class_name(child);
                if (typeName.find(L"SizingFrame") != std::wstring_view::npos) {
                    hosts.boundaryHost = child;
                    hosts.contentHost = child;
                    hosts.legacyMode = true;
                    return hosts;
                }
            }
        }
    }

    std::vector<FrameworkElement> scanQueue{ root };
    while (!scanQueue.empty()) {
        auto current = scanQueue.back();
        scanQueue.pop_back();

        const auto name = current.Name();
        if (name == L"FrameRoot") {
            hosts.boundaryHost = current;
        } else if (name == L"MainMenu") {
            hosts.contentHost = current;
        }

        if (hosts.boundaryHost && hosts.contentHost) {
            break;
        }

        const int childCount = Media::VisualTreeHelper::GetChildrenCount(current);
        for (int i = childCount - 1; i >= 0; --i) {
            if (auto child = Media::VisualTreeHelper::GetChild(current, i).try_as<FrameworkElement>()) {
                scanQueue.push_back(child);
            }
        }
    }

    return hosts;
}

void AssignElementWidth(const FrameworkElement& el, double w) {
    el.Width(w);
    el.MinWidth(w);
    el.MaxWidth(w);
}

void AssignElementHeight(const FrameworkElement& el, double h) {
    el.Height(h);
    el.MinHeight(h);
    el.MaxHeight(h);
}

void Apply() {
    if (g_applying || !g_applyPending) return;
    g_applying = true;
    g_applyPending = false;
    try {
        auto window = Window::Current();
        auto content = window ? window.Content().try_as<FrameworkElement>() : nullptr;
        if (!content) { g_applying = false; return; }
        if (!IsSearchHost()) {
            ApplyHiddenElements(content);
            if (g_settings.layoutEditor) {
                ApplyStartSavedLayout(content);
            } else {
                RestoreStartDefaultLayout();
            }
        }

        auto hosts = LocateSizingHosts(content);
        if (!hosts.contentHost || !hosts.boundaryHost) {
            g_applying = false;
            return;
        }

        auto resetActiveHosts = [&] {
            if (g_activeContentHost) {
                g_contentSnapshot.Restore(g_activeContentHost);
                g_activeContentHost = nullptr;
                g_contentSnapshot = {};
            }
            if (g_activeBoundaryHost) {
                g_boundarySnapshot.Restore(g_activeBoundaryHost);
                g_activeBoundaryHost = nullptr;
                g_boundarySnapshot = {};
            }
        };

        if (g_activeContentHost && (g_activeContentHost != hosts.contentHost || g_activeBoundaryHost != hosts.boundaryHost)) {
            resetActiveHosts();
        }

        if (!g_settings.enabled) {
            resetActiveHosts();
            g_applying = false;
            return;
        }

        if (!g_activeContentHost) {
            g_activeContentHost = hosts.contentHost;
            g_activeBoundaryHost = hosts.boundaryHost;
            g_contentSnapshot.Record(hosts.contentHost);
            g_boundarySnapshot.Record(hosts.boundaryHost, !hosts.legacyMode);
        }

        const double baseTargetWidth = std::clamp(static_cast<double>(g_settings.width), 300.0, 3840.0);
        const double baseTargetHeight = std::clamp(static_cast<double>(g_settings.height), 350.0, 2160.0);

        double finalWidth = baseTargetWidth;
        double finalHeight = baseTargetHeight;

        if (hosts.legacyMode) {
            const auto m = hosts.boundaryHost.Margin();
            const double gutter = m.Left + m.Right;
            const double effectiveGutter = (gutter > 0.0) ? gutter : 24.0;
            finalWidth = std::max(300.0, baseTargetWidth - effectiveGutter);
            finalHeight = std::max(350.0, baseTargetHeight - effectiveGutter);
        } else if (hosts.boundaryHost && hosts.contentHost) {
            const double outerW = hosts.boundaryHost.ActualWidth();
            const double innerW = hosts.contentHost.ActualWidth();
            if (outerW > innerW && innerW > 0.0) {
                finalWidth = std::max(300.0, baseTargetWidth - (outerW - innerW));
            }
        }

        if (hosts.contentHost.Width() != finalWidth) {
            AssignElementWidth(hosts.contentHost, finalWidth);
        }
        if (hosts.boundaryHost.Height() != finalHeight) {
            AssignElementHeight(hosts.boundaryHost, finalHeight);
        }
        if (!hosts.legacyMode) {
            auto currentMargin = hosts.boundaryHost.Margin();
            if (currentMargin.Left != 0 || currentMargin.Top != 0 || currentMargin.Right != 0 || currentMargin.Bottom != 0) {
                hosts.boundaryHost.Margin(Thickness{0, 0, 0, 0});
            }
        }
    } catch (const winrt::hresult_error&) {}
    g_applying = false;
}

bool Load() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_HOOK_CONFIG_MAPPING_NAME);
    if (!mapping) return false;
    auto* config = static_cast<SharedHookConfig*>(MapViewOfFile(mapping,
        FILE_MAP_READ, 0, 0, sizeof(SharedHookConfig)));
    if (config) {
        SharedHookConfig cfg = SharedHookConfig::Read(config);
        g_settings.enabled = cfg.startMenuSizing;
        g_settings.layoutEditor = false;
        g_settings.hideSearch = cfg.startHideSearch;
        g_settings.hidePinned = cfg.startHidePinned;
        g_settings.hideRecommended = cfg.startHideRecommended;
        g_settings.hideProfile = cfg.startHideProfile;
        g_settings.hidePower = cfg.startHidePower;
        g_settings.hideViewSelector = cfg.startHideViewSelector;
        g_settings.hideFolders = cfg.startHideFolders;
        if (IsSearchHost()) {
            g_settings.width = cfg.searchWidth;
            g_settings.height = cfg.searchHeight;
        } else {
            g_settings.width = cfg.startMenuWidth;
            g_settings.height = cfg.startMenuHeight;
        }
        UnmapViewOfFile(config);
    }
    CloseHandle(mapping);
    return config != nullptr;
}
}

bool RefreshSettings() {
    if (!Load()) return false;
    try {
        g_applyPending = true;
        auto window = Window::Current();
        if (!window) {
            try {
                auto mainView = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView();
                if (mainView) {
                    auto coreWindow = mainView.CoreWindow();
                    if (coreWindow) {
                        auto dispatcher = coreWindow.Dispatcher();
                        if (dispatcher) {
                            dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [] { RefreshSettings(); });
                            return true;
                        }
                    }
                }
            } catch (const winrt::hresult_error&) {}
            return false;
        }
        auto content = window.Content().try_as<FrameworkElement>();
        if (!content) return false;

        const bool needsHooks = g_settings.enabled ||
            g_settings.hideSearch || g_settings.hidePinned || g_settings.hideRecommended ||
            g_settings.hideProfile || g_settings.hidePower || g_settings.hideViewSelector ||
            g_settings.hideFolders;

        if (!needsHooks) {
            RestoreHiddenElements();
            RestoreStartDefaultLayout();
            if (g_layoutToken) {
                content.LayoutUpdated(g_layoutToken);
                g_layoutToken = {};
            }
            if (g_pointerMovedHandler) {
                content.RemoveHandler(UIElement::PointerMovedEvent(), winrt::box_value(g_pointerMovedHandler));
                g_pointerMovedHandler = nullptr;
            }
            if (g_pointerPressedHandler) {
                content.RemoveHandler(UIElement::PointerPressedEvent(), winrt::box_value(g_pointerPressedHandler));
                g_pointerPressedHandler = nullptr;
            }
            if (g_pointerReleasedHandler) {
                content.RemoveHandler(UIElement::PointerReleasedEvent(), winrt::box_value(g_pointerReleasedHandler));
                g_pointerReleasedHandler = nullptr;
            }
            g_applyPending = false;
            return true;
        }

        if (!g_layoutToken) {
            g_layoutToken = content.LayoutUpdated([](const auto& sender, const auto&) {
                Apply();
                if (auto el = sender.try_as<FrameworkElement>()) EnsureStartLayoutHandlers(el);
            });
        }
        EnsureStartLayoutHandlers(content);
        Apply();
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

void Shutdown() {
    g_settings.enabled = false;
    g_settings.layoutEditor = false;
    g_applyPending = true;
    Apply();
    RestoreHiddenElements();
    RestoreStartDefaultLayout();
    g_appliedHideMask = UINT32_MAX;
    try {
        auto window = Window::Current();
        auto content = window ? window.Content().try_as<FrameworkElement>() : nullptr;
        if (content) {
            if (g_layoutToken) content.LayoutUpdated(g_layoutToken);
            if (g_pointerMovedHandler) content.RemoveHandler(UIElement::PointerMovedEvent(), winrt::box_value(g_pointerMovedHandler));
            if (g_pointerPressedHandler) content.RemoveHandler(UIElement::PointerPressedEvent(), winrt::box_value(g_pointerPressedHandler));
            if (g_pointerReleasedHandler) content.RemoveHandler(UIElement::PointerReleasedEvent(), winrt::box_value(g_pointerReleasedHandler));
        }
    } catch (const winrt::hresult_error&) {}
    g_layoutToken = {};
    g_pointerMovedHandler = nullptr;
    g_pointerPressedHandler = nullptr;
    g_pointerReleasedHandler = nullptr;
}
}
