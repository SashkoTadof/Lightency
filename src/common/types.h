#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

#define WM_LIGHTENCY_APPLY (WM_APP + 0x4242)
#include "version.h"

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

#include <atomic>

namespace Lightency {

inline constexpr const wchar_t* SHARED_HOOK_CONFIG_MAPPING_NAME = L"Lightency_Shared_Config_v10";

struct SharedHookConfig {
    std::atomic<uint32_t> seq;

    SharedHookConfig() = default;
    SharedHookConfig(const SharedHookConfig& other) {
        memcpy(this, &other, sizeof(SharedHookConfig));
    }
    SharedHookConfig& operator=(const SharedHookConfig& other) {
        if (this != &other) memcpy(this, &other, sizeof(SharedHookConfig));
        return *this;
    }

    void BeginWrite() {
        uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_release);
    }

    void EndWrite() {
        uint32_t s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_release);
    }

    static SharedHookConfig Read(const SharedHookConfig* mapped) {
        if (!mapped) return {};
        SharedHookConfig local;
        uint32_t s1, s2;
        do {
            s1 = mapped->seq.load(std::memory_order_acquire);
            if (s1 & 1) {
                YieldProcessor();
                continue;
            }
            memcpy(&local, mapped, sizeof(SharedHookConfig));
            s2 = mapped->seq.load(std::memory_order_acquire);
        } while (s1 != s2 || (s1 & 1));
        return local;
    }

    bool dockAnimation = true;
    int maxScale = 135;
    int effectRadius = 45;
    int spacingFactor = 50;
    int animationType = 0;
    bool disableBounce = false;
    bool excludeSystemButtons = true;
    bool autoRadius = true;
    bool autoPhysics = false;
    bool trayItems = false;
    bool hideTrayChevron = false;
    bool hideTrayLanguage = false;
    bool hideTrayNetwork = false;
    bool hideTrayVolume = false;
    bool hideTrayBattery = false;
    bool hideTrayClock = false;
    bool layoutEditor = false;
    bool dragDropAssist = true;
    bool clearTaskbar = true;
    bool hideTaskbarBorder = true;
    bool startMenuSizing = false;
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
    int startIconSize = 48;
    bool startIconAccentColor = false;
    int startIconColorHue = 0;
    DWORD masterPid = 0;
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