#pragma once
#include <windows.h>
#include "../common/types.h"

namespace Lightency::BorderManager {

void Update(const AppConfig& config);
void Refresh();
void Shutdown();

}
