#include "theme.h"

#include <algorithm>

namespace
{
constexpr D2D1_COLOR_F kWhite{ 1.0f, 1.0f, 1.0f, 1.0f };
constexpr D2D1_COLOR_F kBlack{ 0.0f, 0.0f, 0.0f, 1.0f };
constexpr unsigned int kFallbackAccent = 0x0099BC;
}

D2D1_COLOR_F colorFromHex(unsigned int rgb, float alpha)
{
    return D2D1_COLOR_F{
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        alpha,
    };
}

D2D1_COLOR_F mixColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t)
{
    const float k = std::clamp(t, 0.0f, 1.0f);
    return D2D1_COLOR_F{
        a.r + (b.r - a.r) * k,
        a.g + (b.g - a.g) * k,
        a.b + (b.b - a.b) * k,
        a.a + (b.a - a.a) * k,
    };
}

D2D1_COLOR_F withAlpha(const D2D1_COLOR_F& color, float alpha)
{
    return D2D1_COLOR_F{ color.r, color.g, color.b, alpha };
}

float relativeLuminance(const D2D1_COLOR_F& color)
{
    return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
}

bool systemUsesDarkMode()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        &type,
        &value,
        &size);
    if (status != ERROR_SUCCESS)
    {
        return true;
    }
    return value == 0;
}

D2D1_COLOR_F systemAccentColor()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\DWM",
        L"AccentColor",
        RRF_RT_REG_DWORD,
        &type,
        &value,
        &size);
    if (status != ERROR_SUCCESS)
    {
        return colorFromHex(kFallbackAccent);
    }

    // The DWM value is packed 0xAABBGGRR, not the 0xAARRGGBB most Win32 APIs use.
    const unsigned int r = value & 0xFF;
    const unsigned int g = (value >> 8) & 0xFF;
    const unsigned int b = (value >> 16) & 0xFF;
    return colorFromHex((r << 16) | (g << 8) | b);
}

D2D1_COLOR_F usableAccent(const D2D1_COLOR_F& accent, bool dark)
{
    // Windows accents range from near-black to near-white. Pull whatever the user
    // picked into a band that still reads against our surface.
    const float target = dark ? 0.42f : 0.30f;
    D2D1_COLOR_F result = accent;

    if (dark)
    {
        for (int i = 0; i < 24 && relativeLuminance(result) < target; ++i)
        {
            result = mixColor(result, kWhite, 0.09f);
        }
    }
    else
    {
        for (int i = 0; i < 24 && relativeLuminance(result) > target; ++i)
        {
            result = mixColor(result, kBlack, 0.09f);
        }
    }

    return result;
}

Theme resolveTheme(const Settings& settings)
{
    bool dark = true;
    switch (settings.appearance)
    {
    case Appearance::Dark: dark = true; break;
    case Appearance::Light: dark = false; break;
    case Appearance::System: dark = systemUsesDarkMode(); break;
    }

    const D2D1_COLOR_F rawAccent = settings.useSystemAccent ? systemAccentColor() : colorFromHex(kFallbackAccent);
    const D2D1_COLOR_F accent = usableAccent(rawAccent, dark);

    Theme t;
    t.dark = dark;
    t.accent = accent;
    t.onAccent = relativeLuminance(accent) > 0.55f ? colorFromHex(0x0E1116) : kWhite;

    if (dark)
    {
        t.windowBg = colorFromHex(0x16181D);
        t.headerBg = colorFromHex(0x1B1E24);
        t.footerBg = colorFromHex(0x131519);
        t.divider = colorFromHex(0x23262E);
        t.border = colorFromHex(0x2A2E37);

        t.rowHover = colorFromHex(0x20242C);
        t.rowSelected = colorFromHex(0x272D38);

        t.textPrimary = colorFromHex(0xF2F5F8);
        t.textSecondary = colorFromHex(0x99A2B1);
        t.textMuted = colorFromHex(0x6E7683);
        t.textPlaceholder = colorFromHex(0x5C6470);

        t.iconBg = colorFromHex(0x232831);
        t.iconFg = colorFromHex(0xC3CBD6);
        t.iconSelectedBg = mixColor(t.rowSelected, accent, 0.22f);

        t.toggleOff = colorFromHex(0x3C424E);
        t.toggleKnob = colorFromHex(0xF5F7FA);

        t.pillBg = colorFromHex(0x232831);
        t.pillText = colorFromHex(0x99A2B1);

        t.controlBg = colorFromHex(0x232831);
        t.controlHover = colorFromHex(0x2D343F);
        t.controlPressed = colorFromHex(0x373F4C);
        t.controlText = colorFromHex(0xD8DEE7);

        t.scrollThumb = colorFromHex(0x3A414C);
    }
    else
    {
        t.windowBg = colorFromHex(0xFBFCFD);
        t.headerBg = colorFromHex(0xFFFFFF);
        t.footerBg = colorFromHex(0xF3F5F8);
        t.divider = colorFromHex(0xE4E8ED);
        t.border = colorFromHex(0xD5DBE3);

        t.rowHover = colorFromHex(0xF0F3F7);
        t.rowSelected = colorFromHex(0xE7ECF2);

        t.textPrimary = colorFromHex(0x14171C);
        t.textSecondary = colorFromHex(0x59636F);
        t.textMuted = colorFromHex(0x828B98);
        t.textPlaceholder = colorFromHex(0x98A1AD);

        t.iconBg = colorFromHex(0xEDF0F4);
        t.iconFg = colorFromHex(0x4C5561);
        t.iconSelectedBg = mixColor(t.rowSelected, accent, 0.18f);

        t.toggleOff = colorFromHex(0xC2C9D3);
        t.toggleKnob = colorFromHex(0xFFFFFF);

        t.pillBg = colorFromHex(0xEDF0F4);
        t.pillText = colorFromHex(0x59636F);

        t.controlBg = colorFromHex(0xEDF0F4);
        t.controlHover = colorFromHex(0xE1E6ED);
        t.controlPressed = colorFromHex(0xD3DAE3);
        t.controlText = colorFromHex(0x2C3540);

        t.scrollThumb = colorFromHex(0xC2C9D3);
    }

    t.accentSoft = mixColor(t.windowBg, accent, dark ? 0.22f : 0.16f);
    t.caret = accent;
    t.selectionBg = mixColor(t.headerBg, accent, dark ? 0.38f : 0.28f);

    return t;
}
