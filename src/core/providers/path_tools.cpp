#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
class PathToolsProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"path-tools";
        info.title = L"PATH tools";
        return info;
    }

    void index(const ProviderContext& ctx, std::vector<Command>& out) override
    {
        if (!ctx.settings.indexPathTools)
        {
            return;
        }

        std::unordered_set<std::wstring> seen;
        seen.reserve(4096);

        std::wstringstream stream(env(L"Path"));
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
                // Dedup by file name: the first hit on PATH is the one that runs.
                if (!seen.insert(lowerCopy(name)).second)
                {
                    continue;
                }
                out.push_back(makeCommand(CommandKind::PathTool, stripExtension(name), L"PATH tool", entry.path().wstring(), 3100));
            }
        }
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"PATH tools"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open PATH tool search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for PATH tool search", info().id));
        out.push_back(makeSettingItem(SettingField::IndexPathTools, SettingKind::Toggle,
                                      L"Index PATH", L"Add executables, cmd files, and bat files from PATH"));
    }
};
}

std::unique_ptr<Provider> makePathToolsProvider()
{
    return std::make_unique<PathToolsProvider>();
}
