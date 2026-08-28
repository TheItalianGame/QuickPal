#include "clipboard_history.h"

#include "util.h"

#include <algorithm>
#include <mutex>

namespace
{
std::mutex g_mutex;
std::vector<std::wstring> g_history;

std::wstring compactClipboardText(std::wstring text)
{
    std::replace(text.begin(), text.end(), L'\r', L' ');
    std::replace(text.begin(), text.end(), L'\n', L' ');
    text = trimCopy(text);
    if (text.size() > 4096)
    {
        text.resize(4096);
    }
    return text;
}
}

void captureClipboardHistory(HWND owner)
{
    auto value = clipboardText(owner);
    if (!value)
    {
        return;
    }

    std::wstring text = compactClipboardText(*value);
    if (text.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_history.erase(std::remove(g_history.begin(), g_history.end(), text), g_history.end());
    g_history.insert(g_history.begin(), std::move(text));
    if (g_history.size() > 80)
    {
        g_history.resize(80);
    }
}

std::vector<std::wstring> clipboardHistorySnapshot()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_history;
}

