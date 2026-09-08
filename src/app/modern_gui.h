#pragma once
#include <windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <gdiplus.h>
#include "../common/types.h"
namespace Lightency {
class ModernGUI {
public:
    static void Show(HINSTANCE hInstance, AppConfig& config, void (*onConfigChanged)());
    static void Close();
    static bool IsVisible();
};
}
