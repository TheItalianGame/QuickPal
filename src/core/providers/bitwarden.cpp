#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
constexpr DWORD kBwTimeoutMs = 30000;
constexpr DWORD kBwListTimeoutMs = 90000;
constexpr wchar_t kProviderId[] = L"bitwarden";
constexpr wchar_t kInstallUrl[] = L"https://bitwarden.com/help/cli/";
constexpr wchar_t kDefaultVaultUrl[] = L"https://vault.bitwarden.com";

ULONGLONG currentTimeMs()
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    constexpr ULONGLONG kUnixEpochAsFileTime = 116444736000000000ULL;
    return value.QuadPart > kUnixEpochAsFileTime ? (value.QuadPart - kUnixEpochAsFileTime) / 10000ULL : 0;
}

struct BwProcessResult
{
    bool started = false;
    bool timedOut = false;
    DWORD exitCode = 1;
    std::wstring output;
};

struct BwItem
{
    std::wstring id;
    std::wstring name;
    std::wstring uri;
    std::wstring domain;
    std::wstring folder;
    std::wstring vault;
    std::wstring username;
};

void secureClear(std::wstring& value)
{
    if (!value.empty())
    {
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
    }
}

void secureClear(std::string& value)
{
    if (!value.empty())
    {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

std::wstring trimCliOutput(std::wstring value)
{
    while (!value.empty() && (value.back() == L'\0' || value.back() == L'\r' ||
                             value.back() == L'\n' || value.back() == L' ' || value.back() == L'\t'))
    {
        value.pop_back();
    }
    while (!value.empty() && (value.front() == L'\r' || value.front() == L'\n' ||
                              value.front() == L' ' || value.front() == L'\t'))
    {
        value.erase(value.begin());
    }
    return value;
}

std::wstring quoteArg(const std::wstring& value)
{
    std::wstring out = L"\"";
    for (wchar_t ch : value)
    {
        if (ch == L'"')
        {
            out += L"\\\"";
        }
        else
        {
            out.push_back(ch);
        }
    }
    out += L"\"";
    return out;
}

std::wstring makeCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args)
{
    std::wstring command = quoteArg(exe);
    for (const auto& arg : args)
    {
        command += L" ";
        command += quoteArg(arg);
    }
    return command;
}

std::wstring findBwExe()
{
    const std::vector<std::wstring> candidates = {
        executableDirectory() + L"\\bw.exe",
        env(L"LOCALAPPDATA") + L"\\Programs\\Bitwarden CLI\\bw.exe",
        env(L"ProgramFiles") + L"\\Bitwarden CLI\\bw.exe",
        env(L"ProgramFiles") + L"\\Bitwarden\\bw.exe",
    };

    for (const auto& candidate : candidates)
    {
        if (!candidate.empty())
        {
            std::error_code ec;
            if (fs::exists(candidate, ec))
            {
                return candidate;
            }
        }
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = SearchPathW(nullptr, L"bw.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length >= buffer.size())
    {
        buffer.assign(static_cast<size_t>(length) + 1, L'\0');
        length = SearchPathW(nullptr, L"bw.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    }
    if (length > 0 && length < buffer.size())
    {
        buffer.resize(length);
        return buffer;
    }
    return {};
}

std::wstring sessionCachePath()
{
    return settingsDirectory() + L"\\bitwarden_session.bin";
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
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
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

void writeFileBytes(const std::wstring& path, const std::string& bytes)
{
    CreateDirectoryW(settingsDirectory().c_str(), nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void deleteFileQuietly(const std::wstring& path)
{
    DeleteFileW(path.c_str());
}

std::wstring hexBytes(const std::vector<unsigned char>& bytes)
{
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes)
    {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

int hexNibble(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9')
    {
        return ch - L'0';
    }
    if (ch >= L'a' && ch <= L'f')
    {
        return ch - L'a' + 10;
    }
    if (ch >= L'A' && ch <= L'F')
    {
        return ch - L'A' + 10;
    }
    return -1;
}

std::vector<unsigned char> bytesFromHex(const std::wstring& hex)
{
    if (hex.size() % 2 != 0)
    {
        return {};
    }

    std::vector<unsigned char> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = hexNibble(hex[i]);
        const int lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
        {
            return {};
        }
        bytes.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return bytes;
}

std::string protectForCurrentUser(const std::wstring& text)
{
    std::string bytes = toUtf8(text);
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(bytes.data());
    input.cbData = static_cast<DWORD>(bytes.size());

    DATA_BLOB output{};
    const BOOL ok = CryptProtectData(&input, L"QuickPal Bitwarden session", nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &output);
    secureClear(bytes);
    if (!ok)
    {
        return {};
    }

    std::string protectedBytes(reinterpret_cast<char*>(output.pbData),
                               reinterpret_cast<char*>(output.pbData) + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return protectedBytes;
}

std::wstring unprotectForCurrentUser(const std::string& bytes)
{
    if (bytes.empty())
    {
        return {};
    }

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()));
    input.cbData = static_cast<DWORD>(bytes.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
    {
        return {};
    }

    std::string plain(reinterpret_cast<char*>(output.pbData),
                      reinterpret_cast<char*>(output.pbData) + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);

    std::wstring text = fromUtf8(plain);
    secureClear(plain);
    return text;
}

bool envKeyMatches(const std::wstring& variable, const std::wstring& key)
{
    const size_t split = variable.find(L'=');
    if (split == std::wstring::npos || split != key.size())
    {
        return false;
    }
    return _wcsnicmp(variable.c_str(), key.c_str(), key.size()) == 0;
}

std::vector<wchar_t> makeEnvironmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& overrides)
{
    std::vector<wchar_t> block;
    LPWCH current = GetEnvironmentStringsW();
    if (current)
    {
        for (const wchar_t* item = current; *item;)
        {
            const std::wstring variable = item;
            bool replaced = false;
            for (const auto& overrideValue : overrides)
            {
                if (envKeyMatches(variable, overrideValue.first))
                {
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
            {
                block.insert(block.end(), variable.begin(), variable.end());
                block.push_back(L'\0');
            }
            item += variable.size() + 1;
        }
        FreeEnvironmentStringsW(current);
    }

    for (const auto& overrideValue : overrides)
    {
        std::wstring variable = overrideValue.first + L"=" + overrideValue.second;
        block.insert(block.end(), variable.begin(), variable.end());
        block.push_back(L'\0');
        secureClear(variable);
    }
    block.push_back(L'\0');
    return block;
}

void appendAvailablePipeBytes(HANDLE pipe, std::string& output)
{
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
        {
            return;
        }

        std::array<char, 8192> buffer{};
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(std::min<size_t>(buffer.size(), available)), &read, nullptr) || read == 0)
        {
            return;
        }
        output.append(buffer.data(), buffer.data() + read);
        if (output.size() > 64 * 1024 * 1024)
        {
            return;
        }
    }
}

BwProcessResult runBw(const std::vector<std::wstring>& args,
                      std::vector<std::pair<std::wstring, std::wstring>> envOverrides = {},
                      DWORD timeoutMs = kBwTimeoutMs,
                      DWORD creationFlags = CREATE_NO_WINDOW)
{
    BwProcessResult result;
    const std::wstring bw = findBwExe();
    if (bw.empty())
    {
        result.output = L"bw.exe was not found on PATH.";
        return result;
    }

    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0))
    {
        result.output = L"Could not create bw.exe pipe.";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE nulInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nulInput == INVALID_HANDLE_VALUE ? nullptr : nulInput;

    PROCESS_INFORMATION process{};
    std::wstring commandLine = makeCommandLine(bw, args);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    std::vector<wchar_t> environment = makeEnvironmentBlock(envOverrides);
    for (auto& overrideValue : envOverrides)
    {
        secureClear(overrideValue.second);
    }
    envOverrides.clear();

    const BOOL created = CreateProcessW(
        bw.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        creationFlags | CREATE_UNICODE_ENVIRONMENT,
        environment.empty() ? nullptr : environment.data(),
        nullptr, &startup, &process);

    CloseHandle(writePipe);
    if (nulInput != INVALID_HANDLE_VALUE)
    {
        CloseHandle(nulInput);
    }
    secureClear(commandLine);
    if (!mutableCommand.empty())
    {
        SecureZeroMemory(mutableCommand.data(), mutableCommand.size() * sizeof(wchar_t));
        mutableCommand.clear();
    }
    if (!environment.empty())
    {
        SecureZeroMemory(environment.data(), environment.size() * sizeof(wchar_t));
        environment.clear();
    }

    if (!created)
    {
        CloseHandle(readPipe);
        result.output = L"Could not start bw.exe.";
        return result;
    }

    result.started = true;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    std::string bytes;
    for (;;)
    {
        appendAvailablePipeBytes(readPipe, bytes);
        const DWORD wait = WaitForSingleObject(process.hProcess, 10);
        if (wait == WAIT_OBJECT_0)
        {
            break;
        }
        if (GetTickCount64() >= deadline)
        {
            result.timedOut = true;
            TerminateProcess(process.hProcess, 1);
            break;
        }
    }
    appendAvailablePipeBytes(readPipe, bytes);
    GetExitCodeProcess(process.hProcess, &result.exitCode);

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);

    result.output = fromUtf8(bytes);
    secureClear(bytes);
    return result;
}

void launchBwInteractive(const std::vector<std::wstring>& args)
{
    const std::wstring bw = findBwExe();
    if (bw.empty())
    {
        ShellExecuteW(nullptr, L"open", kInstallUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring commandLine = makeCommandLine(bw, args);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    if (CreateProcessW(bw.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                       CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                       nullptr, nullptr, &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    secureClear(commandLine);
    if (!mutableCommand.empty())
    {
        SecureZeroMemory(mutableCommand.data(), mutableCommand.size() * sizeof(wchar_t));
    }
}

size_t skipWs(const std::wstring& json, size_t pos)
{
    while (pos < json.size() && iswspace(json[pos]))
    {
        ++pos;
    }
    return pos;
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
        wchar_t ch = json[i];
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
            break;
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
            if (i + 4 < json.size())
            {
                unsigned int value = 0;
                for (int j = 0; j < 4; ++j)
                {
                    wchar_t hex = json[i + 1 + j];
                    value <<= 4;
                    if (hex >= L'0' && hex <= L'9')
                    {
                        value |= static_cast<unsigned int>(hex - L'0');
                    }
                    else if (hex >= L'a' && hex <= L'f')
                    {
                        value |= static_cast<unsigned int>(hex - L'a' + 10);
                    }
                    else if (hex >= L'A' && hex <= L'F')
                    {
                        value |= static_cast<unsigned int>(hex - L'A' + 10);
                    }
                }
                out.push_back(static_cast<wchar_t>(value));
                i += 4;
            }
            break;
        default:
            out.push_back(json[i]);
            break;
        }
    }
    return std::nullopt;
}

size_t findMatching(const std::wstring& json, size_t start, wchar_t open, wchar_t close)
{
    if (start >= json.size() || json[start] != open)
    {
        return std::wstring::npos;
    }

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
        else if (ch == open)
        {
            ++depth;
        }
        else if (ch == close && --depth == 0)
        {
            return i;
        }
    }
    return std::wstring::npos;
}

std::optional<size_t> jsonPropertyPos(const std::wstring& object, const wchar_t* key)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = 0; i < object.size(); ++i)
    {
        const wchar_t ch = object[i];
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
            if (depth == 1)
            {
                size_t next = 0;
                auto parsed = parseJsonStringAt(object, i, &next);
                if (parsed && *parsed == key)
                {
                    size_t pos = skipWs(object, next);
                    if (pos < object.size() && object[pos] == L':')
                    {
                        return skipWs(object, pos + 1);
                    }
                }
                i = next > 0 ? next - 1 : i;
                continue;
            }
            inString = true;
        }
        else if (ch == L'{')
        {
            ++depth;
        }
        else if (ch == L'}')
        {
            --depth;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> jsonStringProperty(const std::wstring& object, const wchar_t* key)
{
    const auto pos = jsonPropertyPos(object, key);
    if (!pos || *pos >= object.size() || object[*pos] != L'"')
    {
        return std::nullopt;
    }
    return parseJsonStringAt(object, *pos);
}

std::wstring jsonObjectProperty(const std::wstring& object, const wchar_t* key)
{
    const auto pos = jsonPropertyPos(object, key);
    if (!pos || *pos >= object.size() || object[*pos] != L'{')
    {
        return {};
    }
    const size_t end = findMatching(object, *pos, L'{', L'}');
    return end == std::wstring::npos ? std::wstring{} : object.substr(*pos, end - *pos + 1);
}

std::wstring jsonArrayProperty(const std::wstring& object, const wchar_t* key)
{
    const auto pos = jsonPropertyPos(object, key);
    if (!pos || *pos >= object.size() || object[*pos] != L'[')
    {
        return {};
    }
    const size_t end = findMatching(object, *pos, L'[', L']');
    return end == std::wstring::npos ? std::wstring{} : object.substr(*pos, end - *pos + 1);
}

std::vector<std::wstring> jsonObjectsFromArray(const std::wstring& array)
{
    std::vector<std::wstring> objects;
    for (size_t i = 0; i < array.size(); ++i)
    {
        if (array[i] != L'{')
        {
            continue;
        }
        const size_t end = findMatching(array, i, L'{', L'}');
        if (end == std::wstring::npos)
        {
            break;
        }
        objects.push_back(array.substr(i, end - i + 1));
        i = end;
    }
    return objects;
}

std::wstring domainFromUri(std::wstring uri)
{
    uri = trimCopy(std::move(uri));
    const size_t scheme = uri.find(L"://");
    if (scheme != std::wstring::npos)
    {
        uri = uri.substr(scheme + 3);
    }
    const size_t at = uri.find(L'@');
    if (at != std::wstring::npos)
    {
        uri = uri.substr(at + 1);
    }
    const size_t slash = uri.find_first_of(L"/?#");
    if (slash != std::wstring::npos)
    {
        uri = uri.substr(0, slash);
    }
    const size_t colon = uri.find(L':');
    if (colon != std::wstring::npos)
    {
        uri = uri.substr(0, colon);
    }
    return lowerCopy(uri);
}

std::unordered_map<std::wstring, std::wstring> parseIdNameMap(const std::wstring& json)
{
    std::unordered_map<std::wstring, std::wstring> out;
    for (const auto& object : jsonObjectsFromArray(json))
    {
        auto id = jsonStringProperty(object, L"id");
        auto name = jsonStringProperty(object, L"name");
        if (id && name && !id->empty() && !name->empty())
        {
            out[*id] = *name;
        }
    }
    return out;
}

std::vector<BwItem> parseItems(std::wstring& json,
                               const std::unordered_map<std::wstring, std::wstring>& folders,
                               const std::unordered_map<std::wstring, std::wstring>& organizations,
                               bool includeUsername)
{
    std::vector<BwItem> items;
    std::vector<std::wstring> objects = jsonObjectsFromArray(json);
    for (auto& object : objects)
    {
        auto id = jsonStringProperty(object, L"id");
        auto name = jsonStringProperty(object, L"name");
        if (!id || !name || id->empty() || name->empty())
        {
            secureClear(object);
            continue;
        }

        BwItem item;
        item.id = *id;
        item.name = *name;

        if (auto folderId = jsonStringProperty(object, L"folderId"))
        {
            if (const auto it = folders.find(*folderId); it != folders.end())
            {
                item.folder = it->second;
            }
        }
        if (auto orgId = jsonStringProperty(object, L"organizationId"))
        {
            if (const auto it = organizations.find(*orgId); it != organizations.end())
            {
                item.vault = it->second;
            }
        }

        std::wstring login = jsonObjectProperty(object, L"login");
        if (!login.empty())
        {
            if (includeUsername)
            {
                if (auto username = jsonStringProperty(login, L"username"))
                {
                    item.username = *username;
                }
            }

            std::wstring uris = jsonArrayProperty(login, L"uris");
            for (const auto& uriObject : jsonObjectsFromArray(uris))
            {
                if (auto uri = jsonStringProperty(uriObject, L"uri"); uri && !uri->empty())
                {
                    item.uri = *uri;
                    item.domain = domainFromUri(*uri);
                    break;
                }
            }
            secureClear(uris);
            secureClear(login);
        }

        items.push_back(std::move(item));
        secureClear(object);
    }
    secureClear(json);
    return items;
}

std::wstring joinSubtitle(const BwItem& item, bool includeUsername)
{
    std::vector<std::wstring> parts;
    if (!item.domain.empty())
    {
        parts.push_back(item.domain);
    }
    if (includeUsername && !item.username.empty())
    {
        parts.push_back(item.username);
    }
    if (!item.folder.empty())
    {
        parts.push_back(item.folder);
    }
    if (!item.vault.empty())
    {
        parts.push_back(item.vault);
    }
    if (parts.empty())
    {
        return L"Bitwarden item";
    }

    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
        {
            out += L" | ";
        }
        out += parts[i];
    }
    return out;
}

Command makeBwItemCommand(const BwItem& item, bool includeUsername, int rank)
{
    Command command = makeCommand(CommandKind::BitwardenItem, item.name, joinSubtitle(item, includeUsername), item.uri, 6400 - rank);
    command.data = item.id;
    std::wstring search = item.name + L" " + item.domain + L" " + item.folder + L" " + item.vault;
    if (includeUsername)
    {
        search += L" " + item.username;
    }
    command.searchText = lowerCopy(search);
    command.key = L"bitwarden|" + lowerCopy(item.id);
    return command;
}

Command makeControlCommand(const std::wstring& title, const std::wstring& subtitle, const std::wstring& action)
{
    Command command = makeCommand(CommandKind::BitwardenControl, title, subtitle, action, 0);
    command.provider = kProviderId;
    return command;
}

std::optional<std::wstring> promptSecret(HWND owner, const wchar_t* title, const wchar_t* label);

class BitwardenProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = kProviderId;
        info.title = L"Bitwarden";
        info.prefixes = { L"pw" };
        info.mode = QueryMode::Bitwarden;
        info.exclusive = true;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        expireSessionIfNeeded(ctx.settings);
        forgetUsernamesIfDisabled(ctx.settings);

        const std::wstring subject = q.subject();
        const auto terms = q.subjectTerms();

        if (findBwExe().empty())
        {
            sink.add(makeControlCommand(L"Install Bitwarden CLI",
                                        L"bw.exe is required for the Bitwarden provider", L"install"), 18000);
            return;
        }

        std::vector<BwItem> items;
        std::wstring status;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            items = items_;
            status = status_;
        }

        if (items.empty())
        {
            sink.add(makeControlCommand(L"Unlock and sync Bitwarden",
                                        L"Uses bw.exe directly and caches metadata only", L"sync"), 18000);
            sink.add(makeControlCommand(L"Log in to Bitwarden CLI",
                                        L"Opens bw.exe login in its own console", L"login"), 17900);
            if (!status.empty())
            {
                sink.add(makeControlCommand(L"Bitwarden status", status, L"noop"), 1000);
            }
            return;
        }

        int rank = 0;
        for (const auto& item : items)
        {
            Command command = makeBwItemCommand(item, ctx.settings.bitwardenSearchUsernames, rank);
            const int base = terms.empty() ? command.weight : scoreCommandTerms(terms, command);
            if (base >= 0)
            {
                sink.add(std::move(command), base + 15500 - rank);
            }
            ++rank;
        }

        if (subject.empty())
        {
            sink.add(makeControlCommand(L"Sync Bitwarden metadata",
                                        status.empty() ? L"Refresh cached item names, domains, folders, and usernames" : status,
                                        L"sync"), 12000);
        }
    }

    bool execute(const ProviderContext& ctx, const Command& command) override
    {
        expireSessionIfNeeded(ctx.settings);

        if (command.kind == CommandKind::BitwardenControl)
        {
            return executeControl(ctx, command.data.empty() ? command.arg : command.data);
        }
        if (command.kind != CommandKind::BitwardenItem)
        {
            return false;
        }

        switch (command.action)
        {
        case ActionKind::None:
        case ActionKind::Open:
        case ActionKind::BitwardenOpenSite:
            return openSite(command);
        case ActionKind::BitwardenOpenItem:
            return openItem(command);
        case ActionKind::BitwardenCopyUsername:
            return copySecret(ctx, command, L"username");
        case ActionKind::BitwardenCopyPassword:
            return copySecret(ctx, command, L"password");
        case ActionKind::BitwardenCopyTotp:
            return copySecret(ctx, command, L"totp");
        default:
            return false;
        }
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Bitwarden"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open Bitwarden search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for Bitwarden search", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Install Bitwarden CLI", L"Open Bitwarden CLI install docs", info().id, L"install"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Log in to CLI", L"Run bw.exe login in its own console", info().id, L"login"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Sync metadata", L"Cache item name, domain, folder, vault, and username", info().id, L"sync"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Lock session", L"Clear QuickPal's Bitwarden session and run bw lock", info().id, L"lock"));
        out.push_back(makeSettingItem(SettingField::BitwardenSearchUsernames, SettingKind::Toggle,
                                      L"Search usernames", L"Include usernames in metadata search results"));
        out.push_back(makeSettingItem(SettingField::BitwardenUnlockWithPin, SettingKind::Toggle,
                                      L"Unlock with PIN", L"Use a local QuickPal PIN for the active session window"));
        out.push_back(makeSettingItem(SettingField::BitwardenRequireMasterOnRestart, SettingKind::Toggle,
                                      L"Master password on restart", L"Do not persist the Bitwarden session between app launches"));
        out.push_back(makeSettingItem(SettingField::BitwardenSecretTimeoutSeconds, SettingKind::Stepper,
                                      L"Secret timeout", L"Clear the in-memory session after this many minutes"));
        out.push_back(makeSettingItem(SettingField::BitwardenClipboardClearSeconds, SettingKind::Stepper,
                                      L"Clipboard clear", L"Clear secret clipboard data if unchanged"));
        out.push_back(makeSettingItem(SettingField::BitwardenLockOnSleep, SettingKind::Toggle,
                                      L"Lock on sleep", L"Run bw lock when Windows suspends"));
        out.push_back(makeSettingItem(SettingField::BitwardenLockOnExit, SettingKind::Toggle,
                                      L"Lock on exit", L"Run bw lock when QuickPal exits"));
        out.push_back(makeSettingItem(SettingField::BitwardenUseServe, SettingKind::Toggle,
                                      L"Use bw serve", L"Advanced only; direct bw.exe remains the default path"));
    }

    bool applySetting(const ProviderContext& ctx, const SettingItem& item) override
    {
        return executeControl(ctx, item.settingKey);
    }

private:
    void clearSessionStateLocked()
    {
        secureClear(session_);
        pinSalt_.clear();
        pinHash_.clear();
        pinAttempts_ = 0;
        nextPinAllowedMs_ = 0;
    }

    bool loadPersistedSession(const Settings& settings)
    {
        if (settings.bitwardenRequireMasterOnRestart)
        {
            deleteFileQuietly(sessionCachePath());
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!session_.empty() && currentTimeMs() < sessionExpiresAtMs_)
            {
                return true;
            }
        }

        std::string protectedBytes = readFileBytes(sessionCachePath());
        if (protectedBytes.empty())
        {
            return false;
        }
        std::wstring text = unprotectForCurrentUser(protectedBytes);
        secureClear(protectedBytes);
        if (text.empty())
        {
            deleteFileQuietly(sessionCachePath());
            return false;
        }

        std::wstringstream input(text);
        std::wstring expiryText;
        std::wstring vaultUrl;
        std::wstring saltHex;
        std::wstring hashHex;
        std::wstring session;
        std::getline(input, expiryText);
        std::getline(input, vaultUrl);
        std::getline(input, saltHex);
        std::getline(input, hashHex);
        std::getline(input, session);
        ULONGLONG expiry = _wcstoui64(expiryText.c_str(), nullptr, 10);
        std::vector<unsigned char> salt = bytesFromHex(saltHex);
        std::vector<unsigned char> hash = bytesFromHex(hashHex);
        secureClear(text);

        if (session.empty() || expiry <= currentTimeMs() ||
            (settings.bitwardenUnlockWithPin && (salt.empty() || hash.empty())))
        {
            secureClear(session);
            deleteFileQuietly(sessionCachePath());
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            clearSessionStateLocked();
            session_ = std::move(session);
            sessionExpiresAtMs_ = expiry;
            vaultUrl_ = vaultUrl.empty() ? kDefaultVaultUrl : vaultUrl;
            pinSalt_ = std::move(salt);
            pinHash_ = std::move(hash);
            status_ = L"Bitwarden session restored";
        }
        return true;
    }

    void savePersistedSession(const Settings& settings)
    {
        if (settings.bitwardenRequireMasterOnRestart)
        {
            deleteFileQuietly(sessionCachePath());
            return;
        }

        std::wstring session;
        std::wstring vaultUrl;
        ULONGLONG expiry = 0;
        std::vector<unsigned char> salt;
        std::vector<unsigned char> hash;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_.empty() || sessionExpiresAtMs_ <= currentTimeMs())
            {
                deleteFileQuietly(sessionCachePath());
                return;
            }
            if (settings.bitwardenUnlockWithPin && (pinSalt_.empty() || pinHash_.empty()))
            {
                deleteFileQuietly(sessionCachePath());
                return;
            }
            session = session_;
            vaultUrl = vaultUrl_;
            expiry = sessionExpiresAtMs_;
            salt = pinSalt_;
            hash = pinHash_;
        }

        std::wstring text = std::to_wstring(expiry) + L"\n" + vaultUrl + L"\n" +
            hexBytes(salt) + L"\n" + hexBytes(hash) + L"\n" + session + L"\n";
        std::string protectedBytes = protectForCurrentUser(text);
        secureClear(text);
        secureClear(session);
        if (!protectedBytes.empty())
        {
            writeFileBytes(sessionCachePath(), protectedBytes);
            secureClear(protectedBytes);
        }
    }

    bool executeControl(const ProviderContext& ctx, const std::wstring& action)
    {
        if (action == L"install")
        {
            ShellExecuteW(nullptr, L"open", kInstallUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        }
        if (action == L"login")
        {
            launchBwInteractive({ L"login" });
            return true;
        }
        if (action == L"sync")
        {
            return refreshMetadata(ctx, true);
        }
        if (action == L"lock")
        {
            lockSession(ctx.settings, true);
            return true;
        }
        return true;
    }

    void expireSessionIfNeeded(const Settings& settings)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_.empty() && currentTimeMs() >= sessionExpiresAtMs_)
        {
            clearSessionStateLocked();
            status_ = L"Bitwarden session timed out";
            deleteFileQuietly(sessionCachePath());
        }
        if (settings.bitwardenRequireMasterOnRestart)
        {
            deleteFileQuietly(sessionCachePath());
        }
    }

    bool ensureSession(const ProviderContext& ctx, bool secretAction)
    {
        loadPersistedSession(ctx.settings);

        bool hadActiveSession = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hadActiveSession = !session_.empty() && currentTimeMs() < sessionExpiresAtMs_;
        }

        if (!hadActiveSession)
        {
            if (!unlockWithMasterPassword(ctx))
            {
                return false;
            }
            if (ctx.settings.bitwardenUnlockWithPin && !pinReady())
            {
                setPin(ctx.window);
            }
            savePersistedSession(ctx.settings);
            return true;
        }

        if (secretAction && ctx.settings.bitwardenUnlockWithPin && pinReady())
        {
            return verifyPin(ctx.window);
        }
        return true;
    }

    bool unlockWithMasterPassword(const ProviderContext& ctx)
    {
        if (findBwExe().empty())
        {
            MessageBoxW(ctx.window, L"bw.exe was not found. Install Bitwarden CLI first.", L"QuickPal Bitwarden", MB_OK | MB_ICONINFORMATION);
            ShellExecuteW(nullptr, L"open", kInstallUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return false;
        }

        auto password = promptSecret(ctx.window, L"Unlock Bitwarden", L"Bitwarden master password");
        if (!password || password->empty())
        {
            return false;
        }

        BwProcessResult result = runBw({ L"unlock", L"--raw", L"--passwordenv", L"QUICKPAL_BW_PASSWORD", L"--nointeraction" },
                                       { { L"QUICKPAL_BW_PASSWORD", *password } }, kBwTimeoutMs);
        secureClear(*password);
        if (!result.started || result.timedOut || result.exitCode != 0)
        {
            std::wstring message = result.timedOut ? L"bw unlock timed out." : trimCliOutput(result.output);
            secureClear(result.output);
            if (message.empty())
            {
                message = L"bw unlock failed.";
            }
            MessageBoxW(ctx.window, message.c_str(), L"QuickPal Bitwarden", MB_OK | MB_ICONWARNING);
            return false;
        }

        std::wstring session = trimCliOutput(result.output);
        secureClear(result.output);
        if (session.empty())
        {
            MessageBoxW(ctx.window, L"bw unlock did not return a session key.", L"QuickPal Bitwarden", MB_OK | MB_ICONWARNING);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            secureClear(session_);
            session_ = std::move(session);
            sessionExpiresAtMs_ = currentTimeMs() + static_cast<ULONGLONG>(ctx.settings.bitwardenSecretTimeoutSeconds) * 1000ULL;
            status_ = L"Bitwarden unlocked";
        }
        savePersistedSession(ctx.settings);
        return true;
    }

    bool pinReady() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !pinSalt_.empty() && !pinHash_.empty();
    }

    bool setPin(HWND owner)
    {
        auto pin = promptSecret(owner, L"Set QuickPal PIN", L"Local PIN for this Bitwarden session");
        if (!pin || pin->empty())
        {
            return false;
        }
        auto confirm = promptSecret(owner, L"Confirm QuickPal PIN", L"Repeat local PIN");
        if (!confirm || *confirm != *pin)
        {
            if (confirm)
            {
                secureClear(*confirm);
            }
            secureClear(*pin);
            MessageBoxW(owner, L"PIN entries did not match. This Bitwarden session will require the master password again after timeout/restart.",
                        L"QuickPal Bitwarden", MB_OK | MB_ICONWARNING);
            return false;
        }

        std::vector<unsigned char> salt(16);
        if (BCryptGenRandom(nullptr, salt.data(), static_cast<ULONG>(salt.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        {
            secureClear(*confirm);
            secureClear(*pin);
            return false;
        }
        std::vector<unsigned char> hash = hashPin(*pin, salt);
        secureClear(*confirm);
        secureClear(*pin);
        if (hash.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pinSalt_ = std::move(salt);
        pinHash_ = std::move(hash);
        pinAttempts_ = 0;
        nextPinAllowedMs_ = 0;
        return true;
    }

    bool verifyPin(HWND owner)
    {
        const ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (now < nextPinAllowedMs_)
            {
                MessageBoxW(owner, L"Too many incorrect PIN attempts. Try again in a moment.",
                            L"QuickPal Bitwarden", MB_OK | MB_ICONWARNING);
                return false;
            }
        }

        auto pin = promptSecret(owner, L"QuickPal PIN", L"Enter local Bitwarden PIN");
        if (!pin || pin->empty())
        {
            return false;
        }

        std::vector<unsigned char> salt;
        std::vector<unsigned char> expected;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            salt = pinSalt_;
            expected = pinHash_;
        }
        const std::vector<unsigned char> actual = hashPin(*pin, salt);
        secureClear(*pin);

        bool matches = actual.size() == expected.size();
        unsigned char diff = 0;
        for (size_t i = 0; i < std::min(actual.size(), expected.size()); ++i)
        {
            diff |= actual[i] ^ expected[i];
        }
        matches = matches && diff == 0;

        std::lock_guard<std::mutex> lock(mutex_);
        if (matches)
        {
            pinAttempts_ = 0;
            nextPinAllowedMs_ = 0;
            return true;
        }

        ++pinAttempts_;
        const ULONGLONG backoffSeconds = std::min<ULONGLONG>(30, 1ULL << std::min(pinAttempts_, 5));
        nextPinAllowedMs_ = GetTickCount64() + backoffSeconds * 1000ULL;
        MessageBoxW(owner, L"Incorrect PIN.", L"QuickPal Bitwarden", MB_OK | MB_ICONWARNING);
        return false;
    }

    std::vector<unsigned char> hashPin(const std::wstring& pin, const std::vector<unsigned char>& salt) const
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        {
            return {};
        }

        DWORD objectBytes = 0;
        DWORD hashBytes = 0;
        DWORD returned = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                              sizeof(objectBytes), &returned, 0) < 0 ||
            BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes),
                              sizeof(hashBytes), &returned, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(alg, 0);
            return {};
        }

        std::vector<unsigned char> object(objectBytes);
        std::vector<unsigned char> hash(hashBytes);
        BCRYPT_HASH_HANDLE handle = nullptr;
        bool ok = BCryptCreateHash(alg, &handle, object.data(), objectBytes, nullptr, 0, 0) >= 0;
        ok = ok && BCryptHashData(handle, const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), 0) >= 0;
        std::string bytes = toUtf8(pin);
        ok = ok && BCryptHashData(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                  static_cast<ULONG>(bytes.size()), 0) >= 0;
        ok = ok && BCryptFinishHash(handle, hash.data(), hashBytes, 0) >= 0;
        secureClear(bytes);
        if (handle)
        {
            BCryptDestroyHash(handle);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return ok ? hash : std::vector<unsigned char>{};
    }

    bool refreshMetadata(const ProviderContext& ctx, bool runSync)
    {
        if (!ensureSession(ctx, true))
        {
            return false;
        }

        std::wstring session = sessionSnapshot();
        if (session.empty())
        {
            return false;
        }

        if (runSync)
        {
            BwProcessResult sync = runBw({ L"sync", L"--quiet", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwTimeoutMs);
            secureClear(sync.output);
        }

        BwProcessResult status = runBw({ L"status", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwTimeoutMs);
        BwProcessResult folders = runBw({ L"list", L"folders", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwListTimeoutMs);
        BwProcessResult organizations = runBw({ L"list", L"organizations", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwListTimeoutMs);
        BwProcessResult items = runBw({ L"list", L"items", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwListTimeoutMs);
        secureClear(session);

        if (!items.started || items.timedOut || items.exitCode != 0)
        {
            const std::wstring message = items.timedOut ? L"bw list items timed out." : trimCliOutput(items.output);
            secureClear(items.output);
            secureClear(folders.output);
            secureClear(organizations.output);
            secureClear(status.output);
            MessageBoxW(ctx.window, message.empty() ? L"Could not read Bitwarden items." : message.c_str(),
                        L"QuickPal Bitwarden", MB_OK | MB_ICONWARNING);
            return false;
        }

        std::wstring vaultUrl = vaultUrl_;
        if (status.exitCode == 0)
        {
            if (auto serverUrl = jsonStringProperty(status.output, L"serverUrl"); serverUrl && !serverUrl->empty())
            {
                vaultUrl = *serverUrl;
            }
        }
        const auto folderMap = folders.exitCode == 0 ? parseIdNameMap(folders.output) : std::unordered_map<std::wstring, std::wstring>{};
        const auto orgMap = organizations.exitCode == 0 ? parseIdNameMap(organizations.output) : std::unordered_map<std::wstring, std::wstring>{};
        std::vector<BwItem> parsed = parseItems(items.output, folderMap, orgMap, ctx.settings.bitwardenSearchUsernames);
        secureClear(folders.output);
        secureClear(organizations.output);
        secureClear(status.output);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            items_ = std::move(parsed);
            vaultUrl_ = vaultUrl.empty() ? kDefaultVaultUrl : vaultUrl;
            usernamesPresent_ = ctx.settings.bitwardenSearchUsernames;
            status_ = std::to_wstring(items_.size()) + L" Bitwarden items cached";
        }
        savePersistedSession(ctx.settings);
        return true;
    }

    std::wstring sessionSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_;
    }

    std::optional<BwItem> itemById(const std::wstring& id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : items_)
        {
            if (item.id == id)
            {
                return item;
            }
        }
        return std::nullopt;
    }

    void forgetUsernamesIfDisabled(const Settings& settings)
    {
        if (settings.bitwardenSearchUsernames)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!usernamesPresent_)
        {
            return;
        }
        for (auto& item : items_)
        {
            secureClear(item.username);
        }
        usernamesPresent_ = false;
    }

    bool copySecret(const ProviderContext& ctx, const Command& command, const wchar_t* field)
    {
        if (command.data.empty() || !ensureSession(ctx, true))
        {
            return false;
        }

        std::wstring session = sessionSnapshot();
        BwProcessResult result = runBw({ L"get", field, command.data, L"--raw", L"--nointeraction" },
                                       { { L"BW_SESSION", session } }, kBwTimeoutMs);
        secureClear(session);

        if (!result.started || result.timedOut || result.exitCode != 0)
        {
            std::wstring message = result.timedOut ? L"bw get timed out." : trimCliOutput(result.output);
            secureClear(result.output);
            if (message.empty())
            {
                message = L"Could not read that Bitwarden field.";
            }
            MessageBoxW(ctx.window, message.c_str(), L"QuickPal Bitwarden", MB_OK | MB_ICONWARNING);
            return false;
        }

        std::wstring secret = trimCliOutput(result.output);
        secureClear(result.output);
        if (secret.empty())
        {
            MessageBoxW(ctx.window, L"Bitwarden returned an empty value.", L"QuickPal Bitwarden", MB_OK | MB_ICONINFORMATION);
            return false;
        }

        const bool ok = copySensitiveTextToClipboard(ctx.window, secret, ctx.settings.bitwardenClipboardClearSeconds);
        secureClear(secret);
        return ok;
    }

    bool openSite(const Command& command)
    {
        if (command.arg.empty())
        {
            if (auto item = itemById(command.data); item && !item->uri.empty())
            {
                ShellExecuteW(nullptr, L"open", item->uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return true;
            }
            return openItem(command);
        }
        ShellExecuteW(nullptr, L"open", command.arg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    bool openItem(const Command& command)
    {
        if (command.data.empty())
        {
            return false;
        }
        std::wstring base;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            base = vaultUrl_.empty() ? kDefaultVaultUrl : vaultUrl_;
        }
        while (!base.empty() && base.back() == L'/')
        {
            base.pop_back();
        }
        const std::wstring url = base + L"/#/vault?itemId=" + command.data;
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    void lockSession(const Settings& settings, bool runBwLock)
    {
        std::wstring session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session = session_;
            clearSessionStateLocked();
            status_ = L"Bitwarden session locked";
        }
        deleteFileQuietly(sessionCachePath());

        if (runBwLock && !session.empty())
        {
            BwProcessResult result = runBw({ L"lock", L"--quiet", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwTimeoutMs);
            secureClear(result.output);
        }
        secureClear(session);
    }

    mutable std::mutex mutex_;
    std::vector<BwItem> items_;
    bool usernamesPresent_ = false;
    std::wstring session_;
    ULONGLONG sessionExpiresAtMs_ = 0;
    std::wstring vaultUrl_ = kDefaultVaultUrl;
    std::wstring status_;
    std::vector<unsigned char> pinSalt_;
    std::vector<unsigned char> pinHash_;
    int pinAttempts_ = 0;
    ULONGLONG nextPinAllowedMs_ = 0;
};

struct PromptState
{
    const wchar_t* label = L"";
    HWND edit = nullptr;
    bool done = false;
    bool ok = false;
    std::wstring value;
};

LRESULT CALLBACK promptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PromptState* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        return TRUE;
    case WM_CREATE:
    {
        state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND label = CreateWindowW(L"STATIC", state ? state->label : L"", WS_CHILD | WS_VISIBLE,
                                   16, 14, 320, 20, hwnd, nullptr, nullptr, nullptr);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL,
                                    16, 42, 320, 24, hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
        HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                176, 78, 76, 26, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                                    260, 78, 76, 26, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (state)
        {
            state->edit = edit;
        }
        SetFocus(edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && state)
        {
            const int length = GetWindowTextLengthW(state->edit);
            state->value.assign(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, state->value.data(), length + 1);
            state->value.resize(static_cast<size_t>(length));
            state->ok = true;
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL && state)
        {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_RETURN && state)
        {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE && state)
        {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state)
        {
            state->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::optional<std::wstring> promptSecret(HWND owner, const wchar_t* title, const wchar_t* label)
{
    static ATOM atom = 0;
    if (!atom)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = promptProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"QuickPal.BitwardenPrompt";
        atom = RegisterClassExW(&wc);
    }

    PromptState state;
    state.label = label;

    RECT ownerRect{};
    if (!owner || !GetWindowRect(owner, &ownerRect))
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    }
    const int width = 368;
    const int height = 150;
    const int x = static_cast<int>(ownerRect.left) +
        std::max<int>(0, (static_cast<int>(ownerRect.right - ownerRect.left) - width) / 2);
    const int y = static_cast<int>(ownerRect.top) +
        std::max<int>(0, (static_cast<int>(ownerRect.bottom - ownerRect.top) - height) / 2);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
                                L"QuickPal.BitwardenPrompt", title,
                                WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!hwnd)
    {
        return std::nullopt;
    }

    if (owner)
    {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner)
    {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    if (!state.ok)
    {
        secureClear(state.value);
        return std::nullopt;
    }
    return state.value;
}
}

std::unique_ptr<Provider> makeBitwardenProvider()
{
    return std::make_unique<BitwardenProvider>();
}
