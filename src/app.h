#pragma once

#include "core/search.h"
#include "core/types.h"
#include "ui/editor.h"
#include "ui/graphics.h"
#include "ui/icons.h"
#include "ui/theme.h"

#include <shellapi.h>

#include <string>
#include <vector>

// Which sub-control of a settings row the pointer is currently pressing, so the
// painter can render a pressed state that matches where the click actually landed.
enum class PressedPart
{
    None,
    Toggle,
    Minus,
    Plus,
    Segment,
    Action,
};

struct AppState
{
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool visible = false;
    UiMode mode = UiMode::Palette;

    TextEditor editor;
    bool caretVisible = true;
    float queryScrollX = 0.0f;

    std::vector<Result> results;
    QueryMode queryMode = QueryMode::Commands;
    std::wstring forcedProviderId;
    std::vector<std::wstring> highlightTerms;
    int selected = 0;
    int hovered = -1;

    bool actionMenu = false;
    Command actionTarget;
    std::wstring actionReturnText;

    int settingsSelected = 0;
    int settingsHovered = -1;
    float settingsScroll = 0.0f;

    // Inline numeric entry: typing digits on a stepper row edits the value directly
    // instead of requiring dozens of button presses.
    bool editingNumber = false;
    std::wstring numberBuffer;
    bool capturingShortcut = false;
    std::wstring capturingShortcutProvider;

    int pressedRow = -1;
    PressedPart pressedPart = PressedPart::None;
    int pressedSegment = -1;
    int pressedPaletteRow = -1;
    bool draggingText = false;

    Theme theme;
    Graphics graphics;
    GlyphCache glyphs;
    ShellIconCache icons;

    NOTIFYICONDATAW tray{};
    float windowHeightDip = 0.0f;
    bool mouseTracking = false;
};

extern AppState g_app;

float pxToDip(int pixels, UINT dpi);
int dipToPx(float dip, UINT dpi);

void refreshTheme();
