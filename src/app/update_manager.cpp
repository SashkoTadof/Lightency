#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "update_manager.h"
#include "injector.h"
#include "branded_dialog.h"
#include "icon_gen.h"
#include "../common/types.h"
#include "../common/composition.h"
#include <windows.h>
#include <windowsx.h>
#include <unknwn.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowsapp.lib")

namespace Lightency {
namespace {

constexpr wchar_t kClassName[] = L"LightencyUpdateWindow";
constexpr wchar_t kLatestApi[] = L"/repos/SashkoTadof/Lightency/releases/latest";
constexpr UINT WM_UPDATE_PROGRESS = WM_USER + 301;
constexpr UINT WM_UPDATE_STATUS   = WM_USER + 302;
constexpr UINT WM_UPDATE_FAILED   = WM_USER + 303;
constexpr UINT WM_UPDATE_READY    = WM_USER + 304;

std::atomic<bool> g_checkRunning{ false };

struct GdiplusScope {
    ULONG_PTR token = 0;
    bool initialized = false;
    GdiplusScope() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
            initialized = true;
        }
    }
    ~GdiplusScope() {
        if (initialized) {
            Gdiplus::GdiplusShutdown(token);
        }
    }
};

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

struct ReleaseAsset {
    std::wstring name;
    std::wstring downloadUrl;
    uint64_t size = 0;
};

struct ReleaseInfo {
    std::wstring tag;
    std::wstring name;
    std::wstring body;
    std::wstring htmlUrl;
    std::wstring zipUrl;
    uint64_t zipSize = 0;
    bool isNewer = false;
};

enum class DialogMode {
    Details,
    Downloading,
    Extracting,
    Error
};

struct DialogState {
    ReleaseInfo info;
    DialogMode mode = DialogMode::Details;
    std::wstring statusText;
    std::wstring errorText;
    uint64_t downloadedBytes = 0;
    uint64_t totalBytes = 0;
    float progress = 0.0f;
    int scrollOffset = 0;
    int maxScroll = 0;
    int hoverButton = 0;
    bool hoverClose = false;
    float closeGlow = 0.0f;
    float primaryGlow = 0.0f;
    float secondaryGlow = 0.0f;
    float githubGlow = 0.0f;
    float cancelGlow = 0.0f;
    float scrollThumbGlow = 0.0f;
    bool isDraggingScroll = false;
    int dragStartY = 0;
    int dragStartOffset = 0;
    std::atomic<bool> cancelRequested{ false };
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    bool manual = false;
    HICON hAppIcon = nullptr;
    std::thread workerThread;
};

std::string UnescapeJsonString(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            ++i;
            switch (input[i]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 < input.size()) {
                    unsigned int codepoint = 0;
                    for (int j = 0; j < 4; ++j) {
                        char c = input[++i];
                        codepoint <<= 4;
                        if (c >= '0' && c <= '9') codepoint |= (c - '0');
                        else if (c >= 'a' && c <= 'f') codepoint |= (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') codepoint |= (c - 'A' + 10);
                    }
                    if (codepoint <= 0x7F) {
                        out += static_cast<char>(codepoint);
                    } else if (codepoint <= 0x7FF) {
                        out += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
                        out += static_cast<char>(0x80 | (codepoint & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
                        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (codepoint & 0x3F));
                    }
                }
                break;
            }
            default: out += input[i]; break;
            }
        } else {
            out += input[i];
        }
    }
    return out;
}

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count, L' ');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, ' ');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string FindJsonString(const std::string& json, const std::string& key, size_t startPos = 0) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern, startPos);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    size_t cur = pos + 1;
    while (cur < json.size()) {
        if (json[cur] == '\\') {
            cur += 2;
            continue;
        }
        if (json[cur] == '"') {
            return UnescapeJsonString(json.substr(pos + 1, cur - pos - 1));
        }
        ++cur;
    }
    return {};
}

uint64_t FindJsonInt(const std::string& json, const std::string& key, size_t startPos = 0) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern, startPos);
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return 0;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) ++pos;
    uint64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return val;
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

std::wstring FormatVersionTag(std::wstring tag) {
    if (tag.empty()) return {};
    if (tag[0] == L'v' || tag[0] == L'V') return tag;
    return L"v" + tag;
}

std::wstring FormatBytes(uint64_t bytes) {
    if (bytes == 0) return L"0 KB";
    double kb = bytes / 1024.0;
    if (kb < 1024.0) {
        std::wostringstream oss;
        oss << std::fixed << std::setprecision(1) << kb << L" KB";
        return oss.str();
    }
    double mb = kb / 1024.0;
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1) << mb << L" MB";
    return oss.str();
}

std::wstring CleanMarkdown(const std::wstring& markdown) {
    std::wstring clean;
    clean.reserve(markdown.size());
    std::wistringstream stream(markdown);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(L" \t");
        if (start == std::wstring::npos) continue;
        std::wstring content = line.substr(start);
        if (content.size() >= 2 && (content.substr(0, 2) == L"- " || content.substr(0, 2) == L"* " || content.substr(0, 2) == L"+ ")) {
            clean += L"• " + content.substr(2) + L"\r\n";
        } else if (content.size() >= 3 && content.substr(0, 3) == L"###") {
            size_t idx = 3;
            while (idx < content.size() && content[idx] == L' ') ++idx;
            clean += content.substr(idx) + L":\r\n";
        } else if (content.size() >= 2 && content.substr(0, 2) == L"##") {
            size_t idx = 2;
            while (idx < content.size() && content[idx] == L' ') ++idx;
            clean += content.substr(idx) + L"\r\n";
        } else if (content.size() >= 1 && content[0] == L'#') {
            size_t idx = 1;
            while (idx < content.size() && content[idx] == L' ') ++idx;
            clean += content.substr(idx) + L"\r\n";
        } else {
            clean += content + L"\r\n";
        }
    }

    size_t pos = 0;
    while ((pos = clean.find(L"**", pos)) != std::wstring::npos) {
        clean.erase(pos, 2);
    }

    pos = 0;
    while ((pos = clean.find(L"```", pos)) != std::wstring::npos) {
        clean.erase(pos, 3);
    }

    pos = 0;
    while ((pos = clean.find(L'`', pos)) != std::wstring::npos) {
        clean.erase(pos, 1);
    }

    pos = 0;
    while ((pos = clean.find(L'[', pos)) != std::wstring::npos) {
        size_t closeBracket = clean.find(L']', pos);
        if (closeBracket != std::wstring::npos && closeBracket + 1 < clean.size() && clean[closeBracket + 1] == L'(') {
            size_t closeParen = clean.find(L')', closeBracket + 2);
            if (closeParen != std::wstring::npos) {
                std::wstring label = clean.substr(pos + 1, closeBracket - pos - 1);
                clean.replace(pos, closeParen - pos + 1, label);
                pos += label.size();
                continue;
            }
        }
        ++pos;
    }

    while (!clean.empty() && (clean.back() == L'\r' || clean.back() == L'\n')) {
        clean.pop_back();
    }

    return clean;
}

std::wstring ExtractChanges(const std::wstring& markdown) {
    std::wistringstream stream(markdown);
    std::wstring line;
    std::vector<std::wstring> lines;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
    }

    bool inChanges = false;
    std::wstring extracted;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring trimmed = lines[i];
        size_t start = trimmed.find_first_not_of(L" \t#*");
        std::wstring lower;
        if (start != std::wstring::npos) {
            lower = trimmed.substr(start);
            for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
        }

        bool isChangesHeader = (lower.rfind(L"changes", 0) == 0 ||
                                lower.rfind(L"what's changed", 0) == 0 ||
                                lower.rfind(L"changelog", 0) == 0 ||
                                lower.rfind(L"what's new", 0) == 0);

        if (isChangesHeader) {
            inChanges = true;
            continue;
        }

        if (inChanges) {
            bool isNextHeader = (trimmed.size() >= 1 && (trimmed[0] == L'#' ||
                                 lower.rfind(L"requirements", 0) == 0 ||
                                 lower.rfind(L"installation", 0) == 0 ||
                                 lower.rfind(L"notes", 0) == 0 ||
                                 lower.rfind(L"downloads", 0) == 0 ||
                                 lower.rfind(L"prerequisites", 0) == 0));
            if (isNextHeader) {
                break;
            }
            extracted += lines[i] + L"\n";
        }
    }

    if (extracted.empty()) {
        for (const auto& l : lines) {
            std::wstring trimmed = l;
            size_t start = trimmed.find_first_not_of(L" \t");
            if (start != std::wstring::npos) {
                if (trimmed.compare(start, 2, L"- ") == 0 ||
                    trimmed.compare(start, 2, L"* ") == 0 ||
                    trimmed.compare(start, 2, L"+ ") == 0) {
                    extracted += l + L"\n";
                }
            }
        }
    }

    if (extracted.empty()) {
        extracted = markdown;
    }

    return CleanMarkdown(extracted);
}

bool ReadLatestRelease(std::string& json) {
    InternetHandle session{ WinHttpOpen(L"Lightency-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.value) return false;
    WinHttpSetTimeouts(session.value, 5000, 5000, 8000, 8000);
    InternetHandle connection{ WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) };
    if (!connection.value) return false;
    InternetHandle request{ WinHttpOpenRequest(connection.value, L"GET", kLatestApi, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
    if (!request.value) return false;
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nUser-Agent: Lightency\r\n";
    if (!WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1), nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &size, nullptr) || status != 200) return false;
    std::vector<char> data;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available) || !available) break;
        if (data.size() + available > 4 * 1024 * 1024) break;
        size_t offset = data.size();
        data.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, data.data() + offset, available, &read)) break;
        data.resize(offset + read);
    }
    json.assign(data.begin(), data.end());
    return !json.empty();
}

bool ParseReleaseInfo(const std::string& jsonString, ReleaseInfo& info) {
    try {
        winrt::hstring wjson = winrt::to_hstring(jsonString);
        winrt::Windows::Data::Json::JsonObject root = winrt::Windows::Data::Json::JsonObject::Parse(wjson);

        if (root.HasKey(L"tag_name")) info.tag = root.GetNamedString(L"tag_name").c_str();
        if (root.HasKey(L"name")) info.name = root.GetNamedString(L"name").c_str();
        if (root.HasKey(L"body")) info.body = ExtractChanges(std::wstring(root.GetNamedString(L"body").c_str()));
        if (root.HasKey(L"html_url")) info.htmlUrl = root.GetNamedString(L"html_url").c_str();
        
        if (info.htmlUrl.empty()) info.htmlUrl = L"https://github.com/SashkoTadof/Lightency/releases/latest";
        if (info.name.empty()) info.name = L"Lightency " + info.tag;

        if (root.HasKey(L"assets")) {
            auto assets = root.GetNamedArray(L"assets");
            for (uint32_t i = 0; i < assets.Size(); ++i) {
                auto asset = assets.GetObjectAt(i);
                if (asset.HasKey(L"name") && asset.HasKey(L"browser_download_url")) {
                    std::wstring name = asset.GetNamedString(L"name").c_str();
                    std::wstring url = asset.GetNamedString(L"browser_download_url").c_str();
                    
                    if (name.find(L"Lightency") != std::wstring::npos &&
                        name.find(L"symbols") == std::wstring::npos &&
                        name.find(L"source") == std::wstring::npos &&
                        name.size() >= 4 && name.substr(name.size() - 4) == L".zip") {
                        
                        info.zipUrl = url;
                        if (asset.HasKey(L"size")) {
                            info.zipSize = static_cast<uint64_t>(asset.GetNamedNumber(L"size"));
                        }
                        break;
                    }
                }
            }
        }
    } catch (const winrt::hresult_error&) {
        return false;
    }
    info.isNewer = IsNewer(info.tag);
    return !info.tag.empty();
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

std::wstring GetTempUpdateDir() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t guidStr[40];
    StringFromGUID2(guid, guidStr, 40);

    std::wstring dir = std::wstring(tempPath) + L"LightencyUpdate_" + guidStr;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool DownloadFile(const std::wstring& url, const std::wstring& destPath, uint64_t expectedSize,
                  std::function<void(uint64_t downloaded, uint64_t total)> progressCallback,
                  std::atomic<bool>& cancelFlag) {
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256]{};
    wchar_t urlPath[2048]{};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = ARRAYSIZE(hostName);
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = ARRAYSIZE(urlPath);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComp)) return false;
    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS) return false;

    InternetHandle session{ WinHttpOpen(L"Lightency-Downloader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    if (!session.value) return false;
    WinHttpSetTimeouts(session.value, 6000, 6000, 10000, 15000);

    InternetHandle connection{ WinHttpConnect(session.value, hostName, urlComp.nPort, 0) };
    if (!connection.value) return false;

    DWORD flags = WINHTTP_FLAG_SECURE;
    InternetHandle request{ WinHttpOpenRequest(connection.value, L"GET", urlPath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) };
    if (!request.value) return false;

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    const wchar_t headers[] = L"User-Agent: Lightency\r\n";
    if (!WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1), nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) return false;

    DWORD status = 0, statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &statusSize, nullptr) || status != 200) return false;

    DWORD contentLength = 0, clSize = sizeof(contentLength);
    uint64_t totalBytes = expectedSize;
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &contentLength, &clSize, nullptr)) {
        if (totalBytes == 0) totalBytes = contentLength;
    }

    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    std::vector<char> buffer(32 * 1024);
    uint64_t downloaded = 0;
    bool success = true;

    while (!cancelFlag) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) { success = false; break; }
        if (available == 0) break;

        DWORD toRead = (std::min)(available, static_cast<DWORD>(buffer.size()));
        DWORD readBytes = 0;
        if (!WinHttpReadData(request.value, buffer.data(), toRead, &readBytes)) { success = false; break; }
        if (readBytes == 0) break;

        DWORD written = 0;
        if (!WriteFile(hFile, buffer.data(), readBytes, &written, nullptr) || written != readBytes) {
            success = false;
            break;
        }

        downloaded += readBytes;
        if (progressCallback) progressCallback(downloaded, totalBytes > 0 ? totalBytes : downloaded);
    }

    CloseHandle(hFile);
    
    if (success && !cancelFlag && expectedSize > 0) {
        if (downloaded != expectedSize) success = false;
    }

    if (cancelFlag || !success) {
        DeleteFileW(destPath.c_str());
        return false;
    }
    return true;
}

std::wstring FindExecutableInDir(const std::wstring& rootDir) {
    std::wstring directExe = rootDir + L"\\lightency.exe";
    if (GetFileAttributesW(directExe.c_str()) != INVALID_FILE_ATTRIBUTES) return rootDir;

    WIN32_FIND_DATAW fd{};
    std::wstring pattern = rootDir + L"\\*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return {};

    std::wstring foundDir;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            std::wstring sub = rootDir + L"\\" + fd.cFileName;
            std::wstring testExe = sub + L"\\lightency.exe";
            if (GetFileAttributesW(testExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                foundDir = sub;
                break;
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return foundDir;
}

bool ExecuteUpdatePayload(const std::wstring& zipPath, std::wstring& errorMsg) {
    std::wstring tempDir = GetTempUpdateDir();
    std::wstring extractDir = tempDir + L"\\extracted";
    CreateDirectoryW(extractDir.c_str(), nullptr);

    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring tarCmd = std::wstring(sysDir) + L"\\tar.exe -xf \"" + zipPath + L"\" -C \"" + extractDir + L"\"";
    STARTUPINFOW siTar{ sizeof(siTar) };
    PROCESS_INFORMATION piTar{};
    siTar.dwFlags = STARTF_USESHOWWINDOW;
    siTar.wShowWindow = SW_HIDE;

    std::vector<wchar_t> cmdBuf(tarCmd.begin(), tarCmd.end());
    cmdBuf.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &siTar, &piTar)) {
        errorMsg = L"Failed to launch extraction utility (tar.exe).";
        return false;
    }
    WaitForSingleObject(piTar.hProcess, 30000);
    DWORD tarExit = 0;
    GetExitCodeProcess(piTar.hProcess, &tarExit);
    CloseHandle(piTar.hProcess);
    CloseHandle(piTar.hThread);

    if (tarExit != 0) {
        errorMsg = L"Extraction failed with code " + std::to_wstring(tarExit);
        return false;
    }

    std::wstring sourceDir = FindExecutableInDir(extractDir);
    if (sourceDir.empty()) {
        errorMsg = L"Updated lightency.exe was not found inside the release archive.";
        return false;
    }

    wchar_t currentExePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, currentExePath, MAX_PATH);
    std::wstring currentExe(currentExePath);
    size_t lastSlash = currentExe.find_last_of(L"\\/");
    std::wstring targetDir = (lastSlash != std::wstring::npos) ? currentExe.substr(0, lastSlash) : L"";
    if (targetDir.empty()) {
        errorMsg = L"Could not determine Lightency installation directory.";
        return false;
    }

    std::wstring exeOld = targetDir + L"\\lightency.exe.old";
    std::wstring dllOld = targetDir + L"\\lightency_hook.dll.old";
    
    // Kill StartMenuExperienceHost to force it to restart with the new DLL unloaded.
    STARTUPINFOW siKill{ sizeof(siKill) };
    PROCESS_INFORMATION piKill{};
    siKill.dwFlags = STARTF_USESHOWWINDOW;
    siKill.wShowWindow = SW_HIDE;
    std::wstring killCmd = std::wstring(sysDir) + L"\\taskkill.exe /F /IM StartMenuExperienceHost.exe";
    std::vector<wchar_t> killBuf(killCmd.begin(), killCmd.end());
    killBuf.push_back(L'\0');
    if (CreateProcessW(nullptr, killBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &siKill, &piKill)) {
        WaitForSingleObject(piKill.hProcess, 3000);
        CloseHandle(piKill.hProcess);
        CloseHandle(piKill.hThread);
    }

    // Delete any existing .old files first
    DeleteFileW(exeOld.c_str());
    DeleteFileW(dllOld.c_str());

    // Rename current files
    MoveFileW((targetDir + L"\\lightency.exe").c_str(), exeOld.c_str());
    MoveFileW((targetDir + L"\\lightency_hook.dll").c_str(), dllOld.c_str());

    // Copy new executables natively to ensure atomicity
    if (!CopyFileW((sourceDir + L"\\lightency.exe").c_str(), (targetDir + L"\\lightency.exe").c_str(), FALSE)) {
        // Rollback
        DeleteFileW((targetDir + L"\\lightency.exe").c_str());
        MoveFileW(exeOld.c_str(), (targetDir + L"\\lightency.exe").c_str());
        MoveFileW(dllOld.c_str(), (targetDir + L"\\lightency_hook.dll").c_str());
        errorMsg = L"Failed to overwrite lightency.exe.";
        return false;
    }

    CopyFileW((sourceDir + L"\\lightency_hook.dll").c_str(), (targetDir + L"\\lightency_hook.dll").c_str(), FALSE);

    // Xcopy the rest
    wchar_t sysDir2[MAX_PATH]{};
    GetSystemDirectoryW(sysDir2, MAX_PATH);
    std::wstring xcopyCmd = std::wstring(sysDir2) + L"\\cmd.exe /c xcopy /y /e /i /q \"" + sourceDir + L"\\*\" \"" + targetDir + L"\\\"";
    STARTUPINFOW siXcopy{ sizeof(siXcopy) };
    PROCESS_INFORMATION piXcopy{};
    siXcopy.dwFlags = STARTF_USESHOWWINDOW;
    siXcopy.wShowWindow = SW_HIDE;
    std::vector<wchar_t> xcopyBuf(xcopyCmd.begin(), xcopyCmd.end());
    xcopyBuf.push_back(L'\0');
    if (CreateProcessW(nullptr, xcopyBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &siXcopy, &piXcopy)) {
        WaitForSingleObject(piXcopy.hProcess, 10000);
        CloseHandle(piXcopy.hProcess);
        CloseHandle(piXcopy.hThread);
    }

    // Launch new process
    std::wstring launchCmd = L"\"" + targetDir + L"\\lightency.exe\" --cleanup-update \"" + tempDir + L"\"";
    STARTUPINFOW siLaunch{ sizeof(siLaunch) };
    PROCESS_INFORMATION piLaunch{};
    std::vector<wchar_t> launchBuf(launchCmd.begin(), launchCmd.end());
    launchBuf.push_back(L'\0');
    if (!CreateProcessW(nullptr, launchBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, targetDir.c_str(), &siLaunch, &piLaunch)) {
        errorMsg = L"Failed to start updated Lightency.";
        return false;
    }
    CloseHandle(piLaunch.hProcess);
    CloseHandle(piLaunch.hThread);


    ApplyNormalToAllTaskbars();
    Injector::Shutdown();
    ExitProcess(0);
    return true;
}

constexpr UINT_PTR kHoverAnimationTimer = 8;

static BYTE BlendByte(BYTE from, BYTE to, float amount) {
    return static_cast<BYTE>(from + (to - from) * std::clamp(amount, 0.0f, 1.0f));
}

static void DrawPill(Gdiplus::Graphics& g, const Gdiplus::Brush& brush, float x, float y, float w, float h) {
    float r = h / 2.0f;
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, 2 * r, 2 * r, 90, 180);
    path.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 180);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static void DrawPillOutline(Gdiplus::Graphics& g, const Gdiplus::Pen& pen, float x, float y, float w, float h) {
    const float inset = 0.5f;
    float r = (h - 1.0f) / 2.0f;
    Gdiplus::GraphicsPath path;
    path.AddArc(x + inset, y + inset, 2 * r, 2 * r, 90, 180);
    path.AddArc(x + w - inset - 2 * r, y + inset, 2 * r, 2 * r, 270, 180);
    path.CloseFigure();
    g.DrawPath(&pen, &path);
}

void FillRounded(Gdiplus::Graphics& g, const Gdiplus::RectF& rect, float radius, const Gdiplus::Color& color) {
    Gdiplus::GraphicsPath path;
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void DrawRoundedOutline(Gdiplus::Graphics& g, const Gdiplus::RectF& rect, float radius, const Gdiplus::Color& color, float width = 1.0f) {
    Gdiplus::GraphicsPath path;
    const float diameter = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180, 90);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270, 90);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    Gdiplus::Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

static float GetDpiScale(HWND hWnd) {
    if (!hWnd) return 1.0f;
    UINT dpi = GetDpiForWindow(hWnd);
    return (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
}

bool PtInRectF(const Gdiplus::RectF& r, const POINT& pt) {
    return pt.x >= r.X && pt.x <= r.GetRight() && pt.y >= r.Y && pt.y <= r.GetBottom();
}

Gdiplus::RectF GetHeaderCloseRect(int width, float scale) {
    return { width - 36.0f * scale, 0.0f, 36.0f * scale, 36.0f * scale };
}

Gdiplus::RectF GetPrimaryButtonRect(int width, int height, float scale) {
    float w = 92.0f * scale;
    float h = 28.0f * scale;
    return { width - 18.0f * scale - w, height - 14.0f * scale - h, w, h };
}

Gdiplus::RectF GetSecondaryButtonRect(int width, int height, float scale) {
    float w = 84.0f * scale;
    float h = 28.0f * scale;
    float primaryW = 92.0f * scale;
    return { width - 18.0f * scale - primaryW - 8.0f * scale - w, height - 14.0f * scale - h, w, h };
}

Gdiplus::RectF GetCancelButtonRect(int width, int height, float scale) {
    float w = 84.0f * scale;
    float h = 28.0f * scale;
    return { width - 18.0f * scale - w, height - 14.0f * scale - h, w, h };
}

Gdiplus::RectF GetGitHubButtonRect(int, int height, float scale) {
    float w = 78.0f * scale;
    float h = 28.0f * scale;
    return { 18.0f * scale, height - 14.0f * scale - h, w, h };
}

Gdiplus::RectF GetChangelogCardRect(int width, int height, float scale) {
    return { 18.0f * scale, 68.0f * scale, width - 36.0f * scale, height - 68.0f * scale - 48.0f * scale };
}

Gdiplus::RectF GetScrollThumbRect(const Gdiplus::RectF& card, float scale, int scrollOffset, int maxScroll) {
    float trackH = card.Height - 24.0f * scale;
    float trackTop = card.Y + 12.0f * scale;
    float trackX = card.X + card.Width - 8.0f * scale;
    if (maxScroll <= 0 || trackH <= 0) return { 0, 0, 0, 0 };
    float thumbH = (std::max)(20.0f * scale, trackH * trackH / (trackH + maxScroll));
    float thumbY = trackTop + (static_cast<float>(scrollOffset) / maxScroll) * (trackH - thumbH);
    return { trackX, thumbY, 4.0f * scale, thumbH };
}

void PaintDialog(HWND hwnd, DialogState* state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client{};
    GetClientRect(hwnd, &client);

    int width = client.right - client.left;
    int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    float scale = GetDpiScale(hwnd);

    HDC memDC = CreateCompatibleDC(dc);
    HBITMAP memBitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

    Gdiplus::Graphics g(memDC);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(Gdiplus::Color(255, 24, 26, 28));

    Gdiplus::Font captionFont(L"Segoe UI", 12.0f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font titleFont(L"Segoe UI", 13.0f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font subtitleFont(L"Segoe UI", 11.5f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font bodyFont(L"Segoe UI", 11.5f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font previewFont(L"Segoe UI", 11.0f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font buttonFont(L"Segoe UI", 11.5f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font smallBtnFont(L"Segoe UI", 11.0f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

    Gdiplus::SolidBrush textWhite(Gdiplus::Color(255, 242, 245, 248));
    Gdiplus::SolidBrush textDim(Gdiplus::Color(255, 140, 148, 158));
    Gdiplus::SolidBrush textBody(Gdiplus::Color(255, 218, 222, 228));
    Gdiplus::SolidBrush textMuted(Gdiplus::Color(255, 175, 182, 190));
    Gdiplus::SolidBrush cyanText(Gdiplus::Color(255, 0, 180, 255));

    Gdiplus::StringFormat centerFmt;
    centerFmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    centerFmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    int iconSz = static_cast<int>(18.0f * scale);
    int iconX = static_cast<int>(14.0f * scale);
    int iconY = static_cast<int>(9.0f * scale);
    if (state->hAppIcon) {
        DrawIconEx(memDC, iconX, iconY, state->hAppIcon, iconSz, iconSz, 0, nullptr, DI_NORMAL);
    }

    g.DrawString(L"Lightency", -1, &captionFont, Gdiplus::PointF(36.0f * scale, 8.0f * scale), &textWhite);

    Gdiplus::Pen closePen(Gdiplus::Color(255, BlendByte(180, 235, state->closeGlow), BlendByte(180, 75, state->closeGlow),
        BlendByte(180, 65, state->closeGlow)), 1.1f);
    closePen.SetStartCap(Gdiplus::LineCapRound);
    closePen.SetEndCap(Gdiplus::LineCapRound);

    float cx = width - 20.0f * scale;
    float cy = 18.0f * scale;
    float csz = 4.0f * scale;

    if (state->closeGlow > 0.01f) {
        Gdiplus::Pen closeHalo(Gdiplus::Color(static_cast<BYTE>(58.0f * state->closeGlow), 235, 75, 65), 3.0f * scale);
        closeHalo.SetStartCap(Gdiplus::LineCapRound);
        closeHalo.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&closeHalo, cx - csz, cy - csz, cx + csz, cy + csz);
        g.DrawLine(&closeHalo, cx + csz, cy - csz, cx - csz, cy + csz);
    }
    g.TranslateTransform(0.5f, 0.5f);
    g.DrawLine(&closePen, cx - csz, cy - csz, cx + csz, cy + csz);
    g.DrawLine(&closePen, cx + csz, cy - csz, cx - csz, cy + csz);
    g.TranslateTransform(-0.5f, -0.5f);

    Gdiplus::Pen sepPen(Gdiplus::Color(255, 34, 38, 44), 1.0f);
    g.DrawLine(&sepPen, 0.0f, 36.0f * scale, static_cast<float>(width), 36.0f * scale);

    if (state->mode == DialogMode::Details) {
        std::wstring heading = state->info.isNewer
            ? (L"Lightency " + FormatVersionTag(state->info.tag))
            : (L"Up to date (" + FormatVersionTag(LIGHTENCY_VERSION) + L")");

        g.DrawString(heading.c_str(), -1, &titleFont, Gdiplus::PointF(18.0f * scale, 45.0f * scale), &textWhite);

        Gdiplus::RectF card = GetChangelogCardRect(width, height, scale);
        FillRounded(g, card, 6.0f * scale, Gdiplus::Color(255, 18, 20, 23));
        DrawRoundedOutline(g, card, 6.0f * scale, Gdiplus::Color(255, 34, 38, 46), 1.0f);

        Gdiplus::Region oldClip;
        g.GetClip(&oldClip);
        Gdiplus::RectF clipRect(card.X + 14.0f * scale, card.Y + 12.0f * scale, card.Width - 28.0f * scale, card.Height - 24.0f * scale);
        g.SetClip(clipRect);

        Gdiplus::RectF textBound(clipRect.X, clipRect.Y - static_cast<float>(state->scrollOffset),
                                clipRect.Width - 10.0f * scale, 20000.0f);
        Gdiplus::StringFormat wrapFmt;
        wrapFmt.SetAlignment(Gdiplus::StringAlignmentNear);
        wrapFmt.SetLineAlignment(Gdiplus::StringAlignmentNear);

        std::wstring body = state->info.body.empty() ? L"No changes listed." : state->info.body;
        Gdiplus::RectF measured;
        g.MeasureString(body.c_str(), -1, &bodyFont, textBound, &wrapFmt, &measured);
        g.DrawString(body.c_str(), -1, &bodyFont, textBound, &wrapFmt, &textBody);

        state->maxScroll = (std::max)(0, static_cast<int>(measured.Height - clipRect.Height));

        g.SetClip(&oldClip);

        if (state->maxScroll > 0) {
            Gdiplus::RectF thumb = GetScrollThumbRect(card, scale, state->scrollOffset, state->maxScroll);
            BYTE thR = BlendByte(50, 85, state->scrollThumbGlow);
            BYTE thG = BlendByte(56, 96, state->scrollThumbGlow);
            BYTE thB = BlendByte(65, 110, state->scrollThumbGlow);
            FillRounded(g, thumb, 2.0f * scale, Gdiplus::Color(255, thR, thG, thB));
        }

        Gdiplus::RectF btnPrimary = GetPrimaryButtonRect(width, height, scale);
        if (state->primaryGlow > 0.01f) {
            Gdiplus::Pen halo(Gdiplus::Color(static_cast<BYTE>(48.0f * state->primaryGlow), 0, 210, 255), 2.5f * scale);
            DrawPillOutline(g, halo, btnPrimary.X, btnPrimary.Y, btnPrimary.Width, btnPrimary.Height);
        }
        BYTE prR = BlendByte(0, 35, state->primaryGlow);
        BYTE prG = BlendByte(180, 200, state->primaryGlow);
        BYTE prB = 255;
        Gdiplus::SolidBrush primaryBg(Gdiplus::Color(255, prR, prG, prB));
        DrawPill(g, primaryBg, btnPrimary.X, btnPrimary.Y, btnPrimary.Width, btnPrimary.Height);
        std::wstring primaryLabel = state->info.isNewer ? L"Update" : L"OK";
        g.DrawString(primaryLabel.c_str(), -1, &buttonFont, btnPrimary, &centerFmt, &textWhite);

        Gdiplus::RectF btnSecondary = GetSecondaryButtonRect(width, height, scale);
        BYTE secR = BlendByte(32, 44, state->secondaryGlow);
        BYTE secG = BlendByte(36, 50, state->secondaryGlow);
        BYTE secB = BlendByte(42, 60, state->secondaryGlow);
        Gdiplus::SolidBrush secBg(Gdiplus::Color(255, secR, secG, secB));
        DrawPill(g, secBg, btnSecondary.X, btnSecondary.Y, btnSecondary.Width, btnSecondary.Height);

        BYTE bdrR = BlendByte(52, 75, state->secondaryGlow);
        BYTE bdrG = BlendByte(58, 85, state->secondaryGlow);
        BYTE bdrB = BlendByte(68, 100, state->secondaryGlow);
        Gdiplus::Pen secBorder(Gdiplus::Color(255, bdrR, bdrG, bdrB), 1.0f);
        DrawPillOutline(g, secBorder, btnSecondary.X, btnSecondary.Y, btnSecondary.Width, btnSecondary.Height);

        BYTE txtR = BlendByte(218, 245, state->secondaryGlow);
        BYTE txtG = BlendByte(222, 248, state->secondaryGlow);
        BYTE txtB = BlendByte(228, 255, state->secondaryGlow);
        Gdiplus::SolidBrush secText(Gdiplus::Color(255, txtR, txtG, txtB));
        std::wstring secLabel = state->info.isNewer ? L"Later" : L"Reinstall";
        g.DrawString(secLabel.c_str(), -1, &buttonFont, btnSecondary, &centerFmt, &secText);

        Gdiplus::RectF btnGithub = GetGitHubButtonRect(width, height, scale);
        BYTE gitR = BlendByte(26, 38, state->githubGlow);
        BYTE gitG = BlendByte(30, 44, state->githubGlow);
        BYTE gitB = BlendByte(36, 54, state->githubGlow);
        Gdiplus::SolidBrush gitBg(Gdiplus::Color(255, gitR, gitG, gitB));
        DrawPill(g, gitBg, btnGithub.X, btnGithub.Y, btnGithub.Width, btnGithub.Height);

        BYTE gbR = BlendByte(46, 68, state->githubGlow);
        BYTE gbG = BlendByte(52, 78, state->githubGlow);
        BYTE gbB = BlendByte(60, 92, state->githubGlow);
        Gdiplus::Pen gitBorder(Gdiplus::Color(255, gbR, gbG, gbB), 1.0f);
        DrawPillOutline(g, gitBorder, btnGithub.X, btnGithub.Y, btnGithub.Width, btnGithub.Height);

        BYTE gtR = BlendByte(140, 200, state->githubGlow);
        BYTE gtG = BlendByte(148, 210, state->githubGlow);
        BYTE gtB = BlendByte(158, 225, state->githubGlow);
        Gdiplus::SolidBrush gitText(Gdiplus::Color(255, gtR, gtG, gtB));
        g.DrawString(L"GitHub", -1, &smallBtnFont, btnGithub, &centerFmt, &gitText);

    } else if (state->mode == DialogMode::Downloading || state->mode == DialogMode::Extracting) {
        std::wstring heading = (state->mode == DialogMode::Extracting) ? L"Installing..." : L"Downloading...";
        g.DrawString(heading.c_str(), -1, &titleFont, Gdiplus::PointF(28.0f * scale, 132.0f * scale), &textWhite);

        float barX = 28.0f * scale;
        float barY = 164.0f * scale;
        float barW = width - 56.0f * scale;
        float barH = 4.0f * scale;
        FillRounded(g, Gdiplus::RectF(barX, barY, barW, barH), 2.0f * scale, Gdiplus::Color(255, 34, 38, 46));

        float fillW = std::clamp(barW * state->progress, 0.0f, barW);
        if (fillW > 2.0f * scale) {
            FillRounded(g, Gdiplus::RectF(barX, barY, fillW, barH), 2.0f * scale, Gdiplus::Color(255, 0, 180, 255));
        }

        if (state->mode == DialogMode::Extracting) {
            g.DrawString(L"Preparing restart...", -1, &subtitleFont,
                         Gdiplus::PointF(28.0f * scale, 178.0f * scale), &textDim);
        } else {
            std::wstring byteInfo = FormatBytes(state->downloadedBytes) + L" / " + FormatBytes(state->totalBytes);
            g.DrawString(byteInfo.c_str(), -1, &subtitleFont, Gdiplus::PointF(28.0f * scale, 178.0f * scale), &textDim);

            std::wstring percent = std::to_wstring(static_cast<int>(state->progress * 100)) + L"%";
            Gdiplus::StringFormat rightFmt;
            rightFmt.SetAlignment(Gdiplus::StringAlignmentFar);
            rightFmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
            g.DrawString(percent.c_str(), -1, &subtitleFont,
                         Gdiplus::RectF(28.0f * scale, 178.0f * scale, barW, 20.0f * scale),
                         &rightFmt, &cyanText);
        }

        if (state->mode == DialogMode::Downloading) {
            Gdiplus::RectF btnCancel = GetCancelButtonRect(width, height, scale);
            BYTE cR = BlendByte(32, 44, state->cancelGlow);
            BYTE cG = BlendByte(36, 50, state->cancelGlow);
            BYTE cB = BlendByte(42, 60, state->cancelGlow);
            Gdiplus::SolidBrush cancelBg(Gdiplus::Color(255, cR, cG, cB));
            DrawPill(g, cancelBg, btnCancel.X, btnCancel.Y, btnCancel.Width, btnCancel.Height);

            BYTE cbR = BlendByte(52, 75, state->cancelGlow);
            BYTE cbG = BlendByte(58, 85, state->cancelGlow);
            BYTE cbB = BlendByte(68, 100, state->cancelGlow);
            Gdiplus::Pen cancelBorder(Gdiplus::Color(255, cbR, cbG, cbB), 1.0f);
            DrawPillOutline(g, cancelBorder, btnCancel.X, btnCancel.Y, btnCancel.Width, btnCancel.Height);

            BYTE ctR = BlendByte(218, 245, state->cancelGlow);
            BYTE ctG = BlendByte(222, 248, state->cancelGlow);
            BYTE ctB = BlendByte(228, 255, state->cancelGlow);
            Gdiplus::SolidBrush cancelText(Gdiplus::Color(255, ctR, ctG, ctB));
            g.DrawString(L"Cancel", -1, &buttonFont, btnCancel, &centerFmt, &cancelText);
        }

    } else if (state->mode == DialogMode::Error) {
        Gdiplus::SolidBrush errBrush(Gdiplus::Color(255, 240, 110, 110));
        g.DrawString(L"Update failed", -1, &titleFont, Gdiplus::PointF(28.0f * scale, 132.0f * scale), &errBrush);

        std::wstring err = state->errorText.empty() ? L"An unexpected error occurred." : state->errorText;
        g.DrawString(err.c_str(), -1, &bodyFont, Gdiplus::PointF(28.0f * scale, 162.0f * scale), &textBody);

        Gdiplus::RectF btnClose = GetPrimaryButtonRect(width, height, scale);
        if (state->primaryGlow > 0.01f) {
            Gdiplus::Pen halo(Gdiplus::Color(static_cast<BYTE>(48.0f * state->primaryGlow), 0, 210, 255), 2.5f * scale);
            DrawPillOutline(g, halo, btnClose.X, btnClose.Y, btnClose.Width, btnClose.Height);
        }
        BYTE prR = BlendByte(0, 35, state->primaryGlow);
        BYTE prG = BlendByte(180, 200, state->primaryGlow);
        BYTE prB = 255;
        Gdiplus::SolidBrush primaryBg(Gdiplus::Color(255, prR, prG, prB));
        DrawPill(g, primaryBg, btnClose.X, btnClose.Y, btnClose.Width, btnClose.Height);
        g.DrawString(L"Close", -1, &buttonFont, btnClose, &centerFmt, &textWhite);

        Gdiplus::RectF btnGithub = GetGitHubButtonRect(width, height, scale);
        BYTE gitR = BlendByte(26, 38, state->githubGlow);
        BYTE gitG = BlendByte(30, 44, state->githubGlow);
        BYTE gitB = BlendByte(36, 54, state->githubGlow);
        Gdiplus::SolidBrush gitBg(Gdiplus::Color(255, gitR, gitG, gitB));
        DrawPill(g, gitBg, btnGithub.X, btnGithub.Y, btnGithub.Width, btnGithub.Height);

        BYTE gbR = BlendByte(46, 68, state->githubGlow);
        BYTE gbG = BlendByte(52, 78, state->githubGlow);
        BYTE gbB = BlendByte(60, 92, state->githubGlow);
        Gdiplus::Pen gitBorder(Gdiplus::Color(255, gbR, gbG, gbB), 1.0f);
        DrawPillOutline(g, gitBorder, btnGithub.X, btnGithub.Y, btnGithub.Width, btnGithub.Height);

        BYTE gtR = BlendByte(140, 200, state->githubGlow);
        BYTE gtG = BlendByte(148, 210, state->githubGlow);
        BYTE gtB = BlendByte(158, 225, state->githubGlow);
        Gdiplus::SolidBrush gitText(Gdiplus::Color(255, gtR, gtG, gtB));
        g.DrawString(L"GitHub", -1, &smallBtnFont, btnGithub, &centerFmt, &gitText);
    }

    BitBlt(dc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

void StartDownloadWorker(DialogState* state) {
    state->mode = DialogMode::Downloading;
    state->progress = 0.0f;
    state->downloadedBytes = 0;
    state->totalBytes = state->info.zipSize;
    state->cancelRequested = false;
    InvalidateRect(state->hwnd, nullptr, FALSE);

    if (state->workerThread.joinable()) {
        state->workerThread.join();
    }

    state->workerThread = std::thread([state]() {
        std::wstring tempDir = GetTempUpdateDir();
        std::wstring destZip = tempDir + L"\\update.zip";

        bool ok = DownloadFile(state->info.zipUrl, destZip, state->info.zipSize,
            [state](uint64_t down, uint64_t total) {
                state->downloadedBytes = down;
                state->totalBytes = total;
                state->progress = (total > 0) ? std::clamp(static_cast<float>(down) / total, 0.0f, 1.0f) : 0.0f;
                PostMessageW(state->hwnd, WM_UPDATE_PROGRESS, 0, 0);
            },
            state->cancelRequested
        );

        if (state->cancelRequested) {
            state->mode = DialogMode::Details;
            PostMessageW(state->hwnd, WM_UPDATE_STATUS, 0, 0);
            return;
        }

        if (!ok) {
            state->errorText = L"Download failed. Please check your internet connection.";
            state->mode = DialogMode::Error;
            PostMessageW(state->hwnd, WM_UPDATE_FAILED, 0, 0);
            return;
        }

        state->mode = DialogMode::Extracting;
        state->progress = 1.0f;
        PostMessageW(state->hwnd, WM_UPDATE_STATUS, 0, 0);

        std::wstring errorMsg;
        if (!ExecuteUpdatePayload(destZip, errorMsg)) {
            state->errorText = errorMsg;
            state->mode = DialogMode::Error;
            PostMessageW(state->hwnd, WM_UPDATE_FAILED, 0, 0);
        }
    });
}

LRESULT CALLBACK UpdateWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<DialogState*>(cs->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state) PaintDialog(hwnd, state);
        return 0;
    case WM_TIMER: {
        if (wParam != kHoverAnimationTimer) break;
        if (!state) return 0;
        bool moving = false;
        auto animate = [&moving](float& value, float target) {
            const float delta = target - value;
            if (std::abs(delta) < 0.015f) value = target;
            else { value += delta * 0.24f; moving = true; }
        };
        animate(state->closeGlow, state->hoverClose ? 1.0f : 0.0f);
        animate(state->primaryGlow, (state->hoverButton == 1) ? 1.0f : 0.0f);
        animate(state->secondaryGlow, (state->hoverButton == 2) ? 1.0f : 0.0f);
        animate(state->githubGlow, (state->hoverButton == 3) ? 1.0f : 0.0f);
        animate(state->scrollThumbGlow, (state->hoverButton == 4) ? 1.0f : 0.0f);
        animate(state->cancelGlow, (state->hoverButton == 5) ? 1.0f : 0.0f);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (!moving) KillTimer(hwnd, kHoverAnimationTimer);
        return 0;
    }
    case WM_UPDATE_PROGRESS:
    case WM_UPDATE_STATUS:
    case WM_UPDATE_FAILED:
        if (state) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        if (!state || state->mode != DialogMode::Details) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        state->scrollOffset = std::clamp(state->scrollOffset - delta / 2, 0, state->maxScroll);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!state) return 0;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT client{};
        GetClientRect(hwnd, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        float scale = GetDpiScale(hwnd);

        if (state->isDraggingScroll) {
            Gdiplus::RectF card = GetChangelogCardRect(width, height, scale);
            float trackH = card.Height - 38.0f * scale;
            if (trackH > 0 && state->maxScroll > 0) {
                int dy = pt.y - state->dragStartY;
                int offsetDelta = static_cast<int>((static_cast<float>(dy) / trackH) * state->maxScroll);
                state->scrollOffset = std::clamp(state->dragStartOffset + offsetDelta, 0, state->maxScroll);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        bool newHoverClose = PtInRectF(GetHeaderCloseRect(width, scale), pt);
        int newHoverButton = 0;

        if (state->mode == DialogMode::Details) {
            if (PtInRectF(GetPrimaryButtonRect(width, height, scale), pt)) newHoverButton = 1;
            else if (PtInRectF(GetSecondaryButtonRect(width, height, scale), pt)) newHoverButton = 2;
            else if (PtInRectF(GetGitHubButtonRect(width, height, scale), pt)) newHoverButton = 3;
            else if (state->maxScroll > 0 && PtInRectF(GetScrollThumbRect(GetChangelogCardRect(width, height, scale), scale, state->scrollOffset, state->maxScroll), pt)) newHoverButton = 4;
        } else if (state->mode == DialogMode::Downloading) {
            if (PtInRectF(GetCancelButtonRect(width, height, scale), pt)) newHoverButton = 5;
        } else if (state->mode == DialogMode::Error) {
            if (PtInRectF(GetPrimaryButtonRect(width, height, scale), pt)) newHoverButton = 1;
            else if (PtInRectF(GetGitHubButtonRect(width, height, scale), pt)) newHoverButton = 3;
        }

        if (newHoverClose != state->hoverClose || newHoverButton != state->hoverButton) {
            state->hoverClose = newHoverClose;
            state->hoverButton = newHoverButton;
            SetTimer(hwnd, kHoverAnimationTimer, 16, nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (state) {
            if (state->hoverClose || state->hoverButton != 0) {
                state->hoverClose = false;
                state->hoverButton = 0;
                SetTimer(hwnd, kHoverAnimationTimer, 16, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!state) return 0;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT client{};
        GetClientRect(hwnd, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        float scale = GetDpiScale(hwnd);

        if (state->mode == DialogMode::Details && state->maxScroll > 0) {
            Gdiplus::RectF thumb = GetScrollThumbRect(GetChangelogCardRect(width, height, scale), scale, state->scrollOffset, state->maxScroll);
            if (PtInRectF(thumb, pt)) {
                state->isDraggingScroll = true;
                state->dragStartY = pt.y;
                state->dragStartOffset = state->scrollOffset;
                SetCapture(hwnd);
                return 0;
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!state) return 0;
        if (state->isDraggingScroll) {
            state->isDraggingScroll = false;
            ReleaseCapture();
            return 0;
        }

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT client{};
        GetClientRect(hwnd, &client);
        int width = client.right - client.left;
        int height = client.bottom - client.top;
        float scale = GetDpiScale(hwnd);

        if (PtInRectF(GetHeaderCloseRect(width, scale), pt)) {
            if (state->mode == DialogMode::Downloading) state->cancelRequested = true;
            DestroyWindow(hwnd);
            return 0;
        }

        if (state->mode == DialogMode::Details) {
            if (PtInRectF(GetPrimaryButtonRect(width, height, scale), pt)) {
                if (state->info.isNewer) {
                    if (state->info.zipUrl.empty()) {
                        ShellExecuteW(nullptr, L"open", state->info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        DestroyWindow(hwnd);
                    } else {
                        StartDownloadWorker(state);
                    }
                } else {
                    DestroyWindow(hwnd);
                }
            } else if (PtInRectF(GetSecondaryButtonRect(width, height, scale), pt)) {
                if (state->info.isNewer) {
                    DestroyWindow(hwnd);
                } else {
                    if (state->info.zipUrl.empty()) {
                        ShellExecuteW(nullptr, L"open", state->info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        DestroyWindow(hwnd);
                    } else {
                        StartDownloadWorker(state);
                    }
                }
            } else if (PtInRectF(GetGitHubButtonRect(width, height, scale), pt)) {
                ShellExecuteW(nullptr, L"open", state->info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else if (state->mode == DialogMode::Downloading) {
            if (PtInRectF(GetCancelButtonRect(width, height, scale), pt)) {
                state->cancelRequested = true;
            }
        } else if (state->mode == DialogMode::Error) {
            if (PtInRectF(GetPrimaryButtonRect(width, height, scale), pt)) {
                DestroyWindow(hwnd);
            } else if (PtInRectF(GetGitHubButtonRect(width, height, scale), pt)) {
                ShellExecuteW(nullptr, L"open", state->info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (state && state->mode == DialogMode::Downloading) state->cancelRequested = true;
            else DestroyWindow(hwnd);
            return 0;
        } else if (wParam == VK_RETURN) {
            if (state && state->mode == DialogMode::Details) {
                if (state->info.isNewer) {
                    if (state->info.zipUrl.empty()) {
                        ShellExecuteW(nullptr, L"open", state->info.htmlUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        DestroyWindow(hwnd);
                    } else {
                        StartDownloadWorker(state);
                    }
                } else {
                    DestroyWindow(hwnd);
                }
            } else if (state && state->mode == DialogMode::Error) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            RECT client{};
            GetClientRect(hwnd, &client);
            int width = client.right - client.left;
            int height = client.bottom - client.top;
            float scale = GetDpiScale(hwnd);

            if (!PtInRectF(GetHeaderCloseRect(width, scale), pt)) {
                if (state && state->mode == DialogMode::Details) {
                    if (PtInRectF(GetPrimaryButtonRect(width, height, scale), pt) ||
                        PtInRectF(GetSecondaryButtonRect(width, height, scale), pt) ||
                        PtInRectF(GetGitHubButtonRect(width, height, scale), pt)) {
                        return HTCLIENT;
                    }
                    if (state->maxScroll > 0) {
                        Gdiplus::RectF card = GetChangelogCardRect(width, height, scale);
                        if (PtInRectF(card, pt)) {
                            return HTCLIENT;
                        }
                    }
                } else if (state && state->mode == DialogMode::Downloading) {
                    if (PtInRectF(GetCancelButtonRect(width, height, scale), pt)) return HTCLIENT;
                } else if (state && state->mode == DialogMode::Error) {
                    if (PtInRectF(GetPrimaryButtonRect(width, height, scale), pt) ||
                        PtInRectF(GetGitHubButtonRect(width, height, scale), pt)) return HTCLIENT;
                }
                return HTCAPTION;
            }
        }
        return hit;
    }
    case WM_CLOSE:
        if (state && state->mode == DialogMode::Downloading) state->cancelRequested = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kHoverAnimationTimer);
        if (state && state->hAppIcon) {
            DestroyIcon(state->hAppIcon);
            state->hAppIcon = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowUpdateDialog(HWND owner, const ReleaseInfo& info, bool manual) {
    GdiplusScope gdiScope;

    static ATOM atom = [] {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = UpdateWndProc;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc);
    }();
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    DialogState state;
    state.info = info;
    state.owner = owner;
    state.manual = manual;
    state.mode = DialogMode::Details;

    HDC screenDC = GetDC(nullptr);
    int dpi = screenDC ? GetDeviceCaps(screenDC, LOGPIXELSX) : 96;
    if (screenDC) ReleaseDC(nullptr, screenDC);
    float scale = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;

    state.hAppIcon = IconGenerator::CreateMinimalistIcon(static_cast<int>(24.0f * scale));

    const int width = static_cast<int>(480.0f * scale);
    const int height = static_cast<int>(380.0f * scale);

    RECT anchor{};
    if (!owner || !IsWindowVisible(owner) || !GetWindowRect(owner, &anchor) || (anchor.right - anchor.left) < 100) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
    }
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;

    HWND window = CreateWindowExW(WS_EX_TOPMOST, kClassName, L"Lightency Update",
        WS_POPUP, x, y, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) return;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    int corner = 2;
    DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
    COLORREF borderColor = RGB(46, 52, 60);
    DwmSetWindowAttribute(window, 34, &borderColor, sizeof(borderColor));

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (state.workerThread.joinable()) {
        state.cancelRequested = true;
        state.workerThread.join();
    }
}

void PerformCheck(HWND owner, bool manual) {
    if (g_checkRunning.exchange(true)) {
        if (manual && IsWindow(owner))
            BrandedDialog::Show(owner, L"An update check is already in progress.", BrandedDialog::Kind::Info, L"Update");
        return;
    }
    struct ResetFlag { ~ResetFlag() { g_checkRunning = false; } } reset;

    if (!manual && !CheckDue()) return;

    std::string json;
    if (!ReadLatestRelease(json)) {
        if (manual && IsWindow(owner))
            BrandedDialog::Show(owner, L"Could not connect to GitHub Releases.", BrandedDialog::Kind::Warning, L"Update");
        return;
    }

    MarkChecked();

    ReleaseInfo info;
    if (!ParseReleaseInfo(json, info)) {
        if (manual && IsWindow(owner))
            BrandedDialog::Show(owner, L"Failed to read release details.", BrandedDialog::Kind::Warning, L"Update");
        return;
    }

    if (!info.isNewer && !manual) {
        return;
    }

    ShowUpdateDialog(owner, info, manual);
}

}

static std::thread g_checkThread;

void UpdateManager::Start(HWND owner) {
    if (g_checkRunning.load(std::memory_order_acquire)) return;
    if (g_checkThread.joinable()) g_checkThread.join();
    g_checkThread = std::thread([owner]() { PerformCheck(owner, false); });
}

void UpdateManager::CheckNow(HWND owner) {
    if (g_checkRunning.load(std::memory_order_acquire)) return;
    if (g_checkThread.joinable()) g_checkThread.join();
    g_checkThread = std::thread([owner]() { PerformCheck(owner, true); });
}

}
