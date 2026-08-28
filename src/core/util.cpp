#include "util.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

IconSource iconSourceFor(CommandKind kind)
{
    switch (kind)
    {
    case CommandKind::App:
    case CommandKind::PathTool:
    case CommandKind::File:
    case CommandKind::Folder:
        return IconSource::ShellPath;
    case CommandKind::Window:
        return IconSource::WindowHandle;
    default:
        return IconSource::Glyph;
    }
}

std::wstring env(const wchar_t* name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
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

std::wstring expandEnv(const std::wstring& input)
{
    const DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (needed == 0)
    {
        return input;
    }
    std::wstring output(needed, L'\0');
    ExpandEnvironmentStringsW(input.c_str(), output.data(), needed);
    while (!output.empty() && output.back() == L'\0')
    {
        output.pop_back();
    }
    return output;
}

std::wstring executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::wstring exe(path);
    const auto slash = exe.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : exe.substr(0, slash);
}

std::wstring lowerCopy(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::wstring trimCopy(const std::wstring& value)
{
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first]))
    {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1]))
    {
        --last;
    }
    return value.substr(first, last - first);
}

bool startsWith(const std::wstring& value, const std::wstring& prefix)
{
    return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::vector<std::wstring> splitTerms(const std::wstring& query)
{
    std::vector<std::wstring> terms;
    std::wstringstream stream(query);
    std::wstring term;
    while (stream >> term)
    {
        terms.push_back(term);
    }
    return terms;
}

std::wstring fileNameFromPath(const std::wstring& path)
{
    try
    {
        return fs::path(path).filename().wstring();
    }
    catch (...)
    {
        const auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }
}

std::wstring stripExtension(std::wstring name)
{
    const auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
    {
        return name.substr(0, dot);
    }
    return name;
}

std::wstring extensionLower(const std::wstring& path)
{
    try
    {
        return lowerCopy(fs::path(path).extension().wstring());
    }
    catch (...)
    {
        const auto dot = path.find_last_of(L'.');
        return dot == std::wstring::npos ? L"" : lowerCopy(path.substr(dot));
    }
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

std::wstring urlEncode(const std::wstring& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    const std::string utf8 = toUtf8(value);
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

bool copyTextToClipboard(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner))
    {
        return false;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem)
    {
        CloseClipboard();
        return false;
    }
    if (void* data = GlobalLock(mem))
    {
        memcpy(data, text.c_str(), bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    else
    {
        GlobalFree(mem);
    }
    CloseClipboard();
    return true;
}

std::optional<std::wstring> clipboardText(HWND owner)
{
    if (!OpenClipboard(owner))
    {
        return std::nullopt;
    }
    std::optional<std::wstring> result;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
    {
        if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data)))
        {
            result = std::wstring(text);
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
}

Command makeCommand(CommandKind kind, std::wstring title, std::wstring subtitle, std::wstring arg, int weight)
{
    Command command;
    command.kind = kind;
    command.title = std::move(title);
    command.subtitle = std::move(subtitle);
    command.arg = std::move(arg);
    command.weight = weight;
    command.searchText = lowerCopy(command.title + L" " + command.subtitle + L" " + command.arg);
    command.key = std::to_wstring(static_cast<int>(command.kind)) + L"|" +
        (command.arg.empty() ? lowerCopy(command.title) : lowerCopy(command.arg));
    return command;
}
