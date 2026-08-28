#include "providers.h"

#include "../settings.h"
#include "../util.h"

namespace
{
struct EnumContext
{
    const std::vector<std::wstring>* terms;
    HWND self;
    ResultSink* sink;
};

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

    Command command = makeCommand(CommandKind::Window, title, L"Open window", title, 5100);
    command.hwnd = hwnd;

    const int score = scoreCommandTerms(*ctx->terms, command);
    if (score >= 0)
    {
        // Live windows outrank indexed commands with the same text.
        ctx->sink->add(std::move(command), score + 12000);
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
        info.exclusive = false;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        const auto terms = q.subjectTerms();
        EnumContext enumCtx{ &terms, ctx.window, &sink };
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
    }
};
}

std::unique_ptr<Provider> makeWindowsProvider()
{
    return std::make_unique<WindowsProvider>();
}
