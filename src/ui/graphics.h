#pragma once

#include "comptr.h"
#include "theme.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdint>
#include <string>
#include <unordered_map>

enum class FontRole
{
    Query,
    RowTitle,
    RowSubtitle,
    SectionHeader,
    Hint,
    Pill,
    SettingTitle,
    SettingSubtitle,
    Value,
    Mono,
    Count,
};

class Graphics
{
public:
    bool createFactories();
    void destroy();

    // Device resources live as long as the app can keep them, so summoning the
    // palette never pays for device creation.
    bool ensureDeviceResources(HWND hwnd, D2D1_SIZE_U pixelSize, UINT dpi);
    void discardDeviceResources();
    void resize(D2D1_SIZE_U pixelSize);
    void setDpi(UINT dpi);

    bool ready() const { return renderTarget_.get() != nullptr; }
    ID2D1HwndRenderTarget* rt() const { return renderTarget_.get(); }
    ID2D1Factory* d2d() const { return d2dFactory_.get(); }
    IDWriteFactory* dwrite() const { return dwriteFactory_.get(); }
    IWICImagingFactory* wic() const { return wicFactory_.get(); }

    // One scratch brush recolored per draw: changing a solid brush's color costs
    // nothing, while creating a brush per fill (the old GDI pattern) does.
    ID2D1SolidColorBrush* solid(const D2D1_COLOR_F& color);

    // Stable brush used as a DirectWrite drawing effect for matched characters.
    // It must outlive the layouts that reference it.
    ID2D1SolidColorBrush* highlightBrush() const { return highlight_.get(); }
    void setHighlightColor(const D2D1_COLOR_F& color);

    IDWriteTextFormat* format(FontRole role) const;

    // Cached, DPI-independent (the render target scales DIPs for us). `variant`
    // separates otherwise-identical strings that carry different per-range
    // formatting, so highlighted and plain copies never collide. `outCreated`
    // reports a cache miss, so callers only re-apply per-range styling once.
    IDWriteTextLayout* layout(const std::wstring& text, FontRole role, float maxWidth, float maxHeight, uint64_t variant = 0, bool* outCreated = nullptr);
    void clearLayoutCache();

private:
    bool createTextFormats();

    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    ComPtr<ID2D1SolidColorBrush> scratch_;
    ComPtr<ID2D1SolidColorBrush> highlight_;
    ComPtr<IDWriteTextFormat> formats_[static_cast<size_t>(FontRole::Count)];

    std::unordered_map<uint64_t, ComPtr<IDWriteTextLayout>> layoutCache_;
    UINT dpi_ = 96;
};
