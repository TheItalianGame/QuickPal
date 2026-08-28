#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <shellapi.h>
#include <windows.h>

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

class QuickLinksProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"quicklinks";
        info.title = L"Quicklinks";
        info.runsUnprefixed = true;
        return info;
    }

    void index(const ProviderContext&, std::vector<Command>& out) override
    {
        refresh();
        for (const auto& link : linksSnapshot())
        {
            Command command = makeCommand(CommandKind::QuickLink, link.alias + L" quicklink",
                                          link.url, link.alias, 3600);
            command.searchText += L" quicklink alias url";
            out.push_back(std::move(command));
        }
    }

    void query(const ProviderContext&, const Query& q, ResultSink& sink) override
    {
        if (q.terms.size() < 2)
        {
            return;
        }

        const std::wstring alias = q.terms.front();
        const size_t firstSpace = q.raw.find_first_of(L" \t");
        if (firstSpace == std::wstring::npos)
        {
            return;
        }
        const std::wstring body = trimCopy(q.raw.substr(firstSpace + 1));
        if (body.empty())
        {
            return;
        }

        for (const auto& link : linksSnapshot())
        {
            if (link.alias != alias)
            {
                continue;
            }
            Command command = makeCommand(CommandKind::QuickLink, L"Open " + alias, body, replaceQuery(link.url, body), 9200);
            command.data = link.url;
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
