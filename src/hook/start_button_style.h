#pragma once
#include <windows.h>
#include <unknwn.h>
#include "../common/types.h"

namespace Lightency::StartButtonStyle {

void AttachTaskbar(IUnknown* taskbarFrame);
void UpdateSettings(const SharedHookConfig& config);
void RefreshSettings();
void Shutdown();

}
