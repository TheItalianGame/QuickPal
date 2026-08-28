#pragma once

#include <windows.h>

#include <optional>
#include <string>

struct ShortcutSpec
{
    UINT modifiers = 0;
    UINT vk = 0;
    std::wstring text;
};

std::optional<ShortcutSpec> parseShortcutText(const std::wstring& text);
std::optional<ShortcutSpec> shortcutFromKeyState(WPARAM key);
std::wstring formatShortcut(UINT modifiers, UINT vk);
bool isModifierKey(WPARAM key);

