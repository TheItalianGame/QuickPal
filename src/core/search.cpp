#include "search.h"

#include "indexer.h"
#include "settings.h"
#include "util.h"

#include <shared_mutex>

namespace
{
void runProvider(Provider* provider, const ProviderInfo& info, const ProviderContext& ctx, const Query& query, ResultSink& sink)
{
    sink.setCurrentProvider(info.id);
    provider->query(ctx, query, sink);
    sink.setCurrentProvider(nullptr);
}

// A copy of the query with prefix fields stripped, for providers that activate on
// shape rather than on a prefix someone else claimed.
Query withoutPrefix(const Query& query)
{
    Query bare = query;
    bare.prefix.clear();
    bare.body.clear();
    bare.bodyLower.clear();
    bare.bodyTerms.clear();
    return bare;
}

void addFallbackActions(const Query& query, const wchar_t* matchedProviderId, ResultSink& sink)
{
    const std::wstring subject = trimCopy(query.subject());
    if (subject.empty())
    {
        return;
    }

    if (!matchedProviderId || wcscmp(matchedProviderId, L"files") != 0)
    {
        Command file = makeCommand(CommandKind::PaletteQuery, L"Search files for \"" + subject + L"\"",
                                   L"Run a file search", L"f " + subject, 0);
        sink.add(std::move(file), 9000);
    }

    if (!matchedProviderId || wcscmp(matchedProviderId, L"web") != 0)
    {
        Command web = makeCommand(CommandKind::Web, L"Search the web for \"" + subject + L"\"",
                                  L"Google Search", subject, 0);
        web.provider = L"web";
        sink.add(std::move(web), 8900);
    }

    if (!matchedProviderId || wcscmp(matchedProviderId, L"shell") != 0)
    {
        Command shell = makeCommand(CommandKind::Shell, L"Run \"" + subject + L"\" in shell",
                                    L"PowerShell or cmd, based on Settings", subject, 0);
        shell.provider = L"shell";
        sink.add(std::move(shell), 8800);
    }
}

void scanStaticIndexForProvider(const ProviderInfo& info, const std::vector<std::wstring>& terms, ResultSink& sink)
{
    std::shared_lock<std::shared_mutex> lock(indexMutex());
    for (const auto& command : staticIndexUnlocked())
    {
        if (command.provider && wcscmp(command.provider, info.id) == 0)
        {
            sink.addScored(command, terms);
        }
    }
}

Query makeScopedQuery(const std::wstring& rawInput, const ProviderInfo& info)
{
    Query query;
    query.raw = trimCopy(rawInput);
    query.lower = lowerCopy(query.raw);
    query.terms = splitTerms(query.lower);

    if (!info.prefixes.empty())
    {
        query.prefix = info.prefixes.front();
        query.body = query.raw;
        query.bodyLower = query.lower;
        query.bodyTerms = query.terms;
    }
    return query;
}
}

SearchOutput runSearch(const std::wstring& rawQuery, const Settings& settings, HWND self)
{
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    SearchOutput out;
    const ProviderContext ctx{ settings, self };
    ProviderRegistry& registry = ProviderRegistry::instance();

    Provider* matched = nullptr;
    const Query query = parseQuery(rawQuery, settings, &matched);
    const ProviderInfo* matchedInfo = registry.infoFor(matched);

    ResultSink sink(settings.maxResults);

    if (matched && matchedInfo && query.hasPrefix())
    {
        // A focused mode owns the whole result list.
        out.mode = matchedInfo->mode;
        out.highlightTerms = query.bodyTerms;
        runProvider(matched, *matchedInfo, ctx, query, sink);
        scanStaticIndexForProvider(*matchedInfo, query.bodyTerms, sink);
    }
    else
    {
        if (matched && matchedInfo)
        {
            out.mode = matchedInfo->mode;
            out.highlightTerms = query.bodyTerms;
            runProvider(matched, *matchedInfo, ctx, query, sink);
        }
        else
        {
            out.highlightTerms = query.terms;
        }

        const Query bare = withoutPrefix(query);
        for (const auto& entry : registry.entries())
        {
            if (entry.provider == matched || !entry.info.runsUnprefixed)
            {
                continue;
            }
            const int before = sink.size();
            runProvider(entry.provider, entry.info, ctx, bare, sink);
            // Only claim the header pill if this provider actually contributed and
            // nothing more specific already did.
            if (sink.size() != before && out.mode == QueryMode::Commands)
            {
                out.mode = entry.info.mode;
            }
        }

        {
            std::shared_lock<std::shared_mutex> lock(indexMutex());
            for (const auto& command : staticIndexUnlocked())
            {
                sink.addScored(command, query.terms);
            }
        }
    }

    if (sink.empty() && !query.empty())
    {
        if (out.mode == QueryMode::Commands)
        {
            out.mode = QueryMode::Actions;
        }
        addFallbackActions(query, matchedInfo ? matchedInfo->id : nullptr, sink);
    }

    out.results = sink.take();

    QueryPerformanceCounter(&end);
    out.elapsedMs = frequency.QuadPart > 0
        ? (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / static_cast<double>(frequency.QuadPart)
        : 0.0;
    return out;
}

bool executeThroughProvider(const Command& command, const Settings& settings, HWND self,
                            HWND previousWindow, ProviderStatusReporter statusReporter)
{
    Provider* provider = ProviderRegistry::instance().byId(command.provider);
    if (!provider)
    {
        return false;
    }
    const ProviderContext ctx{ settings, self, previousWindow, statusReporter };
    return provider->execute(ctx, command);
}

SearchOutput runProviderSearch(const std::wstring& rawQuery, const wchar_t* providerId,
                               const Settings& settings, HWND self)
{
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    SearchOutput out;
    ProviderRegistry& registry = ProviderRegistry::instance();
    Provider* provider = registry.byId(providerId);
    if (!provider)
    {
        return runSearch(rawQuery, settings, self);
    }

    const ProviderContext ctx{ settings, self };
    const ProviderInfo* info = registry.infoFor(provider);
    if (!info)
    {
        return runSearch(rawQuery, settings, self);
    }
    const Query query = makeScopedQuery(rawQuery, *info);
    ResultSink sink(settings.maxResults);

    out.mode = info->mode;
    out.highlightTerms = query.terms;
    runProvider(provider, *info, ctx, query, sink);

    scanStaticIndexForProvider(*info, query.terms, sink);

    out.results = sink.take();

    QueryPerformanceCounter(&end);
    out.elapsedMs = frequency.QuadPart > 0
        ? (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / static_cast<double>(frequency.QuadPart)
        : 0.0;
    return out;
}
