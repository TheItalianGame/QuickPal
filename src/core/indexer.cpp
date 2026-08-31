#include "indexer.h"

#include "provider.h"
#include "settings.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
std::shared_mutex g_indexMutex;
std::vector<Command> g_staticIndex;
std::vector<Command> g_fileIndex;

std::mutex g_statusMutex;
std::wstring g_statusBase = L"Indexing commands...";
std::wstring g_statusProviderId;
std::wstring g_statusProvider;
std::wstring g_statusTransient;

std::atomic_bool g_rebuilding{ false };
std::atomic_bool g_fileIndexing{ false };
std::atomic_int g_staticCount{ 0 };
std::atomic_int g_fileCount{ 0 };
std::atomic_bool g_everythingReady{ false };
std::atomic_bool g_everythingHttpReady{ false };
std::atomic<HWND> g_notifyWindow{ nullptr };

EverythingSdkClient g_everything;
EverythingHttpClient g_everythingHttp;

void notifyIndexChanged()
{
    if (HWND hwnd = g_notifyWindow.load())
    {
        PostMessageW(hwnd, kIndexUpdatedMessage, 0, 0);
    }
}

bool shouldSkipDirectory(const std::wstring& nameLower)
{
    static const std::unordered_set<std::wstring> skip = {
        L".git", L".hg", L".svn", L"node_modules", L"appdata", L"windows", L"program files",
        L"program files (x86)", L"programdata", L"$recycle.bin", L"system volume information",
    };
    return skip.find(nameLower) != skip.end();
}

std::wstring pathKey(const fs::path& path)
{
    return lowerCopy(path.wstring());
}

void appendScanRoot(std::vector<fs::path>& roots, std::unordered_set<std::wstring>& seenRoots, const std::wstring& rawPath)
{
    if (rawPath.empty())
    {
        return;
    }
    fs::path path(rawPath);
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec))
    {
        return;
    }
    if (seenRoots.insert(pathKey(path)).second)
    {
        roots.push_back(std::move(path));
    }
}

void appendDefaultScanRoots(std::vector<fs::path>& roots, std::unordered_set<std::wstring>& seenRoots)
{
    appendScanRoot(roots, seenRoots, env(L"USERPROFILE"));
    appendScanRoot(roots, seenRoots, env(L"OneDrive"));
    appendScanRoot(roots, seenRoots, env(L"OneDriveConsumer"));
    appendScanRoot(roots, seenRoots, env(L"OneDriveCommercial"));
    appendScanRoot(roots, seenRoots, env(L"PUBLIC") + L"\\Desktop");
    appendScanRoot(roots, seenRoots, env(L"PUBLIC") + L"\\Documents");
    appendScanRoot(roots, seenRoots, expandEnv(L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs"));
    appendScanRoot(roots, seenRoots, expandEnv(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs"));
    appendScanRoot(roots, seenRoots, env(L"ProgramFiles"));
    appendScanRoot(roots, seenRoots, env(L"ProgramFiles(x86)"));
}

void scanFilesRecursive(const fs::path& root, std::vector<Command>& out, std::unordered_set<std::wstring>& seenPaths,
                        int depth, int maxDepth, size_t maxItems)
{
    if (depth > maxDepth || out.size() >= maxItems)
    {
        return;
    }
    if (depth == 0 && !seenPaths.insert(pathKey(root)).second)
    {
        return;
    }

    std::error_code ec;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; it != end && !ec && out.size() < maxItems; it.increment(ec))
    {
        const auto path = it->path();
        const std::wstring name = path.filename().wstring();
        const std::wstring lowerName = lowerCopy(name);

        if (it->is_directory(ec))
        {
            if (!shouldSkipDirectory(lowerName))
            {
                if (!seenPaths.insert(pathKey(path)).second)
                {
                    continue;
                }
                Command command = makeCommand(CommandKind::Folder, name, L"Indexed folder", path.wstring(), 1800);
                command.data = path.wstring();
                out.push_back(std::move(command));
                scanFilesRecursive(path, out, seenPaths, depth + 1, maxDepth, maxItems);
            }
        }
        else if (it->is_regular_file(ec))
        {
            if (!seenPaths.insert(pathKey(path)).second)
            {
                continue;
            }
            Command command = makeCommand(CommandKind::File, name, L"Indexed file", path.wstring(), 1600);
            command.data = path.wstring();
            out.push_back(std::move(command));
        }
    }
}

std::wstring readyStatus()
{
    std::wstringstream status;
    status << L"Ready. " << g_staticCount.load() << L" commands";
    if (g_everythingHttpReady.load())
    {
        status << L", Everything HTTP API";
    }
    if (g_everythingReady.load())
    {
        status << L", Everything SDK";
    }
    if (!g_everythingHttpReady.load() && !g_everythingReady.load())
    {
        status << L", " << g_fileCount.load() << L" files indexed";
    }
    return status.str();
}

std::vector<Command> buildStaticCommands(const Settings& settings)
{
    const ProviderContext ctx{ settings, g_notifyWindow.load() };
    std::vector<Command> commands;
    commands.reserve(4096);

    for (const auto& entry : ProviderRegistry::instance().entries())
    {
        const size_t before = commands.size();
        entry.provider->index(ctx, commands);
        for (size_t i = before; i < commands.size(); ++i)
        {
            commands[i].provider = entry.info.id;
        }
    }
    return commands;
}

std::vector<Command> buildFallbackFileCommands(const Settings& settings)
{
    std::vector<Command> files;
    if (!settings.fallbackFileIndex)
    {
        return files;
    }

    files.reserve(static_cast<size_t>(std::min(settings.fileLimit, 50000)));

    const std::wstring profile = env(L"USERPROFILE");
    std::vector<fs::path> roots;
    std::unordered_set<std::wstring> seenRoots;
    seenRoots.reserve(32);
    if (!profile.empty())
    {
        if (settings.indexDesktop)
        {
            appendScanRoot(roots, seenRoots, profile + L"\\Desktop");
        }
        if (settings.indexDocuments)
        {
            appendScanRoot(roots, seenRoots, profile + L"\\Documents");
        }
        if (settings.indexDownloads)
        {
            appendScanRoot(roots, seenRoots, profile + L"\\Downloads");
        }
    }
    if (settings.indexDefaultPaths)
    {
        appendDefaultScanRoots(roots, seenRoots);
    }

    std::unordered_set<std::wstring> seenPaths;
    seenPaths.reserve(static_cast<size_t>(std::min(settings.fileLimit * 2, 250000)));
    for (const auto& root : roots)
    {
        std::error_code ec;
        if (fs::exists(root, ec))
        {
            scanFilesRecursive(root, files, seenPaths, 0, settings.fileDepth, static_cast<size_t>(settings.fileLimit));
        }
    }
    return files;
}
}

void setIndexNotifyWindow(HWND hwnd)
{
    g_notifyWindow.store(hwnd);
}

std::shared_mutex& indexMutex()
{
    return g_indexMutex;
}

const std::vector<Command>& staticIndexUnlocked()
{
    return g_staticIndex;
}

const std::vector<Command>& fileIndexUnlocked()
{
    return g_fileIndex;
}

bool everythingReady()
{
    return g_everythingReady.load();
}

bool everythingHttpReady()
{
    return g_everythingHttpReady.load();
}

void setEverythingReady(bool ready)
{
    g_everythingReady.store(ready);
}

void setEverythingHttpReady(bool ready)
{
    g_everythingHttpReady.store(ready);
}

bool fileIndexing()
{
    return g_fileIndexing.load();
}

int staticCommandCount()
{
    return g_staticCount.load();
}

int indexedFileCount()
{
    return g_fileCount.load();
}

EverythingSdkClient& everythingClient()
{
    return g_everything;
}

EverythingHttpClient& everythingHttpClient()
{
    return g_everythingHttp;
}

void setStatus(const std::wstring& value)
{
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_statusBase = value;
        g_statusTransient.clear();
    }
    notifyIndexChanged();
}

void setProviderStatus(const std::wstring& providerId, const std::wstring& value)
{
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_statusProviderId = providerId;
        g_statusProvider = value.empty() ? L"" : providerId + L"  |  " + value;
    }
    notifyIndexChanged();
}

void clearProviderStatus(const std::wstring& providerId)
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (providerId.empty() || providerId == g_statusProviderId)
    {
        g_statusProviderId.clear();
        g_statusProvider.clear();
    }
}

void setTransientStatus(const std::wstring& value)
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_statusTransient = value;
}

void clearTransientStatus()
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_statusTransient.clear();
}

std::wstring getStatus()
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (!g_statusTransient.empty())
    {
        return g_statusTransient;
    }
    return g_statusProvider.empty() ? g_statusBase : g_statusProvider;
}

std::wstring getBaseStatus()
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_statusBase;
}

void buildFileIndexAsync()
{
    if (g_fileIndexing.exchange(true))
    {
        return;
    }

    std::thread([] {
        const Settings settings = getSettingsSnapshot();
        if (!settings.fallbackFileIndex)
        {
            {
                std::unique_lock<std::shared_mutex> lock(g_indexMutex);
                g_fileIndex.clear();
                g_fileCount = 0;
            }
            g_fileIndexing = false;
            setStatus(L"Fallback file index disabled");
            return;
        }

        std::vector<Command> files;
        files.reserve(static_cast<size_t>(std::min(settings.fileLimit, 50000)));

        const std::wstring profile = env(L"USERPROFILE");
        std::vector<fs::path> roots;
        std::unordered_set<std::wstring> seenRoots;
        seenRoots.reserve(32);
        if (!profile.empty())
        {
            if (settings.indexDesktop)
            {
                appendScanRoot(roots, seenRoots, profile + L"\\Desktop");
            }
            if (settings.indexDocuments)
            {
                appendScanRoot(roots, seenRoots, profile + L"\\Documents");
            }
            if (settings.indexDownloads)
            {
                appendScanRoot(roots, seenRoots, profile + L"\\Downloads");
            }
        }
        if (settings.indexDefaultPaths)
        {
            appendDefaultScanRoots(roots, seenRoots);
        }

        std::unordered_set<std::wstring> seenPaths;
        seenPaths.reserve(static_cast<size_t>(std::min(settings.fileLimit * 2, 250000)));
        for (const auto& root : roots)
        {
            std::error_code ec;
            if (fs::exists(root, ec))
            {
                scanFilesRecursive(root, files, seenPaths, 0, settings.fileDepth, static_cast<size_t>(settings.fileLimit));
            }
        }

        {
            std::unique_lock<std::shared_mutex> lock(g_indexMutex);
            g_fileIndex = std::move(files);
            g_fileCount = static_cast<int>(g_fileIndex.size());
        }

        g_fileIndexing = false;
        setStatus(readyStatus());
    }).detach();
}

void rebuildIndexAsync()
{
    if (g_rebuilding.exchange(true))
    {
        return;
    }

    setStatus(L"Indexing apps and API commands...");

    std::thread([] {
        const Settings settings = getSettingsSnapshot();
        const ProviderContext ctx{ settings, g_notifyWindow.load() };

        std::vector<Command> commands;
        commands.reserve(4096);

        // Every index-time provider contributes to one shared candidate list, and
        // each entry is stamped with its owner so execute() can route back.
        for (const auto& entry : ProviderRegistry::instance().entries())
        {
            const size_t before = commands.size();
            entry.provider->index(ctx, commands);
            const wchar_t* id = entry.info.id;
            for (size_t i = before; i < commands.size(); ++i)
            {
                commands[i].provider = id;
            }
        }

        const bool everythingSdk = settings.useEverything && g_everything.load();
        // An empty search is the cheapest probe for "is the HTTP server answering".
        const bool everythingHttp = settings.useEverythingHttp &&
            g_everythingHttp.search(everythingHttpSettingsFrom(settings), L"", 1).ok;
        g_everythingReady = everythingSdk;
        g_everythingHttpReady = everythingHttp;

        {
            std::unique_lock<std::shared_mutex> lock(g_indexMutex);
            g_staticIndex = std::move(commands);
            g_staticCount = static_cast<int>(g_staticIndex.size());
        }

        g_rebuilding = false;
        if (everythingSdk || everythingHttp)
        {
            setStatus(readyStatus());
        }
        else if (settings.fallbackFileIndex)
        {
            setStatus(L"Everything API unavailable. Building fallback file index...");
            buildFileIndexAsync();
        }
        else
        {
            {
                std::unique_lock<std::shared_mutex> lock(g_indexMutex);
                g_fileIndex.clear();
                g_fileCount = 0;
            }
            std::wstringstream status;
            status << L"Ready. " << g_staticCount.load() << L" commands, file search disabled";
            setStatus(status.str());
        }
    }).detach();
}

void rebuildIndexBlocking(bool includeFallbackFileIndex)
{
    bool expected = false;
    while (!g_rebuilding.compare_exchange_weak(expected, true))
    {
        expected = false;
        Sleep(5);
    }

    setStatus(L"Benchmark indexing commands...");

    const Settings settings = getSettingsSnapshot();
    std::vector<Command> commands = buildStaticCommands(settings);

    const bool everythingSdk = settings.useEverything && g_everything.load();
    const bool everythingHttp = settings.useEverythingHttp &&
        g_everythingHttp.search(everythingHttpSettingsFrom(settings), L"", 1).ok;
    g_everythingReady = everythingSdk;
    g_everythingHttpReady = everythingHttp;

    {
        std::unique_lock<std::shared_mutex> lock(g_indexMutex);
        g_staticIndex = std::move(commands);
        g_staticCount = static_cast<int>(g_staticIndex.size());
    }

    if (includeFallbackFileIndex && !everythingSdk && !everythingHttp && settings.fallbackFileIndex)
    {
        g_fileIndexing = true;
        std::vector<Command> files = buildFallbackFileCommands(settings);
        {
            std::unique_lock<std::shared_mutex> lock(g_indexMutex);
            g_fileIndex = std::move(files);
            g_fileCount = static_cast<int>(g_fileIndex.size());
        }
        g_fileIndexing = false;
    }
    else if (!includeFallbackFileIndex)
    {
        std::unique_lock<std::shared_mutex> lock(g_indexMutex);
        g_fileIndex.clear();
        g_fileCount = 0;
    }

    g_rebuilding = false;
    setStatus(readyStatus());
}
