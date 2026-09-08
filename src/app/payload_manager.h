#pragma once
#include <string>
namespace Lightency {
class PayloadManager {
public:
    static std::wstring GetHookDllPath();
    static std::wstring GetSymSrvDllPath();
private:
};
}
