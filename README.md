# QuickPal

QuickPal is a tiny native C++ command palette for Windows. It is built for low input latency:

- Win32/GDI UI, no Electron/Qt/WinUI startup cost.
- Commands are indexed once into memory.
- Each keystroke scores the in-memory index and keeps only the top results.
- File search can use the Everything HTTP API, the Everything SDK DLL, or a bounded background fallback index for Desktop, Documents, Downloads, and optional default Windows locations.
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

## Use

- Launch `QuickPal.exe`.
- QuickPal starts as a tray app.
- Press `Alt+Q`, left-click the tray icon, or use the tray menu to open it.
- Right-click the tray icon for Settings, Reload indexes, shell runner toggle, Everything HTTP/SDK toggles, and Exit.
- Type normally to search indexed apps, PATH tools, settings, and built-ins.
- `file <name>` or `f <name>` searches files through Everything HTTP, Everything SDK, or the fallback index. File rows show full path, size, and modified date.
- `?? <query>` opens a web search.
- `> <command>` runs a shell command. PowerShell is the default runner; switch to cmd in Settings or the tray menu.
- `= <expression>`, `calc <expression>`, or raw math like `23*47` calculates in-process and copies the result on Enter.
- `win <title>` switches to an open window.
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

Chrome tab search is intentionally not included. Chrome does not expose a reliable fast native tab-list API without remote debugging or an extension, and this build avoids depending on a debug port.

## PowerToys Reference

The real PowerToys Command Palette lives in Microsoft PowerToys under `src/modules/cmdpal`.
