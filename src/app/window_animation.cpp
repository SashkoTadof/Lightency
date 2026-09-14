#include "window_animation.h"
#include "debug_log.h"
#include "payload_manager.h"
#include "../common/types.h"
#include <windows.h>
#include <dwmapi.h>
#include <unknwn.h>
#include <objbase.h>
#include <uiautomation.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <memory>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <cwctype>

namespace Lightency::WindowAnimation {
namespace {
constexpr wchar_t kGhostClass[] = L"LightencyGenieGhost";
std::atomic<bool> g_enabled{false};
std::atomic<int> g_duration{420};
std::atomic<int> g_curveIntensity{100};
std::atomic<int> g_targetWidth{22};
std::atomic<int> g_repeatGuard{80};
std::atomic<int> g_captureDelay{180};
std::atomic<int> g_workers{0};
std::atomic<unsigned long long> g_cacheGeneration{0};
HWINEVENTHOOK g_minimizeStart = nullptr;
HWINEVENTHOOK g_foreground = nullptr;
HHOOK g_earlyHook = nullptr;
HMODULE g_earlyHookModule = nullptr;
HANDLE g_ipcMapping = nullptr;
WindowAnimationIpc* g_ipc = nullptr;
HWND g_receiverWindow = nullptr;

struct Snapshot {
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;
};

std::mutex g_cacheMutex;
HWND g_cachedWindow = nullptr;
Snapshot g_cachedSnapshot;
RECT g_cachedRect{};
POINT g_cachedTarget{};
bool g_cachedTargetValid = false;
std::mutex g_activeMutex;
std::unordered_set<HWND> g_activeAnimations;
std::unordered_map<HWND, ULONGLONG> g_lastAnimationStarted;

void Start(HWND window, bool rising, bool early);
void FindTarget(HWND window, const RECT& source, int& x, int& y);

bool BeginAnimation(HWND window) {
    std::lock_guard lock(g_activeMutex);
    const ULONGLONG now = GetTickCount64();
    const auto last = g_lastAnimationStarted.find(window);
    const ULONGLONG minimumInterval =
        static_cast<ULONGLONG>(g_duration.load(std::memory_order_relaxed) +
            g_repeatGuard.load(std::memory_order_relaxed));
    if (last != g_lastAnimationStarted.end() && now - last->second < minimumInterval)
        return false;
    if (!g_activeAnimations.insert(window).second)
        return false;
    g_lastAnimationStarted[window] = now;
    return true;
}

void EndAnimation(HWND window) {
    std::lock_guard lock(g_activeMutex);
    g_activeAnimations.erase(window);
}

bool IsAnimationActive(HWND window) {
    std::lock_guard lock(g_activeMutex);
    return g_activeAnimations.contains(window);
}

struct Animation {
    HWND window = nullptr;
    Snapshot snapshot;
    RECT source{};
    int targetX = 0;
    int targetY = 0;
    int duration = 420;
    bool rising = false;
    HANDLE firstFrame = nullptr;
};

bool GetAnimationRect(HWND window, RECT& rect) {
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &rect, sizeof(rect)))) {
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (rect.left > -30000 && rect.top > -30000 && width >= 100 && height >= 80)
            return true;
    }

    WINDOWPLACEMENT placement{sizeof(placement)};
    if (GetWindowPlacement(window, &placement)) {
        rect = placement.rcNormalPosition;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width >= 100 && height >= 80) {
            LogDebug("WindowAnimation: using normal placement rect=" +
                std::to_string(rect.left) + "," + std::to_string(rect.top) + "," +
                std::to_string(width) + "x" + std::to_string(height));
            return true;
        }
    }

    return GetWindowRect(window, &rect) && rect.left > -30000 && rect.top > -30000 &&
        rect.right - rect.left >= 100 && rect.bottom - rect.top >= 80;
}

LRESULT CALLBACK GhostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ReceiverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == GetWindowAnimationEarlyMsg()) {
        HWND target = GetAncestor(reinterpret_cast<HWND>(wParam), GA_ROOT);
        target = target ? target : reinterpret_cast<HWND>(wParam);
        Start(target, false, true);
        if (IsAnimationActive(target)) {
            BOOL cloak = TRUE;
            DwmSetWindowAttribute(target, DWMWA_CLOAK, &cloak, sizeof(cloak));
        }
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool InstallEarlyHook() {
    if (g_earlyHook) return true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = ReceiverProc;
    wc.lpszClassName = L"LightencyWindowAnimationReceiver";
    RegisterClassExW(&wc);
    g_receiverWindow = CreateWindowExW(0, wc.lpszClassName, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_receiverWindow) return false;

    g_ipcMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(WindowAnimationIpc), L"Lightency_WindowAnimation_v1");
    g_ipc = g_ipcMapping ? static_cast<WindowAnimationIpc*>(MapViewOfFile(
        g_ipcMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(WindowAnimationIpc))) : nullptr;
    if (!g_ipc) return false;
    g_ipc->masterPid = GetCurrentProcessId();
    g_ipc->receiverWindow = g_receiverWindow;
    g_ipc->enabled = TRUE;

    const std::wstring dllPath = PayloadManager::GetHookDllPath();
    g_earlyHookModule = LoadLibraryW(dllPath.c_str());
    auto proc = g_earlyHookModule ? reinterpret_cast<HOOKPROC>(
        GetProcAddress(g_earlyHookModule, "LightencyCbtProc")) : nullptr;
    g_earlyHook = proc ? SetWindowsHookExW(WH_CBT, proc, g_earlyHookModule, 0) : nullptr;
    LogDebug("WindowAnimation: early hook installed=" + std::to_string(g_earlyHook != nullptr));
    return g_earlyHook != nullptr;
}

void UninstallEarlyHook() {
    if (g_ipc) g_ipc->enabled = FALSE;
    if (g_earlyHook) UnhookWindowsHookEx(g_earlyHook);
    g_earlyHook = nullptr;
    if (g_earlyHookModule) FreeLibrary(g_earlyHookModule);
    g_earlyHookModule = nullptr;
    if (g_ipc) UnmapViewOfFile(g_ipc);
    g_ipc = nullptr;
    if (g_ipcMapping) CloseHandle(g_ipcMapping);
    g_ipcMapping = nullptr;
    if (g_receiverWindow) DestroyWindow(g_receiverWindow);
    g_receiverWindow = nullptr;
}

void EnsureGhostClass() {
    static std::atomic<bool> registered{false};
    if (registered.exchange(true)) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = GhostProc;
    wc.lpszClassName = kGhostClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

bool ShouldAnimate(HWND window) {
    if (!g_enabled.load(std::memory_order_relaxed) || !IsWindow(window)) {
        LogDebug("WindowAnimation: invalid/disabled");
        return false;
    }
    wchar_t className[96]{};
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (wcscmp(className, kGhostClass) == 0 || wcscmp(className, L"Shell_TrayWnd") == 0) {
        LogDebug("WindowAnimation: excluded class");
        return false;
    }
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (style & WS_CHILD) {
        LogDebug("WindowAnimation: child class=" + std::to_string(reinterpret_cast<uintptr_t>(window)));
        return false;
    }
    RECT rect{};
    const bool validRect = GetAnimationRect(window, rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    LogDebug("WindowAnimation: eligible-check hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(window)) +
        " style=" + std::to_string(style) + " rect=" + std::to_string(rect.left) + "," +
        std::to_string(rect.top) + "," + std::to_string(width) + "x" + std::to_string(height));
    return validRect;
}

Snapshot Capture(HWND window, const RECT& rect, bool screenFirst = false) {
    Snapshot result;
    result.width = rect.right - rect.left;
    result.height = rect.bottom - rect.top;
    if (result.width <= 0 || result.height <= 0) return result;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = result.width;
    info.bmiHeader.biHeight = -result.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    result.bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &result.bits, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    HGDIOBJ old = result.bitmap ? SelectObject(memory, result.bitmap) : nullptr;
    BOOL captured = FALSE;
    if (result.bitmap && screenFirst)
        captured = BitBlt(memory, 0, 0, result.width, result.height, screen,
            rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    if (!captured && result.bitmap)
        captured = PrintWindow(window, memory, 2);
    if (!captured && result.bitmap && !screenFirst)
        captured = BitBlt(memory, 0, 0, result.width, result.height, screen,
            rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    if (old) SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!captured) {
        if (result.bitmap) DeleteObject(result.bitmap);
        return {};
    }
    auto* pixels = static_cast<unsigned int*>(result.bits);
    const size_t count = static_cast<size_t>(result.width) * result.height;
    for (size_t i = 0; i < count; ++i) pixels[i] |= 0xFF000000u;
    return result;
}

void CacheWindow(HWND window) {
    window = GetAncestor(window, GA_ROOT);
    if (!window || !ShouldAnimate(window) || IsIconic(window)) return;
    RECT rect{};
    if (!GetAnimationRect(window, rect)) return;
    Snapshot snapshot = Capture(window, rect, true);
    if (!snapshot.bitmap) return;
    int targetX = 0, targetY = 0;
    FindTarget(window, rect, targetX, targetY);
    POINT target{targetX, targetY};
    BOOL transitionsDisabled = TRUE;
    DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
        &transitionsDisabled, sizeof(transitionsDisabled));
    std::lock_guard lock(g_cacheMutex);
    if (g_cachedWindow && g_cachedWindow != window && IsWindow(g_cachedWindow)) {
        BOOL enabled = FALSE;
        DwmSetWindowAttribute(g_cachedWindow, DWMWA_TRANSITIONS_FORCEDISABLED,
            &enabled, sizeof(enabled));
    }
    if (g_cachedSnapshot.bitmap) DeleteObject(g_cachedSnapshot.bitmap);
    g_cachedWindow = window;
    g_cachedSnapshot = snapshot;
    g_cachedRect = rect;
    g_cachedTarget = target;
    g_cachedTargetValid = true;
    LogDebug("WindowAnimation: cached foreground hwnd=" +
        std::to_string(reinterpret_cast<uintptr_t>(window)));
}

bool TakeCached(HWND window, Snapshot& snapshot, RECT& rect, POINT& target,
    bool& targetValid) {
    std::lock_guard lock(g_cacheMutex);
    if (g_cachedWindow != window || !g_cachedSnapshot.bitmap) return false;
    snapshot = g_cachedSnapshot;
    rect = g_cachedRect;
    target = g_cachedTarget;
    targetValid = g_cachedTargetValid;
    g_cachedSnapshot = {};
    g_cachedWindow = nullptr;
    g_cachedTargetValid = false;
    LogDebug("WindowAnimation: using foreground cache");
    return true;
}

struct CacheRequest {
    HWND window;
    unsigned long long generation;
};

DWORD WINAPI CacheAfterPaint(void* parameter) {
    std::unique_ptr<CacheRequest> request(static_cast<CacheRequest*>(parameter));
    Sleep(static_cast<DWORD>(g_captureDelay.load(std::memory_order_relaxed)));
    HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (g_enabled.load(std::memory_order_relaxed) &&
        request->generation == g_cacheGeneration.load(std::memory_order_acquire) &&
        foreground == request->window) {
        CacheWindow(request->window);
    }
    g_workers.fetch_sub(1, std::memory_order_release);
    return 0;
}

void QueueCache(HWND window) {
    window = GetAncestor(window, GA_ROOT);
    if (!window) return;
    {
        std::lock_guard lock(g_cacheMutex);
        if (g_cachedWindow == window && g_cachedSnapshot.bitmap) return;
    }
    const auto generation = g_cacheGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto request = std::make_unique<CacheRequest>();
    request->window = window;
    request->generation = generation;
    g_workers.fetch_add(1, std::memory_order_relaxed);
    if (!QueueUserWorkItem(reinterpret_cast<LPTHREAD_START_ROUTINE>(CacheAfterPaint), request.release(), WT_EXECUTELONGFUNCTION)) {
        g_workers.fetch_sub(1, std::memory_order_release);
    }
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool FindTaskbarButton(HWND window, HWND taskbar, const RECT& tray, int& x, int& y) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = apartment == S_OK || apartment == S_FALSE;
    IUIAutomation* automation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation)))) {
        if (uninitialize) CoUninitialize();
        return false;
    }

    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* buttons = nullptr;
    IUIAutomationElementArray* elements = nullptr;
    bool found = false;
    int bestScore = 0;

    wchar_t titleBuffer[512]{};
    GetWindowTextW(window, titleBuffer, ARRAYSIZE(titleBuffer));
    const std::wstring title = Lower(titleBuffer);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::wstring executable;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[1024]{};
        DWORD length = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(process, 0, path, &length)) {
            const wchar_t* filename = wcsrchr(path, L'\\');
            executable = Lower(filename ? filename + 1 : path);
            if (const size_t dot = executable.rfind(L'.'); dot != std::wstring::npos)
                executable.resize(dot);
            if (executable == L"msedge") executable = L"edge";
        }
        CloseHandle(process);
    }

    VARIANT buttonType{};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    if (SUCCEEDED(automation->ElementFromHandle(taskbar, &root)) &&
        SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
            buttonType, &buttons)) &&
        SUCCEEDED(root->FindAll(TreeScope_Subtree, buttons, &elements))) {
        int count = 0;
        elements->get_Length(&count);
        for (int index = 0; index < count; ++index) {
            IUIAutomationElement* element = nullptr;
            if (FAILED(elements->GetElement(index, &element)) || !element) continue;
            BSTR rawName = nullptr;
            RECT bounds{};
            element->get_CurrentName(&rawName);
            element->get_CurrentBoundingRectangle(&bounds);
            const std::wstring name = rawName ? Lower(rawName) : L"";
            if (rawName) SysFreeString(rawName);
            element->Release();
            if (bounds.right <= tray.left || bounds.left >= tray.right ||
                bounds.bottom <= tray.top || bounds.top >= tray.bottom || name.empty()) continue;

            int score = 0;
            if (!executable.empty() && name.find(executable) != std::wstring::npos) score += 100;
            if (!title.empty() && (name.find(title) != std::wstring::npos ||
                title.find(name) != std::wstring::npos)) score += 200;
            size_t start = 0;
            while (start < title.size()) {
                while (start < title.size() && !iswalnum(title[start])) ++start;
                size_t end = start;
                while (end < title.size() && iswalnum(title[end])) ++end;
                if (end - start >= 4 && name.find(title.substr(start, end - start)) != std::wstring::npos)
                    score += static_cast<int>(end - start);
                start = end;
            }
            if (score > bestScore) {
                bestScore = score;
                x = (bounds.left + bounds.right) / 2;
                y = (bounds.top + bounds.bottom) / 2;
                found = true;
            }
        }
    }
    if (elements) elements->Release();
    if (buttons) buttons->Release();
    if (root) root->Release();
    automation->Release();
    if (uninitialize) CoUninitialize();
    return found;
}

void FindTarget(HWND window, const RECT& source, int& x, int& y) {
    HMONITOR monitor = MonitorFromRect(&source, MONITOR_DEFAULTTONEAREST);
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr))) {
        if (MonitorFromWindow(secondary, MONITOR_DEFAULTTONULL) == monitor) {
            taskbar = secondary;
            break;
        }
    }
    RECT tray{};
    POINT cursor{};
    GetCursorPos(&cursor);
    if (taskbar && GetWindowRect(taskbar, &tray)) {
        if (FindTaskbarButton(window, taskbar, tray, x, y)) {
            LogDebug("WindowAnimation: matched taskbar button x=" + std::to_string(x) +
                " y=" + std::to_string(y));
        } else if (PtInRect(&tray, cursor)) {
            x = cursor.x;
            y = (tray.top + tray.bottom) / 2;
            LogDebug("WindowAnimation: using taskbar cursor fallback x=" +
                std::to_string(x));
        } else {
            x = (source.left + source.right) / 2;
            y = (tray.top + tray.bottom) / 2;
            LogDebug("WindowAnimation: taskbar button not found, using monitor fallback");
        }
    } else {
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        x = (source.left + source.right) / 2;
        y = info.rcMonitor.bottom - 24;
    }
}

namespace GenieEngine {

struct Kinematics {
    static float EvaluateEase(float linearProgress, bool isRestoring) {
        const float t = std::clamp(linearProgress, 0.0f, 1.0f);
        if (isRestoring) {
            const float inv = 1.0f - t;
            return 1.0f - (inv * inv * inv * (inv * (inv * 6.0f - 15.0f) + 10.0f));
        }
        return t * t * (t * (t * -2.0f + 5.0f) - 2.0f * t + 2.0f);
    }
};

struct Vector2D {
    float x = 0.0f;
    float y = 0.0f;
};

inline Vector2D EvaluateCubicBezier(const Vector2D& p0, const Vector2D& p1,
                                    const Vector2D& p2, const Vector2D& p3,
                                    float t) {
    const float u = 1.0f - t;
    const float tt = t * t;
    const float uu = u * u;
    const float uuu = uu * u;
    const float ttt = tt * t;
    return {
        uuu * p0.x + 3.0f * uu * t * p1.x + 3.0f * u * tt * p2.x + ttt * p3.x,
        uuu * p0.y + 3.0f * uu * t * p1.y + 3.0f * u * tt * p2.y + ttt * p3.y
    };
}

struct GuideRails {
    Vector2D leftP0, leftP1, leftP2, leftP3;
    Vector2D rightP0, rightP1, rightP2, rightP3;
    float deltaX = 0.0f;
    float swayMultiplier = 1.0f;

    void Setup(const RECT& src, float targetX, float targetY, float pinWidth,
               float canvasOffsetX, float canvasOffsetY, float curveIntensityVal) {
        const float sx0 = static_cast<float>(src.left) - canvasOffsetX;
        const float sx1 = static_cast<float>(src.right) - canvasOffsetX;
        const float sy0 = static_cast<float>(src.top) - canvasOffsetY;
        const float tx = targetX - canvasOffsetX;
        const float ty = targetY - canvasOffsetY;
        const float halfPin = pinWidth * 0.5f;

        deltaX = tx - (sx0 + sx1) * 0.5f;
        const float deltaY = ty - sy0;
        swayMultiplier = 0.20f + curveIntensityVal * 0.0065f;

        leftP0 = { sx0, sy0 };
        leftP1 = { sx0 + deltaX * 0.12f * swayMultiplier, sy0 + deltaY * 0.38f };
        leftP2 = { (tx - halfPin) - deltaX * 0.08f * swayMultiplier, sy0 + deltaY * 0.78f };
        leftP3 = { tx - halfPin, ty };

        rightP0 = { sx1, sy0 };
        rightP1 = { sx1 + deltaX * 0.12f * swayMultiplier, sy0 + deltaY * 0.38f };
        rightP2 = { (tx + halfPin) - deltaX * 0.08f * swayMultiplier, sy0 + deltaY * 0.78f };
        rightP3 = { tx + halfPin, ty };
    }

    void SampleSpan(float vTex, float sigmaTop, float sigmaBot, float phase,
                    float& outX0, float& outX1, float& outY) const {
        const float sigma = std::clamp(sigmaTop + vTex * (sigmaBot - sigmaTop), 0.0f, 1.0f);
        const Vector2D ptLeft = EvaluateCubicBezier(leftP0, leftP1, leftP2, leftP3, sigma);
        const Vector2D ptRight = EvaluateCubicBezier(rightP0, rightP1, rightP2, rightP3, sigma);

        const float fluidWave = std::sin(vTex * 6.2831853f) * (phase * (1.0f - phase) * 4.0f) *
                                deltaX * 0.06f * swayMultiplier;

        outX0 = ptLeft.x + fluidWave;
        outX1 = std::max(outX0 + 1.0f, ptRight.x + fluidWave);
        outY = (ptLeft.y + ptRight.y) * 0.5f;
    }
};

struct Scanline {
    float x0 = 0.0f;
    float x1 = 0.0f;
    float v = 0.0f;
    bool active = false;
};

void RasterizeSurface(uint32_t* targetPixels, int canvasW, int canvasH,
                      const uint32_t* sourcePixels, int sourceW, int sourceH,
                      const GuideRails& rails, float collapseProgress) {
    if (!targetPixels || !sourcePixels || canvasW <= 0 || canvasH <= 0) return;

    memset(targetPixels, 0, static_cast<size_t>(canvasW) * canvasH * sizeof(uint32_t));

    const float phase = collapseProgress;
    const float sigmaBot = std::clamp(phase * 1.32f, 0.0f, 1.0f);
    const float sigmaTop = std::clamp((phase - 0.22f) / 0.78f, 0.0f, 1.0f);

    if (sigmaTop >= 0.999f) return;

    std::vector<Scanline> spans(canvasH);
    const int sampleSteps = std::clamp(canvasH * 2, 240, 1200);

    int minY = canvasH;
    int maxY = -1;

    for (int step = 0; step <= sampleSteps; ++step) {
        const float vTex = static_cast<float>(step) / static_cast<float>(sampleSteps);
        float x0 = 0.0f, x1 = 0.0f, yCoord = 0.0f;
        rails.SampleSpan(vTex, sigmaTop, sigmaBot, phase, x0, x1, yCoord);

        const int y = static_cast<int>(std::round(yCoord));
        if (y >= 0 && y < canvasH) {
            Scanline& span = spans[y];
            if (!span.active) {
                span.x0 = x0;
                span.x1 = x1;
                span.v = vTex;
                span.active = true;
            } else {
                span.x0 = std::min(span.x0, x0);
                span.x1 = std::max(span.x1, x1);
            }
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }

    if (minY > maxY) return;

    for (int y = minY + 1; y < maxY; ++y) {
        if (!spans[y].active) {
            int nextY = y + 1;
            while (nextY <= maxY && !spans[nextY].active) ++nextY;
            if (nextY <= maxY) {
                const float blend = static_cast<float>(y - (y - 1)) / static_cast<float>(nextY - (y - 1));
                spans[y].x0 = std::lerp(spans[y - 1].x0, spans[nextY].x0, blend);
                spans[y].x1 = std::lerp(spans[y - 1].x1, spans[nextY].x1, blend);
                spans[y].v = std::lerp(spans[y - 1].v, spans[nextY].v, blend);
                spans[y].active = true;
            }
        }
    }

    const int maxSrcX = std::max(0, sourceW - 1);
    const int maxSrcY = std::max(0, sourceH - 1);

    for (int y = minY; y <= maxY; ++y) {
        const Scanline& span = spans[y];
        if (!span.active) continue;

        const float spanWidth = span.x1 - span.x0;
        if (spanWidth <= 0.5f) continue;

        const float invSpan = 1.0f / spanWidth;
        const float vFloat = span.v * maxSrcY;
        const int v0 = std::clamp(static_cast<int>(vFloat), 0, maxSrcY);
        const int v1 = std::min(v0 + 1, maxSrcY);
        const int fv_int = static_cast<int>((vFloat - v0) * 256.0f);
        const int ifv_int = 256 - fv_int;

        const uint32_t* srcRow0 = sourcePixels + v0 * sourceW;
        const uint32_t* srcRow1 = sourcePixels + v1 * sourceW;

        const int startX = std::clamp(static_cast<int>(std::floor(span.x0)), 0, canvasW);
        const int endX = std::clamp(static_cast<int>(std::ceil(span.x1)), 0, canvasW);
        uint32_t* dstRow = targetPixels + y * canvasW;

        for (int x = startX; x < endX; ++x) {
            const float u = (static_cast<float>(x) - span.x0) * invSpan;
            if (u < 0.0f || u > 1.0f) continue;

            const float uFloat = u * maxSrcX;
            const int u0 = static_cast<int>(uFloat);
            const int u1 = std::min(u0 + 1, maxSrcX);
            const int fu_int = static_cast<int>((uFloat - u0) * 256.0f);
            const int ifu_int = 256 - fu_int;

            const uint32_t c00 = srcRow0[u0];
            const uint32_t c10 = srcRow0[u1];
            const uint32_t c01 = srcRow1[u0];
            const uint32_t c11 = srcRow1[u1];

            const int w00 = (ifu_int * ifv_int) >> 8;
            const int w10 = (fu_int * ifv_int) >> 8;
            const int w01 = (ifu_int * fv_int) >> 8;
            const int w11 = (fu_int * fv_int) >> 8;

            const uint32_t b = (((c00 & 0xFF) * w00 + (c10 & 0xFF) * w10 +
                                 (c01 & 0xFF) * w01 + (c11 & 0xFF) * w11) >> 8) & 0xFF;
            const uint32_t g = ((((c00 >> 8) & 0xFF) * w00 + ((c10 >> 8) & 0xFF) * w10 +
                                 ((c01 >> 8) & 0xFF) * w01 + ((c11 >> 8) & 0xFF) * w11) >> 8) & 0xFF;
            const uint32_t r = ((((c00 >> 16) & 0xFF) * w00 + ((c10 >> 16) & 0xFF) * w10 +
                                 ((c01 >> 16) & 0xFF) * w01 + ((c11 >> 16) & 0xFF) * w11) >> 8) & 0xFF;

            dstRow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

}

void PresentLayeredSurface(HWND ghostWnd, HDC screenDC, HDC memDC,
                           const POINT& position, const SIZE& size) {
    POINT srcPoint{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO ulwInfo{};
    ulwInfo.cbSize = sizeof(ulwInfo);
    ulwInfo.hdcDst = screenDC;
    ulwInfo.pptDst = &position;
    ulwInfo.psize = &size;
    ulwInfo.hdcSrc = memDC;
    ulwInfo.pptSrc = &srcPoint;
    ulwInfo.pblend = &blend;
    ulwInfo.dwFlags = ULW_ALPHA;
    UpdateLayeredWindowIndirect(ghostWnd, &ulwInfo);
}

DWORD WINAPI AnimateGenieWindow(void* parameter) {
    std::unique_ptr<Animation> data(static_cast<Animation*>(parameter));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    EnsureGhostClass();

    const int srcW = data->snapshot.width;
    const int srcH = data->snapshot.height;
    if (srcW <= 0 || srcH <= 0 || !data->snapshot.bits) {
        if (data->firstFrame) CloseHandle(data->firstFrame);
        EndAnimation(data->window);
        g_workers.fetch_sub(1, std::memory_order_release);
        return 0;
    }

    const float targetPinWidth = static_cast<float>(std::clamp(
        g_targetWidth.load(std::memory_order_relaxed), 12, 64));
    const float curveIntensityVal = static_cast<float>(
        g_curveIntensity.load(std::memory_order_relaxed));

    const float deltaX = data->targetX - (data->source.left + data->source.right) * 0.5f;
    const int swayMargin = static_cast<int>(std::ceil(std::abs(deltaX) * 0.22f + 32.0f));

    const int canvasLeft = std::min(static_cast<int>(data->source.left),
                                    static_cast<int>(data->targetX - targetPinWidth * 0.5f)) - swayMargin;
    const int canvasRight = std::max(static_cast<int>(data->source.right),
                                     static_cast<int>(data->targetX + targetPinWidth * 0.5f)) + swayMargin;
    const int canvasTop = std::min(static_cast<int>(data->source.top), data->targetY);
    const int canvasBottom = std::max(static_cast<int>(data->source.bottom), data->targetY) + 32;

    const int canvasW = std::max(1, canvasRight - canvasLeft);
    const int canvasH = std::max(1, canvasBottom - canvasTop);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = canvasW;
    bmi.bmiHeader.biHeight = -canvasH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* canvasBitsRaw = nullptr;
    HBITMAP canvasBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &canvasBitsRaw, nullptr, 0);
    HGDIOBJ oldBitmap = canvasBitmap ? SelectObject(memDC, canvasBitmap) : nullptr;
    auto* canvasBits = static_cast<uint32_t*>(canvasBitsRaw);

    HWND ghostWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kGhostClass, L"", WS_POPUP,
        canvasLeft, canvasTop, canvasW, canvasH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    GenieEngine::GuideRails rails;
    rails.Setup(data->source, static_cast<float>(data->targetX), static_cast<float>(data->targetY),
                targetPinWidth, static_cast<float>(canvasLeft), static_cast<float>(canvasTop),
                curveIntensityVal);

    const POINT windowOrigin{ canvasLeft, canvasTop };
    const SIZE canvasSize{ canvasW, canvasH };
    const auto* srcPixels = static_cast<const uint32_t*>(data->snapshot.bits);

    const float initialProgress = data->rising ? 1.0f : 0.0f;
    GenieEngine::RasterizeSurface(canvasBits, canvasW, canvasH, srcPixels, srcW, srcH, rails, initialProgress);
    if (ghostWnd) {
        PresentLayeredSurface(ghostWnd, screenDC, memDC, windowOrigin, canvasSize);
        ShowWindow(ghostWnd, SW_SHOWNOACTIVATE);
        DwmFlush();
    }
    if (data->firstFrame) {
        SetEvent(data->firstFrame);
    }

    LARGE_INTEGER timerFreq{}, animStart{};
    QueryPerformanceFrequency(&timerFreq);
    QueryPerformanceCounter(&animStart);

    int renderedFrames = 0;
    const double totalDuration = static_cast<double>(data->duration);

    while (ghostWnd && g_enabled.load(std::memory_order_relaxed)) {
        LARGE_INTEGER frameTime{};
        QueryPerformanceCounter(&frameTime);
        const double elapsedMs = static_cast<double>(frameTime.QuadPart - animStart.QuadPart) *
                                 1000.0 / static_cast<double>(timerFreq.QuadPart);
        const float linearProgress = std::clamp(static_cast<float>(elapsedMs / totalDuration), 0.0f, 1.0f);

        const float eased = GenieEngine::Kinematics::EvaluateEase(linearProgress, data->rising);
        const float collapse = data->rising ? (1.0f - eased) : eased;

        GenieEngine::RasterizeSurface(canvasBits, canvasW, canvasH, srcPixels, srcW, srcH, rails, collapse);
        PresentLayeredSurface(ghostWnd, screenDC, memDC, windowOrigin, canvasSize);
        DwmFlush();
        ++renderedFrames;

        if (linearProgress >= 1.0f) {
            break;
        }
    }

    if (ghostWnd) DestroyWindow(ghostWnd);
    if (oldBitmap) SelectObject(memDC, oldBitmap);
    if (canvasBitmap) DeleteObject(canvasBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (IsWindow(data->window)) {
        BOOL cloak = FALSE;
        DwmSetWindowAttribute(data->window, DWMWA_CLOAK, &cloak, sizeof(cloak));
        BOOL disabled = FALSE;
        DwmSetWindowAttribute(data->window, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
    }

    if (data->snapshot.bitmap) DeleteObject(data->snapshot.bitmap);
    if (data->firstFrame) CloseHandle(data->firstFrame);
    EndAnimation(data->window);
    g_workers.fetch_sub(1, std::memory_order_release);
    LogDebug("WindowAnimation: Genie animation completed frames=" + std::to_string(renderedFrames));
    return 0;
}

void Start(HWND window, bool rising, bool early) {
    LogDebug("WindowAnimation: start hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(window)) +
        " rising=" + std::to_string(rising));
    if (!ShouldAnimate(window)) {
        LogDebug("WindowAnimation: skipped by eligibility");
        return;
    }
    if (!BeginAnimation(window)) {
        LogDebug("WindowAnimation: duplicate event ignored");
        return;
    }
    RECT rect{};
    Snapshot snapshot;
    POINT cachedTarget{};
    bool cachedTargetValid = false;
    const bool usedCache = TakeCached(window, snapshot, rect, cachedTarget,
        cachedTargetValid);
    if (!usedCache && early && GetAnimationRect(window, rect))
        snapshot = Capture(window, rect, true);
    if (!snapshot.bitmap) {
        LogDebug("WindowAnimation: no visible snapshot, animation skipped");
        BOOL enabled = FALSE;
        DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
            &enabled, sizeof(enabled));
        EndAnimation(window);
        return;
    }
    if (!snapshot.bitmap) {
        LogDebug("WindowAnimation: capture failed error=" + std::to_string(GetLastError()));
        EndAnimation(window);
        return;
    }
    LogDebug("WindowAnimation: captured " + std::to_string(snapshot.width) + "x" +
        std::to_string(snapshot.height));

    auto data = std::make_unique<Animation>();
    data->window = window;
    data->snapshot = snapshot;
    data->source = rect;
    data->duration = g_duration.load(std::memory_order_relaxed);
    data->rising = rising;
    if (cachedTargetValid) {
        data->targetX = cachedTarget.x;
        data->targetY = cachedTarget.y;
        LogDebug("WindowAnimation: using cached taskbar target x=" +
            std::to_string(data->targetX) + " y=" + std::to_string(data->targetY));
    } else {
        FindTarget(window, rect, data->targetX, data->targetY);
    }
    data->firstFrame = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    BOOL transitionsDisabled = TRUE;
    DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
        &transitionsDisabled, sizeof(transitionsDisabled));
    if (rising) {
        BOOL cloak = TRUE;
        DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }

    HANDLE firstFrame = data->firstFrame;
    g_workers.fetch_add(1, std::memory_order_relaxed);
    Animation* rawData = data.release();
    HANDLE thread = CreateThread(nullptr, 0, AnimateGenieWindow, rawData, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        if (firstFrame) WaitForSingleObject(firstFrame, 80);
    } else {
        g_workers.fetch_sub(1, std::memory_order_release);
        if (rawData->firstFrame) CloseHandle(rawData->firstFrame);
        if (rawData->snapshot.bitmap) DeleteObject(rawData->snapshot.bitmap);
        delete rawData;
        BOOL enabled = FALSE;
        DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
            &enabled, sizeof(enabled));
        EndAnimation(window);
    }
}

void CALLBACK WinEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG objectId,
    LONG, DWORD, DWORD) {
    LogDebug("WindowAnimation: event=" + std::to_string(event) + " object=" +
        std::to_string(objectId) + " hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(window)));
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (event == EVENT_SYSTEM_MINIMIZESTART) {
        HWND root = GetAncestor(window, GA_ROOT);
        Start(root ? root : window, false, false);
    }
}

void CALLBACK ForegroundEvent(HWINEVENTHOOK, DWORD, HWND window, LONG objectId,
    LONG, DWORD, DWORD) {
    if (objectId == OBJID_WINDOW && g_enabled.load(std::memory_order_relaxed))
        QueueCache(window);
}
}

void Update(const AppConfig& config) {
    const bool enabled = config.windowGenieAnimation;
    g_duration.store(std::clamp(config.windowAnimationDuration, 150, 1000), std::memory_order_relaxed);
    g_curveIntensity.store(std::clamp(config.windowCurveIntensity, 50, 125), std::memory_order_relaxed);
    g_targetWidth.store(std::clamp(config.windowTargetWidth, 12, 64), std::memory_order_relaxed);
    g_repeatGuard.store(std::clamp(config.windowRepeatGuard, 0, 500), std::memory_order_relaxed);
    g_captureDelay.store(std::clamp(config.windowCaptureDelay, 50, 500), std::memory_order_relaxed);
    g_enabled.store(enabled, std::memory_order_release);
    if (enabled && !g_minimizeStart) {
        InstallEarlyHook();
        g_minimizeStart = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART,
            EVENT_SYSTEM_MINIMIZESTART, nullptr, WinEvent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        g_foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundEvent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        LogDebug("WindowAnimation: hooks installed=" + std::to_string(g_minimizeStart != nullptr));
        QueueCache(GetForegroundWindow());
    } else if (!enabled) {
        UninstallEarlyHook();
        if (g_minimizeStart) UnhookWinEvent(g_minimizeStart);
        if (g_foreground) UnhookWinEvent(g_foreground);
        g_minimizeStart = nullptr;
        g_foreground = nullptr;
        g_cacheGeneration.fetch_add(1, std::memory_order_release);
        std::lock_guard lock(g_cacheMutex);
        if (g_cachedWindow && IsWindow(g_cachedWindow)) {
            BOOL transitionsEnabled = FALSE;
            DwmSetWindowAttribute(g_cachedWindow, DWMWA_TRANSITIONS_FORCEDISABLED,
                &transitionsEnabled, sizeof(transitionsEnabled));
        }
        if (g_cachedSnapshot.bitmap) DeleteObject(g_cachedSnapshot.bitmap);
        g_cachedSnapshot = {};
        g_cachedWindow = nullptr;
        {
            std::lock_guard activeLock(g_activeMutex);
            g_activeAnimations.clear();
            g_lastAnimationStarted.clear();
        }
    }
}

void Shutdown() {
    AppConfig disabled;
    disabled.windowGenieAnimation = false;
    Update(disabled);
    for (int i = 0; i < 100 && g_workers.load(std::memory_order_acquire) > 0; ++i) Sleep(10);
}
}
