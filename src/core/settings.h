#pragma once

#include "everything_client.h"
#include "types.h"

#include <string>
#include <vector>

EverythingHttpSettings everythingHttpSettingsFrom(const Settings& settings);

std::wstring settingsDirectory();
std::wstring settingsPath();

void loadSettings();
void saveSettings();
Settings getSettingsSnapshot();
Settings normalizedSettings(Settings settings);

enum class SettingField
{
    Appearance,
    UseSystemAccent,
    UseEverythingHttp,
    EverythingHttpPort,
    UseEverything,
    FallbackFileIndex,
    IndexDesktop,
    IndexDocuments,
    IndexDownloads,
    IndexDefaultPaths,
    FileDepth,
    FileLimit,
    IndexStartMenu,
    IndexPathTools,
    ShellUsesPowerShell,
    UseChromeTabs,
    MaxResults,
    ShowLatency,
    ProviderShortcut,
    ProviderAction,
    InstallChromeExtension,
    ReloadIndexes,
};

enum class SettingKind
{
    Toggle,
    Stepper,
    Choice,
    Action,
};

struct SettingItem
{
    SettingField field = SettingField::ShowLatency;
    SettingKind kind = SettingKind::Toggle;
    std::wstring title;
    std::wstring subtitle;
    std::wstring providerId;
    std::wstring settingKey;
};

// A flattened list of section headers and interactive rows. Both the painter and
// the hit-tester walk this same list, so they cannot disagree about what is where.
struct SettingRow
{
    bool isHeader = false;
    std::wstring header;
    SettingItem item;
};

SettingRow makeSettingHeader(std::wstring text);
SettingRow makeSettingItem(SettingField field, SettingKind kind, std::wstring title, std::wstring subtitle,
                           std::wstring providerId = L"", std::wstring settingKey = L"");

const std::vector<SettingRow>& settingRows();

// Index of the first selectable (non-header) row, and neighbour lookups that skip headers.
int firstSelectableRow();
int nextSelectableRow(int from, int direction);

bool isToggleSetting(SettingField field);
bool isNumericSetting(SettingField field);

struct StepperSpec
{
    int minValue = 0;
    int maxValue = 0;
    const int* ladder = nullptr;   // when set, +/- walks these stops instead of adding step
    int ladderCount = 0;
    int step = 1;
};

StepperSpec stepperSpec(SettingField field);
int settingNumericValue(SettingField field, const Settings& settings);
void setSettingNumericValue(SettingField field, int value);

const wchar_t* const* settingChoices(SettingField field, int& count);
int settingChoiceIndex(SettingField field, const Settings& settings);

bool settingToggleValue(SettingField field, const Settings& settings);
std::wstring settingValueText(SettingField field, const Settings& settings);
std::wstring settingValueText(const SettingItem& item, const Settings& settings);

std::wstring providerShortcutValue(const std::wstring& providerId);
void setProviderShortcutValue(const std::wstring& providerId, const std::wstring& shortcut);

bool settingNeedsRebuild(SettingField field);
bool settingNeedsRepaintOnly(SettingField field);

struct SettingChange
{
    bool needsRebuild = false;
    bool needsThemeRefresh = false;
};

// Mutates, normalizes, and persists in one step, then reports what the UI owes as
// follow-up. direction is -1 / +1 for steppers and choices; either value flips a toggle.
SettingChange applySetting(SettingField field, int direction);
