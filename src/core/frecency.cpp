#include "frecency.h"

#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace
{
struct Entry
{
    int launches = 0;
    std::time_t lastUsed = 0;
};

std::mutex g_mutex;
std::unordered_map<std::wstring, Entry> g_entries;
bool g_loaded = false;

std::wstring frecencyPath()
{
    return settingsDirectory() + L"\\frecency.tsv";
}

uint64_t fnv1a(const std::wstring& value)
{
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t ch : value)
    {
        const uint32_t c = static_cast<uint32_t>(ch);
        hash ^= c & 0xFFu;
        hash *= 1099511628211ULL;
        hash ^= (c >> 8) & 0xFFu;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring hexKey(uint64_t value)
{
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out(16, L'0');
    for (int i = 15; i >= 0; --i)
    {
        out[static_cast<size_t>(i)] = kHex[value & 0x0F];
        value >>= 4;
    }
    return out;
}

std::wstring commandHash(const Command& command)
{
    const std::wstring& key = command.key.empty() ? command.searchText : command.key;
    return key.empty() ? std::wstring{} : hexKey(fnv1a(key));
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
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
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
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void loadLocked()
{
    if (g_loaded)
    {
        return;
    }
    g_loaded = true;

    const std::string bytes = readFileBytes(frecencyPath());
    std::istringstream input(bytes);
    std::string line;
    while (std::getline(input, line))
    {
        std::istringstream fields(line);
        std::string key;
        std::string launches;
        std::string lastUsed;
        if (!std::getline(fields, key, '\t') || !std::getline(fields, launches, '\t') ||
            !std::getline(fields, lastUsed, '\t'))
        {
            continue;
        }

        Entry entry;
        entry.launches = std::max(0, std::atoi(launches.c_str()));
        entry.lastUsed = static_cast<std::time_t>(_atoi64(lastUsed.c_str()));
        if (entry.launches <= 0 || key.empty())
        {
            continue;
        }
        g_entries[std::wstring(key.begin(), key.end())] = entry;
    }
}

void saveLocked()
{
    std::ostringstream output;
    for (const auto& [key, entry] : g_entries)
    {
        if (entry.launches <= 0)
        {
            continue;
        }
        output << std::string(key.begin(), key.end()) << '\t'
               << entry.launches << '\t'
               << static_cast<long long>(entry.lastUsed) << '\t'
               << '\n';
    }
    writeFileBytes(frecencyPath(), output.str());
}
}

void loadFrecency()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
}

int frecencyBonus(const Command& command)
{
    if (command.kind == CommandKind::Action || command.kind == CommandKind::PaletteQuery)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();

    const std::wstring hash = commandHash(command);
    const auto it = g_entries.find(hash);
    if (it == g_entries.end())
    {
        return 0;
    }

    const Entry& entry = it->second;
    const std::time_t now = std::time(nullptr);
    const double ageDays = entry.lastUsed > 0
        ? std::max(0.0, std::difftime(now, entry.lastUsed) / 86400.0)
        : 365.0;

    const int launchBonus = std::min(2600, entry.launches * 160);
    const int recencyBonus = static_cast<int>(1800.0 * std::exp(-ageDays / 14.0));
    return launchBonus + recencyBonus;
}

void recordCommandLaunch(const Command& command)
{
    if (command.kind == CommandKind::Action || command.kind == CommandKind::PaletteQuery)
    {
        return;
    }

    const std::wstring hash = commandHash(command);
    if (hash.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    loadLocked();
    Entry& entry = g_entries[hash];
    entry.launches = std::min(entry.launches + 1, 1000000);
    entry.lastUsed = std::time(nullptr);
    saveLocked();
}
