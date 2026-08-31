#include "providers.h"

#include "../settings.h"
#include "../util.h"
#include "../../ui/theme.h"

#include <bcrypt.h>
#include <asyncinfo.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <winstring.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.security.credentials.ui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
constexpr DWORD kBwTimeoutMs = 30000;
constexpr DWORD kBwUnlockTimeoutMs = 120000;
constexpr DWORD kBwListTimeoutMs = 90000;
constexpr DWORD kBwServeStartTimeoutMs = 12000;
constexpr DWORD kBwServeRequestTimeoutMs = 5000;
constexpr wchar_t kProviderId[] = L"bitwarden";
constexpr wchar_t kInstallUrl[] = L"https://bitwarden.com/help/cli/";
constexpr wchar_t kDefaultVaultUrl[] = L"https://vault.bitwarden.com";
constexpr GUID kUserConsentVerifierStaticsId =
    { 0xaf4f3f91, 0x564c, 0x4ddc, { 0xb8, 0xb5, 0x97, 0x34, 0x47, 0x62, 0x7c, 0x65 } };
constexpr GUID kAsyncInfoId =
    { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

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

struct BwHttpResult
{
    bool sent = false;
    DWORD statusCode = 0;
    std::wstring output;

    bool ok() const
    {
        return sent && statusCode >= 200 && statusCode < 300;
    }
};

struct BwServeProcess
{
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD processId = 0;
};

struct BwServeWatchState
{
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    ULONGLONG deadlineMs = 0;
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
    std::wstring password;
    bool hasTotp = false;
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

void secureClear(std::vector<unsigned char>& value)
{
    if (!value.empty())
    {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

std::wstring friendlyBitwardenError(const std::wstring& raw, const wchar_t* fallback)
{
    const std::wstring lower = lowerCopy(raw);
    if (lower.find(L"decryption operation failed") != std::wstring::npos ||
        lower.find(L"cryptography error") != std::wstring::npos)
    {
        return L"That master password could not unlock this vault. Check it and try again. If the password is correct, reconnect Bitwarden from Settings.";
    }
    if (lower.find(L"not logged in") != std::wstring::npos || lower.find(L"unauthenticated") != std::wstring::npos ||
        lower.find(L"invalid_grant") != std::wstring::npos)
    {
        return L"The Bitwarden CLI sign-in has expired. Sign in again from the Bitwarden Settings section.";
    }
    return fallback;
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

std::wstring pinCachePath()
{
    return settingsDirectory() + L"\\bitwarden_pin.bin";
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

BwHttpResult requestLocalBw(USHORT port, const std::wstring& authToken,
                            const wchar_t* method, const std::wstring& path,
                            DWORD timeoutMs = kBwServeRequestTimeoutMs)
{
    BwHttpResult result;
    if (authToken.empty())
    {
        return result;
    }
    HINTERNET session = WinHttpOpen(L"QuickPal/Bitwarden", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        return result;
    }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
    if (!connection)
    {
        WinHttpCloseHandle(session);
        return result;
    }

    HINTERNET request = WinHttpOpenRequest(connection, method, path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request)
    {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    std::wstring authHeader = L"Authorization: Bearer " + authToken;
    const BOOL addedHeader = WinHttpAddRequestHeaders(
        request, authHeader.c_str(), static_cast<DWORD>(authHeader.size()),
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    secureClear(authHeader);
    if (!addedHeader)
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    const BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                      WinHttpReceiveResponse(request, nullptr);
    result.sent = sent == TRUE;
    if (sent)
    {
        DWORD size = sizeof(result.statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &result.statusCode, &size,
                            WINHTTP_NO_HEADER_INDEX);

        std::string bytes;
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            {
                break;
            }
            if (bytes.size() + available > 64 * 1024 * 1024)
            {
                result.sent = false;
                break;
            }
            const size_t offset = bytes.size();
            bytes.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, bytes.data() + offset, available, &read))
            {
                result.sent = false;
                bytes.resize(offset);
                break;
            }
            bytes.resize(offset + read);
        }
        result.output = fromUtf8(bytes);
        secureClear(bytes);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

std::optional<std::wstring> randomBwServeAuthToken()
{
    std::array<unsigned char, 32> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    {
        SecureZeroMemory(bytes.data(), bytes.size());
        return std::nullopt;
    }

    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring token;
    token.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes)
    {
        token.push_back(digits[value >> 4]);
        token.push_back(digits[value & 0x0f]);
    }
    SecureZeroMemory(bytes.data(), bytes.size());
    return token;
}

BwServeProcess startBwServeProcess(USHORT port, const std::wstring& sessionToken,
                                   const std::wstring& authToken)
{
    BwServeProcess result;
    const std::wstring bw = findBwExe();
    if (bw.empty() || sessionToken.empty() || authToken.empty())
    {
        return result;
    }

    const std::vector<std::wstring> args = {
        L"serve", L"--hostname", L"127.0.0.1", L"--port", std::to_wstring(port),
        L"--auth-token-env", L"QUICKPAL_BW_SERVE_TOKEN"
    };
    std::wstring commandLine = makeCommandLine(bw, args);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    std::vector<std::pair<std::wstring, std::wstring>> overrides = {
        { L"BW_SESSION", sessionToken },
        { L"QUICKPAL_BW_SERVE_TOKEN", authToken },
        { L"BW_NOINTERACTION", L"true" }
    };
    std::vector<wchar_t> environment = makeEnvironmentBlock(overrides);
    for (auto& entry : overrides)
    {
        secureClear(entry.second);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        bw.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        environment.empty() ? nullptr : environment.data(),
        nullptr, &startup, &process);

    secureClear(commandLine);
    if (!mutableCommand.empty())
    {
        SecureZeroMemory(mutableCommand.data(), mutableCommand.size() * sizeof(wchar_t));
    }
    if (!environment.empty())
    {
        SecureZeroMemory(environment.data(), environment.size() * sizeof(wchar_t));
    }

    if (!created)
    {
        return result;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                          &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, process.hProcess))
    {
        TerminateProcess(process.hProcess, 0);
        if (job)
        {
            CloseHandle(job);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return result;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    result.process = process.hProcess;
    result.job = job;
    result.processId = process.dwProcessId;
    return result;
}

DWORD WINAPI watchBwServeDeadline(void* parameter)
{
    std::unique_ptr<BwServeWatchState> state(static_cast<BwServeWatchState*>(parameter));
    if (!state || !state->process)
    {
        return 0;
    }

    for (;;)
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= state->deadlineMs)
        {
            if (WaitForSingleObject(state->process, 0) == WAIT_TIMEOUT)
            {
                if (state->job)
                {
                    TerminateJobObject(state->job, 0);
                }
                else
                {
                    TerminateProcess(state->process, 0);
                }
            }
            break;
        }
        const ULONGLONG remaining = state->deadlineMs - now;
        const DWORD waitMs = static_cast<DWORD>(std::min<ULONGLONG>(remaining, 60000));
        const DWORD wait = WaitForSingleObject(state->process, waitMs);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
        {
            break;
        }
    }
    CloseHandle(state->process);
    state->process = nullptr;
    if (state->job)
    {
        CloseHandle(state->job);
        state->job = nullptr;
    }
    return 0;
}

bool armBwServeDeadline(HANDLE process, HANDLE job, ULONGLONG deadlineMs)
{
    HANDLE watchedProcess = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &watchedProcess,
                         SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0))
    {
        return false;
    }

    HANDLE watchedJob = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), job, GetCurrentProcess(), &watchedJob,
                         JOB_OBJECT_TERMINATE, FALSE, 0))
    {
        CloseHandle(watchedProcess);
        return false;
    }

    auto state = std::make_unique<BwServeWatchState>();
    state->process = watchedProcess;
    state->job = watchedJob;
    state->deadlineMs = deadlineMs;
    HANDLE thread = CreateThread(nullptr, 0, watchBwServeDeadline, state.get(), 0, nullptr);
    if (!thread)
    {
        CloseHandle(watchedProcess);
        CloseHandle(watchedJob);
        return false;
    }
    state.release();
    CloseHandle(thread);
    return true;
}

USHORT randomBwServePort()
{
    unsigned int value = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    {
        value = static_cast<unsigned int>(GetTickCount64());
    }
    return static_cast<USHORT>(49152 + (value % 15000));
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

std::wstring bwResponseObject(const std::wstring& response)
{
    return jsonObjectProperty(response, L"data");
}

std::wstring bwResponseArray(const std::wstring& response)
{
    std::wstring direct = jsonArrayProperty(response, L"data");
    if (!direct.empty())
    {
        return direct;
    }
    std::wstring envelope = bwResponseObject(response);
    if (envelope.empty())
    {
        return {};
    }
    std::wstring nested = jsonArrayProperty(envelope, L"data");
    secureClear(envelope);
    return nested;
}

std::optional<std::wstring> bwResponseString(const std::wstring& response)
{
    return jsonStringProperty(response, L"data");
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
            if (auto password = jsonStringProperty(login, L"password"))
            {
                item.password = std::move(*password);
            }
            if (auto totp = jsonStringProperty(login, L"totp"); totp && !totp->empty())
            {
                item.hasTotp = true;
                secureClear(*totp);
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
    command.hasTotp = item.hasTotp;
    return command;
}

Command makeControlCommand(const std::wstring& title, const std::wstring& subtitle, const std::wstring& action)
{
    Command command = makeCommand(CommandKind::BitwardenControl, title, subtitle, action, 0);
    command.provider = kProviderId;
    return command;
}

std::optional<std::wstring> promptValue(HWND owner, const wchar_t* title, const wchar_t* label,
                                        bool secret, const std::wstring& initial = {});
std::optional<std::wstring> promptSecret(HWND owner, const wchar_t* title, const wchar_t* label);
void showBitwardenMessage(HWND owner, const wchar_t* title, const std::wstring& message, bool warning = true);

class BitwardenProvider : public Provider
{
public:
    BitwardenProvider()
    {
        loadPersistedPin();
    }

    ~BitwardenProvider() override
    {
        {
            std::lock_guard<std::mutex> lock(refreshMutex_);
            refreshStop_ = true;
            refreshCancel_.store(true);
        }
        refreshCv_.notify_all();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopServeLocked();
        }
        if (refreshWorker_.joinable())
        {
            refreshWorker_.join();
        }
    }

    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = kProviderId;
        info.title = L"Bitwarden";
        info.prefixes = { L"pw" };
        info.mode = QueryMode::Bitwarden;
        info.exclusive = true;
        info.settingsSummary = L"Windows Hello, PIN access, secret actions, clipboard, and locking";
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
            sink.add(makeControlCommand(L"Connect Bitwarden",
                                        L"Sign in or unlock, then read the local vault cache", L"connect"), 18000);
            if (!status.empty())
            {
                sink.add(makeControlCommand(L"Bitwarden status", status, L"noop"), 1000);
            }
            return;
        }

        int rank = 0;
        int matchCount = 0;
        for (const auto& item : items)
        {
            Command command = makeBwItemCommand(item, ctx.settings.bitwardenSearchUsernames, rank);
            const int base = terms.empty() ? command.weight : scoreCommandTerms(terms, command);
            if (base >= 0)
            {
                sink.add(std::move(command), base + 15500 - rank);
                ++matchCount;
            }
            ++rank;
        }

        if (subject.empty())
        {
            sink.add(makeControlCommand(L"Sync Bitwarden metadata",
                                        status.empty() ? L"Refresh cached item names, domains, folders, and usernames" : status,
                                        L"sync"), 12000);
        }
        else if (matchCount == 0)
        {
            sink.add(makeControlCommand(L"No matches - sync Bitwarden",
                                        L"Refresh from Bitwarden, then search this term again", L"sync"), 12000);
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
            return copySecret(ctx, command, L"password");
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
        out.push_back(makeSettingHeader(L"Search"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open Bitwarden search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for Bitwarden search", info().id));
        out.push_back(makeSettingItem(SettingField::BitwardenSearchUsernames, SettingKind::Toggle,
                                      L"Search usernames", L"Include usernames in metadata search results"));

        out.push_back(makeSettingHeader(L"Access"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Account email", L"Saved locally; the master password is never saved", info().id, L"account-email"));
        out.push_back(makeSettingItem(SettingField::BitwardenUnlockWithPin, SettingKind::Toggle,
                                      L"Unlock secrets with PIN", L"Use the persistent QuickPal PIN configured below"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Set or change PIN", L"PIN setup only happens here in Settings", info().id, L"set-pin"));
        out.push_back(makeSettingItem(SettingField::BitwardenUnlockWithHello, SettingKind::Toggle,
                                      L"Unlock with Windows Hello", L"Prefer face, fingerprint, or Windows PIN; saved PIN is fallback"));
        out.push_back(makeSettingItem(SettingField::BitwardenPinTimeoutSeconds, SettingKind::Stepper,
                                      L"Secret authorization", L"Ask for PIN or Windows Hello again after this window"));
        out.push_back(makeSettingItem(SettingField::BitwardenRequireMasterOnRestart, SettingKind::Toggle,
                                      L"Master password after restart", L"Never persist the Bitwarden session between QuickPal launches"));
        out.push_back(makeSettingItem(SettingField::BitwardenUseServe, SettingKind::Toggle,
                                      L"Fast local API", L"Managed automatically and limited to the authorized window"));

        out.push_back(makeSettingHeader(L"Secrets"));
        out.push_back(makeSettingItem(SettingField::BitwardenClipboardClearSeconds, SettingKind::Stepper,
                                      L"Clipboard clear", L"Clear copied passwords and TOTP codes if unchanged"));
        out.push_back(makeSettingItem(SettingField::BitwardenLockOnSleep, SettingKind::Toggle,
                                      L"Lock on sleep", L"Run bw lock when Windows suspends"));
        out.push_back(makeSettingItem(SettingField::BitwardenLockOnExit, SettingKind::Toggle,
                                      L"Lock on exit", L"Run bw lock when QuickPal exits"));

        out.push_back(makeSettingHeader(L"Maintenance"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Install Bitwarden CLI", L"Open Bitwarden CLI install docs", info().id, L"install"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Connect Bitwarden", L"Sign in or unlock, then read the local vault cache", info().id, L"connect"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Sync metadata", L"Cache item name, domain, folder, vault, and username", info().id, L"sync"));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Lock session", L"Clear QuickPal's Bitwarden session and run bw lock", info().id, L"lock"));
    }

    bool applySetting(const ProviderContext& ctx, const SettingItem& item) override
    {
        return executeControl(ctx, item.settingKey);
    }

private:
    enum class HelloResult
    {
        Verified,
        Canceled,
        Unavailable,
        Failed,
    };

    bool loadPersistedPin()
    {
        std::string protectedBytes = readFileBytes(pinCachePath());
        if (protectedBytes.empty())
        {
            return false;
        }
        std::wstring text = unprotectForCurrentUser(protectedBytes);
        secureClear(protectedBytes);
        if (text.empty())
        {
            deleteFileQuietly(pinCachePath());
            return false;
        }

        std::wstringstream input(text);
        std::wstring version;
        std::wstring saltHex;
        std::wstring hashHex;
        std::getline(input, version);
        std::getline(input, saltHex);
        std::getline(input, hashHex);
        std::vector<unsigned char> salt = bytesFromHex(saltHex);
        std::vector<unsigned char> hash = bytesFromHex(hashHex);
        secureClear(text);
        if (version != L"1" || salt.size() != 16 || hash.size() != 32)
        {
            secureClear(salt);
            secureClear(hash);
            deleteFileQuietly(pinCachePath());
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        secureClear(pinSalt_);
        secureClear(pinHash_);
        pinSalt_ = std::move(salt);
        pinHash_ = std::move(hash);
        pinAttempts_ = 0;
        nextPinAllowedMs_ = 0;
        pinAuthorizedUntilMs_ = 0;
        return true;
    }

    void savePersistedPin()
    {
        std::vector<unsigned char> salt;
        std::vector<unsigned char> hash;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            salt = pinSalt_;
            hash = pinHash_;
        }
        if (salt.size() != 16 || hash.size() != 32)
        {
            secureClear(salt);
            secureClear(hash);
            return;
        }

        const std::wstring saltText = hexBytes(salt);
        const std::wstring hashText = hexBytes(hash);
        std::wstring text = L"1\n" + saltText + L"\n" + hashText + L"\n";
        secureClear(salt);
        secureClear(hash);
        std::string protectedBytes = protectForCurrentUser(text);
        secureClear(text);
        if (!protectedBytes.empty())
        {
            writeFileBytes(pinCachePath(), protectedBytes);
            secureClear(protectedBytes);
        }
    }

    void clearCachedSecretsLocked()
    {
        for (auto& item : items_)
        {
            secureClear(item.password);
        }
    }

    bool serveProcessRunningLocked()
    {
        if (!serveProcess_)
        {
            return false;
        }
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(serveProcess_, &exitCode) || exitCode != STILL_ACTIVE)
        {
            if (serveJob_)
            {
                CloseHandle(serveJob_);
                serveJob_ = nullptr;
            }
            CloseHandle(serveProcess_);
            serveProcess_ = nullptr;
            serveProcessId_ = 0;
            servePort_ = 0;
            serveDeadlineMs_ = 0;
            secureClear(serveAuthToken_);
            return false;
        }
        return true;
    }

    void stopServeLocked()
    {
        if (serveProcess_)
        {
            if (WaitForSingleObject(serveProcess_, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(serveProcess_, 0);
            }
            CloseHandle(serveProcess_);
        }
        if (serveJob_)
        {
            CloseHandle(serveJob_);
        }
        serveProcess_ = nullptr;
        serveJob_ = nullptr;
        serveProcessId_ = 0;
        servePort_ = 0;
        serveDeadlineMs_ = 0;
        secureClear(serveAuthToken_);
    }

    void clearSessionStateLocked()
    {
        stopServeLocked();
        secureClear(session_);
        sessionExpiresAtMs_ = 0;
        clearCachedSecretsLocked();
        pinAttempts_ = 0;
        nextPinAllowedMs_ = 0;
        pinAuthorizedUntilMs_ = 0;
        needsInitialSync_ = false;
    }

    bool ensureServe(const Settings& settings)
    {
        if (!settings.bitwardenUseServe)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopServeLocked();
            return false;
        }

        std::wstring session;
        ULONGLONG deadlineMs = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const ULONGLONG now = GetTickCount64();
            if (serveProcessRunningLocked() && !serveAuthToken_.empty() && now < serveDeadlineMs_)
            {
                return true;
            }
            stopServeLocked();
            if (session_.empty())
            {
                return false;
            }
            if (settings.bitwardenUnlockWithPin || settings.bitwardenUnlockWithHello)
            {
                if (pinAuthorizedUntilMs_ <= now)
                {
                    return false;
                }
                deadlineMs = pinAuthorizedUntilMs_;
            }
            else
            {
                deadlineMs = now + static_cast<ULONGLONG>(std::max(60, settings.bitwardenPinTimeoutSeconds)) * 1000ULL;
            }
            session = session_;
        }

        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const USHORT port = randomBwServePort();
            auto authToken = randomBwServeAuthToken();
            if (!authToken)
            {
                break;
            }
            BwServeProcess server = startBwServeProcess(port, session, *authToken);
            if (!server.process)
            {
                secureClear(*authToken);
                continue;
            }

            bool ready = false;
            const ULONGLONG startupDeadline = GetTickCount64() + kBwServeStartTimeoutMs;
            while (GetTickCount64() < startupDeadline)
            {
                if (WaitForSingleObject(server.process, 0) == WAIT_OBJECT_0)
                {
                    break;
                }
                BwHttpResult status = requestLocalBw(port, *authToken, L"GET", L"/status", 1000);
                secureClear(status.output);
                if (status.ok())
                {
                    ready = true;
                    break;
                }
                Sleep(75);
            }

            if (ready && armBwServeDeadline(server.process, server.job, deadlineMs))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopServeLocked();
                serveProcess_ = server.process;
                serveJob_ = server.job;
                serveProcessId_ = server.processId;
                servePort_ = port;
                serveDeadlineMs_ = deadlineMs;
                serveAuthToken_ = *authToken;
                secureClear(*authToken);
                secureClear(session);
                return true;
            }

            if (WaitForSingleObject(server.process, 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(server.process, 0);
            }
            if (server.job)
            {
                CloseHandle(server.job);
            }
            CloseHandle(server.process);
            secureClear(*authToken);
        }
        secureClear(session);
        return false;
    }

    std::optional<BwHttpResult> callServe(const Settings& settings, const wchar_t* method,
                                          const std::wstring& path,
                                          DWORD timeoutMs = kBwServeRequestTimeoutMs)
    {
        if (!ensureServe(settings))
        {
            return std::nullopt;
        }
        USHORT port = 0;
        std::wstring authToken;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!serveProcessRunningLocked())
            {
                return std::nullopt;
            }
            port = servePort_;
            authToken = serveAuthToken_;
        }

        BwHttpResult result = requestLocalBw(port, authToken, method, path, timeoutMs);
        secureClear(authToken);
        if (!result.sent)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (servePort_ == port)
            {
                stopServeLocked();
            }
        }
        return result;
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
        std::wstring version;
        std::wstring expiryText;
        std::wstring vaultUrl;
        std::wstring saltHex;
        std::wstring hashHex;
        std::wstring session;
        std::getline(input, version);
        if (version != L"2" && version != L"3")
        {
            secureClear(text);
            deleteFileQuietly(sessionCachePath());
            return false;
        }
        std::getline(input, expiryText);
        std::getline(input, vaultUrl);
        if (version == L"2")
        {
            std::getline(input, saltHex);
            std::getline(input, hashHex);
        }
        std::getline(input, session);
        ULONGLONG expiry = _wcstoui64(expiryText.c_str(), nullptr, 10);
        std::vector<unsigned char> salt = bytesFromHex(saltHex);
        std::vector<unsigned char> hash = bytesFromHex(hashHex);
        secureClear(text);

        if (session.empty() || expiry <= currentTimeMs())
        {
            secureClear(session);
            secureClear(salt);
            secureClear(hash);
            deleteFileQuietly(sessionCachePath());
            return false;
        }

        bool migratedPin = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clearSessionStateLocked();
            session_ = std::move(session);
            sessionExpiresAtMs_ = expiry;
            vaultUrl_ = vaultUrl.empty() ? kDefaultVaultUrl : vaultUrl;
            if (pinSalt_.empty() && pinHash_.empty() && salt.size() == 16 && hash.size() == 32)
            {
                pinSalt_ = std::move(salt);
                pinHash_ = std::move(hash);
                migratedPin = true;
            }
            status_ = L"Bitwarden session restored";
        }
        secureClear(salt);
        secureClear(hash);
        if (migratedPin)
        {
            savePersistedPin();
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_.empty() || sessionExpiresAtMs_ <= currentTimeMs())
            {
                deleteFileQuietly(sessionCachePath());
                return;
            }
            session = session_;
            vaultUrl = vaultUrl_;
            expiry = sessionExpiresAtMs_;
        }

        std::wstring text = L"3\n" + std::to_wstring(expiry) + L"\n" + vaultUrl + L"\n" + session + L"\n";
        std::string protectedBytes = protectForCurrentUser(text);
        secureClear(text);
        secureClear(session);
        if (!protectedBytes.empty())
        {
            writeFileBytes(sessionCachePath(), protectedBytes);
            secureClear(protectedBytes);
        }
    }

    struct RefreshRequest
    {
        Settings settings;
        HWND window = nullptr;
        HWND previousWindow = nullptr;
        ProviderStatusReporter statusReporter = nullptr;
        bool runSync = false;
    };

    void ensureRefreshWorker()
    {
        bool expected = false;
        if (refreshWorkerStarted_.compare_exchange_strong(expected, true))
        {
            refreshWorker_ = std::thread([this] { refreshWorkerLoop(); });
        }
    }

    void refreshWorkerLoop()
    {
        for (;;)
        {
            RefreshRequest request;
            {
                std::unique_lock<std::mutex> lock(refreshMutex_);
                refreshCv_.wait(lock, [this] { return refreshStop_ || refreshPending_; });
                if (refreshStop_)
                {
                    break;
                }
                request = refreshRequest_;
                refreshPending_ = false;
                refreshRunning_ = true;
            }

            const ProviderContext ctx{ request.settings, request.window, request.previousWindow,
                                       request.statusReporter };
            const bool ok = refreshMetadataAuthorized(ctx, request.runSync);
            if (!ok && !refreshCancel_.load())
            {
                ctx.reportStatus(kProviderId, L"Vault refresh failed. Try Sync metadata again.");
            }

            {
                std::lock_guard<std::mutex> lock(refreshMutex_);
                refreshRunning_ = false;
            }
            refreshDoneCv_.notify_all();
            if (request.window)
            {
                PostMessageW(request.window, kAsyncProviderUpdatedMessage, 0, 0);
            }
        }
    }

    bool queueMetadataRefresh(const ProviderContext& ctx, bool runSync)
    {
        if (!ensureSession(ctx, true))
        {
            ctx.reportStatus(kProviderId, L"Connect canceled.");
            return false;
        }

        ensureRefreshWorker();
        {
            std::lock_guard<std::mutex> lock(refreshMutex_);
            if (refreshPending_ || refreshRunning_)
            {
                ctx.reportStatus(kProviderId, L"Vault refresh already in progress...");
                return true;
            }
            refreshCancel_.store(false);
            refreshRequest_ = RefreshRequest{ ctx.settings, ctx.window, ctx.previousWindow,
                                              ctx.statusReporter, runSync };
            refreshPending_ = true;
        }
        ctx.reportStatus(kProviderId, L"Connected. Refreshing in the background...");
        refreshCv_.notify_one();
        return true;
    }

    void cancelMetadataRefreshAndWait()
    {
        refreshCancel_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopServeLocked();
        }
        std::unique_lock<std::mutex> lock(refreshMutex_);
        if (refreshPending_ && !refreshRunning_)
        {
            refreshPending_ = false;
        }
        refreshDoneCv_.wait(lock, [this] { return !refreshRunning_ && !refreshPending_; });
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
            return loginNative(ctx);
        }
        if (action == L"reconnect")
        {
            lockSession(ctx.settings, false);
            BwProcessResult logout = runBw({ L"logout", L"--nointeraction" }, {}, kBwTimeoutMs);
            secureClear(logout.output);
            return loginNative(ctx);
        }
        if (action == L"account-email")
        {
            auto email = promptValue(ctx.window, L"Bitwarden account", L"Account email", false,
                                     ctx.settings.bitwardenAccountEmail);
            if (email)
            {
                *email = trimCliOutput(std::move(*email));
                setBitwardenAccountEmail(*email);
            }
            return true;
        }
        if (action == L"set-pin")
        {
            if (!ctx.settings.bitwardenUnlockWithPin)
            {
                showBitwardenMessage(ctx.window, L"PIN unlock is off",
                                     L"Turn on Unlock secrets with PIN, then set the PIN.", false);
                return true;
            }
            if (!ensureSession(ctx, false))
            {
                return false;
            }
            if (!setPin(ctx.window, ctx.settings.bitwardenPinTimeoutSeconds))
            {
                return false;
            }
            return true;
        }
        if (action == L"connect")
        {
            ctx.reportStatus(kProviderId, L"Connecting to the local vault...");
            return queueMetadataRefresh(ctx, false);
        }
        if (action == L"sync")
        {
            ctx.reportStatus(kProviderId, L"Syncing vault metadata...");
            return queueMetadataRefresh(ctx, true);
        }
        if (action == L"lock")
        {
            lockSession(ctx.settings, true);
            return true;
        }
        return true;
    }

    bool loginNative(const ProviderContext& ctx)
    {
        if (findBwExe().empty())
        {
            showBitwardenMessage(ctx.window, L"Bitwarden CLI not found",
                                 L"Install the Bitwarden CLI before signing in.", false);
            return false;
        }

        std::wstring email = ctx.settings.bitwardenAccountEmail;
        if (email.empty())
        {
            auto entered = promptValue(ctx.window, L"Bitwarden sign in", L"Account email", false);
            if (!entered)
            {
                return false;
            }
            email = trimCliOutput(std::move(*entered));
            if (email.empty())
            {
                return false;
            }
            setBitwardenAccountEmail(email);
        }

        auto password = promptSecret(ctx.window, L"Bitwarden sign in", L"Master password");
        if (!password || password->empty())
        {
            return false;
        }

        ctx.reportStatus(kProviderId, L"Signing in...");
        BwProcessResult result = runBw({ L"login", email, L"--raw", L"--passwordenv", L"QUICKPAL_BW_PASSWORD", L"--nointeraction" },
                                       { { L"QUICKPAL_BW_PASSWORD", *password } }, kBwUnlockTimeoutMs);
        secureClear(*password);
        secureClear(email);
        if (!result.started || result.timedOut || result.exitCode != 0)
        {
            const std::wstring raw = trimCliOutput(result.output);
            std::wstring message = result.timedOut ? L"Bitwarden sign in timed out." :
                friendlyBitwardenError(raw, L"Bitwarden could not sign in. Check the account email and master password.");
            secureClear(result.output);
            showBitwardenMessage(ctx.window, L"Could not sign in", message);
            return false;
        }

        std::wstring session = trimCliOutput(result.output);
        secureClear(result.output);
        if (session.empty())
        {
            showBitwardenMessage(ctx.window, L"Could not open vault",
                                 L"Bitwarden signed in but did not return an unlocked session.");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clearSessionStateLocked();
            session_ = std::move(session);
            sessionExpiresAtMs_ = std::numeric_limits<ULONGLONG>::max();
            needsInitialSync_ = true;
            status_ = L"Bitwarden signed in";
        }
        authorizeAfterMaster(ctx.settings);
        ctx.reportStatus(kProviderId, L"Signed in. Preparing the vault...");
        savePersistedSession(ctx.settings);
        return true;
    }

    void expireSessionIfNeeded(const Settings& settings)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool authorizationExpired =
            (settings.bitwardenUnlockWithPin || settings.bitwardenUnlockWithHello) &&
            GetTickCount64() >= pinAuthorizedUntilMs_;
        if (!settings.bitwardenUseServe || authorizationExpired)
        {
            stopServeLocked();
        }
        if (authorizationExpired)
        {
            clearCachedSecretsLocked();
        }
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
            BwProcessResult cliStatus = runBw({ L"status", L"--nointeraction" }, {}, kBwTimeoutMs);
            bool unauthenticated = false;
            if (cliStatus.exitCode == 0)
            {
                if (auto status = jsonStringProperty(cliStatus.output, L"status"))
                {
                    unauthenticated = _wcsicmp(status->c_str(), L"unauthenticated") == 0;
                }
                if (ctx.settings.bitwardenAccountEmail.empty())
                {
                    if (auto email = jsonStringProperty(cliStatus.output, L"userEmail"); email && !email->empty())
                    {
                        setBitwardenAccountEmail(*email);
                    }
                }
            }
            secureClear(cliStatus.output);
            if (unauthenticated)
            {
                return loginNative(ctx);
            }
            if (!unlockWithMasterPassword(ctx))
            {
                return false;
            }
            savePersistedSession(ctx.settings);
            return true;
        }

        if (secretAction && (ctx.settings.bitwardenUnlockWithPin || ctx.settings.bitwardenUnlockWithHello))
        {
            const ULONGLONG now = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (now < pinAuthorizedUntilMs_)
                {
                    return true;
                }
                stopServeLocked();
                clearCachedSecretsLocked();
            }
            if (ctx.settings.bitwardenUnlockWithHello)
            {
                ctx.reportStatus(kProviderId, L"Waiting for Windows Hello...");
                const HelloResult hello = verifyWindowsHello();
                if (hello == HelloResult::Verified)
                {
                    authorizeSecrets(ctx.settings);
                    return true;
                }
                if (hello == HelloResult::Canceled)
                {
                    return false;
                }
            }
            if (!ctx.settings.bitwardenUnlockWithPin)
            {
                showBitwardenMessage(ctx.window, L"Windows Hello unavailable",
                                     L"Windows Hello could not verify this request. Turn on PIN fallback or use the master password.");
                return false;
            }
            if (!pinReady())
            {
                showBitwardenMessage(ctx.window, L"Set a QuickPal PIN",
                                     L"PIN unlock is enabled, but no PIN is configured. Set it in Settings > Bitwarden.", false);
                return false;
            }
            return verifyPin(ctx.window, ctx.settings);
        }
        return true;
    }

    bool unlockWithMasterPassword(const ProviderContext& ctx)
    {
        if (findBwExe().empty())
        {
            showBitwardenMessage(ctx.window, L"Bitwarden CLI not found",
                                 L"Install the Bitwarden CLI before unlocking.", false);
            ShellExecuteW(nullptr, L"open", kInstallUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return false;
        }

        auto password = promptSecret(ctx.window, L"Unlock Bitwarden", L"Bitwarden master password");
        if (!password || password->empty())
        {
            return false;
        }

        ctx.reportStatus(kProviderId, L"Unlocking the local vault...");
        BwProcessResult result = runBw({ L"unlock", L"--raw", L"--passwordenv", L"QUICKPAL_BW_PASSWORD", L"--nointeraction" },
                                       { { L"QUICKPAL_BW_PASSWORD", *password } }, kBwUnlockTimeoutMs);
        secureClear(*password);
        if (!result.started || result.timedOut || result.exitCode != 0)
        {
            const std::wstring raw = trimCliOutput(result.output);
            std::wstring message = result.timedOut ? L"Bitwarden took too long to unlock." :
                friendlyBitwardenError(raw, L"Bitwarden could not unlock. Check the master password and try again.");
            secureClear(result.output);
            showBitwardenMessage(ctx.window, L"Could not unlock", message);
            return false;
        }

        std::wstring session = trimCliOutput(result.output);
        secureClear(result.output);
        if (session.empty())
        {
            showBitwardenMessage(ctx.window, L"Could not unlock",
                                 L"Bitwarden did not return an unlocked session.");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            secureClear(session_);
            session_ = std::move(session);
            sessionExpiresAtMs_ = std::numeric_limits<ULONGLONG>::max();
            status_ = L"Bitwarden unlocked";
        }
        authorizeAfterMaster(ctx.settings);
        ctx.reportStatus(kProviderId, L"Vault unlocked.");
        savePersistedSession(ctx.settings);
        return true;
    }

    void authorizeSecrets(const Settings& settings)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pinAttempts_ = 0;
        nextPinAllowedMs_ = 0;
        pinAuthorizedUntilMs_ = GetTickCount64() +
            static_cast<ULONGLONG>(settings.bitwardenPinTimeoutSeconds) * 1000ULL;
    }

    void authorizeAfterMaster(const Settings& settings)
    {
        if (settings.bitwardenUnlockWithPin || settings.bitwardenUnlockWithHello)
        {
            authorizeSecrets(settings);
        }
    }

    HelloResult verifyWindowsHello()
    {
        using namespace ABI::Windows::Security::Credentials::UI;

        const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
        if (FAILED(initialized))
        {
            return HelloResult::Unavailable;
        }

        HSTRING className = nullptr;
        HSTRING message = nullptr;
        IUserConsentVerifierStatics* verifier = nullptr;
        ABI::Windows::Foundation::IAsyncOperation<UserConsentVerificationResult>* operation = nullptr;
        IAsyncInfo* asyncInfo = nullptr;
        HelloResult result = HelloResult::Failed;

        if (SUCCEEDED(WindowsCreateString(RuntimeClass_Windows_Security_Credentials_UI_UserConsentVerifier,
                                           static_cast<UINT32>(wcslen(RuntimeClass_Windows_Security_Credentials_UI_UserConsentVerifier)),
                                           &className)) &&
            SUCCEEDED(RoGetActivationFactory(className,
                                             kUserConsentVerifierStaticsId,
                                             reinterpret_cast<void**>(&verifier))) &&
            SUCCEEDED(WindowsCreateString(L"Unlock QuickPal Bitwarden secrets",
                                          static_cast<UINT32>(wcslen(L"Unlock QuickPal Bitwarden secrets")),
                                          &message)) &&
            SUCCEEDED(verifier->RequestVerificationAsync(message, &operation)) && operation &&
            SUCCEEDED(operation->QueryInterface(kAsyncInfoId, reinterpret_cast<void**>(&asyncInfo))))
        {
            const ULONGLONG deadline = GetTickCount64() + kBwUnlockTimeoutMs;
            AsyncStatus status = AsyncStatus::Started;
            while (GetTickCount64() < deadline && SUCCEEDED(asyncInfo->get_Status(&status)) && status == AsyncStatus::Started)
            {
                Sleep(25);
            }
            if (status == AsyncStatus::Completed)
            {
                UserConsentVerificationResult verification = UserConsentVerificationResult_Canceled;
                if (SUCCEEDED(operation->GetResults(&verification)))
                {
                    if (verification == UserConsentVerificationResult_Verified)
                    {
                        result = HelloResult::Verified;
                    }
                    else if (verification == UserConsentVerificationResult_Canceled)
                    {
                        result = HelloResult::Canceled;
                    }
                    else
                    {
                        result = HelloResult::Unavailable;
                    }
                }
            }
            else if (status == AsyncStatus::Canceled)
            {
                result = HelloResult::Canceled;
            }
            else if (status == AsyncStatus::Started)
            {
                asyncInfo->Cancel();
            }
        }

        if (asyncInfo)
        {
            asyncInfo->Release();
        }
        if (operation)
        {
            operation->Release();
        }
        if (verifier)
        {
            verifier->Release();
        }
        if (message)
        {
            WindowsDeleteString(message);
        }
        if (className)
        {
            WindowsDeleteString(className);
        }
        RoUninitialize();
        return result;
    }

    bool pinReady() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !pinSalt_.empty() && !pinHash_.empty();
    }

    bool setPin(HWND owner, int timeoutSeconds)
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
            showBitwardenMessage(owner, L"PINs did not match",
                                 L"The Bitwarden session was not authorized. Try setting the PIN again.");
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

        {
            std::lock_guard<std::mutex> lock(mutex_);
            secureClear(pinSalt_);
            secureClear(pinHash_);
            pinSalt_ = std::move(salt);
            pinHash_ = std::move(hash);
            pinAttempts_ = 0;
            nextPinAllowedMs_ = 0;
            pinAuthorizedUntilMs_ = GetTickCount64() + static_cast<ULONGLONG>(timeoutSeconds) * 1000ULL;
        }
        savePersistedPin();
        return true;
    }

    bool verifyPin(HWND owner, const Settings& settings)
    {
        const ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (now < pinAuthorizedUntilMs_)
            {
                return true;
            }
            if (now < nextPinAllowedMs_)
            {
                showBitwardenMessage(owner, L"PIN temporarily paused",
                                     L"Too many incorrect attempts. Try again in a moment.");
                return false;
            }
        }

        const std::wstring label = L"Enter PIN (authorizes secrets for " +
            std::to_wstring(settings.bitwardenPinTimeoutSeconds / 60) + L" minutes)";
        auto pin = promptSecret(owner, L"Unlock Bitwarden secrets", label.c_str());
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
        std::vector<unsigned char> actual = hashPin(*pin, salt);
        secureClear(*pin);

        bool matches = actual.size() == expected.size();
        unsigned char diff = 0;
        for (size_t i = 0; i < std::min(actual.size(), expected.size()); ++i)
        {
            diff |= actual[i] ^ expected[i];
        }
        matches = matches && diff == 0;

        if (matches)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pinAttempts_ = 0;
            nextPinAllowedMs_ = 0;
            pinAuthorizedUntilMs_ = GetTickCount64() +
                static_cast<ULONGLONG>(settings.bitwardenPinTimeoutSeconds) * 1000ULL;
            if (!actual.empty())
            {
                SecureZeroMemory(actual.data(), actual.size());
            }
            return true;
        }

        bool lockedOut = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++pinAttempts_;
            lockedOut = pinAttempts_ >= 5;
            if (!lockedOut)
            {
                const ULONGLONG backoffSeconds = std::min<ULONGLONG>(30, 1ULL << std::min(pinAttempts_, 5));
                nextPinAllowedMs_ = GetTickCount64() + backoffSeconds * 1000ULL;
            }
        }
        if (!actual.empty())
        {
            SecureZeroMemory(actual.data(), actual.size());
        }
        if (lockedOut)
        {
            lockSession(settings, true);
            showBitwardenMessage(owner, L"Bitwarden locked",
                                 L"Five incorrect PIN attempts locked the session. Enter the master password to continue.");
            return false;
        }
        showBitwardenMessage(owner, L"Incorrect PIN", L"That PIN did not match. Try again.");
        return false;
    }

    std::vector<unsigned char> hashPin(const std::wstring& pin, const std::vector<unsigned char>& salt) const
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        {
            return {};
        }
        std::string bytes = toUtf8(pin);
        std::vector<unsigned char> hash(32);
        const NTSTATUS status = BCryptDeriveKeyPBKDF2(
            alg,
            reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
            const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()),
            210000ULL, hash.data(), static_cast<ULONG>(hash.size()), 0);
        secureClear(bytes);
        BCryptCloseAlgorithmProvider(alg, 0);
        return status >= 0 ? hash : std::vector<unsigned char>{};
    }

    bool refreshMetadataAuthorized(const ProviderContext& ctx, bool runSync)
    {
        std::wstring session = sessionSnapshot();
        if (session.empty() || refreshCancel_.load())
        {
            secureClear(session);
            return false;
        }

        bool initialSync = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initialSync = needsInitialSync_;
        }
        const bool shouldSync = runSync || initialSync;
        bool syncedWithApi = false;
        if (ctx.settings.bitwardenUseServe && ensureServe(ctx.settings))
        {
            if (shouldSync)
            {
                ctx.reportStatus(kProviderId, L"Syncing the Bitwarden vault...");
                auto sync = callServe(ctx.settings, L"POST", L"/sync", kBwListTimeoutMs);
                syncedWithApi = sync && sync->ok();
                if (sync)
                {
                    secureClear(sync->output);
                }
                if (refreshCancel_.load())
                {
                    secureClear(session);
                    return false;
                }
            }

            ctx.reportStatus(kProviderId, L"Reading vault items...");
            auto status = callServe(ctx.settings, L"GET", L"/status");
            auto folders = callServe(ctx.settings, L"GET", L"/list/object/folders");
            auto organizations = callServe(ctx.settings, L"GET", L"/list/object/organizations");
            auto items = callServe(ctx.settings, L"GET", L"/list/object/items");
            std::wstring itemJson = items && items->ok() ? bwResponseArray(items->output) : L"";
            if (items && items->ok() && !itemJson.empty())
            {
                std::wstring folderJson = folders && folders->ok() ? bwResponseArray(folders->output) : L"[]";
                std::wstring organizationJson = organizations && organizations->ok() ? bwResponseArray(organizations->output) : L"[]";
                const auto folderMap = parseIdNameMap(folderJson);
                const auto orgMap = parseIdNameMap(organizationJson);
                std::vector<BwItem> parsed = parseItems(itemJson, folderMap, orgMap,
                                                        ctx.settings.bitwardenSearchUsernames);

                std::wstring vaultUrl;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    vaultUrl = vaultUrl_;
                }
                if (status && status->ok())
                {
                    std::wstring statusData = bwResponseObject(status->output);
                    if (auto serverUrl = jsonStringProperty(statusData, L"serverUrl"); serverUrl && !serverUrl->empty())
                    {
                        vaultUrl = *serverUrl;
                    }
                    secureClear(statusData);
                }

                secureClear(items->output);
                if (folders)
                {
                    secureClear(folders->output);
                }
                if (organizations)
                {
                    secureClear(organizations->output);
                }
                if (status)
                {
                    secureClear(status->output);
                }
                secureClear(folderJson);
                secureClear(organizationJson);
                secureClear(session);

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    clearCachedSecretsLocked();
                    items_ = std::move(parsed);
                    vaultUrl_ = vaultUrl.empty() ? kDefaultVaultUrl : vaultUrl;
                    usernamesPresent_ = ctx.settings.bitwardenSearchUsernames;
                    status_ = std::to_wstring(items_.size()) + L" Bitwarden items cached via local API";
                    if (syncedWithApi)
                    {
                        needsInitialSync_ = false;
                    }
                }
                std::wstring readyStatus;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    readyStatus = status_;
                }
                ctx.reportStatus(kProviderId, readyStatus);
                savePersistedSession(ctx.settings);
                return true;
            }
            if (items)
            {
                secureClear(items->output);
            }
            secureClear(itemJson);
            if (folders)
            {
                secureClear(folders->output);
            }
            if (organizations)
            {
                secureClear(organizations->output);
            }
            if (status)
            {
                secureClear(status->output);
            }
            if (refreshCancel_.load())
            {
                secureClear(session);
                return false;
            }
        }

        bool syncedDirectly = false;
        if (shouldSync && !syncedWithApi)
        {
            if (refreshCancel_.load())
            {
                secureClear(session);
                return false;
            }
            ctx.reportStatus(kProviderId, L"Syncing through the bundled CLI...");
            BwProcessResult sync = runBw({ L"sync", L"--quiet", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwTimeoutMs);
            syncedDirectly = sync.started && !sync.timedOut && sync.exitCode == 0;
            secureClear(sync.output);
        }

        if (refreshCancel_.load())
        {
            secureClear(session);
            return false;
        }

        ctx.reportStatus(kProviderId, L"Reading vault items through the bundled CLI...");
        BwProcessResult items = runBw({ L"list", L"items", L"--nointeraction" }, { { L"BW_SESSION", session } }, kBwListTimeoutMs);
        secureClear(session);

        if (!items.started || items.timedOut || items.exitCode != 0)
        {
            secureClear(items.output);
            return false;
        }

        const std::unordered_map<std::wstring, std::wstring> folderMap;
        const std::unordered_map<std::wstring, std::wstring> orgMap;
        std::vector<BwItem> parsed = parseItems(items.output, folderMap, orgMap, ctx.settings.bitwardenSearchUsernames);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            clearCachedSecretsLocked();
            items_ = std::move(parsed);
            usernamesPresent_ = ctx.settings.bitwardenSearchUsernames;
            status_ = std::to_wstring(items_.size()) + L" Bitwarden items cached from the local vault";
            if (syncedWithApi || syncedDirectly)
            {
                needsInitialSync_ = false;
            }
        }
        std::wstring readyStatus;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            readyStatus = status_;
        }
        ctx.reportStatus(kProviderId, readyStatus);
        savePersistedSession(ctx.settings);
        return true;
    }

    std::wstring sessionSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_;
    }

    std::wstring cachedSecretById(const std::wstring& id, const wchar_t* field) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : items_)
        {
            if (item.id == id)
            {
                if (_wcsicmp(field, L"password") == 0)
                {
                    return item.password;
                }
                if (_wcsicmp(field, L"username") == 0)
                {
                    return item.username;
                }
                return {};
            }
        }
        return {};
    }

    std::wstring itemUriById(const std::wstring& id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : items_)
        {
            if (item.id == id)
            {
                return item.uri;
            }
        }
        return {};
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

        std::wstring secret = cachedSecretById(command.data, field);
        if (_wcsicmp(field, L"totp") == 0 && !command.hasTotp)
        {
            showBitwardenMessage(ctx.window, L"Nothing to copy",
                                 L"This Bitwarden item does not have a TOTP code.", false);
            return false;
        }

        bool resolvedByApi = false;
        if (secret.empty() && ctx.settings.bitwardenUseServe && ensureServe(ctx.settings))
        {
            const std::wstring path = L"/object/" + std::wstring(field) + L"/" + command.data;
            auto response = callServe(ctx.settings, L"GET", path);
            if (response && response->ok())
            {
                if (auto data = bwResponseString(response->output))
                {
                    secret = std::move(*data);
                    resolvedByApi = true;
                }
            }
            if (response)
            {
                secureClear(response->output);
            }
        }

        if (secret.empty() && !resolvedByApi)
        {
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
                showBitwardenMessage(ctx.window, L"Could not copy secret",
                                     friendlyBitwardenError(message, L"QuickPal could not read that Bitwarden field."));
                return false;
            }
            secret = trimCliOutput(result.output);
            secureClear(result.output);
        }

        if (secret.empty())
        {
            showBitwardenMessage(ctx.window, L"Nothing to copy",
                                 L"This Bitwarden item does not contain that value.", false);
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
            const std::wstring uri = itemUriById(command.data);
            if (!uri.empty())
            {
                ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

    void lockSession(const Settings&, bool runBwLock)
    {
        cancelMetadataRefreshAndWait();
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
    ULONGLONG pinAuthorizedUntilMs_ = 0;
    bool needsInitialSync_ = false;
    std::atomic_bool refreshWorkerStarted_{ false };
    std::atomic_bool refreshCancel_{ false };
    std::thread refreshWorker_;
    std::mutex refreshMutex_;
    std::condition_variable refreshCv_;
    std::condition_variable refreshDoneCv_;
    bool refreshStop_ = false;
    bool refreshPending_ = false;
    bool refreshRunning_ = false;
    RefreshRequest refreshRequest_;
    HANDLE serveProcess_ = nullptr;
    HANDLE serveJob_ = nullptr;
    DWORD serveProcessId_ = 0;
    USHORT servePort_ = 0;
    ULONGLONG serveDeadlineMs_ = 0;
    std::wstring serveAuthToken_;
};

struct PromptState
{
    std::wstring title;
    std::wstring label;
    HWND edit = nullptr;
    bool secret = true;
    bool messageOnly = false;
    bool warning = false;
    std::wstring initial;
    bool done = false;
    bool ok = false;
    std::wstring value;
    Theme theme{};
    HBRUSH backgroundBrush = nullptr;
    HBRUSH controlBrush = nullptr;
    HFONT eyebrowFont = nullptr;
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT buttonFont = nullptr;
};

COLORREF dialogColor(const D2D1_COLOR_F& color)
{
    return RGB(static_cast<BYTE>(color.r * 255.0f + 0.5f),
               static_cast<BYTE>(color.g * 255.0f + 0.5f),
               static_cast<BYTE>(color.b * 255.0f + 0.5f));
}

HFONT makeDialogFont(HWND hwnd, int points, int weight)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    return CreateFontW(-MulDiv(points, dpi ? dpi : 96, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

// Every dialog coordinate below is authored at 96 DPI and scaled through here, so
// the boxes track the fonts instead of drifting out from under them.
int scaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

struct PromptLayout
{
    UINT dpi = 96;
    RECT accentBar{};
    RECT eyebrow{};
    RECT title{};
    RECT label{};
    RECT edit{};
    RECT ok{};
    RECT cancel{};
    int buttonRadius = 12;
};

// Positions derive from the live client rect, never from the requested window
// size: the modal frame takes a few pixels, and right-aligned controls sized
// against the outer width hang off the edge.
PromptLayout computePromptLayout(UINT dpi, int clientWidth, int clientHeight, bool messageOnly)
{
    PromptLayout layout;
    layout.dpi = dpi;
    layout.buttonRadius = scaleForDpi(12, dpi);

    const int pad = scaleForDpi(24, dpi);
    const int right = clientWidth - pad;
    const int buttonHeight = scaleForDpi(34, dpi);
    const int buttonTop = clientHeight - pad - buttonHeight;

    layout.accentBar = RECT{ 0, 0, clientWidth, scaleForDpi(4, dpi) };
    layout.eyebrow = RECT{ pad, scaleForDpi(20, dpi), right, scaleForDpi(40, dpi) };
    layout.title = RECT{ pad, scaleForDpi(43, dpi), right, scaleForDpi(72, dpi) };

    if (messageOnly)
    {
        layout.label = RECT{ pad, scaleForDpi(83, dpi), right, buttonTop - scaleForDpi(12, dpi) };
        const int width = scaleForDpi(104, dpi);
        layout.ok = RECT{ right - width, buttonTop, right, buttonTop + buttonHeight };
    }
    else
    {
        layout.label = RECT{ pad, scaleForDpi(80, dpi), right, scaleForDpi(102, dpi) };
        const int editTop = scaleForDpi(108, dpi);
        layout.edit = RECT{ pad, editTop, right, editTop + scaleForDpi(36, dpi) };
        const int width = scaleForDpi(82, dpi);
        const int gap = scaleForDpi(6, dpi);
        layout.cancel = RECT{ right - width, buttonTop, right, buttonTop + buttonHeight };
        layout.ok = RECT{ layout.cancel.left - gap - width, buttonTop,
                          layout.cancel.left - gap, buttonTop + buttonHeight };
    }

    return layout;
}

PromptLayout promptLayoutFor(HWND hwnd, bool messageOnly)
{
    RECT client{};
    GetClientRect(hwnd, &client);
    return computePromptLayout(GetDpiForWindow(hwnd), client.right - client.left,
                               client.bottom - client.top, messageOnly);
}

// AdjustWindowRectExForDpi needs Windows 10 1607; fall back to the system-DPI
// version rather than guessing the frame thickness.
void adjustWindowRectForDpi(RECT& rect, DWORD style, DWORD exStyle, UINT dpi)
{
    using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjustForDpi = reinterpret_cast<AdjustForDpi>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")));
    if (adjustForDpi)
    {
        adjustForDpi(&rect, style, FALSE, exStyle, dpi);
        return;
    }
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
}

void drawDialogButton(const DRAWITEMSTRUCT& item, PromptState& state)
{
    const bool primary = item.CtlID == IDOK;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const D2D1_COLOR_F background = primary
        ? (pressed ? mixColor(state.theme.accent, state.theme.windowBg, 0.22f) : state.theme.accent)
        : (pressed ? state.theme.controlPressed : state.theme.controlBg);
    HBRUSH brush = CreateSolidBrush(dialogColor(background));
    HPEN pen = CreatePen(PS_SOLID, 1, dialogColor(primary ? state.theme.accent : state.theme.divider));
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    const int radius = scaleForDpi(12, GetDpiForWindow(item.hwndItem));
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom,
              radius, radius);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t text[32]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, dialogColor(primary ? state.theme.onAccent : state.theme.controlText));
    HGDIOBJ oldFont = SelectObject(item.hDC, state.buttonFont);
    RECT label = item.rcItem;
    DrawTextW(item.hDC, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, oldFont);
}

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
        if (!state)
        {
            return -1;
        }
        state->eyebrowFont = makeDialogFont(hwnd, 9, FW_SEMIBOLD);
        state->titleFont = makeDialogFont(hwnd, 17, FW_SEMIBOLD);
        state->bodyFont = makeDialogFont(hwnd, 10, FW_NORMAL);
        state->buttonFont = makeDialogFont(hwnd, 10, FW_SEMIBOLD);
        state->backgroundBrush = CreateSolidBrush(dialogColor(state->theme.windowBg));
        state->controlBrush = CreateSolidBrush(dialogColor(state->theme.controlBg));

        const PromptLayout layout = promptLayoutFor(hwnd, state->messageOnly);

        if (!state->messageOnly)
        {
            const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                (state->secret ? ES_PASSWORD : 0);
            state->edit = CreateWindowW(L"EDIT", L"", editStyle,
                                        layout.edit.left, layout.edit.top,
                                        layout.edit.right - layout.edit.left,
                                        layout.edit.bottom - layout.edit.top,
                                        hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
            SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->bodyFont), TRUE);
            const int editMargin = scaleForDpi(10, layout.dpi);
            SendMessageW(state->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(editMargin, editMargin));
            SetWindowTextW(state->edit, state->initial.c_str());
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
        }

        HWND ok = CreateWindowW(L"BUTTON", L"Continue", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                layout.ok.left, layout.ok.top,
                                layout.ok.right - layout.ok.left, layout.ok.bottom - layout.ok.top,
                                hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(state->buttonFont), TRUE);
        if (!state->messageOnly)
        {
            HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        layout.cancel.left, layout.cancel.top,
                                        layout.cancel.right - layout.cancel.left,
                                        layout.cancel.bottom - layout.cancel.top,
                                        hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(state->buttonFont), TRUE);
            SetFocus(state->edit);
        }
        else
        {
            SetWindowTextW(ok, L"OK");
            SetFocus(ok);
        }
        return 0;
    }
    case WM_PAINT:
        if (state)
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, state->backgroundBrush);
            const PromptLayout layout = promptLayoutFor(hwnd, state->messageOnly);

            HBRUSH accent = CreateSolidBrush(dialogColor(state->theme.accent));
            RECT accentBar = layout.accentBar;
            FillRect(dc, &accentBar, accent);
            DeleteObject(accent);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, dialogColor(state->theme.accent));
            HGDIOBJ oldFont = SelectObject(dc, state->eyebrowFont);
            RECT eyebrow = layout.eyebrow;
            DrawTextW(dc, L"QUICKPAL  ·  BITWARDEN", -1, &eyebrow, DT_LEFT | DT_SINGLELINE);

            SetTextColor(dc, dialogColor(state->theme.textPrimary));
            SelectObject(dc, state->titleFont);
            RECT titleRect = layout.title;
            DrawTextW(dc, state->title.c_str(), -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            SetTextColor(dc, dialogColor(state->theme.textSecondary));
            SelectObject(dc, state->bodyFont);
            RECT labelRect = layout.label;
            DrawTextW(dc, state->label.c_str(), -1, &labelRect,
                      state->messageOnly ? (DT_LEFT | DT_WORDBREAK) : (DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS));

            HPEN border = CreatePen(PS_SOLID, 1, dialogColor(state->theme.border));
            HGDIOBJ oldPen = SelectObject(dc, border);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, client.right, client.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(border);
            SelectObject(dc, oldFont);
            EndPaint(hwnd, &paint);
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
        if (state)
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, dialogColor(state->theme.controlText));
            SetBkColor(dc, dialogColor(state->theme.controlBg));
            return reinterpret_cast<LRESULT>(state->controlBrush);
        }
        break;
    case WM_DRAWITEM:
        if (state)
        {
            drawDialogButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam), *state);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && state)
        {
            if (!state->messageOnly)
            {
                const int length = GetWindowTextLengthW(state->edit);
                state->value.assign(static_cast<size_t>(length) + 1, L'\0');
                GetWindowTextW(state->edit, state->value.data(), length + 1);
                state->value.resize(static_cast<size_t>(length));
            }
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
    case WM_DESTROY:
        if (state)
        {
            if (state->backgroundBrush) DeleteObject(state->backgroundBrush);
            if (state->controlBrush) DeleteObject(state->controlBrush);
            if (state->eyebrowFont) DeleteObject(state->eyebrowFont);
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->bodyFont) DeleteObject(state->bodyFont);
            if (state->buttonFont) DeleteObject(state->buttonFont);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

ATOM ensurePromptClass()
{
    static ATOM atom = 0;
    if (!atom)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = promptProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.style = CS_DROPSHADOW;
        wc.lpszClassName = L"QuickPal.BitwardenPrompt";
        atom = RegisterClassExW(&wc);
    }
    return atom;
}

bool runBitwardenDialog(HWND owner, PromptState& state, int clientWidth, int clientHeight)
{
    if (!ensurePromptClass())
    {
        return false;
    }

    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    const DWORD style = WS_POPUP;
    // The palette itself is topmost, so a plain popup would open behind it.
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;

    // Callers size the content; the frame is added on top so the interior really
    // is the size the layout was authored for.
    RECT frame{ 0, 0, scaleForDpi(clientWidth, dpi), scaleForDpi(clientHeight, dpi) };
    adjustWindowRectForDpi(frame, style, exStyle, dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;

    RECT ownerRect{};
    if (!owner || !GetWindowRect(owner, &ownerRect))
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    }
    const int x = static_cast<int>(ownerRect.left) +
        std::max<int>(0, (static_cast<int>(ownerRect.right - ownerRect.left) - width) / 2);
    const int y = static_cast<int>(ownerRect.top) +
        std::max<int>(0, (static_cast<int>(ownerRect.bottom - ownerRect.top) - height) / 2);

    HWND hwnd = CreateWindowExW(exStyle,
                                L"QuickPal.BitwardenPrompt", L"QuickPal Bitwarden", style,
                                x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!hwnd)
    {
        return false;
    }
    if (owner)
    {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
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
    return state.ok;
}

std::optional<std::wstring> promptValue(HWND owner, const wchar_t* title, const wchar_t* label,
                                        bool secret, const std::wstring& initial)
{
    PromptState state;
    state.title = title;
    state.label = label;
    state.secret = secret;
    state.initial = initial;
    state.theme = resolveTheme(getSettingsSnapshot());
    if (!runBitwardenDialog(owner, state, 480, 228))
    {
        secureClear(state.value);
        return std::nullopt;
    }
    return state.value;
}

std::optional<std::wstring> promptSecret(HWND owner, const wchar_t* title, const wchar_t* label)
{
    return promptValue(owner, title, label, true);
}

void showBitwardenMessage(HWND owner, const wchar_t* title, const std::wstring& message, bool warning)
{
    PromptState state;
    state.title = title;
    state.label = message;
    state.messageOnly = true;
    state.warning = warning;
    state.theme = resolveTheme(getSettingsSnapshot());
    runBitwardenDialog(owner, state, 520, 244);
}
}

std::unique_ptr<Provider> makeBitwardenProvider()
{
    return std::make_unique<BitwardenProvider>();
}
