#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <string>
#include <unordered_map>
#include <vector>

constexpr wchar_t kWindowClass[] = L"QuickPal.Native.Window";
constexpr wchar_t kWindowTitle[] = L"QuickPal";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowPaletteMessage = WM_APP + 2;
constexpr UINT kIndexUpdatedMessage = WM_APP + 3;
constexpr UINT kIconReadyMessage = WM_APP + 4;
constexpr UINT kAsyncProviderUpdatedMessage = WM_APP + 5;

constexpr int kHotkeyId = 7001;
constexpr int kCaretTimerId = 7101;

constexpr int kDefaultMaxResults = 12;
constexpr int kMinMaxResults = 5;
constexpr int kMaxMaxResults = 16;

enum class CommandKind
{
    Builtin,
    App,
    PathTool,
    File,
    Folder,
    Shell,
    Web,
    Calc,
    OpenCalculator,
    Window,
    Setting,
    OpenSettings,
    ReloadIndex,
    ExitApp,
    PaletteQuery,
    QuickLink,
    Clipboard,
    Process,
    ChromeTab,
    Snippet,
    ValueTool,
    BitwardenItem,
    BitwardenControl,
    Action,
};

enum class ActionKind
{
    None,
    Open,
    RunAsAdministrator,
    OpenContainingFolder,
    CopyPath,
    CopyName,
    CopyTitle,
    CopySubtitle,
    OpenWith,
    KillProcess,
    PasteText,
    PinClipboard,
    UnpinClipboard,
    CloseBrowserTab,
    ReloadBrowserTab,
    WindowMinimize,
    WindowMaximizeRestore,
    WindowSnapLeft,
    WindowSnapRight,
    WindowCenter,
    WindowClose,
    BitwardenCopyUsername,
    BitwardenCopyPassword,
    BitwardenCopyTotp,
    BitwardenOpenSite,
    BitwardenOpenItem,
    ConfigureProvider,
    SetProviderShortcut,
    SetProviderPrefix,
};

struct Command
{
    CommandKind kind = CommandKind::Builtin;
    std::wstring title;
    std::wstring subtitle;
    std::wstring arg;
    std::wstring searchText;
    std::wstring key;
    std::wstring data;
    int weight = 0;
    HWND hwnd = nullptr;
    DWORD processId = 0;
    CommandKind targetKind = CommandKind::Builtin;
    ActionKind action = ActionKind::None;
    // Id of the provider that produced this, stamped automatically when a result is
    // added. Points at a static literal, so copying a Command stays cheap.
    const wchar_t* provider = nullptr;
};

// Drives the mode pill in the header and tells the engine which provider claimed
// the query.
enum class QueryMode
{
    Commands,
    Files,
    Web,
    Shell,
    Math,
    Windows,
    Processes,
    Clipboard,
    BrowserTabs,
    Snippets,
    Values,
    Bitwarden,
    Actions,
};

struct Result
{
    Command command;
    int score = 0;
};

// Which artwork a row should use. Shell icons cost disk I/O and are resolved on a
// worker thread; vector glyphs are free and always available as the placeholder.
enum class IconSource
{
    Glyph,
    ShellPath,
    WindowHandle,
};

IconSource iconSourceFor(CommandKind kind);

enum class Appearance
{
    System,
    Dark,
    Light,
};

struct Settings
{
    bool useEverything = true;
    bool useEverythingHttp = true;
    bool fallbackFileIndex = true;
    bool indexDesktop = true;
    bool indexDocuments = true;
    bool indexDownloads = true;
    bool indexDefaultPaths = true;
    bool indexStartMenu = true;
    bool indexPathTools = true;
    bool showLatency = true;
    bool shellUsesPowerShell = true;
    bool useChromeTabs = true;
    bool bitwardenSearchUsernames = true;
    bool bitwardenUnlockWithPin = true;
    bool bitwardenRequireMasterOnRestart = true;
    bool bitwardenUseServe = false;
    bool bitwardenLockOnSleep = true;
    bool bitwardenLockOnExit = true;
    bool useSystemAccent = true;
    Appearance appearance = Appearance::System;
    int maxResults = kDefaultMaxResults;
    int fileDepth = 5;
    int fileLimit = 50000;
    int bitwardenSecretTimeoutSeconds = 300;
    int bitwardenClipboardClearSeconds = 30;

    // Everything's local HTTP server; defaults are imported from Everything.ini
    // when it exists, so this usually needs no configuration.
    std::wstring everythingHttpHost = L"127.0.0.1";
    std::wstring everythingHttpUsername;
    std::wstring everythingHttpPassword;
    int everythingHttpPort = 2342;

    std::unordered_map<std::wstring, std::wstring> providerShortcuts = {
        { L"files", L"Alt+E" },
    };
    std::unordered_map<std::wstring, std::wstring> providerPrefixes;
};

enum class UiMode
{
    Palette,
    Settings,
};
