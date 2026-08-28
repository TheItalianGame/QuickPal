#include "score.h"

#include "util.h"

#include <algorithm>

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
        if (pos == 0 || haystack[pos - 1] == L' ' || haystack[pos - 1] == L'\\' || haystack[pos - 1] == L'/' ||
            haystack[pos - 1] == L'-' || haystack[pos - 1] == L'_')
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
            last.length = std::max(lastEnd, ranges[i].start + ranges[i].length) - last.start;
        }
        else
        {
            merged.push_back(ranges[i]);
        }
    }
    return merged;
}
