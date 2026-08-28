#include "graphics.h"

#include <d2d1helper.h>

#include <algorithm>

namespace
{
constexpr size_t kMaxCachedLayouts = 600;

struct FontSpec
{
    const wchar_t* family;
    DWRITE_FONT_WEIGHT weight;
    float size;
};

// Sizes are in DIPs. The render target carries the DPI, so nothing here scales.
const FontSpec kFontSpecs[static_cast<size_t>(FontRole::Count)] = {
    /* Query */          { L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_NORMAL, 20.0f },
    /* RowTitle */       { L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f },
    /* RowSubtitle */    { L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 11.5f },
    /* SectionHeader */  { L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f },
    /* Hint */           { L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 11.0f },
    /* Pill */           { L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 10.0f },
    /* SettingTitle */   { L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f },
    /* SettingSubtitle */{ L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 11.0f },
    /* Value */          { L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f },
    /* Mono */           { L"Cascadia Mono", DWRITE_FONT_WEIGHT_NORMAL, 13.0f },
};

bool fontFamilyExists(IDWriteFactory* factory, const wchar_t* family)
{
    if (!factory || !family)
    {
        return false;
    }
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(collection.put(), FALSE)) || !collection)
    {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(collection->FindFamilyName(family, &index, &exists)))
    {
        return false;
    }
    return exists != FALSE;
}

uint64_t hashCombine(uint64_t seed, uint64_t value)
{
    seed ^= value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
    return seed;
}
}

bool Graphics::createFactories()
{
    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options, d2dFactory_.putVoid())))
    {
        return false;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwriteFactory_.put()))))
    {
        return false;
    }

    // WIC is only needed to decode shell icons; a failure here costs icons, not the app.
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(IWICImagingFactory), wicFactory_.putVoid());

    return createTextFormats();
}

bool Graphics::createTextFormats()
{
    if (!dwriteFactory_)
    {
        return false;
    }

    // Segoe UI Variable is Win11-only and Cascadia ships with Terminal; fall back
    // rather than letting DirectWrite silently substitute something arbitrary.
    const bool hasVariableText = fontFamilyExists(dwriteFactory_.get(), L"Segoe UI Variable Text");
    const bool hasVariableDisplay = fontFamilyExists(dwriteFactory_.get(), L"Segoe UI Variable Display");
    const bool hasCascadia = fontFamilyExists(dwriteFactory_.get(), L"Cascadia Mono");

    for (size_t i = 0; i < static_cast<size_t>(FontRole::Count); ++i)
    {
        FontSpec spec = kFontSpecs[i];
        if (wcscmp(spec.family, L"Segoe UI Variable Text") == 0 && !hasVariableText)
        {
            spec.family = L"Segoe UI";
        }
        else if (wcscmp(spec.family, L"Segoe UI Variable Display") == 0 && !hasVariableDisplay)
        {
            spec.family = L"Segoe UI";
        }
        else if (wcscmp(spec.family, L"Cascadia Mono") == 0 && !hasCascadia)
        {
            spec.family = L"Consolas";
        }

        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwriteFactory_->CreateTextFormat(spec.family, nullptr, spec.weight,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                    spec.size, L"en-us", format.put())))
        {
            return false;
        }

        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // Real ellipsis truncation, so long paths degrade instead of overflowing.
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format.get(), sign.put())))
        {
            DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            format->SetTrimming(&trimming, sign.get());
        }

        formats_[i] = format;
    }

    return true;
}

bool Graphics::ensureDeviceResources(HWND hwnd, D2D1_SIZE_U pixelSize, UINT dpi)
{
    if (renderTarget_)
    {
        return true;
    }
    if (!d2dFactory_ || !hwnd)
    {
        return false;
    }

    pixelSize.width = std::max<UINT32>(pixelSize.width, 1);
    pixelSize.height = std::max<UINT32>(pixelSize.height, 1);

    const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    // PRESENT_OPTIONS_IMMEDIATELY skips waiting for vblank, which is what keeps
    // keystroke-to-pixel latency down on a palette that redraws per character.
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd, pixelSize, D2D1_PRESENT_OPTIONS_IMMEDIATELY);

    if (FAILED(d2dFactory_->CreateHwndRenderTarget(&rtProps, &hwndProps, renderTarget_.put())))
    {
        renderTarget_.reset();
        return false;
    }

    dpi_ = dpi == 0 ? 96 : dpi;
    renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    renderTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    renderTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), scratch_.put());
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), highlight_.put());

    // Cached layouts can hold the old highlight brush as a drawing effect, so they
    // must not outlive the device that created it.
    clearLayoutCache();
    return true;
}

void Graphics::discardDeviceResources()
{
    clearLayoutCache();
    scratch_.reset();
    highlight_.reset();
    renderTarget_.reset();
}

void Graphics::destroy()
{
    discardDeviceResources();
    for (auto& format : formats_)
    {
        format.reset();
    }
    wicFactory_.reset();
    dwriteFactory_.reset();
    d2dFactory_.reset();
}

void Graphics::resize(D2D1_SIZE_U pixelSize)
{
    if (!renderTarget_)
    {
        return;
    }
    pixelSize.width = std::max<UINT32>(pixelSize.width, 1);
    pixelSize.height = std::max<UINT32>(pixelSize.height, 1);
    renderTarget_->Resize(pixelSize);
}

void Graphics::setDpi(UINT dpi)
{
    dpi_ = dpi == 0 ? 96 : dpi;
    if (renderTarget_)
    {
        renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
}

ID2D1SolidColorBrush* Graphics::solid(const D2D1_COLOR_F& color)
{
    if (!scratch_)
    {
        return nullptr;
    }
    scratch_->SetColor(color);
    scratch_->SetOpacity(color.a);
    return scratch_.get();
}

void Graphics::setHighlightColor(const D2D1_COLOR_F& color)
{
    if (highlight_)
    {
        highlight_->SetColor(color);
    }
}

IDWriteTextFormat* Graphics::format(FontRole role) const
{
    const size_t index = static_cast<size_t>(role);
    if (index >= static_cast<size_t>(FontRole::Count))
    {
        return nullptr;
    }
    return formats_[index].get();
}

IDWriteTextLayout* Graphics::layout(const std::wstring& text, FontRole role, float maxWidth, float maxHeight, uint64_t variant, bool* outCreated)
{
    if (outCreated)
    {
        *outCreated = false;
    }

    IDWriteTextFormat* fmt = format(role);
    if (!fmt || !dwriteFactory_)
    {
        return nullptr;
    }

    maxWidth = std::max(maxWidth, 0.0f);
    maxHeight = std::max(maxHeight, 0.0f);

    uint64_t key = std::hash<std::wstring>{}(text);
    key = hashCombine(key, static_cast<uint64_t>(role));
    key = hashCombine(key, static_cast<uint64_t>(maxWidth * 4.0f));
    key = hashCombine(key, static_cast<uint64_t>(maxHeight * 4.0f));
    key = hashCombine(key, variant);

    if (const auto it = layoutCache_.find(key); it != layoutCache_.end())
    {
        return it->second.get();
    }

    if (layoutCache_.size() >= kMaxCachedLayouts)
    {
        clearLayoutCache();
    }

    ComPtr<IDWriteTextLayout> created;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                fmt, maxWidth, maxHeight, created.put())))
    {
        return nullptr;
    }

    if (outCreated)
    {
        *outCreated = true;
    }

    IDWriteTextLayout* raw = created.get();
    layoutCache_.emplace(key, std::move(created));
    return raw;
}

void Graphics::clearLayoutCache()
{
    layoutCache_.clear();
}
