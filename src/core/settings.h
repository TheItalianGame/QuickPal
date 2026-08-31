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
    WindowPosition,
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
    BitwardenSearchUsernames,
    BitwardenUnlockWithPin,
    BitwardenUnlockWithHello,
    BitwardenRequireMasterOnRestart,
    BitwardenUseServe,
    BitwardenLockOnSleep,
    BitwardenLockOnExit,
    BitwardenPinTimeoutSeconds,
    BitwardenClipboardClearSeconds,
    MaxResults,
    ShowLatency,
    ProviderShortcut,
    ProviderPrefix,
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

// Settings are grouped into stable pages. General is first; every provider that
// contributes rows owns one page selected from the right-side settings rail.
struct SettingSection
{
    std::wstring id;
    std::wstring title;
    std::wstring subtitle;
    std::vector<SettingRow> rows;
};

SettingRow makeSettingHeader(std::wstring text);
SettingRow makeSettingItem(SettingField field, SettingKind kind, std::wstring title, std::wstring subtitle,
                           std::wstring providerId = L"", std::wstring settingKey = L"");

const std::vector<SettingSection>& settingSections();
const SettingSection& settingSection(int index);
int settingSectionIndex(const std::wstring& id);

// Index of the first selectable (non-header) row, and neighbour lookups that skip headers.
int firstSelectableRow(const std::vector<SettingRow>& rows);
int nextSelectableRow(const std::vector<SettingRow>& rows, int from, int direction);

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
std::wstring providerPrefixValue(const std::wstring& providerId);
void setProviderPrefixValue(const std::wstring& providerId, const std::wstring& prefix);
std::wstring providerPrefixConflict(const std::wstring& providerId, const std::wstring& prefix);
void setBitwardenAccountEmail(const std::wstring& email);

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
