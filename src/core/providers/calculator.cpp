#include "providers.h"

#include "../expression.h"
#include "../settings.h"
#include "../util.h"

namespace
{
// Activates on an explicit prefix, or on shape alone when a bare query looks like
// arithmetic. runsUnprefixed is what makes "12*7" work with no prefix at all.
class CalculatorProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"calculator";
        info.title = L"Calculator";
        info.prefixes = { L"=", L"calc" };
        info.mode = QueryMode::Math;
        info.exclusive = false;
        info.runsUnprefixed = true;
        return info;
    }

    void query(const ProviderContext&, const Query& q, ResultSink& sink) override
    {
        if (!q.hasPrefix() && !looksLikeMath(q.raw))
        {
            return;
        }

        const std::wstring expression = q.subject();
        if (expression.empty())
        {
            return;
        }

        ExpressionParser parser(expression);
        const auto value = parser.parse();
        if (!value)
        {
            return;
        }

        const std::wstring formatted = formatDouble(*value);
        sink.add(makeCommand(CommandKind::Calc, formatted, L"Calculator result — Enter copies it", formatted, 9000), 24000);
    }

    bool execute(const ProviderContext& ctx, const Command& command) override
    {
        if (command.kind != CommandKind::Calc)
        {
            return false;
        }
        copyTextToClipboard(ctx.window, command.arg);
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Calculator"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open calculator directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for calculator mode", info().id));
    }
};
}

std::unique_ptr<Provider> makeCalculatorProvider()
{
    return std::make_unique<CalculatorProvider>();
}
