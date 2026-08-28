#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\QuickPalChromeTabs";
constexpr uint32_t kMaxNativeMessageBytes = 4 * 1024 * 1024;

std::mutex g_stdoutMutex;

std::wstring settingsDirectory()
{
    const wchar_t* appData = _wgetenv(L"APPDATA");
    if (!appData || !*appData)
    {
        return L".";
    }
    return std::wstring(appData) + L"\\QuickPal";
}

std::wstring cachePath()
{
    return settingsDirectory() + L"\\chrome_tabs.json";
}

void writeCache(const std::string& json)
{
    CreateDirectoryW(settingsDirectory().c_str(), nullptr);
    HANDLE file = CreateFileW(cachePath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    CloseHandle(file);
}

bool readExact(void* buffer, size_t bytes)
{
    return std::fread(buffer, 1, bytes, stdin) == bytes;
}

bool readNativeMessage(std::string& json)
{
    uint32_t length = 0;
    if (!readExact(&length, sizeof(length)))
    {
        return false;
    }
    if (length == 0 || length > kMaxNativeMessageBytes)
    {
        return false;
    }

    json.assign(length, '\0');
    return readExact(json.data(), length);
}

void sendNativeMessage(const std::string& json)
{
    const uint32_t length = static_cast<uint32_t>(json.size());
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    std::fwrite(&length, 1, sizeof(length), stdout);
    std::fwrite(json.data(), 1, json.size(), stdout);
    std::fflush(stdout);
}

void sendActivate(int windowId, int tabId)
{
    std::ostringstream json;
    json << "{\"type\":\"activate\",\"windowId\":" << windowId << ",\"tabId\":" << tabId << "}";
    sendNativeMessage(json.str());
}

void handlePipeRequest(const std::string& request)
{
    std::stringstream stream(request);
    std::string action;
    std::string windowText;
    std::string tabText;
    std::getline(stream, action, '\t');
    std::getline(stream, windowText, '\t');
    std::getline(stream, tabText, '\n');

    if (action != "activate")
    {
        return;
    }

    const int windowId = std::atoi(windowText.c_str());
    const int tabId = std::atoi(tabText.c_str());
    if (windowId > 0 && tabId > 0)
    {
        sendActivate(windowId, tabId);
    }
}

void pipeLoop()
{
    for (;;)
    {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            Sleep(250);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (connected)
        {
            std::string request;
            char buffer[256]{};
            DWORD read = 0;
            while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                request.append(buffer, buffer + read);
                if (request.size() > 4096)
                {
                    break;
                }
            }
            handlePipeRequest(request);
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
}

int main()
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::thread(pipeLoop).detach();

    std::string json;
    while (readNativeMessage(json))
    {
        if (json.find("\"tabs\"") != std::string::npos)
        {
            writeCache(json);
        }
    }

    return 0;
}
