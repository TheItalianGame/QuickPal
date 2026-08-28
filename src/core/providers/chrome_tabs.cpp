#include "providers.h"

#include "../chrome_bridge.h"
#include "../settings.h"
#include "../util.h"

#include <shellapi.h>

#include <sstream>

namespace
{
std::wstring tabData(int windowId, int tabId)
{
    return std::to_wstring(windowId) + L"\t" + std::to_wstring(tabId);
}

bool parseTabData(const std::wstring& data, int& windowId, int& tabId)
{
    const size_t split = data.find(L'\t');
    if (split == std::wstring::npos)
    {
        return false;
    }
    windowId = _wtoi(data.substr(0, split).c_str());
    tabId = _wtoi(data.substr(split + 1).c_str());
    return windowId > 0 && tabId > 0;
}

class ChromeTabsProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = kChromeTabsProviderId;
        info.title = L"Chrome tabs";
        info.prefixes = { L"tab", L"tabs", L"chrome" };
        info.mode = QueryMode::BrowserTabs;
        info.exclusive = true;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        if (!ctx.settings.useChromeTabs)
        {
            return;
        }

        const std::vector<ChromeTabInfo> tabs = readChromeTabsCache();
        if (tabs.empty())
        {
            Command command = makeCommand(CommandKind::ChromeTab,
                                          L"Install Chrome tabs extension",
                                          L"Opens the unpacked extension folder and chrome://extensions",
                                          chromeExtensionDirectory(), 7600);
            command.data = L"install";
            sink.add(std::move(command), 16000);
            return;
        }

        const auto& terms = q.subjectTerms();
        int rank = 0;
        for (const ChromeTabInfo& tab : tabs)
        {
            const std::wstring title = tab.title.empty() ? tab.url : tab.title;
            Command command = makeCommand(CommandKind::ChromeTab, title, tab.url, tab.url, 6200 - rank);
            command.data = tabData(tab.windowId, tab.tabId);
            command.searchText += tab.active ? L" active current chrome tab" : L" chrome tab";

            const int base = terms.empty() ? command.weight : scoreCommandTerms(terms, command);
            if (base >= 0)
            {
                sink.add(std::move(command), base + (tab.active ? 15000 : 13500) - rank);
            }
            ++rank;
        }
    }

    bool execute(const ProviderContext&, const Command& command) override
    {
        if (command.kind != CommandKind::ChromeTab)
        {
            return false;
        }

        if (command.data == L"install")
        {
            openChromeExtensionInstallLocation();
            return true;
        }

        int windowId = 0;
        int tabId = 0;
        if (parseTabData(command.data, windowId, tabId) && activateChromeTab(windowId, tabId))
        {
            return true;
        }

        if (!command.arg.empty())
        {
            ShellExecuteW(nullptr, L"open", command.arg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Browser tabs"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open Chrome tab search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for Chrome tab search", info().id));
        out.push_back(makeSettingItem(SettingField::UseChromeTabs, SettingKind::Toggle,
                                      L"Chrome tabs", L"Read open tabs from the local Chrome extension"));
        out.push_back(makeSettingItem(SettingField::InstallChromeExtension, SettingKind::Action,
                                      L"Install extension", L"Open the unpacked extension folder and Chrome extensions page",
                                      info().id));
    }
};
}

std::unique_ptr<Provider> makeChromeTabsProvider()
{
    return std::make_unique<ChromeTabsProvider>();
}
