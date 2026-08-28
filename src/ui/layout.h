#pragma once

#include "../core/settings.h"
#include "../core/types.h"

#include <d2d1.h>

#include <vector>

// All geometry is in DIPs. The render target carries the DPI, so nothing in the
// UI multiplies by a scale factor.
namespace metrics
{
constexpr float windowWidth = 720.0f;

constexpr float headerHeight = 74.0f;
constexpr float listTop = 82.0f;
constexpr float rowPitch = 50.0f;
constexpr float rowHeight = 46.0f;
constexpr float detailRowPitch = 68.0f;
constexpr float detailRowHeight = 64.0f;
constexpr float footerHeight = 34.0f;
constexpr float listBottomPad = 8.0f;
constexpr float emptyStateHeight = 60.0f;

constexpr float sideMargin = 14.0f;
constexpr float textMargin = 24.0f;
constexpr float rowRadius = 10.0f;

constexpr float settingsHeaderHeight = 78.0f;
constexpr float settingsListTop = 86.0f;
constexpr float settingsSectionHeight = 38.0f;
constexpr float settingsItemHeight = 54.0f;
constexpr float settingsHeight = 640.0f;
constexpr float settingsControlWidth = 200.0f;

constexpr float scrollBarWidth = 4.0f;
constexpr float scrollBarInset = 5.0f;
}

struct PaletteRowRects
{
    D2D1_RECT_F row{};
    D2D1_RECT_F iconBox{};
    D2D1_RECT_F title{};
    D2D1_RECT_F subtitle{};
    D2D1_RECT_F detail{};
    D2D1_RECT_F hint{};
};

struct PaletteLayout
{
    float width = 0.0f;
    float height = 0.0f;
    D2D1_RECT_F header{};
    D2D1_RECT_F searchIcon{};
    D2D1_RECT_F queryText{};
    D2D1_RECT_F modePill{};
    D2D1_RECT_F divider{};
    D2D1_RECT_F listArea{};
    D2D1_RECT_F emptyText{};
    D2D1_RECT_F footer{};
    D2D1_RECT_F statusText{};
    D2D1_RECT_F footerHint{};
    std::vector<PaletteRowRects> rows;
};

struct SettingsRowRects
{
    bool isHeader = false;
    D2D1_RECT_F row{};
    D2D1_RECT_F headerText{};
    D2D1_RECT_F title{};
    D2D1_RECT_F subtitle{};
    D2D1_RECT_F control{};
    D2D1_RECT_F toggle{};
    D2D1_RECT_F minus{};
    D2D1_RECT_F value{};
    D2D1_RECT_F plus{};
    D2D1_RECT_F action{};
    D2D1_RECT_F segments[4]{};
    int segmentCount = 0;
};

struct SettingsLayout
{
    float width = 0.0f;
    float height = 0.0f;
    float contentHeight = 0.0f;
    float maxScroll = 0.0f;
    D2D1_RECT_F header{};
    D2D1_RECT_F titleText{};
    D2D1_RECT_F subtitleText{};
    D2D1_RECT_F divider{};
    D2D1_RECT_F listArea{};
    D2D1_RECT_F footer{};
    D2D1_RECT_F statusText{};
    D2D1_RECT_F footerHint{};
    D2D1_RECT_F scrollThumb{};
    bool showScrollbar = false;
    // Already offset by scrollY, so painting and hit-testing share coordinates.
    std::vector<SettingsRowRects> rows;
};

float paletteHeightForRows(int rowCount, bool detailRows = false);

PaletteLayout computePaletteLayout(float width, float height, int rowCount, bool detailRows = false);
SettingsLayout computeSettingsLayout(float width, float height, const std::vector<SettingRow>& rows, float scrollY);

// Scroll offset needed to bring a row fully inside the settings list viewport.
float settingsScrollToReveal(const SettingsLayout& layout, int rowIndex, float currentScroll);

bool pointInRect(const D2D1_RECT_F& rect, float x, float y);
