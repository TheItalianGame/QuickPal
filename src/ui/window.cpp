#include "window.h"

#include "../app.h"
#include "../core/chrome_bridge.h"
#include "../core/clipboard_history.h"
#include "../core/frecency.h"
#include "../core/indexer.h"
#include "../core/provider.h"
#include "../core/search.h"
#include "../core/settings.h"
#include "../core/shortcuts.h"
#include "../core/util.h"
#include "icons.h"
#include "layout.h"
#include "paint.h"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <sstream>

namespace
{
// Not in MinGW's dwmapi.h yet; all four are supported on Windows 11.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCornerRound = 2;
constexpr int kProviderHotkeyBase = 8000;

std::vector<std::wstring> g_providerHotkeyIds;

void executeCommand(const Command& command);
void beginShortcutCapture(const std::wstring& providerId);
void beginPrefixEdit(const std::wstring& providerId);

COLORREF toColorRef(const D2D1_COLOR_F& color)
{
    return RGB(static_cast<BYTE>(color.r * 255.0f + 0.5f),
               static_cast<BYTE>(color.g * 255.0f + 0.5f),
               static_cast<BYTE>(color.b * 255.0f + 0.5f));
}

bool ctrlDown()
{
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool shiftDown()
{
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}

bool altDown()
{
    return (GetKeyState(VK_MENU) & 0x8000) != 0;
}

void invalidate()
{
    if (g_app.hwnd)
    {
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
    }
}

void rememberPreviousForeground()
{
    if (g_app.visible)
    {
        return;
    }
    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != g_app.hwnd)
    {
        g_app.previousForeground = foreground;
    }
}

void cancelShortcutCapture()
{
    g_app.capturingShortcut = false;
    g_app.capturingShortcutProvider.clear();
}

void cancelPrefixEdit()
{
    g_app.editingPrefix = false;
    g_app.prefixProvider.clear();
    g_app.prefixBuffer.clear();
}

std::wstring providerIdForHotkey(WPARAM id)
{
    if (id < kProviderHotkeyBase)
    {
        return {};
    }
    const size_t index = static_cast<size_t>(id - kProviderHotkeyBase);
    return index < g_providerHotkeyIds.size() ? g_providerHotkeyIds[index] : std::wstring{};
}

void unregisterProviderHotkeys(HWND hwnd)
{
    for (size_t i = 0; i < g_providerHotkeyIds.size(); ++i)
    {
        UnregisterHotKey(hwnd, kProviderHotkeyBase + static_cast<int>(i));
    }
    g_providerHotkeyIds.clear();
}

void registerProviderHotkeys(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }

    unregisterProviderHotkeys(hwnd);
    const Settings settings = getSettingsSnapshot();
    for (const auto& entry : ProviderRegistry::instance().entries())
    {
        const std::wstring providerId = entry.info.id;
        const auto it = settings.providerShortcuts.find(providerId);
        if (it == settings.providerShortcuts.end())
        {
            continue;
        }

        const auto shortcut = parseShortcutText(it->second);
        if (!shortcut)
        {
            continue;
        }

        const int hotkeyId = kProviderHotkeyBase + static_cast<int>(g_providerHotkeyIds.size());
        if (RegisterHotKey(hwnd, hotkeyId, shortcut->modifiers | MOD_NOREPEAT, shortcut->vk))
        {
            g_providerHotkeyIds.push_back(providerId);
        }
        else
        {
            setTransientStatus(L"Shortcut unavailable: " + shortcut->text);
        }
    }
}

void resetCaretBlink()
{
    g_app.caretVisible = true;
    if (g_app.hwnd && g_app.visible)
    {
        UINT blink = GetCaretBlinkTime();
        if (blink == 0 || blink == INFINITE)
        {
            blink = 530;
        }
        SetTimer(g_app.hwnd, kCaretTimerId, blink, nullptr);
    }
}

// The palette rides on DWM for rounded corners and its shadow, which is why the
// old SetWindowRgn clipping (and its stair-stepped edges) is gone.
void applyWindowChrome(HWND hwnd, const Theme& theme)
{
    DWORD corner = kDwmCornerRound;
    DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));

    BOOL dark = theme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));

    COLORREF border = toColorRef(theme.border);
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &border, sizeof(border));
}

D2D1_POINT_2F clientPointDip(LPARAM lParam)
{
    return D2D1::Point2F(pxToDip(GET_X_LPARAM(lParam), g_app.dpi),
                         pxToDip(GET_Y_LPARAM(lParam), g_app.dpi));
}

int hitTestPaletteRow(const PaletteLayout& layout, float x, float y)
{
    const int count = std::min<int>(static_cast<int>(g_app.results.size()), static_cast<int>(layout.rows.size()));
    for (int i = 0; i < count; ++i)
    {
        if (pointInRect(layout.rows[static_cast<size_t>(i)].row, x, y))
        {
            return i;
        }
    }
    return -1;
}

struct SettingsHit
{
    int row = -1;
    int section = -1;
    PressedPart part = PressedPart::None;
    int segment = -1;
};

SettingsHit hitTestSettings(const SettingsLayout& layout, float x, float y)
{
    SettingsHit hit;
    for (int i = 0; i < static_cast<int>(layout.railRows.size()); ++i)
    {
        if (pointInRect(layout.railRows[static_cast<size_t>(i)], x, y))
        {
            hit.section = i;
            hit.part = PressedPart::SettingsSection;
            return hit;
        }
    }
    if (!pointInRect(layout.listArea, x, y))
    {
        return hit;
    }

    const auto& rows = activeSettingRows();
    for (size_t i = 0; i < layout.rows.size() && i < rows.size(); ++i)
    {
        const SettingsRowRects& r = layout.rows[i];
        if (r.isHeader || !pointInRect(r.row, x, y))
        {
            continue;
        }

        hit.row = static_cast<int>(i);
        switch (rows[i].item.kind)
        {
        case SettingKind::Toggle:
            // The whole row is a deliberate target for toggles, matching Windows
            // Settings. Steppers below are the opposite: buttons only.
            hit.part = PressedPart::Toggle;
            break;
        case SettingKind::Stepper:
            if (pointInRect(r.minus, x, y))
            {
                hit.part = PressedPart::Minus;
            }
            else if (pointInRect(r.plus, x, y))
            {
                hit.part = PressedPart::Plus;
            }
            break;
        case SettingKind::Choice:
            for (int s = 0; s < r.segmentCount; ++s)
            {
                if (pointInRect(r.segments[s], x, y))
                {
                    hit.part = PressedPart::Segment;
                    hit.segment = s;
                    break;
                }
            }
            break;
        case SettingKind::Action:
            if (pointInRect(r.action, x, y))
            {
                hit.part = PressedPart::Action;
            }
            break;
        }
        return hit;
    }
    return hit;
}

size_t queryPositionFromPoint(const PaletteLayout& layout, float x, float y, bool* isTrailing)
{
    if (isTrailing)
    {
        *isTrailing = false;
    }
    const std::wstring& text = g_app.editor.text();
    if (text.empty())
    {
        return 0;
    }

    IDWriteTextLayout* textLayout = g_app.graphics.layout(
        text, FontRole::Query, 100000.0f, layout.queryText.bottom - layout.queryText.top, 0xC4E7);
    if (!textLayout)
    {
        return text.size();
    }

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    const float localX = x - layout.queryText.left + g_app.queryScrollX;
    const float localY = y - layout.queryText.top;
    if (FAILED(textLayout->HitTestPoint(localX, localY, &trailing, &inside, &metrics)))
    {
        return text.size();
    }

    if (isTrailing)
    {
        *isTrailing = trailing != FALSE;
    }
    return std::min<size_t>(metrics.textPosition + (trailing ? 1 : 0), text.size());
}

void clampSelection()
{
    const int count = static_cast<int>(g_app.results.size());
    g_app.selected = count == 0 ? 0 : std::clamp(g_app.selected, 0, count - 1);
}

void scrollSettingsSelectionIntoView()
{
    const SettingsLayout layout = buildSettingsLayout();
    g_app.settingsScroll = settingsScrollToReveal(layout, g_app.settingsSelected, g_app.settingsScroll);
}

void openPathOrUri(const std::wstring& path)
{
    if (!path.empty())
    {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

bool isUrlLike(const std::wstring& value)
{
    const std::wstring lower = lowerCopy(value);
    return startsWith(lower, L"http://") || startsWith(lower, L"https://") ||
           startsWith(lower, L"ms-settings:") || startsWith(lower, L"shell:");
}

bool isInternalArg(const std::wstring& value)
{
    return startsWith(lowerCopy(value), L"system:") || startsWith(lowerCopy(value), L"value:");
}

bool hasFilesystemArg(const Command& command)
{
    if (command.arg.empty() || isUrlLike(command.arg) || isInternalArg(command.arg))
    {
        return false;
    }
    return command.kind == CommandKind::File || command.kind == CommandKind::Folder ||
           command.kind == CommandKind::App || command.kind == CommandKind::PathTool ||
           command.kind == CommandKind::Builtin;
}

std::wstring quoted(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

void runShellCommandWithVerb(const std::wstring& command, const wchar_t* verb)
{
    const Settings settings = getSettingsSnapshot();
    if (settings.shellUsesPowerShell)
    {
        const std::wstring params = L"-NoExit -NoProfile -ExecutionPolicy Bypass -Command " + command;
        ShellExecuteW(nullptr, verb, L"powershell.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    }
    else
    {
        const std::wstring params = L"/d /k " + command;
        ShellExecuteW(nullptr, verb, L"cmd.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

bool runAsAdministrator(const Command& command)
{
    if (command.kind == CommandKind::Shell)
    {
        runShellCommandWithVerb(command.arg, L"runas");
        return true;
    }
    if (!hasFilesystemArg(command))
    {
        return false;
    }
    ShellExecuteW(nullptr, L"runas", command.arg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

bool paletteUsesDetailRows()
{
    if (g_app.queryMode == QueryMode::Files)
    {
        return true;
    }
    for (const auto& result : g_app.results)
    {
        const CommandKind kind = result.command.kind;
        if ((kind == CommandKind::File || kind == CommandKind::Folder) && !result.command.data.empty())
        {
            return true;
        }
    }
    return false;
}

bool drillIntoSelectedFolder()
{
    if (g_app.results.empty())
    {
        return false;
    }
    clampSelection();
    const Command& command = g_app.results[static_cast<size_t>(g_app.selected)].command;
    if (command.kind != CommandKind::Folder || command.arg.empty())
    {
        return false;
    }

    std::wstring path = command.arg;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
    {
        path += L"\\";
    }
    g_app.editor.setText(L"f " + path);
    g_app.selected = 0;
    refreshResults();
    return true;
}

void openContainingFolder(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }
    const std::wstring params = L"/select," + quoted(path);
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

void openWithDialog(const std::wstring& path)
{
    if (path.empty())
    {
        return;
    }
    const std::wstring params = L"shell32.dll,OpenAs_RunDLL " + quoted(path);
    ShellExecuteW(nullptr, L"open", L"rundll32.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

void terminateProcessId(DWORD processId)
{
    if (processId == 0)
    {
        return;
    }
    if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, processId))
    {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}

bool parseChromeTabData(const std::wstring& data, int& windowId, int& tabId)
{
    const size_t split = data.find(L'\t');
    if (split == std::wstring::npos)
    {
        return false;
    }
    windowId = _wtoi(data.substr(0, split).c_str());
    tabId = _wtoi(data.substr(split + 1).c_str());
    return windowId > 0 && tabId > 0;
}

std::wstring providerTitleForId(const wchar_t* id)
{
    if (!id || !*id)
    {
        return {};
    }
    if (Provider* provider = ProviderRegistry::instance().byId(id))
    {
        if (const ProviderInfo* info = ProviderRegistry::instance().infoFor(provider))
        {
            return info->title;
        }
    }
    return id;
}

std::wstring baseNameForCommand(const Command& command)
{
    if (hasFilesystemArg(command))
    {
        return fileNameFromPath(command.arg);
    }
    if (!command.title.empty())
    {
        return command.title;
    }
    return command.arg;
}

bool isPasteableTextCommand(const Command& command)
{
    return (command.kind == CommandKind::Clipboard || command.kind == CommandKind::Snippet ||
            command.kind == CommandKind::Calc) && !command.arg.empty();
}

void pastePlainTextToPreviousWindow(const std::wstring& text)
{
    HWND target = g_app.previousForeground;
    if (target == g_app.hwnd)
    {
        target = nullptr;
    }
    ShowWindow(g_app.hwnd, SW_HIDE);
    pasteTextToWindow(g_app.hwnd, target, text);
}

void focusWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return;
    }
    if (IsIconic(hwnd))
    {
        ShowWindow(hwnd, SW_RESTORE);
    }
    SetForegroundWindow(hwnd);
}

void restoreQuickPalWindow()
{
    if (!g_app.hwnd || !IsWindow(g_app.hwnd))
    {
        return;
    }
    g_app.visible = true;
    ShowWindow(g_app.hwnd, IsIconic(g_app.hwnd) ? SW_RESTORE : SW_SHOWNORMAL);
    SetWindowPos(g_app.hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_app.hwnd);
    SetFocus(g_app.hwnd);
    resetCaretBlink();
    invalidate();
}

void reportProviderStatus(HWND window, const wchar_t* providerId, const std::wstring& message)
{
    const std::wstring title = providerTitleForId(providerId);
    setProviderStatus(title.empty() ? L"Provider" : title, message);
    if (window && window == g_app.hwnd && g_app.visible &&
        GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId())
    {
        restoreQuickPalWindow();
        UpdateWindow(window);
    }
}

RECT monitorWorkAreaForWindow(HWND hwnd)
{
    RECT fallback{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    return GetMonitorInfoW(monitor, &info) ? info.rcWork : fallback;
}

void snapWindow(HWND hwnd, bool right)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return;
    }
    ShowWindow(hwnd, SW_RESTORE);
    const RECT work = monitorWorkAreaForWindow(hwnd);
    const int width = std::max<int>(1, work.right - work.left);
    const int height = std::max<int>(1, work.bottom - work.top);
    const int half = std::max(1, width / 2);
    const int x = right ? work.right - half : work.left;
    SetWindowPos(hwnd, nullptr, x, work.top, half, height, SWP_NOZORDER | SWP_NOACTIVATE);
    focusWindow(hwnd);
}

void centerWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return;
    }
    ShowWindow(hwnd, SW_RESTORE);
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect))
    {
        return;
    }
    const RECT work = monitorWorkAreaForWindow(hwnd);
    const int width = std::min(std::max<int>(1, rect.right - rect.left), std::max<int>(1, work.right - work.left));
    const int height = std::min(std::max<int>(1, rect.bottom - rect.top), std::max<int>(1, work.bottom - work.top));
    const int x = work.left + std::max<int>(0, ((work.right - work.left) - width) / 2);
    const int y = work.top + std::max<int>(0, ((work.bottom - work.top) - height) / 2);
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    focusWindow(hwnd);
}

wchar_t preferredActionShortcut(ActionKind action)
{
    switch (action)
    {
    case ActionKind::Open: return L'O';
    case ActionKind::RunAsAdministrator: return L'A';
    case ActionKind::OpenContainingFolder: return L'F';
    case ActionKind::CopyPath: return L'C';
    case ActionKind::CopyName: return L'N';
    case ActionKind::CopyTitle: return L'T';
    case ActionKind::CopySubtitle: return L'S';
    case ActionKind::OpenWith: return L'W';
    case ActionKind::KillProcess: return L'K';
    case ActionKind::PasteText: return L'P';
    case ActionKind::PinClipboard: return L'N';
    case ActionKind::UnpinClipboard: return L'U';
    case ActionKind::CloseBrowserTab: return L'X';
    case ActionKind::ReloadBrowserTab: return L'R';
    case ActionKind::WindowMinimize: return L'I';
    case ActionKind::WindowMaximizeRestore: return L'M';
    case ActionKind::WindowSnapLeft: return L'L';
    case ActionKind::WindowSnapRight: return L'R';
    case ActionKind::WindowCenter: return L'C';
    case ActionKind::WindowClose: return L'X';
    case ActionKind::BitwardenCopyUsername: return L'U';
    case ActionKind::BitwardenCopyPassword: return L'P';
    case ActionKind::BitwardenCopyTotp: return L'T';
    case ActionKind::BitwardenOpenSite: return L'O';
    case ActionKind::BitwardenOpenItem: return L'B';
    case ActionKind::ConfigureProvider: return L'G';
    case ActionKind::SetProviderShortcut: return L'K';
    case ActionKind::SetProviderPrefix: return L'E';
    default: return 0;
    }
}

bool actionShortcutUsed(const std::vector<Result>& rows, wchar_t key)
{
    return std::any_of(rows.begin(), rows.end(), [key](const Result& row) {
        return row.command.shortcutKey == key;
    });
}

wchar_t chooseActionShortcut(const std::vector<Result>& rows, ActionKind action,
                             const std::wstring& title)
{
    const wchar_t preferred = preferredActionShortcut(action);
    if (preferred != 0 && !actionShortcutUsed(rows, preferred))
    {
        return preferred;
    }

    for (wchar_t ch : title)
    {
        const wchar_t candidate = static_cast<wchar_t>(std::towupper(ch));
        if (std::iswalnum(candidate) && !actionShortcutUsed(rows, candidate))
        {
            return candidate;
        }
    }

    for (wchar_t candidate = L'1'; candidate <= L'9'; ++candidate)
    {
        if (!actionShortcutUsed(rows, candidate))
        {
            return candidate;
        }
    }
    return 0;
}

void addAction(std::vector<Result>& rows, ActionKind action, const std::wstring& title,
               const std::wstring& subtitle, int score)
{
    Command command = makeCommand(CommandKind::Action, title, subtitle, L"", 0);
    command.action = action;
    command.shortcutKey = chooseActionShortcut(rows, action, title);
    command.targetKind = g_app.actionTarget.kind;
    command.processId = g_app.actionTarget.processId;
    command.hwnd = g_app.actionTarget.hwnd;
    rows.push_back(Result{ std::move(command), score });
}

void leaveActionMenu(bool restoreQuery)
{
    g_app.actionMenu = false;
    g_app.actionTarget = Command{};
    const std::wstring restore = g_app.actionReturnText;
    g_app.actionReturnText.clear();
    if (restoreQuery)
    {
        g_app.editor.setText(restore);
        g_app.selected = 0;
        refreshResults();
    }
    else
    {
        invalidate();
    }
}

bool selectProviderSettingRow(const std::wstring& providerId, SettingField field)
{
    g_app.settingsSection = settingSectionIndex(providerId);
    g_app.settingsScroll = 0.0f;
    const auto& rows = activeSettingRows();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        if (!rows[static_cast<size_t>(i)].isHeader &&
            rows[static_cast<size_t>(i)].item.providerId == providerId &&
            rows[static_cast<size_t>(i)].item.field == field)
        {
            g_app.settingsSelected = i;
            scrollSettingsSelectionIntoView();
            invalidate();
            return true;
        }
    }
    return false;
}

void lockBitwardenSession()
{
    Provider* provider = ProviderRegistry::instance().byId(L"bitwarden");
    if (!provider)
    {
        return;
    }
    Command command = makeCommand(CommandKind::BitwardenControl, L"Lock Bitwarden", L"", L"lock", 0);
    command.provider = L"bitwarden";
    executeThroughProvider(command, getSettingsSnapshot(), g_app.hwnd, g_app.previousForeground,
                           reportProviderStatus);
}

void showProviderSettings(const std::wstring& providerId, SettingField field)
{
    if (providerId.empty())
    {
        return;
    }
    showSettings();
    selectProviderSettingRow(providerId, field);
}

void showActionsForSelected()
{
    if (g_app.results.empty())
    {
        return;
    }
    clampSelection();

    g_app.actionTarget = g_app.results[static_cast<size_t>(g_app.selected)].command;
    g_app.actionReturnText = g_app.editor.text();
    g_app.actionMenu = true;
    g_app.queryMode = QueryMode::Actions;
    g_app.highlightTerms.clear();
    g_app.results.clear();
    g_app.selected = 0;
    g_app.hovered = -1;

    const bool bitwardenTarget = g_app.actionTarget.kind == CommandKind::BitwardenItem;
    if (g_app.actionTarget.kind == CommandKind::Process)
    {
        addAction(g_app.results, ActionKind::KillProcess, L"Kill process", g_app.actionTarget.subtitle, 30000);
    }
    else if (!bitwardenTarget)
    {
        addAction(g_app.results, ActionKind::Open, L"Open", g_app.actionTarget.title, 30000);
    }

    if (isPasteableTextCommand(g_app.actionTarget))
    {
        addAction(g_app.results, ActionKind::PasteText, L"Paste as plain text",
                  g_app.actionTarget.title, 29900);
    }

    if (g_app.actionTarget.kind == CommandKind::Shell || hasFilesystemArg(g_app.actionTarget))
    {
        addAction(g_app.results, ActionKind::RunAsAdministrator, L"Run as administrator",
                  baseNameForCommand(g_app.actionTarget), 29800);
    }

    if (g_app.actionTarget.kind == CommandKind::Window)
    {
        addAction(g_app.results, ActionKind::WindowMinimize, L"Minimize window",
                  g_app.actionTarget.title, 29780);
        addAction(g_app.results, ActionKind::WindowMaximizeRestore, L"Maximize or restore window",
                  g_app.actionTarget.title, 29770);
        addAction(g_app.results, ActionKind::WindowSnapLeft, L"Snap window left",
                  g_app.actionTarget.title, 29760);
        addAction(g_app.results, ActionKind::WindowSnapRight, L"Snap window right",
                  g_app.actionTarget.title, 29750);
        addAction(g_app.results, ActionKind::WindowCenter, L"Center window",
                  g_app.actionTarget.title, 29740);
        addAction(g_app.results, ActionKind::WindowClose, L"Close window",
                  g_app.actionTarget.title, 29730);
    }

    int chromeWindowId = 0;
    int chromeTabId = 0;
    if (g_app.actionTarget.kind == CommandKind::ChromeTab &&
        parseChromeTabData(g_app.actionTarget.data, chromeWindowId, chromeTabId))
    {
        addAction(g_app.results, ActionKind::ReloadBrowserTab, L"Reload tab",
                  g_app.actionTarget.title, 29780);
        addAction(g_app.results, ActionKind::CloseBrowserTab, L"Close tab",
                  g_app.actionTarget.title, 29770);
    }

    if (g_app.actionTarget.kind == CommandKind::Clipboard)
    {
        const bool pinned = isClipboardTextPinned(g_app.actionTarget.arg);
        addAction(g_app.results, pinned ? ActionKind::UnpinClipboard : ActionKind::PinClipboard,
                  pinned ? L"Unpin clipboard item" : L"Pin clipboard item",
                  g_app.actionTarget.title, 29750);
    }

    if (g_app.actionTarget.kind == CommandKind::BitwardenItem)
    {
        addAction(g_app.results, ActionKind::BitwardenCopyPassword, L"Copy password",
                  g_app.actionTarget.title, 29790);
        addAction(g_app.results, ActionKind::BitwardenCopyUsername, L"Copy username",
                  g_app.actionTarget.title, 29780);
        if (g_app.actionTarget.hasTotp)
        {
            addAction(g_app.results, ActionKind::BitwardenCopyTotp, L"Copy TOTP",
                      g_app.actionTarget.title, 29770);
        }
        addAction(g_app.results, ActionKind::BitwardenOpenSite, L"Open site",
                  g_app.actionTarget.arg.empty() ? g_app.actionTarget.title : g_app.actionTarget.arg, 29760);
        addAction(g_app.results, ActionKind::BitwardenOpenItem, L"Open in Bitwarden",
                  g_app.actionTarget.title, 29750);
    }

    if (hasFilesystemArg(g_app.actionTarget))
    {
        addAction(g_app.results, ActionKind::OpenContainingFolder, L"Open containing folder",
                  g_app.actionTarget.arg, 29700);
        addAction(g_app.results, ActionKind::CopyPath, L"Copy path",
                  g_app.actionTarget.arg, 29600);
        addAction(g_app.results, ActionKind::CopyName, L"Copy name",
                  baseNameForCommand(g_app.actionTarget), 29500);
        if (g_app.actionTarget.kind == CommandKind::File)
        {
            addAction(g_app.results, ActionKind::OpenWith, L"Open with",
                      baseNameForCommand(g_app.actionTarget), 29400);
        }
    }
    else if (!bitwardenTarget && !g_app.actionTarget.arg.empty())
    {
        addAction(g_app.results, ActionKind::CopyPath, L"Copy value", g_app.actionTarget.arg, 29600);
    }

    if (!bitwardenTarget && !g_app.actionTarget.title.empty())
    {
        addAction(g_app.results, ActionKind::CopyTitle, L"Copy title",
                  g_app.actionTarget.title, 29300);
    }
    if (!bitwardenTarget && !g_app.actionTarget.subtitle.empty())
    {
        addAction(g_app.results, ActionKind::CopySubtitle, L"Copy subtitle",
                  g_app.actionTarget.subtitle, 29200);
    }

    if (g_app.actionTarget.provider && *g_app.actionTarget.provider)
    {
        const std::wstring providerTitle = providerTitleForId(g_app.actionTarget.provider);
        addAction(g_app.results, ActionKind::ConfigureProvider, L"Open provider settings",
                  providerTitle, 28900);
        addAction(g_app.results, ActionKind::SetProviderShortcut, L"Set provider shortcut",
                  providerTitle, 28800);
        addAction(g_app.results, ActionKind::SetProviderPrefix, L"Set provider prefix",
                  providerTitle, 28700);
    }

    positionWindow();
    invalidate();
}

bool executeActionCommand(const Command& action)
{
    if (action.kind != CommandKind::Action)
    {
        return false;
    }

    const Command target = g_app.actionTarget;
    switch (action.action)
    {
    case ActionKind::Open:
        leaveActionMenu(false);
        executeCommand(target);
        return true;
    case ActionKind::RunAsAdministrator:
        recordCommandLaunch(target);
        runAsAdministrator(target);
        break;
    case ActionKind::OpenContainingFolder:
        openContainingFolder(target.arg);
        break;
    case ActionKind::CopyPath:
        copyTextToClipboard(g_app.hwnd, target.arg);
        break;
    case ActionKind::CopyName:
        copyTextToClipboard(g_app.hwnd, baseNameForCommand(target));
        break;
    case ActionKind::CopyTitle:
        copyTextToClipboard(g_app.hwnd, target.title);
        break;
    case ActionKind::CopySubtitle:
        copyTextToClipboard(g_app.hwnd, target.subtitle);
        break;
    case ActionKind::OpenWith:
        openWithDialog(target.arg);
        break;
    case ActionKind::KillProcess:
        terminateProcessId(target.processId);
        break;
    case ActionKind::PasteText:
        pastePlainTextToPreviousWindow(target.arg);
        break;
    case ActionKind::PinClipboard:
        pinClipboardText(target.arg);
        break;
    case ActionKind::UnpinClipboard:
        unpinClipboardText(target.arg);
        break;
    case ActionKind::CloseBrowserTab:
    case ActionKind::ReloadBrowserTab:
    {
        int windowId = 0;
        int tabId = 0;
        if (parseChromeTabData(target.data, windowId, tabId))
        {
            if (action.action == ActionKind::CloseBrowserTab)
            {
                closeChromeTab(windowId, tabId);
            }
            else
            {
                reloadChromeTab(windowId, tabId);
            }
        }
        break;
    }
    case ActionKind::WindowMinimize:
        if (IsWindow(target.hwnd))
        {
            ShowWindow(target.hwnd, SW_MINIMIZE);
        }
        break;
    case ActionKind::WindowMaximizeRestore:
        if (IsWindow(target.hwnd))
        {
            ShowWindow(target.hwnd, IsZoomed(target.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            focusWindow(target.hwnd);
        }
        break;
    case ActionKind::WindowSnapLeft:
        snapWindow(target.hwnd, false);
        break;
    case ActionKind::WindowSnapRight:
        snapWindow(target.hwnd, true);
        break;
    case ActionKind::WindowCenter:
        centerWindow(target.hwnd);
        break;
    case ActionKind::WindowClose:
        if (IsWindow(target.hwnd))
        {
            PostMessageW(target.hwnd, WM_CLOSE, 0, 0);
        }
        break;
    case ActionKind::BitwardenCopyUsername:
    case ActionKind::BitwardenCopyPassword:
    case ActionKind::BitwardenCopyTotp:
    case ActionKind::BitwardenOpenSite:
    case ActionKind::BitwardenOpenItem:
    {
        Command routed = target;
        routed.action = action.action;
        executeThroughProvider(routed, getSettingsSnapshot(), g_app.hwnd, g_app.previousForeground,
                               reportProviderStatus);
        break;
    }
    case ActionKind::ConfigureProvider:
        leaveActionMenu(false);
        showProviderSettings(target.provider ? target.provider : L"", SettingField::ProviderShortcut);
        return true;
    case ActionKind::SetProviderShortcut:
        leaveActionMenu(false);
        showProviderSettings(target.provider ? target.provider : L"", SettingField::ProviderShortcut);
        beginShortcutCapture(target.provider ? target.provider : L"");
        return true;
    case ActionKind::SetProviderPrefix:
        leaveActionMenu(false);
        showProviderSettings(target.provider ? target.provider : L"", SettingField::ProviderPrefix);
        beginPrefixEdit(target.provider ? target.provider : L"");
        return true;
    default:
        return false;
    }

    hidePalette();
    return true;
}

void executeSelectedAsAdmin()
{
    if (g_app.results.empty() && !g_app.actionMenu)
    {
        return;
    }
    const Command target = g_app.actionMenu
        ? g_app.actionTarget
        : g_app.results[static_cast<size_t>(std::clamp(g_app.selected, 0, static_cast<int>(g_app.results.size()) - 1))].command;
    if (runAsAdministrator(target))
    {
        recordCommandLaunch(target);
        hidePalette();
    }
}

void quitApp()
{
    Shell_NotifyIconW(NIM_DELETE, &g_app.tray);
    PostQuitMessage(0);
}

void executeCommand(const Command& command)
{
    recordCommandLaunch(command);

    // Kinds that drive the palette itself need the window, so they stay here.
    // Everything else belongs to the provider that produced it.
    switch (command.kind)
    {
    case CommandKind::Action:
        executeActionCommand(command);
        return;
    case CommandKind::OpenSettings:
        showSettings();
        return;
    case CommandKind::OpenCalculator:
        g_app.mode = UiMode::Palette;
        g_app.editor.setText(L"=");
        g_app.selected = 0;
        refreshResults();
        positionWindow();
        return;
    case CommandKind::PaletteQuery:
        g_app.mode = UiMode::Palette;
        g_app.editor.setText(command.arg);
        g_app.selected = 0;
        refreshResults();
        positionWindow();
        return;
    case CommandKind::ReloadIndex:
        rebuildIndexAsync();
        return;
    case CommandKind::ExitApp:
        quitApp();
        return;
    default:
        break;
    }

    const bool hideBeforeProvider = command.kind == CommandKind::Clipboard || command.kind == CommandKind::Snippet;
    if (hideBeforeProvider)
    {
        ShowWindow(g_app.hwnd, SW_HIDE);
    }

    const std::wstring providerAction = command.data.empty() ? command.arg : command.data;
    const bool keepBitwardenSearch = command.kind == CommandKind::BitwardenControl && command.provider &&
        wcscmp(command.provider, L"bitwarden") == 0 &&
        (providerAction == L"connect" || providerAction == L"sync");
    const bool handledByProvider = executeThroughProvider(command, getSettingsSnapshot(), g_app.hwnd,
                                                          g_app.previousForeground, reportProviderStatus);
    if (!handledByProvider && command.kind != CommandKind::BitwardenControl)
    {
        // Shared default: the command names a path or a URI.
        openPathOrUri(command.arg);
    }
    if (keepBitwardenSearch)
    {
        refreshResults();
        restoreQuickPalWindow();
        return;
    }
    hidePalette();
}

void executeSelected()
{
    if (g_app.results.empty())
    {
        return;
    }
    clampSelection();
    executeCommand(g_app.results[static_cast<size_t>(g_app.selected)].command);
}

void commitNumberEdit()
{
    if (!g_app.editingNumber)
    {
        return;
    }
    const auto& rows = activeSettingRows();
    const int index = g_app.settingsSelected;
    if (index >= 0 && index < static_cast<int>(rows.size()) && !rows[index].isHeader &&
        rows[index].item.kind == SettingKind::Stepper && !g_app.numberBuffer.empty())
    {
        const int value = _wtoi(g_app.numberBuffer.c_str());
        const SettingField field = rows[index].item.field;
        setSettingNumericValue(field, value);
        if (settingNeedsRebuild(field))
        {
            rebuildIndexAsync();
        }
    }
    g_app.editingNumber = false;
    g_app.numberBuffer.clear();
}

void cancelNumberEdit()
{
    g_app.editingNumber = false;
    g_app.numberBuffer.clear();
}

void switchSettingsSection(int index)
{
    const auto& sections = settingSections();
    if (sections.empty())
    {
        return;
    }

    commitNumberEdit();
    cancelShortcutCapture();
    cancelPrefixEdit();
    g_app.settingsSection = std::clamp(index, 0, static_cast<int>(sections.size()) - 1);
    g_app.settingsSelected = firstSelectableRow(activeSettingRows());
    g_app.settingsHovered = -1;
    g_app.settingsScroll = 0.0f;
    g_app.pressedRow = -1;
    g_app.pressedSettingsSection = -1;
    g_app.pressedPart = PressedPart::None;
    invalidate();
}

void beginShortcutCapture(const std::wstring& providerId)
{
    cancelNumberEdit();
    cancelPrefixEdit();
    g_app.capturingShortcut = true;
    g_app.capturingShortcutProvider = providerId;
    setTransientStatus(L"Press a shortcut. Backspace clears, Esc cancels.");
    invalidate();
}

void finishShortcutCapture(WPARAM key)
{
    if (!g_app.capturingShortcut)
    {
        return;
    }

    if (key == VK_ESCAPE)
    {
        cancelShortcutCapture();
        clearTransientStatus();
        invalidate();
        return;
    }

    if (key == VK_BACK || key == VK_DELETE)
    {
        setProviderShortcutValue(g_app.capturingShortcutProvider, L"");
        cancelShortcutCapture();
        registerProviderHotkeys(g_app.hwnd);
        setTransientStatus(L"Provider shortcut cleared.");
        invalidate();
        return;
    }

    if (isModifierKey(key))
    {
        return;
    }

    const auto shortcut = shortcutFromKeyState(key);
    if (!shortcut)
    {
        setTransientStatus(L"Shortcut needs Ctrl, Alt, Shift, or Win.");
        invalidate();
        return;
    }

    setProviderShortcutValue(g_app.capturingShortcutProvider, shortcut->text);
    cancelShortcutCapture();
    registerProviderHotkeys(g_app.hwnd);
    setTransientStatus(L"Provider shortcut set to " + shortcut->text + L".");
    invalidate();
}

bool commitPrefixEdit()
{
    if (!g_app.editingPrefix)
    {
        return true;
    }

    const std::wstring prefix = normalizeProviderPrefix(g_app.prefixBuffer);
    const std::wstring conflict = providerPrefixConflict(g_app.prefixProvider, prefix);
    if (!conflict.empty())
    {
        setTransientStatus(L"Prefix already used by " + conflict + L".");
        invalidate();
        return false;
    }

    setProviderPrefixValue(g_app.prefixProvider, prefix);
    cancelPrefixEdit();
    setTransientStatus(prefix.empty() ? L"Provider prefix reset." : L"Provider prefix set to " + prefix + L".");
    invalidate();
    return true;
}

void beginPrefixEdit(const std::wstring& providerId)
{
    cancelNumberEdit();
    cancelShortcutCapture();
    g_app.editingPrefix = true;
    g_app.prefixProvider = providerId;

    const std::wstring current = providerPrefixValue(providerId);
    g_app.prefixBuffer = current == L"None" ? std::wstring{} : current;
    setTransientStatus(L"Type a prefix. Enter saves, Delete resets, Esc cancels.");
    invalidate();
}

void appendPrefixChar(wchar_t ch)
{
    if (!g_app.editingPrefix)
    {
        return;
    }
    if (std::iswspace(ch) || std::iswcntrl(ch))
    {
        setTransientStatus(L"Prefixes cannot contain spaces.");
        invalidate();
        return;
    }
    if (g_app.prefixBuffer.size() >= 16)
    {
        setTransientStatus(L"Prefixes are limited to 16 characters.");
        invalidate();
        return;
    }

    g_app.prefixBuffer.push_back(ch);
    g_app.prefixBuffer = normalizeProviderPrefix(g_app.prefixBuffer);
    invalidate();
}

void handlePrefixEditKey(WPARAM key)
{
    switch (key)
    {
    case VK_ESCAPE:
        cancelPrefixEdit();
        clearTransientStatus();
        invalidate();
        return;
    case VK_RETURN:
        commitPrefixEdit();
        return;
    case VK_UP:
        if (commitPrefixEdit())
        {
            g_app.settingsSelected = nextSelectableRow(activeSettingRows(), g_app.settingsSelected, -1);
            scrollSettingsSelectionIntoView();
            invalidate();
        }
        return;
    case VK_DOWN:
    case VK_TAB:
        if (commitPrefixEdit())
        {
            g_app.settingsSelected = nextSelectableRow(activeSettingRows(), g_app.settingsSelected, 1);
            scrollSettingsSelectionIntoView();
            invalidate();
        }
        return;
    case VK_BACK:
        if (!g_app.prefixBuffer.empty())
        {
            g_app.prefixBuffer.pop_back();
            invalidate();
        }
        return;
    case VK_DELETE:
        setProviderPrefixValue(g_app.prefixProvider, L"");
        cancelPrefixEdit();
        setTransientStatus(L"Provider prefix reset.");
        invalidate();
        return;
    default:
        return;
    }
}

void applySettingAt(int rowIndex, int direction)
{
    const auto& rows = activeSettingRows();
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()) || rows[rowIndex].isHeader)
    {
        return;
    }

    cancelNumberEdit();
    const SettingItem& item = rows[rowIndex].item;
    const SettingField field = item.field;
    if (field == SettingField::ProviderShortcut)
    {
        beginShortcutCapture(item.providerId);
        return;
    }
    if (field == SettingField::ProviderPrefix)
    {
        beginPrefixEdit(item.providerId);
        return;
    }
    if (field == SettingField::InstallChromeExtension)
    {
        openChromeExtensionInstallLocation();
        setTransientStatus(L"Opened Chrome extension folder and chrome://extensions.");
        invalidate();
        return;
    }
    if (field == SettingField::ProviderAction)
    {
        if (Provider* provider = ProviderRegistry::instance().byId(item.providerId.c_str()))
        {
            const ProviderContext ctx{ getSettingsSnapshot(), g_app.hwnd, g_app.previousForeground,
                                       reportProviderStatus };
            const bool applied = provider->applySetting(ctx, item);
            if (item.settingKey != L"install")
            {
                restoreQuickPalWindow();
            }
            if (applied)
            {
                setTransientStatus(item.title + L" complete.");
                invalidate();
                return;
            }
        }
        setTransientStatus(L"Provider action unavailable.");
        invalidate();
        return;
    }

    const SettingChange change = applySetting(field, direction);

    if (change.needsThemeRefresh)
    {
        refreshTheme();
        applyWindowChrome(g_app.hwnd, g_app.theme);
        g_app.graphics.clearLayoutCache();
    }
    if (change.needsRebuild)
    {
        rebuildIndexAsync();
    }
    if (field == SettingField::WindowPosition)
    {
        positionWindow();
    }
    invalidate();
}

void addTrayIcon(HWND hwnd)
{
    g_app.tray = {};
    g_app.tray.cbSize = sizeof(g_app.tray);
    g_app.tray.hWnd = hwnd;
    g_app.tray.uID = 1;
    g_app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_app.tray.uCallbackMessage = kTrayMessage;
    g_app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_app.tray.szTip, L"QuickPal");
    Shell_NotifyIconW(NIM_ADD, &g_app.tray);
}

void showTrayMenu(HWND hwnd)
{
    POINT pt{};
    GetCursorPos(&pt);
    const Settings settings = getSettingsSnapshot();

    HMENU menu = CreatePopupMenu();
    InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING, 1, L"Open QuickPal (Alt+Q)");
    InsertMenuW(menu, 1, MF_BYPOSITION | MF_STRING, 2, L"Settings");
    InsertMenuW(menu, 2, MF_BYPOSITION | MF_STRING, 3, L"Reload indexes");
    InsertMenuW(menu, 3, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(menu, 4, MF_BYPOSITION | MF_STRING, 4, L"Use PowerShell for > commands");
    InsertMenuW(menu, 5, MF_BYPOSITION | MF_STRING, 5, L"Use Everything SDK for file search");
    InsertMenuW(menu, 6, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    InsertMenuW(menu, 7, MF_BYPOSITION | MF_STRING, 6, L"Exit");
    CheckMenuItem(menu, 4, MF_BYCOMMAND | (settings.shellUsesPowerShell ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, 5, MF_BYCOMMAND | (settings.useEverything ? MF_CHECKED : MF_UNCHECKED));

    SetForegroundWindow(hwnd);
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    switch (choice)
    {
    case 1: showPalette(); break;
    case 2: showSettings(); break;
    case 3: rebuildIndexAsync(); break;
    case 4: applySetting(SettingField::ShellUsesPowerShell, 1); invalidate(); break;
    case 5: applySetting(SettingField::UseEverything, 1); rebuildIndexAsync(); break;
    case 6: quitApp(); break;
    default: break;
    }
}

void handlePaletteKey(WPARAM key)
{
    if (g_app.actionMenu && !ctrlDown() && !altDown())
    {
        const wchar_t shortcut = static_cast<wchar_t>(std::towupper(static_cast<wchar_t>(key)));
        if (std::iswalnum(shortcut))
        {
            for (int i = 0; i < static_cast<int>(g_app.results.size()); ++i)
            {
                const Command& action = g_app.results[static_cast<size_t>(i)].command;
                if (action.kind == CommandKind::Action && action.shortcutKey == shortcut)
                {
                    const Command command = action;
                    g_app.selected = i;
                    g_app.suppressNextCharacter = true;
                    executeCommand(command);
                    return;
                }
            }
        }
    }

    switch (key)
    {
    case VK_ESCAPE:
        if (g_app.actionMenu)
        {
            leaveActionMenu(true);
            return;
        }
        hidePalette();
        return;
    case VK_RETURN:
        executeSelected();
        return;
    case VK_UP:
        if (!g_app.results.empty())
        {
            g_app.selected = std::max(0, g_app.selected - 1);
            invalidate();
        }
        return;
    case VK_DOWN:
    case VK_TAB:
        if (!g_app.results.empty())
        {
            g_app.selected = std::min(static_cast<int>(g_app.results.size()) - 1, g_app.selected + 1);
            invalidate();
        }
        return;
    case VK_PRIOR:
        g_app.selected = 0;
        invalidate();
        return;
    case VK_NEXT:
        g_app.selected = std::max(0, static_cast<int>(g_app.results.size()) - 1);
        invalidate();
        return;
    case VK_LEFT:
        g_app.editor.moveLeft(ctrlDown(), shiftDown());
        resetCaretBlink();
        invalidate();
        return;
    case VK_RIGHT:
        if (!ctrlDown() && !shiftDown() && drillIntoSelectedFolder())
        {
            return;
        }
        g_app.editor.moveRight(ctrlDown(), shiftDown());
        resetCaretBlink();
        invalidate();
        return;
    case VK_HOME:
        if (ctrlDown())
        {
            g_app.selected = 0;
        }
        else
        {
            g_app.editor.moveHome(shiftDown());
            resetCaretBlink();
        }
        invalidate();
        return;
    case VK_END:
        if (ctrlDown())
        {
            g_app.selected = std::max(0, static_cast<int>(g_app.results.size()) - 1);
        }
        else
        {
            g_app.editor.moveEnd(shiftDown());
            resetCaretBlink();
        }
        invalidate();
        return;
    case VK_BACK:
        if (g_app.editor.backspace(ctrlDown()))
        {
            g_app.selected = 0;
            resetCaretBlink();
            refreshResults();
        }
        return;
    case VK_DELETE:
        if (g_app.editor.deleteForward(ctrlDown()))
        {
            g_app.selected = 0;
            resetCaretBlink();
            refreshResults();
        }
        return;
    default:
        break;
    }
}

void handleSettingsKey(WPARAM key)
{
    if (g_app.editingPrefix)
    {
        handlePrefixEditKey(key);
        return;
    }
    if (g_app.capturingShortcut)
    {
        finishShortcutCapture(key);
        return;
    }

    const auto& rows = activeSettingRows();

    if (ctrlDown() && key == VK_PRIOR)
    {
        switchSettingsSection(g_app.settingsSection - 1);
        return;
    }
    if (ctrlDown() && key == VK_NEXT)
    {
        switchSettingsSection(g_app.settingsSection + 1);
        return;
    }
    switch (key)
    {
    case VK_ESCAPE:
        if (g_app.editingNumber)
        {
            cancelNumberEdit();
            invalidate();
        }
        else
        {
            hidePalette();
        }
        return;
    case VK_UP:
        commitNumberEdit();
        if (!commitPrefixEdit())
        {
            return;
        }
        g_app.settingsSelected = nextSelectableRow(rows, g_app.settingsSelected, -1);
        scrollSettingsSelectionIntoView();
        invalidate();
        return;
    case VK_DOWN:
    case VK_TAB:
        commitNumberEdit();
        if (!commitPrefixEdit())
        {
            return;
        }
        g_app.settingsSelected = nextSelectableRow(rows, g_app.settingsSelected, 1);
        scrollSettingsSelectionIntoView();
        invalidate();
        return;
    case VK_LEFT:
        applySettingAt(g_app.settingsSelected, -1);
        return;
    case VK_RIGHT:
        applySettingAt(g_app.settingsSelected, 1);
        return;
    case VK_RETURN:
    case VK_SPACE:
        if (g_app.editingNumber)
        {
            commitNumberEdit();
            invalidate();
        }
        else
        {
            applySettingAt(g_app.settingsSelected, 1);
        }
        return;
    case VK_BACK:
        if (g_app.editingNumber && !g_app.numberBuffer.empty())
        {
            g_app.numberBuffer.pop_back();
            invalidate();
        }
        return;
    case VK_PRIOR:
        g_app.settingsScroll = std::max(0.0f, g_app.settingsScroll - 200.0f);
        invalidate();
        return;
    case VK_NEXT:
    {
        const SettingsLayout layout = buildSettingsLayout();
        g_app.settingsScroll = std::min(layout.maxScroll, g_app.settingsScroll + 200.0f);
        invalidate();
        return;
    }
    default:
        break;
    }

    (void)rows;
}
}

void refreshResults()
{
    if (g_app.mode == UiMode::Settings)
    {
        invalidate();
        return;
    }
    if (g_app.actionMenu)
    {
        g_app.actionMenu = false;
        g_app.actionTarget = Command{};
        g_app.actionReturnText.clear();
    }

    const Settings settings = getSettingsSnapshot();
    SearchOutput output = g_app.forcedProviderId.empty()
        ? runSearch(g_app.editor.text(), settings, g_app.hwnd)
        : runProviderSearch(g_app.editor.text(), g_app.forcedProviderId.c_str(), settings, g_app.hwnd);

    const size_t previousCount = g_app.results.size();
    g_app.results = std::move(output.results);
    g_app.queryMode = output.mode;
    g_app.highlightTerms = std::move(output.highlightTerms);
    clampSelection();

    if (settings.showLatency)
    {
        std::wstringstream status;
        status.setf(std::ios::fixed, std::ios::floatfield);
        status.precision(2);
        status << getBaseStatus() << L"  |  " << g_app.results.size() << L" results in " << output.elapsedMs << L" ms";
        setTransientStatus(status.str());
    }
    else
    {
        clearTransientStatus();
    }

    // Resize to fit while positionWindow keeps the query field's top edge anchored;
    // result changes therefore grow and shrink only below the typing area.
    if (g_app.visible && g_app.results.size() != previousCount)
    {
        positionWindow();
    }
    invalidate();
}

void positionWindow()
{
    if (!g_app.hwnd)
    {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info))
    {
        return;
    }

    UINT dpi = g_app.dpi == 0 ? 96 : g_app.dpi;

    // Placing the window can move it to a monitor with a different scale, so settle
    // on the DPI actually in effect before committing to a size.
    for (int pass = 0; pass < 2; ++pass)
    {
        const Settings settings = getSettingsSnapshot();
        const float heightDip = g_app.mode == UiMode::Settings
            ? metrics::settingsHeight
            : paletteHeightForRows(static_cast<int>(g_app.results.size()), paletteUsesDetailRows());

        const int workWidth = std::max<int>(1, info.rcWork.right - info.rcWork.left);
        const float widthDip = g_app.mode == UiMode::Settings ? metrics::settingsWindowWidth : metrics::windowWidth;
        const int preferredWidth = dipToPx(widthDip, dpi);
        const int horizontalMargin = std::min(workWidth - 1, std::max(0, dipToPx(24.0f, dpi)));
        const int availableWidth = std::max(1, workWidth - horizontalMargin);
        const int width = std::min(preferredWidth, availableWidth);
        const int maxHeight = (info.rcWork.bottom - info.rcWork.top) - dipToPx(64.0f, dpi);
        const int minHeight = dipToPx(180.0f, dpi);
        const int height = std::clamp(dipToPx(heightDip, dpi), minHeight, std::max(minHeight, maxHeight));

        const int workHeight = std::max<int>(1, info.rcWork.bottom - info.rcWork.top);
        const int x = info.rcWork.left + std::max(0, (workWidth - width) / 2);
        const WindowPosition position = settings.windowPosition;
        const int centerY = position == WindowPosition::Upper
            ? info.rcWork.top + workHeight / 4
            : info.rcWork.top + workHeight / 2;
        int y = 0;
        int placedHeight = height;
        if (g_app.mode == UiMode::Palette)
        {
            if (!g_app.paletteTopAnchorValid || g_app.paletteAnchorMonitor != monitor)
            {
                const int anchorHeight = std::clamp(
                    dipToPx(paletteHeightForRows(settings.maxResults, paletteUsesDetailRows()), dpi),
                    minHeight, std::max(minHeight, maxHeight));
                g_app.paletteTopAnchor = std::clamp(
                    centerY - anchorHeight / 2,
                    static_cast<int>(info.rcWork.top),
                    static_cast<int>(info.rcWork.bottom) - anchorHeight);
                g_app.paletteAnchorMonitor = monitor;
                g_app.paletteTopAnchorValid = true;
            }
            y = g_app.paletteTopAnchor;
            placedHeight = std::min(height, std::max(minHeight, static_cast<int>(info.rcWork.bottom) - y));
        }
        else
        {
            y = std::clamp(centerY - height / 2, static_cast<int>(info.rcWork.top),
                           static_cast<int>(info.rcWork.bottom) - height);
        }
        SetWindowPos(g_app.hwnd, HWND_TOPMOST, x, y, width, placedHeight, SWP_NOACTIVATE);

        const UINT actual = GetDpiForWindow(g_app.hwnd);
        if (actual == 0 || actual == dpi)
        {
            break;
        }
        g_app.paletteTopAnchorValid = false;
        dpi = actual;
    }

    if (dpi != g_app.dpi)
    {
        g_app.dpi = dpi;
        g_app.graphics.setDpi(dpi);
    }

    RECT client{};
    if (GetClientRect(g_app.hwnd, &client))
    {
        g_app.graphics.resize(D2D1::SizeU(
            static_cast<UINT32>(std::max<LONG>(client.right - client.left, 1)),
            static_cast<UINT32>(std::max<LONG>(client.bottom - client.top, 1))));
    }
}

void showPalette()
{
    rememberPreviousForeground();
    g_app.paletteTopAnchorValid = false;
    g_app.paletteAnchorMonitor = nullptr;
    g_app.visible = true;
    g_app.mode = UiMode::Palette;
    g_app.actionMenu = false;
    g_app.actionTarget = Command{};
    g_app.actionReturnText.clear();
    g_app.forcedProviderId.clear();
    g_app.editor.clear();
    g_app.queryScrollX = 0.0f;
    g_app.selected = 0;
    g_app.hovered = -1;
    g_app.pressedPaletteRow = -1;
    cancelNumberEdit();
    cancelShortcutCapture();
    cancelPrefixEdit();

    refreshTheme();
    applyWindowChrome(g_app.hwnd, g_app.theme);
    refreshResults();
    positionWindow();

    ShowWindow(g_app.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_app.hwnd);
    SetFocus(g_app.hwnd);
    resetCaretBlink();
}

void showProviderPalette(const std::wstring& providerId)
{
    rememberPreviousForeground();
    g_app.paletteTopAnchorValid = false;
    g_app.paletteAnchorMonitor = nullptr;
    g_app.visible = true;
    g_app.mode = UiMode::Palette;
    g_app.actionMenu = false;
    g_app.actionTarget = Command{};
    g_app.actionReturnText.clear();
    g_app.forcedProviderId = providerId;
    g_app.editor.clear();
    g_app.queryScrollX = 0.0f;
    g_app.selected = 0;
    g_app.hovered = -1;
    g_app.pressedPaletteRow = -1;
    cancelNumberEdit();
    cancelShortcutCapture();
    cancelPrefixEdit();

    refreshTheme();
    applyWindowChrome(g_app.hwnd, g_app.theme);
    refreshResults();
    positionWindow();

    ShowWindow(g_app.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_app.hwnd);
    SetFocus(g_app.hwnd);
    resetCaretBlink();
}

void showSettings()
{
    rememberPreviousForeground();
    g_app.paletteTopAnchorValid = false;
    g_app.paletteAnchorMonitor = nullptr;
    g_app.visible = true;
    g_app.mode = UiMode::Settings;
    g_app.forcedProviderId.clear();
    g_app.hovered = -1;
    g_app.settingsHovered = -1;
    g_app.settingsSection = 0;
    g_app.settingsSectionHovered = -1;
    g_app.pressedSettingsSection = -1;
    g_app.pressedRow = -1;
    g_app.pressedPart = PressedPart::None;
    cancelNumberEdit();
    cancelPrefixEdit();

    const auto& rows = activeSettingRows();
    if (g_app.settingsSelected < 0 || g_app.settingsSelected >= static_cast<int>(rows.size()) ||
        rows[static_cast<size_t>(g_app.settingsSelected)].isHeader)
    {
        g_app.settingsSelected = firstSelectableRow(rows);
    }

    refreshTheme();
    applyWindowChrome(g_app.hwnd, g_app.theme);
    positionWindow();
    scrollSettingsSelectionIntoView();

    ShowWindow(g_app.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_app.hwnd);
    SetFocus(g_app.hwnd);
    invalidate();
}

void hidePalette()
{
    commitNumberEdit();
    g_app.visible = false;
    g_app.draggingText = false;
    g_app.actionMenu = false;
    g_app.actionTarget = Command{};
    g_app.actionReturnText.clear();
    g_app.forcedProviderId.clear();
    cancelShortcutCapture();
    cancelPrefixEdit();
    if (g_app.hwnd)
    {
        KillTimer(g_app.hwnd, kCaretTimerId);
        if (GetCapture() == g_app.hwnd)
        {
            ReleaseCapture();
        }
        ShowWindow(g_app.hwnd, SW_HIDE);
    }
}

bool registerWindowClass(HINSTANCE instance)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = wndProc;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    // CS_DBLCLKS enables word-select in the query field; CS_DROPSHADOW gives the
    // popup a real compositor shadow to go with the DWM rounded corners.
    wc.style = CS_DROPSHADOW | CS_DBLCLKS;
    return RegisterClassExW(&wc) != 0;
}

HWND createMainWindow(HINSTANCE instance)
{
    return CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWindowClass,
        kWindowTitle,
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        760, 430,
        nullptr, nullptr, instance, nullptr);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        g_app.dpi = GetDpiForWindow(hwnd);
        refreshTheme();
        applyWindowChrome(hwnd, g_app.theme);
        addTrayIcon(hwnd);
        setIndexNotifyWindow(hwnd);
        AddClipboardFormatListener(hwnd);
        captureClipboardHistory(hwnd);
        g_app.icons.start(hwnd);
        RegisterHotKey(hwnd, kHotkeyId, MOD_ALT | MOD_NOREPEAT, L'Q');
        registerProviderHotkeys(hwnd);
        rebuildIndexAsync();
        return 0;

    case WM_CLIPBOARDUPDATE:
        captureClipboardHistory(hwnd);
        if (g_app.visible && g_app.queryMode == QueryMode::Clipboard)
        {
            refreshResults();
        }
        return 0;

    case kShowPaletteMessage:
        showPalette();
        return 0;

    case kIndexUpdatedMessage:
    case kAsyncProviderUpdatedMessage:
        if (g_app.visible)
        {
            refreshResults();
        }
        return 0;

    case kIconReadyMessage:
        g_app.icons.integrate(g_app.graphics);
        if (g_app.visible)
        {
            invalidate();
        }
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND && getSettingsSnapshot().bitwardenLockOnSleep)
        {
            lockBitwardenSession();
        }
        return TRUE;

    case WM_HOTKEY:
        if (wParam == kHotkeyId)
        {
            if (g_app.visible)
            {
                hidePalette();
            }
            else
            {
                showPalette();
            }
            return 0;
        }
        if (const std::wstring providerId = providerIdForHotkey(wParam); !providerId.empty())
        {
            if (g_app.visible && g_app.mode == UiMode::Palette && g_app.forcedProviderId == providerId)
            {
                hidePalette();
            }
            else
            {
                showProviderPalette(providerId);
            }
            return 0;
        }
        break;

    case kTrayMessage:
        if (LOWORD(lParam) == WM_LBUTTONUP)
        {
            showPalette();
        }
        else if (LOWORD(lParam) == WM_RBUTTONUP)
        {
            showTrayMenu(hwnd);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && g_app.visible)
        {
            // Owned provider prompts are part of the QuickPal interaction. Keep the
            // palette visible behind them; ordinary app switching still dismisses it.
            const HWND activated = reinterpret_cast<HWND>(lParam);
            const bool ownedPrompt = activated && GetWindow(activated, GW_OWNER) == hwnd;
            if (!ownedPrompt)
            {
                hidePalette();
            }
        }
        return 0;

    case WM_SETTINGCHANGE:
        if (lParam && _wcsicmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
        {
            refreshTheme();
            applyWindowChrome(hwnd, g_app.theme);
            g_app.graphics.clearLayoutCache();
            invalidate();
        }
        return 0;

    case WM_DPICHANGED:
    {
        g_app.dpi = HIWORD(wParam);
        g_app.graphics.setDpi(g_app.dpi);
        g_app.graphics.clearLayoutCache();
        if (auto* suggested = reinterpret_cast<RECT*>(lParam))
        {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        invalidate();
        return 0;
    }

    case WM_SIZE:
        g_app.graphics.resize(D2D1::SizeU(
            static_cast<UINT32>(std::max<int>(LOWORD(lParam), 1)),
            static_cast<UINT32>(std::max<int>(HIWORD(lParam), 1))));
        return 0;

    case WM_TIMER:
        if (wParam == kCaretTimerId)
        {
            if (g_app.visible && g_app.mode == UiMode::Palette)
            {
                g_app.caretVisible = !g_app.caretVisible;
                invalidate();
            }
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        if (!paintFrame())
        {
            // Device was lost mid-frame; the next paint recreates it.
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_IME_STARTCOMPOSITION:
        // Put the candidate window under the caret instead of the window corner.
        if (HIMC context = ImmGetContext(hwnd))
        {
            const PaletteLayout layout = buildPaletteLayout();
            COMPOSITIONFORM form{};
            form.dwStyle = CFS_POINT;
            form.ptCurrentPos.x = dipToPx(layout.queryText.left, g_app.dpi);
            form.ptCurrentPos.y = dipToPx(layout.queryText.top, g_app.dpi);
            ImmSetCompositionWindow(context, &form);
            ImmReleaseContext(hwnd, context);
        }
        break;

    case WM_SYSKEYDOWN:
        if (g_app.mode == UiMode::Settings && (g_app.capturingShortcut || g_app.editingPrefix))
        {
            handleSettingsKey(wParam);
            return 0;
        }
        break;

    case WM_SYSCHAR:
        if (g_app.mode == UiMode::Settings && (g_app.capturingShortcut || g_app.editingPrefix))
        {
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (g_app.mode == UiMode::Settings && (g_app.capturingShortcut || g_app.editingPrefix))
        {
            handleSettingsKey(wParam);
            return 0;
        }

        if (ctrlDown())
        {
            switch (wParam)
            {
            case VK_RETURN:
                if (shiftDown() && g_app.mode == UiMode::Palette)
                {
                    executeSelectedAsAdmin();
                    return 0;
                }
                break;
            case VK_OEM_COMMA:
                showSettings();
                return 0;
            case L'R':
                rebuildIndexAsync();
                return 0;
            case L'Q':
                quitApp();
                return 0;
            default:
                break;
            }

            if (g_app.mode == UiMode::Palette)
            {
                switch (wParam)
                {
                case L'A':
                    g_app.editor.selectAll();
                    resetCaretBlink();
                    invalidate();
                    return 0;
                case L'K':
                    if (!g_app.actionMenu)
                    {
                        showActionsForSelected();
                    }
                    return 0;
                case L'C':
                    if (g_app.editor.hasSelection())
                    {
                        copyTextToClipboard(hwnd, g_app.editor.selectedText());
                    }
                    return 0;
                case L'X':
                    if (g_app.editor.hasSelection())
                    {
                        copyTextToClipboard(hwnd, g_app.editor.selectedText());
                        g_app.editor.deleteSelection();
                        resetCaretBlink();
                        refreshResults();
                    }
                    return 0;
                case L'V':
                    if (auto text = clipboardText(hwnd))
                    {
                        std::wstring flat = *text;
                        std::replace(flat.begin(), flat.end(), L'\r', L' ');
                        std::replace(flat.begin(), flat.end(), L'\n', L' ');
                        g_app.editor.insert(flat);
                        g_app.selected = 0;
                        resetCaretBlink();
                        refreshResults();
                    }
                    return 0;
                default:
                    break;
                }
            }
        }

        if (g_app.mode == UiMode::Settings)
        {
            handleSettingsKey(wParam);
        }
        else
        {
            handlePaletteKey(wParam);
        }
        return 0;

    case WM_CHAR:
    {
        const wchar_t ch = static_cast<wchar_t>(wParam);
        if (g_app.suppressNextCharacter)
        {
            g_app.suppressNextCharacter = false;
            return 0;
        }
        if (ch < 32 || ch == 127 || ctrlDown())
        {
            return 0;
        }

        if (g_app.mode == UiMode::Settings)
        {
            if (g_app.editingPrefix)
            {
                appendPrefixChar(ch);
                return 0;
            }
            // Typing a number on a stepper row beats pressing + thirty times.
            if (g_app.capturingShortcut)
            {
                return 0;
            }
            const auto& rows = activeSettingRows();
            const int index = g_app.settingsSelected;
            if (std::iswdigit(ch) && index >= 0 && index < static_cast<int>(rows.size()) &&
                !rows[index].isHeader && rows[index].item.kind == SettingKind::Stepper)
            {
                if (!g_app.editingNumber)
                {
                    g_app.editingNumber = true;
                    g_app.numberBuffer.clear();
                }
                if (g_app.numberBuffer.size() < 7)
                {
                    g_app.numberBuffer.push_back(ch);
                }
                invalidate();
            }
            return 0;
        }

        if (g_app.actionMenu)
        {
            return 0;
        }
        g_app.editor.insertChar(ch);
        g_app.selected = 0;
        resetCaretBlink();
        refreshResults();
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (g_app.mode == UiMode::Settings)
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const SettingsLayout layout = buildSettingsLayout();
            g_app.settingsScroll = std::clamp(
                g_app.settingsScroll - static_cast<float>(delta) * 0.6f, 0.0f, layout.maxScroll);
            invalidate();
        }
        else if (!g_app.results.empty())
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            g_app.selected = std::clamp(g_app.selected - (delta > 0 ? 1 : -1), 0,
                                        static_cast<int>(g_app.results.size()) - 1);
            invalidate();
        }
        return 0;

    case WM_MOUSEMOVE:
    {
        const D2D1_POINT_2F point = clientPointDip(lParam);

        if (g_app.draggingText && g_app.mode == UiMode::Palette)
        {
            const PaletteLayout layout = buildPaletteLayout();
            const size_t position = queryPositionFromPoint(layout, point.x, point.y, nullptr);
            g_app.editor.placeCaret(position, true);
            resetCaretBlink();
            invalidate();
        }
        else if (g_app.mode == UiMode::Settings)
        {
            const SettingsLayout layout = buildSettingsLayout();
            const SettingsHit hit = hitTestSettings(layout, point.x, point.y);
            if (hit.row != g_app.settingsHovered || hit.section != g_app.settingsSectionHovered)
            {
                g_app.settingsHovered = hit.row;
                g_app.settingsSectionHovered = hit.section;
                invalidate();
            }
        }
        else
        {
            const PaletteLayout layout = buildPaletteLayout();
            const int hovered = hitTestPaletteRow(layout, point.x, point.y);
            if (hovered != g_app.hovered)
            {
                g_app.hovered = hovered;
                if (hovered >= 0)
                {
                    g_app.selected = hovered;
                }
                invalidate();
            }
        }

        if (!g_app.mouseTracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_app.mouseTracking = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_app.mouseTracking = false;
        g_app.hovered = -1;
        g_app.settingsHovered = -1;
        g_app.settingsSectionHovered = -1;
        invalidate();
        return 0;

    case WM_LBUTTONDBLCLK:
        if (g_app.mode == UiMode::Palette)
        {
            const D2D1_POINT_2F point = clientPointDip(lParam);
            const PaletteLayout layout = buildPaletteLayout();
            if (pointInRect(layout.queryText, point.x, point.y))
            {
                const size_t position = queryPositionFromPoint(layout, point.x, point.y, nullptr);
                g_app.editor.selectWordAt(position);
                resetCaretBlink();
                invalidate();
                return 0;
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        const D2D1_POINT_2F point = clientPointDip(lParam);

        if (g_app.mode == UiMode::Settings)
        {
            const SettingsLayout layout = buildSettingsLayout();
            const SettingsHit hit = hitTestSettings(layout, point.x, point.y);
            if (hit.section >= 0)
            {
                g_app.pressedSettingsSection = hit.section;
                g_app.pressedPart = PressedPart::SettingsSection;
                SetCapture(hwnd);
                invalidate();
            }
            else if (hit.row >= 0)
            {
                if (g_app.settingsSelected != hit.row)
                {
                    commitNumberEdit();
                    if (!commitPrefixEdit())
                    {
                        return 0;
                    }
                }
                g_app.settingsSelected = hit.row;
                g_app.pressedRow = hit.row;
                g_app.pressedPart = hit.part;
                g_app.pressedSegment = hit.segment;
                SetCapture(hwnd);
                invalidate();
            }
            return 0;
        }

        const PaletteLayout layout = buildPaletteLayout();
        if (pointInRect(layout.queryText, point.x, point.y))
        {
            const size_t position = queryPositionFromPoint(layout, point.x, point.y, nullptr);
            g_app.editor.placeCaret(position, shiftDown());
            g_app.draggingText = true;
            SetCapture(hwnd);
            resetCaretBlink();
            invalidate();
            return 0;
        }

        const int row = hitTestPaletteRow(layout, point.x, point.y);
        if (row >= 0)
        {
            g_app.selected = row;
            g_app.pressedPaletteRow = row;
            SetCapture(hwnd);
            invalidate();
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const D2D1_POINT_2F point = clientPointDip(lParam);
        if (GetCapture() == hwnd)
        {
            ReleaseCapture();
        }

        if (g_app.draggingText)
        {
            g_app.draggingText = false;
            return 0;
        }

        if (g_app.mode == UiMode::Settings)
        {
            const int pressedRow = g_app.pressedRow;
            const PressedPart pressedPart = g_app.pressedPart;
            const int pressedSegment = g_app.pressedSegment;
            const int pressedSection = g_app.pressedSettingsSection;
            g_app.pressedRow = -1;
            g_app.pressedPart = PressedPart::None;
            g_app.pressedSegment = -1;
            g_app.pressedSettingsSection = -1;

            const SettingsLayout layout = buildSettingsLayout();
            const SettingsHit hit = hitTestSettings(layout, point.x, point.y);
            if (pressedPart == PressedPart::SettingsSection)
            {
                if (hit.section == pressedSection)
                {
                    switchSettingsSection(pressedSection);
                }
                else
                {
                    invalidate();
                }
                return 0;
            }

            if (pressedRow < 0)
            {
                invalidate();
                return 0;
            }

            // Only act when the release lands on the same control that was pressed.
            if (hit.row != pressedRow || hit.part != pressedPart || hit.segment != pressedSegment)
            {
                invalidate();
                return 0;
            }

            const auto& rows = activeSettingRows();
            switch (pressedPart)
            {
            case PressedPart::Toggle:
            case PressedPart::Action:
                applySettingAt(pressedRow, 1);
                break;
            case PressedPart::Minus:
                applySettingAt(pressedRow, -1);
                break;
            case PressedPart::Plus:
                applySettingAt(pressedRow, 1);
                break;
            case PressedPart::Segment:
            {
                if (pressedRow < static_cast<int>(rows.size()) && pressedSegment >= 0)
                {
                    const SettingField field = rows[pressedRow].item.field;
                    const Settings settings = getSettingsSnapshot();
                    const int current = settingChoiceIndex(field, settings);
                    // Walk to the clicked segment so persistence stays in applySetting.
                    int guard = 0;
                    while (settingChoiceIndex(field, getSettingsSnapshot()) != pressedSegment && guard++ < 8)
                    {
                        applySettingAt(pressedRow, 1);
                    }
                    (void)current;
                }
                break;
            }
            default:
                invalidate();
                break;
            }
            return 0;
        }

        const int pressedRow = g_app.pressedPaletteRow;
        g_app.pressedPaletteRow = -1;
        if (pressedRow >= 0)
        {
            const PaletteLayout layout = buildPaletteLayout();
            if (hitTestPaletteRow(layout, point.x, point.y) == pressedRow)
            {
                g_app.selected = pressedRow;
                executeSelected();
            }
        }
        return 0;
    }

    case WM_RBUTTONUP:
    {
        if (g_app.mode != UiMode::Palette || g_app.actionMenu)
        {
            return 0;
        }

        const D2D1_POINT_2F point = clientPointDip(lParam);
        const PaletteLayout layout = buildPaletteLayout();
        const int row = hitTestPaletteRow(layout, point.x, point.y);
        if (row >= 0)
        {
            g_app.selected = row;
            showActionsForSelected();
        }
        return 0;
    }

    case WM_CLOSE:
        hidePalette();
        return 0;

    case WM_DESTROY:
        if (getSettingsSnapshot().bitwardenLockOnExit)
        {
            lockBitwardenSession();
        }
        KillTimer(hwnd, kCaretTimerId);
        UnregisterHotKey(hwnd, kHotkeyId);
        unregisterProviderHotkeys(hwnd);
        RemoveClipboardFormatListener(hwnd);
        g_app.icons.stop();
        g_app.graphics.destroy();
        Shell_NotifyIconW(NIM_DELETE, &g_app.tray);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
