#include "shortcuts.h"

#include "util.h"

#include <cwctype>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
bool down(int vk)
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

std::wstring keyName(UINT vk)
{
    if (vk >= L'A' && vk <= L'Z')
    {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= L'0' && vk <= L'9')
    {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    }

    switch (vk)
    {
    case VK_OEM_COMMA: return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_PLUS: return L"=";
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_BACK: return L"Backspace";
    case VK_DELETE: return L"Delete";
    case VK_INSERT: return L"Insert";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    default: return {};
    }
}

UINT vkFromToken(const std::wstring& token)
{
    if (token.size() == 1)
    {
        const wchar_t ch = static_cast<wchar_t>(std::towupper(token[0]));
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))
        {
            return ch;
        }
        SHORT vk = VkKeyScanW(ch);
        if (vk != -1)
        {
            return static_cast<UINT>(vk & 0xff);
        }
    }

    const std::wstring lower = lowerCopy(token);
    if (lower.size() >= 2 && lower[0] == L'f')
    {
        const int number = _wtoi(lower.substr(1).c_str());
        if (number >= 1 && number <= 24)
        {
            return VK_F1 + number - 1;
        }
    }

    static const std::unordered_map<std::wstring, UINT> names = {
        { L"space", VK_SPACE }, { L"tab", VK_TAB }, { L"backspace", VK_BACK },
        { L"delete", VK_DELETE }, { L"del", VK_DELETE }, { L"insert", VK_INSERT },
        { L"ins", VK_INSERT }, { L"home", VK_HOME }, { L"end", VK_END },
        { L"pageup", VK_PRIOR }, { L"pagedown", VK_NEXT }, { L"up", VK_UP },
        { L"down", VK_DOWN }, { L"left", VK_LEFT }, { L"right", VK_RIGHT },
        { L",", VK_OEM_COMMA }, { L".", VK_OEM_PERIOD }, { L"-", VK_OEM_MINUS },
        { L"=", VK_OEM_PLUS },
    };
    const auto it = names.find(lower);
    return it == names.end() ? 0 : it->second;
}
}

bool isModifierKey(WPARAM key)
{
    switch (key)
    {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

std::wstring formatShortcut(UINT modifiers, UINT vk)
{
    std::vector<std::wstring> parts;
    if (modifiers & MOD_CONTROL)
    {
        parts.push_back(L"Ctrl");
    }
    if (modifiers & MOD_ALT)
    {
        parts.push_back(L"Alt");
    }
    if (modifiers & MOD_SHIFT)
    {
        parts.push_back(L"Shift");
    }
    if (modifiers & MOD_WIN)
    {
        parts.push_back(L"Win");
    }

    const std::wstring key = keyName(vk);
    if (key.empty())
    {
        return {};
    }
    parts.push_back(key);

    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
        {
            out += L"+";
        }
        out += parts[i];
    }
    return out;
}

std::optional<ShortcutSpec> parseShortcutText(const std::wstring& text)
{
    const std::wstring trimmed = trimCopy(text);
    if (trimmed.empty() || lowerCopy(trimmed) == L"none")
    {
        return std::nullopt;
    }

    std::wstringstream stream(trimmed);
    std::wstring token;
    std::vector<std::wstring> parts;
    while (std::getline(stream, token, L'+'))
    {
        token = trimCopy(token);
        if (!token.empty())
        {
            parts.push_back(token);
        }
    }
    if (parts.empty())
    {
        return std::nullopt;
    }

    ShortcutSpec spec;
    for (size_t i = 0; i + 1 < parts.size(); ++i)
    {
        const std::wstring part = lowerCopy(parts[i]);
        if (part == L"ctrl" || part == L"control")
        {
            spec.modifiers |= MOD_CONTROL;
        }
        else if (part == L"alt")
        {
            spec.modifiers |= MOD_ALT;
        }
        else if (part == L"shift")
        {
            spec.modifiers |= MOD_SHIFT;
        }
        else if (part == L"win" || part == L"windows")
        {
            spec.modifiers |= MOD_WIN;
        }
    }

    spec.vk = vkFromToken(parts.back());
    spec.text = formatShortcut(spec.modifiers, spec.vk);
    if (spec.vk == 0 || spec.modifiers == 0 || spec.text.empty())
    {
        return std::nullopt;
    }
    return spec;
}

std::optional<ShortcutSpec> shortcutFromKeyState(WPARAM key)
{
    if (isModifierKey(key))
    {
        return std::nullopt;
    }

    ShortcutSpec spec;
    if (down(VK_CONTROL) || down(VK_LCONTROL) || down(VK_RCONTROL))
    {
        spec.modifiers |= MOD_CONTROL;
    }
    if (down(VK_MENU) || down(VK_LMENU) || down(VK_RMENU))
    {
        spec.modifiers |= MOD_ALT;
    }
    if (down(VK_SHIFT) || down(VK_LSHIFT) || down(VK_RSHIFT))
    {
        spec.modifiers |= MOD_SHIFT;
    }
    if (down(VK_LWIN) || down(VK_RWIN))
    {
        spec.modifiers |= MOD_WIN;
    }
    spec.vk = static_cast<UINT>(key);
    spec.text = formatShortcut(spec.modifiers, spec.vk);
    if (spec.modifiers == 0 || spec.text.empty())
    {
        return std::nullopt;
    }
    return spec;
}
