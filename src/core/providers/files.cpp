#include "providers.h"

#include "../indexer.h"
#include "../settings.h"
#include "../util.h"

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
    const DWORD attributes = GetFileAttributesW(entry.path.c_str());
    const bool folder = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
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

bool addPathBrowseResults(const std::wstring& query, ResultSink& sink)
{
    if (!isPathQuery(query))
    {
        return false;
    }

    std::error_code ec;
    fs::path path(expandPathQuery(query));
    fs::path directory;
    std::wstring filter;

    if (fs::exists(path, ec) && fs::is_directory(path, ec))
    {
        directory = path;
    }
    else
    {
        directory = path.parent_path();
        filter = lowerCopy(path.filename().wstring());
    }

    if (directory.empty() || !fs::exists(directory, ec) || !fs::is_directory(directory, ec))
    {
        return true;
    }

    struct Entry
    {
        fs::path path;
        bool folder = false;
    };
    std::vector<Entry> entries;
    entries.reserve(static_cast<size_t>(sink.limit()));

    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; it != end && !ec; it.increment(ec))
    {
        const std::wstring name = it->path().filename().wstring();
        if (!filter.empty() && !startsWith(lowerCopy(name), filter))
        {
            continue;
        }
        entries.push_back(Entry{ it->path(), it->is_directory(ec) });
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder)
        {
            return a.folder;
        }
        return lowerCopy(a.path.filename().wstring()) < lowerCopy(b.path.filename().wstring());
    });

    int rank = 0;
    for (const auto& entry : entries)
    {
        if (rank >= sink.limit())
        {
            break;
        }
        const FileResultEntry file = fileEntryFromPath(entry.path.wstring());
        Command command = makeCommand(entry.folder ? CommandKind::Folder : CommandKind::File,
                                      fileNameFromPath(file.path), fileEntrySubtitle(file), file.path, 7200 - rank);
        command.data = file.path;
        sink.add(std::move(command), 18000 - rank);
        ++rank;
    }
    return true;
}

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

        if (addPathBrowseResults(fileQuery, sink))
        {
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

private:
    AsyncEverythingSearch async_;
};
}

std::unique_ptr<Provider> makeFilesProvider()
{
    return std::make_unique<FilesProvider>();
}
