#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

#define WM_LIGHTENCY_APPLY (WM_APP + 0x4242)
#define LIGHTENCY_VERSION L"v1.0.0"
#define LIGHTENCY_VERSION_STR "1.0.0"

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
    bool clearTaskbar;
    bool hideTaskbarBorder;
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
    bool showTrayIcon = true;
    bool autostart = false;
    bool automaticUpdates = true;
};

}
