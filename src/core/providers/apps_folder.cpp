#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <unordered_set>

namespace
{
std::wstring displayName(IShellFolder* folder, PCUITEMID_CHILD child, SHGDNF flags)
{
    STRRET value{};
    if (!folder || FAILED(folder->GetDisplayNameOf(child, flags, &value)))
    {
        return {};
    }
    wchar_t buffer[2048]{};
    if (FAILED(StrRetToBufW(&value, child, buffer, static_cast<UINT>(std::size(buffer)))))
    {
        return {};
    }
    return buffer;
}

class AppsFolderProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"apps-folder";
        info.title = L"Store apps";
        return info;
    }

    void index(const ProviderContext& ctx, std::vector<Command>& out) override
    {
        if (!ctx.settings.indexStartMenu)
        {
            return;
        }

        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool uninit = SUCCEEDED(init);

        PIDLIST_ABSOLUTE appsPidl = nullptr;
        if (FAILED(SHParseDisplayName(L"shell:AppsFolder", nullptr, &appsPidl, 0, nullptr)) || !appsPidl)
        {
            if (uninit)
            {
                CoUninitialize();
            }
            return;
        }

        IShellFolder* desktop = nullptr;
        IShellFolder* apps = nullptr;
        IEnumIDList* items = nullptr;

        if (SUCCEEDED(SHGetDesktopFolder(&desktop)) &&
            SUCCEEDED(desktop->BindToObject(appsPidl, nullptr, IID_IShellFolder, reinterpret_cast<void**>(&apps))) &&
            SUCCEEDED(apps->EnumObjects(nullptr, SHCONTF_NONFOLDERS, &items)) && items)
        {
            std::unordered_set<std::wstring> seen;
            seen.reserve(1024);

            for (;;)
            {
                PITEMID_CHILD child = nullptr;
                ULONG fetched = 0;
                if (items->Next(1, &child, &fetched) != S_OK || !child)
                {
                    break;
                }

                const std::wstring name = displayName(apps, child, SHGDN_NORMAL);
                const std::wstring parsing = displayName(apps, child, SHGDN_FORPARSING);
                CoTaskMemFree(child);

                if (name.empty() || parsing.empty())
                {
                    continue;
                }
                if (!seen.insert(lowerCopy(parsing)).second)
                {
                    continue;
                }

                Command command = makeCommand(CommandKind::App, name, L"Store app", parsing, 3900);
                command.searchText += L" uwp store app";
                out.push_back(std::move(command));
            }
        }

        if (items)
        {
            items->Release();
        }
        if (apps)
        {
            apps->Release();
        }
        if (desktop)
        {
            desktop->Release();
        }
        CoTaskMemFree(appsPidl);
        if (uninit)
        {
            CoUninitialize();
        }
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Store apps"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open Store app search directly", info().id));
    }
};
}

std::unique_ptr<Provider> makeAppsFolderProvider()
{
    return std::make_unique<AppsFolderProvider>();
}
