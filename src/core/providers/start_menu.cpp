#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
void scanShortcuts(std::vector<Command>& out, std::unordered_set<std::wstring>& seen, const std::wstring& root)
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
        if (!seen.insert(lowerCopy(path)).second)
        {
            continue;
        }
        out.push_back(makeCommand(CommandKind::App, stripExtension(fileNameFromPath(path)), L"Start Menu app", path, 3800));
    }
}

class StartMenuProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"start-menu";
        info.title = L"Start Menu apps";
        return info;
    }

    void index(const ProviderContext& ctx, std::vector<Command>& out) override
    {
        if (!ctx.settings.indexStartMenu)
        {
            return;
        }

        std::unordered_set<std::wstring> seen;
        seen.reserve(2048);
        scanShortcuts(out, seen, expandEnv(L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs"));
        scanShortcuts(out, seen, expandEnv(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs"));
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Start Menu apps"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open Start Menu app search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for Start Menu app search", info().id));
        out.push_back(makeSettingItem(SettingField::IndexStartMenu, SettingKind::Toggle,
                                      L"Index apps", L"Scan Start Menu shortcuts and Store apps"));
    }
};
}

std::unique_ptr<Provider> makeStartMenuProvider()
{
    return std::make_unique<StartMenuProvider>();
}
