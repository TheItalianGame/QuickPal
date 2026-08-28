#include "providers.h"

#include "../util.h"

namespace
{
class BuiltinsProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"builtins";
        info.title = L"Built-in commands";
        return info;
    }

    void index(const ProviderContext&, std::vector<Command>& out) override
    {
        out.push_back(makeCommand(CommandKind::ReloadIndex, L"Reload indexes", L"Refresh apps, PATH tools, and file index", L"", 5200));
        out.push_back(makeCommand(CommandKind::OpenSettings, L"QuickPal settings", L"Native settings UI", L"", 5150));
        out.push_back(makeCommand(CommandKind::OpenCalculator, L"Calculator", L"Built-in local calculator", L"=", 5125));
        out.push_back(makeCommand(CommandKind::ExitApp, L"Exit QuickPal", L"Quit the background hotkey listener", L"", 3000));
        out.push_back(makeCommand(CommandKind::Builtin, L"Open Terminal", L"Windows Terminal", L"wt.exe", 5000));
        out.push_back(makeCommand(CommandKind::Builtin, L"Open PowerShell", L"PowerShell", L"powershell.exe", 4900));
        out.push_back(makeCommand(CommandKind::Builtin, L"Open Command Prompt", L"cmd.exe", L"cmd.exe", 4800));
        out.push_back(makeCommand(CommandKind::Builtin, L"Open File Explorer", L"Explorer", L"explorer.exe", 4700));
        out.push_back(makeCommand(CommandKind::Builtin, L"Open Task Manager", L"System monitor", L"taskmgr.exe", 4600));
        out.push_back(makeCommand(CommandKind::Builtin, L"Lock workstation", L"Win32 LockWorkStation API", L"lock", 4300));
        out.push_back(makeCommand(CommandKind::Builtin, L"Open PowerToys Command Palette source",
                                  L"GitHub: microsoft/PowerToys/src/modules/cmdpal",
                                  L"https://github.com/microsoft/PowerToys/tree/main/src/modules/cmdpal", 4200));

        const std::wstring profile = env(L"USERPROFILE");
        if (!profile.empty())
        {
            out.push_back(makeCommand(CommandKind::Folder, L"Open Desktop", L"User folder", profile + L"\\Desktop", 4200));
            out.push_back(makeCommand(CommandKind::Folder, L"Open Documents", L"User folder", profile + L"\\Documents", 4200));
            out.push_back(makeCommand(CommandKind::Folder, L"Open Downloads", L"User folder", profile + L"\\Downloads", 4200));
        }
    }

    bool execute(const ProviderContext&, const Command& command) override
    {
        if (command.kind == CommandKind::Builtin && command.arg == L"lock")
        {
            LockWorkStation();
            return true;
        }
        // Everything else is a path or URI; let the shared handler open it.
        return false;
    }
};
}

std::unique_ptr<Provider> makeBuiltinsProvider()
{
    return std::make_unique<BuiltinsProvider>();
}
