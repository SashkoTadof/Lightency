#include "modern_gui.h"
#include "icon_gen.h"
#include "config.h"
#include "injector.h"
#include "update_manager.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <memory>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

namespace Lightency {

static const wchar_t* GUI_CLASS_NAME = L"LightencyModernGUIWindow";


constexpr float kWindowWidth = 360.0f;
constexpr float kMainHeaderHeight = 66.0f;
constexpr float kSubPageHeaderHeight = 42.0f;
constexpr float kRowHeight = 40.0f;
constexpr float kBottomPadding = 10.0f;
constexpr float kSidePadding = 16.0f;

enum class SubPage { None, DockSettings, TrayItems, WindowSettings, StartMenuHub, StartMenuSettings, StartMenuHideElements, StartButtonSettings };
enum class RowType { Toggle, Stepper, Segmented, Action, ColorPicker, Slider, Navigation };

struct RowItem {
    std::wstring title;
    RowType type = RowType::Toggle;
    bool hasGear = false;
    SubPage gearTarget = SubPage::None;
    bool* pBoolValue = nullptr;
    int* pIntValue = nullptr;
    int minInt = 0;
    int maxInt = 100;
    int stepInt = 5;
    std::wstring unit = L"";
    std::wstring displayOverride = L"";
    int selectedIndex = 0;
    bool disabled = false;
    std::wstring segmentLeft;
    std::wstring segmentRight;
};

namespace {
    HWND g_hWnd = nullptr;
    AppConfig* g_pConfig = nullptr;
    void (*g_onConfigChanged)() = nullptr;
    ULONG_PTR g_gdiplusToken = 0;

    int g_activeTab = 0;
    SubPage g_subPage = SubPage::None;

    int g_hoveredTab = -1;
    int g_hoveredRow = -1;
    int g_hoveredStepperBtn = 0;
    bool g_hoveredGear = false;
    bool g_hoveredToggle = false;
    bool g_hoveredBack = false;
    bool g_hoveredClose = false;
    bool g_trackingMouse = false;
    bool g_draggingColorHue = false;
    float g_dragHue = -1.0f;
    float g_backGlow = 0.0f;
    float g_closeGlow = 0.0f;
    float g_gearGlow[16]{};
    float g_tabGlow[3] = { 0.0f, 0.0f, 0.0f };
    constexpr UINT_PTR kHoverAnimationTimer = 7;

    HICON g_hAppIcon = nullptr;

    struct FontCache {
        std::unique_ptr<FontFamily> segoeUI;
        std::unique_ptr<FontFamily> fluentIcons;
        std::unique_ptr<FontFamily> mdl2Icons;

        FontCache() {
            segoeUI = std::make_unique<FontFamily>(L"Segoe UI");
            fluentIcons = std::make_unique<FontFamily>(L"Segoe Fluent Icons");
            if (fluentIcons->GetLastStatus() != Ok) {
                mdl2Icons = std::make_unique<FontFamily>(L"Segoe MDL2 Assets");
            }
        }
    };
    std::unique_ptr<FontCache> g_fonts;

    struct DoubleBuffer {
        HDC hdc;
        HDC memDC;
        HBITMAP memBM;
        HGDIOBJ oldBM;
        int width, height;

        DoubleBuffer(HWND hWnd, HDC targetDC) : hdc(targetDC) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            width = rc.right - rc.left;
            height = rc.bottom - rc.top;

            memDC = CreateCompatibleDC(hdc);
            memBM = CreateCompatibleBitmap(hdc, width, height);
            oldBM = SelectObject(memDC, memBM);
        }

        ~DoubleBuffer() {
            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);
        }
    };
}

static float GetDpiScale(HWND hWnd) {
    if (!hWnd) return 1.0f;
    UINT dpi = GetDpiForWindow(hWnd);
    return (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
}

static void DrawPill(Graphics& g, const Brush& brush, float x, float y, float w, float h) {
    float r = h / 2.0f;
    GraphicsPath path;
    path.AddArc(x, y, 2 * r, 2 * r, 90, 180);
    path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 180);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static void DrawNativeToggle(Graphics& g, float x, float y, float scale, bool checked, bool hovered, bool disabled = false) {
    float w = 36.0f * scale;
    float h = 18.0f * scale;
    float thumbR = 6.0f * scale;

    if (disabled) {
        SolidBrush trackBrush(Color(255, 38, 38, 38));
        DrawPill(g, trackBrush, x, y, w, h);
        SolidBrush thumbBrush(Color(255, 75, 75, 75));
        g.FillEllipse(&thumbBrush, x + 3.0f * scale, y + (h - thumbR * 2) / 2.0f, thumbR * 2, thumbR * 2);
        return;
    }

    if (checked) {
        SolidBrush trackBrush(hovered ? Color(255, 35, 200, 255) : Color(255, 0, 180, 255));
        DrawPill(g, trackBrush, x, y, w, h);
        SolidBrush thumbBrush(Color(255, 255, 255, 255));
        g.FillEllipse(&thumbBrush, x + w - thumbR * 2 - 3.0f * scale, y + (h - thumbR * 2) / 2.0f, thumbR * 2, thumbR * 2);
    } else {
        SolidBrush trackBrush(hovered ? Color(255, 48, 48, 48) : Color(255, 36, 36, 36));
        DrawPill(g, trackBrush, x, y, w, h);

        Pen borderPen(hovered ? Color(255, 130, 130, 130) : Color(255, 80, 80, 80), 1.0f);
        const float inset = 0.5f;
        float r = (h - 1.0f) / 2.0f;
        GraphicsPath path;
        path.AddArc(x + inset, y + inset, 2 * r, 2 * r, 90, 180);
        path.AddArc(x + w - inset - 2 * r, y + inset, 2 * r, 2 * r, 270, 180);
        path.CloseFigure();
        g.DrawPath(&borderPen, &path);

        SolidBrush thumbBrush(hovered ? Color(255, 220, 220, 220) : Color(255, 170, 170, 170));
        g.FillEllipse(&thumbBrush, x + 3.0f * scale, y + (h - thumbR * 2) / 2.0f, thumbR * 2, thumbR * 2);
    }
}

static void DrawSymbolGlyph(Graphics& g, wchar_t glyph, const RectF& rect, const Brush& brush, float size) {
    if (!g_fonts) return;

    TextRenderingHint oldHint = g.GetTextRenderingHint();
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    StringFormat centerFmt;
    centerFmt.SetAlignment(StringAlignmentCenter);
    centerFmt.SetLineAlignment(StringAlignmentCenter);

    if (g_fonts->fluentIcons && g_fonts->fluentIcons->GetLastStatus() == Ok) {
        Font font(g_fonts->fluentIcons.get(), size, FontStyleRegular, UnitPixel);
        g.DrawString(&glyph, 1, &font, rect, &centerFmt, &brush);
    } else if (g_fonts->mdl2Icons) {
        Font font(g_fonts->mdl2Icons.get(), size, FontStyleRegular, UnitPixel);
        g.DrawString(&glyph, 1, &font, rect, &centerFmt, &brush);
    }

    g.SetTextRenderingHint(oldHint);
}

static BYTE BlendByte(BYTE from, BYTE to, float amount) {
    return static_cast<BYTE>(from + (to - from) * std::clamp(amount, 0.0f, 1.0f));
}

static void DrawGearIcon(Graphics& g, float x, float y, float w, float h, float scale, float glow) {
    if (glow > 0.01f) {
        SolidBrush halo(Color(static_cast<BYTE>(42.0f * glow), 0, 210, 255));
        const float d = 0.8f * scale;
        DrawSymbolGlyph(g, 0xE713, RectF(x - d, y, w, h), halo, 12.0f * scale);
        DrawSymbolGlyph(g, 0xE713, RectF(x + d, y, w, h), halo, 12.0f * scale);
        DrawSymbolGlyph(g, 0xE713, RectF(x, y - d, w, h), halo, 12.0f * scale);
        DrawSymbolGlyph(g, 0xE713, RectF(x, y + d, w, h), halo, 12.0f * scale);
    }
    SolidBrush iconBrush(Color(255, BlendByte(160, 0, glow), BlendByte(160, 210, glow), BlendByte(160, 255, glow)));
    DrawSymbolGlyph(g, 0xE713, RectF(x, y, w, h), iconBrush, 12.0f * scale);
}

static void DrawNativeStepper(Graphics& g, float x, float y, float scale, int value, const std::wstring& unit, int hoveredBtn, const std::wstring& displayOverride = L"", bool disabled = false) {
    float w = 92.0f * scale;
    float h = 22.0f * scale;
    float btnW = 20.0f * scale;

    SolidBrush bgBrush(disabled ? Color(255, 30, 30, 30) : Color(255, 38, 38, 38));
    DrawPill(g, bgBrush, x, y, w, h);

    Pen borderPen(disabled ? Color(255, 45, 45, 45) : Color(255, 60, 60, 60), 1.0f);
    float r = h / 2.0f;
    GraphicsPath path;
    path.AddArc(x, y, 2 * r, 2 * r, 90, 180);
    path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 180);
    path.CloseFigure();

    g.TranslateTransform(0.5f, 0.5f);
    g.DrawPath(&borderPen, &path);
    g.TranslateTransform(-0.5f, -0.5f);

    if (!disabled) {
        SolidBrush hoverBg(Color(255, 30, 48, 56));
        if (hoveredBtn == 1) {
            GraphicsPath minusPath;
            minusPath.AddArc(x, y, 2 * r, 2 * r, 90, 180);
            minusPath.AddLine(x + r, y, x + btnW, y);
            minusPath.AddLine(x + btnW, y + h, x + r, y + h);
            minusPath.CloseFigure();
            g.FillPath(&hoverBg, &minusPath);
        } else if (hoveredBtn == 2) {
            GraphicsPath plusPath;
            plusPath.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 180);
            plusPath.AddLine(x + w - r, y + h, x + w - btnW, y + h);
            plusPath.AddLine(x + w - btnW, y, x + w - r, y);
            plusPath.CloseFigure();
            g.FillPath(&hoverBg, &plusPath);
        }
    }

    Font btnFont(g_fonts->segoeUI.get(), 11.5f * scale, FontStyleBold, UnitPixel);
    Font valFont(g_fonts->segoeUI.get(), 11.0f * scale, FontStyleRegular, UnitPixel);

    StringFormat centerFmt;
    centerFmt.SetAlignment(StringAlignmentCenter);
    centerFmt.SetLineAlignment(StringAlignmentCenter);
    centerFmt.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush textBrush(disabled ? Color(255, 110, 110, 110) : Color(255, 230, 230, 230));
    SolidBrush btnBrush(disabled ? Color(255, 75, 75, 75) : (hoveredBtn != 0 ? Color(255, 0, 210, 255) : Color(255, 180, 180, 180)));

    g.DrawString(L"-", -1, &btnFont, RectF(x, y, btnW, h), &centerFmt, &btnBrush);
    std::wstring valStr = displayOverride.empty() ? (std::to_wstring(value) + unit) : displayOverride;
    g.DrawString(valStr.c_str(), -1, &valFont, RectF(x + btnW, y, w - 2 * btnW, h), &centerFmt, &textBrush);
    g.DrawString(L"+", -1, &btnFont, RectF(x + w - btnW, y, btnW, h), &centerFmt, &btnBrush);
}

static void DrawSegmentedCombo(Graphics& g, float x, float y, float scale, int selectedIndex,
                               const wchar_t* left = L"Auto", const wchar_t* right = L"Custom") {
    float w = 110.0f * scale;
    float h = 22.0f * scale;
    float segW = w / 2.0f;

    SolidBrush bgBrush(Color(255, 34, 34, 34));
    DrawPill(g, bgBrush, x, y, w, h);

    Pen borderPen(Color(255, 60, 60, 60), 1.0f);
    float r = h / 2.0f;
    GraphicsPath path;
    path.AddArc(x, y, 2 * r, 2 * r, 90, 180);
    path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 180);
    path.CloseFigure();

    g.TranslateTransform(0.5f, 0.5f);
    g.DrawPath(&borderPen, &path);
    g.TranslateTransform(-0.5f, -0.5f);

    float activeX = (selectedIndex == 0) ? (x + 1.5f * scale) : (x + segW);
    SolidBrush activeBrush(Color(255, 0, 180, 255));
    DrawPill(g, activeBrush, activeX, y + 1.5f * scale, segW - 1.5f * scale, h - 3.0f * scale);

    Font font(g_fonts->segoeUI.get(), 10.5f * scale, FontStyleRegular, UnitPixel);
    Font activeFont(g_fonts->segoeUI.get(), 10.5f * scale, FontStyleBold, UnitPixel);

    StringFormat centerFmt;
    centerFmt.SetAlignment(StringAlignmentCenter);
    centerFmt.SetLineAlignment(StringAlignmentCenter);

    SolidBrush activeText(Color(255, 255, 255, 255));
    SolidBrush inactiveText(Color(255, 170, 170, 170));

    g.DrawString(left, -1, (selectedIndex == 0) ? &activeFont : &font, RectF(x, y, segW, h), &centerFmt, (selectedIndex == 0) ? &activeText : &inactiveText);
    g.DrawString(right, -1, (selectedIndex == 1) ? &activeFont : &font, RectF(x + segW, y, segW, h), &centerFmt, (selectedIndex == 1) ? &activeText : &inactiveText);
}

static void DrawValueSlider(Graphics& g, float x, float y, float w, float h, float scale,
                            int value, int minValue, int maxValue) {
    const float trackY = y + h * 0.5f - 2.0f * scale;
    const float pct = std::clamp((value - minValue) / static_cast<float>(maxValue - minValue), 0.0f, 1.0f);
    SolidBrush track(Color(255, 48, 48, 48)), fill(Color(255, 0, 180, 255)), thumb(Color(255, 245, 245, 245));
    DrawPill(g, track, x, trackY, w, 4.0f * scale);
    DrawPill(g, fill, x, trackY, std::max(4.0f * scale, w * pct), 4.0f * scale);
    g.FillEllipse(&thumb, x + w * pct - 5.0f * scale, trackY - 3.0f * scale, 10.0f * scale, 10.0f * scale);
}

static Gdiplus::Color HueToGdiColor(float hue) {
    if (hue < 0.0f) hue = 0.0f;
    float h = std::fmod(hue, 360.0f) / 60.0f;
    float c = 1.0f;
    float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 1.0f) { r = c; g = x; }
    else if (h < 2.0f) { r = x; g = c; }
    else if (h < 3.0f) { g = c; b = x; }
    else if (h < 4.0f) { g = x; b = c; }
    else if (h < 5.0f) { r = x; b = c; }
    else { r = c; b = x; }
    return Gdiplus::Color(255, static_cast<BYTE>(r * 255.0f), static_cast<BYTE>(g * 255.0f), static_cast<BYTE>(b * 255.0f));
}

static void DrawColorPicker(Graphics& g, float x, float y, float w, float h, float scale, int hue) {
    Gdiplus::GraphicsState gs = g.Save();
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    float trackW = w;
    float trackH = 12.0f * scale;
    float trackX = x;
    float trackY = y + (h - trackH) / 2.0f;
    float tr = trackH / 2.0f;

    if (trackW <= 0.0f || trackH <= 0.0f) { g.Restore(gs); return; }

    Color rainbowColors[] = {
        Color(255, 255, 0, 0),
        Color(255, 255, 255, 0),
        Color(255, 0, 255, 0),
        Color(255, 0, 255, 255),
        Color(255, 0, 0, 255),
        Color(255, 255, 0, 255),
        Color(255, 255, 0, 0)
    };
    REAL rainbowPositions[] = {
        0.0f, 1.0f / 6.0f, 2.0f / 6.0f, 3.0f / 6.0f, 4.0f / 6.0f, 5.0f / 6.0f, 1.0f
    };

    LinearGradientBrush rainbowBrush(PointF(trackX + tr, trackY), PointF(trackX + trackW - tr, trackY), Color(255, 255, 0, 0), Color(255, 255, 0, 0));
    rainbowBrush.SetWrapMode(WrapModeClamp);
    rainbowBrush.SetInterpolationColors(rainbowColors, rainbowPositions, 7);

    GraphicsPath trackPath;
    trackPath.AddArc(trackX, trackY, 2 * tr, 2 * tr, 90, 180);
    trackPath.AddArc(trackX + trackW - 2 * tr, trackY, 2 * tr, 2 * tr, 270, 180);
    trackPath.CloseFigure();

    g.FillPath(&rainbowBrush, &trackPath);

    Pen trackBorderPen(Color(55, 0, 0, 0), 1.0f);
    g.DrawPath(&trackBorderPen, &trackPath);

    float effectiveHue = (g_draggingColorHue && g_dragHue >= 0.0f)
        ? g_dragHue
        : (hue < 0 ? 210.0f : static_cast<float>(hue));
    float normHue = std::clamp(effectiveHue / 360.0f, 0.0f, 1.0f);

    float activeW = trackW - 2 * tr;
    float thumbX = trackX + tr + normHue * activeW;
    float thumbY = trackY + tr;
    float thumbR = (g_draggingColorHue ? 9.5f : 8.5f) * scale;

    Color currentColor = HueToGdiColor(effectiveHue);

    if (g_draggingColorHue) {
        SolidBrush haloBrush(Color(50, currentColor.GetR(), currentColor.GetG(), currentColor.GetB()));
        float hr = thumbR + 3.0f * scale;
        g.FillEllipse(&haloBrush, thumbX - hr, thumbY - hr, hr * 2.0f, hr * 2.0f);
    }

    for (int i = 3; i >= 1; --i) {
        float rShadow = thumbR + i * 1.0f * scale;
        BYTE shadowAlpha = static_cast<BYTE>(12 * (4 - i));
        SolidBrush shadowBrush(Color(shadowAlpha, 0, 0, 0));
        g.FillEllipse(&shadowBrush, thumbX - rShadow, thumbY - rShadow + 1.0f * scale, rShadow * 2.0f, rShadow * 2.0f);
    }

    SolidBrush thumbOuterBrush(Color(255, 255, 255, 255));
    g.FillEllipse(&thumbOuterBrush, thumbX - thumbR, thumbY - thumbR, thumbR * 2.0f, thumbR * 2.0f);

    float innerR = thumbR - 2.5f * scale;
    SolidBrush thumbInnerBrush(currentColor);
    g.FillEllipse(&thumbInnerBrush, thumbX - innerR, thumbY - innerR, innerR * 2.0f, innerR * 2.0f);

    Pen innerBorderPen(Color(30, 0, 0, 0), 1.0f);
    g.DrawEllipse(&innerBorderPen, thumbX - innerR, thumbY - innerR, innerR * 2.0f, innerR * 2.0f);

    Pen thumbBorderPen(Color(55, 0, 0, 0), 1.0f);
    g.DrawEllipse(&thumbBorderPen, thumbX - thumbR, thumbY - thumbR, thumbR * 2.0f, thumbR * 2.0f);

    g.Restore(gs);
}

static std::vector<RowItem> GetCurrentItems() {
    std::vector<RowItem> items;
    if (!g_pConfig) return items;

    if (g_subPage == SubPage::DockSettings) {
        items.push_back({ L"Sizing & physics", RowType::Segmented, false, SubPage::None, nullptr, nullptr, 0, 0, 0, L"", L"", g_pConfig->dockAutoPhysics ? 0 : 1 });

        auto addStepper = [&](const wchar_t* title, int* val, int minV, int maxV, const wchar_t* unit) {
            RowItem r = { title, RowType::Stepper, false, SubPage::None, nullptr, val, minV, maxV, 5, unit };
            if (g_pConfig->dockAutoPhysics) { r.displayOverride = L"Auto"; r.disabled = true; }
            items.push_back(r);
        };

        addStepper(L"Zoom scale", &g_pConfig->dockMaxScale, 110, 180, L"%");
        addStepper(L"Wave radius", &g_pConfig->dockRadius, 30, 200, L"px");
        addStepper(L"Neighbor spacing", &g_pConfig->dockSpacing, 20, 80, L"%");

        items.push_back({ L"Icon click bounce", RowType::Toggle, false, SubPage::None, &g_pConfig->dockBounce });
        items.push_back({ L"Keep Start button fixed", RowType::Toggle, false, SubPage::None, &g_pConfig->dockExcludeSystem });
    } else if (g_subPage == SubPage::TrayItems) {
        items.push_back({ L"Hide overflow arrow", RowType::Toggle, false, SubPage::None, &g_pConfig->hideTrayChevron });
        items.push_back({ L"Hide language", RowType::Toggle, false, SubPage::None, &g_pConfig->hideTrayLanguage });
        items.push_back({ L"Hide network", RowType::Toggle, false, SubPage::None, &g_pConfig->hideTrayNetwork });
        items.push_back({ L"Hide volume", RowType::Toggle, false, SubPage::None, &g_pConfig->hideTrayVolume });
        items.push_back({ L"Hide battery", RowType::Toggle, false, SubPage::None, &g_pConfig->hideTrayBattery });
        items.push_back({ L"Hide clock", RowType::Toggle, false, SubPage::None, &g_pConfig->hideTrayClock });
    } else if (g_subPage == SubPage::WindowSettings) {
        items.push_back({ L"Animation duration", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->windowAnimationDuration, 150, 1000, 50, L"ms" });
        items.push_back({ L"Curve intensity", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->windowCurveIntensity, 50, 125, 5, L"%" });
        items.push_back({ L"Target width", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->windowTargetWidth, 12, 64, 2, L"px" });
        items.push_back({ L"Repeat guard", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->windowRepeatGuard, 0, 500, 20, L"ms" });
        items.push_back({ L"Capture delay", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->windowCaptureDelay, 50, 500, 25, L"ms" });
    } else if (g_subPage == SubPage::StartMenuHub) {
        items.push_back({ L"Start Menu Size", RowType::Toggle, true, SubPage::StartMenuSettings, &g_pConfig->startMenuSizing });
        items.push_back({ L"Hide Elements", RowType::Navigation, true, SubPage::StartMenuHideElements });
    } else if (g_subPage == SubPage::StartMenuHideElements) {
        items.push_back({ L"Hide Search box", RowType::Toggle, false, SubPage::None, &g_pConfig->startHideSearch });
        items.push_back({ L"Hide Pinned apps", RowType::Toggle, false, SubPage::None, &g_pConfig->startHidePinned });
        items.push_back({ L"Hide Recommended", RowType::Toggle, false, SubPage::None, &g_pConfig->startHideRecommended });
        items.push_back({ L"Hide User profile", RowType::Toggle, false, SubPage::None, &g_pConfig->startHideProfile });
        items.push_back({ L"Hide Power button", RowType::Toggle, false, SubPage::None, &g_pConfig->startHidePower });
        items.push_back({ L"Hide View selector", RowType::Toggle, false, SubPage::None, &g_pConfig->startHideViewSelector });
        items.push_back({ L"Hide Folders & icons", RowType::Toggle, false, SubPage::None, &g_pConfig->startHideFolders });
    } else if (g_subPage == SubPage::StartMenuSettings) {
        RowItem mode{ L"Mode", RowType::Segmented, false, SubPage::None, &g_pConfig->startMenuAdvanced };
        mode.selectedIndex = g_pConfig->startMenuAdvanced ? 1 : 0;
        mode.segmentLeft = L"Simple"; mode.segmentRight = L"Advanced"; items.push_back(mode);
        if (!g_pConfig->startMenuAdvanced) {
            items.push_back({ L"Overall size", RowType::Slider, false, SubPage::None, nullptr, &g_pConfig->startMenuScale, 75, 150, 5, L"%" });
        } else {
            items.push_back({ L"Start menu width", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->startMenuWidth, 320, 1400, 20, L"px" });
            items.push_back({ L"Start menu height", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->startMenuHeight, 400, 1200, 20, L"px" });
            items.push_back({ L"Search width", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->searchWidth, 320, 1400, 20, L"px" });
            items.push_back({ L"Search height", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->searchHeight, 400, 1200, 20, L"px" });
        }
    } else if (g_subPage == SubPage::StartButtonSettings) {
        items.push_back({ L"Move Start button", RowType::Toggle, false, SubPage::None, &g_pConfig->layoutEditor });
        items.push_back({ L"Icon scale", RowType::Stepper, false, SubPage::None, nullptr, &g_pConfig->startIconSize, 50, 180, 5, L"%" });
        items.push_back({ L"Start Color", RowType::Toggle, false, SubPage::None, &g_pConfig->startIconCustom });
        if (g_pConfig->startIconCustom) {
            items.push_back({ L"Color", RowType::ColorPicker, false, SubPage::None, nullptr, &g_pConfig->startIconColorHue, 0, 359, 1 });
        }
    } else {
        if (g_activeTab == 0) {
            items.push_back({ L"Clear taskbar", RowType::Toggle, false, SubPage::None, &g_pConfig->clearTaskbar });
            items.push_back({ L"Dock animation", RowType::Toggle, true, SubPage::DockSettings, &g_pConfig->dockAnimation });
            items.push_back({ L"Tray icons", RowType::Toggle, true, SubPage::TrayItems, &g_pConfig->trayItems });
            items.push_back({ L"Drag & drop", RowType::Toggle, false, SubPage::None, &g_pConfig->dragDropAssist });
            items.push_back({ L"Start Menu", RowType::Navigation, true, SubPage::StartMenuHub });
            items.push_back({ L"Start Button", RowType::Navigation, true, SubPage::StartButtonSettings });
        } else if (g_activeTab == 1) {
            items.push_back({ L"Minimize window animation (beta)", RowType::Toggle, true, SubPage::WindowSettings, &g_pConfig->windowGenieAnimation });
            items.push_back({ L"Remove window borders", RowType::Toggle, false, SubPage::None, &g_pConfig->hideWindowBorders });
        } else {
            items.push_back({ L"Show tray icon", RowType::Toggle, false, SubPage::None, &g_pConfig->showTrayIcon });
            items.push_back({ L"Launch at startup", RowType::Toggle, false, SubPage::None, &g_pConfig->autostart });
            items.push_back({ L"Check for updates automatically", RowType::Toggle, false, SubPage::None, &g_pConfig->automaticUpdates });
            items.push_back({ L"Updates", RowType::Action });
        }
    }
    return items;
}

static void PaintWindow(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    {
        DoubleBuffer buffer(hWnd, hdc);
        Graphics g(buffer.memDC);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        float scale = GetDpiScale(hWnd);

        SolidBrush bgBrush(Color(255, 24, 26, 28));
        g.FillRectangle(&bgBrush, 0, 0, buffer.width, buffer.height);

        Font captionFont(g_fonts->segoeUI.get(), 12.5f * scale, FontStyleBold, UnitPixel);
        Font tabFont(g_fonts->segoeUI.get(), 11.5f * scale, FontStyleRegular, UnitPixel);
        Font tabActiveFont(g_fonts->segoeUI.get(), 11.5f * scale, FontStyleBold, UnitPixel);
        Font titleFont(g_fonts->segoeUI.get(), 12.5f * scale, FontStyleRegular, UnitPixel);

        StringFormat centerFmt, vCenterFmt;
        centerFmt.SetAlignment(StringAlignmentCenter);
        centerFmt.SetLineAlignment(StringAlignmentCenter);
        vCenterFmt.SetLineAlignment(StringAlignmentCenter);

        if (g_subPage != SubPage::None) {
            if (g_backGlow > 0.01f) {
                Pen halo(Color(static_cast<BYTE>(55.0f * g_backGlow), 0, 210, 255), 3.5f * scale);
                halo.SetStartCap(LineCapRound); halo.SetEndCap(LineCapRound);
                const float cx = 20.0f * scale, cy = 15.0f * scale;
                g.DrawLine(&halo, cx - 4.0f * scale, cy, cx + 4.0f * scale, cy);
            }
            Pen backPen(Color(255, BlendByte(180, 0, g_backGlow), BlendByte(180, 210, g_backGlow),
                BlendByte(180, 255, g_backGlow)), 1.25f * scale);
            backPen.SetStartCap(LineCapRound);
            backPen.SetEndCap(LineCapRound);
            const float backCx = 20.0f * scale;
            const float backCy = 15.0f * scale;
            g.DrawLine(&backPen, backCx - 4.0f * scale, backCy,
                backCx + 4.0f * scale, backCy);
            g.DrawLine(&backPen, backCx - 4.0f * scale, backCy,
                backCx - 1.0f * scale, backCy - 3.0f * scale);
            g.DrawLine(&backPen, backCx - 4.0f * scale, backCy,
                backCx - 1.0f * scale, backCy + 3.0f * scale);

            SolidBrush titleBrush(Color(255, 240, 240, 240));
            const wchar_t* subPageTitle = g_subPage == SubPage::DockSettings
                ? L"Dock Animation Settings" : (g_subPage == SubPage::TrayItems
                ? L"Tray Items" : (g_subPage == SubPage::StartMenuHub
                ? L"Start Menu" : (g_subPage == SubPage::StartMenuSettings
                ? L"Start Menu Size" : (g_subPage == SubPage::StartMenuHideElements
                ? L"Hide Elements" : (g_subPage == SubPage::StartButtonSettings
                ? L"Start Button Settings" : L"Window Animation Settings")))));
            g.DrawString(subPageTitle, -1, &captionFont, PointF(36.0f * scale, 8.0f * scale), &titleBrush);
        } else {
            if (g_hAppIcon) {
                int iconSz = static_cast<int>(18.0f * scale);
                DrawIconEx(buffer.memDC, static_cast<int>(12.0f * scale), static_cast<int>(8.0f * scale), g_hAppIcon, iconSz, iconSz, 0, nullptr, DI_NORMAL);
            }

            SolidBrush titleBrush(Color(255, 240, 240, 240));
            g.DrawString(L"Lightency", -1, &captionFont, PointF(34.0f * scale, 8.0f * scale), &titleBrush);

            float tabY = 31.0f * scale;
            float tabW = 64.0f * scale;
            float tabStartX = 12.0f * scale;
            const wchar_t* tabs[] = { L"Taskbar", L"Window", L"Settings" };

            for (int i = 0; i < 3; ++i) {
                float tx = tabStartX + i * tabW;
                bool isActive = (g_activeTab == i);
                const float tabGlow = std::max(g_tabGlow[i], isActive ? 0.85f : 0.0f);

                if (isActive) {
                    SolidBrush activeHalo(Color(static_cast<BYTE>(28.0f * tabGlow), 0, 210, 255));
                    const float d = 0.55f * scale;
                    g.DrawString(tabs[i], -1, &tabActiveFont, RectF(tx - d, tabY, tabW, 24.0f * scale), &centerFmt, &activeHalo);
                    g.DrawString(tabs[i], -1, &tabActiveFont, RectF(tx + d, tabY, tabW, 24.0f * scale), &centerFmt, &activeHalo);
                    SolidBrush activeText(Color(255, 0, 210, 255));
                    g.DrawString(tabs[i], -1, &tabActiveFont, RectF(tx, tabY, tabW, 24.0f * scale), &centerFmt, &activeText);
                } else {
                    if (tabGlow > 0.01f) {
                        SolidBrush halo(Color(static_cast<BYTE>(38.0f * tabGlow), 0, 210, 255));
                        const float d = 0.7f * scale;
                        g.DrawString(tabs[i], -1, &tabFont, RectF(tx - d, tabY, tabW, 24.0f * scale), &centerFmt, &halo);
                        g.DrawString(tabs[i], -1, &tabFont, RectF(tx + d, tabY, tabW, 24.0f * scale), &centerFmt, &halo);
                    }
                    SolidBrush inactiveText(Color(255, BlendByte(150, 0, tabGlow),
                        BlendByte(150, 210, tabGlow), BlendByte(150, 255, tabGlow)));
                    g.DrawString(tabs[i], -1, &tabFont, RectF(tx, tabY, tabW, 24.0f * scale), &centerFmt, &inactiveText);
                }
            }
        }

        Pen closePen(Color(255, BlendByte(180, 235, g_closeGlow), BlendByte(180, 75, g_closeGlow),
            BlendByte(180, 65, g_closeGlow)), 1.0f);
        int cx = static_cast<int>(buffer.width - 20.0f * scale);
        int cy = static_cast<int>(15.0f * scale);
        int csz = static_cast<int>(4.0f * scale);

        if (g_closeGlow > 0.01f) {
            Pen closeHalo(Color(static_cast<BYTE>(58.0f * g_closeGlow), 235, 75, 65), 3.0f * scale);
            closeHalo.SetStartCap(LineCapRound); closeHalo.SetEndCap(LineCapRound);
            g.DrawLine(&closeHalo, cx - csz, cy - csz, cx + csz, cy + csz);
            g.DrawLine(&closeHalo, cx + csz, cy - csz, cx - csz, cy + csz);
        }
        g.TranslateTransform(0.5f, 0.5f);
        g.DrawLine(&closePen, cx - csz, cy - csz, cx + csz, cy + csz);
        g.DrawLine(&closePen, cx + csz, cy - csz, cx - csz, cy + csz);
        g.TranslateTransform(-0.5f, -0.5f);

        float startY = (g_subPage != SubPage::None ? kSubPageHeaderHeight : kMainHeaderHeight) * scale;
        float padX = kSidePadding * scale;
        float rowH = kRowHeight * scale;
        auto items = GetCurrentItems();

        for (size_t i = 0; i < items.size(); ++i) {
            float ry = startY + i * rowH;
            bool isHovered = (g_hoveredRow == static_cast<int>(i));

            if (isHovered) {
                SolidBrush hoverBg(Color(255, 32, 38, 44));
                DrawPill(g, hoverBg, 7.0f * scale, ry + 2.0f * scale, static_cast<float>(buffer.width - 14.0f * scale), rowH - 4.0f * scale);
            }

            SolidBrush textBrush(items[i].disabled ? Color(255, 120, 120, 120) : Color(255, 235, 235, 235));
            float controlLeft = buffer.width - padX - 38.0f * scale;
            if (items[i].type == RowType::Stepper) controlLeft = buffer.width - padX - 94.0f * scale;
            else if (items[i].type == RowType::Action) controlLeft = buffer.width - padX - 78.0f * scale;
            else if (items[i].type == RowType::Segmented) controlLeft = buffer.width - padX - 110.0f * scale;
            else if (items[i].type == RowType::ColorPicker || items[i].type == RowType::Slider) controlLeft = buffer.width - padX - 144.0f * scale;
            else if (items[i].type == RowType::Navigation) controlLeft = buffer.width - padX - 28.0f * scale;
            else if (items[i].hasGear) controlLeft = buffer.width - padX - 68.0f * scale;
            float textX = padX + 2.0f * scale;
            g.DrawString(items[i].title.c_str(), -1, &titleFont,
                         RectF(textX, ry, std::max(0.0f, controlLeft - textX - 10.0f * scale), rowH),
                         &vCenterFmt, &textBrush);

            if (items[i].type == RowType::Toggle && items[i].pBoolValue) {
                DrawNativeToggle(g, buffer.width - padX - 38.0f * scale, ry + (rowH - 18.0f * scale) / 2.0f, scale, *(items[i].pBoolValue), isHovered && g_hoveredToggle, items[i].disabled);
                if (items[i].hasGear) {
                    DrawGearIcon(g, buffer.width - padX - 68.0f * scale, ry + (rowH - 24.0f * scale) / 2.0f, 24.0f * scale, 24.0f * scale, scale,
                        i < ARRAYSIZE(g_gearGlow) ? g_gearGlow[i] : 0.0f);
                }
            } else if (items[i].type == RowType::Navigation) {
                Font chevronFont(g_fonts->segoeUI.get(), 17.0f * scale, FontStyleRegular, UnitPixel);
                SolidBrush chevronBrush(Color(255, 170, 180, 188));
                g.DrawString(L"›", -1, &chevronFont,
                    RectF(buffer.width - padX - 38.0f * scale, ry, 36.0f * scale, rowH),
                    &centerFmt, &chevronBrush);
            } else if (items[i].type == RowType::Stepper && items[i].pIntValue) {
                DrawNativeStepper(g, buffer.width - padX - 94.0f * scale, ry + (rowH - 22.0f * scale) / 2.0f, scale, *(items[i].pIntValue), items[i].unit, isHovered ? g_hoveredStepperBtn : 0, items[i].displayOverride, items[i].disabled);
            } else if (items[i].type == RowType::Segmented) {
                DrawSegmentedCombo(g, buffer.width - padX - 110.0f * scale, ry + (rowH - 22.0f * scale) / 2.0f, scale, items[i].selectedIndex,
                    items[i].segmentLeft.empty() ? L"Auto" : items[i].segmentLeft.c_str(), items[i].segmentRight.empty() ? L"Custom" : items[i].segmentRight.c_str());
            } else if (items[i].type == RowType::ColorPicker && items[i].pIntValue) {
                float pickerW = 140.0f * scale;
                float px = buffer.width - padX - pickerW;
                DrawColorPicker(g, px, ry, pickerW, rowH, scale, *(items[i].pIntValue));
            } else if (items[i].type == RowType::Slider && items[i].pIntValue) {
                float sliderW = 140.0f * scale;
                DrawValueSlider(g, buffer.width - padX - sliderW, ry, sliderW, rowH, scale,
                    *items[i].pIntValue, items[i].minInt, items[i].maxInt);
            } else if (items[i].type == RowType::Action) {
                float bx = buffer.width - padX - 76.0f * scale;
                float by = ry + (rowH - 22.0f * scale) / 2.0f;
                SolidBrush buttonBg(isHovered ? Color(255, 0, 195, 255) : Color(255, 0, 180, 255));
                DrawPill(g, buttonBg, bx, by, 76.0f * scale, 22.0f * scale);
                Font buttonFont(g_fonts->segoeUI.get(), 10.5f * scale, FontStyleBold, UnitPixel);
                SolidBrush buttonText(Color(255, 255, 255, 255));
                const wchar_t* btnLabel = items[i].displayOverride.empty() ? L"Check now" : items[i].displayOverride.c_str();
                g.DrawString(btnLabel, -1, &buttonFont, RectF(bx, by, 76.0f * scale, 22.0f * scale), &centerFmt, &buttonText);
            }
        }
    }

    EndPaint(hWnd, &ps);
}

static void ResolveHitTest(int x, int y, int width, float scale,
                           bool& outClose, bool& outBack, int& outTab, int& outRow,
                           int& outStepperBtn, bool& outGear, bool& outToggle) {
    outClose = (x >= width - static_cast<int>(36.0f * scale) && y >= 0 && y <= static_cast<int>(28.0f * scale));
    outBack = (g_subPage != SubPage::None && x >= static_cast<int>(6.0f * scale) && x <= static_cast<int>(36.0f * scale) && y >= 0 && y <= static_cast<int>(28.0f * scale));
    outTab = -1; outRow = -1; outStepperBtn = 0; outGear = false; outToggle = false;

    if (g_subPage == SubPage::None && y >= 31.0f * scale && y <= 55.0f * scale) {
        float tabW = 64.0f * scale;
        float tabStartX = 12.0f * scale;
        for (int i = 0; i < 3; ++i) {
            float tx = tabStartX + i * tabW;
            if (x >= tx && x <= tx + tabW) outTab = i;
        }
    }

    float startY = (g_subPage != SubPage::None ? kSubPageHeaderHeight : kMainHeaderHeight) * scale;
    if (y >= startY && x >= 6.0f * scale && x <= width - 6.0f * scale) {
        float rowH = kRowHeight * scale;
        int rowIndex = static_cast<int>((y - startY) / rowH);
        auto items = GetCurrentItems();

        if (rowIndex >= 0 && rowIndex < static_cast<int>(items.size())) {
            outRow = rowIndex;
            float padX = kSidePadding * scale;

            if (items[rowIndex].type == RowType::Navigation) {
                outGear = true;
            } else if (items[rowIndex].type == RowType::Stepper) {
                float sx = width - padX - 94.0f * scale;
                float btnW = 20.0f * scale;
                if (x >= sx && x <= sx + 92.0f * scale) {
                    if (x <= sx + btnW) outStepperBtn = 1;
                    else if (x >= sx + 92.0f * scale - btnW) outStepperBtn = 2;
                }
            } else if (items[rowIndex].type == RowType::Action) {
                float bx = width - padX - 76.0f * scale;
                if (x >= bx && x <= bx + 76.0f * scale) outToggle = true;
            } else if (items[rowIndex].type == RowType::Toggle) {
                float tx = width - padX - 38.0f * scale;
                if (x >= tx && x <= tx + 36.0f * scale) outToggle = true;
                if (items[rowIndex].hasGear) {
                    float gx = width - padX - 68.0f * scale;
                    if (x >= gx && x <= gx + 24.0f * scale) outGear = true;
                }
            } else if (items[rowIndex].type == RowType::ColorPicker || items[rowIndex].type == RowType::Slider) {
                float pickerW = 140.0f * scale;
                float px = width - padX - pickerW;
                if (x >= px - 6.0f * scale && x <= width - padX + 6.0f * scale) outToggle = true;
            }
        }
    }
}

static void ResizeWindowForContent(HWND hWnd) {
    auto items = GetCurrentItems();
    float scale = GetDpiScale(hWnd);
    float headerH = (g_subPage != SubPage::None) ? kSubPageHeaderHeight : kMainHeaderHeight;
    int contentH = static_cast<int>(std::lround((headerH + items.size() * kRowHeight + kBottomPadding) * scale));
    int contentW = static_cast<int>(std::lround(kWindowWidth * scale));
    SetWindowPos(hWnd, nullptr, 0, 0, contentW, contentH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK ModernWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    float scale = GetDpiScale(hWnd);

    switch (uMsg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        enum DWM_WINDOW_CORNER_PREFERENCE { DWMWCP_ROUND = 2 };
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
        COLORREF borderColor = RGB(45, 55, 65);
        DwmSetWindowAttribute(hWnd, 34, &borderColor, sizeof(borderColor));
        MARGINS margins = { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        return 0;
    }
    case WM_NCCALCSIZE: return (wParam) ? 0 : DefWindowProcW(hWnd, uMsg, wParam, lParam);
    case WM_NCACTIVATE: {
        COLORREF borderColor = RGB(45, 55, 65);
        DwmSetWindowAttribute(hWnd, 34, &borderColor, sizeof(borderColor));
        return TRUE;
    }
    case WM_NCPAINT: return 0;
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        RECT rc; GetClientRect(hWnd, &rc);

        bool isClose, isBack; int dummyTab, dummyRow, dummyBtn; bool dummyGear, dummyToggle;
        ResolveHitTest(pt.x, pt.y, rc.right - rc.left, scale, isClose, isBack, dummyTab, dummyRow, dummyBtn, dummyGear, dummyToggle);

        if (isClose || isBack || pt.y > static_cast<int>(28.0f * scale)) return HTCLIENT;
        return HTCAPTION;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintWindow(hWnd); return 0;
    case WM_TIMER: {
        if (wParam != kHoverAnimationTimer) break;
        bool moving = false;
        auto animate = [&moving](float& value, float target) {
            const float delta = target - value;
            if (std::abs(delta) < 0.015f) value = target;
            else { value += delta * 0.24f; moving = true; }
        };
        animate(g_backGlow, g_hoveredBack ? 1.0f : 0.0f);
        animate(g_closeGlow, g_hoveredClose ? 1.0f : 0.0f);
        for (size_t i = 0; i < ARRAYSIZE(g_gearGlow); ++i) {
            animate(g_gearGlow[i], g_hoveredGear && g_hoveredRow == static_cast<int>(i) ? 1.0f : 0.0f);
        }
        animate(g_tabGlow[0], g_hoveredTab == 0 ? 1.0f : 0.0f);
        animate(g_tabGlow[1], g_hoveredTab == 1 ? 1.0f : 0.0f);
        animate(g_tabGlow[2], g_hoveredTab == 2 ? 1.0f : 0.0f);
        InvalidateRect(hWnd, nullptr, FALSE);
        if (!moving) KillTimer(hWnd, kHoverAnimationTimer);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (g_draggingColorHue && (wParam & MK_LBUTTON)) {
            RECT rc; GetClientRect(hWnd, &rc);
            float padX = kSidePadding * scale;
            float pickerW = 140.0f * scale;
            float px = (rc.right - rc.left) - padX - pickerW;
            float trackH = 12.0f * scale;
            float tr = trackH / 2.0f;
            float activeW = pickerW - 2 * tr;
            float mx = static_cast<float>(GET_X_LPARAM(lParam));
            float pct = std::clamp((mx - (px + tr)) / activeW, 0.0f, 1.0f);
            g_dragHue = pct * 360.0f;
            if (g_dragHue >= 360.0f) g_dragHue = 359.9f;
            int newHue = static_cast<int>(g_dragHue);
            static ULONGLONG s_lastHueUpdateTick = 0;
            ULONGLONG now = GetTickCount64();
            if (g_pConfig && (g_pConfig->startIconColorHue != newHue || g_pConfig->startIconAccentColor)) {
                g_pConfig->startIconCustom = true;
                g_pConfig->startIconAccentColor = false;
                g_pConfig->startIconColorHue = newHue;
                if (now - s_lastHueUpdateTick >= 35) {
                    s_lastHueUpdateTick = now;
                    Injector::Update(*g_pConfig);
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        RECT rc; GetClientRect(hWnd, &rc);
        bool nClose, nBack, nGear, nToggle;
        int nTab, nRow, nBtn;

        ResolveHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), rc.right - rc.left, scale,
                       nClose, nBack, nTab, nRow, nBtn, nGear, nToggle);

        if (nClose != g_hoveredClose || nBack != g_hoveredBack || nTab != g_hoveredTab ||
            nRow != g_hoveredRow || nBtn != g_hoveredStepperBtn ||
            nGear != g_hoveredGear || nToggle != g_hoveredToggle) {

            g_hoveredClose = nClose; g_hoveredBack = nBack; g_hoveredTab = nTab;
            g_hoveredRow = nRow; g_hoveredStepperBtn = nBtn;
            g_hoveredGear = nGear; g_hoveredToggle = nToggle;
            SetTimer(hWnd, kHoverAnimationTimer, 16, nullptr);
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        if (!g_trackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_draggingColorHue) {
            g_draggingColorHue = false;
            g_dragHue = -1.0f;
            ReleaseCapture();
            if (g_pConfig) Injector::Update(*g_pConfig);
            if (g_onConfigChanged) g_onConfigChanged();
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_CAPTURECHANGED: {
        if (g_draggingColorHue) {
            g_draggingColorHue = false;
            g_dragHue = -1.0f;
            if (g_pConfig) Injector::Update(*g_pConfig);
            if (g_onConfigChanged) g_onConfigChanged();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        g_trackingMouse = false;
        g_hoveredClose = false; g_hoveredBack = false; g_hoveredTab = -1;
        g_hoveredRow = -1; g_hoveredStepperBtn = 0; g_hoveredGear = false; g_hoveredToggle = false;
        SetTimer(hWnd, kHoverAnimationTimer, 16, nullptr);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(hWnd, &rc);
        bool cClose, cBack, cGear, cToggle;
        int cTab, cRow, cBtn;

        ResolveHitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), rc.right - rc.left, scale,
                       cClose, cBack, cTab, cRow, cBtn, cGear, cToggle);

        if (cClose) { ShowWindow(hWnd, SW_HIDE); return 0; }
        if (cBack) {
            g_subPage = (g_subPage == SubPage::StartMenuSettings || g_subPage == SubPage::StartMenuHideElements)
                ? SubPage::StartMenuHub : SubPage::None;
            ResizeWindowForContent(hWnd); InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        if (cTab != -1 && g_activeTab != cTab) {
            g_activeTab = cTab;
            ResizeWindowForContent(hWnd); InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        if (cRow != -1) {
            auto items = GetCurrentItems();
            if (cRow >= items.size()) return 0;
            auto& item = items[cRow];

            if (cGear) {
                g_subPage = item.gearTarget;
                ResizeWindowForContent(hWnd); InvalidateRect(hWnd, nullptr, FALSE);
            } else if (cToggle && item.pBoolValue && !item.disabled) {
                *item.pBoolValue = !(*item.pBoolValue);
                if (item.pBoolValue == &g_pConfig->startIconCustom) {
                    if (*item.pBoolValue) {
                        g_pConfig->startIconAccentColor = false;
                        if (g_pConfig->startIconColorHue < 0) {
                            g_pConfig->startIconColorHue = 210;
                        }
                    }
                }
                if (g_onConfigChanged) g_onConfigChanged();
                ResizeWindowForContent(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (cBtn != 0 && item.pIntValue && !item.disabled) {
                *item.pIntValue = (cBtn == 1) ? std::max(item.minInt, *item.pIntValue - item.stepInt)
                                              : std::min(item.maxInt, *item.pIntValue + item.stepInt);
                if (g_onConfigChanged) g_onConfigChanged();
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (item.type == RowType::Segmented) {
                float sx = (rc.right - rc.left) - kSidePadding * scale - 110.0f * scale;
                const bool left = GET_X_LPARAM(lParam) <= sx + 55.0f * scale;
                if (item.pBoolValue) *item.pBoolValue = !left;
                else if (g_pConfig) g_pConfig->dockAutoPhysics = left;
                if (g_onConfigChanged) g_onConfigChanged();
                ResizeWindowForContent(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (item.type == RowType::Slider && item.pIntValue) {
                const float sliderW = 140.0f * scale;
                const float sx = (rc.right - rc.left) - kSidePadding * scale - sliderW;
                const float pct = std::clamp((GET_X_LPARAM(lParam) - sx) / sliderW, 0.0f, 1.0f);
                int value = item.minInt + static_cast<int>(std::lround(pct * (item.maxInt - item.minInt)));
                value = (value / item.stepInt) * item.stepInt;
                *item.pIntValue = std::clamp(value, item.minInt, item.maxInt);
                if (g_pConfig && g_subPage == SubPage::StartMenuSettings) {
                    const double factor = g_pConfig->startMenuScale / 100.0;
                    g_pConfig->startMenuWidth = static_cast<int>(std::lround(960 * factor));
                    g_pConfig->startMenuHeight = static_cast<int>(std::lround(720 * factor));
                    g_pConfig->searchWidth = g_pConfig->startMenuWidth;
                    g_pConfig->searchHeight = g_pConfig->startMenuHeight;
                }
                if (g_onConfigChanged) g_onConfigChanged();
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (item.type == RowType::ColorPicker && item.pIntValue) {
                float padX = kSidePadding * scale;
                float pickerW = 140.0f * scale;
                float px = (rc.right - rc.left) - padX - pickerW;
                float trackH = 12.0f * scale;
                float tr = trackH / 2.0f;
                float activeW = pickerW - 2 * tr;
                float mx = static_cast<float>(GET_X_LPARAM(lParam));
                float pct = std::clamp((mx - (px + tr)) / activeW, 0.0f, 1.0f);
                g_dragHue = pct * 360.0f;
                if (g_dragHue >= 360.0f) g_dragHue = 359.9f;
                int newHue = static_cast<int>(g_dragHue);
                g_draggingColorHue = true;
                SetCapture(hWnd);
                if (g_pConfig) {
                    g_pConfig->startIconCustom = true;
                    g_pConfig->startIconAccentColor = false;
                    g_pConfig->startIconColorHue = newHue;
                    Injector::Update(*g_pConfig);
                }
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (item.type == RowType::Action && cToggle) {
                UpdateManager::CheckNow(hWnd);
            }
        }
        return 0;
    }
    case WM_DPICHANGED: {
        ResizeWindowForContent(hWnd);
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    }
    case WM_CLOSE: ShowWindow(hWnd, SW_HIDE); return 0;
    case WM_DESTROY: g_hWnd = nullptr; return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void ModernGUI::Show(HINSTANCE hInstance, AppConfig& config, void (*onConfigChanged)()) {
    g_pConfig = &config;
    g_onConfigChanged = onConfigChanged;

    if (g_gdiplusToken == 0) {
        GdiplusStartupInput gdiplusStartupInput;
        GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);
        g_fonts = std::make_unique<FontCache>();
    }

    if (!g_hWnd) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ModernWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = GUI_CLASS_NAME;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        g_hAppIcon = IconGenerator::CreateMinimalistIcon(32);
        wc.hIcon = g_hAppIcon;
        wc.hIconSm = g_hAppIcon;
        RegisterClassExW(&wc);

        float initialScale = 1.0f;
        HDC screenDC = GetDC(nullptr);
        if (screenDC) {
            int dpiY = GetDeviceCaps(screenDC, LOGPIXELSY);
            if (dpiY > 0) initialScale = static_cast<float>(dpiY) / 96.0f;
            ReleaseDC(nullptr, screenDC);
        }

        auto items = GetCurrentItems();
        int winW = static_cast<int>(std::lround(kWindowWidth * initialScale));
        int winH = static_cast<int>(std::lround((kMainHeaderHeight + items.size() * kRowHeight + kBottomPadding) * initialScale));

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);

        g_hWnd = CreateWindowExW(
            WS_EX_APPWINDOW, GUI_CLASS_NAME, L"Lightency",
            WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            (screenW - winW) / 2, (screenH - winH) / 2, winW, winH,
            nullptr, nullptr, hInstance, nullptr);
    }

    if (g_hWnd) {
        ResizeWindowForContent(g_hWnd);
        ShowWindow(g_hWnd, SW_SHOW);
        SetForegroundWindow(g_hWnd);
    }
}

void ModernGUI::Close() {
    if (g_hWnd) {
        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
    }
    if (g_hAppIcon) {
        DestroyIcon(g_hAppIcon);
        g_hAppIcon = nullptr;
    }
    if (g_gdiplusToken != 0) {
        g_fonts.reset();
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

bool ModernGUI::IsVisible() {
    return g_hWnd && IsWindowVisible(g_hWnd);
}

}
