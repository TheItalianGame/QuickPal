#include "paint.h"

#include "../app.h"
#include "../core/indexer.h"
#include "../core/search.h"
#include "../core/settings.h"
#include "../core/util.h"
#include "icons.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cstdint>

namespace
{
struct TextStyle
{
    FontRole role = FontRole::RowTitle;
    D2D1_COLOR_F color{};
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
};

uint64_t hashCombine(uint64_t seed, uint64_t value)
{
    seed ^= value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

D2D1_RECT_F inset(const D2D1_RECT_F& r, float amount)
{
    return D2D1_RECT_F{ r.left + amount, r.top + amount, r.right - amount, r.bottom - amount };
}

void fillRounded(ID2D1RenderTarget* rt, Graphics& gfx, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color)
{
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gfx.solid(color));
}

void fill(ID2D1RenderTarget* rt, Graphics& gfx, const D2D1_RECT_F& r, const D2D1_COLOR_F& color)
{
    rt->FillRectangle(r, gfx.solid(color));
}

void drawText(ID2D1RenderTarget* rt, Graphics& gfx, const std::wstring& text, const D2D1_RECT_F& box,
              const TextStyle& style, const std::vector<HighlightRange>* highlights = nullptr)
{
    if (text.empty())
    {
        return;
    }

    const float width = std::max(0.0f, box.right - box.left);
    const float height = std::max(0.0f, box.bottom - box.top);

    uint64_t variant = hashCombine(0x9E37ULL, static_cast<uint64_t>(style.align));
    if (highlights)
    {
        for (const auto& range : *highlights)
        {
            variant = hashCombine(variant, (static_cast<uint64_t>(range.start) << 20) | range.length);
        }
    }

    bool created = false;
    IDWriteTextLayout* layout = gfx.layout(text, style.role, width, height, variant, &created);
    if (!layout)
    {
        return;
    }

    // Per-range styling only needs applying on a cache miss; the cache key already
    // separates layouts that carry different styling.
    if (created)
    {
        layout->SetTextAlignment(style.align);
        if (highlights)
        {
            for (const auto& range : *highlights)
            {
                const DWRITE_TEXT_RANGE dwRange{ range.start, range.length };
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, dwRange);
                layout->SetDrawingEffect(gfx.highlightBrush(), dwRange);
            }
        }
    }

    rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout, gfx.solid(style.color),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float measureTextWidth(Graphics& gfx, const std::wstring& text, FontRole role)
{
    if (text.empty())
    {
        return 0.0f;
    }
    IDWriteTextLayout* layout = gfx.layout(text, role, 4096.0f, 64.0f, 0xA11CE);
    if (!layout)
    {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)))
    {
        return 0.0f;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

const wchar_t* actionHint(CommandKind kind)
{
    switch (kind)
    {
    case CommandKind::Web: return L"Search";
    case CommandKind::Shell: return L"Run";
    case CommandKind::Calc: return L"Copy";
    case CommandKind::Window: return L"Switch";
    case CommandKind::OpenSettings: return L"Configure";
    case CommandKind::ReloadIndex: return L"Reload";
    case CommandKind::ExitApp: return L"Quit";
    case CommandKind::OpenCalculator: return L"Compute";
    default: return L"Open";
    }
}

void drawPill(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const D2D1_RECT_F& anchor, const std::wstring& text)
{
    if (text.empty())
    {
        return;
    }
    const float textWidth = measureTextWidth(gfx, text, FontRole::Pill);
    const float pillWidth = std::min(anchor.right - anchor.left, textWidth + 20.0f);
    const D2D1_RECT_F box{ anchor.right - pillWidth, anchor.top, anchor.right, anchor.bottom };
    const float radius = (box.bottom - box.top) * 0.5f;
    fillRounded(rt, gfx, box, radius, theme.accentSoft);
    drawText(rt, gfx, text, box, TextStyle{ FontRole::Pill, theme.accent, DWRITE_TEXT_ALIGNMENT_CENTER });
}

void drawToggle(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const D2D1_RECT_F& box, bool on, bool hot)
{
    const float radius = (box.bottom - box.top) * 0.5f;
    D2D1_COLOR_F track = on ? theme.accent : theme.toggleOff;
    if (hot)
    {
        track = mixColor(track, theme.textPrimary, 0.12f);
    }
    fillRounded(rt, gfx, box, radius, track);

    const float knobRadius = radius - 3.0f;
    const float cx = on ? box.right - 3.0f - knobRadius : box.left + 3.0f + knobRadius;
    const float cy = box.top + radius;
    const D2D1_COLOR_F knob = on ? theme.onAccent : theme.toggleKnob;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), knobRadius, knobRadius), gfx.solid(knob));
}

void drawStepperButton(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const D2D1_RECT_F& box,
                       bool isPlus, bool hot, bool pressed, bool enabled)
{
    D2D1_COLOR_F background = theme.controlBg;
    if (pressed)
    {
        background = theme.controlPressed;
    }
    else if (hot)
    {
        background = theme.controlHover;
    }
    fillRounded(rt, gfx, box, 7.0f, background);

    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float half = 5.0f;
    ID2D1SolidColorBrush* brush = gfx.solid(enabled ? theme.controlText : theme.textMuted);
    rt->DrawLine(D2D1::Point2F(cx - half, cy), D2D1::Point2F(cx + half, cy), brush, 1.7f);
    if (isPlus)
    {
        rt->DrawLine(D2D1::Point2F(cx, cy - half), D2D1::Point2F(cx, cy + half), brush, 1.7f);
    }
}

void drawRowBackground(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const D2D1_RECT_F& row, bool selected, bool hovered)
{
    if (selected)
    {
        fillRounded(rt, gfx, row, metrics::rowRadius, theme.rowSelected);
        const D2D1_RECT_F marker{ row.left + 1.0f, row.top + 9.0f, row.left + 4.0f, row.bottom - 9.0f };
        fillRounded(rt, gfx, marker, 1.5f, theme.accent);
    }
    else if (hovered)
    {
        fillRounded(rt, gfx, row, metrics::rowRadius, theme.rowHover);
    }
}

void drawCommandIcon(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const Command& command,
                     const D2D1_RECT_F& box, bool selected)
{
    fillRounded(rt, gfx, box, 8.0f, selected ? theme.iconSelectedBg : theme.iconBg);

    if (ID2D1Bitmap* bitmap = g_app.icons.acquire(command, gfx))
    {
        rt->DrawBitmap(bitmap, inset(box, 4.0f), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }

    drawKindGlyph(rt, gfx, g_app.glyphs, command.kind, inset(box, 6.0f),
                  selected ? theme.accent : theme.iconFg);
}

void paintQueryField(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const PaletteLayout& layout)
{
    drawSearchGlyph(rt, gfx, layout.searchIcon, theme.textMuted);

    const std::wstring& text = g_app.editor.text();
    const D2D1_RECT_F box = layout.queryText;
    const float boxWidth = box.right - box.left;

    if (text.empty())
    {
        drawText(rt, gfx, L"Search apps, files, settings — try f, ??, >, = or win",
                 box, TextStyle{ FontRole::Query, theme.textPlaceholder });
        g_app.queryScrollX = 0.0f;
        // The caret still belongs on an empty field.
        if (g_app.caretVisible)
        {
            const D2D1_RECT_F caret{ box.left, box.top + 6.0f, box.left + 2.0f, box.bottom - 6.0f };
            fillRounded(rt, gfx, caret, 1.0f, theme.caret);
        }
        return;
    }

    // A generous max width means DirectWrite never ellipsizes the query; we scroll
    // it under a clip instead so the caret stays on screen.
    bool created = false;
    IDWriteTextLayout* layoutText = gfx.layout(text, FontRole::Query, 100000.0f, box.bottom - box.top, 0xC4E7, &created);
    if (!layoutText)
    {
        return;
    }

    FLOAT caretX = 0.0f;
    FLOAT caretY = 0.0f;
    DWRITE_HIT_TEST_METRICS caretMetrics{};
    layoutText->HitTestTextPosition(static_cast<UINT32>(g_app.editor.caret()), FALSE, &caretX, &caretY, &caretMetrics);

    DWRITE_TEXT_METRICS textMetrics{};
    layoutText->GetMetrics(&textMetrics);
    const float textWidth = textMetrics.widthIncludingTrailingWhitespace;

    float scroll = g_app.queryScrollX;
    if (caretX - scroll < 6.0f)
    {
        scroll = caretX - 6.0f;
    }
    if (caretX - scroll > boxWidth - 8.0f)
    {
        scroll = caretX - (boxWidth - 8.0f);
    }
    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, textWidth - boxWidth + 8.0f));
    g_app.queryScrollX = scroll;

    rt->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_ALIASED);

    D2D1_MATRIX_3X2_F previous;
    rt->GetTransform(&previous);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(-scroll, 0.0f) * previous);

    if (g_app.editor.hasSelection())
    {
        const auto [begin, end] = g_app.editor.selection();
        UINT32 needed = 0;
        layoutText->HitTestTextRange(static_cast<UINT32>(begin), static_cast<UINT32>(end - begin),
                                     box.left, box.top, nullptr, 0, &needed);
        if (needed > 0)
        {
            std::vector<DWRITE_HIT_TEST_METRICS> runs(needed);
            if (SUCCEEDED(layoutText->HitTestTextRange(static_cast<UINT32>(begin), static_cast<UINT32>(end - begin),
                                                       box.left, box.top, runs.data(), needed, &needed)))
            {
                for (const auto& run : runs)
                {
                    const D2D1_RECT_F selection{ run.left, run.top, run.left + run.width, run.top + run.height };
                    fillRounded(rt, gfx, selection, 2.0f, theme.selectionBg);
                }
            }
        }
    }

    rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), layoutText, gfx.solid(theme.textPrimary));

    if (g_app.caretVisible)
    {
        const float x = box.left + caretX;
        const float top = box.top + caretY;
        const D2D1_RECT_F caret{ x, top + 2.0f, x + 2.0f, top + caretMetrics.height - 2.0f };
        fillRounded(rt, gfx, caret, 1.0f, theme.caret);
    }

    rt->SetTransform(previous);
    rt->PopAxisAlignedClip();
}

void paintPalette(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const PaletteLayout& layout)
{
    fill(rt, gfx, layout.header, theme.headerBg);
    fill(rt, gfx, layout.divider, theme.divider);

    paintQueryField(rt, gfx, theme, layout);

    if (const wchar_t* mode = queryModeLabel(g_app.queryMode))
    {
        drawPill(rt, gfx, theme, layout.modePill, mode);
    }

    const int rowCount = std::min<int>(static_cast<int>(g_app.results.size()), static_cast<int>(layout.rows.size()));
    for (int i = 0; i < rowCount; ++i)
    {
        const Command& command = g_app.results[static_cast<size_t>(i)].command;
        const PaletteRowRects& r = layout.rows[static_cast<size_t>(i)];
        const bool selected = i == g_app.selected;
        const bool hovered = i == g_app.hovered;

        drawRowBackground(rt, gfx, theme, r.row, selected, hovered);
        drawCommandIcon(rt, gfx, theme, command, r.iconBox, selected);

        const auto highlights = highlightRanges(command.title, g_app.highlightTerms);
        drawText(rt, gfx, command.title, r.title,
                 TextStyle{ FontRole::RowTitle, theme.textPrimary },
                 highlights.empty() ? nullptr : &highlights);

        const std::wstring& subtitle = command.subtitle.empty() ? command.arg : command.subtitle;
        drawText(rt, gfx, subtitle, r.subtitle, TextStyle{ FontRole::RowSubtitle, theme.textSecondary });

        if (selected)
        {
            drawText(rt, gfx, actionHint(command.kind), r.hint,
                     TextStyle{ FontRole::Hint, theme.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING });
        }
    }

    if (g_app.results.empty())
    {
        const std::wstring message = g_app.editor.empty() ? L"Start typing to search" : L"No matches";
        drawText(rt, gfx, message, layout.emptyText, TextStyle{ FontRole::RowTitle, theme.textMuted });
    }

    fill(rt, gfx, layout.footer, theme.footerBg);
    drawText(rt, gfx, getStatus(), layout.statusText, TextStyle{ FontRole::Hint, theme.textMuted });
    drawText(rt, gfx, L"↑↓ move  ·  ↵ run  ·  Esc close", layout.footerHint,
             TextStyle{ FontRole::Hint, theme.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING });
}

void paintSettings(ID2D1RenderTarget* rt, Graphics& gfx, const Theme& theme, const SettingsLayout& layout)
{
    const Settings settings = getSettingsSnapshot();
    const auto& rows = settingRows();

    fill(rt, gfx, layout.header, theme.headerBg);
    fill(rt, gfx, layout.divider, theme.divider);
    drawText(rt, gfx, L"QuickPal settings", layout.titleText, TextStyle{ FontRole::Query, theme.textPrimary });
    drawText(rt, gfx, L"Index providers, appearance, and latency controls", layout.subtitleText,
             TextStyle{ FontRole::SettingSubtitle, theme.textSecondary });

    rt->PushAxisAlignedClip(layout.listArea, D2D1_ANTIALIAS_MODE_ALIASED);

    for (size_t i = 0; i < layout.rows.size() && i < rows.size(); ++i)
    {
        const SettingsRowRects& r = layout.rows[i];
        if (r.row.bottom < layout.listArea.top || r.row.top > layout.listArea.bottom)
        {
            continue;
        }

        const SettingRow& row = rows[i];
        if (row.isHeader)
        {
            drawText(rt, gfx, row.header, r.headerText, TextStyle{ FontRole::SectionHeader, theme.accent });
            continue;
        }

        const int index = static_cast<int>(i);
        const bool selected = index == g_app.settingsSelected;
        const bool hovered = index == g_app.settingsHovered;
        const bool pressedHere = index == g_app.pressedRow;

        drawRowBackground(rt, gfx, theme, r.row, selected, hovered);
        drawText(rt, gfx, row.item.title, r.title, TextStyle{ FontRole::SettingTitle, theme.textPrimary });
        drawText(rt, gfx, row.item.subtitle, r.subtitle, TextStyle{ FontRole::SettingSubtitle, theme.textSecondary });

        switch (row.item.kind)
        {
        case SettingKind::Toggle:
            drawToggle(rt, gfx, theme, r.toggle, settingToggleValue(row.item.field, settings), hovered || selected);
            break;

        case SettingKind::Stepper:
        {
            const StepperSpec spec = stepperSpec(row.item.field);
            const int value = settingNumericValue(row.item.field, settings);
            drawStepperButton(rt, gfx, theme, r.minus, false, hovered,
                              pressedHere && g_app.pressedPart == PressedPart::Minus, value > spec.minValue);
            drawStepperButton(rt, gfx, theme, r.plus, true, hovered,
                              pressedHere && g_app.pressedPart == PressedPart::Plus, value < spec.maxValue);

            const bool editing = g_app.editingNumber && selected;
            fillRounded(rt, gfx, r.value, 7.0f, editing ? theme.accentSoft : theme.controlBg);
            if (editing)
            {
                const float radius = 7.0f;
                rt->DrawRoundedRectangle(D2D1::RoundedRect(inset(r.value, 0.5f), radius, radius),
                                         gfx.solid(theme.accent), 1.0f);
            }
            const std::wstring shown = editing
                ? (g_app.numberBuffer.empty() ? std::wstring(L"_") : g_app.numberBuffer)
                : settingValueText(row.item.field, settings);
            drawText(rt, gfx, shown, r.value,
                     TextStyle{ FontRole::Value, editing ? theme.accent : theme.controlText, DWRITE_TEXT_ALIGNMENT_CENTER });
            break;
        }

        case SettingKind::Choice:
        {
            int count = 0;
            const wchar_t* const* choices = settingChoices(row.item.field, count);
            const int active = settingChoiceIndex(row.item.field, settings);
            if (r.segmentCount > 0)
            {
                const D2D1_RECT_F track{ r.segments[0].left, r.segments[0].top,
                                         r.segments[r.segmentCount - 1].right, r.segments[0].bottom };
                fillRounded(rt, gfx, track, 8.0f, theme.controlBg);
            }
            for (int s = 0; s < r.segmentCount && s < count; ++s)
            {
                const bool isActive = s == active;
                if (isActive)
                {
                    fillRounded(rt, gfx, inset(r.segments[s], 2.0f), 6.0f, theme.accent);
                }
                else if (pressedHere && g_app.pressedPart == PressedPart::Segment && g_app.pressedSegment == s)
                {
                    fillRounded(rt, gfx, inset(r.segments[s], 2.0f), 6.0f, theme.controlPressed);
                }
                drawText(rt, gfx, choices[s], r.segments[s],
                         TextStyle{ FontRole::Pill, isActive ? theme.onAccent : theme.controlText, DWRITE_TEXT_ALIGNMENT_CENTER });
            }
            break;
        }

        case SettingKind::Action:
        {
            const bool pressed = pressedHere && g_app.pressedPart == PressedPart::Action;
            const D2D1_COLOR_F background = pressed ? theme.controlPressed : (hovered ? theme.controlHover : theme.controlBg);
            fillRounded(rt, gfx, r.action, 8.0f, background);
            drawText(rt, gfx, settingValueText(row.item.field, settings), r.action,
                     TextStyle{ FontRole::Value, theme.accent, DWRITE_TEXT_ALIGNMENT_CENTER });
            break;
        }
        }
    }

    rt->PopAxisAlignedClip();

    if (layout.showScrollbar)
    {
        const float radius = (layout.scrollThumb.right - layout.scrollThumb.left) * 0.5f;
        fillRounded(rt, gfx, layout.scrollThumb, radius, theme.scrollThumb);
    }

    fill(rt, gfx, layout.footer, theme.footerBg);
    drawText(rt, gfx, getStatus(), layout.statusText, TextStyle{ FontRole::Hint, theme.textMuted });
    drawText(rt, gfx, L"↑↓ move  ·  ←→ adjust  ·  ↵ apply  ·  Esc close",
             layout.footerHint, TextStyle{ FontRole::Hint, theme.textMuted, DWRITE_TEXT_ALIGNMENT_TRAILING });
}
}

D2D1_SIZE_F clientSizeDip()
{
    RECT client{};
    if (!g_app.hwnd || !GetClientRect(g_app.hwnd, &client))
    {
        return D2D1::SizeF(metrics::windowWidth, 400.0f);
    }
    return D2D1::SizeF(pxToDip(client.right - client.left, g_app.dpi),
                       pxToDip(client.bottom - client.top, g_app.dpi));
}

PaletteLayout buildPaletteLayout()
{
    const D2D1_SIZE_F size = clientSizeDip();
    return computePaletteLayout(size.width, size.height, static_cast<int>(g_app.results.size()));
}

SettingsLayout buildSettingsLayout()
{
    const D2D1_SIZE_F size = clientSizeDip();
    return computeSettingsLayout(size.width, size.height, settingRows(), g_app.settingsScroll);
}

bool paintFrame()
{
    Graphics& gfx = g_app.graphics;
    RECT client{};
    if (!g_app.hwnd || !GetClientRect(g_app.hwnd, &client))
    {
        return false;
    }

    const D2D1_SIZE_U pixelSize = D2D1::SizeU(
        static_cast<UINT32>(std::max<LONG>(client.right - client.left, 1)),
        static_cast<UINT32>(std::max<LONG>(client.bottom - client.top, 1)));

    if (!gfx.ensureDeviceResources(g_app.hwnd, pixelSize, g_app.dpi))
    {
        return false;
    }

    buildGlyphCache(g_app.glyphs, gfx.d2d());
    gfx.setHighlightColor(g_app.theme.accent);

    ID2D1RenderTarget* rt = gfx.rt();
    rt->BeginDraw();
    rt->Clear(g_app.theme.windowBg);

    if (g_app.mode == UiMode::Settings)
    {
        paintSettings(rt, gfx, g_app.theme, buildSettingsLayout());
    }
    else
    {
        paintPalette(rt, gfx, g_app.theme, buildPaletteLayout());
    }

    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        gfx.discardDeviceResources();
        g_app.icons.dropDeviceBitmaps();
        return false;
    }
    return SUCCEEDED(hr);
}
