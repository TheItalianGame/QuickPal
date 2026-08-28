#include "providers.h"

#include "../util.h"

#include <shellapi.h>

namespace
{
class WebProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"web";
        info.title = L"Web search";
        info.prefixes = { L"??" };
        info.mode = QueryMode::Web;
        info.exclusive = true;
        return info;
    }

    void query(const ProviderContext&, const Query& q, ResultSink& sink) override
    {
        const std::wstring& webQuery = q.body;
        if (webQuery.empty())
        {
            return;
        }
        sink.add(makeCommand(CommandKind::Web, L"Search the web", webQuery, webQuery, 9000), 25000);
    }

    bool execute(const ProviderContext&, const Command& command) override
    {
        if (command.kind != CommandKind::Web)
        {
            return false;
        }
        const std::wstring url = L"https://www.google.com/search?q=" + urlEncode(command.arg);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
};
}

std::unique_ptr<Provider> makeWebProvider()
{
    return std::make_unique<WebProvider>();
}
