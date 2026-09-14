#pragma once
#include <windows.h>
#include <vector>

namespace Lightency::DragDropAssist {
void SetEnabled(bool enabled);
void UpdateTargets(void* taskbar, HWND window, const std::vector<RECT>& targets);
void Remove(void* taskbar);
void Shutdown();
}
