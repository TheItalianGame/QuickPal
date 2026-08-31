#pragma once

#include "types.h"

#include <string>
#include <vector>

struct ClipboardEntry
{
    std::wstring text;
    bool pinned = false;
};

void captureClipboardHistory(HWND owner);
std::vector<ClipboardEntry> clipboardHistorySnapshot();
bool isClipboardTextPinned(const std::wstring& text);
void pinClipboardText(const std::wstring& text);
void unpinClipboardText(const std::wstring& text);
