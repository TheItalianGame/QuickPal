#include "provider.h"

#include "frecency.h"
#include "util.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace
{
bool isSymbolPrefix(const std::wstring& prefix)
{
    return !prefix.empty() && std::iswalnum(prefix.front()) == 0;
}

void appendPrefix(std::vector<std::wstring>& prefixes, std::unordered_set<std::wstring>& seen, std::wstring prefix)
{
    prefix = normalizeProviderPrefix(std::move(prefix));
    if (prefix.empty())
    {
        return;
    }
    const std::wstring key = lowerCopy(prefix);
    if (seen.insert(key).second)
    {
        prefixes.push_back(std::move(prefix));
    }
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
        RegisteredProvider entry;
        entry.provider = provider.get();
        entry.info = provider->info();
        providers_.push_back(std::move(provider));
        entries_.push_back(std::move(entry));
    }
}

const ProviderInfo* ProviderRegistry::infoFor(const Provider* provider) const
{
    if (!provider)
    {
        return nullptr;
    }
    for (const auto& entry : entries_)
    {
        if (entry.provider == provider)
        {
            return &entry.info;
        }
    }
    return nullptr;
}

Provider* ProviderRegistry::byId(const wchar_t* id) const
{
    if (!id)
    {
        return nullptr;
    }
    for (const auto& entry : entries_)
    {
        if (wcscmp(entry.info.id, id) == 0)
        {
            return entry.provider;
        }
    }
    return nullptr;
}

Provider* ProviderRegistry::matchPrefix(const std::wstring& raw, const std::wstring& lower,
                                        const Settings& settings, std::wstring& outPrefix) const
{
    Provider* best = nullptr;
    size_t bestLength = 0;

    for (const auto& entry : entries_)
    {
        for (const auto& prefix : effectiveProviderPrefixes(entry.info, settings))
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
                best = entry.provider;
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

Query parseQuery(const std::wstring& rawInput, const Settings& settings, Provider** outMatched)
{
    Query query;
    query.raw = trimCopy(rawInput);
    query.lower = lowerCopy(query.raw);
    query.terms = splitTerms(query.lower);

    std::wstring prefix;
    Provider* matched = ProviderRegistry::instance().matchPrefix(query.raw, query.lower, settings, prefix);
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

std::wstring normalizeProviderPrefix(std::wstring prefix)
{
    prefix = trimCopy(prefix);
    if (prefix.empty() || lowerCopy(prefix) == L"none")
    {
        return {};
    }

    std::wstring out;
    out.reserve(std::min<size_t>(prefix.size(), 16));
    for (wchar_t ch : prefix)
    {
        if (std::iswspace(ch) || std::iswcntrl(ch))
        {
            continue;
        }
        out.push_back(ch);
        if (out.size() >= 16)
        {
            break;
        }
    }

    if (!out.empty() && std::iswalnum(out.front()))
    {
        out = lowerCopy(out);
    }
    return out;
}

std::vector<std::wstring> effectiveProviderPrefixes(const ProviderInfo& info, const Settings& settings)
{
    std::vector<std::wstring> prefixes;
    std::unordered_set<std::wstring> seen;
    seen.reserve(info.prefixes.size() + 1);

    if (const auto it = settings.providerPrefixes.find(info.id); it != settings.providerPrefixes.end())
    {
        appendPrefix(prefixes, seen, it->second);
    }
    for (const auto& prefix : info.prefixes)
    {
        appendPrefix(prefixes, seen, prefix);
    }
    return prefixes;
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
    case QueryMode::BrowserTabs: return L"TABS";
    case QueryMode::Snippets: return L"SNIPPETS";
    case QueryMode::Values: return L"VALUES";
    case QueryMode::Bitwarden: return L"BITWARDEN";
    case QueryMode::Actions: return L"ACTIONS";
    default: return nullptr;
    }
}
