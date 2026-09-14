#include "start_button_style.h"
#include "../common/types.h"
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Automation.h>

using namespace winrt::Windows::UI::Xaml;
namespace Composition = winrt::Windows::UI::Composition;

namespace Lightency::StartButtonStyle {
namespace {

struct HslColor {
    double hue = 0.0;
    double saturation = 0.0;
    double lightness = 0.0;
};

HslColor RgbToHsl(winrt::Windows::UI::Color color) {
    const double r = color.R / 255.0;
    const double g = color.G / 255.0;
    const double b = color.B / 255.0;
    const double mx = (std::max)({r, g, b});
    const double mn = (std::min)({r, g, b});
    const double d = mx - mn;
    HslColor hsl{0.0, 0.0, (mx + mn) / 2.0};
    if (d > 0.00001) {
        hsl.saturation = d / (1.0 - std::abs(2.0 * hsl.lightness - 1.0));
        if (mx == r) hsl.hue = (g - b) / d;
        else if (mx == g) hsl.hue = (b - r) / d + 2.0;
        else hsl.hue = (r - g) / d + 4.0;
        hsl.hue *= 60.0;
        if (hsl.hue < 0.0) hsl.hue += 360.0;
    }
    return hsl;
}

winrt::Windows::UI::Color HslToRgb(HslColor hsl, uint8_t alpha) {
    const double c = (1.0 - std::abs(2.0 * hsl.lightness - 1.0)) * hsl.saturation;
    const double h = hsl.hue / 60.0;
    const double x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    const double m = hsl.lightness - c / 2.0;
    double r = 0.0, g = 0.0, b = 0.0;
    if (h < 1.0) { r = c; g = x; }
    else if (h < 2.0) { r = x; g = c; }
    else if (h < 3.0) { g = c; b = x; }
    else if (h < 4.0) { g = x; b = c; }
    else if (h < 5.0) { r = x; b = c; }
    else { r = c; b = x; }
    auto toByte = [m](double v) -> uint8_t {
        return static_cast<uint8_t>(std::lround(std::clamp(v + m, 0.0, 1.0) * 255.0));
    };
    return winrt::Windows::UI::Color{alpha, toByte(r), toByte(g), toByte(b)};
}

HslColor GetSystemAccentHsl() {
    try {
        winrt::Windows::UI::ViewManagement::UISettings uiSettings;
        auto color = uiSettings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Accent);
        return RgbToHsl(color);
    } catch (const winrt::hresult_error&) {
        return HslColor{210.0, 1.0, 0.5};
    }
}

winrt::Windows::UI::Color TransformColor(winrt::Windows::UI::Color source, const SharedHookConfig& cfg, const HslColor& accentHsl) {
    HslColor hsl = RgbToHsl(source);
    if (cfg.startIconAccentColor) {
        hsl.hue = accentHsl.hue;
        hsl.saturation = std::clamp(hsl.saturation * (accentHsl.saturation > 0.05 ? 1.0 : 0.0), 0.0, 1.0);
    } else if (cfg.startIconColorHue >= 0) {
        hsl.hue = std::fmod(static_cast<double>(cfg.startIconColorHue), 360.0);
        if (hsl.hue < 0.0) hsl.hue += 360.0;
        if (hsl.saturation < 0.3) hsl.saturation = 0.85;
    }
    return HslToRgb(hsl, source.A);
}

SharedHookConfig GetCurrentHookConfig() {
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_HOOK_CONFIG_MAPPING_NAME);
    if (hMap) {
        auto* p = static_cast<SharedHookConfig*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedHookConfig)));
        if (p) {
            SharedHookConfig cfg = SharedHookConfig::Read(p);
            UnmapViewOfFile(p);
            CloseHandle(hMap);
            return cfg;
        }
        CloseHandle(hMap);
    }
    return SharedHookConfig{};
}

std::mutex g_mutex;
winrt::weak_ref<FrameworkElement> g_trackedStartButton{nullptr};
winrt::weak_ref<FrameworkElement> g_trackedIcon{nullptr};
winrt::event_token g_accentToken{};
winrt::Windows::UI::ViewManagement::UISettings g_uiSettings{nullptr};
std::unordered_map<void*, winrt::Windows::UI::Color> g_baselineColors;
SharedHookConfig g_cachedConfig{};

FrameworkElement FindStartButton(FrameworkElement const& root) {
    if (!root) return nullptr;
    std::vector<FrameworkElement> stack{root};
    while (!stack.empty()) {
        FrameworkElement el = stack.back();
        stack.pop_back();
        try {
            auto autoId = Automation::AutomationProperties::GetAutomationId(el);
            if (autoId == L"StartButton") return el;
            int count = Media::VisualTreeHelper::GetChildrenCount(el);
            for (int i = 0; i < count; ++i) {
                if (auto child = Media::VisualTreeHelper::GetChild(el, i).try_as<FrameworkElement>()) {
                    stack.push_back(child);
                }
            }
        } catch (const winrt::hresult_error&) {}
    }
    return nullptr;
}

FrameworkElement FindStartIcon(FrameworkElement const& startButton) {
    if (!startButton) return nullptr;
    std::vector<FrameworkElement> stack{startButton};
    while (!stack.empty()) {
        FrameworkElement el = stack.back();
        stack.pop_back();
        try {
            if (el.Name() == L"Icon") return el;
            int count = Media::VisualTreeHelper::GetChildrenCount(el);
            for (int i = 0; i < count; ++i) {
                if (auto child = Media::VisualTreeHelper::GetChild(el, i).try_as<FrameworkElement>()) {
                    stack.push_back(child);
                }
            }
        } catch (const winrt::hresult_error&) {}
    }
    return nullptr;
}

void ApplyIconScale(FrameworkElement const& icon, const SharedHookConfig& cfg) {
    if (!icon) return;
    try {
        if (cfg.startIconSize == 100) {
            icon.ClearValue(UIElement::RenderTransformProperty());
            icon.ClearValue(UIElement::RenderTransformOriginProperty());
            return;
        }
        Media::ScaleTransform scale = nullptr;
        if (auto current = icon.RenderTransform()) {
            scale = current.try_as<Media::ScaleTransform>();
        }
        if (!scale) {
            scale = Media::ScaleTransform();
            icon.RenderTransform(scale);
        }
        icon.RenderTransformOrigin(winrt::Windows::Foundation::Point{0.5f, 0.5f});
        const double factor = std::clamp(cfg.startIconSize, 40, 250) / 100.0;
        scale.ScaleX(factor);
        scale.ScaleY(factor);
    } catch (const winrt::hresult_error&) {}
}

void ApplyIconColors(FrameworkElement const& icon, const SharedHookConfig& cfg, const HslColor& accentHsl) {
    if (!icon) return;
    try {
        auto rootVisual = Hosting::ElementCompositionPreview::GetElementChildVisual(icon);
        if (!rootVisual) return;

        std::vector<Composition::Visual> visualStack{ rootVisual };
        std::vector<Composition::CompositionShape> shapeStack;
        std::vector<Composition::CompositionBrush> brushList;

        while (!visualStack.empty()) {
            auto vis = visualStack.back();
            visualStack.pop_back();

            if (auto sv = vis.try_as<Composition::ShapeVisual>()) {
                for (const auto& shape : sv.Shapes()) {
                    shapeStack.push_back(shape);
                }
            }
            if (auto cv = vis.try_as<Composition::ContainerVisual>()) {
                for (const auto& child : cv.Children()) {
                    visualStack.push_back(child);
                }
            }
        }

        while (!shapeStack.empty()) {
            auto shp = shapeStack.back();
            shapeStack.pop_back();

            if (auto group = shp.try_as<Composition::CompositionContainerShape>()) {
                for (const auto& child : group.Shapes()) {
                    shapeStack.push_back(child);
                }
            } else if (auto sprite = shp.try_as<Composition::CompositionSpriteShape>()) {
                if (auto fill = sprite.FillBrush()) brushList.push_back(fill);
                if (auto stroke = sprite.StrokeBrush()) brushList.push_back(stroke);
            }
        }

        const bool customEnabled = cfg.startIconCustom && (cfg.startIconAccentColor || cfg.startIconColorHue >= 0);
        static const winrt::Windows::UI::Color kDefaultStartColor{ 255, 0, 164, 239 };

        for (const auto& brush : brushList) {
            if (auto colorBrush = brush.try_as<Composition::CompositionColorBrush>()) {
                try { colorBrush.StopAnimation(L"Color"); } catch (const winrt::hresult_error&) {}
                void* key = winrt::get_abi(colorBrush);
                auto it = g_baselineColors.find(key);
                if (it == g_baselineColors.end()) {
                    auto current = colorBrush.Color();
                    if (!customEnabled) {
                        g_baselineColors[key] = current;
                    } else {
                        HslColor currentHsl = RgbToHsl(current);
                        bool isCustom = false;
                        if (cfg.startIconAccentColor) {
                            isCustom = (std::abs(currentHsl.hue - accentHsl.hue) < 2.0);
                        } else if (cfg.startIconColorHue >= 0) {
                            double diff = std::abs(currentHsl.hue - cfg.startIconColorHue);
                            if (diff > 180.0) diff = 360.0 - diff;
                            isCustom = (diff < 5.0);
                        }
                        g_baselineColors[key] = isCustom ? kDefaultStartColor : current;
                    }
                    it = g_baselineColors.find(key);
                }
                const auto base = it->second;
                colorBrush.Color(customEnabled ? TransformColor(base, cfg, accentHsl) : base);
            } else if (auto gradBrush = brush.try_as<Composition::CompositionGradientBrush>()) {
                for (const auto& stop : gradBrush.ColorStops()) {
                    try { stop.StopAnimation(L"Color"); } catch (const winrt::hresult_error&) {}
                    void* key = winrt::get_abi(stop);
                    auto it = g_baselineColors.find(key);
                    if (it == g_baselineColors.end()) {
                        auto current = stop.Color();
                        if (!customEnabled) {
                            g_baselineColors[key] = current;
                        } else {
                            HslColor currentHsl = RgbToHsl(current);
                            bool isCustom = false;
                            if (cfg.startIconAccentColor) {
                                isCustom = (std::abs(currentHsl.hue - accentHsl.hue) < 2.0);
                            } else if (cfg.startIconColorHue >= 0) {
                                double diff = std::abs(currentHsl.hue - cfg.startIconColorHue);
                                if (diff > 180.0) diff = 360.0 - diff;
                                isCustom = (diff < 5.0);
                            }
                            g_baselineColors[key] = isCustom ? kDefaultStartColor : current;
                        }
                        it = g_baselineColors.find(key);
                    }
                    const auto base = it->second;
                    stop.Color(customEnabled ? TransformColor(base, cfg, accentHsl) : base);
                }
            }
        }
    } catch (const winrt::hresult_error&) {}
}

UINT_PTR g_animTimerId = 0;
int g_animTicksLeft = 0;

void TriggerClickAnimationProtection() {
    g_animTicksLeft = 35;
    if (g_animTimerId == 0) {
        g_animTimerId = SetTimer(nullptr, 0, 25, [](HWND, UINT, UINT_PTR id, DWORD) {
            std::lock_guard lk(g_mutex);
            if (!g_cachedConfig.startIconCustom || (!g_cachedConfig.startIconAccentColor && g_cachedConfig.startIconColorHue < 0)) {
                KillTimer(nullptr, id);
                g_animTimerId = 0;
                return;
            }
            if (auto ic = g_trackedIcon.get()) {
                const HslColor accentHsl = GetSystemAccentHsl();
                ApplyIconColors(ic, g_cachedConfig, accentHsl);
            }
            if (--g_animTicksLeft <= 0) {
                KillTimer(nullptr, id);
                g_animTimerId = 0;
            }
        });
    }
}

void ApplyStyleInternal(FrameworkElement const& icon, const SharedHookConfig& cfg) {
    if (!icon) return;
    const HslColor accentHsl = GetSystemAccentHsl();
    ApplyIconScale(icon, cfg);
    ApplyIconColors(icon, cfg, accentHsl);
}

}

void AttachTaskbar(IUnknown* taskbarFrame) {
    if (!taskbarFrame) return;
    try {
        FrameworkElement root{nullptr};
        if (FAILED(taskbarFrame->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(root)))) return;
        FrameworkElement startButton = FindStartButton(root);
        if (!startButton) return;
        FrameworkElement icon = FindStartIcon(startButton);
        if (!icon) return;

        std::lock_guard lock(g_mutex);
        g_trackedStartButton = winrt::make_weak(startButton);
        g_trackedIcon = winrt::make_weak(icon);

        g_cachedConfig = GetCurrentHookConfig();
        ApplyStyleInternal(icon, g_cachedConfig);

        icon.Loaded([](winrt::Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
            if (auto loadedIcon = sender.try_as<FrameworkElement>()) {
                std::lock_guard lk(g_mutex);
                ApplyStyleInternal(loadedIcon, g_cachedConfig);
            }
        });

        auto onPointerEvent = [](auto const&, auto const&) {
            std::lock_guard lk(g_mutex);
            if (!g_cachedConfig.startIconCustom || (!g_cachedConfig.startIconAccentColor && g_cachedConfig.startIconColorHue < 0)) return;
            if (auto ic = g_trackedIcon.get()) {
                const HslColor accentHsl = GetSystemAccentHsl();
                ApplyIconColors(ic, g_cachedConfig, accentHsl);
            }
            TriggerClickAnimationProtection();
        };

        startButton.PointerEntered(onPointerEvent);
        startButton.PointerExited(onPointerEvent);
        startButton.PointerPressed(onPointerEvent);
        startButton.PointerReleased(onPointerEvent);
        startButton.PointerCanceled(onPointerEvent);
        startButton.PointerCaptureLost(onPointerEvent);

        auto onLayout = [](auto const&, winrt::Windows::Foundation::IInspectable const&) {
            std::lock_guard lk(g_mutex);
            if (!g_cachedConfig.startIconCustom || (!g_cachedConfig.startIconAccentColor && g_cachedConfig.startIconColorHue < 0)) return;
            if (auto ic = g_trackedIcon.get()) {
                const HslColor accentHsl = GetSystemAccentHsl();
                ApplyIconColors(ic, g_cachedConfig, accentHsl);
            }
        };
        icon.LayoutUpdated(onLayout);
        startButton.LayoutUpdated(onLayout);

        if (!g_uiSettings) {
            g_uiSettings = winrt::Windows::UI::ViewManagement::UISettings();
            g_accentToken = g_uiSettings.ColorValuesChanged([](auto const&, auto const&) {
                std::lock_guard lk(g_mutex);
                if (auto ic = g_trackedIcon.get()) {
                    ApplyStyleInternal(ic, g_cachedConfig);
                }
            });
        }
    } catch (const winrt::hresult_error&) {}
}

void UpdateSettings(const SharedHookConfig& config) {
    std::lock_guard lock(g_mutex);
    g_cachedConfig = config;
    if (!config.startIconCustom && g_animTimerId != 0) {
        KillTimer(nullptr, g_animTimerId);
        g_animTimerId = 0;
    }
    if (auto icon = g_trackedIcon.get()) {
        ApplyStyleInternal(icon, config);
    }
}

void RefreshSettings() {
    const SharedHookConfig cfg = GetCurrentHookConfig();
    UpdateSettings(cfg);
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_animTimerId != 0) {
        KillTimer(nullptr, g_animTimerId);
        g_animTimerId = 0;
    }
    if (g_uiSettings && g_accentToken.value != 0) {
        try {
            g_uiSettings.ColorValuesChanged(g_accentToken);
        } catch (const winrt::hresult_error&) {}
        g_accentToken = {};
        g_uiSettings = nullptr;
    }
    SharedHookConfig disabled{};
    disabled.startIconCustom = false;
    disabled.startIconSize = 100;
    disabled.startIconAccentColor = false;
    disabled.startIconColorHue = -1;
    if (auto icon = g_trackedIcon.get()) {
        ApplyStyleInternal(icon, disabled);
    }
    g_baselineColors.clear();
    g_trackedStartButton = nullptr;
    g_trackedIcon = nullptr;
}

}
