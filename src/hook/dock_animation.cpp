#include "dock_animation.h"
#include "minhook/MinHook.h"
#include <unknwn.h>
#include <objbase.h>
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <dbghelp.h>
#include <stdio.h>
#include <string>
#include <vector>

#define WH_MOD_ID L"lightency_dock"

static void LogDock(const std::wstring& msg) {
#ifdef _DEBUG
    OutputDebugStringW((msg + L"\\n").c_str());
#else
    (void)msg;
#endif
}

#define Wh_Log(fmt, ...) do {     wchar_t buf[512];     swprintf_s(buf, fmt, ##__VA_ARGS__);     LogDock(buf); } while(0)

static Lightency::SharedHookConfig g_lightencyDockConfig = {
    true, 135, 45, 50, 0, false, true, true, true,
    false, false, false, false, false, false, false, false,
    true, true, 0
};
static HANDLE s_hConfigMap = NULL;
static Lightency::SharedHookConfig* s_pLiveSharedConfig = nullptr;
void LoadSettings();
static void RestoreDefaultLayout();

void Wh_ModSettingsChanged();

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
            s_hConfigMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Lightency_Shared_Config_v7");
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
            newLayoutEditor != g_lightencyDockConfig.layoutEditor ||
            newDockAnim != g_lightencyDockConfig.dockAnimation) {
            bool animToggledOff = (g_lightencyDockConfig.dockAnimation && !newDockAnim);
            bool layoutToggledOff = (g_lightencyDockConfig.layoutEditor && !newLayoutEditor);
            g_lightencyDockConfig = *s_pLiveSharedConfig;
            g_lightencyDockConfig.dockAnimation = newDockAnim;
            g_lightencyDockConfig.layoutEditor = newLayoutEditor;
            Wh_Log(L"DockAnimation: Live config updated: scale=%d radius=%d anim=%d",
                   g_lightencyDockConfig.maxScale, g_lightencyDockConfig.effectRadius, g_lightencyDockConfig.dockAnimation);
            LoadSettings();


            if (layoutToggledOff) {
                RestoreDefaultLayout();
            }
            if (animToggledOff || !newDockAnim) {
                Wh_ModSettingsChanged();
            }
        }
    }
}

namespace WindhawkUtils {
    template <typename T, typename D, typename O>
    inline bool SetFunctionHook(T target, D detour, O original) {
        if (!target) return false;
        if (MH_CreateHook(reinterpret_cast<LPVOID>(target),
                          reinterpret_cast<LPVOID>(detour),
                          reinterpret_cast<LPVOID*>(original)) == MH_OK) {
            return MH_EnableHook(reinterpret_cast<LPVOID>(target)) == MH_OK;
        }
        return false;
    }

    struct SYMBOL_HOOK {
        struct {
            const wchar_t* name;
        } symbols[1];
        void* pOriginal;
        void* hook;
    };
}

static bool HookSymbols(HMODULE module, WindhawkUtils::SYMBOL_HOOK* hooks, size_t count,
                        const wchar_t* typeName) {
    if (!module || !hooks || count == 0) {
        LogDock(L"HookSymbols: invalid arguments");
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const DWORD imageTimestamp = nt->FileHeader.TimeDateStamp;

    wchar_t cacheBase[MAX_PATH] = {};
    DWORD cacheBaseLen = GetEnvironmentVariableW(
        L"LOCALAPPDATA", cacheBase, ARRAYSIZE(cacheBase));
    std::wstring offsetCachePath;
    if (cacheBaseLen > 0 && cacheBaseLen < ARRAYSIZE(cacheBase)) {
        std::wstring appDir = std::wstring(cacheBase) + L"\\Lightency";
        CreateDirectoryW(appDir.c_str(), nullptr);
        offsetCachePath = appDir + L"\\hook_offsets.ini";
    }

    uint64_t typeHash = 1469598103934665603ull;
    for (const wchar_t* p = typeName; *p; ++p) {
        typeHash ^= static_cast<uint16_t>(*p);
        typeHash *= 1099511628211ull;
    }
    const std::wstring cacheSection = L"image_" + std::to_wstring(imageTimestamp) +
        L"_" + std::to_wstring(imageSize);
    auto cacheKey = [&](size_t index) {
        return L"hook_" + std::to_wstring(typeHash) + L"_" + std::to_wstring(index);
    };
    auto isExecutableAddress = [](const void* address) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        const DWORD protection = mbi.Protect & 0xff;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    };


    if (!offsetCachePath.empty()) {
        std::vector<DWORD64> cachedAddresses(count, 0);
        bool cacheComplete = true;
        for (size_t i = 0; i < count; ++i) {
            wchar_t value[32] = {};
            GetPrivateProfileStringW(cacheSection.c_str(), cacheKey(i).c_str(), L"",
                                     value, ARRAYSIZE(value), offsetCachePath.c_str());
            wchar_t* end = nullptr;
            const unsigned long long rva = wcstoull(value, &end, 16);
            if (!value[0] || !end || *end || rva == 0 || rva >= imageSize) {
                cacheComplete = false;
                break;
            }
            const auto address = reinterpret_cast<DWORD64>(module) + rva;
            if (!isExecutableAddress(reinterpret_cast<const void*>(address))) {
                cacheComplete = false;
                break;
            }
            cachedAddresses[i] = address;
        }
        if (cacheComplete) {
            bool allHooked = true;
            for (size_t i = 0; i < count; ++i) {
                void* target = reinterpret_cast<void*>(cachedAddresses[i]);
                const MH_STATUS createStatus = MH_CreateHook(
                    target, hooks[i].hook, reinterpret_cast<LPVOID*>(hooks[i].pOriginal));
                const MH_STATUS enableStatus = createStatus == MH_OK
                    ? MH_EnableHook(target) : createStatus;
                if (createStatus != MH_OK || enableStatus != MH_OK) allHooked = false;
            }
            if (allHooked) {
                LogDock(L"HookSymbols: validated offset cache hit");
            }
            return allHooked;
        }
    }

    HANDLE hProcess = GetCurrentProcess();
    static bool s_symInited = false;
    static decltype(&SymSetOptions) pSymSetOptions = nullptr;
    static decltype(&SymInitializeW) pSymInitializeW = nullptr;
    static decltype(&SymLoadModuleExW) pSymLoadModuleExW = nullptr;
    static decltype(&SymEnumSymbolsW) pSymEnumSymbolsW = nullptr;
    if (!s_symInited) {
        wchar_t curDir[MAX_PATH] = {};
        HMODULE hThis = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&HookSymbols, &hThis);
        if (hThis) {
            GetModuleFileNameW(hThis, curDir, MAX_PATH);
            wchar_t* pLast = wcsrchr(curDir, L'\\');
            if (pLast) *pLast = L'\0';
        }

        std::wstring dbgHelpPath = std::wstring(curDir) + L"\\dbghelp.dll";
        HMODULE dbgHelp = LoadLibraryExW(
            dbgHelpPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dbgHelp) dbgHelp = LoadLibraryW(L"dbghelp.dll");
        if (!dbgHelp) return false;

        pSymSetOptions = reinterpret_cast<decltype(pSymSetOptions)>(
            GetProcAddress(dbgHelp, "SymSetOptions"));
        pSymInitializeW = reinterpret_cast<decltype(pSymInitializeW)>(
            GetProcAddress(dbgHelp, "SymInitializeW"));
        pSymLoadModuleExW = reinterpret_cast<decltype(pSymLoadModuleExW)>(
            GetProcAddress(dbgHelp, "SymLoadModuleExW"));
        pSymEnumSymbolsW = reinterpret_cast<decltype(pSymEnumSymbolsW)>(
            GetProcAddress(dbgHelp, "SymEnumSymbolsW"));
        if (!pSymSetOptions || !pSymInitializeW || !pSymLoadModuleExW ||
            !pSymEnumSymbolsW) return false;

        pSymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_AUTO_PUBLICS |
                       SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);


        std::wstring symSrvPath = std::wstring(curDir) + L"\\symsrv.dll";
        LoadLibraryW(symSrvPath.c_str());

        wchar_t localAppData[MAX_PATH] = {};
        DWORD localAppDataLen = GetEnvironmentVariableW(
            L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
        std::wstring cacheDir;
        if (localAppDataLen > 0 && localAppDataLen < ARRAYSIZE(localAppData)) {
            std::wstring appDir = std::wstring(localAppData) + L"\\Lightency";
            CreateDirectoryW(appDir.c_str(), nullptr);
            cacheDir = appDir + L"\\symbols";
        } else {
            cacheDir = std::wstring(curDir) + L"\\symbols";
        }
        CreateDirectoryW(cacheDir.c_str(), nullptr);

        std::wstring searchPath = std::wstring(curDir) + L";" + cacheDir +
            L";SRV*" + cacheDir + L"*https://msdl.microsoft.com/download/symbols";
        BOOL bInit = pSymInitializeW(hProcess, searchPath.c_str(), FALSE);
        LogDock(std::wstring(L"SymInitializeW: bInit=") + std::to_wstring(bInit));
        if (!bInit) return false;
        s_symInited = true;
    }

    wchar_t modPath[MAX_PATH] = {};
    GetModuleFileNameW(module, modPath, MAX_PATH);
    DWORD64 base = pSymLoadModuleExW(hProcess, NULL, modPath, NULL, (DWORD64)module, 0, NULL, 0);
    LogDock(std::wstring(L"SymLoadModuleExW base=") + std::to_wstring((unsigned long long)base));
    if (!base) base = (DWORD64)module;

    struct EnumCtx {
        bool isMoved;
        DWORD64 foundAddr;
        std::wstring matchedName;
        const wchar_t* typeName;
    };

    auto EnumSym = [](PSYMBOL_INFOW pSymInfo, ULONG, PVOID userCtx) -> BOOL {
        EnumCtx* pCtx = (EnumCtx*)userCtx;
        if (pSymInfo->Name[0] != L'`' &&
            wcsstr(pSymInfo->Name, L"winrt::impl::produce<") &&
            wcsstr(pSymInfo->Name, pCtx->typeName) &&
            !wcsstr(pSymInfo->Name, L"catch") &&
            !wcsstr(pSymInfo->Name, L"dtor") &&
            !wcsstr(pSymInfo->Name, L"lambda")) {
            if (pCtx->isMoved && wcsstr(pSymInfo->Name, L"OnPointerMoved")) {
                pCtx->foundAddr = pSymInfo->Address;
                pCtx->matchedName = pSymInfo->Name;
                return FALSE;
            }
            if (!pCtx->isMoved && wcsstr(pSymInfo->Name, L"OnPointerExited")) {
                pCtx->foundAddr = pSymInfo->Address;
                pCtx->matchedName = pSymInfo->Name;
                return FALSE;
            }
        }
        return TRUE;
    };

    bool allHooked = true;
    for (size_t i = 0; i < count; i++) {
        EnumCtx ctx = {};
        ctx.isMoved = (i == 0);
        ctx.typeName = typeName;
        std::wstring mask = std::wstring(L"*produce*") + typeName +
            (i == 0 ? L"*OnPointerMoved*" : L"*OnPointerExited*");
        pSymEnumSymbolsW(hProcess, base, mask.c_str(), (PSYM_ENUMERATESYMBOLS_CALLBACKW)EnumSym, &ctx);
        LogDock(std::wstring(L"SymEnumSymbolsW [") + std::to_wstring(i) + L"] name=" + ctx.matchedName + L" foundAddr=" + std::to_wstring((unsigned long long)ctx.foundAddr) + L" RVA=0x" + std::to_wstring((unsigned long long)(ctx.foundAddr ? ctx.foundAddr - base : 0)));

        if (ctx.foundAddr) {
            void* pTarget = (void*)ctx.foundAddr;
            MH_STATUS sCreate = MH_CreateHook(pTarget, hooks[i].hook, (LPVOID*)hooks[i].pOriginal);
            MH_STATUS sEnable = MH_EnableHook(pTarget);
            LogDock(std::wstring(L"MH_CreateHook=") + std::to_wstring(sCreate) + L" MH_EnableHook=" + std::to_wstring(sEnable));
            if (sCreate != MH_OK || sEnable != MH_OK) {
                allHooked = false;
            } else if (!offsetCachePath.empty()) {
                wchar_t rvaValue[32] = {};
                swprintf_s(rvaValue, L"%llX",
                           static_cast<unsigned long long>(ctx.foundAddr - base));
                WritePrivateProfileStringW(cacheSection.c_str(), cacheKey(i).c_str(),
                                           rvaValue, offsetCachePath.c_str());
            }
        } else {
            allHooked = false;
        }
    }
    return allHooked;
}


#undef GetCurrentTime

#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include <winrt/Windows.Foundation.h>

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
    Visibility originalVisibility = Visibility::Visible;
};

static std::unordered_map<void*, TrayVisibilityState> g_trayVisibilityStates;

struct TaskbarAppearanceState {
    winrt::weak_ref<winrt::Windows::UI::Xaml::Shapes::Shape> element;
    Brush originalFill{nullptr};
    bool border = false;
};

static std::unordered_map<void*, TaskbarAppearanceState> g_taskbarAppearanceStates;

static std::wstring TrayToLower(std::wstring value);
static bool ContainsAny(const std::wstring& value,
                        std::initializer_list<const wchar_t*> needles);

static void RestoreTaskbarAppearance() {
    for (auto& [key, state] : g_taskbarAppearanceStates) {
        if (auto shape = state.element.get()) {
            try {
                shape.Fill(state.originalFill);
            } catch (...) {}
        }
    }
    g_taskbarAppearanceStates.clear();
}

static void ApplyTaskbarAppearance(FrameworkElement const& taskbarElement) {
    if (!taskbarElement) return;
    try {
        FrameworkElement root = taskbarElement;
        while (auto parent = VisualTreeHelper::GetParent(root).try_as<FrameworkElement>()) {
            root = parent;
        }

        std::vector<FrameworkElement> stack{root};
        while (!stack.empty()) {
            FrameworkElement element = stack.back();
            stack.pop_back();

            const std::wstring name = element.Name().c_str();
            const bool background = name == L"BackgroundFill";
            const bool border = name == L"BackgroundStroke";
            if (background || border) {
                if (auto shape = element.try_as<winrt::Windows::UI::Xaml::Shapes::Shape>()) {
                    void* key = winrt::get_abi(shape);
                    auto [it, inserted] = g_taskbarAppearanceStates.try_emplace(
                        key, TaskbarAppearanceState{shape, shape.Fill(), border});
                    const bool transparent = border
                        ? g_lightencyDockConfig.hideTaskbarBorder
                        : g_lightencyDockConfig.clearTaskbar;
                    shape.Fill(transparent
                        ? Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent())
                        : it->second.originalFill);
                }
            }

            const int childCount = VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < childCount; ++i) {
                if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                    stack.push_back(child);
                }
            }
        }

        root.InvalidateMeasure();
        root.InvalidateArrange();
        root.UpdateLayout();
    } catch (...) {}
}

static void RestoreTrayVisibility() {
    for (auto& [key, state] : g_trayVisibilityStates) {
        if (auto element = state.element.get()) {
            try {
                element.Visibility(state.originalVisibility);
                element.InvalidateMeasure();
                element.InvalidateArrange();
                if (auto parent = VisualTreeHelper::GetParent(element).try_as<FrameworkElement>()) {
                    parent.InvalidateMeasure();
                    parent.InvalidateArrange();
                    parent.UpdateLayout();
                } else {
                    element.UpdateLayout();
                }
            } catch (...) {}
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
                element.InvalidateMeasure();
                element.InvalidateArrange();
                if (auto parent = VisualTreeHelper::GetParent(element).try_as<FrameworkElement>()) {
                    parent.InvalidateMeasure();
                    parent.InvalidateArrange();
                    parent.UpdateLayout();
                } else {
                    element.UpdateLayout();
                }
            } catch (...) {}
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
        } catch (...) {

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
            } catch (...) {}
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

        if (ContainsAny(className, {L"language"}) ||
            ContainsAny(name, {L"language", L"inputindicator"})) {
            return TrayItemKind::Language;
        }
        if (ContainsAny(className, {L"batteryiconcontent"})) {
            return TrayItemKind::Battery;
        }

        if (auto textBlock = element.try_as<Controls::TextBlock>()) {
            std::wstring text = textBlock.Text().c_str();
            if (!text.empty()) {
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
                    key, TrayVisibilityState{ element, element.Visibility() });
                element.Visibility(hide ? Visibility::Collapsed : it->second.originalVisibility);
            }

            int childCount = VisualTreeHelper::GetChildrenCount(element);
            for (int i = 0; i < childCount; ++i) {
                if (auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>()) {
                    stack.push_back(child);
                }
            }
        }
    } catch (...) {}
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
    } catch (...) {}
}


struct {

    int animationType;

    double maxScale;

    int effectRadius;

    double spacingFactor;

    int bounceDelay;

    double focusDuration;

    bool disableVerticalBounce;

    bool taskbarLabelsMode;

    int excludeSystemButtonsMode;

    double lerpSpeed;

    bool disableBounce;

} g_settings;


std::atomic<bool> g_taskbarViewDllLoaded = false;

std::atomic<bool> g_systemTrayDllLoaded = false;

std::atomic<bool> g_hooksApplied = false;


winrt::event_token g_renderingToken;

std::atomic<bool> g_isRenderingHooked = false;

std::atomic<void*> g_activeContextKey = nullptr;

std::chrono::steady_clock::time_point g_lastSignificantMoveTime;

const double MOUSE_STOP_THRESHOLD = 0.5;


std::atomic<double> g_lastMouseX = -1.0;

std::atomic<bool> g_isBouncing = false;

std::atomic<double> g_bounceIntensity = 0.0;

std::chrono::steady_clock::time_point g_bounceStartTime;

const double BOUNCE_PERIOD_MS = 1200.0;

const double BOUNCE_SCALE_AMOUNT = 0.05;

const double BOUNCE_TRANSLATE_Y = -4.0;


std::atomic<bool> g_isMouseInside = false;

std::atomic<double> g_animationIntensity = 0.0;

std::chrono::steady_clock::time_point g_lastRenderTime;


enum class TaskbarEdge {

    Bottom,

    Top,

    Left,

    Right

};


struct IconAnimState {

    double waveScale = 1.0;

    double waveTranslateX = 0.0;

};


struct TaskbarIconInfo {

    winrt::weak_ref<FrameworkElement> element;

    double originalCenterX = 0.0;

    double elementWidth = 0.0;

    IconAnimState state;

};


struct HostSignature {

    int count = -1;

    double width = -1.0;

    uint64_t orderHash = 0;

};


struct DockAnimationContext {

    bool isInitialized = false;

    winrt::weak_ref<FrameworkElement> taskbarFrame;

    winrt::weak_ref<FrameworkElement> iconHost;

    std::vector<TaskbarIconInfo> icons;


    HostSignature lastSig;

    std::chrono::steady_clock::time_point lastDirtyCheck{};


    TaskbarEdge edge = TaskbarEdge::Bottom;

    bool isVertical = false;

    HWND hWnd = nullptr;

    RECT lastTrayRect = {0, 0, 0, 0};


    double smoothedMousePosition = 0.0;
    bool hasSmoothedMousePosition = false;

};


std::map<void*, DockAnimationContext> g_contexts;


void LoadSettings();

void ApplyAnimation(double mouseX, DockAnimationContext& ctx, double intensity, double dtSec);

void InitializeAnimationHooks(void* pThis, FrameworkElement const& taskbarFrame);

void OnTaskbarPointerMoved(void* pThis_key, Input::PointerRoutedEventArgs const& args);

void OnTaskbarPointerExited(void* pThis_key);

void ResetAllIconScales(std::vector<TaskbarIconInfo>& icons);

void RefreshIconPositions(DockAnimationContext& ctx);

void OnCompositionTargetRendering(winrt::Windows::Foundation::IInspectable const&,

                                  winrt::Windows::Foundation::IInspectable const&);


HMODULE GetTaskbarViewModuleHandle();

bool HookTaskbarViewDllSymbols(HMODULE module);

static bool RebaseIconGeometryFast(DockAnimationContext& ctx);


HWND GetCurrentThreadTrayWindow() {

    DWORD currentThreadId = GetCurrentThreadId();

    HWND hPrimary = FindWindow(L"Shell_TrayWnd", NULL);

    if (hPrimary && GetWindowThreadProcessId(hPrimary, nullptr) == currentThreadId) {

        return hPrimary;

    }

    HWND hSec = nullptr;

    while ((hSec = FindWindowEx(NULL, hSec, L"Shell_SecondaryTrayWnd", NULL)) != nullptr) {

        if (GetWindowThreadProcessId(hSec, nullptr) == currentThreadId) {

            return hSec;

        }

    }

    POINT pt;

    if (GetCursorPos(&pt)) {

        HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

        if (hMonitor) {

            if (hPrimary && MonitorFromWindow(hPrimary, MONITOR_DEFAULTTONULL) == hMonitor) {

                return hPrimary;

            }

            hSec = nullptr;

            while ((hSec = FindWindowEx(NULL, hSec, L"Shell_SecondaryTrayWnd", NULL)) != nullptr) {

                if (MonitorFromWindow(hSec, MONITOR_DEFAULTTONULL) == hMonitor) {

                    return hSec;

                }

            }

        }

    }

    return hPrimary;

}


void UpdateTaskbarEdge(DockAnimationContext& ctx) {

    auto frame = ctx.taskbarFrame.get();

    if (!frame) return;


    HWND hWnd = ctx.hWnd;

    if (!hWnd || !IsWindow(hWnd)) {

        hWnd = GetCurrentThreadTrayWindow();

        ctx.hWnd = hWnd;

    }


    if (!hWnd) {

        if (frame.ActualWidth() < frame.ActualHeight()) {

            ctx.edge = TaskbarEdge::Left;

        } else {

            ctx.edge = TaskbarEdge::Bottom;

        }

        ctx.isVertical = (ctx.edge == TaskbarEdge::Left || ctx.edge == TaskbarEdge::Right);

        return;

    }


    RECT rect = {0};

    if (GetWindowRect(hWnd, &rect)) {

        if (rect.left == ctx.lastTrayRect.left &&

            rect.top == ctx.lastTrayRect.top &&

            rect.right == ctx.lastTrayRect.right &&

            rect.bottom == ctx.lastTrayRect.bottom) {

            return;

        }


        ctx.lastTrayRect = rect;


        LONG w = rect.right - rect.left;

        LONG h = rect.bottom - rect.top;

        if (w < h) {

            HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);

            MONITORINFO mi = { sizeof(mi) };

            if (GetMonitorInfo(hMonitor, &mi)) {

                if (rect.left <= mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left) / 2) {

                    ctx.edge = TaskbarEdge::Left;

                } else {

                    ctx.edge = TaskbarEdge::Right;

                }

            } else {

                ctx.edge = TaskbarEdge::Left;

            }

        } else {

            HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);

            MONITORINFO mi = { sizeof(mi) };

            if (GetMonitorInfo(hMonitor, &mi)) {

                if (rect.top <= mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top) / 2) {

                    ctx.edge = TaskbarEdge::Top;

                } else {

                    ctx.edge = TaskbarEdge::Bottom;

                }

            } else {

                ctx.edge = TaskbarEdge::Bottom;

            }

        }

        ctx.isVertical = (ctx.edge == TaskbarEdge::Left || ctx.edge == TaskbarEdge::Right);

    }

}


FrameworkElement EnumChildElements(

    FrameworkElement element,

    std::function<bool(FrameworkElement)> enumCallback) {

    int childrenCount = VisualTreeHelper::GetChildrenCount(element);

    for (int i = 0; i < childrenCount; i++) {

        auto child = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();

        if (!child) continue;

        if (enumCallback(child)) return child;

    }

    return nullptr;

}


FrameworkElement FindChildByClassName(FrameworkElement element,

                                          PCWSTR className) {

    if (!element) return nullptr;

    return EnumChildElements(element, [className](FrameworkElement child) {

        return winrt::get_class_name(child) == className;

    });

}


static void PrepareHighResolutionIconSources(FrameworkElement const& root,
                                             double maxScale) {
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
    } catch (...) {

    }
}


double CalculateScale(double distance, double radius, double maxScale) {

    if (!g_lightencyDockConfig.dockAnimation || maxScale <= 1.0) return 1.0;
    if (distance > radius) return 1.0;


    double t = distance / radius;

    double factor = 0.0;


    switch (g_settings.animationType) {

        case 1:

            factor = 1.0 - t;

            break;

        case 2:

            factor = pow(1.0 - t, 3);

            break;

        case 0:

        default:

            factor = (cos(t * 3.14159) + 1.0) / 2.0;

            break;

    }

    if (factor < 0) factor = 0;

    return (maxScale - 1.0) * factor + 1.0;

}


void ApplyAnimation(double mouseX, DockAnimationContext& ctx, double intensity, double dtSec) {
    EnsureLiveConfigMapped();
    if (!g_lightencyDockConfig.dockAnimation) {
        ResetAllIconScales(ctx.icons);
        return;
    }

    const double spacingFactor = g_lightencyDockConfig.autoPhysics ? 0.50 : g_settings.spacingFactor;

    std::vector<double> scales(ctx.icons.size());

    std::vector<double> extraWidths(ctx.icons.size());

    double totalExpansion = 0.0;

    size_t closestIconIndex = (size_t)-1;

    double minDistance = g_settings.effectRadius + 1.0;

    auto taskbarFrame = ctx.taskbarFrame.get();

    if (!taskbarFrame) return;

    ApplyTaskbarAppearance(taskbarFrame);
    ApplyTrayItemVisibility(taskbarFrame);

    if (!ctx.hasSmoothedMousePosition) {
        ctx.smoothedMousePosition = mouseX;
        ctx.hasSmoothedMousePosition = true;
    } else {
        const double cursorDelta = mouseX - ctx.smoothedMousePosition;


        const double followRate = 20.0 + std::min(std::abs(cursorDelta) * 0.30, 40.0);
        const double cursorAlpha = 1.0 - std::exp(-followRate * dtSec);
        ctx.smoothedMousePosition += cursorDelta * std::clamp(cursorAlpha, 0.0, 1.0);
    }
    mouseX = ctx.smoothedMousePosition;

    for (size_t i = 0; i < ctx.icons.size(); i++) {

        auto element = ctx.icons[i].element.get();

        if (!element) continue;

        double distance = 0.0;

        if (g_settings.taskbarLabelsMode) {

            double iconStart = ctx.icons[i].originalCenterX;

            double iconEnd = iconStart + ctx.icons[i].elementWidth;

            if (mouseX < iconStart) {

                distance = iconStart - mouseX;

            } else if (mouseX > iconEnd) {

                distance = mouseX - iconEnd;

            } else {

                distance = 0.0;

            }

        } else {

            distance = std::abs(mouseX - ctx.icons[i].originalCenterX);

        }

        scales[i] = CalculateScale(distance, (g_lightencyDockConfig.autoPhysics ? (ctx.icons[i].elementWidth > 10.0 ? ctx.icons[i].elementWidth * 1.15 : 46.0) : (double)g_settings.effectRadius), (g_lightencyDockConfig.autoPhysics ? 1.35 : g_settings.maxScale));

        double size = ctx.isVertical ? element.ActualHeight() : element.ActualWidth();

        extraWidths[i] = (scales[i] - 1.0) * size * spacingFactor;

        totalExpansion += extraWidths[i];


        if (distance < minDistance) {

            minDistance = distance;

            closestIconIndex = i;

        }

    }


    double bounceScaleFactor = 1.0;

    double bounceTranslateOffset = 0.0;


    double currentBounceIntensity = g_bounceIntensity.load();


    if (currentBounceIntensity > 0.0 && closestIconIndex != (size_t)-1 &&

        minDistance < (g_settings.effectRadius / 2.0)) {

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(

                            std::chrono::steady_clock::now() - g_bounceStartTime)

                            .count();

        double t = std::fmod(elapsed_ms, BOUNCE_PERIOD_MS) / BOUNCE_PERIOD_MS;

        double normalizedWave = std::sin(t * 3.14159);

        bounceScaleFactor = 1.0 + (normalizedWave * BOUNCE_SCALE_AMOUNT * currentBounceIntensity);

        bounceTranslateOffset = normalizedWave * BOUNCE_TRANSLATE_Y * currentBounceIntensity;


        if (ctx.isVertical) {

            if (ctx.edge == TaskbarEdge::Left) {

                bounceTranslateOffset *= -1.0;

            }

        } else {

            if (ctx.edge == TaskbarEdge::Top) {

                bounceTranslateOffset *= -1.0;

            }

        }

    }


    double alpha = 1.0;

    if (g_settings.lerpSpeed > 0.0) {

        alpha = 1.0 - std::exp(-g_settings.lerpSpeed * dtSec);

    }

    alpha = std::clamp(alpha, 0.0, 1.0);


    double cumulativeShift = 0.0;


    for (size_t i = 0; i < ctx.icons.size(); i++) {

        auto element = ctx.icons[i].element.get();

        if (!element) continue;

        auto tg = element.RenderTransform().try_as<TransformGroup>();

        if (!tg || tg.Children().Size() < 5) continue;

        auto waveScale = tg.Children().GetAt(1).try_as<ScaleTransform>();

        auto waveTranslate = tg.Children().GetAt(2).try_as<TranslateTransform>();

        auto bounceScale = tg.Children().GetAt(3).try_as<ScaleTransform>();

        auto bounceTranslate = tg.Children().GetAt(4).try_as<TranslateTransform>();


        if (!waveScale || !waveTranslate || !bounceScale || !bounceTranslate) continue;

        double selfShift = extraWidths[i] / 2.0;

        double centerOffset = totalExpansion / 2.0;

        double finalShift = cumulativeShift + selfShift - centerOffset;

        cumulativeShift += extraWidths[i];

        double targetScale = 1.0 + (scales[i] - 1.0) * intensity;

        double targetTranslate = finalShift * intensity;


        if (g_settings.lerpSpeed > 0.0) {

            ctx.icons[i].state.waveScale += (targetScale - ctx.icons[i].state.waveScale) * alpha;

            ctx.icons[i].state.waveTranslateX += (targetTranslate - ctx.icons[i].state.waveTranslateX) * alpha;

        } else {

            ctx.icons[i].state.waveScale = targetScale;

            ctx.icons[i].state.waveTranslateX = targetTranslate;

        }


        waveScale.ScaleX(ctx.icons[i].state.waveScale);

        waveScale.ScaleY(ctx.icons[i].state.waveScale);
        Controls::Canvas::SetZIndex(element, (int)(ctx.icons[i].state.waveScale * 100.0));
        HideButtonBackgroundPlate(element);


        if (ctx.isVertical) {

            waveTranslate.X(0.0);

            waveTranslate.Y(ctx.icons[i].state.waveTranslateX);

        } else {

            waveTranslate.X(ctx.icons[i].state.waveTranslateX);

            waveTranslate.Y(0.0);

        }


        if (i == closestIconIndex && currentBounceIntensity > 0.0) {

            bounceScale.ScaleX(bounceScaleFactor);

            bounceScale.ScaleY(bounceScaleFactor);


            if (!g_settings.disableBounce && !g_settings.disableVerticalBounce) {

                if (ctx.isVertical) {

                    bounceTranslate.X(bounceTranslateOffset);

                    bounceTranslate.Y(0.0);

                } else {

                    bounceTranslate.X(0.0);

                    bounceTranslate.Y(bounceTranslateOffset);

                }

            } else {

                bounceTranslate.X(0.0);

                bounceTranslate.Y(0.0);

            }

        } else {

            bounceScale.ScaleX(1.0);

            bounceScale.ScaleY(1.0);

            bounceTranslate.X(0.0);

            bounceTranslate.Y(0.0);

        }

    }

}


void ResetAllIconScales(std::vector<TaskbarIconInfo>& icons) {

    if (icons.empty()) return;

    try {

        for (auto& iconInfo : icons) {

            auto element = iconInfo.element.get();

            if (!element) continue;

            auto tg = element.RenderTransform().try_as<TransformGroup>();

            if (!tg || tg.Children().Size() < 5) continue;

            auto waveScale = tg.Children().GetAt(1).try_as<ScaleTransform>();

            auto waveTranslate = tg.Children().GetAt(2).try_as<TranslateTransform>();

            auto bounceScale = tg.Children().GetAt(3).try_as<ScaleTransform>();

            auto bounceTranslate = tg.Children().GetAt(4).try_as<TranslateTransform>();


            if (waveScale) {

                waveScale.ScaleX(1.0);

                waveScale.ScaleY(1.0);

            }

            if (waveTranslate) {

                waveTranslate.X(0.0);

                waveTranslate.Y(0.0);

            }

            if (bounceScale) {

                bounceScale.ScaleX(1.0);

                bounceScale.ScaleY(1.0);

            }

            if (bounceTranslate) {

                bounceTranslate.X(0.0);

                bounceTranslate.Y(0.0);

            }

        }

    } catch (winrt::hresult_error const& e) {

        Wh_Log(L"DockAnimation: HRESULT error in ResetAllIconScales: %s", e.message().c_str());

    }

}


void OnTaskbarPointerMoved(void* pThis_key, Input::PointerRoutedEventArgs const& args) {
    EnsureLiveConfigMapped();

    try {

        auto it = g_contexts.find(pThis_key);

        if (it == g_contexts.end()) return;

        auto& ctx = it->second;

        auto frame = ctx.taskbarFrame.get();

        if (!frame) return;

        g_isMouseInside = true;

        g_activeContextKey = pThis_key;


        UpdateTaskbarEdge(ctx);


        double mousePos = ctx.isVertical

            ? args.GetCurrentPoint(frame).Position().Y

            : args.GetCurrentPoint(frame).Position().X;


        if (std::abs(mousePos - g_lastMouseX.load()) > MOUSE_STOP_THRESHOLD) {

            g_lastSignificantMoveTime = std::chrono::steady_clock::now();

        }

        g_lastMouseX = mousePos;

        if (!g_isRenderingHooked.exchange(true)) {

            g_lastRenderTime = std::chrono::steady_clock::now();

            g_renderingToken = Media::CompositionTarget::Rendering(OnCompositionTargetRendering);

            g_lastSignificantMoveTime = std::chrono::steady_clock::now();

            Wh_Log(L"DockAnimation: Started render loop (FocusIn).");

        }

    } catch (winrt::hresult_error const& e) {

        Wh_Log(L"DockAnimation: HRESULT Error in OnTaskbarPointerMoved: %s", e.message().c_str());

    }

}


void OnTaskbarPointerExited(void* pThis_key) {

    try {

        if (g_activeContextKey.load() == pThis_key) {

            g_isMouseInside = false;

            g_isBouncing = false;

        }

    } catch (winrt::hresult_error const& e) {

        Wh_Log(L"DockAnimation: HRESULT Error in OnTaskbarPointerExited: %s", e.message().c_str());

    }

}


FrameworkElement FindChildByClassNamePartial(FrameworkElement element, PCWSTR partialName) {

    if (!element) return nullptr;

    return EnumChildElements(element, [partialName](FrameworkElement child) {

        auto className = winrt::get_class_name(child);

        return std::wstring_view(className).find(partialName) != std::wstring_view::npos;

    });

}


static FrameworkElement FindIconHost(FrameworkElement const& taskbarFrame) {

    FrameworkElement host = FindChildByClassNamePartial(taskbarFrame, L"TaskbarFrameRepeater");

    if (!host) host = FindChildByClassName(taskbarFrame, L"TaskbarFrameRepeater");

    if (!host) host = FindChildByClassName(taskbarFrame, L"TaskbarItemHost");

    return host;

}


static uint64_t Fnv1aMix(uint64_t h, uint64_t v) {

    h ^= v;

    h *= 1099511628211ULL;

    return h;

}


static HostSignature ComputeHostSignature(FrameworkElement const& host, bool isVertical) {

    HostSignature sig;

    sig.count = VisualTreeHelper::GetChildrenCount(host);

    sig.width = isVertical ? host.ActualHeight() : host.ActualWidth();


    uint64_t h = 1469598103934665603ULL;

    for (int i = 0; i < sig.count; i++) {

        auto child = VisualTreeHelper::GetChild(host, i).try_as<FrameworkElement>();

        if (!child) continue;

        h = Fnv1aMix(h, (uint64_t)winrt::get_abi(child));

        double size = isVertical ? child.ActualHeight() : child.ActualWidth();

        h = Fnv1aMix(h, (uint64_t)(size * 10.0));

    }

    sig.orderHash = h;

    return sig;

}


static bool SigDifferent(HostSignature const& a, HostSignature const& b) {

    if (a.count != b.count) return true;

    if (std::abs(a.width - b.width) > 0.5) return true;

    if (a.orderHash != b.orderHash) return true;

    return false;

}


enum class ButtonKind {

    App,

    Start,

    Search,

    TaskView,

    Widgets,

    Weather,

    SystemOther

};


static std::wstring W(winrt::hstring const& s) {

    return std::wstring(s.c_str());

}


static std::wstring ToLower(std::wstring s) {

    for (auto& ch : s) ch = (wchar_t)towlower(ch);

    return s;

}


static ButtonKind ClassifyButton(FrameworkElement const& e) {

    if (!e) return ButtonKind::SystemOther;


    std::wstring cn     = W(winrt::get_class_name(e));

    std::wstring feName = W(e.Name());

    std::wstring aid    = W(AutomationProperties::GetAutomationId(e));


    std::wstring cnL = ToLower(cn);

    std::wstring aidL = ToLower(aid);

    std::wstring feL = ToLower(feName);


    auto matches = [&](const wchar_t* s) {

        return cnL.find(s) != std::wstring::npos ||

               aidL.find(s) != std::wstring::npos ||

               feL.find(s) != std::wstring::npos;

    };


    if (matches(L"startbutton")) return ButtonKind::Start;

    if (matches(L"searchbutton")) return ButtonKind::Search;

    if (matches(L"taskviewbutton")) return ButtonKind::TaskView;

    if (matches(L"widgetsbutton")) return ButtonKind::Widgets;

    if (matches(L"weather")) return ButtonKind::Weather;


    if (matches(L"appid:")) return ButtonKind::App;


    {

        if (cnL == L"taskbar.tasklistbutton" ||

            (cnL.size() >= 13 && cnL.rfind(L".tasklistbutton") == cnL.size() - 13)) {

            return ButtonKind::App;

        }

    }


    return ButtonKind::SystemOther;

}


static bool ShouldAnimateElement(FrameworkElement const& e) {

    const int mode = g_settings.excludeSystemButtonsMode;

    if (mode == 0) return true;


    ButtonKind k = ClassifyButton(e);


    if (mode == 2) return k == ButtonKind::App;

    if (mode == 1) return k != ButtonKind::Start;


    return true;

}


static void ResetElementTransforms(FrameworkElement const& element) {

    auto tg = element.RenderTransform().try_as<TransformGroup>();

    if (!tg || tg.Children().Size() < 5) return;


    auto waveScale = tg.Children().GetAt(1).try_as<ScaleTransform>();

    auto waveTranslate = tg.Children().GetAt(2).try_as<TranslateTransform>();

    auto bounceScale = tg.Children().GetAt(3).try_as<ScaleTransform>();

    auto bounceTranslate = tg.Children().GetAt(4).try_as<TranslateTransform>();


    if (waveScale) { waveScale.ScaleX(1.0); waveScale.ScaleY(1.0); }

    if (waveTranslate) { waveTranslate.X(0.0); waveTranslate.Y(0.0); }

    if (bounceScale) { bounceScale.ScaleX(1.0); bounceScale.ScaleY(1.0); }

    if (bounceTranslate) { bounceTranslate.X(0.0); bounceTranslate.Y(0.0); }
    Controls::Canvas::SetZIndex(element, 0);

}


static bool RebaseIconGeometryFast(DockAnimationContext& ctx) {

    auto frame = ctx.taskbarFrame.get();

    if (!frame) return false;


    bool changed = false;


    for (auto it = ctx.icons.begin(); it != ctx.icons.end(); ) {

        if (!it->element.get()) {

            it = ctx.icons.erase(it);

            changed = true;

        } else {

            ++it;

        }

    }


    for (auto& info : ctx.icons) {

        auto e = info.element.get();

        if (!e) continue;


        try {

            double oldX = info.originalCenterX;

            double oldW = info.elementWidth;


            auto t = e.TransformToVisual(frame);

            auto pt = t.TransformPoint({0, 0});

            double w = ctx.isVertical ? e.ActualHeight() : e.ActualWidth();

            info.elementWidth = w;


            float xOrigin = 0.5f;

            float yOrigin = 0.5f;

            if (ctx.isVertical) {

                xOrigin = (ctx.edge == TaskbarEdge::Left) ? 0.0f : 1.0f;

            } else {

                bool isTop = (ctx.edge == TaskbarEdge::Top);

                yOrigin = 0.5f;

            }


            if (g_settings.taskbarLabelsMode) {

                double newStartX = ctx.isVertical ? pt.Y : pt.X;

                info.originalCenterX = newStartX;


                float iconCenterPx = 24.0f;

                float iconCenterProportion = (w > 0.0) ? (iconCenterPx / (float)w) : 0.5f;

                if (ctx.isVertical) {

                    e.RenderTransformOrigin({ xOrigin, iconCenterProportion });

                } else {

                    e.RenderTransformOrigin({ iconCenterProportion, yOrigin });

                }


                if (std::abs(newStartX - oldX) > 0.5) changed = true;

            } else {

                double newCenterX = ctx.isVertical ? (pt.Y + (w / 2.0)) : (pt.X + (w / 2.0));

                info.originalCenterX = newCenterX;


                if (ctx.isVertical) {

                    e.RenderTransformOrigin({ xOrigin, yOrigin });

                } else {

                    e.RenderTransformOrigin({ 0.5f, yOrigin });

                }


                if (std::abs(newCenterX - oldX) > 0.5) changed = true;

            }


            if (std::abs(w - oldW) > 0.5) changed = true;

        } catch (...) {

        }

    }


    if (changed && ctx.icons.size() > 1) {

        std::sort(ctx.icons.begin(), ctx.icons.end(),

            [](TaskbarIconInfo const& a, TaskbarIconInfo const& b) {

                return a.originalCenterX < b.originalCenterX;

            });

    }


    return changed;

}


void RefreshIconPositions(DockAnimationContext& ctx) {


    std::unordered_map<void*, IconAnimState> prevStates;

    std::unordered_set<void*> prevElements;


    for (auto& old : ctx.icons) {

        if (auto e = old.element.get()) {

            void* key = winrt::get_abi(e);

            prevStates[key] = old.state;

            prevElements.insert(key);

        }

    }


    std::vector<TaskbarIconInfo> newIcons;


    auto taskbarFrame = ctx.taskbarFrame.get();

    if (!taskbarFrame) return;

    auto IsTaskListButtonExact = [&](FrameworkElement const& e) -> bool {

        if (!e) return false;

        return winrt::get_class_name(e) == L"Taskbar.TaskListButton";

    };


    auto IsTaskListButtonPanel = [&](FrameworkElement const& e) -> bool {

        if (!e) return false;

        return winrt::get_class_name(e) == L"Taskbar.TaskListButtonPanel";

    };


    std::function<FrameworkElement(FrameworkElement)> FindFirstTaskListButton =

        [&](FrameworkElement root) -> FrameworkElement {

            if (!root) return nullptr;

            if (IsTaskListButtonExact(root)) return root;


            int c = VisualTreeHelper::GetChildrenCount(root);

            for (int i = 0; i < c; i++) {

                auto ch = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();

                if (!ch) continue;

                if (IsTaskListButtonExact(ch)) return ch;

                auto deep = FindFirstTaskListButton(ch);

                if (deep) return deep;

            }

            return nullptr;

        };


    std::function<FrameworkElement(FrameworkElement)> FindClassifierNode =

        [&](FrameworkElement root) -> FrameworkElement {

            if (!root) return nullptr;


            auto aid = W(AutomationProperties::GetAutomationId(root));

            auto nm  = W(AutomationProperties::GetName(root));

            if (!aid.empty() || !nm.empty()) return root;


            int c = VisualTreeHelper::GetChildrenCount(root);

            for (int i = 0; i < c; i++) {

                auto ch = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();

                if (!ch) continue;

                auto hit = FindClassifierNode(ch);

                if (hit) return hit;

            }

            return nullptr;

        };


    auto IsFallbackCandidate = [&](FrameworkElement const& e) -> bool {

        if (!e) return false;


        if (IsTaskListButtonExact(e) || IsTaskListButtonPanel(e)) return true;


        auto cn = winrt::get_class_name(e);

        if (std::wstring_view(cn).find(L"Taskbar.") != std::wstring_view::npos) {

            auto aid = W(AutomationProperties::GetAutomationId(e));

            auto nm  = W(AutomationProperties::GetName(e));

            if (!aid.empty() || !nm.empty()) return true;

        }


        return false;

    };


    auto SetupAndAddElement = [&](FrameworkElement const& element) {

        try {

            if (!element) return;

            PrepareHighResolutionIconSources(element, g_settings.maxScale);

            FrameworkElement animationElement = element;
            if (ClassifyButton(element) == ButtonKind::App) {
                std::vector<FrameworkElement> search{ element };
                double bestArea = 0.0;
                while (!search.empty()) {
                    FrameworkElement candidate = search.back();
                    search.pop_back();
                    if (candidate.try_as<Controls::Image>()) {
                        double area = candidate.ActualWidth() * candidate.ActualHeight();
                        if (candidate.ActualWidth() >= 14.0 && candidate.ActualHeight() >= 14.0 &&
                            area > bestArea) {
                            animationElement = candidate;
                            bestArea = area;
                        }
                    }
                    int count = VisualTreeHelper::GetChildrenCount(candidate);
                    for (int i = 0; i < count; ++i) {
                        if (auto child = VisualTreeHelper::GetChild(candidate, i).try_as<FrameworkElement>()) {
                            search.push_back(child);
                        }
                    }
                }
            }

            auto existingTransform = animationElement.RenderTransform();
            auto tg = existingTransform.try_as<TransformGroup>();
            bool hasDockChain = tg && tg.Children().Size() >= 5 &&
                tg.Children().GetAt(1).try_as<ScaleTransform>() &&
                tg.Children().GetAt(2).try_as<TranslateTransform>() &&
                tg.Children().GetAt(3).try_as<ScaleTransform>() &&
                tg.Children().GetAt(4).try_as<TranslateTransform>();

            if (!hasDockChain) {
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
                animationElement.RenderTransform(tg);
            }


            float xOrigin = 0.5f;

            float yOrigin = 0.5f;

            if (ctx.isVertical) {

                xOrigin = (ctx.edge == TaskbarEdge::Left) ? 0.0f : 1.0f;

            } else {

                bool isTop = (ctx.edge == TaskbarEdge::Top);

                yOrigin = 0.5f;

            }


            auto transform = animationElement.TransformToVisual(taskbarFrame);

            auto point = transform.TransformPoint({0, 0});


            TaskbarIconInfo info;

            info.element = animationElement;

            info.elementWidth = ctx.isVertical ? element.ActualHeight() : element.ActualWidth();


            if (g_settings.taskbarLabelsMode) {

                float iconCenterPx = 24.0f;

                float iconCenterProportion =

                    (info.elementWidth > 0) ? (iconCenterPx / (float)info.elementWidth) : 0.5f;


                if (ctx.isVertical) {

                    animationElement.RenderTransformOrigin({ xOrigin, iconCenterProportion });

                } else {

                    animationElement.RenderTransformOrigin({ iconCenterProportion, yOrigin });

                }

                info.originalCenterX = ctx.isVertical ? point.Y : point.X;

            } else {

                if (ctx.isVertical) {

                    animationElement.RenderTransformOrigin({ xOrigin, yOrigin });

                } else {

                    animationElement.RenderTransformOrigin({ 0.5f, yOrigin });

                }

                info.originalCenterX = ctx.isVertical
                    ? (point.Y + animationElement.ActualHeight() / 2.0)
                    : (point.X + animationElement.ActualWidth() / 2.0);

            }


            void* key = winrt::get_abi(animationElement);


            auto it = prevStates.find(key);

            if (it != prevStates.end()) {

                info.state = it->second;


                auto waveScale = tg.Children().GetAt(1).try_as<ScaleTransform>();

                auto waveTranslate = tg.Children().GetAt(2).try_as<TranslateTransform>();

                if (waveScale) {

                    waveScale.ScaleX(info.state.waveScale);

                    waveScale.ScaleY(info.state.waveScale);

                }

                if (waveTranslate) {

                    if (ctx.isVertical) {

                        waveTranslate.Y(info.state.waveTranslateX);

                    } else {

                        waveTranslate.X(info.state.waveTranslateX);

                    }

                }

            }


            newIcons.push_back(info);

        } catch (winrt::hresult_error const& e) {

            Wh_Log(L"DockAnimation: HRESULT error in SetupAndAddElement: %s", e.message().c_str());

        }

    };


    auto FindSmartHost = [&]() -> FrameworkElement {

        std::vector<FrameworkElement> nodes;

        nodes.reserve(512);


        std::vector<FrameworkElement> stack;

        stack.reserve(512);

        stack.push_back(taskbarFrame);


        while (!stack.empty()) {

            auto cur = stack.back();

            stack.pop_back();

            if (!cur) continue;


            nodes.push_back(cur);


            int c = VisualTreeHelper::GetChildrenCount(cur);

            for (int i = 0; i < c; i++) {

                auto ch = VisualTreeHelper::GetChild(cur, i).try_as<FrameworkElement>();

                if (ch) stack.push_back(ch);

            }

        }


        std::function<bool(FrameworkElement,int)> SubtreeHasIconLike =

            [&](FrameworkElement root, int depth) -> bool {

                if (!root || depth <= 0) return false;

                if (IsTaskListButtonExact(root) || IsTaskListButtonPanel(root)) return true;


                int c = VisualTreeHelper::GetChildrenCount(root);

                for (int i = 0; i < c; i++) {

                    auto ch = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();

                    if (!ch) continue;

                    if (SubtreeHasIconLike(ch, depth - 1)) return true;

                }

                return false;

            };


        FrameworkElement best = nullptr;

        int bestHits = 0;

        double bestWidth = 0.0;


        for (auto const& n : nodes) {

            int c = VisualTreeHelper::GetChildrenCount(n);

            if (c < 3 || c > 120) continue;


            int hits = 0;

            for (int i = 0; i < c; i++) {

                auto ch = VisualTreeHelper::GetChild(n, i).try_as<FrameworkElement>();

                if (!ch) continue;

                if (SubtreeHasIconLike(ch, 5)) hits++;

            }


            if (hits >= 3) {

                double w = (double)n.ActualWidth();

                if (hits > bestHits || (hits == bestHits && w > bestWidth)) {

                    best = n;

                    bestHits = hits;

                    bestWidth = w;

                }

            }

        }


        return best;

    };


    auto host = ctx.iconHost.get();

    if (!host) {

        host = FindSmartHost();

        if (host) ctx.iconHost = host;

    }


    try {

        if (host) {


            int count = VisualTreeHelper::GetChildrenCount(host);

            for (int i = 0; i < count; i++) {

                auto child = VisualTreeHelper::GetChild(host, i).try_as<FrameworkElement>();

                if (!child) continue;


                FrameworkElement classifier = FindClassifierNode(child);

                FrameworkElement base = classifier ? classifier : child;


                bool animate = (g_settings.excludeSystemButtonsMode == 0)

                    ? true

                    : ShouldAnimateElement(base);


                FrameworkElement target = FindFirstTaskListButton(child);

                if (!target) target = child;


                if (!animate) {

                    void* kT = winrt::get_abi(target);

                    if (prevElements.contains(kT)) ResetElementTransforms(target);


                    void* kC = winrt::get_abi(child);

                    if (prevElements.contains(kC)) ResetElementTransforms(child);

                    continue;

                }


                SetupAndAddElement(target);

            }


            ctx.lastSig = ComputeHostSignature(host, ctx.isVertical);

        } else {


            std::function<void(FrameworkElement)> recurse = [&](FrameworkElement element) {

                if (!element) return;


                if (IsFallbackCandidate(element)) {

                    FrameworkElement classifier = FindClassifierNode(element);

                    FrameworkElement base = classifier ? classifier : element;


                    bool animate = (g_settings.excludeSystemButtonsMode == 0)

                        ? true

                        : ShouldAnimateElement(base);


                    FrameworkElement target = FindFirstTaskListButton(element);

                    if (!target) target = element;


                    if (!animate) {

                        void* k = winrt::get_abi(target);

                        if (prevElements.contains(k)) ResetElementTransforms(target);

                    } else {

                        SetupAndAddElement(target);

                    }

                }


                int c = VisualTreeHelper::GetChildrenCount(element);

                for (int i = 0; i < c; i++) {

                    auto ch = VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();

                    if (ch) recurse(ch);

                }

            };


            recurse(taskbarFrame);

        }


        std::sort(newIcons.begin(), newIcons.end(),

            [](TaskbarIconInfo const& a, TaskbarIconInfo const& b) {

                return a.originalCenterX < b.originalCenterX;

            });


        ctx.icons = std::move(newIcons);


    } catch (winrt::hresult_error const& e) {

        Wh_Log(L"DockAnimation: HRESULT error during icon search: %s", e.message().c_str());

    }

}


void InitializeAnimationHooks(void* pThis, FrameworkElement const& taskbarFrame) {

    try {

        DockAnimationContext ctx;

        ctx.taskbarFrame = taskbarFrame;

        ctx.isInitialized = false;

        ctx.hWnd = GetCurrentThreadTrayWindow();

        UpdateTaskbarEdge(ctx);

        g_contexts[pThis] = std::move(ctx);

        Wh_Log(L"DockAnimation: Monitor %p registered (hook-based). Edge: %d, HWND: %p", pThis, (int)ctx.edge, ctx.hWnd);

    }

    catch (winrt::hresult_error const& e) {

        Wh_Log(L"DockAnimation: Failed to initialize context for %p: %s",

               pThis, e.message().c_str());

    }

}


void OnCompositionTargetRendering(winrt::Windows::Foundation::IInspectable const&,

                                  winrt::Windows::Foundation::IInspectable const&) {

    try {

        auto now = std::chrono::steady_clock::now();


        double dtSec = std::chrono::duration<double>(now - g_lastRenderTime).count();
        dtSec = std::clamp(dtSec, 0.0, 0.050);
        g_lastRenderTime = now;


        double intensityChange = (dtSec * 1000.0) / g_settings.focusDuration;

        double currentIntensity = g_animationIntensity.load();


        if (g_isMouseInside) {


            currentIntensity = std::min(1.0, currentIntensity + intensityChange);

        } else {


            currentIntensity = std::max(0.0, currentIntensity - intensityChange);

        }


        g_animationIntensity = currentIntensity;

        EnsureLiveConfigMapped();
        if (!g_lightencyDockConfig.dockAnimation) {
            Media::CompositionTarget::Rendering(g_renderingToken);
            g_isRenderingHooked = false;
            for (auto& pair : g_contexts) {
                ResetAllIconScales(pair.second.icons);
                pair.second.isInitialized = false;
                pair.second.hasSmoothedMousePosition = false;
                pair.second.icons.clear();
            }
            g_activeContextKey = nullptr;
            g_lastMouseX = -1.0;
            g_isBouncing = false;
            g_animationIntensity = 0.0;
            return;
        }

        void* pThis_key = g_activeContextKey.load();

        if (pThis_key == nullptr) {

            if (currentIntensity <= 0.0) {

                Media::CompositionTarget::Rendering(g_renderingToken);

                g_isRenderingHooked = false;

                Wh_Log(L"DockAnimation: Stopped render loop (Key=null, Intensity=0).");

            }

            return;

        }


        auto it = g_contexts.find(pThis_key);

        if (it == g_contexts.end()) return;


        auto& ctx = it->second;


        if (!g_isMouseInside && currentIntensity <= 0.0) {

            Media::CompositionTarget::Rendering(g_renderingToken);

            g_isRenderingHooked = false;


            ResetAllIconScales(ctx.icons);

            ctx.isInitialized = false;
            ctx.hasSmoothedMousePosition = false;

            ctx.icons.clear();


            g_activeContextKey = nullptr;

            g_lastMouseX = -1.0;

            g_isBouncing = false;


            Wh_Log(L"DockAnimation: Stopped render loop (FocusOut complete).");

            return;

        }


        if (!ctx.isInitialized) {

            RefreshIconPositions(ctx);

            ctx.isInitialized = true;

        }


        if (g_isMouseInside) {

            auto sinceCheck = std::chrono::duration_cast<std::chrono::milliseconds>(

                now - ctx.lastDirtyCheck).count();


            if (sinceCheck > 120) {

                ctx.lastDirtyCheck = now;


                auto frame = ctx.taskbarFrame.get();

                if (frame) {

                    UpdateTaskbarEdge(ctx);


                    auto host = ctx.iconHost.get();

                    if (!host) {

                        host = FindIconHost(frame);

                        if (host) ctx.iconHost = host;

                    }


                    if (host) {

                        HostSignature sig = ComputeHostSignature(host, ctx.isVertical);


                        if (SigDifferent(sig, ctx.lastSig)) {

                            RefreshIconPositions(ctx);

                        } else {

                            RebaseIconGeometryFast(ctx);

                        }


                        ctx.lastSig = sig;

                    }

                }

            }

        }


        if (ctx.icons.empty()) {

            if (ctx.isInitialized) {

                RefreshIconPositions(ctx);

            }

            if (ctx.icons.empty()) return;

        }


        if (g_settings.disableBounce) {

            g_isBouncing = false;

        } else {

            auto elapsedSinceMove =

                std::chrono::duration_cast<std::chrono::milliseconds>(

                    now - g_lastSignificantMoveTime)

                    .count();


            if (g_isMouseInside && elapsedSinceMove > g_settings.bounceDelay) {

                if (!g_isBouncing.exchange(true)) {

                    g_bounceStartTime = now;

                }

            } else {

                g_isBouncing = false;

            }

        }


        double targetBounceIntensity = g_isBouncing ? 1.0 : 0.0;

        double bounceFadeDuration = g_settings.focusDuration / 1000.0;

        double bounceIntensityChange = dtSec / bounceFadeDuration;

        double currentBounceIntensity = g_bounceIntensity.load();

        if (targetBounceIntensity > currentBounceIntensity) {

            currentBounceIntensity = std::min(targetBounceIntensity, currentBounceIntensity + bounceIntensityChange);

        } else {

            currentBounceIntensity = std::max(targetBounceIntensity, currentBounceIntensity - bounceIntensityChange);

        }

        g_bounceIntensity = currentBounceIntensity;


        double mouseX = g_lastMouseX.load();

        if (mouseX == -1.0) mouseX = 0.0;


        const double easedIntensity = currentIntensity * currentIntensity *
            (3.0 - 2.0 * currentIntensity);
        ApplyAnimation(mouseX, ctx, easedIntensity, dtSec);

    } catch (...) {

        if (g_isRenderingHooked.exchange(false)) {

            Media::CompositionTarget::Rendering(g_renderingToken);

        }

    }

}


using TaskbarFrame_OnPointerMoved_t = int(WINAPI*)(void* pThis, void* pArgs);

TaskbarFrame_OnPointerMoved_t TaskbarFrame_OnPointerMoved_Original;


int WINAPI TaskbarFrame_OnPointerMoved_Hook(void* pThis, void* pArgs) {

    auto original = [=]() {

        return TaskbarFrame_OnPointerMoved_Original(pThis, pArgs);

    };

    FrameworkElement element = nullptr;

    ((IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));

    if (!element)

        return original();

    auto className = winrt::get_class_name(element);

    if (className != L"Taskbar.TaskbarFrame")

        return original();

    Input::PointerRoutedEventArgs args = nullptr;

    ((IUnknown*)pArgs)->QueryInterface(winrt::guid_of<Input::PointerRoutedEventArgs>(), winrt::put_abi(args));

    if (!args)

        return original();

    EnsureLiveConfigMapped();
    ApplyTaskbarAppearance(element);
    ApplyTrayItemVisibility(element);
    if (HandleLayoutPointerMove(element, args)) {
        return 0;
    }
    if (!g_lightencyDockConfig.dockAnimation) {
        OnTaskbarPointerExited(pThis);
        return original();
    }


    if (g_contexts.find(pThis) == g_contexts.end()) {

        InitializeAnimationHooks(pThis, element);

        Wh_Log(L"DockAnimation: Initialized via OnPointerMoved (%s)", className.c_str());

    }


    OnTaskbarPointerMoved(pThis, args);

    return original();

}


using TaskbarFrame_OnPointerExited_t = int(WINAPI*)(void* pThis, void* pArgs);

TaskbarFrame_OnPointerExited_t TaskbarFrame_OnPointerExited_Original;


int WINAPI TaskbarFrame_OnPointerExited_Hook(void* pThis, void* pArgs) {

    auto original = [=]() {

        return TaskbarFrame_OnPointerExited_Original(pThis, pArgs);

    };

    FrameworkElement element = nullptr;

    ((IUnknown*)pThis)->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));

    if (!element)

        return original();

    auto className = winrt::get_class_name(element);

    if (className != L"Taskbar.TaskbarFrame")

        return original();

    OnTaskbarPointerExited(pThis);

    return original();

}

using SystemTrayIcon_OnPointerMoved_t = int(WINAPI*)(void* pThis, void* pArgs);
SystemTrayIcon_OnPointerMoved_t SystemTrayIcon_OnPointerMoved_Original;
SystemTrayIcon_OnPointerMoved_t SystemTrayFrame_OnPointerMoved_Original;

static FrameworkElement ProjectShellXamlElement(void* pThis) {
    FrameworkElement element = nullptr;
    if (pThis) {
        IUnknown* projected = reinterpret_cast<IUnknown**>(pThis)[1];
        if (projected) {
            projected->QueryInterface(
                winrt::guid_of<FrameworkElement>(), winrt::put_abi(element));
        }
    }
    return element;
}

static Input::PointerRoutedEventArgs ProjectPointerArgs(void* pArgs) {
    Input::PointerRoutedEventArgs args = nullptr;
    if (pArgs) {
        reinterpret_cast<IUnknown*>(pArgs)->QueryInterface(
            winrt::guid_of<Input::PointerRoutedEventArgs>(), winrt::put_abi(args));
    }
    return args;
}

int WINAPI SystemTrayFrame_OnPointerMoved_Hook(void* pThis, void* pArgs) {
    auto element = ProjectShellXamlElement(pThis);
    auto args = ProjectPointerArgs(pArgs);
    if (element && args) {
        EnsureLiveConfigMapped();
        ApplyTaskbarAppearance(element);
        ApplyTrayItemVisibility(element);
        if (HandleLayoutPointerMove(element, args)) return 0;
    }
    return SystemTrayFrame_OnPointerMoved_Original(pThis, pArgs);
}

int WINAPI SystemTrayIcon_OnPointerMoved_Hook(void* pThis, void* pArgs) {
    auto element = ProjectShellXamlElement(pThis);
    if (element) {
        EnsureLiveConfigMapped();
        ApplyTaskbarAppearance(element);
        ApplyTrayItemVisibility(element);
        auto args = ProjectPointerArgs(pArgs);
        if (args && HandleLayoutPointerMove(element, args)) {
            return 0;
        }
    }
    return SystemTrayIcon_OnPointerMoved_Original(pThis, pArgs);
}


void LoadSettings() {

    if (!g_lightencyDockConfig.dockAnimation) {
        g_settings.maxScale = 1.0;
        g_settings.effectRadius = 0;
        g_settings.spacingFactor = 0.0;
        return;
    }
    int rawScale = g_lightencyDockConfig.maxScale;


    if (rawScale < 101) {

        rawScale = 101;

    } else if (rawScale > 220) {

        rawScale = 220;

    }


    g_settings.animationType = g_lightencyDockConfig.animationType;

    g_settings.maxScale = (double)rawScale / 100.0;


    g_settings.effectRadius = g_lightencyDockConfig.effectRadius;

    if (g_settings.effectRadius <= 0) g_settings.effectRadius = 100;


    g_settings.spacingFactor = (double)g_lightencyDockConfig.spacingFactor / 100.0;

    if (g_settings.spacingFactor < 0.0) g_settings.spacingFactor = 0.5;


    g_settings.bounceDelay = 100;

    if (g_settings.bounceDelay < 0) g_settings.bounceDelay = 100;


    g_settings.focusDuration = (double)150;

    if (g_settings.focusDuration <= 0) g_settings.focusDuration = 150.0;


    g_settings.disableVerticalBounce = (bool)1;

    g_settings.taskbarLabelsMode = (bool)0;

    g_settings.excludeSystemButtonsMode = (g_lightencyDockConfig.excludeSystemButtons ? 2 : 0);


    g_settings.lerpSpeed = (double)60;

    if (g_settings.lerpSpeed < 0) g_settings.lerpSpeed = 0;

    if (g_settings.lerpSpeed > 60) g_settings.lerpSpeed = 60;


    g_settings.disableBounce = (bool)(g_lightencyDockConfig.disableBounce ? 1 : 0);


    Wh_Log(L"DockAnimation: Settings loaded.");

}


HMODULE GetTaskbarViewModuleHandle() {

    HMODULE module = GetModuleHandle(L"Taskbar.View.dll");

    if (!module) module = GetModuleHandle(L"ExplorerExtensions.dll");

    return module;

}

HMODULE GetSystemTrayModuleHandle() {
    return GetModuleHandleW(L"SystemTray.dll");
}

bool HookSystemTrayDllSymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            { LR"(SystemTray IconView OnPointerMoved)" },
            &SystemTrayIcon_OnPointerMoved_Original,
            SystemTrayIcon_OnPointerMoved_Hook,
        },
    };

    bool iconHooked = HookSymbols(
        module, hooks, ARRAYSIZE(hooks),
        L"implementation::IconView,winrt::Windows::UI::Xaml::Controls::IControlOverrides");

    WindhawkUtils::SYMBOL_HOOK frameHooks[] = {
        {
            { LR"(SystemTray SystemTrayFrame OnPointerMoved)" },
            &SystemTrayFrame_OnPointerMoved_Original,
            SystemTrayFrame_OnPointerMoved_Hook,
        },
    };
    bool frameHooked = HookSymbols(
        module, frameHooks, ARRAYSIZE(frameHooks),
        L"implementation::SystemTrayFrame,winrt::Windows::UI::Xaml::Controls::IControlOverrides");
    return iconHooked || frameHooked;
}


bool HookTaskbarViewDllSymbols(HMODULE module) {


    WindhawkUtils::SYMBOL_HOOK taskbarViewHooks[] = {

        {

            {

                LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerMoved(void *))"

            },

            &TaskbarFrame_OnPointerMoved_Original,

            TaskbarFrame_OnPointerMoved_Hook,

        },

        {

            {

                LR"(public: virtual int __cdecl winrt::impl::produce<struct winrt::Taskbar::implementation::TaskbarFrame,struct winrt::Windows::UI::Xaml::Controls::IControlOverrides>::OnPointerExited(void *))"

            },

            &TaskbarFrame_OnPointerExited_Original,

            TaskbarFrame_OnPointerExited_Hook,

        },

    };

    if (!HookSymbols(module, taskbarViewHooks, ARRAYSIZE(taskbarViewHooks), L"TaskbarFrame")) {

        Wh_Log(L"DockAnimation: HookSymbols failed.");

        return false;

    }

    Wh_Log(L"DockAnimation: HookSymbols succeeded (Pointer events only).");

    return true;

}


using LoadLibraryExW_t = decltype(&LoadLibraryExW);

LoadLibraryExW_t LoadLibraryExW_Original;


HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {

    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);

    if (!module) return module;

    if (!g_taskbarViewDllLoaded && GetTaskbarViewModuleHandle() == module) {

        if (!g_taskbarViewDllLoaded.exchange(true)) {

            Wh_Log(L"DockAnimation: Taskbar.View.dll loaded, hooking symbols...");

            if (HookTaskbarViewDllSymbols(module)) {

                if (!g_hooksApplied.exchange(true)) {


                    Wh_Log(L"DockAnimation: Hooks applied (slow path).");

                }

            }

        }

    }

    return module;

}


BOOL Wh_ModInit() {

    Wh_Log(L"DockAnimation: Wh_ModInit");

    LoadSettings();

    if (!g_startMenuWinEventHook) {
        g_startMenuWinEventHook = SetWinEventHook(
            EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
            StartMenuWinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }

    if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle()) {

        g_taskbarViewDllLoaded = true;

        if (!HookTaskbarViewDllSymbols(taskbarViewModule)) return FALSE;

    } else {

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");

        auto pKernelBaseLibraryExW =

            (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule, "LoadLibraryExW");

        WindhawkUtils::SetFunctionHook(

            pKernelBaseLibraryExW,

            LoadLibraryExW_Hook,

            &LoadLibraryExW_Original);

    }

    return TRUE;

}


void Wh_ModAfterInit() {

    Wh_Log(L"DockAnimation: Wh_ModAfterInit");


    if (g_taskbarViewDllLoaded) {

        g_hooksApplied = true;

        Wh_Log(L"DockAnimation: Hooks already applied by Windhawk (fast path).");

    }

}


typedef void (*RunFromWindowThreadProc_t)(PVOID);


bool RunFromWindowThread(HWND hWnd,

                         RunFromWindowThreadProc_t proc,

                         PVOID procParam) {

    static const UINT runFromWindowThreadRegisteredMsg =

        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {

        RunFromWindowThreadProc_t proc;

        PVOID procParam;

    };


    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);

    if (dwThreadId == 0) {

        return false;

    }


    if (dwThreadId == GetCurrentThreadId()) {

        proc(procParam);

        return true;

    }


    HHOOK hook = SetWindowsHookEx(

        WH_CALLWNDPROC,

        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {

            if (nCode == HC_ACTION) {

                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;

                if (cwp->message == runFromWindowThreadRegisteredMsg) {

                    RUN_FROM_WINDOW_THREAD_PARAM* param =

                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;

                    param->proc(param->procParam);

                }

            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);

        },

        nullptr, dwThreadId);

    if (!hook) {

        return false;

    }


    RUN_FROM_WINDOW_THREAD_PARAM param;

    param.proc = proc;

    param.procParam = procParam;

    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);


    UnhookWindowsHookEx(hook);

    return true;

}


void Wh_ModBeforeUninit() {

    Wh_Log(L"DockAnimation: Wh_ModBeforeUninit (safe cleanup)");

    if (g_startMenuWinEventHook) {
        UnhookWinEvent(g_startMenuWinEventHook);
        g_startMenuWinEventHook = nullptr;
    }
    RestoreDefaultLayout();
    RestoreTaskbarAppearance();


    g_activeContextKey = nullptr;

    g_isBouncing = false;

    g_bounceIntensity = 0.0;

    g_isMouseInside = false;

    g_animationIntensity = 0.0;


    try {

        if (g_isRenderingHooked.exchange(false)) {

            Media::CompositionTarget::Rendering(g_renderingToken);

            Wh_Log(L"DockAnimation: Unhooked CompositionTarget::Rendering.");

        }

    }

    catch (winrt::hresult_error const& e) {

        Wh_Log(L"DockAnimation: HRESULT error unhooking rendering: %s",

               e.message().c_str());

    }


    std::map<HWND, std::vector<winrt::weak_ref<FrameworkElement>>> windowIcons;

    for (auto& pair : g_contexts) {

        auto& ctx = pair.second;

        HWND hWnd = ctx.hWnd ? ctx.hWnd : FindWindow(L"Shell_TrayWnd", NULL);

        if (hWnd) {

            auto& vec = windowIcons[hWnd];

            for (auto& icon : ctx.icons) {

                vec.push_back(icon.element);

            }

        }

    }


    for (auto& pair : windowIcons) {

        HWND hWnd = pair.first;

        std::vector<winrt::weak_ref<FrameworkElement>> elements = std::move(pair.second);

        std::function<void()> action = [elements = std::move(elements)]() {

            try {

                for (auto& weak_el : elements) {

                    if (auto element = weak_el.get()) {

                        ResetElementTransforms(element);

                    }

                }

            }

            catch (...) {}

        };

        RunFromWindowThread(

            hWnd,

            [](PVOID p) {

                auto* fn = static_cast<std::function<void()>*>(p);

                (*fn)();

            },

            &action);

    }


    g_contexts.clear();

    g_taskbarViewDllLoaded = false;

    g_hooksApplied = false;

}


void Wh_ModSettingsChanged() {

    Wh_Log(L"DockAnimation: Settings changed.");

    LoadSettings();

    if (!g_lightencyDockConfig.layoutEditor) {


        RestoreDefaultLayout();
    }

    if (!g_lightencyDockConfig.trayItems) {
        RestoreTrayVisibility();
    }


    std::map<HWND, std::vector<DockAnimationContext*>> windowContexts;

    for (auto& pair : g_contexts) {

        auto& ctx = pair.second;

        HWND hWnd = ctx.hWnd ? ctx.hWnd : FindWindow(L"Shell_TrayWnd", NULL);

        if (hWnd) {

            windowContexts[hWnd].push_back(&ctx);

        }

    }


    for (auto& pair : windowContexts) {

        HWND hWnd = pair.first;

        std::vector<DockAnimationContext*> ctxs = std::move(pair.second);

        std::function<void()> action = [ctxs = std::move(ctxs)]() {

            try {

                for (auto* ctx : ctxs) {

                    ResetAllIconScales(ctx->icons);

                    if (auto frame = ctx->taskbarFrame.get()) {
                        ApplyTaskbarAppearance(frame);
                        ApplyTrayItemVisibility(frame);
                    }

                    ctx->isInitialized = false;

                    ctx->icons.clear();

                }

            } catch (...) {}

        };

        RunFromWindowThread(

            hWnd,

            [](PVOID p) {

                auto* fn = static_cast<std::function<void()>*>(p);

                (*fn)();

            },

            &action);

    }

    g_isBouncing = false;

}

namespace Lightency {

bool DockAnimation::Initialize() {
    LogDock(L"DockAnimation::Initialize start");
    static bool s_minHookInited = false;
    if (!s_minHookInited) {
        MH_STATUS s = MH_Initialize();
        LogDock(std::wstring(L"MH_Initialize=") + std::to_wstring(s));
        s_minHookInited = true;
    }
    BOOL bInit = Wh_ModInit();
    LogDock(std::wstring(L"Wh_ModInit result=") + std::to_wstring(bInit));
    Wh_ModAfterInit();
    return bInit != FALSE;
}

void DockAnimation::Shutdown() {
    LogDock(L"DockAnimation::Shutdown");
    Wh_ModBeforeUninit();
}

void DockAnimation::UpdateSettings(const SharedHookConfig& config) {
    LogDock(std::wstring(L"DockAnimation::UpdateSettings enabled=") + std::to_wstring(config.dockAnimation) + L" scale=" + std::to_wstring(config.maxScale) + L" radius=" + std::to_wstring(config.effectRadius));
    g_lightencyDockConfig = config;
    Wh_ModSettingsChanged();
}

void DockAnimation::RefreshSettings() {
    EnsureLiveConfigMapped();
    Wh_ModSettingsChanged();
}

void DockAnimation::OnWindowCreated(HWND) {}

}
