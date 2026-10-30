#include "drag_drop_assist.h"
#include "../common/diagnostics.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cwctype>
#include <unknwn.h>
#include <objbase.h>
#include <uiautomation.h>
#include <propsys.h>
#include <propkey.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
struct TargetSet {
    HWND window = nullptr;
    RECT taskbarRect{};
    std::vector<RECT> apps;
    LONG appsLeft = LONG_MAX;
    LONG appsRight = LONG_MIN;
};

std::mutex targetsMutex;
std::unordered_map<void*, TargetSet> targetSets;
std::atomic<bool> enabled{false};
std::atomic<bool> workerRunning{false};

bool Contains(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}

std::string Utf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring Lower(std::wstring value) {
    for (auto& ch : value) ch = static_cast<wchar_t>(towlower(ch));
    return value;
}

struct WindowMatch {
    std::wstring buttonName;
    std::wstring appId;
    HWND window = nullptr;
    int score = 0;
};

BOOL CALLBACK FindMatchingWindow(HWND window, LPARAM parameter) {
    auto& match = *reinterpret_cast<WindowMatch*>(parameter);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    wchar_t className[128]{};
    GetClassNameW(window, className, ARRAYSIZE(className));
    if (wcscmp(className, L"Shell_TrayWnd") == 0 ||
        wcscmp(className, L"XamlExplorerHostIslandWindow") == 0) return TRUE;

    wchar_t titleBuffer[512]{};
    GetWindowTextW(window, titleBuffer, ARRAYSIZE(titleBuffer));
    const std::wstring title = Lower(titleBuffer);
    int score = 0;
    if (match.appId == L"microsoft.windows.explorer" &&
        wcscmp(className, L"CabinetWClass") == 0) {
        score = 5000;
    }

    using GetStoreFn = HRESULT(WINAPI*)(HWND, REFIID, void**);
    static const auto getStore = reinterpret_cast<GetStoreFn>(GetProcAddress(
        GetModuleHandleW(L"shell32.dll"), "SHGetPropertyStoreForWindow"));
    if (getStore && !match.appId.empty()) {
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(getStore(window, IID_PPV_ARGS(&store))) && store) {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal &&
                Lower(value.pwszVal) == match.appId) {
                score = 5000;
            }
            PropVariantClear(&value);
            store->Release();
        }
    }
    if (!title.empty() && (match.buttonName.find(title) != std::wstring::npos ||
            title.find(match.buttonName) != std::wstring::npos)) {
        score = 2000 + static_cast<int>(title.size());
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process) {
        wchar_t path[1024]{};
        DWORD pathSize = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(process, 0, path, &pathSize)) {
            const wchar_t* file = wcsrchr(path, L'\\');
            file = file ? file + 1 : path;
            std::wstring executable = Lower(file);
            if (const auto dot = executable.rfind(L'.'); dot != std::wstring::npos)
                executable.resize(dot);
            if (executable.size() > 2 && match.buttonName.find(executable) != std::wstring::npos) {
                const int processScore = 1000 + static_cast<int>(executable.size());
                if (processScore > score) score = processScore;
            }
        }
        CloseHandle(process);
    }
    if (score > match.score) {
        match.score = score;
        match.window = window;
    }
    return TRUE;
}

HWND ResolveTaskbarWindow(const wchar_t* buttonName, const wchar_t* automationId) {
    WindowMatch match{};
    match.buttonName = Lower(buttonName ? buttonName : L"");
    match.appId = Lower(automationId ? automationId : L"");
    if (match.appId.rfind(L"appid: ", 0) == 0) match.appId.erase(0, 7);
    const auto suffix = match.buttonName.find(L"\u00a0\u2014");
    if (suffix != std::wstring::npos) match.buttonName.resize(suffix);
    EnumWindows(FindMatchingWindow, reinterpret_cast<LPARAM>(&match));
    return match.window;
}

bool ActivateWindow(HWND window) {
    if (!IsWindow(window)) return false;
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);

    using SwitchFn = void(WINAPI*)(HWND, BOOL);
    static const auto switchTo = reinterpret_cast<SwitchFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SwitchToThisWindow"));
    if (switchTo) switchTo(window, TRUE);

    const DWORD currentThread = GetCurrentThreadId();
    const DWORD targetThread = GetWindowThreadProcessId(window, nullptr);
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(currentThread, foregroundThread, TRUE);
    if (targetThread && targetThread != currentThread)
        AttachThreadInput(currentThread, targetThread, TRUE);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    if (targetThread && targetThread != currentThread)
        AttachThreadInput(currentThread, targetThread, FALSE);
    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(currentThread, foregroundThread, FALSE);

    Sleep(35);
    const HWND active = GetForegroundWindow();
    return active == window || GetAncestor(active, GA_ROOTOWNER) == GetAncestor(window, GA_ROOTOWNER);
}

std::string WindowState(const char* stage) {
    HWND window = GetForegroundWindow();
    wchar_t className[128]{};
    wchar_t title[256]{};
    if (window) {
        GetClassNameW(window, className, ARRAYSIZE(className));
        GetWindowTextW(window, title, ARRAYSIZE(title));
    }
    char buffer[900]{};
    sprintf_s(buffer, "%s hwnd=%p class=%ls title=%ls", stage, window, className, title);
    return buffer;
}

bool ActivateTaskbarButton(IUIAutomation* automation, POINT point) {
    if (!automation) return false;
    IUIAutomationElement* element = nullptr;
    if (FAILED(automation->ElementFromPoint(point, &element)) || !element) return false;
    IUIAutomationTreeWalker* walker = nullptr;
    automation->get_ControlViewWalker(&walker);
    bool activated = false;
    for (int depth = 0; element && depth < 10 && !activated; ++depth) {
        CONTROLTYPEID type = 0;
        element->get_CurrentControlType(&type);
        if (type == UIA_ButtonControlTypeId) {
            BSTR name = nullptr;
            BSTR automationId = nullptr;
            element->get_CurrentName(&name);
            element->get_CurrentAutomationId(&automationId);
            WriteDiagnostic("explorer", "DragDrop exact button name=" +
                Utf8(name) + " id=" + Utf8(automationId));
            HWND targetWindow = ResolveTaskbarWindow(name, automationId);
            if (targetWindow) {
                activated = ActivateWindow(targetWindow);
                WriteDiagnostic("explorer", "DragDrop resolved hwnd=" +
                    std::to_string(reinterpret_cast<uintptr_t>(targetWindow)) +
                    " activated=" + std::to_string(activated));
            }
            if (name) SysFreeString(name);
            if (automationId) SysFreeString(automationId);

            IUIAutomationInvokePattern* invoke = nullptr;
            if (!activated && SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId,
                    IID_PPV_ARGS(&invoke))) && invoke) {
                activated = SUCCEEDED(invoke->Invoke());
                invoke->Release();
            }
            if (!activated) {
                IUIAutomationLegacyIAccessiblePattern* legacy = nullptr;
                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                        IID_PPV_ARGS(&legacy))) && legacy) {
                    activated = SUCCEEDED(legacy->DoDefaultAction());
                    legacy->Release();
                }
            }
        }
        if (!activated && walker) {
            IUIAutomationElement* parent = nullptr;
            walker->GetParentElement(element, &parent);
            element->Release();
            element = parent;
        }
    }
    if (element) element->Release();
    if (walker) walker->Release();
    return activated;
}

bool Activate(IUIAutomation* automation, POINT center, HWND taskbar, unsigned int index) {
    if (!IsWindow(taskbar) || index > 60) return false;
    WriteDiagnostic("explorer", WindowState("DragDrop before activation"));
    bool directActivation = ActivateTaskbarButton(automation, center);
    WriteDiagnostic("explorer", "DragDrop direct activation=" + std::to_string(directActivation));
    if (directActivation) {
        Sleep(120);
        WriteDiagnostic("explorer", WindowState("DragDrop after direct activation"));
    }
    return directActivation;
}

static IUIAutomation* g_automation = nullptr;
static HHOOK g_mouseHook = nullptr;
static bool g_buttonWasDown = false;
static bool g_startedOnTaskbar = false;
static bool g_dragSession = false;
static ULONGLONG g_buttonUpSince = 0;
static void* g_hoverTaskbar = nullptr;
static int g_hoverIndex = -1;
static POINT g_hoverAnchor{};
static ULONGLONG g_hoverStarted = 0;
static bool g_activationArmed = true;
static void* g_activatedTaskbar = nullptr;
static int g_activatedIndex = -1;
static POINT g_activationPoint{};

LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && enabled.load(std::memory_order_acquire)) {
        if (wParam == WM_MOUSEMOVE || wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP ||
            wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP) {
            
            MSLLHOOKSTRUCT* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            POINT cursor = pMouse->pt;
            
            const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

            const auto tickNow = GetTickCount64();
            if (buttonDown && !g_buttonWasDown) {
                if (!g_dragSession) {
                    g_startedOnTaskbar = false;
                    g_activationArmed = true;
                    g_activatedTaskbar = nullptr;
                    g_activatedIndex = -1;
                    std::lock_guard<std::mutex> lock(targetsMutex);
                    for (const auto& pair : targetSets) {
                        if (Contains(pair.second.taskbarRect, cursor)) {
                            g_startedOnTaskbar = true;
                            break;
                        }
                    }
                    g_dragSession = !g_startedOnTaskbar;
                } else {
                    g_startedOnTaskbar = false;
                }
                g_buttonUpSince = 0;
            }

            if (!buttonDown) {
                if (!g_buttonUpSince) g_buttonUpSince = tickNow;
                if (g_dragSession && tickNow - g_buttonUpSince >= 500) {
                    g_dragSession = false;
                    g_activationArmed = true;
                    g_activatedTaskbar = nullptr;
                    g_activatedIndex = -1;
                }
            }

            if (!buttonDown || !g_dragSession || g_startedOnTaskbar) {
                g_hoverTaskbar = nullptr;
                g_hoverIndex = -1;
            } else {
                void* foundTaskbar = nullptr;
                int foundIndex = -1;
                HWND foundWindow = nullptr;
                POINT foundCenter{};
                {
                    std::lock_guard<std::mutex> lock(targetsMutex);
                    for (const auto& pair : targetSets) {
                        long long bestDistance = LLONG_MAX;
                        const bool insideAppStrip = Contains(pair.second.taskbarRect, cursor) &&
                            cursor.x >= pair.second.appsLeft && cursor.x < pair.second.appsRight;
                        if (insideAppStrip) {
                            for (size_t i = 0; i < pair.second.apps.size(); ++i) {
                                POINT center{
                                    (pair.second.apps[i].left + pair.second.apps[i].right) / 2,
                                    (pair.second.apps[i].top + pair.second.apps[i].bottom) / 2
                                };
                                const long long dx = cursor.x - center.x;
                                const long long dy = cursor.y - center.y;
                                const long long distance = dx * dx + dy * dy;
                                if (distance < bestDistance) {
                                    bestDistance = distance;
                                    foundTaskbar = pair.first;
                                    foundIndex = static_cast<int>(i);
                                    foundWindow = pair.second.window;
                                    foundCenter = center;
                                }
                            }
                        }
                        if (foundIndex >= 0) break;
                    }
                }

                const auto now = GetTickCount64();
                if (!g_activationArmed &&
                    (abs(cursor.x - g_activationPoint.x) > 8 || abs(cursor.y - g_activationPoint.y) > 8)) {
                    g_activationArmed = true;
                }
                if (foundTaskbar != g_hoverTaskbar || foundIndex != g_hoverIndex) {
                    g_hoverTaskbar = foundTaskbar;
                    g_hoverIndex = foundIndex;
                    g_hoverAnchor = cursor;
                    g_hoverStarted = now;
                } else if (foundIndex >= 0 &&
                    (abs(cursor.x - g_hoverAnchor.x) > 5 || abs(cursor.y - g_hoverAnchor.y) > 5)) {
                    g_hoverAnchor = cursor;
                    g_hoverStarted = now;
                } else if (foundIndex >= 0 && g_activationArmed &&
                    (foundTaskbar != g_activatedTaskbar || foundIndex != g_activatedIndex) &&
                    now - g_hoverStarted >= 300) {
                    g_activationArmed = false;
                    g_activatedTaskbar = foundTaskbar;
                    g_activatedIndex = foundIndex;
                    g_activationPoint = cursor;
                    Activate(g_automation, cursor, foundWindow, static_cast<unsigned int>(foundIndex));
                }
            }
            g_buttonWasDown = buttonDown;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

DWORD WINAPI Watcher(void*) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g_automation));

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, GetModuleHandleW(nullptr), 0);
    
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!enabled.load(std::memory_order_acquire)) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }

    if (g_automation) {
        g_automation->Release();
        g_automation = nullptr;
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    workerRunning.store(false, std::memory_order_release);
    return 0;
}

static DWORD g_watcherThreadId = 0;

void StartWatcher() {
    if (workerRunning.exchange(true, std::memory_order_acq_rel)) return;
    HANDLE thread = CreateThread(nullptr, 64 * 1024, Watcher, nullptr,
        STACK_SIZE_PARAM_IS_A_RESERVATION, &g_watcherThreadId);
    if (thread) CloseHandle(thread);
    else workerRunning.store(false, std::memory_order_release);
}
}

void Lightency::DragDropAssist::SetEnabled(bool value) {
    enabled.store(value, std::memory_order_release);
    WriteDiagnostic("explorer", "DragDrop watcher enabled=" + std::to_string(value));
    if (value) {
        StartWatcher();
    } else if (g_watcherThreadId) {
        PostThreadMessageW(g_watcherThreadId, WM_QUIT, 0, 0);
        g_watcherThreadId = 0;
    }
}

void Lightency::DragDropAssist::UpdateTargets(void* taskbar, HWND window,
    const std::vector<RECT>& targets) {
    if (!taskbar || !IsWindow(window)) return;
    TargetSet set;
    set.window = window;
    GetWindowRect(window, &set.taskbarRect);
    set.apps = targets;
    for (const auto& rect : targets) {
        if (rect.left < set.appsLeft) set.appsLeft = rect.left;
        if (rect.right > set.appsRight) set.appsRight = rect.right;
    }
    std::lock_guard<std::mutex> lock(targetsMutex);
    targetSets[taskbar] = std::move(set);
    WriteDiagnostic("explorer", "DragDrop watcher targets=" + std::to_string(targets.size()));
}

void Lightency::DragDropAssist::Remove(void* taskbar) {
    std::lock_guard<std::mutex> lock(targetsMutex);
    targetSets.erase(taskbar);
}

void Lightency::DragDropAssist::Shutdown() {
    enabled.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(targetsMutex);
    targetSets.clear();
}
