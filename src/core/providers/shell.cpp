#include "providers.h"

#include "../util.h"

#include <shellapi.h>

namespace
{
class ShellProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"shell";
        info.title = L"Shell";
        info.prefixes = { L">" };
        info.mode = QueryMode::Shell;
        info.exclusive = true;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        const std::wstring& commandText = q.body;
        if (commandText.empty())
        {
            return;
        }
        sink.add(makeCommand(CommandKind::Shell,
                             ctx.settings.shellUsesPowerShell ? L"Run PowerShell command" : L"Run cmd command",
                             commandText, commandText, 9000),
                 25000);
    }

    bool execute(const ProviderContext& ctx, const Command& command) override
    {
        if (command.kind != CommandKind::Shell)
        {
            return false;
        }

        if (ctx.settings.shellUsesPowerShell)
        {
            const std::wstring params = L"-NoExit -NoProfile -ExecutionPolicy Bypass -Command " + command.arg;
            ShellExecuteW(nullptr, L"open", L"powershell.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        }
        else
        {
            const std::wstring params = L"/d /k " + command.arg;
            ShellExecuteW(nullptr, L"open", L"cmd.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return true;
    }
};
}

std::unique_ptr<Provider> makeShellProvider()
{
    return std::make_unique<ShellProvider>();
}
