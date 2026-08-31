# QuickPal

QuickPal is a tiny native C++ command palette for Windows. It is built for low input latency:

- Win32/GDI UI, no Electron/Qt/WinUI startup cost.
- Commands are indexed once into memory.
- Each keystroke scores the in-memory index and keeps only the top results.
- File search can use the Everything HTTP API, the Everything SDK DLL, or a bounded background fallback index for Desktop, Documents, Downloads, and optional default Windows locations.
- Chrome tab search uses a local Chrome extension and Native Messaging, with no debug port.
- Windows Settings and window switching use Windows APIs/URI handlers.
- Settings and tray management are native Win32 in the same process.

## Build

Run from PowerShell:

```powershell
.\build.ps1
```

The executable is written to:

```text
bin\QuickPal.exe
```

The Chrome Native Messaging helper is written to:

```text
bin\QuickPalChromeHost.exe
```

The search benchmark harness is written to:

```text
bin\QuickPalBench.exe
```

## Use

- Launch `QuickPal.exe`.
- QuickPal starts as a tray app.
- Press `Alt+Q`, left-click the tray icon, or use the tray menu to open it.
- Right-click the tray icon for Settings, Reload indexes, shell runner toggle, Everything HTTP/SDK toggles, and Exit.
- Type normally to search indexed apps, PATH tools, settings, and built-ins.
- `file <name>` or `f <name>` searches files through Everything HTTP, Everything SDK, or the fallback index. File rows show full path, size, and modified date.
- `Alt+E` opens the file search provider directly. Provider shortcuts and primary typed prefixes can be changed in Settings.
- `?? <query>` opens a web search.
- `> <command>` runs a shell command. PowerShell is the default runner; switch to cmd in Settings or the tray menu.
- `= <expression>`, `calc <expression>`, or raw math like `23*47` calculates in-process and copies the result on Enter.
- `win <title>` switches to an open window.
- `tab <query>`, `tabs <query>`, or `chrome <query>` searches open Chrome tabs after the extension is installed.
- `Ctrl+K` or right-click opens actions for the selected result. Actions include admin launch, containing folder, copy title/path/name/value, provider settings, tab close/reload, window snap/center/minimize/close, and clipboard pinning.
- `v <query>`, `clip <query>`, or `clipboard <query>` searches clipboard history. Enter pastes plain text back into the app that was active before QuickPal opened. Pinned clipboard items are stored in `%APPDATA%\QuickPal\clipboard_pins.tsv`.
- `; <query>`, `snip <query>`, or `snippet <query>` searches snippets. Snippets live in `%APPDATA%\QuickPal\snippets.ini`; Settings has an Edit snippets action. `{date}`, `{time}`, and `{datetime}` expand at paste time.
- `ql <alias> <query>` or bare quicklinks like `gh windows api` open user-defined quicklinks from `%APPDATA%\QuickPal\quicklinks.ini`. Settings has an Edit quicklinks action.
- `pw <query>` searches Bitwarden metadata after you sync it. Enter opens the saved site. `Ctrl+K` or right-click exposes Copy username, Copy password, Copy TOTP, Open site, and Open in Bitwarden.
- `guid` generates a UUID. `base64 <text>`, `from64 <text>`, `sha256 <text>`, `sha1 <text>`, and `md5 <text>` generate values locally and copy them on Enter.
- Built-in system commands include Sleep PC, Restart PC, Shut down PC, Sign out, and Empty Recycle Bin.
- `Ctrl+,` opens Settings.
- `Ctrl+R` reloads indexes.
- `Ctrl+Q` exits.

## Everything SDK

For whole-machine indexed file search, install Voidtools Everything and place `Everything64.dll` beside `QuickPal.exe`, somewhere on `PATH`, or in `C:\Program Files\Everything\`. QuickPal dynamically loads it at runtime; it does not require the SDK to build.

The normal Everything app installer may not include `Everything64.dll`; the SDK ZIP from Voidtools does.

## Everything HTTP API

QuickPal can also use Everything's local HTTP API. On startup it imports the host, port, username, and password from `%APPDATA%\Everything\Everything.ini` and saves them into `%APPDATA%\QuickPal\settings.ini`. The Settings UI exposes **Everything HTTP API** and **Everything HTTP port**.

## Fallback Default Locations

When Everything SDK is unavailable or disabled, the fallback file index can include the user profile, OneDrive folders, Public Desktop/Documents, Start Menu folders, and Program Files folders. This is controlled by **Default locations** in Settings and still respects the depth and file-cap settings.

## Chrome Tabs

Chrome tab search uses the unpacked extension in `chrome-extension` and the native host in `bin\QuickPalChromeHost.exe`.

1. Build and run QuickPal once. It registers the Native Messaging host under HKCU.
2. Open Settings > Browser tabs > Install extension, or open `chrome://extensions` manually.
3. Enable Developer mode.
4. Click Load unpacked and choose the `chrome-extension` folder.

The extension keeps `%APPDATA%\QuickPal\chrome_tabs.json` updated. QuickPal reads that local cache for fast search and sends focus requests back through a named pipe.

Chrome tab actions also use the same Native Messaging bridge for close and reload, still without a debug port.

## Bitwarden

Bitwarden search uses `bw.exe` directly. It does not use PowerShell, cmd, or `bw serve` by default. If `bw.exe` is missing, `pw` shows an install action that opens the official Bitwarden CLI docs.

QuickPal only keeps searchable metadata in its Bitwarden cache: item name, primary domain, folder, vault, and username when **Search usernames** is enabled. It does not index or store passwords, TOTP seeds/codes, notes, recovery codes, or custom fields. Secret actions call `bw get username`, `bw get password`, or `bw get totp` only when selected from the actions menu.

The first secret action requires your Bitwarden master password and stores only the returned `BW_SESSION` key in memory until **Secret timeout** expires. With **Unlock with PIN** enabled, QuickPal asks for a local PIN during that active session window; failed attempts get a short backoff. **Master password on restart** defaults on, so QuickPal does not persist the session between app launches.

Secret clipboard writes use Windows clipboard history/cloud exclusion formats and clear the clipboard after **Clipboard clear** seconds if the value is unchanged.

## Benchmark Search Latency

Run the native harness from PowerShell:

```powershell
.\bin\QuickPalBench.exe
```

It measures real `runSearch()` and provider-hotkey `runProviderSearch()` calls one typed character at a time across all providers. The default matrix includes no-prefix, typed-prefix, and scoped provider scenarios for builtins, settings, Start Menu apps, Store apps, PATH tools, quicklinks, snippets, value tools, Bitwarden, files, windows, PowerShell, web search, calculator, processes, clipboard, and Chrome tabs. Useful variants:

```powershell
.\bin\QuickPalBench.exe --list-scenarios
.\bin\QuickPalBench.exe --iterations 500 --budget-ms 2 --fail-on-budget
.\bin\QuickPalBench.exe --json
.\bin\QuickPalBench.exe --provider-query files readme
```

`--include-fallback-index` builds the fallback file index before measuring. Leave it off when you only want keystroke latency against the normal fast Everything/API path.

## Provider Settings

Every provider has Shortcut and Prefix rows in Settings. Shortcut captures a global hotkey; Prefix edits the primary typed alias. Click a Prefix row, type the alias, press Enter to save, Delete to reset, or Esc to cancel. Built-in aliases remain available, and QuickPal prevents duplicate provider prefixes.

## PowerToys Reference

The real PowerToys Command Palette lives in Microsoft PowerToys under `src/modules/cmdpal`.
