#pragma once
#include <string>
#include "../common/diagnostics.h"

inline void LogDebug(const std::string& message) { WriteDiagnostic("app", message); }
