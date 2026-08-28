#include "layout.h"

#include <algorithm>

namespace
{
D2D1_RECT_F rect(float left, float top, float right, float bottom)
{
    return D2D1_RECT_F{ left, top, right, bottom };
}

float settingsRowHeight(const SettingRow& row)
{
    return row.isHeader ? metrics::settingsSectionHeight : metrics::settingsItemHeight;
}
}

bool pointInRect(const D2D1_RECT_F& r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

float paletteHeightForRows(int rowCount, bool detailRows)
{
    const float pitch = detailRows ? metrics::detailRowPitch : metrics::rowPitch;
    const float body = rowCount > 0
        ? static_cast<float>(rowCount) * pitch
        : metrics::emptyStateHeight;
    return metrics::listTop + body + metrics::listBottomPad + metrics::footerHeight;
}

PaletteLayout computePaletteLayout(float width, float height, int rowCount, bool detailRows)
{
    PaletteLayout layout;
    layout.width = width;
    layout.height = height;

    layout.header = rect(0.0f, 0.0f, width, metrics::headerHeight);
    layout.searchIcon = rect(metrics::textMargin, 26.0f, metrics::textMargin + 22.0f, 48.0f);
    layout.modePill = rect(width - metrics::textMargin - 96.0f, 25.0f, width - metrics::textMargin, 49.0f);
    layout.queryText = rect(metrics::textMargin + 34.0f, 20.0f, layout.modePill.left - 12.0f, 54.0f);
    layout.divider = rect(0.0f, metrics::headerHeight, width, metrics::headerHeight + 1.0f);

    const float footerTop = height - metrics::footerHeight;
    layout.footer = rect(0.0f, footerTop, width, height);
    layout.statusText = rect(metrics::textMargin, footerTop, width - 190.0f, height);
    layout.footerHint = rect(width - 190.0f, footerTop, width - metrics::textMargin, height);

    layout.listArea = rect(0.0f, metrics::listTop, width, footerTop - metrics::listBottomPad);
    layout.emptyText = rect(metrics::textMargin + 34.0f, metrics::listTop + 8.0f, width - metrics::textMargin, metrics::listTop + 48.0f);

    layout.rows.reserve(static_cast<size_t>(std::max(rowCount, 0)));
    const float pitch = detailRows ? metrics::detailRowPitch : metrics::rowPitch;
    const float rowHeight = detailRows ? metrics::detailRowHeight : metrics::rowHeight;
    for (int i = 0; i < rowCount; ++i)
    {
        const float top = metrics::listTop + static_cast<float>(i) * pitch;
        PaletteRowRects r;
        r.row = rect(metrics::sideMargin, top, width - metrics::sideMargin, top + rowHeight);
        if (detailRows)
        {
            r.iconBox = rect(r.row.left + 12.0f, top + 14.0f, r.row.left + 42.0f, top + 44.0f);
            r.title = rect(r.row.left + 54.0f, top + 4.0f, r.row.right - 100.0f, top + 24.0f);
            r.subtitle = rect(r.row.left + 54.0f, top + 25.0f, r.row.right - 100.0f, top + 42.0f);
            r.detail = rect(r.row.left + 54.0f, top + 43.0f, r.row.right - 100.0f, top + 60.0f);
            r.hint = rect(r.row.right - 96.0f, top + 22.0f, r.row.right - 12.0f, top + 42.0f);
        }
        else
        {
            r.iconBox = rect(r.row.left + 12.0f, top + 8.0f, r.row.left + 42.0f, top + 38.0f);
            r.title = rect(r.row.left + 54.0f, top + 4.0f, r.row.right - 100.0f, top + 24.0f);
            r.subtitle = rect(r.row.left + 54.0f, top + 23.0f, r.row.right - 100.0f, top + 41.0f);
            r.hint = rect(r.row.right - 96.0f, top + 13.0f, r.row.right - 12.0f, top + 33.0f);
        }
        layout.rows.push_back(r);
    }

    return layout;
}

SettingsLayout computeSettingsLayout(float width, float height, const std::vector<SettingRow>& rows, float scrollY)
{
    SettingsLayout layout;
    layout.width = width;
    layout.height = height;

    layout.header = rect(0.0f, 0.0f, width, metrics::settingsHeaderHeight);
    layout.titleText = rect(metrics::textMargin, 18.0f, width - metrics::textMargin, 46.0f);
    layout.subtitleText = rect(metrics::textMargin, 46.0f, width - metrics::textMargin, 66.0f);
    layout.divider = rect(0.0f, metrics::settingsHeaderHeight, width, metrics::settingsHeaderHeight + 1.0f);

    const float footerTop = height - metrics::footerHeight;
    layout.footer = rect(0.0f, footerTop, width, height);
    layout.statusText = rect(metrics::textMargin, footerTop, width - 220.0f, height);
    layout.footerHint = rect(width - 220.0f, footerTop, width - metrics::textMargin, height);

    layout.listArea = rect(0.0f, metrics::settingsListTop, width, footerTop - 6.0f);

    float content = 6.0f;
    for (const auto& row : rows)
    {
        content += settingsRowHeight(row);
    }
    content += 10.0f;
    layout.contentHeight = content;

    const float viewport = layout.listArea.bottom - layout.listArea.top;
    layout.maxScroll = std::max(0.0f, content - viewport);
    const float scroll = std::clamp(scrollY, 0.0f, layout.maxScroll);

    layout.showScrollbar = layout.maxScroll > 0.5f;
    if (layout.showScrollbar)
    {
        const float trackTop = layout.listArea.top + 4.0f;
        const float trackBottom = layout.listArea.bottom - 4.0f;
        const float trackHeight = std::max(1.0f, trackBottom - trackTop);
        const float thumbHeight = std::max(32.0f, trackHeight * (viewport / content));
        const float travel = trackHeight - thumbHeight;
        const float progress = layout.maxScroll > 0.0f ? scroll / layout.maxScroll : 0.0f;
        const float thumbTop = trackTop + travel * progress;
        const float right = width - metrics::scrollBarInset;
        layout.scrollThumb = rect(right - metrics::scrollBarWidth, thumbTop, right, thumbTop + thumbHeight);
    }

    layout.rows.reserve(rows.size());
    float y = layout.listArea.top + 6.0f - scroll;
    for (const auto& row : rows)
    {
        const float rowH = settingsRowHeight(row);
        SettingsRowRects r;
        r.isHeader = row.isHeader;
        r.row = rect(metrics::sideMargin, y, width - metrics::sideMargin - (layout.showScrollbar ? 8.0f : 0.0f), y + rowH);

        if (row.isHeader)
        {
            r.headerText = rect(r.row.left + 12.0f, y + 12.0f, r.row.right, y + rowH);
        }
        else
        {
            const float cy = y + rowH * 0.5f;
            r.control = rect(r.row.right - metrics::settingsControlWidth - 12.0f, y, r.row.right - 12.0f, y + rowH);
            r.title = rect(r.row.left + 14.0f, y + 8.0f, r.control.left - 16.0f, y + 28.0f);
            r.subtitle = rect(r.row.left + 14.0f, y + 28.0f, r.control.left - 16.0f, y + 46.0f);

            switch (row.item.kind)
            {
            case SettingKind::Toggle:
                r.toggle = rect(r.control.right - 46.0f, cy - 13.0f, r.control.right, cy + 13.0f);
                break;
            case SettingKind::Stepper:
                // Real buttons: the drawn - and + are exactly what the hit test uses.
                r.minus = rect(r.control.right - 132.0f, cy - 14.0f, r.control.right - 102.0f, cy + 14.0f);
                r.value = rect(r.control.right - 100.0f, cy - 14.0f, r.control.right - 32.0f, cy + 14.0f);
                r.plus = rect(r.control.right - 30.0f, cy - 14.0f, r.control.right, cy + 14.0f);
                break;
            case SettingKind::Choice:
            {
                int count = 0;
                settingChoices(row.item.field, count);
                count = std::clamp(count, 0, 4);
                r.segmentCount = count;
                if (count > 0)
                {
                    const float segmentWidth = 58.0f;
                    const float total = segmentWidth * static_cast<float>(count);
                    const float left = r.control.right - total;
                    for (int i = 0; i < count; ++i)
                    {
                        r.segments[i] = rect(left + segmentWidth * static_cast<float>(i), cy - 14.0f,
                                             left + segmentWidth * static_cast<float>(i + 1), cy + 14.0f);
                    }
                }
                break;
            }
            case SettingKind::Action:
            {
                const float width = (row.item.field == SettingField::ProviderShortcut ||
                                     row.item.field == SettingField::ProviderPrefix) ? 132.0f : 92.0f;
                r.action = rect(r.control.right - width, cy - 15.0f, r.control.right, cy + 15.0f);
                break;
            }
            }
        }

        layout.rows.push_back(r);
        y += rowH;
    }

    return layout;
}

float settingsScrollToReveal(const SettingsLayout& layout, int rowIndex, float currentScroll)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(layout.rows.size()))
    {
        return currentScroll;
    }

    const SettingsRowRects& row = layout.rows[rowIndex];
    const float viewTop = layout.listArea.top;
    const float viewBottom = layout.listArea.bottom;

    float scroll = currentScroll;
    if (row.row.top < viewTop + 4.0f)
    {
        scroll -= (viewTop + 4.0f) - row.row.top;
    }
    else if (row.row.bottom > viewBottom - 4.0f)
    {
        scroll += row.row.bottom - (viewBottom - 4.0f);
    }
    return std::clamp(scroll, 0.0f, layout.maxScroll);
}
