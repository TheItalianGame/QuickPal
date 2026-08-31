#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
constexpr ULONGLONG kWindowRefreshIntervalMs = 120;

struct WindowEntry
{
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring processName;
    std::wstring searchText;
    int zOrder = 0;
};

struct EnumContext
{
    HWND self;
    std::vector<WindowEntry>* entries;
    std::unordered_map<DWORD, std::wstring> processNames;
    int zOrder = 0;
};

std::wstring processNameForWindow(EnumContext& ctx, HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
    {
        return {};
    }

    if (const auto it = ctx.processNames.find(pid); it != ctx.processNames.end())
    {
        return it->second;
    }

    std::wstring name;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
    {
        wchar_t path[1024]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0)
        {
            name = fileNameFromPath(std::wstring(path, length));
        }
        CloseHandle(process);
    }

    ctx.processNames[pid] = name;
    return name;
}

BOOL CALLBACK enumProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);
    if (!IsWindowVisible(hwnd) || hwnd == ctx->self)
    {
        return TRUE;
    }
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
    {
        return TRUE;
    }

    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0)
    {
        return TRUE;
    }
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);
    while (!title.empty() && title.back() == L'\0')
    {
        title.pop_back();
    }
    if (title.empty())
    {
        return TRUE;
    }

    const std::wstring processName = processNameForWindow(*ctx, hwnd);
    WindowEntry entry;
    entry.hwnd = hwnd;
    entry.title = std::move(title);
    entry.processName = processName;
    entry.zOrder = ctx->zOrder++;
    entry.searchText = lowerCopy(entry.title);
    if (!processName.empty())
    {
        entry.searchText += L" " + lowerCopy(processName) + L" " + lowerCopy(stripExtension(processName));
    }
    ctx->entries->push_back(std::move(entry));
    return TRUE;
}

std::vector<WindowEntry> enumerateWindows(HWND self)
{
    std::vector<WindowEntry> entries;
    entries.reserve(64);

    EnumContext enumCtx{ self, &entries };
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&enumCtx));
    return entries;
}

class WindowSnapshotCache
{
public:
    ~WindowSnapshotCache()
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

    std::vector<WindowEntry> snapshot(HWND self)
    {
        ensureWorker();

        const ULONGLONG now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requestedSelf_ = self;
            if (!refreshPending_ && now >= nextRefreshMs_)
            {
                refreshPending_ = true;
                nextRefreshMs_ = now + kWindowRefreshIntervalMs;
                cv_.notify_one();
            }
            return snapshot_;
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
            HWND self = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || refreshPending_; });
                if (stop_)
                {
                    break;
                }
                self = requestedSelf_;
                refreshPending_ = false;
            }

            std::vector<WindowEntry> fresh = enumerateWindows(self);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_ = std::move(fresh);
            }

            if (self)
            {
                PostMessageW(self, kAsyncProviderUpdatedMessage, 0, 0);
            }
        }
    }

    std::atomic_bool started_{ false };
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool refreshPending_ = false;
    HWND requestedSelf_ = nullptr;
    ULONGLONG nextRefreshMs_ = 0;
    std::vector<WindowEntry> snapshot_;
};

class WindowsProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"windows";
        info.title = L"Open windows";
        info.prefixes = { L"win", L"window" };
        info.mode = QueryMode::Windows;
        info.exclusive = true;
        info.runsUnprefixed = true;
        return info;
    }

    void query(const ProviderContext& ctx, const Query& q, ResultSink& sink) override
    {
        const auto terms = q.subjectTerms();
        if (!q.hasPrefix() && terms.empty())
        {
            return;
        }

        const std::vector<WindowEntry> windows = cache_.snapshot(ctx.window);
        if (q.hasPrefix() && windows.empty())
        {
            sink.add(makeCommand(CommandKind::Window, L"Window list is warming up",
                                 L"Live windows will appear in a moment", L"", 0), 6000);
            return;
        }

        int rank = 0;
        for (const WindowEntry& window : windows)
        {
            if (!IsWindow(window.hwnd) || !IsWindowVisible(window.hwnd) || window.hwnd == ctx.window)
            {
                continue;
            }

            std::wstring subtitle = L"Open window";
            if (!window.processName.empty())
            {
                subtitle += L" - " + window.processName;
            }

            Command command = makeCommand(CommandKind::Window, window.title, subtitle, window.title,
                                          5600 - std::min(rank, 500));
            command.searchText = window.searchText;
            command.hwnd = window.hwnd;

            const int score = terms.empty() ? command.weight : scoreCommandTerms(terms, command);
            if (score >= 0)
            {
                // EnumWindows is z-ordered, so earlier cached entries are the
                // closest thing to "most recent" without tracking foreground hooks.
                const int boost = q.hasPrefix() ? 22000 : 19000;
                sink.add(std::move(command), score + boost - std::min(rank, 500));
            }
            ++rank;
        }
    }

    bool execute(const ProviderContext&, const Command& command) override
    {
        if (command.kind != CommandKind::Window || !IsWindow(command.hwnd))
        {
            return false;
        }
        if (IsIconic(command.hwnd))
        {
            ShowWindow(command.hwnd, SW_RESTORE);
        }
        SetForegroundWindow(command.hwnd);
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Open windows"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open window switcher directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for window-only search", info().id));
    }

private:
    WindowSnapshotCache cache_;
};
}

std::unique_ptr<Provider> makeWindowsProvider()
{
    return std::make_unique<WindowsProvider>();
}
