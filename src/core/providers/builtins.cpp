#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <shellapi.h>

namespace
{
constexpr wchar_t kSystemLock[] = L"system:lock";
constexpr wchar_t kSystemSleep[] = L"system:sleep";
constexpr wchar_t kSystemRestart[] = L"system:restart";
constexpr wchar_t kSystemShutdown[] = L"system:shutdown";
constexpr wchar_t kSystemSignOut[] = L"system:signout";
constexpr wchar_t kSystemEmptyRecycleBin[] = L"system:empty-recycle-bin";

bool enablePrivilege(const wchar_t* name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        return false;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) &&
                    AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) &&
                    GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

void sleepComputer()
{
    using SetSuspendStateFn = BOOLEAN(WINAPI*)(BOOLEAN, BOOLEAN, BOOLEAN);
    HMODULE power = LoadLibraryW(L"PowrProf.dll");
    if (!power)
    {
        return;
    }
    auto setSuspendState = reinterpret_cast<SetSuspendStateFn>(GetProcAddress(power, "SetSuspendState"));
    if (setSuspendState)
    {
        setSuspendState(FALSE, FALSE, FALSE);
    }
    FreeLibrary(power);
}

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
        out.push_back(makeCommand(CommandKind::Builtin, L"Lock workstation", L"Win32 LockWorkStation API", kSystemLock, 4300));
        out.push_back(makeCommand(CommandKind::Builtin, L"Sleep PC", L"Put this computer to sleep", kSystemSleep, 4250));
        out.push_back(makeCommand(CommandKind::Builtin, L"Restart PC", L"Restart Windows", kSystemRestart, 4240));
        out.push_back(makeCommand(CommandKind::Builtin, L"Shut down PC", L"Shut down Windows", kSystemShutdown, 4230));
        out.push_back(makeCommand(CommandKind::Builtin, L"Sign out", L"Sign out of Windows", kSystemSignOut, 4220));
        out.push_back(makeCommand(CommandKind::Builtin, L"Empty Recycle Bin", L"Delete Recycle Bin contents", kSystemEmptyRecycleBin, 4210));
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
        if (command.kind != CommandKind::Builtin)
        {
            return false;
        }

        if (command.arg == kSystemLock)
        {
            LockWorkStation();
            return true;
        }
        if (command.arg == kSystemSleep)
        {
            sleepComputer();
            return true;
        }
        if (command.arg == kSystemRestart)
        {
            enablePrivilege(SE_SHUTDOWN_NAME);
            ExitWindowsEx(EWX_REBOOT, 0);
            return true;
        }
        if (command.arg == kSystemShutdown)
        {
            enablePrivilege(SE_SHUTDOWN_NAME);
            ExitWindowsEx(EWX_POWEROFF, 0);
            return true;
        }
        if (command.arg == kSystemSignOut)
        {
            ExitWindowsEx(EWX_LOGOFF, 0);
            return true;
        }
        if (command.arg == kSystemEmptyRecycleBin)
        {
            SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
            return true;
        }
        // Everything else is a path or URI; let the shared handler open it.
        return false;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Commands"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open built-in commands directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for built-in commands", info().id));
    }
};
}

std::unique_ptr<Provider> makeBuiltinsProvider()
{
    return std::make_unique<BuiltinsProvider>();
}
