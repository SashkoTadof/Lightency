#include "branded_dialog.h"
#include <dwmapi.h>
#include <unknwn.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <algorithm>

namespace Lightency::BrandedDialog {
namespace {
constexpr wchar_t kClassName[] = L"LightencyBrandedDialog";
constexpr UINT kButtonPrimary = 1001;
constexpr UINT kButtonSecondary = 1002;

struct State {
    std::wstring title;
    std::wstring message;
    Kind kind = Kind::Info;
    bool result = false;
    bool finished = false;
    int hover = 0;
    float closeGlow = 0.0f;
    float primaryGlow = 0.0f;
    float secondaryGlow = 0.0f;
};

RECT PrimaryRect(const RECT& client, bool confirm) {
    const int width = confirm ? 86 : 92;
    return {client.right - 20 - width, client.bottom - 48,
        client.right - 20, client.bottom - 18};
}
RECT SecondaryRect(const RECT& client) {
    return {client.right - 20 - 86 - 10 - 86, client.bottom - 48,
        client.right - 20 - 86 - 10, client.bottom - 18};
}

static BYTE BlendByte(BYTE from, BYTE to, float amount) {
    return static_cast<BYTE>(from + (to - from) * std::clamp(amount, 0.0f, 1.0f));
}

static void DrawPill(Gdiplus::Graphics& g, const Gdiplus::Brush& brush, float x, float y, float w, float h) {
    float r = h / 2.0f;
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, 2 * r, 2 * r, 90, 180);
    path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 180);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static void DrawPillOutline(Gdiplus::Graphics& g, const Gdiplus::Pen& pen, float x, float y, float w, float h) {
    const float inset = 0.5f;
    float r = (h - 1.0f) / 2.0f;
    Gdiplus::GraphicsPath path;
    path.AddArc(x + inset, y + inset, 2 * r, 2 * r, 90, 180);
    path.AddArc(x + w - inset - 2 * r, y + inset, 2 * r, 2 * r, 270, 180);
    path.CloseFigure();
    g.DrawPath(&pen, &path);
}

void Paint(HWND window, State* state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    RECT client{}; GetClientRect(window, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    Gdiplus::Graphics g(memory);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(Gdiplus::Color(255, 20, 24, 28));

    Gdiplus::Font titleFont(L"Segoe UI", 12.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font bodyFont(L"Segoe UI", 12.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font buttonFont(L"Segoe UI", 11.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush titleBrush(Gdiplus::Color(255, 242, 245, 247));
    Gdiplus::SolidBrush bodyBrush(Gdiplus::Color(255, 220, 226, 230));
    g.DrawString(state->title.c_str(), -1, &titleFont,
        Gdiplus::PointF(16.0f, 12.0f), &titleBrush);

    Gdiplus::Pen closePen(Gdiplus::Color(255, BlendByte(180, 235, state->closeGlow), BlendByte(180, 75, state->closeGlow),
        BlendByte(180, 65, state->closeGlow)), 1.1f);
    closePen.SetStartCap(Gdiplus::LineCapRound);
    closePen.SetEndCap(Gdiplus::LineCapRound);

    float cx = static_cast<float>(client.right - 20);
    float cy = 16.0f;
    float csz = 4.0f;

    if (state->closeGlow > 0.01f) {
        Gdiplus::Pen closeHalo(Gdiplus::Color(static_cast<BYTE>(58.0f * state->closeGlow), 235, 75, 65), 3.0f);
        closeHalo.SetStartCap(Gdiplus::LineCapRound);
        closeHalo.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&closeHalo, cx - csz, cy - csz, cx + csz, cy + csz);
        g.DrawLine(&closeHalo, cx + csz, cy - csz, cx - csz, cy + csz);
    }
    g.TranslateTransform(0.5f, 0.5f);
    g.DrawLine(&closePen, cx - csz, cy - csz, cx + csz, cy + csz);
    g.DrawLine(&closePen, cx + csz, cy - csz, cx - csz, cy + csz);
    g.TranslateTransform(-0.5f, -0.5f);

    Gdiplus::StringFormat centered;
    centered.SetAlignment(Gdiplus::StringAlignmentCenter);
    centered.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::StringFormat wrap;
    wrap.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(state->message.c_str(), -1, &bodyFont,
        Gdiplus::RectF(16, 39, static_cast<float>(client.right - 32), 34), &wrap, &bodyBrush);

    RECT primary = PrimaryRect(client, state->kind == Kind::Confirm);
    float px = static_cast<float>(primary.left);
    float py = static_cast<float>(primary.top);
    float pw = static_cast<float>(primary.right - primary.left);
    float ph = static_cast<float>(primary.bottom - primary.top);

    if (state->primaryGlow > 0.01f) {
        Gdiplus::Pen halo(Gdiplus::Color(static_cast<BYTE>(48.0f * state->primaryGlow), 0, 210, 255), 2.5f);
        DrawPillOutline(g, halo, px, py, pw, ph);
    }
    BYTE prR = BlendByte(0, 35, state->primaryGlow);
    BYTE prG = BlendByte(180, 200, state->primaryGlow);
    BYTE prB = 255;
    Gdiplus::SolidBrush primaryBg(Gdiplus::Color(255, prR, prG, prB));
    DrawPill(g, primaryBg, px, py, pw, ph);

    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    g.DrawString(state->kind == Kind::Confirm ? L"Update" : L"OK", -1, &buttonFont,
        Gdiplus::RectF(px, py, pw, ph), &centered, &white);

    if (state->kind == Kind::Confirm) {
        RECT secondary = SecondaryRect(client);
        float sx = static_cast<float>(secondary.left);
        float sy = static_cast<float>(secondary.top);
        float sw = static_cast<float>(secondary.right - secondary.left);
        float sh = static_cast<float>(secondary.bottom - secondary.top);

        BYTE secR = BlendByte(32, 44, state->secondaryGlow);
        BYTE secG = BlendByte(36, 50, state->secondaryGlow);
        BYTE secB = BlendByte(42, 60, state->secondaryGlow);
        Gdiplus::SolidBrush secBg(Gdiplus::Color(255, secR, secG, secB));
        DrawPill(g, secBg, sx, sy, sw, sh);

        BYTE bdrR = BlendByte(52, 75, state->secondaryGlow);
        BYTE bdrG = BlendByte(58, 85, state->secondaryGlow);
        BYTE bdrB = BlendByte(68, 100, state->secondaryGlow);
        Gdiplus::Pen secBorder(Gdiplus::Color(255, bdrR, bdrG, bdrB), 1.0f);
        DrawPillOutline(g, secBorder, sx, sy, sw, sh);

        BYTE txtR = BlendByte(218, 245, state->secondaryGlow);
        BYTE txtG = BlendByte(222, 248, state->secondaryGlow);
        BYTE txtB = BlendByte(228, 255, state->secondaryGlow);
        Gdiplus::SolidBrush secText(Gdiplus::Color(255, txtR, txtG, txtB));
        g.DrawString(L"Later", -1, &buttonFont, Gdiplus::RectF(sx, sy, sw, sh), &centered, &secText);
    }
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
    EndPaint(window, &ps);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_PAINT: Paint(window, state); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        if (wParam != 1) break;
        if (!state) return 0;
        bool moving = false;
        auto animate = [&moving](float& value, float target) {
            const float delta = target - value;
            if (std::abs(delta) < 0.015f) value = target;
            else { value += delta * 0.24f; moving = true; }
        };
        animate(state->closeGlow, state->hover == 3 ? 1.0f : 0.0f);
        animate(state->primaryGlow, state->hover == 1 ? 1.0f : 0.0f);
        animate(state->secondaryGlow, state->hover == 2 ? 1.0f : 0.0f);
        InvalidateRect(window, nullptr, FALSE);
        if (!moving) KillTimer(window, 1);
        return 0;
    }
    case WM_NCHITTEST: {
        LRESULT result = DefWindowProcW(window, message, wParam, lParam);
        if (result == HTCLIENT) {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            RECT client{}; GetClientRect(window, &client);
            if (point.y < 32 && point.x > client.right - 42) return HTCLIENT;
            if (point.y < 40) return HTCAPTION;
        }
        return result;
    }
    case WM_MOUSEMOVE: {
        RECT client{}; GetClientRect(window, &client);
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT primary = PrimaryRect(client, state->kind == Kind::Confirm);
        RECT secondary = SecondaryRect(client);
        int hover = PtInRect(&primary, point) ? 1 :
            (state->kind == Kind::Confirm && PtInRect(&secondary, point) ? 2 :
            (point.y < 32 && point.x > client.right - 42 ? 3 : 0));
        if (hover != state->hover) {
            state->hover = hover;
            SetTimer(window, 1, 16, nullptr);
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (state) {
            state->hover = 0;
            SetTimer(window, 1, 16, nullptr);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        RECT client{}; GetClientRect(window, &client);
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT primary = PrimaryRect(client, state->kind == Kind::Confirm);
        RECT secondary = SecondaryRect(client);
        if (PtInRect(&primary, point)) { state->result = true; state->finished = true; }
        else if (state->kind == Kind::Confirm && PtInRect(&secondary, point)) state->finished = true;
        else if (point.y < 40 && point.x > client.right - 40) state->finished = true;
        if (state->finished) {
            KillTimer(window, 1);
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            state->result = true;
            state->finished = true;
            KillTimer(window, 1);
            DestroyWindow(window);
        } else if (wParam == VK_ESCAPE) {
            state->finished = true;
            KillTimer(window, 1);
            DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        state->finished = true;
        KillTimer(window, 1);
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

bool Show(HWND owner, const std::wstring& message, Kind kind, const std::wstring& title) {
    static ATOM atom = [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = Proc;
        wc.lpszClassName = kClassName; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc);
    }();
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    State state{title, message, kind};
    const int width = 300, height = 118;
    RECT anchor{};
    if (!owner || !GetWindowRect(owner, &anchor)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
    int x = anchor.left + ((anchor.right-anchor.left)-width)/2;
    int y = anchor.top + ((anchor.bottom-anchor.top)-height)/2;
    HWND window = CreateWindowExW(WS_EX_TOPMOST, kClassName, L"",
        WS_POPUP, x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) return false;
    BOOL dark = TRUE; DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    int corner = 2; DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
    COLORREF borderColor = RGB(52, 64, 73);
    DwmSetWindowAttribute(window, 34, &borderColor, sizeof(borderColor));
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(window);
    MSG msg{};
    while (!state.finished && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    return state.result;
}
}
