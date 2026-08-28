#pragma once

#include "score.h"
#include "types.h"

#include <memory>
#include <string>
#include <vector>

struct SettingRow;

// ---------------------------------------------------------------------------
// Provider API
//
// A provider is one source of commands. To add a feature (clipboard history,
// process killer, unit conversion, quicklinks...) you write ONE file:
//
//   1. subclass Provider
//   2. describe yourself in info()
//   3. implement index() and/or query()
//   4. register it in providers/providers.cpp
//
// Nothing else in the codebase needs to change. The engine handles prefix
// parsing, result bounding, ordering, highlighting, and icon resolution.
//
// Two ways to contribute results:
//
//   index()  runs on the background index thread during a rebuild. Push
//            long-lived commands; the shared fuzzy scorer ranks them for free.
//            Use this when the candidate set is knowable ahead of time
//            (installed apps, PATH tools, settings URIs, quicklinks).
//
//   query()  runs on the UI thread for every keystroke. Compute results from
//            the query itself. Use this when results depend on the query
//            (calculator, web search, live file search, running processes).
//            Keep it fast; anything slow belongs on a worker thread.
// ---------------------------------------------------------------------------

// A parsed query. The engine fills this in once and hands the same value to
// every provider, so nobody re-parses prefixes by hand.
struct Query
{
    std::wstring raw;                   // trimmed, original case
    std::wstring lower;                 // lowercased raw
    std::vector<std::wstring> terms;    // whitespace-split lower

    // Set only for the provider whose prefix matched this query.
    std::wstring prefix;                // e.g. L"f", L">", L"??"
    std::wstring body;                  // text after the prefix, trimmed, original case
    std::wstring bodyLower;
    std::vector<std::wstring> bodyTerms;

    bool hasPrefix() const { return !prefix.empty(); }
    bool empty() const { return raw.empty(); }

    // The text a provider should act on, whether or not it was prefix-activated.
    const std::wstring& subject() const { return hasPrefix() ? body : raw; }
    const std::wstring& subjectLower() const { return hasPrefix() ? bodyLower : lower; }
    const std::vector<std::wstring>& subjectTerms() const { return hasPrefix() ? bodyTerms : terms; }
};

// What a provider needs from the outside world. Passed by reference; do not
// retain it beyond the call.
struct ProviderContext
{
    const Settings& settings;
    HWND window = nullptr;   // the palette window, for self-exclusion and clipboard ownership
};

// Collects results, keeps them ordered, and enforces the max-results bound so
// providers never have to think about truncation.
class ResultSink
{
public:
    explicit ResultSink(int limit) : limit_(limit < 1 ? 1 : limit) {}

    // Add with an explicit score. Negative scores are dropped.
    void add(Command command, int score);

    // Add ranked by the shared fuzzy scorer against the query's terms.
    void addScored(Command command, const std::vector<std::wstring>& terms);

    int limit() const { return limit_; }
    int size() const { return static_cast<int>(results_.size()); }
    bool empty() const { return results_.empty(); }

    std::vector<Result> take() { return std::move(results_); }

    // The engine stamps this before calling each provider; add() applies it.
    void setCurrentProvider(const wchar_t* id) { currentProvider_ = id; }

private:
    std::vector<Result> results_;
    int limit_;
    const wchar_t* currentProvider_ = nullptr;
};

struct ProviderInfo
{
    // Stable identifier, e.g. L"files". Used to route execute() and to stamp
    // Command::provider. Must outlive the process (use a string literal).
    const wchar_t* id = L"";
    const wchar_t* title = L"";

    // Activation prefixes. Alphanumeric prefixes match as a word followed by a
    // space ("f cat"); symbol prefixes match immediately (">dir", "??cats").
    // Leave empty for index-only providers.
    std::vector<std::wstring> prefixes;

    // Pill shown in the header when this provider claims the query.
    QueryMode mode = QueryMode::Commands;

    // When prefix-activated, suppress every other provider and the index scan.
    // True for focused modes (files, shell, web); false for additive ones
    // (windows, calculator) that layer on top of normal command results.
    bool exclusive = false;

    // Also call query() when no prefix matched, so the provider can activate on
    // shape alone (the calculator does this via looksLikeMath).
    bool runsUnprefixed = false;
};

class Provider
{
public:
    virtual ~Provider() = default;

    virtual ProviderInfo info() const = 0;

    // Background thread. Push long-lived commands for the shared scorer.
    virtual void index(const ProviderContext&, std::vector<Command>&) {}

    // UI thread, per keystroke. Push query-derived results.
    virtual void query(const ProviderContext&, const Query&, ResultSink&) {}

    // Activate one of this provider's commands. Return true if handled; return
    // false to fall through to the shared open-with-shell behavior.
    virtual bool execute(const ProviderContext&, const Command&) { return false; }

    // Optional provider-owned settings rows. Providers that expose toggles,
    // install actions, or a preferred shortcut append their section here.
    virtual void settings(const ProviderContext&, std::vector<SettingRow>&) {}
};

class ProviderRegistry
{
public:
    static ProviderRegistry& instance();

    void add(std::unique_ptr<Provider> provider);
    const std::vector<std::unique_ptr<Provider>>& all() const { return providers_; }

    // Longest matching prefix wins, so "file " beats "f " on "file report".
    Provider* matchPrefix(const std::wstring& raw, const std::wstring& lower,
                          const Settings& settings, std::wstring& outPrefix) const;
    Provider* byId(const wchar_t* id) const;

private:
    std::vector<std::unique_ptr<Provider>> providers_;
};

// Builds a Query, resolving which provider's effective prefix (if any) claimed it.
Query parseQuery(const std::wstring& rawInput, const Settings& settings, Provider** outMatched);

std::wstring normalizeProviderPrefix(std::wstring prefix);
std::vector<std::wstring> effectiveProviderPrefixes(const ProviderInfo& info, const Settings& settings);

const wchar_t* queryModeLabel(QueryMode mode);
