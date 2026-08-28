#pragma once

#include "types.h"

#include <string>
#include <vector>

void captureClipboardHistory(HWND owner);
std::vector<std::wstring> clipboardHistorySnapshot();

