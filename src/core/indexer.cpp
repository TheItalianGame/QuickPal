#include "indexer.h"

#include "settings.h"
#include "util.h"

#include <shlobj.h>

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

void addBuiltins(std::vector<Command>& commands)
{
    commands.push_back(makeCommand(CommandKind::ReloadIndex, L"Reload indexes", L"Refresh apps, PATH tools, and file index", L"", 5200));
    commands.push_back(makeCommand(CommandKind::OpenSettings, L"QuickPal settings", L"Native settings UI", L"", 5150));
    commands.push_back(makeCommand(CommandKind::OpenCalculator, L"Calculator", L"Built-in local calculator", L"=", 5125));
    commands.push_back(makeCommand(CommandKind::ExitApp, L"Exit QuickPal", L"Quit the background hotkey listener", L"", 3000));
    commands.push_back(makeCommand(CommandKind::Builtin, L"Open Terminal", L"Windows Terminal", L"wt.exe", 5000));
    commands.push_back(makeCommand(CommandKind::Builtin, L"Open PowerShell", L"PowerShell", L"powershell.exe", 4900));
    commands.push_back(makeCommand(CommandKind::Builtin, L"Open Command Prompt", L"cmd.exe", L"cmd.exe", 4800));
    commands.push_back(makeCommand(CommandKind::Builtin, L"Open File Explorer", L"Explorer", L"explorer.exe", 4700));
    commands.push_back(makeCommand(CommandKind::Builtin, L"Open Task Manager", L"System monitor", L"taskmgr.exe", 4600));
    commands.push_back(makeCommand(CommandKind::Builtin, L"Lock workstation", L"Win32 LockWorkStation API", L"lock", 4300));
    commands.push_back(makeCommand(CommandKind::Builtin, L"Open PowerToys Command Palette source", L"GitHub: microsoft/PowerToys/src/modules/cmdpal", L"https://github.com/microsoft/PowerToys/tree/main/src/modules/cmdpal", 4200));

    const std::wstring profile = env(L"USERPROFILE");
    if (!profile.empty())
    {
        commands.push_back(makeCommand(CommandKind::Folder, L"Open Desktop", L"User folder", profile + L"\\Desktop", 4200));
        commands.push_back(makeCommand(CommandKind::Folder, L"Open Documents", L"User folder", profile + L"\\Documents", 4200));
        commands.push_back(makeCommand(CommandKind::Folder, L"Open Downloads", L"User folder", profile + L"\\Downloads", 4200));
    }
}

void addSettingsUris(std::vector<Command>& commands)
{
    struct SettingUri
    {
        const wchar_t* title;
        const wchar_t* uri;
        const wchar_t* keywords;
    };

    constexpr SettingUri entries[] = {
        { L"Settings", L"ms-settings:", L"windows settings control panel preferences" },
        { L"Display settings", L"ms-settings:display", L"monitor resolution brightness scale night light" },
        { L"Sound settings", L"ms-settings:sound", L"audio volume microphone input output" },
        { L"Bluetooth settings", L"ms-settings:bluetooth", L"devices mouse keyboard headset" },
        { L"Network settings", L"ms-settings:network", L"wifi ethernet vpn internet adapter" },
        { L"Apps settings", L"ms-settings:appsfeatures", L"installed uninstall default apps" },
        { L"Startup apps", L"ms-settings:startupapps", L"startup login launch boot" },
        { L"Power and battery", L"ms-settings:powersleep", L"sleep energy battery power" },
        { L"Windows Update", L"ms-settings:windowsupdate", L"updates restart history optional" },
        { L"Privacy settings", L"ms-settings:privacy", L"permissions camera microphone location" },
        { L"Clipboard settings", L"ms-settings:clipboard", L"clipboard history paste sync" },
        { L"Developer settings", L"ms-settings:developers", L"developer mode terminal explorer sudo" },
    };

    for (const auto& entry : entries)
    {
        Command command = makeCommand(CommandKind::Setting, entry.title, L"Windows Settings API URI", entry.uri, 4400);
        command.searchText += L" ";
        command.searchText += lowerCopy(entry.keywords);
        commands.push_back(std::move(command));
    }
}

void addShortcutCommands(std::vector<Command>& commands, std::unordered_set<std::wstring>& seen, const std::wstring& root)
{
    if (root.empty())
    {
        return;
    }

    std::error_code ec;
    if (!fs::exists(root, ec))
    {
        return;
    }

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
        {
            continue;
        }
        const std::wstring path = it->path().wstring();
        const std::wstring ext = extensionLower(path);
        if (ext != L".lnk" && ext != L".url" && ext != L".appref-ms")
        {
            continue;
        }
        const std::wstring key = lowerCopy(path);
        if (!seen.insert(key).second)
        {
            continue;
        }
        commands.push_back(makeCommand(CommandKind::App, stripExtension(fileNameFromPath(path)), L"Start Menu app", path, 3800));
    }
}

void addPathTools(std::vector<Command>& commands, std::unordered_set<std::wstring>& seen)
{
    const std::wstring pathEnv = env(L"Path");
    std::wstringstream stream(pathEnv);
    std::wstring rawDir;
    while (std::getline(stream, rawDir, L';'))
    {
        const std::wstring dir = trimCopy(expandEnv(rawDir));
        if (dir.empty())
        {
            continue;
        }

        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec))
        {
            continue;
        }

        for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        {
            if (ec || !entry.is_regular_file(ec))
            {
                continue;
            }
            const std::wstring ext = extensionLower(entry.path().wstring());
            if (ext != L".exe" && ext != L".cmd" && ext != L".bat")
            {
                continue;
            }
            const std::wstring name = entry.path().filename().wstring();
            if (!seen.insert(L"path:" + lowerCopy(name)).second)
            {
                continue;
            }
            commands.push_back(makeCommand(CommandKind::PathTool, stripExtension(name), L"PATH tool", entry.path().wstring(), 3100));
        }
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

void scanFilesRecursive(const fs::path& root, std::vector<Command>& out, std::unordered_set<std::wstring>& seenPaths, int depth, int maxDepth, size_t maxItems)
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
                out.push_back(makeCommand(CommandKind::Folder, name, L"Indexed folder", path.wstring(), 1800));
                scanFilesRecursive(path, out, seenPaths, depth + 1, maxDepth, maxItems);
            }
        }
        else if (it->is_regular_file(ec))
        {
            if (!seenPaths.insert(pathKey(path)).second)
            {
                continue;
            }
            out.push_back(makeCommand(CommandKind::File, name, L"Indexed file", path.wstring(), 1600));
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
    return g_statusTransient.empty() ? g_statusBase : g_statusTransient;
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
        std::vector<Command> commands;
        commands.reserve(4096);
        std::unordered_set<std::wstring> seen;
        seen.reserve(4096);

        addBuiltins(commands);
        addSettingsUris(commands);

        const Settings settings = getSettingsSnapshot();
        if (settings.indexStartMenu)
        {
            addShortcutCommands(commands, seen, expandEnv(L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs"));
            addShortcutCommands(commands, seen, expandEnv(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs"));
        }
        if (settings.indexPathTools)
        {
            addPathTools(commands, seen);
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
