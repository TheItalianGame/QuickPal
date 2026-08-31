#include "providers.h"

#include "../indexer.h"
#include "../settings.h"
#include "../util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
Command makeFileCommand(const FileResultEntry& entry, int weight)
{
    const bool folder = entry.hasType && entry.isFolder;
    Command command = makeCommand(folder ? CommandKind::Folder : CommandKind::File,
                                  fileNameFromPath(entry.path), fileEntrySubtitle(entry), entry.path, weight);
    command.data = entry.path;
    return command;
}

bool isPathQuery(const std::wstring& query)
{
    if (query.size() >= 3 && std::iswalpha(query[0]) && query[1] == L':' &&
        (query[2] == L'\\' || query[2] == L'/'))
    {
        return true;
    }
    return startsWith(query, L"\\\\") || startsWith(query, L"%") || startsWith(query, L"~") ||
           query.find(L"\\") != std::wstring::npos || query.find(L"/") != std::wstring::npos;
}

std::wstring expandPathQuery(std::wstring query)
{
    if (startsWith(query, L"~"))
    {
        query = env(L"USERPROFILE") + query.substr(1);
    }
    return expandEnv(query);
}

struct PathBrowseEntry
{
    FileResultEntry file;
    std::wstring name;
    std::wstring lowerName;
    bool folder = false;
};

struct PathBrowseSnapshot
{
    bool handled = false;
    bool pending = false;
    bool ready = false;
    std::wstring directory;
    std::vector<FileResultEntry> entries;
};

bool resolvePathBrowseQuery(const std::wstring& query, fs::path& directory, std::wstring& filter)
{
    if (!isPathQuery(query))
    {
        return false;
    }

    std::error_code ec;
    fs::path path(expandPathQuery(query));
    if (fs::exists(path, ec) && fs::is_directory(path, ec))
    {
        directory = path;
    }
    else
    {
        directory = path.parent_path();
        filter = lowerCopy(path.filename().wstring());
    }

    return true;
}

std::vector<PathBrowseEntry> enumeratePathBrowseDirectory(const std::wstring& directory)
{
    std::vector<PathBrowseEntry> entries;
    entries.reserve(256);

    std::error_code ec;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; it != end && !ec; it.increment(ec))
    {
        const std::wstring path = it->path().wstring();
        FileResultEntry file = fileEntryFromPath(path);
        std::wstring name = fileNameFromPath(path);

        PathBrowseEntry entry;
        entry.folder = file.hasType && file.isFolder;
        entry.file = std::move(file);
        entry.name = std::move(name);
        entry.lowerName = lowerCopy(entry.name);
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const PathBrowseEntry& a, const PathBrowseEntry& b) {
        if (a.folder != b.folder)
        {
            return a.folder;
        }
        return a.lowerName < b.lowerName;
    });
    return entries;
}

std::vector<FileResultEntry> selectPathBrowseEntries(const std::vector<PathBrowseEntry>& entries,
                                                     const std::wstring& filter,
                                                     int limit)
{
    std::vector<FileResultEntry> selected;
    selected.reserve(static_cast<size_t>(limit));
    for (const auto& entry : entries)
    {
        if (!filter.empty() && !startsWith(entry.lowerName, filter))
        {
            continue;
        }
        selected.push_back(entry.file);
        if (static_cast<int>(selected.size()) >= limit)
        {
            break;
        }
    }
    return selected;
}

class PathBrowseCache
{
public:
    ~PathBrowseCache()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    PathBrowseSnapshot query(const ProviderContext& ctx, const std::wstring& text, int limit)
    {
        fs::path directoryPath;
        std::wstring filter;
        PathBrowseSnapshot snapshot;
        if (!resolvePathBrowseQuery(text, directoryPath, filter))
        {
            return snapshot;
        }

        snapshot.handled = true;
        std::error_code ec;
        if (directoryPath.empty() || !fs::exists(directoryPath, ec) || !fs::is_directory(directoryPath, ec))
        {
            snapshot.ready = true;
            return snapshot;
        }

        ensureWorker();

        const std::wstring directory = directoryPath.wstring();
        const std::wstring key = lowerCopy(directory);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.directory = directory;
            if (key == completedKey_)
            {
                snapshot.ready = true;
                snapshot.entries = selectPathBrowseEntries(completed_, filter, limit);
                return snapshot;
            }

            if (key != requestedKey_)
            {
                requestedKey_ = key;
                requestedDirectory_ = directory;
                notify_ = ctx.window;
                ++generation_;
                cv_.notify_one();
            }

            snapshot.pending = true;
            return snapshot;
        }
    }

private:
    void ensureWorker()
    {
        bool expected = false;
        if (started_.compare_exchange_strong(expected, true))
        {
            worker_ = std::thread([this] { workerLoop(); });
        }
    }

    void workerLoop()
    {
        for (;;)
        {
            uint64_t generation = 0;
            std::wstring directory;
            HWND notify = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || generation_ != runningGeneration_; });
                if (stop_)
                {
                    break;
                }
                runningGeneration_ = generation_;
                generation = runningGeneration_;
                directory = requestedDirectory_;
                notify = notify_;
            }

            std::vector<PathBrowseEntry> fresh = enumeratePathBrowseDirectory(directory);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_ || generation != generation_)
                {
                    continue;
                }
                completedKey_ = requestedKey_;
                completed_ = std::move(fresh);
            }
            if (notify)
            {
                PostMessageW(notify, kAsyncProviderUpdatedMessage, 0, 0);
            }
        }
    }

    std::atomic_bool started_{ false };
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    uint64_t generation_ = 0;
    uint64_t runningGeneration_ = 0;
    std::wstring requestedKey_;
    std::wstring requestedDirectory_;
    HWND notify_ = nullptr;
    std::wstring completedKey_;
    std::vector<PathBrowseEntry> completed_;
};

struct AsyncSnapshot
{
    bool pending = false;
    bool ready = false;
    std::vector<FileResultEntry> entries;
};

class AsyncEverythingSearch
{
public:
    ~AsyncEverythingSearch()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    AsyncSnapshot query(const ProviderContext& ctx, const std::wstring& text, int limit)
    {
        ensureWorker();

        const std::wstring lower = lowerCopy(text);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lower == completedLower_)
            {
                AsyncSnapshot snapshot;
                snapshot.ready = true;
                snapshot.entries = completed_;
                return snapshot;
            }

            if (lower != requestedLower_ || limit != requestedLimit_)
            {
                requestedLower_ = lower;
                requestedText_ = text;
                requestedSettings_ = ctx.settings;
                requestedLimit_ = limit;
                notify_ = ctx.window;
                ++generation_;
                pending_ = true;
                cv_.notify_one();
            }

            AsyncSnapshot snapshot;
            snapshot.pending = pending_;
            return snapshot;
        }
    }

private:
    struct Request
    {
        uint64_t generation = 0;
        std::wstring lower;
        std::wstring text;
        Settings settings;
        int limit = 0;
        HWND notify = nullptr;
    };

    void ensureWorker()
    {
        bool expected = false;
        if (started_.compare_exchange_strong(expected, true))
        {
            worker_ = std::thread([this] { workerLoop(); });
        }
    }

    void workerLoop()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        for (;;)
        {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || generation_ != runningGeneration_; });
                if (stop_)
                {
                    break;
                }
                runningGeneration_ = generation_;
                request = Request{ runningGeneration_, requestedLower_, requestedText_, requestedSettings_, requestedLimit_, notify_ };
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(45));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_ || request.generation != generation_)
                {
                    continue;
                }
            }

            std::vector<FileResultEntry> entries;
            bool ok = false;
            if (request.settings.useEverythingHttp && everythingHttpReady())
            {
                const auto reply = everythingHttpClient().search(
                    everythingHttpSettingsFrom(request.settings), request.text, request.limit);
                if (reply.ok)
                {
                    entries = reply.entries;
                    ok = true;
                }
                else
                {
                    setEverythingHttpReady(false);
                }
            }

            if (!ok && everythingReady())
            {
                entries = everythingClient().search(request.text, static_cast<DWORD>(request.limit));
                ok = true;
            }

            HWND notify = nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stop_ && request.generation == generation_)
                {
                    completedLower_ = request.lower;
                    completed_ = std::move(entries);
                    pending_ = false;
                    notify = request.notify;
                }
            }
            if (notify)
            {
                PostMessageW(notify, kAsyncProviderUpdatedMessage, 0, 0);
            }
        }

        CoUninitialize();
    }

    std::atomic_bool started_{ false };
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool pending_ = false;
    uint64_t generation_ = 0;
    uint64_t runningGeneration_ = 0;
    std::wstring requestedLower_;
    std::wstring requestedText_;
    Settings requestedSettings_;
    int requestedLimit_ = 0;
    HWND notify_ = nullptr;
    std::wstring completedLower_;
    std::vector<FileResultEntry> completed_;
};

// Three backends, best first: Everything's HTTP server, then its SDK, then our own
// bounded background walk.
class FilesProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"files";
        info.title = L"Files";
        info.prefixes = { L"f", L"file" };
        info.mode = QueryMode::Files;
        info.exclusive = true;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        const std::wstring& fileQuery = q.subject();
        if (fileQuery.empty())
        {
            sink.add(makeCommand(CommandKind::File, L"Search files", L"Type a file name after f", L"", 4200), 7000);
            return;
        }

        const PathBrowseSnapshot browse = pathBrowse_.query(ctx, fileQuery, sink.limit());
        if (browse.handled)
        {
            if (browse.ready)
            {
                int rank = 0;
                for (const auto& entry : browse.entries)
                {
                    sink.add(makeFileCommand(entry, 7200 - rank), 18000 - rank);
                    ++rank;
                }
                return;
            }
            if (browse.pending)
            {
                sink.add(makeCommand(CommandKind::Folder, L"Opening folder", browse.directory, browse.directory, 0), 8500);
                return;
            }
            return;
        }

        if ((ctx.settings.useEverythingHttp && everythingHttpReady()) || everythingReady())
        {
            const AsyncSnapshot snapshot = async_.query(ctx, fileQuery, sink.limit());
            if (snapshot.ready)
            {
                int rank = 0;
                for (const auto& entry : snapshot.entries)
                {
                    sink.add(makeFileCommand(entry, 6600 - rank), 16000 - rank);
                    ++rank;
                }
                return;
            }
            if (snapshot.pending)
            {
                sink.add(makeCommand(CommandKind::File, L"Searching Everything", fileQuery, L"", 0), 8000);
                return;
            }
        }

        const auto terms = q.subjectTerms();
        {
            std::shared_lock<std::shared_mutex> lock(indexMutex());
            for (const auto& command : fileIndexUnlocked())
            {
                sink.addScored(command, terms);
            }
        }

        if (fileIndexing())
        {
            sink.add(makeCommand(CommandKind::File, L"File index is warming up",
                                 L"Results improve as the background index completes", L"", 0), 6000);
        }
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Files / Everything"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open file search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for file search", info().id));
        out.push_back(makeSettingItem(SettingField::UseEverythingHttp, SettingKind::Toggle,
                                      L"Everything HTTP API", L"Use Everything's local HTTP server first"));
        out.push_back(makeSettingItem(SettingField::EverythingHttpPort, SettingKind::Stepper,
                                      L"Everything HTTP port", L"Port from Tools > Options > HTTP Server"));
        out.push_back(makeSettingItem(SettingField::UseEverything, SettingKind::Toggle,
                                      L"Everything SDK", L"Use Everything64.dll when HTTP is not available"));
        out.push_back(makeSettingItem(SettingField::FallbackFileIndex, SettingKind::Toggle,
                                      L"Fallback index", L"Scan selected local folders if Everything is unavailable"));
        out.push_back(makeSettingItem(SettingField::IndexDefaultPaths, SettingKind::Toggle,
                                      L"Default paths", L"Include profile, OneDrive, public folders, apps, and program files"));
        out.push_back(makeSettingItem(SettingField::IndexDesktop, SettingKind::Toggle,
                                      L"Desktop", L"Include your Desktop in the fallback index"));
        out.push_back(makeSettingItem(SettingField::IndexDocuments, SettingKind::Toggle,
                                      L"Documents", L"Include your Documents in the fallback index"));
        out.push_back(makeSettingItem(SettingField::IndexDownloads, SettingKind::Toggle,
                                      L"Downloads", L"Include your Downloads in the fallback index"));
        out.push_back(makeSettingItem(SettingField::FileDepth, SettingKind::Stepper,
                                      L"Fallback depth", L"Maximum folder depth for local scans"));
        out.push_back(makeSettingItem(SettingField::FileLimit, SettingKind::Stepper,
                                      L"Fallback file cap", L"Maximum entries kept in the fallback index"));
    }

private:
    PathBrowseCache pathBrowse_;
    AsyncEverythingSearch async_;
};
}

std::unique_ptr<Provider> makeFilesProvider()
{
    return std::make_unique<FilesProvider>();
}
