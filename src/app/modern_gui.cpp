#include "modern_gui.h"
#include "icon_gen.h"
#include "config.h"
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

enum class SubPage { None, DockSettings, TrayItems };
enum class RowType { Toggle, Stepper, Segmented, Action };

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
        float r = h / 2.0f;
        GraphicsPath path;
        path.AddArc(x, y, 2 * r, 2 * r, 90, 180);
        path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 180);
        path.CloseFigure();

        g.TranslateTransform(0.5f, 0.5f);
        g.DrawPath(&borderPen, &path);
        g.TranslateTransform(-0.5f, -0.5f);

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

static void DrawGearIcon(Graphics& g, float x, float y, float w, float h, float scale, bool hovered) {
    if (hovered) {
        SolidBrush hoverBg(Color(255, 30, 48, 56));
        DrawPill(g, hoverBg, x, y, w, h);
    }
    SolidBrush iconBrush(hovered ? Color(255, 0, 210, 255) : Color(255, 160, 160, 160));
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

static void DrawSegmentedCombo(Graphics& g, float x, float y, float scale, int selectedIndex) {
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

    g.DrawString(L"Auto", -1, (selectedIndex == 0) ? &activeFont : &font, RectF(x, y, segW, h), &centerFmt, (selectedIndex == 0) ? &activeText : &inactiveText);
    g.DrawString(L"Custom", -1, (selectedIndex == 1) ? &activeFont : &font, RectF(x + segW, y, segW, h), &centerFmt, (selectedIndex == 1) ? &activeText : &inactiveText);
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
    } else {
        if (g_activeTab == 0) {
            items.push_back({ L"Clear taskbar", RowType::Toggle, false, SubPage::None, &g_pConfig->clearTaskbar });
            items.push_back({ L"Dock animation", RowType::Toggle, true, SubPage::DockSettings, &g_pConfig->dockAnimation });
            items.push_back({ L"Tray items", RowType::Toggle, true, SubPage::TrayItems, &g_pConfig->trayItems });
            items.push_back({ L"Move Start button (Ctrl+drag)", RowType::Toggle, false, SubPage::None, &g_pConfig->layoutEditor });
        } else {
            items.push_back({ L"Show tray icon", RowType::Toggle, false, SubPage::None, &g_pConfig->showTrayIcon });
            items.push_back({ L"Start with Windows", RowType::Toggle, false, SubPage::None, &g_pConfig->autostart });
            items.push_back({ L"Automatic update checks", RowType::Toggle, false, SubPage::None, &g_pConfig->automaticUpdates });
            items.push_back({ L"Check for updates", RowType::Action });
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
            if (g_hoveredBack) {
                SolidBrush backHoverBg(Color(255, 30, 48, 56));
                DrawPill(g, backHoverBg, 8.0f * scale, 6.0f * scale, 24.0f * scale, 20.0f * scale);
            }
            SolidBrush backBrush(g_hoveredBack ? Color(255, 0, 210, 255) : Color(255, 180, 180, 180));
            DrawSymbolGlyph(g, 0xE72B, RectF(8.0f * scale, 6.0f * scale, 24.0f * scale, 20.0f * scale), backBrush, 11.0f * scale);

            SolidBrush titleBrush(Color(255, 240, 240, 240));
            const wchar_t* subPageTitle = g_subPage == SubPage::DockSettings
                ? L"Dock Animation Settings" : L"Tray Items";
            g.DrawString(subPageTitle, -1, &captionFont, PointF(36.0f * scale, 8.0f * scale), &titleBrush);
        } else {
            if (g_hAppIcon) {
                int iconSz = static_cast<int>(18.0f * scale);
                DrawIconEx(buffer.memDC, static_cast<int>(12.0f * scale), static_cast<int>(8.0f * scale), g_hAppIcon, iconSz, iconSz, 0, nullptr, DI_NORMAL);
            }

            SolidBrush titleBrush(Color(255, 240, 240, 240));
            g.DrawString(L"Lightency", -1, &captionFont, PointF(34.0f * scale, 8.0f * scale), &titleBrush);

            float tabY = 31.0f * scale;
            float tabW = 80.0f * scale;
            float tabStartX = 12.0f * scale;
            const wchar_t* tabs[] = { L"Taskbar", L"Settings" };

            for (int i = 0; i < 2; ++i) {
                float tx = tabStartX + i * (tabW + 4.0f * scale);
                bool isActive = (g_activeTab == i);
                bool isHovered = (g_hoveredTab == i);

                if (isActive) {
                    SolidBrush activeBg(Color(255, 30, 44, 52));
                    DrawPill(g, activeBg, tx, tabY, tabW, 24.0f * scale);
                    SolidBrush indicatorBrush(Color(255, 0, 180, 255));
                    g.FillRectangle(&indicatorBrush,
                                    static_cast<int>(tx + 14.0f * scale),
                                    static_cast<int>(tabY + 22.0f * scale),
                                    static_cast<int>(tabW - 28.0f * scale),
                                    static_cast<int>(2.0f * scale));
                    SolidBrush activeText(Color(255, 255, 255, 255));
                    g.DrawString(tabs[i], -1, &tabActiveFont, RectF(tx, tabY, tabW, 22.0f * scale), &centerFmt, &activeText);
                } else {
                    if (isHovered) {
                        SolidBrush hoverBg(Color(255, 34, 38, 42));
                        DrawPill(g, hoverBg, tx, tabY, tabW, 24.0f * scale);
                    }
                    SolidBrush inactiveText(isHovered ? Color(255, 220, 220, 220) : Color(255, 150, 150, 150));
                    g.DrawString(tabs[i], -1, &tabFont, RectF(tx, tabY, tabW, 24.0f * scale), &centerFmt, &inactiveText);
                }
            }
        }

        if (g_hoveredClose) {
            SolidBrush closeHoverBg(Color(255, 196, 43, 28));
            DrawPill(g, closeHoverBg, buffer.width - 32.0f * scale, 5.0f * scale, 24.0f * scale, 20.0f * scale);
        }
        Pen closePen(g_hoveredClose ? Color(255, 255, 255, 255) : Color(255, 180, 180, 180), 1.0f);
        int cx = static_cast<int>(buffer.width - 20.0f * scale);
        int cy = static_cast<int>(15.0f * scale);
        int csz = static_cast<int>(3.5f * scale);

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
            if (items[i].type == RowType::Stepper || items[i].type == RowType::Action) controlLeft = buffer.width - padX - 94.0f * scale;
            else if (items[i].type == RowType::Segmented) controlLeft = buffer.width - padX - 110.0f * scale;
            else if (items[i].hasGear) controlLeft = buffer.width - padX - 68.0f * scale;
            float textX = padX + 2.0f * scale;
            g.DrawString(items[i].title.c_str(), -1, &titleFont,
                         RectF(textX, ry, std::max(0.0f, controlLeft - textX - 10.0f * scale), rowH),
                         &vCenterFmt, &textBrush);

            if (items[i].type == RowType::Toggle && items[i].pBoolValue) {
                DrawNativeToggle(g, buffer.width - padX - 38.0f * scale, ry + (rowH - 18.0f * scale) / 2.0f, scale, *(items[i].pBoolValue), isHovered && g_hoveredToggle, items[i].disabled);
                if (items[i].hasGear) {
                    DrawGearIcon(g, buffer.width - padX - 68.0f * scale, ry + (rowH - 24.0f * scale) / 2.0f, 24.0f * scale, 24.0f * scale, scale, isHovered && g_hoveredGear);
                }
            } else if (items[i].type == RowType::Stepper && items[i].pIntValue) {
                DrawNativeStepper(g, buffer.width - padX - 94.0f * scale, ry + (rowH - 22.0f * scale) / 2.0f, scale, *(items[i].pIntValue), items[i].unit, isHovered ? g_hoveredStepperBtn : 0, items[i].displayOverride, items[i].disabled);
            } else if (items[i].type == RowType::Segmented) {
                DrawSegmentedCombo(g, buffer.width - padX - 110.0f * scale, ry + (rowH - 22.0f * scale) / 2.0f, scale, items[i].selectedIndex);
            } else if (items[i].type == RowType::Action) {
                float bx = buffer.width - padX - 92.0f * scale;
                float by = ry + (rowH - 24.0f * scale) / 2.0f;
                SolidBrush buttonBg(isHovered ? Color(255, 0, 195, 255) : Color(255, 0, 180, 255));
                DrawPill(g, buttonBg, bx, by, 92.0f * scale, 24.0f * scale);
                Font buttonFont(g_fonts->segoeUI.get(), 11.0f * scale, FontStyleBold, UnitPixel);
                SolidBrush buttonText(Color(255, 255, 255, 255));
                g.DrawString(L"Check", -1, &buttonFont, RectF(bx, by, 92.0f * scale, 24.0f * scale), &centerFmt, &buttonText);
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
        float tabW = 80.0f * scale;
        float tabStartX = 12.0f * scale;
        for (int i = 0; i < 2; ++i) {
            float tx = tabStartX + i * (tabW + 4.0f * scale);
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

            if (items[rowIndex].type == RowType::Stepper) {
                float sx = width - padX - 94.0f * scale;
                float btnW = 20.0f * scale;
                if (x >= sx && x <= sx + 92.0f * scale) {
                    if (x <= sx + btnW) outStepperBtn = 1;
                    else if (x >= sx + 92.0f * scale - btnW) outStepperBtn = 2;
                }
            } else if (items[rowIndex].type == RowType::Action) {
                float bx = width - padX - 92.0f * scale;
                if (x >= bx && x <= bx + 92.0f * scale) outToggle = true;
            } else if (items[rowIndex].type == RowType::Toggle) {
                float tx = width - padX - 38.0f * scale;
                if (x >= tx && x <= tx + 36.0f * scale) outToggle = true;
                if (items[rowIndex].hasGear) {
                    float gx = width - padX - 68.0f * scale;
                    if (x >= gx && x <= gx + 24.0f * scale) outGear = true;
                }
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

    case WM_MOUSEMOVE: {
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
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        if (!g_trackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        g_trackingMouse = false;
        g_hoveredClose = false; g_hoveredBack = false; g_hoveredTab = -1;
        g_hoveredRow = -1; g_hoveredStepperBtn = 0; g_hoveredGear = false; g_hoveredToggle = false;
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
            g_subPage = SubPage::None;
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
                if (g_onConfigChanged) g_onConfigChanged();
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (cBtn != 0 && item.pIntValue && !item.disabled) {
                *item.pIntValue = (cBtn == 1) ? std::max(item.minInt, *item.pIntValue - item.stepInt)
                                              : std::min(item.maxInt, *item.pIntValue + item.stepInt);
                if (g_onConfigChanged) g_onConfigChanged();
                InvalidateRect(hWnd, nullptr, FALSE);
            } else if (item.type == RowType::Segmented) {
                float sx = (rc.right - rc.left) - kSidePadding * scale - 110.0f * scale;
                if (g_pConfig) g_pConfig->dockAutoPhysics = (GET_X_LPARAM(lParam) <= sx + 55.0f * scale);
                if (g_onConfigChanged) g_onConfigChanged();
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
