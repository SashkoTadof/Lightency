#pragma once
#include "../common/types.h"
#include <string>
namespace Lightency {
class ConfigManager {
public:
    static void Init();
    static AppConfig Load();
    static void Save(const AppConfig& config);
    static void SetAutostart(bool enable);
    static bool IsAutostartEnabled();
private:
    static std::wstring GetConfigPath();
};
}
