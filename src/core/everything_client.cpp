#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "everything_client.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
std::wstring env(const wchar_t* name)
{
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0)
    {
        return {};
    }
    std::wstring value(needed, L'\0');
    GetEnvironmentVariableW(name, value.data(), needed);
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return value;
}

std::wstring executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe(path);
    const auto slash = exe.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : exe.substr(0, slash);
}

std::string toUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr);
    return result;
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

std::wstring urlEncode(const std::wstring& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string utf8 = toUtf8(value);
    std::string out;
    out.reserve(utf8.size() * 3);

    for (unsigned char c : utf8)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out.push_back(static_cast<char>(c));
        }
        else if (c == ' ')
        {
            out.push_back('+');
        }
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }

    return std::wstring(out.begin(), out.end());
}

std::string base64Encode(const std::string& input)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3)
    {
        const unsigned int b0 = static_cast<unsigned char>(input[i]);
        const unsigned int b1 = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
        const unsigned int b2 = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < input.size() ? table[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < input.size() ? table[triple & 0x3F] : '=');
    }

    return out;
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
    std::wstring value = object.substr(start, pos - start);
    if (value == L"null")
    {
        return std::nullopt;
    }
    return value;
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

std::optional<ULONGLONG> parseUnsigned64(const std::wstring& text)
{
    if (text.empty())
    {
        return std::nullopt;
    }
    try
    {
        size_t parsed = 0;
        const auto value = std::stoull(text, &parsed, 10);
        return parsed == text.size() ? std::optional<ULONGLONG>(value) : std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::wstring pathFromEverythingObject(const std::wstring& object)
{
    if (auto direct = jsonValueText(object, L"full_path"))
    {
        return *direct;
    }
    if (auto direct = jsonValueText(object, L"fullpath"))
    {
        return *direct;
    }
    if (auto direct = jsonValueText(object, L"filename"))
    {
        return *direct;
    }

    const std::wstring path = jsonValueText(object, L"path").value_or(L"");
    const std::wstring name = jsonValueText(object, L"name").value_or(L"");
    if (!path.empty() && !name.empty())
    {
        return (fs::path(path) / name).wstring();
    }
    return !name.empty() ? name : path;
}

std::vector<FileResultEntry> parseEverythingHttpResults(const std::wstring& json)
{
    std::vector<FileResultEntry> entries;
    size_t resultsPos = json.find(L"\"results\"");
    if (resultsPos == std::wstring::npos)
    {
        return entries;
    }

    size_t arrayPos = json.find(L'[', resultsPos);
    if (arrayPos == std::wstring::npos)
    {
        return entries;
    }

    size_t pos = arrayPos + 1;
    while ((pos = json.find(L'{', pos)) != std::wstring::npos)
    {
        const size_t end = findObjectEnd(json, pos);
        if (end == std::wstring::npos)
        {
            break;
        }

        const std::wstring object = json.substr(pos, end - pos + 1);
        FileResultEntry entry;
        entry.path = pathFromEverythingObject(object);
        if (!entry.path.empty())
        {
            if (auto size = jsonValueText(object, L"size"))
            {
                if (auto bytes = parseUnsigned64(*size))
                {
                    entry.sizeText = formatFileSize(*bytes);
                }
            }
            if (auto modified = jsonValueText(object, L"date_modified"))
            {
                if (auto fileTimeValue = parseUnsigned64(*modified))
                {
                    FILETIME ft{};
                    ft.dwLowDateTime = static_cast<DWORD>(*fileTimeValue & 0xffffffffULL);
                    ft.dwHighDateTime = static_cast<DWORD>(*fileTimeValue >> 32);
                    entry.modifiedText = formatFileTime(ft);
                }
                else
                {
                    entry.modifiedText = *modified;
                }
            }
            entries.push_back(std::move(entry));
        }
        pos = end + 1;
    }

    return entries;
}
}

std::wstring formatFileSize(ULONGLONG bytes)
{
    constexpr const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < static_cast<int>(std::size(units)) - 1)
    {
        value /= 1024.0;
        ++unit;
    }

    wchar_t buffer[64]{};
    if (unit == 0)
    {
        swprintf_s(buffer, L"%llu %s", bytes, units[unit]);
    }
    else
    {
        swprintf_s(buffer, L"%.1f %s", value, units[unit]);
    }
    return buffer;
}

std::wstring formatFileTime(const FILETIME& fileTime)
{
    if (fileTime.dwLowDateTime == 0 && fileTime.dwHighDateTime == 0)
    {
        return {};
    }

    FILETIME localFileTime{};
    SYSTEMTIME systemTime{};
    if (!FileTimeToLocalFileTime(&fileTime, &localFileTime) || !FileTimeToSystemTime(&localFileTime, &systemTime))
    {
        return {};
    }

    wchar_t date[64]{};
    wchar_t time[64]{};
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &systemTime, nullptr, date, static_cast<int>(std::size(date)));
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &systemTime, nullptr, time, static_cast<int>(std::size(time)));
    return std::wstring(date) + L" " + time;
}

FileResultEntry fileEntryFromPath(const std::wstring& path)
{
    FileResultEntry entry;
    entry.path = path;

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            entry.sizeText = L"<DIR>";
        }
        else
        {
            const ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            entry.sizeText = formatFileSize(size);
        }
        entry.modifiedText = formatFileTime(data.ftLastWriteTime);
    }

    return entry;
}

std::wstring fileEntrySubtitle(const FileResultEntry& entry)
{
    std::wstring subtitle;
    subtitle += entry.sizeText.empty() ? L"-" : entry.sizeText;
    subtitle += L" | ";
    subtitle += entry.modifiedText.empty() ? L"-" : entry.modifiedText;
    return subtitle;
}

bool EverythingSdkClient::load()
{
    if (loaded_)
    {
        return true;
    }
    if (attempted_)
    {
        return false;
    }

    attempted_ = true;
    const auto appDir = executableDirectory();
    std::vector<std::wstring> candidates = {
        appDir + L"\\Everything64.dll",
        L"Everything64.dll",
    };

    if (auto pf = env(L"ProgramFiles"); !pf.empty())
    {
        candidates.push_back(pf + L"\\Everything\\Everything64.dll");
    }
    if (auto pf86 = env(L"ProgramFiles(x86)"); !pf86.empty())
    {
        candidates.push_back(pf86 + L"\\Everything\\Everything64.dll");
    }

    for (const auto& candidate : candidates)
    {
        module_ = LoadLibraryW(candidate.c_str());
        if (!module_)
        {
            continue;
        }

        setSearch_ = reinterpret_cast<SetSearchFn>(GetProcAddress(module_, "Everything_SetSearchW"));
        setRequestFlags_ = reinterpret_cast<SetRequestFlagsFn>(GetProcAddress(module_, "Everything_SetRequestFlags"));
        setMax_ = reinterpret_cast<SetMaxFn>(GetProcAddress(module_, "Everything_SetMax"));
        query_ = reinterpret_cast<QueryFn>(GetProcAddress(module_, "Everything_QueryW"));
        getNumResults_ = reinterpret_cast<GetNumResultsFn>(GetProcAddress(module_, "Everything_GetNumResults"));
        getFullPath_ = reinterpret_cast<GetFullPathFn>(GetProcAddress(module_, "Everything_GetResultFullPathNameW"));
        getSize_ = reinterpret_cast<GetSizeFn>(GetProcAddress(module_, "Everything_GetResultSize"));
        getDateModified_ = reinterpret_cast<GetDateModifiedFn>(GetProcAddress(module_, "Everything_GetResultDateModified"));

        loaded_ = setSearch_ && setRequestFlags_ && setMax_ && query_ && getNumResults_ && getFullPath_ && getSize_ && getDateModified_;
        if (loaded_)
        {
            loadedPath_ = candidate;
            return true;
        }

        FreeLibrary(module_);
        module_ = nullptr;
    }

    return false;
}

bool EverythingSdkClient::loaded() const
{
    return loaded_;
}

std::wstring EverythingSdkClient::loadedPath() const
{
    return loadedPath_;
}

std::vector<FileResultEntry> EverythingSdkClient::search(const std::wstring& query, DWORD maxResults)
{
    std::vector<FileResultEntry> entries;
    if (!loaded_ || query.empty())
    {
        return entries;
    }

    constexpr DWORD requestFullPath = 0x00000004;
    constexpr DWORD requestSize = 0x00000010;
    constexpr DWORD requestDateModified = 0x00000040;
    setRequestFlags_(requestFullPath | requestSize | requestDateModified);
    setMax_(maxResults);
    setSearch_(query.c_str());

    if (!query_(TRUE))
    {
        return entries;
    }

    const DWORD count = std::min<DWORD>(getNumResults_(), maxResults);
    entries.reserve(count);
    for (DWORD i = 0; i < count; ++i)
    {
        wchar_t buffer[32768]{};
        const DWORD len = getFullPath_(i, buffer, static_cast<DWORD>(std::size(buffer)));
        if (len == 0)
        {
            continue;
        }

        FileResultEntry entry;
        entry.path = buffer;

        LARGE_INTEGER size{};
        if (getSize_(i, &size))
        {
            entry.sizeText = formatFileSize(static_cast<ULONGLONG>(size.QuadPart));
        }

        FILETIME modified{};
        if (getDateModified_(i, &modified))
        {
            entry.modifiedText = formatFileTime(modified);
        }

        entries.push_back(std::move(entry));
    }

    return entries;
}

EverythingHttpClient::EverythingHttpClient()
{
    session_ = WinHttpOpen(L"QuickPal/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (session_)
    {
        WinHttpSetTimeouts(session_, 80, 80, 250, 350);
    }
}

EverythingHttpClient::~EverythingHttpClient()
{
    if (session_)
    {
        WinHttpCloseHandle(session_);
    }
}

EverythingHttpSearchResult EverythingHttpClient::search(const EverythingHttpSettings& settings, const std::wstring& query, int maxResults)
{
    EverythingHttpSearchResult result;
    if (!session_ || settings.host.empty() || settings.port <= 0)
    {
        return result;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < skipUntilMs_)
    {
        return result;
    }

    HINTERNET connect = WinHttpConnect(session_, settings.host.c_str(), static_cast<INTERNET_PORT>(settings.port), 0);
    if (!connect)
    {
        skipUntilMs_ = now + 3000;
        return result;
    }

    const std::wstring path = L"/?search=" + urlEncode(query) +
        L"&json=1&count=" + std::to_wstring(std::max(1, maxResults)) +
        L"&offset=0&path_column=1&size_column=1&date_modified_column=1";
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        skipUntilMs_ = now + 3000;
        return result;
    }

    std::wstring authHeader;
    if (!settings.username.empty() || !settings.password.empty())
    {
        const std::string token = base64Encode(toUtf8(settings.username + L":" + settings.password));
        authHeader = L"Authorization: Basic " + std::wstring(token.begin(), token.end()) + L"\r\n";
        WinHttpAddRequestHeaders(request, authHeader.c_str(), static_cast<DWORD>(authHeader.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    BOOL ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (!ok)
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        skipUntilMs_ = now + 3000;
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    result.statusCode = statusCode;
    if (statusCode != 200)
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        skipUntilMs_ = now + 3000;
        return result;
    }

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        const size_t oldSize = body.size();
        body.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + oldSize, available, &read))
        {
            break;
        }
        body.resize(oldSize + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);

    result.entries = parseEverythingHttpResults(fromUtf8(body));
    result.ok = true;
    skipUntilMs_ = 0;
    return result;
}
