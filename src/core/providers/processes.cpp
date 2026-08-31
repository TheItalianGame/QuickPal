#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
constexpr ULONGLONG kProcessRefreshIntervalMs = 750;

struct ProcessEntry
{
    std::wstring name;
    std::wstring searchText;
    std::wstring memoryText;
    DWORD pid = 0;
    int rank = 0;
};

std::wstring formatMemory(SIZE_T bytes)
{
    if (bytes == 0)
    {
        return L"memory unknown";
    }
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::wstringstream out;
    out.setf(std::ios::fixed, std::ios::floatfield);
    out.precision(mb < 100.0 ? 1 : 0);
    out << mb << L" MB";
    return out.str();
}

SIZE_T processMemory(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
    {
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }
    if (!process)
    {
        return 0;
    }

    PROCESS_MEMORY_COUNTERS counters{};
    SIZE_T result = 0;
    if (GetProcessMemoryInfo(process, &counters, sizeof(counters)))
    {
        result = counters.WorkingSetSize;
    }
    CloseHandle(process);
    return result;
}

std::vector<ProcessEntry> enumerateProcesses()
{
    std::vector<ProcessEntry> entries;
    entries.reserve(256);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return entries;
    }

    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (!Process32FirstW(snapshot, &process))
    {
        CloseHandle(snapshot);
        return entries;
    }

    int rank = 0;
    do
    {
        if (process.th32ProcessID == 0 || process.th32ProcessID == GetCurrentProcessId())
        {
            continue;
        }

        ProcessEntry entry;
        entry.name = process.szExeFile;
        entry.searchText = lowerCopy(entry.name) + L" " + lowerCopy(stripExtension(entry.name));
        entry.memoryText = formatMemory(processMemory(process.th32ProcessID));
        entry.pid = process.th32ProcessID;
        entry.rank = rank++;
        entries.push_back(std::move(entry));
    } while (Process32NextW(snapshot, &process));

    CloseHandle(snapshot);
    return entries;
}

class ProcessSnapshotCache
{
public:
    ~ProcessSnapshotCache()
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

    std::vector<ProcessEntry> snapshot(HWND notify)
    {
        ensureWorker();

        const ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            notify_ = notify;
            if (!refreshPending_ && now >= nextRefreshMs_)
            {
                refreshPending_ = true;
                nextRefreshMs_ = now + kProcessRefreshIntervalMs;
                cv_.notify_one();
            }
            return snapshot_;
        }
    }

    void refreshSoon(HWND notify)
    {
        ensureWorker();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            notify_ = notify;
            refreshPending_ = true;
            nextRefreshMs_ = 0;
        }
        cv_.notify_one();
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
            HWND notify = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || refreshPending_; });
                if (stop_)
                {
                    break;
                }
                notify = notify_;
                refreshPending_ = false;
            }

            std::vector<ProcessEntry> fresh = enumerateProcesses();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_ = std::move(fresh);
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
    bool refreshPending_ = false;
    HWND notify_ = nullptr;
    ULONGLONG nextRefreshMs_ = 0;
    std::vector<ProcessEntry> snapshot_;
};

class ProcessesProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"processes";
        info.title = L"Processes";
        info.prefixes = { L"kill", L"proc", L"process" };
        info.mode = QueryMode::Processes;
        info.exclusive = true;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        const auto entries = cache_.snapshot(ctx.window);
        if (q.hasPrefix() && entries.empty())
        {
            sink.add(makeCommand(CommandKind::Process, L"Process list is warming up",
                                 L"Running processes will appear in a moment", L"", 0), 6000);
            return;
        }

        const auto terms = q.subjectTerms();
        struct Candidate
        {
            std::wstring name;
            DWORD pid = 0;
            int score = 0;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(256);

        for (const auto& entry : entries)
        {
            if (entry.pid == 0)
            {
                continue;
            }

            Command command = makeCommand(CommandKind::Process, entry.name, L"", entry.name,
                                          4200 - std::min(entry.rank, 500));
            command.searchText = entry.searchText;
            const int base = terms.empty() ? command.weight : scoreCommandTerms(terms, command);
            if (base >= 0)
            {
                candidates.push_back(Candidate{ entry.name, entry.pid, base + 12000 - std::min(entry.rank, 500) });
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });

        const int count = std::min<int>(sink.limit(), static_cast<int>(candidates.size()));
        for (int i = 0; i < count; ++i)
        {
            const Candidate& candidate = candidates[static_cast<size_t>(i)];
            std::wstringstream subtitle;
            const auto memory = std::find_if(entries.begin(), entries.end(), [&](const ProcessEntry& entry) {
                return entry.pid == candidate.pid;
            });
            subtitle << L"PID " << candidate.pid << L" - "
                     << (memory == entries.end() ? L"memory unknown" : memory->memoryText);
            Command command = makeCommand(CommandKind::Process, candidate.name, subtitle.str(), candidate.name, 4200 - i);
            command.processId = candidate.pid;
            sink.add(std::move(command), candidate.score);
        }
    }

    bool execute(const ProviderContext& ctx, const Command& command) override
    {
        if (command.kind != CommandKind::Process || command.processId == 0)
        {
            return false;
        }

        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, command.processId);
        if (!process)
        {
            return true;
        }
        TerminateProcess(process, 1);
        CloseHandle(process);
        cache_.refreshSoon(ctx.window);
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Processes"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open process search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for process search", info().id));
    }

private:
    ProcessSnapshotCache cache_;
};
}

std::unique_ptr<Provider> makeProcessesProvider()
{
    return std::make_unique<ProcessesProvider>();
}
