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

bool SendKey(WORD key, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = flags;
    const bool sent = SendInput(1, &input, sizeof(input)) == 1;
    Sleep(3);
    return sent;
}

bool SendWinT() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LWIN;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'T';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'T';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_LWIN;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT)) == ARRAYSIZE(inputs);
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
        return true;
    }

    const BOOL foregroundResult = SetForegroundWindow(taskbar);
    WriteDiagnostic("explorer", "DragDrop SetForegroundWindow=" + std::to_string(foregroundResult));
    Sleep(10);
    bool ok = SendWinT();
    if (ok) {
        Sleep(20);
        for (unsigned int i = 0; i < index; ++i) {
            ok &= SendKey(VK_RIGHT, 0);
            ok &= SendKey(VK_RIGHT, KEYEVENTF_KEYUP);
        }
    }
    ok &= SendKey(VK_UP, 0);
    ok &= SendKey(VK_UP, KEYEVENTF_KEYUP);
    Sleep(10);
    WriteDiagnostic("explorer", WindowState("DragDrop after Up"));
    ok &= SendKey(VK_RETURN, 0);
    ok &= SendKey(VK_RETURN, KEYEVENTF_KEYUP);
    Sleep(80);
    WriteDiagnostic("explorer", WindowState("DragDrop after Enter"));
    return ok;
}

DWORD WINAPI Watcher(void*) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* automation = nullptr;
    CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    bool buttonWasDown = false;
    bool startedOnTaskbar = false;
    bool dragSession = false;
    ULONGLONG buttonUpSince = 0;
    void* hoverTaskbar = nullptr;
    int hoverIndex = -1;
    POINT hoverAnchor{};
    ULONGLONG hoverStarted = 0;
    bool activationArmed = true;
    void* activatedTaskbar = nullptr;
    int activatedIndex = -1;
    POINT activationPoint{};

    while (enabled.load(std::memory_order_acquire)) {
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        const auto tickNow = GetTickCount64();
        if (buttonDown && !buttonWasDown) {
            if (!dragSession) {
                startedOnTaskbar = false;
                activationArmed = true;
                activatedTaskbar = nullptr;
                activatedIndex = -1;
                std::lock_guard<std::mutex> lock(targetsMutex);
                for (const auto& pair : targetSets) {
                    if (Contains(pair.second.taskbarRect, cursor)) {
                        startedOnTaskbar = true;
                        break;
                    }
                }
                dragSession = !startedOnTaskbar;
            } else {
                startedOnTaskbar = false;
            }
            buttonUpSince = 0;
            WriteDiagnostic("explorer", "DragDrop mouse down x=" + std::to_string(cursor.x) +
                " y=" + std::to_string(cursor.y) + " startedOnTaskbar=" +
                std::to_string(startedOnTaskbar) + " continuation=" +
                std::to_string(dragSession) + " sets=" + std::to_string(targetSets.size()));
        }

        if (!buttonDown) {
            if (!buttonUpSince) buttonUpSince = tickNow;
            if (dragSession && tickNow - buttonUpSince >= 500) {
                dragSession = false;
                activationArmed = true;
                activatedTaskbar = nullptr;
                activatedIndex = -1;
            }
        }

        if (!buttonDown || !dragSession || startedOnTaskbar) {
            hoverTaskbar = nullptr;
            hoverIndex = -1;
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
            if (!activationArmed &&
                (abs(cursor.x - activationPoint.x) > 8 || abs(cursor.y - activationPoint.y) > 8)) {
                activationArmed = true;
            }
            if (foundTaskbar != hoverTaskbar || foundIndex != hoverIndex) {
                hoverTaskbar = foundTaskbar;
                hoverIndex = foundIndex;
                hoverAnchor = cursor;
                hoverStarted = now;
                if (foundIndex >= 0) {
                    WriteDiagnostic("explorer", "DragDrop hover target index=" +
                        std::to_string(foundIndex) + " x=" + std::to_string(cursor.x) +
                        " y=" + std::to_string(cursor.y));
                }
            } else if (foundIndex >= 0 &&
                (abs(cursor.x - hoverAnchor.x) > 5 || abs(cursor.y - hoverAnchor.y) > 5)) {
                hoverAnchor = cursor;
                hoverStarted = now;
            } else if (foundIndex >= 0 && activationArmed &&
                (foundTaskbar != activatedTaskbar || foundIndex != activatedIndex) &&
                now - hoverStarted >= 300) {
                activationArmed = false;
                activatedTaskbar = foundTaskbar;
                activatedIndex = foundIndex;
                activationPoint = cursor;
                const bool result = Activate(automation, cursor, foundWindow,
                    static_cast<unsigned int>(foundIndex));
                WriteDiagnostic("explorer", "DragDrop watcher activation index=" +
                    std::to_string(foundIndex) + " result=" + std::to_string(result));
            }
        }

        buttonWasDown = buttonDown;
        const DWORD sleepDuration = (buttonDown || dragSession) ? 15 : 60;
        Sleep(sleepDuration);
    }

    if (automation) automation->Release();
    if (SUCCEEDED(apartment)) CoUninitialize();
    workerRunning.store(false, std::memory_order_release);
    return 0;
}

void StartWatcher() {
    if (workerRunning.exchange(true, std::memory_order_acq_rel)) return;
    HANDLE thread = CreateThread(nullptr, 64 * 1024, Watcher, nullptr,
        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread) CloseHandle(thread);
    else workerRunning.store(false, std::memory_order_release);
}
}

void Lightency::DragDropAssist::SetEnabled(bool value) {
    enabled.store(value, std::memory_order_release);
    WriteDiagnostic("explorer", "DragDrop watcher enabled=" + std::to_string(value));
    if (value) StartWatcher();
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
