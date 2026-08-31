#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ChromeTabInfo
{
    int tabId = 0;
    int windowId = 0;
    std::wstring title;
    std::wstring url;
    bool active = false;
};

constexpr wchar_t kChromeTabsProviderId[] = L"browser-tabs";
constexpr wchar_t kChromeNativeHostName[] = L"com.quickpal.tabs";
constexpr wchar_t kChromeTabsExtensionId[] = L"abjcicifgkcemgbkgaalmldengbkdhkk";

void registerChromeNativeMessagingHost();
std::wstring chromeExtensionDirectory();
std::wstring chromeTabsCachePath();
void openChromeExtensionInstallLocation();
std::vector<ChromeTabInfo> readChromeTabsCache();
bool activateChromeTab(int windowId, int tabId);
bool closeChromeTab(int windowId, int tabId);
bool reloadChromeTab(int windowId, int tabId);
