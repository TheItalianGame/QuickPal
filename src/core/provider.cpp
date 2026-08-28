#include "provider.h"

#include "frecency.h"
#include "util.h"

#include <algorithm>
#include <cwctype>

namespace
{
bool isSymbolPrefix(const std::wstring& prefix)
{
    return !prefix.empty() && std::iswalnum(prefix.front()) == 0;
}
}

void ResultSink::add(Command command, int score)
{
    if (score < 0)
    {
        return;
    }
    score += frecencyBonus(command);
    if (static_cast<int>(results_.size()) >= limit_ && !results_.empty() && score <= results_.back().score)
    {
        return;
    }

    if (currentProvider_ && !command.provider)
    {
        command.provider = currentProvider_;
    }

    Result result{ std::move(command), score };
    auto it = std::find_if(results_.begin(), results_.end(), [&](const Result& existing) {
        return result.score > existing.score;
    });
    results_.insert(it, std::move(result));
    if (static_cast<int>(results_.size()) > limit_)
    {
        results_.pop_back();
    }
}

void ResultSink::addScored(Command command, const std::vector<std::wstring>& terms)
{
    const int score = scoreCommandTerms(terms, command);
    add(std::move(command), score);
}

ProviderRegistry& ProviderRegistry::instance()
{
    static ProviderRegistry registry;
    return registry;
}

void ProviderRegistry::add(std::unique_ptr<Provider> provider)
{
    if (provider)
    {
        providers_.push_back(std::move(provider));
    }
}

Provider* ProviderRegistry::byId(const wchar_t* id) const
{
    if (!id)
    {
        return nullptr;
    }
    for (const auto& provider : providers_)
    {
        if (wcscmp(provider->info().id, id) == 0)
        {
            return provider.get();
        }
    }
    return nullptr;
}

Provider* ProviderRegistry::matchPrefix(const std::wstring& raw, const std::wstring& lower, std::wstring& outPrefix) const
{
    Provider* best = nullptr;
    size_t bestLength = 0;

    for (const auto& provider : providers_)
    {
        for (const auto& prefix : provider->info().prefixes)
        {
            if (prefix.empty())
            {
                continue;
            }

            bool matches = false;
            if (isSymbolPrefix(prefix))
            {
                // Symbols bind immediately: ">dir", "??cats", "=2+2".
                matches = startsWith(raw, prefix);
            }
            else
            {
                // Words need a separating space so "find" is not read as "f ind".
                matches = startsWith(lower, prefix + L" ");
            }

            if (matches && prefix.size() > bestLength)
            {
                best = provider.get();
                bestLength = prefix.size();
                outPrefix = prefix;
            }
        }
    }

    if (!best)
    {
        outPrefix.clear();
    }
    return best;
}

Query parseQuery(const std::wstring& rawInput, Provider** outMatched)
{
    Query query;
    query.raw = trimCopy(rawInput);
    query.lower = lowerCopy(query.raw);
    query.terms = splitTerms(query.lower);

    std::wstring prefix;
    Provider* matched = ProviderRegistry::instance().matchPrefix(query.raw, query.lower, prefix);
    if (matched && !prefix.empty())
    {
        // Word prefixes consume the separating space; symbol prefixes do not.
        const size_t skip = std::iswalnum(prefix.front()) ? prefix.size() + 1 : prefix.size();
        query.prefix = prefix;
        query.body = trimCopy(query.raw.substr(std::min(skip, query.raw.size())));
        query.bodyLower = lowerCopy(query.body);
        query.bodyTerms = splitTerms(query.bodyLower);
    }

    if (outMatched)
    {
        *outMatched = matched;
    }
    return query;
}

const wchar_t* queryModeLabel(QueryMode mode)
{
    switch (mode)
    {
    case QueryMode::Files: return L"FILES";
    case QueryMode::Web: return L"WEB";
    case QueryMode::Shell: return L"SHELL";
    case QueryMode::Math: return L"MATH";
    case QueryMode::Windows: return L"WINDOWS";
    case QueryMode::Processes: return L"PROCESSES";
    case QueryMode::Clipboard: return L"CLIPBOARD";
    case QueryMode::Actions: return L"ACTIONS";
    default: return nullptr;
    }
}
