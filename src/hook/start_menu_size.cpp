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
#include <optional>
#include <cmath>
#include <vector>

using namespace winrt::Windows::UI::Xaml;

namespace Lightency::StartMenuSize {
namespace {
struct Dimensions {
    bool enabled = false;
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
bool g_applying = false;
bool g_applyPending = false;

struct HiddenElement {
    winrt::weak_ref<FrameworkElement> element;
    Visibility visibility;
};
std::vector<HiddenElement> g_hiddenElements;
size_t g_lastHiddenCount = 0;
uint32_t g_appliedHideMask = UINT32_MAX;

enum class HideKind { None, Search, Pinned, Recommended, Profile, Power, ViewSelector, Folders, Footer };

bool ContainsAny(std::wstring_view text, std::initializer_list<std::wstring_view> words) {
    for (auto word : words) {
        if (text.find(word) != std::wstring_view::npos) return true;
    }
    return false;
}

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

void DumpVisualTree(const FrameworkElement& root) {
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
    try {
        auto window = Window::Current();
        auto content = window ? window.Content().try_as<FrameworkElement>() : nullptr;
        if (!content) { g_applying = false; return; }
        if (!IsSearchHost()) ApplyHiddenElements(content);

        auto hosts = LocateSizingHosts(content);
        if (!hosts.contentHost || !hosts.boundaryHost) {
            g_applying = false;
            return;
        }
        g_applyPending = false;

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

        AssignElementWidth(hosts.contentHost, finalWidth);
        AssignElementHeight(hosts.boundaryHost, finalHeight);
        if (!hosts.legacyMode) {
            hosts.boundaryHost.Margin(Thickness{0, 0, 0, 0});
        }
        hosts.contentHost.InvalidateMeasure();
    } catch (...) {
        WriteDiagnostic("start", "Apply failed hr=" + std::to_string(winrt::to_hresult()));
    }
    g_applying = false;
}

bool Load() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_HOOK_CONFIG_MAPPING_NAME);
    if (!mapping) return false;
    auto* config = static_cast<SharedHookConfig*>(MapViewOfFile(mapping,
        FILE_MAP_READ, 0, 0, sizeof(SharedHookConfig)));
    if (config) {
        g_settings.enabled = config->startMenuSizing;
        g_settings.hideSearch = config->startHideSearch;
        g_settings.hidePinned = config->startHidePinned;
        g_settings.hideRecommended = config->startHideRecommended;
        g_settings.hideProfile = config->startHideProfile;
        g_settings.hidePower = config->startHidePower;
        g_settings.hideViewSelector = config->startHideViewSelector;
        g_settings.hideFolders = config->startHideFolders;
        if (IsSearchHost()) {
            g_settings.width = config->searchWidth;
            g_settings.height = config->searchHeight;
        } else {
            g_settings.width = config->startMenuWidth;
            g_settings.height = config->startMenuHeight;
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
            auto dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().CoreWindow().Dispatcher();
            dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [] { RefreshSettings(); });
            return true;
        }
        auto content = window.Content().try_as<FrameworkElement>();
        if (!content) return false;
        if (!g_layoutToken) {
            g_layoutToken = content.LayoutUpdated([](auto&&, auto&&) { Apply(); });
        }
        Apply();
        WriteDiagnostic("start", "Applied size " + std::to_string(g_settings.width) +
            "x" + std::to_string(g_settings.height) + " enabled=" + std::to_string(g_settings.enabled) +
            " hidden=" + std::to_string(g_lastHiddenCount));
        return true;
    } catch (...) {
        WriteDiagnostic("start", "Refresh failed hr=" + std::to_string(winrt::to_hresult()));
        return false;
    }
}

void Shutdown() {
    g_settings.enabled = false;
    g_applyPending = true;
    Apply();
    RestoreHiddenElements();
    g_appliedHideMask = UINT32_MAX;
    try {
        auto window = Window::Current();
        auto content = window ? window.Content().try_as<FrameworkElement>() : nullptr;
        if (content && g_layoutToken) content.LayoutUpdated(g_layoutToken);
    } catch (...) {}
    g_layoutToken = {};
}
}
