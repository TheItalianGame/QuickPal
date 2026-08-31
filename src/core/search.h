#pragma once

#include "provider.h"
#include "score.h"
#include "types.h"

#include <string>
#include <vector>

struct SearchOutput
{
    std::vector<Result> results;
    QueryMode mode = QueryMode::Commands;
    // Lowercased terms the UI highlights inside result titles.
    std::vector<std::wstring> highlightTerms;
    double elapsedMs = 0.0;
};

// Parses the query, dispatches to whichever providers claim it, and scans the
// shared index. Providers never see each other.
SearchOutput runSearch(const std::wstring& rawQuery, const Settings& settings, HWND self);

// Same ranking path, but scoped to a single provider. This powers provider
// hotkeys without forcing providers to invent visible prefixes just for shortcuts.
SearchOutput runProviderSearch(const std::wstring& rawQuery, const wchar_t* providerId,
                               const Settings& settings, HWND self);

// Hands a command back to the provider that produced it. Returns false when no
// provider claims it, so the caller can apply the shared open-with-shell default.
bool executeThroughProvider(const Command& command, const Settings& settings, HWND self, HWND previousWindow = nullptr);
