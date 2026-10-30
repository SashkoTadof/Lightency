#pragma once
#include <windows.h>

namespace Lightency {
class UpdateManager {
public:
    static void Start(HWND owner);
    static void CheckNow(HWND owner);
};
}
