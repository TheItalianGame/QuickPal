#include "settings.h"

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
    if (settings.everythingHttpHost.empty())
    {
        settings.everythingHttpHost = L"127.0.0.1";
    }
    if (settings.appearance != Appearance::System && settings.appearance != Appearance::Dark && settings.appearance != Appearance::Light)
    {
        settings.appearance = Appearance::System;
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
    writeBool(L"UseSystemAccent", settings.useSystemAccent);
    writeInt(L"Appearance", static_cast<int>(settings.appearance));
    writeInt(L"MaxResults", settings.maxResults);
    writeInt(L"FileDepth", settings.fileDepth);
    writeInt(L"FileLimit", settings.fileLimit);
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
    settings.everythingHttpPort = readInt(L"EverythingHttpPort", settings.everythingHttpPort);
    settings.fallbackFileIndex = readBool(L"FallbackFileIndex", settings.fallbackFileIndex);
    settings.indexDesktop = readBool(L"IndexDesktop", settings.indexDesktop);
    settings.indexDocuments = readBool(L"IndexDocuments", settings.indexDocuments);
    settings.indexDownloads = readBool(L"IndexDownloads", settings.indexDownloads);
    settings.indexDefaultPaths = readBool(L"IndexDefaultPaths", settings.indexDefaultPaths);
    settings.indexStartMenu = readBool(L"IndexStartMenu", settings.indexStartMenu);
    settings.indexPathTools = readBool(L"IndexPathTools", settings.indexPathTools);
    settings.showLatency = readBool(L"ShowLatency", settings.showLatency);
    settings.shellUsesPowerShell = readBool(L"ShellUsesPowerShell", settings.shellUsesPowerShell);
    settings.useSystemAccent = readBool(L"UseSystemAccent", settings.useSystemAccent);
    settings.appearance = static_cast<Appearance>(readInt(L"Appearance", static_cast<int>(settings.appearance)));
    settings.maxResults = readInt(L"MaxResults", settings.maxResults);
    settings.fileDepth = readInt(L"FileDepth", settings.fileDepth);
    settings.fileLimit = readInt(L"FileLimit", settings.fileLimit);

    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings = normalizedSettings(settings);
    }
    saveSettings();
}

const std::vector<SettingRow>& settingRows()
{
    static const std::vector<SettingRow> rows = [] {
        std::vector<SettingRow> list;
        auto header = [&](const wchar_t* text) {
            SettingRow row;
            row.isHeader = true;
            row.header = text;
            list.push_back(row);
        };
        auto item = [&](SettingField field, SettingKind kind, const wchar_t* title, const wchar_t* subtitle) {
            SettingRow row;
            row.isHeader = false;
            row.item = SettingItem{ field, kind, title, subtitle };
            list.push_back(row);
        };

        header(L"Appearance");
        item(SettingField::Appearance, SettingKind::Choice, L"Theme", L"Follow Windows, or pin QuickPal to dark or light");
        item(SettingField::UseSystemAccent, SettingKind::Toggle, L"System accent", L"Tint highlights with the Windows accent color");

        header(L"File search");
        item(SettingField::UseEverythingHttp, SettingKind::Toggle, L"Everything HTTP API", L"Query Everything's local HTTP server with saved credentials");
        item(SettingField::EverythingHttpPort, SettingKind::Stepper, L"HTTP port", L"Port of the local Everything HTTP server — type a number to set it");
        item(SettingField::UseEverything, SettingKind::Toggle, L"Everything SDK", L"Use Everything's indexed API for whole-machine file search");
        item(SettingField::FallbackFileIndex, SettingKind::Toggle, L"Fallback file index", L"Background index used when the Everything SDK is unavailable");
        item(SettingField::IndexDesktop, SettingKind::Toggle, L"Index Desktop", L"Include Desktop in the fallback file index");
        item(SettingField::IndexDocuments, SettingKind::Toggle, L"Index Documents", L"Include Documents in the fallback file index");
        item(SettingField::IndexDownloads, SettingKind::Toggle, L"Index Downloads", L"Include Downloads in the fallback file index");
        item(SettingField::IndexDefaultPaths, SettingKind::Toggle, L"Default locations", L"Include profile, OneDrive, Public, and app folders");
        item(SettingField::FileDepth, SettingKind::Stepper, L"Fallback depth", L"How many directory levels the background walk descends");
        item(SettingField::FileLimit, SettingKind::Stepper, L"Fallback file cap", L"Maximum fallback file entries kept in memory");

        header(L"Command sources");
        item(SettingField::IndexStartMenu, SettingKind::Toggle, L"Start Menu apps", L"Index app shortcuts from the Start Menu folders");
        item(SettingField::IndexPathTools, SettingKind::Toggle, L"PATH tools", L"Index executables and scripts discoverable through PATH");

        header(L"Behavior");
        item(SettingField::ShellUsesPowerShell, SettingKind::Toggle, L"Shell runner", L"Run > commands with PowerShell when on, cmd when off");
        item(SettingField::MaxResults, SettingKind::Stepper, L"Max results", L"Bounds ranking and rendering work per keystroke");
        item(SettingField::ShowLatency, SettingKind::Toggle, L"Latency readout", L"Show result timing in the footer");

        header(L"Maintenance");
        item(SettingField::ReloadIndexes, SettingKind::Action, L"Reload indexes", L"Rebuild every provider immediately");

        return list;
    }();
    return rows;
}

int firstSelectableRow()
{
    const auto& rows = settingRows();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        if (!rows[i].isHeader)
        {
            return i;
        }
    }
    return 0;
}

int nextSelectableRow(int from, int direction)
{
    const auto& rows = settingRows();
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
           field == SettingField::FileLimit || field == SettingField::EverythingHttpPort;
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
    count = 0;
    return nullptr;
}

int settingChoiceIndex(SettingField field, const Settings& settings)
{
    if (field == SettingField::Appearance)
    {
        return static_cast<int>(settings.appearance);
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
        return std::to_wstring(value);
    }
    if (field == SettingField::Appearance)
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
    return settingToggleValue(field, settings) ? L"On" : L"Off";
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
    case SettingField::ReloadIndexes:
        break;
    }
    g_settings = normalizedSettings(g_settings);
}
