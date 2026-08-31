#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <mutex>
#include <sstream>

namespace
{
struct QuickLink
{
    std::wstring alias;
    std::wstring url;
};

std::wstring quickLinksPath()
{
    return settingsDirectory() + L"\\quicklinks.ini";
}

std::wstring readQuickLinkValue(const std::wstring& alias)
{
    std::wstring value(2048, L'\0');
    const DWORD len = GetPrivateProfileStringW(L"QuickLinks", alias.c_str(), L"", value.data(),
                                               static_cast<DWORD>(value.size()), quickLinksPath().c_str());
    value.resize(len);
    return value;
}

void ensureDefaults()
{
    const std::wstring path = quickLinksPath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return;
    }
    WritePrivateProfileStringW(L"QuickLinks", L"gh", L"https://github.com/search?q={query}", path.c_str());
    WritePrivateProfileStringW(L"QuickLinks", L"yt", L"https://www.youtube.com/results?search_query={query}", path.c_str());
    WritePrivateProfileStringW(L"QuickLinks", L"wiki", L"https://en.wikipedia.org/wiki/Special:Search?search={query}", path.c_str());
    WritePrivateProfileStringW(L"QuickLinks", L"maps", L"https://www.google.com/maps/search/{query}", path.c_str());
}

std::vector<QuickLink> loadQuickLinks()
{
    ensureDefaults();

    std::wstring aliases(8192, L'\0');
    const DWORD len = GetPrivateProfileStringW(L"QuickLinks", nullptr, L"", aliases.data(),
                                               static_cast<DWORD>(aliases.size()), quickLinksPath().c_str());
    aliases.resize(len);

    std::vector<QuickLink> links;
    for (size_t i = 0; i < aliases.size();)
    {
        const wchar_t* begin = aliases.c_str() + i;
        const size_t length = wcslen(begin);
        if (length == 0)
        {
            break;
        }
        std::wstring alias(begin, length);
        const std::wstring url = readQuickLinkValue(alias);
        if (!alias.empty() && url.find(L"{query}") != std::wstring::npos)
        {
            links.push_back(QuickLink{ lowerCopy(alias), url });
        }
        i += length + 1;
    }
    return links;
}

std::wstring replaceQuery(std::wstring templ, const std::wstring& query)
{
    const std::wstring encoded = urlEncode(query);
    size_t pos = 0;
    while ((pos = templ.find(L"{query}", pos)) != std::wstring::npos)
    {
        templ.replace(pos, 7, encoded);
        pos += encoded.size();
    }
    return templ;
}

std::wstring primaryPrefix(const ProviderInfo& info, const Settings& settings)
{
    const std::vector<std::wstring> prefixes = effectiveProviderPrefixes(info, settings);
    return prefixes.empty() ? L"ql" : prefixes.front();
}

Command makeQuickLinkPrompt(const QuickLink& link, const std::wstring& prefix, int rank)
{
    Command command = makeCommand(CommandKind::PaletteQuery, link.alias + L" quicklink",
                                  link.url, prefix + L" " + link.alias + L" ", 3800 - rank);
    command.searchText += L" quicklink alias url";
    return command;
}

class QuickLinksProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"quicklinks";
        info.title = L"Quicklinks";
        info.prefixes = { L"ql", L"link" };
        info.mode = QueryMode::Web;
        info.exclusive = true;
        info.runsUnprefixed = true;
        return info;
    }

    void index(const ProviderContext& ctx, std::vector<Command>& out) override
    {
        refresh();
        const std::wstring prefix = primaryPrefix(info(), ctx.settings);
        int rank = 0;
        for (const auto& link : linksSnapshot())
        {
            out.push_back(makeQuickLinkPrompt(link, prefix, rank++));
        }
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        const auto links = linksSnapshot();
        const std::wstring subject = q.subject();
        const std::wstring subjectLower = q.subjectLower();
        const auto terms = q.subjectTerms();
        const std::wstring prefix = primaryPrefix(info(), ctx.settings);

        if (q.hasPrefix() && terms.empty())
        {
            return;
        }

        if (terms.size() < 2)
        {
            return;
        }

        const std::wstring alias = terms.front();
        const size_t firstSpace = subject.find_first_of(L" \t");
        if (firstSpace == std::wstring::npos)
        {
            return;
        }
        const std::wstring body = trimCopy(subject.substr(firstSpace + 1));
        if (body.empty())
        {
            return;
        }

        for (const auto& link : links)
        {
            if (link.alias != alias)
            {
                continue;
            }
            Command command = makeCommand(CommandKind::QuickLink, L"Open " + alias, body, replaceQuery(link.url, body), 9200);
            command.data = link.url;
            command.searchText += L" " + subjectLower + L" quicklink";
            sink.add(std::move(command), 24500);
            return;
        }
    }

    bool execute(const ProviderContext&, const Command& command) override
    {
        if (command.kind != CommandKind::QuickLink || command.arg.empty())
        {
            return false;
        }
        ShellExecuteW(nullptr, L"open", command.arg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Quicklinks"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open quicklink aliases directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for quicklink-only search", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderAction, SettingKind::Action,
                                      L"Edit quicklinks", L"Open quicklinks.ini", info().id, L"open-file"));
    }

    bool applySetting(const ProviderContext&, const SettingItem& item) override
    {
        if (item.field != SettingField::ProviderAction || item.settingKey != L"open-file")
        {
            return false;
        }
        ensureDefaults();
        ShellExecuteW(nullptr, L"open", quickLinksPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

private:
    void refresh() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached_ = loadQuickLinks();
    }

    std::vector<QuickLink> linksSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cached_.empty())
        {
            cached_ = loadQuickLinks();
        }
        return cached_;
    }

    mutable std::mutex mutex_;
    mutable std::vector<QuickLink> cached_;
};
}

std::unique_ptr<Provider> makeQuickLinksProvider()
{
    return std::make_unique<QuickLinksProvider>();
}
