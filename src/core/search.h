#pragma once

#include "types.h"

#include <string>
#include <vector>

enum class QueryMode
{
    Commands,
    Files,
    Web,
    Shell,
    Math,
    Windows,
};

const wchar_t* queryModeLabel(QueryMode mode);

struct SearchOutput
{
    std::vector<Result> results;
    QueryMode mode = QueryMode::Commands;
    // Lowercased terms the UI highlights inside result titles.
    std::vector<std::wstring> highlightTerms;
    double elapsedMs = 0.0;
};

SearchOutput runSearch(const std::wstring& rawQuery, const Settings& settings, HWND self);

struct HighlightRange
{
    UINT32 start = 0;
    UINT32 length = 0;
};

// Character ranges of `text` matched by `termsLower`, merged and in ascending order.
// Prefers a contiguous substring hit per term and falls back to a subsequence walk,
// mirroring how scoreTerm ranks the same text.
std::vector<HighlightRange> highlightRanges(const std::wstring& text, const std::vector<std::wstring>& termsLower);

int scoreTerm(const std::wstring& term, const std::wstring& haystack);
int scoreCommandTerms(const std::vector<std::wstring>& terms, const Command& command);
