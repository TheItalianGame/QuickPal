#pragma once

#include "../core/types.h"

#include <d2d1.h>

// Every color the UI can draw, named once. Nothing else in the codebase may
// hold a literal color value.
struct Theme
{
    bool dark = true;

    D2D1_COLOR_F windowBg;
    D2D1_COLOR_F headerBg;
    D2D1_COLOR_F footerBg;
    D2D1_COLOR_F divider;
    D2D1_COLOR_F border;

    D2D1_COLOR_F rowHover;
    D2D1_COLOR_F rowSelected;

    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F textMuted;
    D2D1_COLOR_F textPlaceholder;

    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentSoft;
    D2D1_COLOR_F onAccent;

    D2D1_COLOR_F iconBg;
    D2D1_COLOR_F iconFg;
    D2D1_COLOR_F iconSelectedBg;

    D2D1_COLOR_F toggleOff;
    D2D1_COLOR_F toggleKnob;

    D2D1_COLOR_F pillBg;
    D2D1_COLOR_F pillText;

    D2D1_COLOR_F caret;
    D2D1_COLOR_F selectionBg;

    D2D1_COLOR_F controlBg;
    D2D1_COLOR_F controlHover;
    D2D1_COLOR_F controlPressed;
    D2D1_COLOR_F controlText;

    D2D1_COLOR_F scrollThumb;
};

D2D1_COLOR_F colorFromHex(unsigned int rgb, float alpha = 1.0f);
D2D1_COLOR_F mixColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
D2D1_COLOR_F withAlpha(const D2D1_COLOR_F& color, float alpha);
float relativeLuminance(const D2D1_COLOR_F& color);

bool systemUsesDarkMode();
D2D1_COLOR_F systemAccentColor();

// Nudges an accent into a band that stays legible on the given background.
D2D1_COLOR_F usableAccent(const D2D1_COLOR_F& accent, bool dark);

Theme resolveTheme(const Settings& settings);
