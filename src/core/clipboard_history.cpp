#include "clipboard_history.h"

#include "settings.h"
#include "util.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace
{
std::mutex g_mutex;
std::vector<std::wstring> g_history;
std::vector<std::wstring> g_pins;
bool g_pinsLoaded = false;

std::wstring pinsPath()
{
    return settingsDirectory() + L"\\clipboard_pins.tsv";
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

std::wstring escapeLine(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text)
    {
        switch (ch)
        {
        case L'\\': out += L"\\\\"; break;
        case L'\t': out += L"\\t"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring unescapeLine(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != L'\\' || i + 1 >= text.size())
        {
            out.push_back(text[i]);
            continue;
        }
        const wchar_t next = text[++i];
        switch (next)
        {
        case L't': out.push_back(L'\t'); break;
        case L'n': out.push_back(L'\n'); break;
        case L'r': out.push_back(L'\r'); break;
        case L'\\': out.push_back(L'\\'); break;
        default:
            out.push_back(next);
            break;
        }
    }
    return out;
}

void loadPinsLocked()
{
    if (g_pinsLoaded)
    {
        return;
    }
    g_pinsLoaded = true;

    const std::wstring text = fromUtf8(readFileBytes(pinsPath()));
    std::wstringstream input(text);
    std::wstring line;
    while (std::getline(input, line))
    {
        line = trimCopy(unescapeLine(line));
        if (!line.empty() && std::find(g_pins.begin(), g_pins.end(), line) == g_pins.end())
        {
            g_pins.push_back(std::move(line));
        }
        if (g_pins.size() >= 40)
        {
            break;
        }
    }
}

void savePinsLocked()
{
    CreateDirectoryW(settingsDirectory().c_str(), nullptr);
    std::wstring text;
    for (const auto& pin : g_pins)
    {
        text += escapeLine(pin);
        text += L"\n";
    }
    writeFileBytes(pinsPath(), toUtf8(text));
}

std::wstring compactClipboardText(std::wstring text)
{
    std::replace(text.begin(), text.end(), L'\r', L' ');
    std::replace(text.begin(), text.end(), L'\n', L' ');
    text = trimCopy(text);
    if (text.size() > 4096)
    {
        text.resize(4096);
    }
    return text;
}
}

void captureClipboardHistory(HWND owner)
{
    if (clipboardHasHistoryExclusion())
    {
        return;
    }

    auto value = clipboardText(owner);
    if (!value)
    {
        return;
    }

    std::wstring text = compactClipboardText(*value);
    if (text.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    loadPinsLocked();
    g_history.erase(std::remove(g_history.begin(), g_history.end(), text), g_history.end());
    g_history.insert(g_history.begin(), std::move(text));
    if (g_history.size() > 80)
    {
        g_history.resize(80);
    }
}

std::vector<ClipboardEntry> clipboardHistorySnapshot()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    loadPinsLocked();

    std::vector<ClipboardEntry> out;
    out.reserve(g_pins.size() + g_history.size());
    std::unordered_set<std::wstring> seen;
    seen.reserve(g_pins.size() + g_history.size());

    for (const auto& text : g_pins)
    {
        if (seen.insert(text).second)
        {
            out.push_back(ClipboardEntry{ text, true });
        }
    }
    for (const auto& text : g_history)
    {
        if (seen.insert(text).second)
        {
            out.push_back(ClipboardEntry{ text, false });
        }
    }
    return out;
}

bool isClipboardTextPinned(const std::wstring& text)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    loadPinsLocked();
    return std::find(g_pins.begin(), g_pins.end(), text) != g_pins.end();
}

void pinClipboardText(const std::wstring& text)
{
    const std::wstring clean = compactClipboardText(text);
    if (clean.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    loadPinsLocked();
    g_pins.erase(std::remove(g_pins.begin(), g_pins.end(), clean), g_pins.end());
    g_pins.insert(g_pins.begin(), clean);
    if (g_pins.size() > 40)
    {
        g_pins.resize(40);
    }
    savePinsLocked();
}

void unpinClipboardText(const std::wstring& text)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    loadPinsLocked();
    const auto before = g_pins.size();
    g_pins.erase(std::remove(g_pins.begin(), g_pins.end(), text), g_pins.end());
    if (g_pins.size() != before)
    {
        savePinsLocked();
    }
}
