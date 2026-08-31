#include "settings.h"

#include "provider.h"
#include "util.h"

#include <algorithm>
#include <mutex>

namespace
{
std::mutex g_settingsMutex;
Settings g_settings;

// Stepping the file cap by a fixed amount meant ~30 presses to cross its range.
// These stops cover 1k..150k in ten.
constexpr int kFileLimitLadder[] = { 1000, 2500, 5000, 10000, 20000, 35000, 50000, 75000, 100000, 150000 };

constexpr const wchar_t* kAppearanceChoices[] = { L"System", L"Dark", L"Light" };
constexpr const wchar_t* kWindowPositionChoices[] = { L"Center", L"Upper" };
}

std::wstring settingsDirectory()
{
    std::wstring appData = env(L"APPDATA");
    if (appData.empty())
    {
        appData = executableDirectory();
    }
    return appData + L"\\QuickPal";
}

std::wstring settingsPath()
{
    return settingsDirectory() + L"\\settings.ini";
}

static void ensureSettingsDirectory()
{
    CreateDirectoryW(settingsDirectory().c_str(), nullptr);
}

static std::wstring readIniString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& fallback)
{
    std::wstring value(4096, L'\0');
    const DWORD len = GetPrivateProfileStringW(section, key, fallback.c_str(), value.data(),
                                               static_cast<DWORD>(value.size()), path.c_str());
    value.resize(len);
    return value;
}

static std::vector<std::wstring> readIniKeys(const std::wstring& path, const wchar_t* section)
{
    std::wstring keys(32768, L'\0');
    const DWORD len = GetPrivateProfileStringW(section, nullptr, L"", keys.data(),
                                               static_cast<DWORD>(keys.size()), path.c_str());
    keys.resize(len);

    std::vector<std::wstring> out;
    for (size_t i = 0; i < keys.size();)
    {
        const wchar_t* begin = keys.c_str() + i;
        const size_t length = wcslen(begin);
        if (length == 0)
        {
            break;
        }
        out.emplace_back(begin, length);
        i += length + 1;
    }
    return out;
}

// Everything already stores its HTTP server config; read it so the feature works
// without the user retyping port and credentials here.
static void importEverythingHttpSettings(Settings& settings)
{
    const std::wstring appData = env(L"APPDATA");
    if (appData.empty())
    {
        return;
    }
    const std::wstring everythingIni = appData + L"\\Everything\\Everything.ini";

    settings.useEverythingHttp = GetPrivateProfileIntW(L"Everything", L"http_server_enabled",
                                                       settings.useEverythingHttp ? 1 : 0,
                                                       everythingIni.c_str()) != 0;
    settings.everythingHttpHost = L"127.0.0.1";
    settings.everythingHttpPort = static_cast<int>(GetPrivateProfileIntW(
        L"Everything", L"http_server_port", settings.everythingHttpPort, everythingIni.c_str()));
    settings.everythingHttpUsername = readIniString(everythingIni, L"Everything", L"http_server_username", settings.everythingHttpUsername);
    settings.everythingHttpPassword = readIniString(everythingIni, L"Everything", L"http_server_password", settings.everythingHttpPassword);
}

EverythingHttpSettings everythingHttpSettingsFrom(const Settings& settings)
{
    EverythingHttpSettings http;
    http.host = settings.everythingHttpHost;
    http.username = settings.everythingHttpUsername;
    http.password = settings.everythingHttpPassword;
    http.port = settings.everythingHttpPort;
    return http;
}

Settings normalizedSettings(Settings settings)
{
    settings.maxResults = std::clamp(settings.maxResults, kMinMaxResults, kMaxMaxResults);
    settings.fileDepth = std::clamp(settings.fileDepth, 1, 8);
    settings.fileLimit = std::clamp(settings.fileLimit, 1000, 150000);
    settings.everythingHttpPort = std::clamp(settings.everythingHttpPort, 1, 65535);
    settings.bitwardenPinTimeoutSeconds = std::clamp(settings.bitwardenPinTimeoutSeconds, 60, 3600);
    settings.bitwardenClipboardClearSeconds = std::clamp(settings.bitwardenClipboardClearSeconds, 5, 300);
    if (settings.everythingHttpHost.empty())
    {
        settings.everythingHttpHost = L"127.0.0.1";
    }
    if (settings.appearance != Appearance::System && settings.appearance != Appearance::Dark && settings.appearance != Appearance::Light)
    {
        settings.appearance = Appearance::System;
    }
    if (settings.windowPosition != WindowPosition::Center && settings.windowPosition != WindowPosition::Upper)
    {
        settings.windowPosition = WindowPosition::Center;
    }
    for (auto it = settings.providerPrefixes.begin(); it != settings.providerPrefixes.end();)
    {
        it->second = normalizeProviderPrefix(it->second);
        if (it->second.empty())
        {
            it = settings.providerPrefixes.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return settings;
}

Settings getSettingsSnapshot()
{
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

void saveSettings()
{
    ensureSettingsDirectory();
    const Settings settings = getSettingsSnapshot();
    const std::wstring path = settingsPath();

    auto writeBool = [&](const wchar_t* key, bool value) {
        WritePrivateProfileStringW(L"QuickPal", key, value ? L"1" : L"0", path.c_str());
    };
    auto writeInt = [&](const wchar_t* key, int value) {
        wchar_t buffer[32]{};
        wsprintfW(buffer, L"%d", value);
        WritePrivateProfileStringW(L"QuickPal", key, buffer, path.c_str());
    };
    auto writeString = [&](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(L"QuickPal", key, value.c_str(), path.c_str());
    };

    writeBool(L"UseEverything", settings.useEverything);
    writeBool(L"UseEverythingHttp", settings.useEverythingHttp);
    writeString(L"EverythingHttpHost", settings.everythingHttpHost);
    writeString(L"EverythingHttpUsername", settings.everythingHttpUsername);
    writeString(L"EverythingHttpPassword", settings.everythingHttpPassword);
    writeString(L"BitwardenAccountEmail", settings.bitwardenAccountEmail);
    writeInt(L"EverythingHttpPort", settings.everythingHttpPort);
    writeBool(L"FallbackFileIndex", settings.fallbackFileIndex);
    writeBool(L"IndexDesktop", settings.indexDesktop);
    writeBool(L"IndexDocuments", settings.indexDocuments);
    writeBool(L"IndexDownloads", settings.indexDownloads);
    writeBool(L"IndexDefaultPaths", settings.indexDefaultPaths);
    writeBool(L"IndexStartMenu", settings.indexStartMenu);
    writeBool(L"IndexPathTools", settings.indexPathTools);
    writeBool(L"ShowLatency", settings.showLatency);
    writeBool(L"ShellUsesPowerShell", settings.shellUsesPowerShell);
    writeBool(L"UseChromeTabs", settings.useChromeTabs);
    writeBool(L"BitwardenSearchUsernames", settings.bitwardenSearchUsernames);
    writeBool(L"BitwardenUnlockWithPin", settings.bitwardenUnlockWithPin);
    writeBool(L"BitwardenUnlockWithHello", settings.bitwardenUnlockWithHello);
    writeBool(L"BitwardenRequireMasterOnRestart", settings.bitwardenRequireMasterOnRestart);
    writeBool(L"BitwardenFastLocalApi", settings.bitwardenUseServe);
    writeBool(L"BitwardenLockOnSleep", settings.bitwardenLockOnSleep);
    writeBool(L"BitwardenLockOnExit", settings.bitwardenLockOnExit);
    writeBool(L"UseSystemAccent", settings.useSystemAccent);
    writeInt(L"Appearance", static_cast<int>(settings.appearance));
    writeInt(L"WindowPosition", static_cast<int>(settings.windowPosition));
    writeInt(L"MaxResults", settings.maxResults);
    writeInt(L"FileDepth", settings.fileDepth);
    writeInt(L"FileLimit", settings.fileLimit);
    writeInt(L"BitwardenPinTimeoutSeconds", settings.bitwardenPinTimeoutSeconds);
    writeInt(L"BitwardenClipboardClearSeconds", settings.bitwardenClipboardClearSeconds);

    WritePrivateProfileStringW(L"ProviderShortcuts", nullptr, nullptr, path.c_str());
    for (const auto& [providerId, shortcut] : settings.providerShortcuts)
    {
        WritePrivateProfileStringW(L"ProviderShortcuts", providerId.c_str(), shortcut.c_str(), path.c_str());
    }

    WritePrivateProfileStringW(L"ProviderPrefixes", nullptr, nullptr, path.c_str());
    for (const auto& [providerId, prefix] : settings.providerPrefixes)
    {
        WritePrivateProfileStringW(L"ProviderPrefixes", providerId.c_str(), prefix.c_str(), path.c_str());
    }
}

void loadSettings()
{
    ensureSettingsDirectory();
    const std::wstring path = settingsPath();
    Settings settings;

    auto readBool = [&](const wchar_t* key, bool fallback) {
        return GetPrivateProfileIntW(L"QuickPal", key, fallback ? 1 : 0, path.c_str()) != 0;
    };
    auto readInt = [&](const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(L"QuickPal", key, fallback, path.c_str()));
    };

    // Seed from Everything.ini first so a fresh install picks up a running server,
    // then let anything saved here win.
    importEverythingHttpSettings(settings);

    settings.useEverything = readBool(L"UseEverything", settings.useEverything);
    settings.useEverythingHttp = readBool(L"UseEverythingHttp", settings.useEverythingHttp);
    settings.everythingHttpHost = readIniString(path, L"QuickPal", L"EverythingHttpHost", settings.everythingHttpHost);
    settings.everythingHttpUsername = readIniString(path, L"QuickPal", L"EverythingHttpUsername", settings.everythingHttpUsername);
    settings.everythingHttpPassword = readIniString(path, L"QuickPal", L"EverythingHttpPassword", settings.everythingHttpPassword);
    settings.bitwardenAccountEmail = readIniString(path, L"QuickPal", L"BitwardenAccountEmail", settings.bitwardenAccountEmail);
    settings.everythingHttpPort = readInt(L"EverythingHttpPort", settings.everythingHttpPort);
    settings.fallbackFileIndex = readBool(L"FallbackFileIndex", settings.fallbackFileIndex);
    settings.indexDesktop = readBool(L"IndexDesktop", settings.indexDesktop);
    settings.indexDocuments = readBool(L"IndexDocuments", settings.indexDocuments);
    settings.indexDownloads = readBool(L"IndexDownloads", settings.indexDownloads);
    settings.indexDefaultPaths = readBool(L"IndexDefaultPaths", settings.indexDefaultPaths);
    settings.indexStartMenu = readBool(L"IndexStartMenu", settings.indexStartMenu);
    settings.indexPathTools = readBool(L"IndexPathTools", settings.indexPathTools);
    // The old latency footer was diagnostic UI; keep it permanently disabled.
    settings.showLatency = false;
    settings.shellUsesPowerShell = readBool(L"ShellUsesPowerShell", settings.shellUsesPowerShell);
    settings.useChromeTabs = readBool(L"UseChromeTabs", settings.useChromeTabs);
    settings.bitwardenSearchUsernames = readBool(L"BitwardenSearchUsernames", settings.bitwardenSearchUsernames);
    settings.bitwardenUnlockWithPin = readBool(L"BitwardenUnlockWithPin", settings.bitwardenUnlockWithPin);
    settings.bitwardenUnlockWithHello = readBool(L"BitwardenUnlockWithHello", settings.bitwardenUnlockWithHello);
    settings.bitwardenRequireMasterOnRestart = readBool(L"BitwardenRequireMasterOnRestart", settings.bitwardenRequireMasterOnRestart);
    settings.bitwardenUseServe = readBool(L"BitwardenFastLocalApi", settings.bitwardenUseServe);
    settings.bitwardenLockOnSleep = readBool(L"BitwardenLockOnSleep", settings.bitwardenLockOnSleep);
    settings.bitwardenLockOnExit = readBool(L"BitwardenLockOnExit", settings.bitwardenLockOnExit);
    settings.useSystemAccent = readBool(L"UseSystemAccent", settings.useSystemAccent);
    settings.appearance = static_cast<Appearance>(readInt(L"Appearance", static_cast<int>(settings.appearance)));
    settings.windowPosition = static_cast<WindowPosition>(readInt(L"WindowPosition", static_cast<int>(settings.windowPosition)));
    settings.maxResults = readInt(L"MaxResults", settings.maxResults);
    settings.fileDepth = readInt(L"FileDepth", settings.fileDepth);
    settings.fileLimit = readInt(L"FileLimit", settings.fileLimit);
    settings.bitwardenPinTimeoutSeconds = readInt(L"BitwardenPinTimeoutSeconds", settings.bitwardenPinTimeoutSeconds);
    settings.bitwardenClipboardClearSeconds = readInt(L"BitwardenClipboardClearSeconds", settings.bitwardenClipboardClearSeconds);

    for (const auto& key : readIniKeys(path, L"ProviderShortcuts"))
    {
        settings.providerShortcuts[key] = readIniString(path, L"ProviderShortcuts", key.c_str(), L"");
    }
    for (const auto& key : readIniKeys(path, L"ProviderPrefixes"))
    {
        settings.providerPrefixes[key] = normalizeProviderPrefix(readIniString(path, L"ProviderPrefixes", key.c_str(), L""));
    }

    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings = normalizedSettings(settings);
    }
    saveSettings();
}

SettingRow makeSettingHeader(std::wstring text)
{
    SettingRow row;
    row.isHeader = true;
    row.header = std::move(text);
    return row;
}

SettingRow makeSettingItem(SettingField field, SettingKind kind, std::wstring title, std::wstring subtitle,
                           std::wstring providerId, std::wstring settingKey)
{
    SettingRow row;
    row.isHeader = false;
    row.item.field = field;
    row.item.kind = kind;
    row.item.title = std::move(title);
    row.item.subtitle = std::move(subtitle);
    row.item.providerId = std::move(providerId);
    row.item.settingKey = std::move(settingKey);
    return row;
}

const std::vector<SettingSection>& settingSections()
{
    static const std::vector<SettingSection> sections = [] {
        std::vector<SettingSection> result;
        SettingSection general;
        general.id = L"general";
        general.title = L"General";
        general.subtitle = L"Appearance, result density, and maintenance";
        std::vector<SettingRow>& list = general.rows;
        auto header = [&](const wchar_t* text) {
            list.push_back(makeSettingHeader(text));
        };
        auto item = [&](SettingField field, SettingKind kind, const wchar_t* title, const wchar_t* subtitle) {
            list.push_back(makeSettingItem(field, kind, title, subtitle));
        };

        header(L"Appearance");
        item(SettingField::Appearance, SettingKind::Choice, L"Theme", L"Follow Windows, or pin QuickPal to dark or light");
        item(SettingField::WindowPosition, SettingKind::Choice, L"Window position", L"Center on screen, or sit higher for faster scanning");
        item(SettingField::UseSystemAccent, SettingKind::Toggle, L"System accent", L"Tint highlights with the Windows accent color");

        header(L"Behavior");
        item(SettingField::MaxResults, SettingKind::Stepper, L"Max results", L"Bounds ranking and rendering work per keystroke");

        header(L"Maintenance");
        item(SettingField::ReloadIndexes, SettingKind::Action, L"Reload indexes", L"Rebuild every provider immediately");

        result.push_back(std::move(general));

        const Settings snapshot = getSettingsSnapshot();
        const ProviderContext ctx{ snapshot, nullptr };
        for (const auto& entry : ProviderRegistry::instance().entries())
        {
            SettingSection section;
            section.id = entry.info.id;
            section.title = entry.info.title;
            section.subtitle = entry.info.settingsSummary;
            entry.provider->settings(ctx, section.rows);
            if (section.rows.empty())
            {
                continue;
            }
            if (section.rows.front().isHeader && _wcsicmp(section.rows.front().header.c_str(), section.title.c_str()) == 0)
            {
                section.rows.erase(section.rows.begin());
            }
            result.push_back(std::move(section));
        }

        return result;
    }();
    return sections;
}

const SettingSection& settingSection(int index)
{
    const auto& sections = settingSections();
    return sections[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(sections.size()) - 1))];
}

int settingSectionIndex(const std::wstring& id)
{
    const auto& sections = settingSections();
    for (int i = 0; i < static_cast<int>(sections.size()); ++i)
    {
        if (_wcsicmp(sections[static_cast<size_t>(i)].id.c_str(), id.c_str()) == 0)
        {
            return i;
        }
    }
    return 0;
}

int firstSelectableRow(const std::vector<SettingRow>& rows)
{
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        if (!rows[i].isHeader)
        {
            return i;
        }
    }
    return 0;
}

int nextSelectableRow(const std::vector<SettingRow>& rows, int from, int direction)
{
    const int count = static_cast<int>(rows.size());
    if (count == 0)
    {
        return 0;
    }
    int index = from;
    for (int guard = 0; guard < count; ++guard)
    {
        index += direction;
        if (index < 0 || index >= count)
        {
            return std::clamp(from, 0, count - 1);
        }
        if (!rows[index].isHeader)
        {
            return index;
        }
    }
    return std::clamp(from, 0, count - 1);
}

bool isToggleSetting(SettingField field)
{
    switch (field)
    {
    case SettingField::UseSystemAccent:
    case SettingField::UseEverything:
    case SettingField::UseEverythingHttp:
    case SettingField::FallbackFileIndex:
    case SettingField::IndexDesktop:
    case SettingField::IndexDocuments:
    case SettingField::IndexDownloads:
    case SettingField::IndexDefaultPaths:
    case SettingField::IndexStartMenu:
    case SettingField::IndexPathTools:
    case SettingField::UseChromeTabs:
    case SettingField::BitwardenSearchUsernames:
    case SettingField::BitwardenUnlockWithPin:
    case SettingField::BitwardenUnlockWithHello:
    case SettingField::BitwardenRequireMasterOnRestart:
    case SettingField::BitwardenUseServe:
    case SettingField::BitwardenLockOnSleep:
    case SettingField::BitwardenLockOnExit:
    case SettingField::ShellUsesPowerShell:
    case SettingField::ShowLatency:
        return true;
    default:
        return false;
    }
}

bool isNumericSetting(SettingField field)
{
    return field == SettingField::MaxResults || field == SettingField::FileDepth ||
           field == SettingField::FileLimit || field == SettingField::EverythingHttpPort ||
           field == SettingField::BitwardenPinTimeoutSeconds ||
           field == SettingField::BitwardenClipboardClearSeconds;
}

StepperSpec stepperSpec(SettingField field)
{
    switch (field)
    {
    case SettingField::MaxResults:
        return StepperSpec{ kMinMaxResults, kMaxMaxResults, nullptr, 0, 1 };
    case SettingField::EverythingHttpPort:
        // Stepping a port one at a time is pointless; inline numeric entry is the
        // real interaction here, so +/- is only for nudging.
        return StepperSpec{ 1, 65535, nullptr, 0, 1 };
    case SettingField::FileDepth:
        return StepperSpec{ 1, 8, nullptr, 0, 1 };
    case SettingField::FileLimit:
        return StepperSpec{ 1000, 150000, kFileLimitLadder, static_cast<int>(std::size(kFileLimitLadder)), 5000 };
    case SettingField::BitwardenPinTimeoutSeconds:
        return StepperSpec{ 60, 3600, nullptr, 0, 60 };
    case SettingField::BitwardenClipboardClearSeconds:
        return StepperSpec{ 5, 300, nullptr, 0, 5 };
    default:
        return StepperSpec{};
    }
}

int settingNumericValue(SettingField field, const Settings& settings)
{
    switch (field)
    {
    case SettingField::MaxResults: return settings.maxResults;
    case SettingField::FileDepth: return settings.fileDepth;
    case SettingField::FileLimit: return settings.fileLimit;
    case SettingField::EverythingHttpPort: return settings.everythingHttpPort;
    case SettingField::BitwardenPinTimeoutSeconds: return settings.bitwardenPinTimeoutSeconds;
    case SettingField::BitwardenClipboardClearSeconds: return settings.bitwardenClipboardClearSeconds;
    default: return 0;
    }
}

void setSettingNumericValue(SettingField field, int value)
{
    const StepperSpec spec = stepperSpec(field);
    const int clamped = std::clamp(value, spec.minValue, spec.maxValue);
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        switch (field)
        {
        case SettingField::MaxResults: g_settings.maxResults = clamped; break;
        case SettingField::FileDepth: g_settings.fileDepth = clamped; break;
        case SettingField::FileLimit: g_settings.fileLimit = clamped; break;
        case SettingField::EverythingHttpPort: g_settings.everythingHttpPort = clamped; break;
        case SettingField::BitwardenPinTimeoutSeconds: g_settings.bitwardenPinTimeoutSeconds = clamped; break;
        case SettingField::BitwardenClipboardClearSeconds: g_settings.bitwardenClipboardClearSeconds = clamped; break;
        default: break;
        }
        g_settings = normalizedSettings(g_settings);
    }
    saveSettings();
}

const wchar_t* const* settingChoices(SettingField field, int& count)
{
    if (field == SettingField::Appearance)
    {
        count = static_cast<int>(std::size(kAppearanceChoices));
        return kAppearanceChoices;
    }
    if (field == SettingField::WindowPosition)
    {
        count = static_cast<int>(std::size(kWindowPositionChoices));
        return kWindowPositionChoices;
    }
    count = 0;
    return nullptr;
}

int settingChoiceIndex(SettingField field, const Settings& settings)
{
    if (field == SettingField::Appearance)
    {
        return static_cast<int>(settings.appearance);
    }
    if (field == SettingField::WindowPosition)
    {
        return static_cast<int>(settings.windowPosition);
    }
    return 0;
}

bool settingToggleValue(SettingField field, const Settings& settings)
{
    switch (field)
    {
    case SettingField::UseSystemAccent: return settings.useSystemAccent;
    case SettingField::UseEverything: return settings.useEverything;
    case SettingField::UseEverythingHttp: return settings.useEverythingHttp;
    case SettingField::FallbackFileIndex: return settings.fallbackFileIndex;
    case SettingField::IndexDesktop: return settings.indexDesktop;
    case SettingField::IndexDocuments: return settings.indexDocuments;
    case SettingField::IndexDownloads: return settings.indexDownloads;
    case SettingField::IndexDefaultPaths: return settings.indexDefaultPaths;
    case SettingField::IndexStartMenu: return settings.indexStartMenu;
    case SettingField::IndexPathTools: return settings.indexPathTools;
    case SettingField::UseChromeTabs: return settings.useChromeTabs;
    case SettingField::BitwardenSearchUsernames: return settings.bitwardenSearchUsernames;
    case SettingField::BitwardenUnlockWithPin: return settings.bitwardenUnlockWithPin;
    case SettingField::BitwardenUnlockWithHello: return settings.bitwardenUnlockWithHello;
    case SettingField::BitwardenRequireMasterOnRestart: return settings.bitwardenRequireMasterOnRestart;
    case SettingField::BitwardenUseServe: return settings.bitwardenUseServe;
    case SettingField::BitwardenLockOnSleep: return settings.bitwardenLockOnSleep;
    case SettingField::BitwardenLockOnExit: return settings.bitwardenLockOnExit;
    case SettingField::ShellUsesPowerShell: return settings.shellUsesPowerShell;
    case SettingField::ShowLatency: return settings.showLatency;
    default: return false;
    }
}

std::wstring settingValueText(SettingField field, const Settings& settings)
{
    if (isNumericSetting(field))
    {
        const int value = settingNumericValue(field, settings);
        if (field == SettingField::FileLimit && value >= 1000)
        {
            // 50000 -> "50k"; keeps the value box narrow at every stop.
            const int thousands = value / 1000;
            const int remainder = (value % 1000) / 100;
            std::wstring text = std::to_wstring(thousands);
            if (remainder != 0)
            {
                text += L"." + std::to_wstring(remainder);
            }
            return text + L"k";
        }
        if (field == SettingField::BitwardenPinTimeoutSeconds)
        {
            return std::to_wstring(value / 60) + L"m";
        }
        if (field == SettingField::BitwardenClipboardClearSeconds)
        {
            return std::to_wstring(value) + L"s";
        }
        return std::to_wstring(value);
    }
    if (field == SettingField::Appearance || field == SettingField::WindowPosition)
    {
        int count = 0;
        const wchar_t* const* choices = settingChoices(field, count);
        const int index = std::clamp(settingChoiceIndex(field, settings), 0, std::max(0, count - 1));
        return count > 0 ? choices[index] : L"";
    }
    if (field == SettingField::ReloadIndexes)
    {
        return L"Run";
    }
    if (field == SettingField::InstallChromeExtension)
    {
        return L"Open";
    }
    if (field == SettingField::ProviderAction)
    {
        return L"Open";
    }
    return settingToggleValue(field, settings) ? L"On" : L"Off";
}

std::wstring providerShortcutValue(const std::wstring& providerId)
{
    const Settings settings = getSettingsSnapshot();
    const auto it = settings.providerShortcuts.find(providerId);
    if (it == settings.providerShortcuts.end() || it->second.empty())
    {
        return L"None";
    }
    return it->second;
}

static std::wstring providerPrefixValue(const std::wstring& providerId, const Settings& settings)
{
    if (Provider* provider = ProviderRegistry::instance().byId(providerId.c_str()))
    {
        const ProviderInfo* info = ProviderRegistry::instance().infoFor(provider);
        if (!info)
        {
            return L"None";
        }
        const auto custom = settings.providerPrefixes.find(providerId);
        if (custom != settings.providerPrefixes.end() && !normalizeProviderPrefix(custom->second).empty())
        {
            return normalizeProviderPrefix(custom->second);
        }

        const std::vector<std::wstring> prefixes = effectiveProviderPrefixes(*info, settings);
        if (!prefixes.empty())
        {
            return prefixes.front();
        }
    }
    return L"None";
}

std::wstring providerPrefixValue(const std::wstring& providerId)
{
    return providerPrefixValue(providerId, getSettingsSnapshot());
}

void setBitwardenAccountEmail(const std::wstring& email)
{
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings.bitwardenAccountEmail = email;
    }
    saveSettings();
}

void setProviderPrefixValue(const std::wstring& providerId, const std::wstring& prefix)
{
    if (providerId.empty())
    {
        return;
    }

    const std::wstring normalized = normalizeProviderPrefix(prefix);
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (normalized.empty())
        {
            g_settings.providerPrefixes.erase(providerId);
        }
        else
        {
            g_settings.providerPrefixes[providerId] = normalized;
        }
    }
    saveSettings();
}

std::wstring providerPrefixConflict(const std::wstring& providerId, const std::wstring& prefix)
{
    const std::wstring normalized = normalizeProviderPrefix(prefix);
    if (normalized.empty())
    {
        return {};
    }

    const Settings settings = getSettingsSnapshot();
    for (const auto& entry : ProviderRegistry::instance().entries())
    {
        const ProviderInfo& info = entry.info;
        if (providerId == info.id)
        {
            continue;
        }
        for (const auto& candidate : effectiveProviderPrefixes(info, settings))
        {
            if (lowerCopy(candidate) == lowerCopy(normalized))
            {
                return info.title;
            }
        }
    }
    return {};
}

void setProviderShortcutValue(const std::wstring& providerId, const std::wstring& shortcut)
{
    if (providerId.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (shortcut.empty() || shortcut == L"None")
        {
            g_settings.providerShortcuts.erase(providerId);
        }
        else
        {
            g_settings.providerShortcuts[providerId] = shortcut;
        }
    }
    saveSettings();
}

std::wstring settingValueText(const SettingItem& item, const Settings& settings)
{
    if (item.field == SettingField::ProviderShortcut)
    {
        const auto it = settings.providerShortcuts.find(item.providerId);
        if (it == settings.providerShortcuts.end() || it->second.empty())
        {
            return L"None";
        }
        return it->second;
    }
    if (item.field == SettingField::ProviderPrefix)
    {
        return providerPrefixValue(item.providerId, settings);
    }
    if (item.field == SettingField::ProviderAction && item.providerId == L"bitwarden" &&
        item.settingKey == L"account-email")
    {
        return settings.bitwardenAccountEmail.empty() ? L"Set" : settings.bitwardenAccountEmail;
    }
    return settingValueText(item.field, settings);
}

bool settingNeedsRebuild(SettingField field)
{
    switch (field)
    {
    case SettingField::UseEverything:
    case SettingField::UseEverythingHttp:
    case SettingField::EverythingHttpPort:
    case SettingField::FallbackFileIndex:
    case SettingField::IndexDesktop:
    case SettingField::IndexDocuments:
    case SettingField::IndexDownloads:
    case SettingField::IndexDefaultPaths:
    case SettingField::IndexStartMenu:
    case SettingField::IndexPathTools:
    case SettingField::UseChromeTabs:
    case SettingField::BitwardenSearchUsernames:
    case SettingField::FileDepth:
    case SettingField::FileLimit:
    case SettingField::ReloadIndexes:
        return true;
    default:
        return false;
    }
}

bool settingNeedsRepaintOnly(SettingField field)
{
    return field == SettingField::Appearance || field == SettingField::UseSystemAccent;
}

static int stepLadder(const StepperSpec& spec, int current, int direction)
{
    if (!spec.ladder || spec.ladderCount == 0)
    {
        return std::clamp(current + direction * spec.step, spec.minValue, spec.maxValue);
    }

    if (direction > 0)
    {
        for (int i = 0; i < spec.ladderCount; ++i)
        {
            if (spec.ladder[i] > current)
            {
                return spec.ladder[i];
            }
        }
        return spec.ladder[spec.ladderCount - 1];
    }

    for (int i = spec.ladderCount - 1; i >= 0; --i)
    {
        if (spec.ladder[i] < current)
        {
            return spec.ladder[i];
        }
    }
    return spec.ladder[0];
}

static void mutateLocked(SettingField field, int direction);

SettingChange applySetting(SettingField field, int direction)
{
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        mutateLocked(field, direction);
        g_settings = normalizedSettings(g_settings);
    }
    saveSettings();

    SettingChange change;
    change.needsRebuild = settingNeedsRebuild(field);
    change.needsThemeRefresh = settingNeedsRepaintOnly(field);
    return change;
}

// Caller must already hold g_settingsMutex.
static void mutateLocked(SettingField field, int direction)
{
    switch (field)
    {
    case SettingField::Appearance:
    {
        int count = 0;
        settingChoices(field, count);
        if (count > 0)
        {
            int index = static_cast<int>(g_settings.appearance) + (direction >= 0 ? 1 : -1);
            index = (index % count + count) % count;
            g_settings.appearance = static_cast<Appearance>(index);
        }
        break;
    }
    case SettingField::WindowPosition:
    {
        int count = 0;
        settingChoices(field, count);
        if (count > 0)
        {
            int index = static_cast<int>(g_settings.windowPosition) + (direction >= 0 ? 1 : -1);
            index = (index % count + count) % count;
            g_settings.windowPosition = static_cast<WindowPosition>(index);
        }
        break;
    }
    case SettingField::UseSystemAccent: g_settings.useSystemAccent = !g_settings.useSystemAccent; break;
    case SettingField::UseEverything: g_settings.useEverything = !g_settings.useEverything; break;
    case SettingField::UseEverythingHttp: g_settings.useEverythingHttp = !g_settings.useEverythingHttp; break;
    case SettingField::EverythingHttpPort:
        g_settings.everythingHttpPort = stepLadder(stepperSpec(field), g_settings.everythingHttpPort, direction);
        break;
    case SettingField::FallbackFileIndex: g_settings.fallbackFileIndex = !g_settings.fallbackFileIndex; break;
    case SettingField::IndexDesktop: g_settings.indexDesktop = !g_settings.indexDesktop; break;
    case SettingField::IndexDocuments: g_settings.indexDocuments = !g_settings.indexDocuments; break;
    case SettingField::IndexDownloads: g_settings.indexDownloads = !g_settings.indexDownloads; break;
    case SettingField::IndexDefaultPaths: g_settings.indexDefaultPaths = !g_settings.indexDefaultPaths; break;
    case SettingField::IndexStartMenu: g_settings.indexStartMenu = !g_settings.indexStartMenu; break;
    case SettingField::IndexPathTools: g_settings.indexPathTools = !g_settings.indexPathTools; break;
    case SettingField::UseChromeTabs: g_settings.useChromeTabs = !g_settings.useChromeTabs; break;
    case SettingField::BitwardenSearchUsernames: g_settings.bitwardenSearchUsernames = !g_settings.bitwardenSearchUsernames; break;
    case SettingField::BitwardenUnlockWithPin: g_settings.bitwardenUnlockWithPin = !g_settings.bitwardenUnlockWithPin; break;
    case SettingField::BitwardenUnlockWithHello: g_settings.bitwardenUnlockWithHello = !g_settings.bitwardenUnlockWithHello; break;
    case SettingField::BitwardenRequireMasterOnRestart: g_settings.bitwardenRequireMasterOnRestart = !g_settings.bitwardenRequireMasterOnRestart; break;
    case SettingField::BitwardenUseServe: g_settings.bitwardenUseServe = !g_settings.bitwardenUseServe; break;
    case SettingField::BitwardenLockOnSleep: g_settings.bitwardenLockOnSleep = !g_settings.bitwardenLockOnSleep; break;
    case SettingField::BitwardenLockOnExit: g_settings.bitwardenLockOnExit = !g_settings.bitwardenLockOnExit; break;
    case SettingField::ShellUsesPowerShell: g_settings.shellUsesPowerShell = !g_settings.shellUsesPowerShell; break;
    case SettingField::ShowLatency: g_settings.showLatency = !g_settings.showLatency; break;
    case SettingField::MaxResults:
        g_settings.maxResults = stepLadder(stepperSpec(field), g_settings.maxResults, direction);
        break;
    case SettingField::FileDepth:
        g_settings.fileDepth = stepLadder(stepperSpec(field), g_settings.fileDepth, direction);
        break;
    case SettingField::FileLimit:
        g_settings.fileLimit = stepLadder(stepperSpec(field), g_settings.fileLimit, direction);
        break;
    case SettingField::BitwardenPinTimeoutSeconds:
        g_settings.bitwardenPinTimeoutSeconds = stepLadder(stepperSpec(field), g_settings.bitwardenPinTimeoutSeconds, direction);
        break;
    case SettingField::BitwardenClipboardClearSeconds:
        g_settings.bitwardenClipboardClearSeconds = stepLadder(stepperSpec(field), g_settings.bitwardenClipboardClearSeconds, direction);
        break;
    case SettingField::ReloadIndexes:
    case SettingField::ProviderShortcut:
    case SettingField::ProviderPrefix:
    case SettingField::ProviderAction:
    case SettingField::InstallChromeExtension:
        break;
    }
    g_settings = normalizedSettings(g_settings);
}
