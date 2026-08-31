#include "providers.h"

#include "../clipboard_history.h"
#include "../settings.h"
#include "../util.h"

namespace
{
std::wstring titleForClipboard(const std::wstring& text)
{
    constexpr size_t kMaxTitle = 96;
    if (text.size() <= kMaxTitle)
    {
        return text;
    }
    return text.substr(0, kMaxTitle - 3) + L"...";
}

class ClipboardProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"clipboard";
        info.title = L"Clipboard";
        info.prefixes = { L"v", L"clip", L"clipboard" };
        info.mode = QueryMode::Clipboard;
        info.exclusive = true;
        return info;
    }

    void query(const ProviderContext&, const Query& q, ResultSink& sink) override
    {
        const auto terms = q.subjectTerms();
        const auto history = clipboardHistorySnapshot();
        if (history.empty())
        {
            sink.add(makeCommand(CommandKind::Clipboard, L"Clipboard history is empty",
                                 L"Copy text once and it will appear here", L"", 0), 5000);
            return;
        }

        int rank = 0;
        for (const auto& entry : history)
        {
            Command command = makeCommand(CommandKind::Clipboard, titleForClipboard(entry.text),
                                          entry.pinned ? L"Pinned clipboard item - Enter pastes it"
                                                       : L"Clipboard item - Enter pastes it",
                                          entry.text, 5400 - rank);
            command.data = entry.pinned ? L"pinned" : L"";
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
        if (command.kind != CommandKind::Clipboard || command.arg.empty())
        {
            return false;
        }
        pasteTextToWindow(ctx.window, ctx.previousWindow, command.arg);
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Clipboard"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open clipboard history directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for clipboard history", info().id));
    }
};
}

std::unique_ptr<Provider> makeClipboardProvider()
{
    return std::make_unique<ClipboardProvider>();
}
