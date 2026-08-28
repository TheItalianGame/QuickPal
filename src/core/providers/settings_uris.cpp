#include "providers.h"

#include "../settings.h"
#include "../util.h"

namespace
{
struct SettingUri
{
    const wchar_t* title;
    const wchar_t* uri;
    const wchar_t* keywords;
};

constexpr SettingUri kEntries[] = {
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

class SettingsUriProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"settings-uris";
        info.title = L"Windows settings";
        return info;
    }

    void index(const ProviderContext&, std::vector<Command>& out) override
    {
        for (const auto& entry : kEntries)
        {
            Command command = makeCommand(CommandKind::Setting, entry.title, L"Windows Settings API URI", entry.uri, 4400);
            // Keywords widen matching without showing up in the visible subtitle.
            command.searchText += L" ";
            command.searchText += lowerCopy(entry.keywords);
            out.push_back(std::move(command));
        }
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Windows settings"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open Windows settings search directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for Windows settings search", info().id));
    }
};
}

std::unique_ptr<Provider> makeSettingsUriProvider()
{
    return std::make_unique<SettingsUriProvider>();
}
