#include "chrome_bridge.h"

#include "settings.h"
#include "util.h"

#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\QuickPalChromeTabs";

std::wstring jsonEscape(const std::wstring& value)
{
    std::wstring out;
    out.reserve(value.size() + 16);
    for (wchar_t ch : value)
    {
        switch (ch)
        {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string readFileBytes(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 8 * 1024 * 1024)
    {
        CloseHandle(file);
        return {};
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);
    return bytes;
}

void writeTextFile(const std::wstring& path, const std::wstring& text)
{
    const std::string bytes = toUtf8(text);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

std::wstring fromUtf8(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), needed);
    return result;
}

int hexValue(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
    if (ch >= L'A' && ch <= L'F') return 10 + ch - L'A';
    return -1;
}

std::optional<std::wstring> parseJsonStringAt(const std::wstring& json, size_t quotePos, size_t* nextPos = nullptr)
{
    if (quotePos >= json.size() || json[quotePos] != L'"')
    {
        return std::nullopt;
    }

    std::wstring out;
    for (size_t i = quotePos + 1; i < json.size(); ++i)
    {
        const wchar_t ch = json[i];
        if (ch == L'"')
        {
            if (nextPos)
            {
                *nextPos = i + 1;
            }
            return out;
        }
        if (ch != L'\\')
        {
            out.push_back(ch);
            continue;
        }
        if (++i >= json.size())
        {
            return std::nullopt;
        }

        switch (json[i])
        {
        case L'"': out.push_back(L'"'); break;
        case L'\\': out.push_back(L'\\'); break;
        case L'/': out.push_back(L'/'); break;
        case L'b': out.push_back(L'\b'); break;
        case L'f': out.push_back(L'\f'); break;
        case L'n': out.push_back(L'\n'); break;
        case L'r': out.push_back(L'\r'); break;
        case L't': out.push_back(L'\t'); break;
        case L'u':
        {
            if (i + 4 >= json.size())
            {
                return std::nullopt;
            }
            int value = 0;
            for (int n = 0; n < 4; ++n)
            {
                const int digit = hexValue(json[i + 1 + n]);
                if (digit < 0)
                {
                    return std::nullopt;
                }
                value = (value << 4) | digit;
            }
            out.push_back(static_cast<wchar_t>(value));
            i += 4;
            break;
        }
        default:
            out.push_back(json[i]);
            break;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> jsonValueText(const std::wstring& object, const std::wstring& key)
{
    const std::wstring needle = L"\"" + key + L"\"";
    size_t pos = object.find(needle);
    if (pos == std::wstring::npos)
    {
        return std::nullopt;
    }
    pos = object.find(L':', pos + needle.size());
    if (pos == std::wstring::npos)
    {
        return std::nullopt;
    }
    ++pos;
    while (pos < object.size() && std::iswspace(object[pos]))
    {
        ++pos;
    }
    if (pos >= object.size())
    {
        return std::nullopt;
    }
    if (object[pos] == L'"')
    {
        return parseJsonStringAt(object, pos);
    }
    const size_t start = pos;
    while (pos < object.size() && object[pos] != L',' && object[pos] != L'}' && !std::iswspace(object[pos]))
    {
        ++pos;
    }
    return object.substr(start, pos - start);
}

size_t findObjectEnd(const std::wstring& json, size_t start)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = start; i < json.size(); ++i)
    {
        const wchar_t ch = json[i];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == L'\\')
            {
                escaped = true;
            }
            else if (ch == L'"')
            {
                inString = false;
            }
            continue;
        }
        if (ch == L'"')
        {
            inString = true;
        }
        else if (ch == L'{')
        {
            ++depth;
        }
        else if (ch == L'}')
        {
            --depth;
            if (depth == 0)
            {
                return i;
            }
        }
    }
    return std::wstring::npos;
}

int jsonInt(const std::wstring& object, const std::wstring& key)
{
    if (auto value = jsonValueText(object, key))
    {
        return _wtoi(value->c_str());
    }
    return 0;
}

bool jsonBool(const std::wstring& object, const std::wstring& key)
{
    if (auto value = jsonValueText(object, key))
    {
        return lowerCopy(*value) == L"true" || *value == L"1";
    }
    return false;
}

std::wstring rootDirectory()
{
    fs::path bin(executableDirectory());
    return bin.parent_path().wstring();
}

std::wstring nativeHostManifestPath()
{
    return settingsDirectory() + L"\\" + kChromeNativeHostName + L".json";
}

std::wstring nativeHostExePath()
{
    return executableDirectory() + L"\\QuickPalChromeHost.exe";
}
}

std::wstring chromeExtensionDirectory()
{
    return (fs::path(rootDirectory()) / L"chrome-extension").wstring();
}

std::wstring chromeTabsCachePath()
{
    return settingsDirectory() + L"\\chrome_tabs.json";
}

void registerChromeNativeMessagingHost()
{
    const std::wstring manifestPath = nativeHostManifestPath();
    const std::wstring manifest =
        L"{\n"
        L"  \"name\": \"" + std::wstring(kChromeNativeHostName) + L"\",\n"
        L"  \"description\": \"QuickPal Chrome tab bridge\",\n"
        L"  \"path\": \"" + jsonEscape(nativeHostExePath()) + L"\",\n"
        L"  \"type\": \"stdio\",\n"
        L"  \"allowed_origins\": [\"chrome-extension://" + std::wstring(kChromeTabsExtensionId) + L"/\"]\n"
        L"}\n";
    writeTextFile(manifestPath, manifest);

    const std::wstring keyPath = L"Software\\Google\\Chrome\\NativeMessagingHosts\\" + std::wstring(kChromeNativeHostName);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(key, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(manifestPath.c_str()),
                       static_cast<DWORD>((manifestPath.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

void openChromeExtensionInstallLocation()
{
    registerChromeNativeMessagingHost();
    const std::wstring dir = chromeExtensionDirectory();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ShellExecuteW(nullptr, L"open", L"chrome.exe", L"chrome://extensions", nullptr, SW_SHOWNORMAL);
}

std::vector<ChromeTabInfo> readChromeTabsCache()
{
    std::vector<ChromeTabInfo> tabs;
    const std::wstring json = ::fromUtf8(readFileBytes(chromeTabsCachePath()));
    const size_t tabsPos = json.find(L"\"tabs\"");
    if (tabsPos == std::wstring::npos)
    {
        return tabs;
    }
    size_t pos = json.find(L'[', tabsPos);
    if (pos == std::wstring::npos)
    {
        return tabs;
    }
    while ((pos = json.find(L'{', pos)) != std::wstring::npos)
    {
        const size_t end = findObjectEnd(json, pos);
        if (end == std::wstring::npos)
        {
            break;
        }
        const std::wstring object = json.substr(pos, end - pos + 1);
        ChromeTabInfo tab;
        tab.tabId = jsonInt(object, L"id");
        tab.windowId = jsonInt(object, L"windowId");
        tab.title = jsonValueText(object, L"title").value_or(L"");
        tab.url = jsonValueText(object, L"url").value_or(L"");
        tab.active = jsonBool(object, L"active");
        if (tab.tabId > 0 && tab.windowId > 0 && (!tab.title.empty() || !tab.url.empty()))
        {
            tabs.push_back(std::move(tab));
        }
        pos = end + 1;
    }
    return tabs;
}

bool sendChromeTabCommand(const wchar_t* action, int windowId, int tabId)
{
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const std::wstring wide = std::wstring(action) + L"\t" +
        std::to_wstring(windowId) + L"\t" + std::to_wstring(tabId) + L"\n";
    const std::string message = toUtf8(wide);
    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
    CloseHandle(pipe);
    return ok && written == message.size();
}

bool activateChromeTab(int windowId, int tabId)
{
    return sendChromeTabCommand(L"activate", windowId, tabId);
}

bool closeChromeTab(int windowId, int tabId)
{
    return sendChromeTabCommand(L"close", windowId, tabId);
}

bool reloadChromeTab(int windowId, int tabId)
{
    return sendChromeTabCommand(L"reload", windowId, tabId);
}
