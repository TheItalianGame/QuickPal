#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "../core/frecency.h"
#include "../core/indexer.h"
#include "../core/providers/providers.h"
#include "../core/search.h"
#include "../core/settings.h"
#include "../core/util.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct Scenario
{
    std::string name;
    std::string group;
    std::wstring query;
    std::wstring providerId;
    bool includeEmptyStep = false;
};

struct Options
{
    int iterations = 200;
    int warmups = 20;
    double budgetMs = 2.0;
    bool failOnBudget = false;
    bool includeFiles = true;
    bool includeFallbackFileIndex = false;
    bool json = false;
    bool listScenarios = false;
    std::vector<std::wstring> customQueries;
    std::vector<Scenario> customProviderQueries;
};

struct Stats
{
    std::string name;
    std::string group;
    std::wstring query;
    std::wstring providerId;
    int steps = 0;
    int samples = 0;
    int maxResults = 0;
    uint64_t checksum = 0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
    double avg = 0.0;
};

std::vector<std::wstring> commandLineArgs()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (!argv)
    {
        return args;
    }
    for (int i = 1; i < argc; ++i)
    {
        args.emplace_back(argv[i]);
    }
    LocalFree(argv);
    return args;
}

void printUsage()
{
    std::cout
        << "QuickPalBench\n"
        << "  --iterations N             measured loops per scenario (default 200)\n"
        << "  --warmups N                warmup loops per scenario (default 20)\n"
        << "  --budget-ms N              p95 budget shown in output (default 2.0)\n"
        << "  --fail-on-budget           exit non-zero when any p95 exceeds budget\n"
        << "  --no-files                 skip file-search scenarios\n"
        << "  --include-fallback-index   build fallback file index before measuring\n"
        << "  --list-scenarios           print the default provider benchmark matrix\n"
        << "  --json                     emit machine-readable JSON\n"
        << "  --query TEXT               add a custom measured typing scenario\n"
        << "  --provider-query ID TEXT   add a custom provider-hotkey scenario\n";
}

Options parseOptions()
{
    Options options;
    const auto args = commandLineArgs();
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring& arg = args[i];
        auto takeValue = [&]() -> std::wstring {
            if (i + 1 >= args.size())
            {
                return {};
            }
            return args[++i];
        };

        if (arg == L"--help" || arg == L"-h" || arg == L"/?")
        {
            printUsage();
            std::exit(0);
        }
        if (arg == L"--iterations" || arg == L"-n")
        {
            options.iterations = std::max(1, _wtoi(takeValue().c_str()));
        }
        else if (arg == L"--warmups")
        {
            options.warmups = std::max(0, _wtoi(takeValue().c_str()));
        }
        else if (arg == L"--budget-ms")
        {
            options.budgetMs = std::max(0.01, _wtof(takeValue().c_str()));
        }
        else if (arg == L"--fail-on-budget")
        {
            options.failOnBudget = true;
        }
        else if (arg == L"--no-files")
        {
            options.includeFiles = false;
        }
        else if (arg == L"--include-fallback-index")
        {
            options.includeFallbackFileIndex = true;
            options.includeFiles = true;
        }
        else if (arg == L"--list-scenarios")
        {
            options.listScenarios = true;
        }
        else if (arg == L"--json")
        {
            options.json = true;
        }
        else if (arg == L"--query")
        {
            std::wstring query = takeValue();
            if (!query.empty())
            {
                options.customQueries.push_back(std::move(query));
            }
        }
        else if (arg == L"--provider-query")
        {
            std::wstring provider = takeValue();
            std::wstring query = takeValue();
            if (!provider.empty())
            {
                options.customProviderQueries.push_back(
                    Scenario{ "custom provider", "scoped", std::move(query), std::move(provider), true });
            }
        }
    }
    return options;
}

std::vector<Scenario> defaultScenarios(bool includeFiles)
{
    std::vector<Scenario> scenarios = {
        { "global app/window", "no-prefix", L"chrome" },
        { "global settings", "no-prefix", L"settings" },
        { "global command", "no-prefix", L"terminal" },
        { "global system", "no-prefix", L"sleep" },
        { "global value", "no-prefix", L"guid" },
        { "global snippet", "no-prefix", L"thanks" },
        { "global quicklink", "no-prefix", L"gh quickpal" },

        { "builtin scoped", "scoped", L"terminal", L"builtins", true },
        { "settings scoped", "scoped", L"display", L"settings-uris", true },
        { "start scoped", "scoped", L"chrome", L"start-menu", true },
        { "store scoped", "scoped", L"calculator", L"apps-folder", true },
        { "path scoped", "scoped", L"git", L"path-tools", true },
        { "quicklink scoped", "scoped", L"gh quickpal", L"quicklinks", true },
        { "snippet scoped", "scoped", L"thanks", L"snippets", true },
        { "value scoped", "scoped", L"sha256 quickpal", L"values", true },
        { "bitwarden scoped", "scoped", L"github", L"bitwarden", true },
        { "window scoped", "scoped", L"chrome", L"windows", true },
        { "shell scoped", "scoped", L"get-date", L"shell", true },
        { "web scoped", "scoped", L"quickpal", L"web", true },
        { "calc scoped", "scoped", L"12345*67", L"calculator", true },
        { "process scoped", "scoped", L"chrome", L"processes", true },
        { "clipboard scoped", "scoped", L"test", L"clipboard", true },
        { "tabs scoped", "scoped", L"chrome", L"browser-tabs", true },

        { "window prefix", "typed-prefix", L"win chrome" },
        { "shell prefix", "typed-prefix", L"> get-date" },
        { "web prefix", "typed-prefix", L"?? quickpal" },
        { "calc prefix", "typed-prefix", L"=12345*67" },
        { "process prefix", "typed-prefix", L"kill chrome" },
        { "clipboard prefix", "typed-prefix", L"v test" },
        { "tabs prefix", "typed-prefix", L"tab chrome" },
        { "snippet prefix", "typed-prefix", L"; thanks" },
        { "quicklink prefix", "typed-prefix", L"ql gh quickpal" },
        { "value guid prefix", "typed-prefix", L"guid" },
        { "value hash prefix", "typed-prefix", L"sha256 quickpal" },
        { "bitwarden prefix", "typed-prefix", L"pw github" },
    };
    if (includeFiles)
    {
        scenarios.push_back({ "file scoped", "scoped", L"readme", L"files", true });
        scenarios.push_back({ "file prefix", "typed-prefix", L"f readme" });
        scenarios.push_back({ "path browse prefix", "typed-prefix", L"f %USERPROFILE%\\" });
    }
    return scenarios;
}

std::vector<std::wstring> typingSteps(const Scenario& scenario)
{
    std::vector<std::wstring> steps;
    steps.reserve(scenario.query.size() + (scenario.includeEmptyStep ? 1 : 0));
    if (scenario.includeEmptyStep)
    {
        steps.push_back(L"");
    }
    for (size_t i = 1; i <= scenario.query.size(); ++i)
    {
        steps.push_back(scenario.query.substr(0, i));
    }
    return steps;
}

SearchOutput runScenarioSearch(const Scenario& scenario, const std::wstring& query, const Settings& settings)
{
    if (!scenario.providerId.empty())
    {
        return runProviderSearch(query, scenario.providerId.c_str(), settings, nullptr);
    }
    return runSearch(query, settings, nullptr);
}

double percentile(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const double raw = p * static_cast<double>(sorted.size() - 1);
    const size_t index = static_cast<size_t>(raw + 0.5);
    return sorted[std::min(index, sorted.size() - 1)];
}

uint64_t foldOutput(const SearchOutput& output)
{
    uint64_t value = static_cast<uint64_t>(output.results.size());
    value = value * 1315423911ULL + static_cast<uint64_t>(output.highlightTerms.size());
    if (!output.results.empty())
    {
        const Command& first = output.results.front().command;
        value = value * 1315423911ULL + first.title.size();
        value = value * 1315423911ULL + first.subtitle.size();
        value = value * 1315423911ULL + static_cast<uint64_t>(output.results.front().score);
    }
    return value;
}

Stats runScenario(const Scenario& scenario, const Settings& settings, const Options& options)
{
    const std::vector<std::wstring> steps = typingSteps(scenario);
    Stats stats;
    stats.name = scenario.name;
    stats.group = scenario.group;
    stats.query = scenario.query;
    stats.providerId = scenario.providerId;
    stats.steps = static_cast<int>(steps.size());

    for (int i = 0; i < options.warmups; ++i)
    {
        for (const auto& query : steps)
        {
            const SearchOutput output = runScenarioSearch(scenario, query, settings);
            stats.checksum ^= foldOutput(output);
        }
    }

    Sleep(140);
    for (const auto& query : steps)
    {
        const SearchOutput output = runScenarioSearch(scenario, query, settings);
        stats.checksum ^= foldOutput(output);
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(options.iterations) * steps.size());
    for (int i = 0; i < options.iterations; ++i)
    {
        for (const auto& query : steps)
        {
            const auto start = std::chrono::steady_clock::now();
            const SearchOutput output = runScenarioSearch(scenario, query, settings);
            const auto end = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
            samples.push_back(elapsed);
            stats.maxResults = std::max<int>(stats.maxResults, static_cast<int>(output.results.size()));
            stats.checksum ^= foldOutput(output) + static_cast<uint64_t>(samples.size());
        }
    }

    stats.samples = static_cast<int>(samples.size());
    if (!samples.empty())
    {
        stats.avg = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
        std::sort(samples.begin(), samples.end());
        stats.p50 = percentile(samples, 0.50);
        stats.p95 = percentile(samples, 0.95);
        stats.p99 = percentile(samples, 0.99);
        stats.max = samples.back();
    }
    return stats;
}

void primeAsyncProviders(const Settings& settings, bool includeFiles)
{
    runProviderSearch(L"chrome", L"windows", settings, nullptr);
    runProviderSearch(L"chrome", L"processes", settings, nullptr);
    runProviderSearch(L"chrome", L"browser-tabs", settings, nullptr);
    if (includeFiles)
    {
        runProviderSearch(L"readme", L"files", settings, nullptr);
    }
    Sleep(900);
}

std::string jsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

void printJson(const std::vector<Stats>& rows, const Options& options, bool passed)
{
    std::cout << "{\n";
    std::cout << "  \"iterations\": " << options.iterations << ",\n";
    std::cout << "  \"warmups\": " << options.warmups << ",\n";
    std::cout << "  \"budgetMs\": " << options.budgetMs << ",\n";
    std::cout << "  \"passed\": " << (passed ? "true" : "false") << ",\n";
    std::cout << "  \"staticCommands\": " << staticCommandCount() << ",\n";
    std::cout << "  \"indexedFiles\": " << indexedFileCount() << ",\n";
    std::cout << "  \"everythingHttp\": " << (everythingHttpReady() ? "true" : "false") << ",\n";
    std::cout << "  \"everythingSdk\": " << (everythingReady() ? "true" : "false") << ",\n";
    std::cout << "  \"scenarios\": [\n";
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const Stats& s = rows[i];
        std::cout << "    {"
                  << "\"name\":\"" << jsonEscape(s.name) << "\","
                  << "\"group\":\"" << jsonEscape(s.group) << "\","
                  << "\"query\":\"" << jsonEscape(toUtf8(s.query)) << "\","
                  << "\"providerId\":\"" << jsonEscape(toUtf8(s.providerId)) << "\","
                  << "\"steps\":" << s.steps << ","
                  << "\"samples\":" << s.samples << ","
                  << "\"p50\":" << s.p50 << ","
                  << "\"p95\":" << s.p95 << ","
                  << "\"p99\":" << s.p99 << ","
                  << "\"max\":" << s.max << ","
                  << "\"avg\":" << s.avg << ","
                  << "\"maxResults\":" << s.maxResults << ","
                  << "\"checksum\":" << s.checksum
                  << "}";
        std::cout << (i + 1 == rows.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n";
    std::cout << "}\n";
}

void printTable(const std::vector<Stats>& rows, const Options& options, bool passed)
{
    std::cout << "QuickPal search benchmark\n";
    std::cout << "Static commands: " << staticCommandCount()
              << " | Indexed files: " << indexedFileCount()
              << " | Everything HTTP: " << (everythingHttpReady() ? "yes" : "no")
              << " | Everything SDK: " << (everythingReady() ? "yes" : "no") << "\n";
    std::cout << "Iterations: " << options.iterations
              << " | Warmups: " << options.warmups
              << " | p95 budget: " << std::fixed << std::setprecision(2) << options.budgetMs << " ms"
              << (options.failOnBudget ? " (enforced)" : " (display only)") << "\n\n";

    std::cout << std::left
              << std::setw(22) << "Scenario"
              << std::setw(14) << "Group"
              << std::setw(15) << "Provider"
              << std::right
              << std::setw(7) << "steps"
              << std::setw(9) << "samples"
              << std::setw(9) << "p50"
              << std::setw(9) << "p95"
              << std::setw(9) << "p99"
              << std::setw(9) << "max"
              << std::setw(9) << "avg"
              << std::setw(9) << "results"
              << "\n";
    std::cout << std::string(118, '-') << "\n";

    for (const Stats& s : rows)
    {
        std::string provider = s.providerId.empty() ? "-" : toUtf8(s.providerId);
        std::cout << std::left
                  << std::setw(22) << s.name.substr(0, 21)
                  << std::setw(14) << s.group.substr(0, 13)
                  << std::setw(15) << provider.substr(0, 14)
                  << std::right
                  << std::setw(7) << s.steps
                  << std::setw(9) << s.samples
                  << std::setw(9) << std::setprecision(3) << s.p50
                  << std::setw(9) << std::setprecision(3) << s.p95
                  << std::setw(9) << std::setprecision(3) << s.p99
                  << std::setw(9) << std::setprecision(3) << s.max
                  << std::setw(9) << std::setprecision(3) << s.avg
                  << std::setw(9) << s.maxResults
                  << "\n";
    }

    std::cout << "\n";
    std::cout << (passed ? "PASS" : "FAIL") << ": p95 "
              << (passed ? "within" : "over") << " budget"
              << (options.failOnBudget ? "" : " (not enforced)") << "\n";
}

void printScenarioList(const std::vector<Scenario>& scenarios)
{
    std::cout << "QuickPal benchmark scenarios\n\n";
    std::cout << std::left
              << std::setw(24) << "Scenario"
              << std::setw(15) << "Group"
              << std::setw(16) << "Provider"
              << "Query\n";
    std::cout << std::string(84, '-') << "\n";
    for (const Scenario& scenario : scenarios)
    {
        const std::string provider = scenario.providerId.empty() ? "-" : toUtf8(scenario.providerId);
        std::cout << std::left
                  << std::setw(24) << scenario.name.substr(0, 23)
                  << std::setw(15) << scenario.group.substr(0, 14)
                  << std::setw(16) << provider.substr(0, 15)
                  << toUtf8(scenario.query)
                  << (scenario.includeEmptyStep ? "  [includes empty provider-open step]" : "")
                  << "\n";
    }
}

std::vector<Scenario> scenariosFromOptions(const Options& options)
{
    if (options.customQueries.empty() && options.customProviderQueries.empty())
    {
        return defaultScenarios(options.includeFiles);
    }

    std::vector<Scenario> scenarios;
    int index = 1;
    for (const auto& query : options.customQueries)
    {
        scenarios.push_back({ "custom " + std::to_string(index++), "custom", query });
    }
    index = 1;
    for (const auto& scenario : options.customProviderQueries)
    {
        Scenario custom = scenario;
        custom.name = "custom provider " + std::to_string(index++);
        scenarios.push_back(std::move(custom));
    }
    return scenarios;
}
}

int main()
{
    const Options options = parseOptions();
    const std::vector<Scenario> scenarios = scenariosFromOptions(options);

    if (options.listScenarios)
    {
        printScenarioList(scenarios);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    loadSettings();
    loadFrecency();
    registerBuiltinProviders();
    rebuildIndexBlocking(options.includeFallbackFileIndex);

    Settings settings = getSettingsSnapshot();
    settings.showLatency = false;
    primeAsyncProviders(settings, options.includeFiles);

    std::vector<Stats> rows;
    rows.reserve(scenarios.size());
    bool passed = true;
    for (const Scenario& scenario : scenarios)
    {
        Stats stats = runScenario(scenario, settings, options);
        if (stats.p95 > options.budgetMs)
        {
            passed = false;
        }
        rows.push_back(std::move(stats));
    }

    if (options.json)
    {
        printJson(rows, options, passed);
    }
    else
    {
        printTable(rows, options, passed);
    }

    CoUninitialize();
    return options.failOnBudget && !passed ? 2 : 0;
}
