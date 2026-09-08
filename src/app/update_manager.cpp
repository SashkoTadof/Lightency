#include "update_manager.h"
#include "../common/types.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#pragma comment(lib, "winhttp.lib")

namespace Lightency {
namespace {
constexpr wchar_t kLatestApi[] = L"/repos/SashkoTadof/Lightency/releases/latest";
std::atomic<bool> g_checkRunning{ false };

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

bool ReadLatestRelease(std::string& json) {
    InternetHandle session{ WinHttpOpen(L"Lightency/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.value) return false;
    WinHttpSetTimeouts(session.value, 4000, 4000, 6000, 6000);
    InternetHandle connection{ WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) };
    if (!connection.value) return false;
    InternetHandle request{ WinHttpOpenRequest(connection.value, L"GET", kLatestApi, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
    if (!request.value) return false;
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1), nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &size, nullptr) || status != 200) return false;
    std::vector<char> data;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) return false;
        if (!available) break;
        if (data.size() + available > 1024 * 1024) return false;
        size_t offset = data.size();
        data.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, data.data() + offset, available, &read)) return false;
        data.resize(offset + read);
    }
    json.assign(data.begin(), data.end());
    return true;
}

std::string JsonString(const std::string& json, const char* key) {
    std::string marker = "\"" + std::string(key) + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    size_t end = json.find('"', pos + 1);
    return end == std::string::npos ? std::string{} : json.substr(pos + 1, end - pos - 1);
}

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count, L' ');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

int ReadVersionPart(const std::wstring& value, size_t& pos) {
    int part = 0;
    while (pos < value.size() && value[pos] >= L'0' && value[pos] <= L'9')
        part = part * 10 + value[pos++] - L'0';
    if (pos < value.size() && value[pos] == L'.') ++pos;
    return part;
}

bool IsNewer(std::wstring remote) {
    std::wstring current = LIGHTENCY_VERSION;
    if (!remote.empty() && (remote[0] == L'v' || remote[0] == L'V')) remote.erase(0, 1);
    if (!current.empty() && (current[0] == L'v' || current[0] == L'V')) current.erase(0, 1);
    size_t a = 0, b = 0;
    for (int i = 0; i < 3; ++i) {
        int left = ReadVersionPart(remote, a), right = ReadVersionPart(current, b);
        if (left != right) return left > right;
    }
    return false;
}

std::wstring StatePath() {
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base))) return {};
    std::wstring dir = std::wstring(base) + L"\\Lightency";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\update_check.dat";
}

bool CheckDue() {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    std::wstring path = StatePath();
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return true;
    ULARGE_INTEGER modified{}, now{};
    modified.LowPart = info.ftLastWriteTime.dwLowDateTime;
    modified.HighPart = info.ftLastWriteTime.dwHighDateTime;
    FILETIME current{};
    GetSystemTimeAsFileTime(&current);
    now.LowPart = current.dwLowDateTime;
    now.HighPart = current.dwHighDateTime;
    return now.QuadPart <= modified.QuadPart || now.QuadPart - modified.QuadPart >= 24ull * 60 * 60 * 10000000;
}

void MarkChecked() {
    std::wstring path = StatePath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

void Check(HWND owner, bool manual) {
    if (g_checkRunning.exchange(true)) {
        if (manual) MessageBoxW(owner, L"An update check is already running.", L"Lightency update", MB_OK | MB_ICONINFORMATION);
        return;
    }
    struct Reset { ~Reset() { g_checkRunning = false; } } reset;
    if (!manual && !CheckDue()) return;
    std::string json;
    if (!ReadLatestRelease(json)) {
        if (manual) MessageBoxW(owner, L"Could not connect to GitHub Releases.", L"Lightency update", MB_OK | MB_ICONWARNING);
        return;
    }
    MarkChecked();
    std::wstring tag = ToWide(JsonString(json, "tag_name"));
    if (!IsNewer(tag)) {
        if (manual) MessageBoxW(owner, L"You already have the latest version.", L"Lightency update", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring page = ToWide(JsonString(json, "html_url"));
    if (page.empty()) page = L"https://github.com/SashkoTadof/Lightency/releases/latest";
    std::wstring message = L"Lightency " + tag + L" is available. Open the download page?";
    if (MessageBoxW(owner, message.c_str(), L"Lightency update", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
        ShellExecuteW(nullptr, L"open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}

void UpdateManager::Start(HWND owner) {
    std::thread([owner]() { Check(owner, false); }).detach();
}

void UpdateManager::CheckNow(HWND owner) {
    std::thread([owner]() { Check(owner, true); }).detach();
}
}
