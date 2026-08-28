#pragma once

#include "../provider.h"

#include <memory>

// Each built-in provider lives in its own translation unit and is exposed through
// one factory. Adding a feature means adding a file here and one line in
// registerBuiltinProviders().
std::unique_ptr<Provider> makeBuiltinsProvider();
std::unique_ptr<Provider> makeSettingsUriProvider();
std::unique_ptr<Provider> makeStartMenuProvider();
std::unique_ptr<Provider> makeAppsFolderProvider();
std::unique_ptr<Provider> makePathToolsProvider();
std::unique_ptr<Provider> makeQuickLinksProvider();
std::unique_ptr<Provider> makeFilesProvider();
std::unique_ptr<Provider> makeWindowsProvider();
std::unique_ptr<Provider> makeShellProvider();
std::unique_ptr<Provider> makeWebProvider();
std::unique_ptr<Provider> makeCalculatorProvider();
std::unique_ptr<Provider> makeProcessesProvider();
std::unique_ptr<Provider> makeClipboardProvider();
std::unique_ptr<Provider> makeChromeTabsProvider();

// Called once at startup, before the first index rebuild.
void registerBuiltinProviders();
