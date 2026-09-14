#include "config.h"
#include <shlobj.h>
#include <algorithm>

namespace Lightency {

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_NAME = L"Lightency";

std::wstring ConfigManager::GetConfigPath() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        std::wstring localIni = path.substr(0, lastSlash + 1) + L"config.ini";
        if (GetFileAttributesW(localIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return localIni;
        }
    }
    wchar_t appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\Lightency";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\config.ini";
    }
    return L"config.ini";
}

void ConfigManager::Init() {
    std::wstring path = GetConfigPath();
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        AppConfig defaultCfg;
        Save(defaultCfg);
    }
}

AppConfig ConfigManager::Load() {
    AppConfig cfg;
    std::wstring path = GetConfigPath();
    auto ReadBool = [&](const wchar_t* sec, const wchar_t* key, bool def) -> bool {
        return GetPrivateProfileIntW(sec, key, def ? 1 : 0, path.c_str()) != 0;
    };

    cfg.clearTaskbar = ReadBool(L"Settings", L"ClearTaskbar", true);
    cfg.hideTaskbarBorder = ReadBool(L"Settings", L"HideTaskbarBorder", true);
    cfg.dockAnimation = ReadBool(L"Settings", L"DockAnimation", true);
    cfg.dockMaxScale = GetPrivateProfileIntW(L"Settings", L"DockMaxScale", 135, path.c_str());
    if (cfg.dockMaxScale < 110 || cfg.dockMaxScale > 180) cfg.dockMaxScale = 135;
    cfg.dockAutoPhysics = ReadBool(L"Settings", L"DockAutoPhysics", true);
    cfg.dockRadius = GetPrivateProfileIntW(L"Settings", L"DockRadius", 45, path.c_str());
    if (cfg.dockRadius < 30 || cfg.dockRadius > 200) cfg.dockRadius = 45;
    cfg.dockSpacing = GetPrivateProfileIntW(L"Settings", L"DockSpacing", 50, path.c_str());
    if (cfg.dockSpacing < 10 || cfg.dockSpacing > 100) cfg.dockSpacing = 50;
    cfg.dockBounce = ReadBool(L"Settings", L"DockBounce", true);
    cfg.dockExcludeSystem = ReadBool(L"Settings", L"DockExcludeSystem", true);
    cfg.trayItems = ReadBool(L"Settings", L"TrayItems", false);
    cfg.hideTrayChevron = ReadBool(L"Settings", L"HideTrayChevron", false);
    cfg.hideTrayLanguage = ReadBool(L"Settings", L"HideTrayLanguage", false);
    cfg.hideTrayNetwork = ReadBool(L"Settings", L"HideTrayNetwork", false);
    cfg.hideTrayVolume = ReadBool(L"Settings", L"HideTrayVolume", false);
    cfg.hideTrayBattery = ReadBool(L"Settings", L"HideTrayBattery", false);
    cfg.hideTrayClock = ReadBool(L"Settings", L"HideTrayClock", false);
    cfg.layoutEditor = ReadBool(L"Settings", L"LayoutEditor", true);
    cfg.dragDropAssist = ReadBool(L"Settings", L"DragDropAssist", true);
    cfg.startMenuSizing = ReadBool(L"StartMenu", L"Enabled", false);
    cfg.startMenuAdvanced = ReadBool(L"StartMenu", L"Advanced", false);
    cfg.startMenuScale = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"StartMenu", L"Scale", 100, path.c_str())), 75, 150);
    cfg.startMenuWidth = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"StartMenu", L"Width", 640, path.c_str())), 320, 1400);
    cfg.startMenuHeight = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"StartMenu", L"Height", 720, path.c_str())), 400, 1200);
    cfg.searchWidth = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"StartMenu", L"SearchWidth", 640, path.c_str())), 320, 1400);
    cfg.searchHeight = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"StartMenu", L"SearchHeight", 720, path.c_str())), 400, 1200);
    cfg.startHideSearch = ReadBool(L"StartMenu", L"HideSearch", false);
    cfg.startHidePinned = ReadBool(L"StartMenu", L"HidePinned", false);
    cfg.startHideRecommended = ReadBool(L"StartMenu", L"HideRecommended", false);
    cfg.startHideProfile = ReadBool(L"StartMenu", L"HideProfile", false);
    cfg.startHidePower = ReadBool(L"StartMenu", L"HidePower", false);
    cfg.startHideViewSelector = ReadBool(L"StartMenu", L"HideViewSelector", false);
    cfg.startHideFolders = ReadBool(L"StartMenu", L"HideFolders", false);
    if (!cfg.startMenuAdvanced) {
        cfg.startMenuWidth = 960 * cfg.startMenuScale / 100;
        cfg.startMenuHeight = 720 * cfg.startMenuScale / 100;
        cfg.searchWidth = cfg.startMenuWidth;
        cfg.searchHeight = cfg.startMenuHeight;
    }
    cfg.startIconCustom = ReadBool(L"StartButton", L"CustomIcon", false);
    cfg.startIconSize = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"StartButton", L"IconSize", 100, path.c_str())), 40, 250);
    cfg.startIconAccentColor = ReadBool(L"StartButton", L"AccentColor", false);
    cfg.startIconColorHue = static_cast<int>(GetPrivateProfileIntW(L"StartButton", L"ColorHue", static_cast<UINT>(-1), path.c_str()));
    cfg.showTrayIcon = ReadBool(L"Settings", L"ShowTrayIcon", true);
    cfg.autostart = IsAutostartEnabled();
    cfg.automaticUpdates = ReadBool(L"Settings", L"AutomaticUpdates", true);
    cfg.windowGenieAnimation = ReadBool(L"Window", L"GenieAnimation", false);
    cfg.hideWindowBorders = ReadBool(L"Window", L"HideBorders", false);
    cfg.windowAnimationDuration = GetPrivateProfileIntW(L"Window", L"AnimationDuration", 420, path.c_str());
    cfg.windowCurveIntensity = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Window", L"CurveIntensity", 100, path.c_str())), 50, 125);
    cfg.windowTargetWidth = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Window", L"TargetWidth", 22, path.c_str())), 12, 64);
    cfg.windowRepeatGuard = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Window", L"RepeatGuard", 80, path.c_str())), 0, 500);
    cfg.windowCaptureDelay = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Window", L"CaptureDelay", 180, path.c_str())), 50, 500);
    if (cfg.windowAnimationDuration < 150 || cfg.windowAnimationDuration > 1000) cfg.windowAnimationDuration = 420;

    return cfg;
}

void ConfigManager::Save(const AppConfig& cfg) {
    std::wstring path = GetConfigPath();
    auto WriteBool = [&](const wchar_t* sec, const wchar_t* key, bool val) {
        WritePrivateProfileStringW(sec, key, val ? L"1" : L"0", path.c_str());
    };
    auto WriteInt = [&](const wchar_t* sec, const wchar_t* key, int val) {
        WritePrivateProfileStringW(sec, key, std::to_wstring(val).c_str(), path.c_str());
    };

    WriteBool(L"Settings", L"ClearTaskbar", cfg.clearTaskbar);
    WriteBool(L"Settings", L"HideTaskbarBorder", cfg.hideTaskbarBorder);
    WriteBool(L"Settings", L"DockAnimation", cfg.dockAnimation);
    WriteBool(L"Settings", L"DockAutoPhysics", cfg.dockAutoPhysics);
    WriteInt(L"Settings", L"DockMaxScale", cfg.dockMaxScale);
    WriteInt(L"Settings", L"DockRadius", cfg.dockRadius);
    WriteInt(L"Settings", L"DockSpacing", cfg.dockSpacing);
    WriteBool(L"Settings", L"DockBounce", cfg.dockBounce);
    WriteBool(L"Settings", L"DockExcludeSystem", cfg.dockExcludeSystem);
    WriteBool(L"Settings", L"TrayItems", cfg.trayItems);
    WriteBool(L"Settings", L"HideTrayChevron", cfg.hideTrayChevron);
    WriteBool(L"Settings", L"HideTrayLanguage", cfg.hideTrayLanguage);
    WriteBool(L"Settings", L"HideTrayNetwork", cfg.hideTrayNetwork);
    WriteBool(L"Settings", L"HideTrayVolume", cfg.hideTrayVolume);
    WriteBool(L"Settings", L"HideTrayBattery", cfg.hideTrayBattery);
    WriteBool(L"Settings", L"HideTrayClock", cfg.hideTrayClock);
    WriteBool(L"Settings", L"LayoutEditor", cfg.layoutEditor);
    WriteBool(L"Settings", L"DragDropAssist", cfg.dragDropAssist);
    WriteBool(L"StartMenu", L"Enabled", cfg.startMenuSizing);
    WriteBool(L"StartMenu", L"Advanced", cfg.startMenuAdvanced);
    WriteInt(L"StartMenu", L"Scale", cfg.startMenuScale);
    WriteInt(L"StartMenu", L"Width", cfg.startMenuWidth);
    WriteInt(L"StartMenu", L"Height", cfg.startMenuHeight);
    WriteInt(L"StartMenu", L"SearchWidth", cfg.searchWidth);
    WriteInt(L"StartMenu", L"SearchHeight", cfg.searchHeight);
    WriteBool(L"StartMenu", L"HideSearch", cfg.startHideSearch);
    WriteBool(L"StartMenu", L"HidePinned", cfg.startHidePinned);
    WriteBool(L"StartMenu", L"HideRecommended", cfg.startHideRecommended);
    WriteBool(L"StartMenu", L"HideProfile", cfg.startHideProfile);
    WriteBool(L"StartMenu", L"HidePower", cfg.startHidePower);
    WriteBool(L"StartMenu", L"HideViewSelector", cfg.startHideViewSelector);
    WriteBool(L"StartMenu", L"HideFolders", cfg.startHideFolders);
    WriteBool(L"StartButton", L"CustomIcon", cfg.startIconCustom);
    WriteInt(L"StartButton", L"IconSize", cfg.startIconSize);
    WriteBool(L"StartButton", L"AccentColor", cfg.startIconAccentColor);
    WriteInt(L"StartButton", L"ColorHue", cfg.startIconColorHue);
    WriteBool(L"Settings", L"ShowTrayIcon", cfg.showTrayIcon);
    WriteBool(L"Settings", L"AutomaticUpdates", cfg.automaticUpdates);
    WriteBool(L"Window", L"GenieAnimation", cfg.windowGenieAnimation);
    WriteBool(L"Window", L"HideBorders", cfg.hideWindowBorders);
    WriteInt(L"Window", L"AnimationDuration", cfg.windowAnimationDuration);
    WriteInt(L"Window", L"CurveIntensity", cfg.windowCurveIntensity);
    WriteInt(L"Window", L"TargetWidth", cfg.windowTargetWidth);
    WritePrivateProfileStringW(L"Window", L"FrameRate", nullptr, path.c_str());
    WriteInt(L"Window", L"RepeatGuard", cfg.windowRepeatGuard);
    WriteInt(L"Window", L"CaptureDelay", cfg.windowCaptureDelay);
    SetAutostart(cfg.autostart);
}

void ConfigManager::SetAutostart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --daemon";
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, APP_NAME);
        }
        RegCloseKey(hKey);
    }
}

bool ConfigManager::IsAutostartEnabled() {
    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD size = sizeof(buf);
        if (RegQueryValueExW(hKey, APP_NAME, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
            enabled = true;
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

}
