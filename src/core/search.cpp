#include "search.h"

#include "indexer.h"
#include "settings.h"
#include "util.h"

#include <shared_mutex>

namespace
{
void runProvider(Provider* provider, const ProviderContext& ctx, const Query& query, ResultSink& sink)
{
    sink.setCurrentProvider(provider->info().id);
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

void addFallbackActions(const Query& query, ResultSink& sink)
{
    const std::wstring subject = trimCopy(query.subject());
    if (subject.empty())
    {
        return;
    }

    if (query.prefix != L"f" && query.prefix != L"file")
    {
        Command file = makeCommand(CommandKind::PaletteQuery, L"Search files for \"" + subject + L"\"",
                                   L"Run a file search", L"f " + subject, 0);
        sink.add(std::move(file), 9000);
    }

    if (query.prefix != L"??")
    {
        Command web = makeCommand(CommandKind::Web, L"Search the web for \"" + subject + L"\"",
                                  L"Google Search", subject, 0);
        web.provider = L"web";
        sink.add(std::move(web), 8900);
    }

    if (query.prefix != L">")
    {
        Command shell = makeCommand(CommandKind::Shell, L"Run \"" + subject + L"\" in shell",
                                    L"PowerShell or cmd, based on Settings", subject, 0);
        shell.provider = L"shell";
        sink.add(std::move(shell), 8800);
    }
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
    const Query query = parseQuery(rawQuery, &matched);

    ResultSink sink(settings.maxResults);

    if (matched && matched->info().exclusive)
    {
        // A focused mode owns the whole result list.
        out.mode = matched->info().mode;
        out.highlightTerms = query.bodyTerms;
        runProvider(matched, ctx, query, sink);
    }
    else
    {
        if (matched)
        {
            out.mode = matched->info().mode;
            out.highlightTerms = query.bodyTerms;
            runProvider(matched, ctx, query, sink);
        }
        else
        {
            out.highlightTerms = query.terms;
        }

        const Query bare = withoutPrefix(query);
        for (const auto& provider : registry.all())
        {
            if (provider.get() == matched || !provider->info().runsUnprefixed)
            {
                continue;
            }
            const int before = sink.size();
            runProvider(provider.get(), ctx, bare, sink);
            // Only claim the header pill if this provider actually contributed and
            // nothing more specific already did.
            if (sink.size() != before && out.mode == QueryMode::Commands)
            {
                out.mode = provider->info().mode;
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
        addFallbackActions(query, sink);
    }

    out.results = sink.take();

    QueryPerformanceCounter(&end);
    out.elapsedMs = frequency.QuadPart > 0
        ? (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / static_cast<double>(frequency.QuadPart)
        : 0.0;
    return out;
}

bool executeThroughProvider(const Command& command, const Settings& settings, HWND self)
{
    Provider* provider = ProviderRegistry::instance().byId(command.provider);
    if (!provider)
    {
        return false;
    }
    const ProviderContext ctx{ settings, self };
    return provider->execute(ctx, command);
}
