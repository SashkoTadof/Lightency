#pragma once
#include "types.h"
#include <vector>
namespace Lightency {
std::vector<HWND> GetAllTaskbars();
void ApplyClearToAllTaskbars();
void ApplyNormalToAllTaskbars();
}
