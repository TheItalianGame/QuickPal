#pragma once

#include "types.h"

#include <string>
#include <vector>

// Shared ranking used by the generic index scan and by any provider that wants the
// same feel. Higher is better; a negative score means "no match, drop it".
int scoreTerm(const std::wstring& term, const std::wstring& haystack);
int scoreCommandTerms(const std::vector<std::wstring>& terms, const Command& command);

struct HighlightRange
{
    UINT32 start = 0;
    UINT32 length = 0;
};

// Character ranges of `text` matched by `termsLower`, merged and ascending.
// Prefers a contiguous substring per term and falls back to a subsequence walk,
// mirroring how scoreTerm ranks the same text.
std::vector<HighlightRange> highlightRanges(const std::wstring& text, const std::vector<std::wstring>& termsLower);
