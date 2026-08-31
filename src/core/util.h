#pragma once

#include "types.h"

#include <optional>
#include <string>
#include <vector>

std::wstring env(const wchar_t* name);
std::wstring expandEnv(const std::wstring& input);
std::wstring executableDirectory();

std::wstring lowerCopy(std::wstring value);
std::wstring trimCopy(const std::wstring& value);
bool startsWith(const std::wstring& value, const std::wstring& prefix);
std::vector<std::wstring> splitTerms(const std::wstring& query);

std::wstring fileNameFromPath(const std::wstring& path);
std::wstring stripExtension(std::wstring name);
std::wstring extensionLower(const std::wstring& path);

std::string toUtf8(const std::wstring& value);
std::wstring fromUtf8(const std::string& value);
std::wstring urlEncode(const std::wstring& value);

bool copyTextToClipboard(HWND owner, const std::wstring& text);
bool copySensitiveTextToClipboard(HWND owner, const std::wstring& text, int clearAfterSeconds);
bool clipboardHasHistoryExclusion();
std::optional<std::wstring> clipboardText(HWND owner);
bool pasteTextToWindow(HWND owner, HWND target, const std::wstring& text);

Command makeCommand(CommandKind kind, std::wstring title, std::wstring subtitle, std::wstring arg, int weight);
