#include "search.h"

#include "expression.h"
#include "indexer.h"
#include "settings.h"
#include "util.h"

#include <algorithm>
#include <shared_mutex>

namespace
{
void insertResult(std::vector<Result>& results, Command command, int score, int maxResults)
{
    if (score < 0)
    {
        return;
    }
    if (static_cast<int>(results.size()) >= maxResults && !results.empty() && score <= results.back().score)
    {
        return;
    }

    Result result{ std::move(command), score };
    auto it = std::find_if(results.begin(), results.end(), [&](const Result& existing) {
        return result.score > existing.score;
    });
    results.insert(it, std::move(result));
    if (static_cast<int>(results.size()) > maxResults)
    {
        results.pop_back();
    }
}

std::vector<Command> enumerateWindows(const std::wstring& filterLower, HWND self)
{
    struct Context
    {
        std::wstring filter;
        HWND self;
        std::vector<Command> commands;
    } context{ filterLower, self, {} };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* ctx = reinterpret_cast<Context*>(lParam);
        if (!IsWindowVisible(hwnd) || hwnd == ctx->self)
        {
            return TRUE;
        }
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        {
            return TRUE;
        }
        const int len = GetWindowTextLengthW(hwnd);
        if (len <= 0)
        {
            return TRUE;
        }
        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(hwnd, title.data(), len + 1);
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
        if (ctx->filter.empty() || scoreCommandTerms(splitTerms(ctx->filter), command) >= 0)
        {
            ctx->commands.push_back(std::move(command));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));

    return context.commands;
}

Command makeFileCommand(const FileResultEntry& entry, int weight)
{
    return makeCommand(CommandKind::File, fileNameFromPath(entry.path), fileEntrySubtitle(entry), entry.path, weight);
}

void addFileResults(std::vector<Result>& results, const std::wstring& rawFileQuery, const Settings& settings, int maxResults)
{
    const std::wstring fileQuery = trimCopy(rawFileQuery);
    if (fileQuery.empty())
    {
        insertResult(results, makeCommand(CommandKind::File, L"Search files", L"Type a file name after f", L"", 4200), 7000, maxResults);
        return;
    }

    if (settings.useEverythingHttp && everythingHttpReady())
    {
        const auto reply = everythingHttpClient().search(everythingHttpSettingsFrom(settings), fileQuery, maxResults);
        if (reply.ok)
        {
            int rank = 0;
            for (const auto& entry : reply.entries)
            {
                insertResult(results, makeFileCommand(entry, 6600 - rank), 16000 - rank, maxResults);
                ++rank;
            }
            return;
        }
        setEverythingHttpReady(false);
    }

    if (everythingReady())
    {
        const auto entries = everythingClient().search(fileQuery, static_cast<DWORD>(maxResults));
        int rank = 0;
        for (const auto& entry : entries)
        {
            insertResult(results, makeFileCommand(entry, 6500 - rank), 15000 - rank, maxResults);
            ++rank;
        }
        return;
    }

    const auto terms = splitTerms(lowerCopy(fileQuery));
    {
        std::shared_lock<std::shared_mutex> lock(indexMutex());
        for (const auto& command : fileIndexUnlocked())
        {
            insertResult(results, command, scoreCommandTerms(terms, command), maxResults);
        }
    }

    if (fileIndexing())
    {
        insertResult(results, makeCommand(CommandKind::File, L"File index is warming up", L"Results improve as the background index completes", L"", 0), 6000, maxResults);
    }
}
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
    default: return nullptr;
    }
}

int scoreTerm(const std::wstring& term, const std::wstring& haystack)
{
    if (term.empty())
    {
        return 0;
    }
    if (haystack.empty())
    {
        return -1;
    }

    if (startsWith(haystack, term))
    {
        return 1200 - static_cast<int>(haystack.size() / 8);
    }

    size_t pos = haystack.find(term);
    while (pos != std::wstring::npos)
    {
        if (pos == 0 || haystack[pos - 1] == L' ' || haystack[pos - 1] == L'\\' || haystack[pos - 1] == L'/' || haystack[pos - 1] == L'-' || haystack[pos - 1] == L'_')
        {
            return 1000 - static_cast<int>(std::min<size_t>(pos, 250));
        }
        pos = haystack.find(term, pos + 1);
    }

    pos = haystack.find(term);
    if (pos != std::wstring::npos)
    {
        return 800 - static_cast<int>(std::min<size_t>(pos, 300));
    }

    size_t h = 0;
    int gaps = 0;
    int span = 0;
    bool started = false;
    for (wchar_t ch : term)
    {
        bool found = false;
        while (h < haystack.size())
        {
            if (haystack[h] == ch)
            {
                if (!started)
                {
                    started = true;
                }
                else
                {
                    ++span;
                }
                ++h;
                found = true;
                break;
            }
            if (started)
            {
                ++gaps;
                ++span;
            }
            ++h;
        }
        if (!found)
        {
            return -1;
        }
    }
    return 380 - gaps * 2 - span;
}

int scoreCommandTerms(const std::vector<std::wstring>& terms, const Command& command)
{
    if (terms.empty())
    {
        return command.weight;
    }

    int total = command.weight;
    for (const auto& term : terms)
    {
        const int score = scoreTerm(term, command.searchText);
        if (score < 0)
        {
            return -1;
        }
        total += score;
    }
    return total;
}

std::vector<HighlightRange> highlightRanges(const std::wstring& text, const std::vector<std::wstring>& termsLower)
{
    std::vector<HighlightRange> ranges;
    if (text.empty() || termsLower.empty())
    {
        return ranges;
    }

    const std::wstring hay = lowerCopy(text);
    for (const auto& term : termsLower)
    {
        if (term.empty())
        {
            continue;
        }

        if (const size_t pos = hay.find(term); pos != std::wstring::npos)
        {
            ranges.push_back(HighlightRange{ static_cast<UINT32>(pos), static_cast<UINT32>(term.size()) });
            continue;
        }

        size_t h = 0;
        std::vector<HighlightRange> subsequence;
        bool complete = true;
        for (wchar_t ch : term)
        {
            bool found = false;
            while (h < hay.size())
            {
                if (hay[h] == ch)
                {
                    subsequence.push_back(HighlightRange{ static_cast<UINT32>(h), 1 });
                    ++h;
                    found = true;
                    break;
                }
                ++h;
            }
            if (!found)
            {
                complete = false;
                break;
            }
        }
        if (complete)
        {
            ranges.insert(ranges.end(), subsequence.begin(), subsequence.end());
        }
    }

    if (ranges.empty())
    {
        return ranges;
    }

    std::sort(ranges.begin(), ranges.end(), [](const HighlightRange& a, const HighlightRange& b) {
        return a.start < b.start;
    });

    std::vector<HighlightRange> merged;
    merged.push_back(ranges.front());
    for (size_t i = 1; i < ranges.size(); ++i)
    {
        HighlightRange& last = merged.back();
        const UINT32 lastEnd = last.start + last.length;
        if (ranges[i].start <= lastEnd)
        {
            const UINT32 end = std::max(lastEnd, ranges[i].start + ranges[i].length);
            last.length = end - last.start;
        }
        else
        {
            merged.push_back(ranges[i]);
        }
    }
    return merged;
}

SearchOutput runSearch(const std::wstring& rawQuery, const Settings& settings, HWND self)
{
    LARGE_INTEGER freq{};
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    SearchOutput out;
    const int maxResults = settings.maxResults;
    out.results.reserve(static_cast<size_t>(maxResults) + 4);

    const std::wstring raw = trimCopy(rawQuery);
    const std::wstring queryLower = lowerCopy(raw);
    const auto queryTerms = splitTerms(queryLower);

    if (startsWith(raw, L">"))
    {
        out.mode = QueryMode::Shell;
        const std::wstring commandText = trimCopy(raw.substr(1));
        if (!commandText.empty())
        {
            insertResult(out.results,
                makeCommand(CommandKind::Shell, settings.shellUsesPowerShell ? L"Run PowerShell command" : L"Run cmd command", commandText, commandText, 9000),
                25000, maxResults);
        }
    }
    else if (startsWith(raw, L"??"))
    {
        out.mode = QueryMode::Web;
        const std::wstring webQuery = trimCopy(raw.substr(2));
        if (!webQuery.empty())
        {
            insertResult(out.results, makeCommand(CommandKind::Web, L"Search the web", webQuery, webQuery, 9000), 25000, maxResults);
        }
    }
    else if (startsWith(queryLower, L"file ") || startsWith(queryLower, L"f "))
    {
        out.mode = QueryMode::Files;
        const size_t skip = startsWith(queryLower, L"file ") ? 5 : 2;
        const std::wstring fileQuery = raw.substr(skip);
        addFileResults(out.results, fileQuery, settings, maxResults);
        out.highlightTerms = splitTerms(lowerCopy(trimCopy(fileQuery)));
    }
    else
    {
        out.highlightTerms = queryTerms;

        if (startsWith(queryLower, L"win ") || startsWith(queryLower, L"window "))
        {
            out.mode = QueryMode::Windows;
            const std::wstring filter = trimCopy(queryLower.substr(startsWith(queryLower, L"win ") ? 4 : 7));
            out.highlightTerms = splitTerms(filter);
            const auto filterTerms = splitTerms(filter);
            for (auto& command : enumerateWindows(filter, self))
            {
                const int score = scoreCommandTerms(filterTerms, command) + 12000;
                insertResult(out.results, std::move(command), score, maxResults);
            }
        }

        if (startsWith(raw, L"=") || startsWith(queryLower, L"calc ") || looksLikeMath(raw))
        {
            std::wstring expr = raw;
            if (startsWith(raw, L"="))
            {
                expr = trimCopy(raw.substr(1));
            }
            else if (startsWith(queryLower, L"calc "))
            {
                expr = trimCopy(raw.substr(5));
            }
            if (!expr.empty())
            {
                ExpressionParser parser(expr);
                if (auto value = parser.parse())
                {
                    if (out.mode == QueryMode::Commands)
                    {
                        out.mode = QueryMode::Math;
                    }
                    const std::wstring formatted = formatDouble(*value);
                    insertResult(out.results,
                        makeCommand(CommandKind::Calc, formatted, L"Calculator result — Enter copies it", formatted, 9000),
                        24000, maxResults);
                }
            }
        }

        {
            std::shared_lock<std::shared_mutex> lock(indexMutex());
            for (const auto& command : staticIndexUnlocked())
            {
                insertResult(out.results, command, scoreCommandTerms(queryTerms, command), maxResults);
            }
        }
    }

    QueryPerformanceCounter(&end);
    out.elapsedMs = freq.QuadPart > 0
        ? (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / static_cast<double>(freq.QuadPart)
        : 0.0;
    return out;
}
