#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

#define WM_LIGHTENCY_APPLY (WM_APP + 0x4242)
#define LIGHTENCY_VERSION L"v1.1.1"
#define LIGHTENCY_VERSION_STR "1.1.1"

enum class TaskbarEffect : int {
    Normal = 0,
    Clear = 1
};

enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2
};

struct ACCENT_POLICY {
    int AccentState;
    int AccentFlags;
    uint32_t GradientColor;
    int AnimationId;
};

struct WINCOMPATTRDATA {
    int Attribute;
    void* Data;
    unsigned long SizeOfData;
};

using pfnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);

namespace Lightency {

inline constexpr const wchar_t* SHARED_HOOK_CONFIG_MAPPING_NAME = L"Lightency_Shared_Config_v9";

struct SharedHookConfig {
    bool dockAnimation;
    int maxScale;
    int effectRadius;
    int spacingFactor;
    int animationType;
    bool disableBounce;
    bool excludeSystemButtons;
    bool autoRadius;
    bool autoPhysics;
    bool trayItems;
    bool hideTrayChevron;
    bool hideTrayLanguage;
    bool hideTrayNetwork;
    bool hideTrayVolume;
    bool hideTrayBattery;
    bool hideTrayClock;
    bool layoutEditor;
    bool dragDropAssist;
    bool clearTaskbar;
    bool hideTaskbarBorder;
    bool startMenuSizing;
    int startMenuWidth;
    int startMenuHeight;
    int searchWidth;
    int searchHeight;
    bool startHideSearch;
    bool startHidePinned;
    bool startHideRecommended;
    bool startHideProfile;
    bool startHidePower;
    bool startHideViewSelector;
    bool startHideFolders;
    bool startIconCustom;
    int startIconSize;
    bool startIconAccentColor;
    int startIconColorHue;
    DWORD masterPid;
};

inline UINT GetLightencyUpdateMsg() {
    static UINT msg = RegisterWindowMessageW(L"Lightency_UpdateHook_Msg");
    return msg;
}

struct AppConfig {
    bool clearTaskbar = true;
    bool hideTaskbarBorder = true;
    bool dockAnimation = true;
    bool dockAutoPhysics = true;
    int dockMaxScale = 135;
    int dockRadius = 45;
    int dockSpacing = 50;
    bool dockBounce = true;
    bool dockExcludeSystem = true;
    bool trayItems = false;
    bool hideTrayChevron = false;
    bool hideTrayLanguage = false;
    bool hideTrayNetwork = false;
    bool hideTrayVolume = false;
    bool hideTrayBattery = false;
    bool hideTrayClock = false;
    bool layoutEditor = true;
    bool dragDropAssist = true;
    bool startMenuSizing = false;
    bool startMenuAdvanced = false;
    int startMenuScale = 100;
    int startMenuWidth = 640;
    int startMenuHeight = 720;
    int searchWidth = 640;
    int searchHeight = 720;
    bool startHideSearch = false;
    bool startHidePinned = false;
    bool startHideRecommended = false;
    bool startHideProfile = false;
    bool startHidePower = false;
    bool startHideViewSelector = false;
    bool startHideFolders = false;
    bool startIconCustom = false;
    int startIconSize = 100;
    bool startIconAccentColor = false;
    int startIconColorHue = -1;
    bool showTrayIcon = true;
    bool autostart = false;
    bool automaticUpdates = true;
    bool windowGenieAnimation = false;
    bool hideWindowBorders = false;
    int windowAnimationDuration = 420;
    int windowCurveIntensity = 100;
    int windowTargetWidth = 22;
    int windowRepeatGuard = 80;
    int windowCaptureDelay = 180;
};

struct WindowAnimationIpc {
    DWORD masterPid;
    HWND receiverWindow;
    BOOL enabled;
};

inline UINT GetWindowAnimationEarlyMsg() {
    static UINT msg = RegisterWindowMessageW(L"Lightency_WindowAnimation_Early_v1");
    return msg;
}

}