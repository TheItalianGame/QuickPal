#include "window.h"

#include "../app.h"
#include "../core/indexer.h"
#include "../core/search.h"
#include "../core/settings.h"
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

void invalidate()
{
    if (g_app.hwnd)
    {
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
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
    PressedPart part = PressedPart::None;
    int segment = -1;
};

SettingsHit hitTestSettings(const SettingsLayout& layout, float x, float y)
{
    SettingsHit hit;
    if (!pointInRect(layout.listArea, x, y))
    {
        return hit;
    }

    const auto& rows = settingRows();
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

void runShellCommand(const std::wstring& command)
{
    const Settings settings = getSettingsSnapshot();
    if (settings.shellUsesPowerShell)
    {
        const std::wstring params = L"-NoExit -NoProfile -ExecutionPolicy Bypass -Command " + command;
        ShellExecuteW(nullptr, L"open", L"powershell.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    }
    else
    {
        const std::wstring params = L"/d /k " + command;
        ShellExecuteW(nullptr, L"open", L"cmd.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void quitApp()
{
    Shell_NotifyIconW(NIM_DELETE, &g_app.tray);
    PostQuitMessage(0);
}

void executeCommand(const Command& command)
{
    switch (command.kind)
    {
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
    case CommandKind::ReloadIndex:
        rebuildIndexAsync();
        return;
    case CommandKind::ExitApp:
        quitApp();
        return;
    case CommandKind::Shell:
        runShellCommand(command.arg);
        break;
    case CommandKind::Web:
        openPathOrUri(L"https://www.google.com/search?q=" + urlEncode(command.arg));
        break;
    case CommandKind::Calc:
        copyTextToClipboard(g_app.hwnd, command.arg);
        break;
    case CommandKind::Window:
        if (IsWindow(command.hwnd))
        {
            if (IsIconic(command.hwnd))
            {
                ShowWindow(command.hwnd, SW_RESTORE);
            }
            SetForegroundWindow(command.hwnd);
        }
        break;
    case CommandKind::Setting:
    case CommandKind::App:
    case CommandKind::PathTool:
    case CommandKind::File:
    case CommandKind::Folder:
        openPathOrUri(command.arg);
        break;
    case CommandKind::Builtin:
        if (command.arg == L"lock")
        {
            LockWorkStation();
        }
        else
        {
            openPathOrUri(command.arg);
        }
        break;
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
    const auto& rows = settingRows();
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

void applySettingAt(int rowIndex, int direction)
{
    const auto& rows = settingRows();
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()) || rows[rowIndex].isHeader)
    {
        return;
    }

    cancelNumberEdit();
    const SettingField field = rows[rowIndex].item.field;
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
    switch (key)
    {
    case VK_ESCAPE:
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
    const auto& rows = settingRows();
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
        g_app.settingsSelected = nextSelectableRow(g_app.settingsSelected, -1);
        scrollSettingsSelectionIntoView();
        invalidate();
        return;
    case VK_DOWN:
    case VK_TAB:
        commitNumberEdit();
        g_app.settingsSelected = nextSelectableRow(g_app.settingsSelected, 1);
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

    const Settings settings = getSettingsSnapshot();
    SearchOutput output = runSearch(g_app.editor.text(), settings, g_app.hwnd);

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

    // The window shrinks to fit, so a two-result query does not leave a tall gap.
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
        const float heightDip = g_app.mode == UiMode::Settings
            ? metrics::settingsHeight
            : paletteHeightForRows(static_cast<int>(g_app.results.size()));

        const int width = dipToPx(metrics::windowWidth, dpi);
        const int maxHeight = (info.rcWork.bottom - info.rcWork.top) - dipToPx(64.0f, dpi);
        const int minHeight = dipToPx(180.0f, dpi);
        const int height = std::clamp(dipToPx(heightDip, dpi), minHeight, std::max(minHeight, maxHeight));

        const int x = info.rcWork.left + ((info.rcWork.right - info.rcWork.left) - width) / 2;
        const int y = info.rcWork.top + dipToPx(96.0f, dpi);
        SetWindowPos(g_app.hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);

        const UINT actual = GetDpiForWindow(g_app.hwnd);
        if (actual == 0 || actual == dpi)
        {
            break;
        }
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
    g_app.visible = true;
    g_app.mode = UiMode::Palette;
    g_app.editor.clear();
    g_app.queryScrollX = 0.0f;
    g_app.selected = 0;
    g_app.hovered = -1;
    g_app.pressedPaletteRow = -1;
    cancelNumberEdit();

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
    g_app.visible = true;
    g_app.mode = UiMode::Settings;
    g_app.hovered = -1;
    g_app.settingsHovered = -1;
    g_app.pressedRow = -1;
    g_app.pressedPart = PressedPart::None;
    cancelNumberEdit();

    const auto& rows = settingRows();
    if (g_app.settingsSelected < 0 || g_app.settingsSelected >= static_cast<int>(rows.size()) ||
        rows[static_cast<size_t>(g_app.settingsSelected)].isHeader)
    {
        g_app.settingsSelected = firstSelectableRow();
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
        g_app.icons.start(hwnd);
        RegisterHotKey(hwnd, kHotkeyId, MOD_ALT | MOD_NOREPEAT, L'Q');
        rebuildIndexAsync();
        return 0;

    case kShowPaletteMessage:
        showPalette();
        return 0;

    case kIndexUpdatedMessage:
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
            hidePalette();
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

    case WM_KEYDOWN:
        if (ctrlDown())
        {
            switch (wParam)
            {
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
        if (ch < 32 || ch == 127 || ctrlDown())
        {
            return 0;
        }

        if (g_app.mode == UiMode::Settings)
        {
            // Typing a number on a stepper row beats pressing + thirty times.
            const auto& rows = settingRows();
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
            if (hit.row != g_app.settingsHovered)
            {
                g_app.settingsHovered = hit.row;
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
            if (hit.row >= 0)
            {
                if (g_app.settingsSelected != hit.row)
                {
                    commitNumberEdit();
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
            g_app.pressedRow = -1;
            g_app.pressedPart = PressedPart::None;
            g_app.pressedSegment = -1;

            if (pressedRow < 0)
            {
                invalidate();
                return 0;
            }

            const SettingsLayout layout = buildSettingsLayout();
            const SettingsHit hit = hitTestSettings(layout, point.x, point.y);
            // Only act when the release lands on the same control that was pressed.
            if (hit.row != pressedRow || hit.part != pressedPart || hit.segment != pressedSegment)
            {
                invalidate();
                return 0;
            }

            const auto& rows = settingRows();
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

    case WM_CLOSE:
        hidePalette();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kCaretTimerId);
        UnregisterHotKey(hwnd, kHotkeyId);
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
