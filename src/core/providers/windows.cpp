#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <unordered_map>

namespace
{
struct EnumContext
{
    const std::vector<std::wstring>* terms;
    HWND self;
    ResultSink* sink;
    bool focusedMode = false;
    std::unordered_map<DWORD, std::wstring> processNames;
};

std::wstring processNameForWindow(EnumContext& ctx, HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
    {
        return {};
    }

    if (const auto it = ctx.processNames.find(pid); it != ctx.processNames.end())
    {
        return it->second;
    }

    std::wstring name;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
    {
        wchar_t path[1024]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0)
        {
            name = fileNameFromPath(std::wstring(path, length));
        }
        CloseHandle(process);
    }

    ctx.processNames[pid] = name;
    return name;
}

BOOL CALLBACK enumProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);
    if (!IsWindowVisible(hwnd) || hwnd == ctx->self)
    {
        return TRUE;
    }
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
    {
        return TRUE;
    }

    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0)
    {
        return TRUE;
    }
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);
    while (!title.empty() && title.back() == L'\0')
    {
        title.pop_back();
    }
    if (title.empty())
    {
        return TRUE;
    }

    const std::wstring processName = processNameForWindow(*ctx, hwnd);
    std::wstring subtitle = L"Open window";
    if (!processName.empty())
    {
        subtitle += L" - " + processName;
    }

    Command command = makeCommand(CommandKind::Window, title, subtitle, title, 5600);
    if (!processName.empty())
    {
        command.searchText += L" " + lowerCopy(processName) + L" " + lowerCopy(stripExtension(processName));
    }
    command.hwnd = hwnd;

    const int score = scoreCommandTerms(*ctx->terms, command);
    if (score >= 0)
    {
        // Bare queries should prefer a matching live window over launching a
        // second copy of the same app. The focused "win" mode gets an even
        // larger boost because it owns the whole result list.
        const int boost = ctx->focusedMode ? 22000 : 19000;
        ctx->sink->add(std::move(command), score + boost);
    }
    return TRUE;
}

class WindowsProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"windows";
        info.title = L"Open windows";
        info.prefixes = { L"win", L"window" };
        info.mode = QueryMode::Windows;
        info.exclusive = true;
        info.runsUnprefixed = true;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        const auto terms = q.subjectTerms();
        if (!q.hasPrefix() && terms.empty())
        {
            return;
        }

        EnumContext enumCtx{ &terms, ctx.window, &sink, q.hasPrefix() };
        EnumWindows(enumProc, reinterpret_cast<LPARAM>(&enumCtx));
    }

    bool execute(const ProviderContext&, const Command& command) override
    {
        if (command.kind != CommandKind::Window || !IsWindow(command.hwnd))
        {
            return false;
        }
        if (IsIconic(command.hwnd))
        {
            ShowWindow(command.hwnd, SW_RESTORE);
        }
        SetForegroundWindow(command.hwnd);
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Open windows"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open window switcher directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for window-only search", info().id));
    }
};
}

std::unique_ptr<Provider> makeWindowsProvider()
{
    return std::make_unique<WindowsProvider>();
}
