#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <shellapi.h>

#include <algorithm>
#include <mutex>
#include <sstream>

namespace
{
struct Snippet
{
    std::wstring alias;
    std::wstring body;
};

std::wstring snippetsPath()
{
    return settingsDirectory() + L"\\snippets.ini";
}

std::vector<std::wstring> readIniKeys(const std::wstring& path, const wchar_t* section)
{
    std::wstring keys(32768, L'\0');
    const DWORD len = GetPrivateProfileStringW(section, nullptr, L"", keys.data(),
                                               static_cast<DWORD>(keys.size()), path.c_str());
    keys.resize(len);

    std::vector<std::wstring> out;
    for (size_t i = 0; i < keys.size();)
    {
        const wchar_t* begin = keys.c_str() + i;
        const size_t length = wcslen(begin);
        if (length == 0)
        {
            break;
        }
        out.emplace_back(begin, length);
        i += length + 1;
    }
    return out;
}

std::wstring readIniValue(const std::wstring& path, const wchar_t* section, const std::wstring& key)
{
    std::wstring value(16384, L'\0');
    const DWORD len = GetPrivateProfileStringW(section, key.c_str(), L"", value.data(),
                                               static_cast<DWORD>(value.size()), path.c_str());
    value.resize(len);
    return value;
}

void ensureDefaults()
{
    const std::wstring path = snippetsPath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return;
    }

    WritePrivateProfileStringW(L"Snippets", L"thanks", L"Thanks!", path.c_str());
    WritePrivateProfileStringW(L"Snippets", L"lmk", L"Let me know what you think.", path.c_str());
    WritePrivateProfileStringW(L"Snippets", L"date", L"{date}", path.c_str());
    WritePrivateProfileStringW(L"Snippets", L"now", L"{datetime}", path.c_str());
}

std::wstring twoDigits(int value)
{
    wchar_t buffer[8]{};
    wsprintfW(buffer, L"%02d", value);
    return buffer;
}

std::wstring localDate()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return std::to_wstring(now.wYear) + L"-" + twoDigits(now.wMonth) + L"-" + twoDigits(now.wDay);
}

std::wstring localTime()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return twoDigits(now.wHour) + L":" + twoDigits(now.wMinute);
}

void replaceAll(std::wstring& text, const std::wstring& needle, const std::wstring& replacement)
{
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::wstring::npos)
    {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

std::wstring renderSnippet(std::wstring text)
{
    const std::wstring date = localDate();
    const std::wstring time = localTime();
    replaceAll(text, L"{date}", date);
    replaceAll(text, L"{time}", time);
    replaceAll(text, L"{datetime}", date + L" " + time);
    return text;
}

std::wstring snippetPreview(const std::wstring& text)
{
    std::wstring preview = text;
    std::replace(preview.begin(), preview.end(), L'\r', L' ');
    std::replace(preview.begin(), preview.end(), L'\n', L' ');
    preview = trimCopy(preview);
    constexpr size_t kMax = 120;
    if (preview.size() > kMax)
    {
        preview = preview.substr(0, kMax - 3) + L"...";
    }
    return preview;
}

std::vector<Snippet> loadSnippets()
{
    ensureDefaults();
    const std::wstring path = snippetsPath();
    std::vector<Snippet> snippets;
    for (const auto& key : readIniKeys(path, L"Snippets"))
    {
        const std::wstring body = readIniValue(path, L"Snippets", key);
        if (!key.empty() && !body.empty())
        {
            snippets.push_back(Snippet{ lowerCopy(key), body });
        }
    }

    std::sort(snippets.begin(), snippets.end(), [](const Snippet& a, const Snippet& b) {
        return a.alias < b.alias;
    });
    return snippets;
}

Command makeSnippetCommand(const Snippet& snippet, int weight)
{
    const std::wstring rendered = renderSnippet(snippet.body);
    Command command = makeCommand(CommandKind::Snippet, snippet.alias,
                                  snippetPreview(rendered), rendered, weight);
    command.data = snippet.alias;
    command.searchText += L" snippet text expansion paste";
    command.key = L"snippet|" + snippet.alias;
    return command;
}

class SnippetsProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"snippets";
        info.title = L"Snippets";
        info.prefixes = { L";", L"snip", L"snippet" };
        info.mode = QueryMode::Snippets;
        info.exclusive = true;
        info.runsUnprefixed = true;
        return info;
    }

    void index(const ProviderContext&, std::vector<Command>& out) override
    {
        refresh();
        (void)out;
    }

    void query(const ProviderContext&, const Query& q, ResultSink& sink) override
    {
        const auto snippets = snapshot();
        if (snippets.empty())
        {
            sink.add(makeCommand(CommandKind::Snippet, L"No snippets yet",
                                 L"Open snippets.ini from Settings to add one", L"", 0), 5000);
            return;
        }

        const auto& terms = q.subjectTerms();
        if (!q.hasPrefix() && terms.empty())
        {
            return;
        }
        int rank = 0;
        for (const auto& snippet : snippets)
        {
            Command command = makeSnippetCommand(snippet, 5600 - rank);
            const int base = terms.empty() ? command.weight : scoreCommandTerms(terms, command);
            if (base >= 0)
            {
                sink.add(std::move(command), base + 14000 - rank);
            }
            ++rank;
        }
    }

    bool execute(const ProviderContext& ctx, const Command& command) override
    {
        if (command.kind != CommandKind::Snippet || command.arg.empty())
        {
            return false;
        }
        pasteTextToWindow(ctx.window, ctx.previousWindow, command.arg);
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Snippets"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open snippets directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for snippet search", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Edit snippets", L"Open snippets.ini", info().id, L"open-file"));
    }

    bool applySetting(const ProviderContext&, const SettingItem& item) override
    {
        if (item.field != SettingField::ProviderAction || item.settingKey != L"open-file")
        {
            return false;
        }
        ensureDefaults();
        ShellExecuteW(nullptr, L"open", snippetsPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

private:
    void refresh() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached_ = loadSnippets();
    }

    std::vector<Snippet> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cached_.empty())
        {
            cached_ = loadSnippets();
        }
        return cached_;
    }

    mutable std::mutex mutex_;
    mutable std::vector<Snippet> cached_;
};
}

std::unique_ptr<Provider> makeSnippetsProvider()
{
    return std::make_unique<SnippetsProvider>();
}
