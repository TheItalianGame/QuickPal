#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <sstream>
#include <vector>

namespace
{
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

    void query(const ProviderContext&, const Query& q, ResultSink& sink) override
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot, &entry))
        {
            CloseHandle(snapshot);
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

        int rank = 0;
        do
        {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == GetCurrentProcessId())
            {
                continue;
            }

            Command command = makeCommand(CommandKind::Process, entry.szExeFile, L"", entry.szExeFile, 4200 - rank);
            const int score = terms.empty() ? 12000 - rank : scoreCommandTerms(terms, command) + 12000;
            if (score >= 0)
            {
                candidates.push_back(Candidate{ entry.szExeFile, entry.th32ProcessID, score });
            }
            ++rank;
        } while (Process32NextW(snapshot, &entry));

        CloseHandle(snapshot);

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });

        const int count = std::min<int>(sink.limit(), static_cast<int>(candidates.size()));
        for (int i = 0; i < count; ++i)
        {
            const Candidate& candidate = candidates[static_cast<size_t>(i)];
            std::wstringstream subtitle;
            subtitle << L"PID " << candidate.pid << L" - " << formatMemory(processMemory(candidate.pid));
            Command command = makeCommand(CommandKind::Process, candidate.name, subtitle.str(), candidate.name, 4200 - i);
            command.processId = candidate.pid;
            sink.add(std::move(command), candidate.score);
        }
    }

    bool execute(const ProviderContext&, const Command& command) override
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
};
}

std::unique_ptr<Provider> makeProcessesProvider()
{
    return std::make_unique<ProcessesProvider>();
}
