#pragma once
#include <windows.h>
#include <string>

namespace Lightency::BrandedDialog {
enum class Kind { Info, Warning, Error, Confirm };
bool Show(HWND owner, const std::wstring& message, Kind kind = Kind::Info,
    const std::wstring& title = L"Lightency");
}
