#include "icons.h"

#include <d2d1helper.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>

#include <algorithm>

namespace
{
constexpr float kGlyphUnits = 24.0f;
constexpr float kGlyphStroke = 1.7f;
constexpr size_t kMaxIconEntries = 900;

// Builds a closed figure from a point list in the 24-unit glyph space.
ComPtr<ID2D1PathGeometry> makePolygon(ID2D1Factory* factory, const D2D1_POINT_2F* points, size_t count, bool closed, D2D1_FIGURE_BEGIN begin)
{
    ComPtr<ID2D1PathGeometry> geometry;
    if (!factory || count < 2)
    {
        return geometry;
    }
    if (FAILED(factory->CreatePathGeometry(geometry.put())))
    {
        return geometry;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put())))
    {
        geometry.reset();
        return geometry;
    }
    sink->BeginFigure(points[0], begin);
    for (size_t i = 1; i < count; ++i)
    {
        sink->AddLine(points[i]);
    }
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geometry;
}

ComPtr<ID2D1PathGeometry> makeDocument(ID2D1Factory* factory)
{
    const D2D1_POINT_2F points[] = {
        D2D1::Point2F(5.5f, 2.5f),
        D2D1::Point2F(13.5f, 2.5f),
        D2D1::Point2F(18.5f, 7.5f),
        D2D1::Point2F(18.5f, 21.5f),
        D2D1::Point2F(5.5f, 21.5f),
    };
    return makePolygon(factory, points, std::size(points), true, D2D1_FIGURE_BEGIN_HOLLOW);
}

// An open ring with a corner arrowhead, i.e. the usual "refresh" mark.
ComPtr<ID2D1PathGeometry> makeRefresh(ID2D1Factory* factory)
{
    ComPtr<ID2D1PathGeometry> geometry;
    if (!factory || FAILED(factory->CreatePathGeometry(geometry.put())))
    {
        return geometry;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put())))
    {
        geometry.reset();
        return geometry;
    }

    const D2D1_POINT_2F start = D2D1::Point2F(17.657f, 6.343f);
    const D2D1_POINT_2F end = D2D1::Point2F(6.343f, 6.343f);

    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(8.0f, 8.0f), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);

    sink->BeginFigure(D2D1::Point2F(17.657f, 1.9f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(start);
    sink->AddLine(D2D1::Point2F(13.2f, 6.343f));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);

    sink->Close();
    return geometry;
}

ComPtr<ID2D1PathGeometry> makePower(ID2D1Factory* factory)
{
    ComPtr<ID2D1PathGeometry> geometry;
    if (!factory || FAILED(factory->CreatePathGeometry(geometry.put())))
    {
        return geometry;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put())))
    {
        geometry.reset();
        return geometry;
    }

    sink->BeginFigure(D2D1::Point2F(15.75f, 6.005f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(8.25f, 6.005f), D2D1::SizeF(7.5f, 7.5f), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);

    sink->BeginFigure(D2D1::Point2F(12.0f, 3.0f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(D2D1::Point2F(12.0f, 11.5f));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);

    sink->Close();
    return geometry;
}

ComPtr<ID2D1PathGeometry> makeSparkle(ID2D1Factory* factory)
{
    ComPtr<ID2D1PathGeometry> geometry;
    if (!factory || FAILED(factory->CreatePathGeometry(geometry.put())))
    {
        return geometry;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put())))
    {
        geometry.reset();
        return geometry;
    }

    sink->BeginFigure(D2D1::Point2F(12.0f, 2.5f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(12.9f, 11.1f), D2D1::Point2F(21.5f, 12.0f)));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(12.9f, 12.9f), D2D1::Point2F(12.0f, 21.5f)));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(11.1f, 12.9f), D2D1::Point2F(2.5f, 12.0f)));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(11.1f, 11.1f), D2D1::Point2F(12.0f, 2.5f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    sink->Close();
    return geometry;
}

// Maps the 24-unit glyph space onto `box` and returns the transform to restore.
D2D1_MATRIX_3X2_F pushGlyphSpace(ID2D1RenderTarget* rt, const D2D1_RECT_F& box)
{
    D2D1_MATRIX_3X2_F previous;
    rt->GetTransform(&previous);

    const float width = box.right - box.left;
    const float height = box.bottom - box.top;
    const float side = std::min(width, height);
    const float scale = side / kGlyphUnits;
    const float ox = box.left + (width - side) * 0.5f;
    const float oy = box.top + (height - side) * 0.5f;

    rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(ox, oy) * previous);
    return previous;
}

void strokeGeometry(ID2D1RenderTarget* rt, ID2D1PathGeometry* geometry, ID2D1Brush* brush, float width)
{
    if (geometry)
    {
        rt->DrawGeometry(geometry, brush, width);
    }
}
}

void buildGlyphCache(GlyphCache& cache, ID2D1Factory* factory)
{
    if (cache.built || !factory)
    {
        return;
    }
    cache.document = makeDocument(factory);
    cache.refresh = makeRefresh(factory);
    cache.power = makePower(factory);
    cache.sparkle = makeSparkle(factory);
    cache.built = true;
}

void drawKindGlyph(ID2D1RenderTarget* rt, Graphics& gfx, GlyphCache& cache, CommandKind kind, D2D1_RECT_F box, const D2D1_COLOR_F& color)
{
    ID2D1SolidColorBrush* brush = gfx.solid(color);
    if (!rt || !brush)
    {
        return;
    }

    const D2D1_MATRIX_3X2_F previous = pushGlyphSpace(rt, box);
    const float s = kGlyphStroke;

    switch (kind)
    {
    case CommandKind::App:
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3.0f, 3.0f, 10.5f, 10.5f), 2.0f, 2.0f), brush);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(13.5f, 3.0f, 21.0f, 10.5f), 2.0f, 2.0f), brush);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(3.0f, 13.5f, 10.5f, 21.0f), 2.0f, 2.0f), brush);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(13.5f, 13.5f, 21.0f, 21.0f), 2.0f, 2.0f), brush);
        break;

    case CommandKind::PathTool:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.5f, 4.0f, 21.5f, 20.0f), 3.0f, 3.0f), brush, s);
        rt->DrawLine(D2D1::Point2F(6.5f, 9.0f), D2D1::Point2F(10.0f, 12.0f), brush, s);
        rt->DrawLine(D2D1::Point2F(10.0f, 12.0f), D2D1::Point2F(6.5f, 15.0f), brush, s);
        rt->DrawLine(D2D1::Point2F(12.5f, 15.5f), D2D1::Point2F(17.0f, 15.5f), brush, s);
        break;

    case CommandKind::File:
    case CommandKind::Snippet:
        strokeGeometry(rt, cache.document.get(), brush, s);
        rt->DrawLine(D2D1::Point2F(13.5f, 2.5f), D2D1::Point2F(13.5f, 7.5f), brush, s);
        rt->DrawLine(D2D1::Point2F(13.5f, 7.5f), D2D1::Point2F(18.5f, 7.5f), brush, s);
        break;

    case CommandKind::Folder:
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.5f, 5.0f, 11.0f, 9.0f), 1.5f, 1.5f), brush);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.5f, 7.5f, 21.5f, 19.5f), 2.5f, 2.5f), brush);
        break;

    case CommandKind::Web:
    case CommandKind::ChromeTab:
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12.0f, 12.0f), 9.0f, 9.0f), brush, s);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(12.0f, 12.0f), 4.2f, 9.0f), brush, s);
        rt->DrawLine(D2D1::Point2F(3.2f, 12.0f), D2D1::Point2F(20.8f, 12.0f), brush, s);
        break;

    case CommandKind::Shell:
        rt->DrawLine(D2D1::Point2F(6.0f, 7.5f), D2D1::Point2F(11.5f, 12.0f), brush, s);
        rt->DrawLine(D2D1::Point2F(11.5f, 12.0f), D2D1::Point2F(6.0f, 16.5f), brush, s);
        rt->DrawLine(D2D1::Point2F(13.0f, 16.5f), D2D1::Point2F(18.5f, 16.5f), brush, s);
        break;

    case CommandKind::Calc:
    case CommandKind::ValueTool:
        rt->DrawLine(D2D1::Point2F(6.5f, 9.5f), D2D1::Point2F(17.5f, 9.5f), brush, s);
        rt->DrawLine(D2D1::Point2F(6.5f, 14.5f), D2D1::Point2F(17.5f, 14.5f), brush, s);
        break;

    case CommandKind::OpenCalculator:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(5.0f, 2.5f, 19.0f, 21.5f), 3.0f, 3.0f), brush, s);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(7.5f, 5.5f, 16.5f, 9.0f), 1.0f, 1.0f), brush);
        for (float y : { 12.5f, 16.5f })
        {
            for (float x : { 8.6f, 12.0f, 15.4f })
            {
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 1.05f, 1.05f), brush);
            }
        }
        break;

    case CommandKind::Window:
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2.5f, 4.0f, 21.5f, 20.0f), 2.5f, 2.5f), brush, s);
        rt->DrawLine(D2D1::Point2F(2.5f, 8.5f), D2D1::Point2F(21.5f, 8.5f), brush, s);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(5.6f, 6.25f), 0.75f, 0.75f), brush);
        break;

    case CommandKind::Setting:
    case CommandKind::OpenSettings:
        rt->DrawLine(D2D1::Point2F(3.5f, 8.0f), D2D1::Point2F(6.6f, 8.0f), brush, s);
        rt->DrawLine(D2D1::Point2F(11.4f, 8.0f), D2D1::Point2F(20.5f, 8.0f), brush, s);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(9.0f, 8.0f), 2.3f, 2.3f), brush, s);
        rt->DrawLine(D2D1::Point2F(3.5f, 16.0f), D2D1::Point2F(12.6f, 16.0f), brush, s);
        rt->DrawLine(D2D1::Point2F(17.4f, 16.0f), D2D1::Point2F(20.5f, 16.0f), brush, s);
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(15.0f, 16.0f), 2.3f, 2.3f), brush, s);
        break;

    case CommandKind::ReloadIndex:
        strokeGeometry(rt, cache.refresh.get(), brush, s);
        break;

    case CommandKind::ExitApp:
        strokeGeometry(rt, cache.power.get(), brush, s);
        break;

    case CommandKind::Builtin:
    default:
        if (cache.sparkle)
        {
            rt->FillGeometry(cache.sparkle.get(), brush);
        }
        break;
    }

    rt->SetTransform(previous);
}

void drawSearchGlyph(ID2D1RenderTarget* rt, Graphics& gfx, D2D1_RECT_F box, const D2D1_COLOR_F& color)
{
    ID2D1SolidColorBrush* brush = gfx.solid(color);
    if (!rt || !brush)
    {
        return;
    }
    const D2D1_MATRIX_3X2_F previous = pushGlyphSpace(rt, box);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(10.5f, 10.5f), 6.6f, 6.6f), brush, 1.9f);
    rt->DrawLine(D2D1::Point2F(15.4f, 15.4f), D2D1::Point2F(20.5f, 20.5f), brush, 1.9f);
    rt->SetTransform(previous);
}

void drawChevron(ID2D1RenderTarget* rt, Graphics& gfx, D2D1_RECT_F box, const D2D1_COLOR_F& color, bool pointRight)
{
    ID2D1SolidColorBrush* brush = gfx.solid(color);
    if (!rt || !brush)
    {
        return;
    }
    const D2D1_MATRIX_3X2_F previous = pushGlyphSpace(rt, box);
    const float tail = pointRight ? 9.5f : 14.5f;
    const float tip = pointRight ? 14.5f : 9.5f;
    rt->DrawLine(D2D1::Point2F(tail, 6.5f), D2D1::Point2F(tip, 12.0f), brush, 1.9f);
    rt->DrawLine(D2D1::Point2F(tip, 12.0f), D2D1::Point2F(tail, 17.5f), brush, 1.9f);
    rt->SetTransform(previous);
}

namespace
{
struct IconHandle
{
    HICON icon = nullptr;
    bool owned = false;   // class icons belong to the other process; never destroy those
};

IconHandle loadIconForPath(const std::wstring& path)
{
    IconHandle handle;
    if (path.empty())
    {
        return handle;
    }

    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX))
    {
        ComPtr<IImageList> list;
        if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, __uuidof(IImageList), list.putVoid())) && list)
        {
            HICON icon = nullptr;
            if (SUCCEEDED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) && icon)
            {
                handle.icon = icon;
                handle.owned = true;
                return handle;
            }
        }
    }

    SHFILEINFOW large{};
    if (SHGetFileInfoW(path.c_str(), 0, &large, sizeof(large), SHGFI_ICON | SHGFI_LARGEICON) && large.hIcon)
    {
        handle.icon = large.hIcon;
        handle.owned = true;
    }
    return handle;
}

IconHandle loadIconForWindow(HWND window)
{
    IconHandle handle;
    if (!window || !IsWindow(window))
    {
        return handle;
    }

    handle.icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
    if (!handle.icon)
    {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(window, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 120, &result))
        {
            handle.icon = reinterpret_cast<HICON>(result);
        }
    }
    if (!handle.icon)
    {
        handle.icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
    }
    return handle;
}
}

void ShellIconCache::start(HWND notifyWindow)
{
    if (running_.exchange(true))
    {
        return;
    }
    notifyWindow_ = notifyWindow;
    worker_ = std::thread([this] { workerLoop(); });
}

void ShellIconCache::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }
    cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void ShellIconCache::workerLoop()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(IWICImagingFactory), wic.putVoid());

    while (true)
    {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load())
            {
                break;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
        }

        Completed done;
        done.key = request.key;

        const IconHandle handle = request.window ? loadIconForWindow(request.window)
                                                 : loadIconForPath(request.path);

        if (handle.icon && wic)
        {
            ComPtr<IWICBitmap> source;
            if (SUCCEEDED(wic->CreateBitmapFromHICON(handle.icon, source.put())) && source)
            {
                ComPtr<IWICFormatConverter> converter;
                if (SUCCEEDED(wic->CreateFormatConverter(converter.put())) && converter &&
                    SUCCEEDED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA,
                                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                                    WICBitmapPaletteTypeMedianCut)))
                {
                    UINT width = 0;
                    UINT height = 0;
                    converter->GetSize(&width, &height);
                    if (width > 0 && height > 0 && width <= 512 && height <= 512)
                    {
                        done.pixels.width = width;
                        done.pixels.height = height;
                        done.pixels.bgra.resize(static_cast<size_t>(width) * height * 4);
                        if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4,
                                                            static_cast<UINT>(done.pixels.bgra.size()),
                                                            done.pixels.bgra.data())))
                        {
                            done.ok = true;
                        }
                    }
                }
            }
        }

        if (handle.icon && handle.owned)
        {
            DestroyIcon(handle.icon);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_.push_back(std::move(done));
        }
        if (notifyWindow_)
        {
            PostMessageW(notifyWindow_, kIconReadyMessage, 0, 0);
        }
    }

    wic.reset();
    CoUninitialize();
}

ID2D1Bitmap* ShellIconCache::acquire(const Command& command, Graphics& gfx)
{
    const IconSource source = iconSourceFor(command.kind);
    if (source == IconSource::Glyph || !gfx.ready())
    {
        return nullptr;
    }

    std::wstring key;
    Request request;
    if (source == IconSource::WindowHandle)
    {
        if (!command.hwnd)
        {
            return nullptr;
        }
        wchar_t buffer[32]{};
        wsprintfW(buffer, L"hwnd:%p", command.hwnd);
        key = buffer;
        request.window = command.hwnd;
    }
    else
    {
        if (command.arg.empty())
        {
            return nullptr;
        }
        key = L"path:" + command.arg;
        request.path = command.arg;
    }

    if (const auto it = entries_.find(key); it != entries_.end())
    {
        return it->second.state == State::Ready ? it->second.bitmap.get() : nullptr;
    }

    if (entries_.size() >= kMaxIconEntries)
    {
        entries_.clear();
    }

    entries_.emplace(key, Entry{});
    request.key = key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(request));
    }
    cv_.notify_one();
    return nullptr;
}

void ShellIconCache::integrate(Graphics& gfx)
{
    std::vector<Completed> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(completed_);
    }
    if (ready.empty() || !gfx.ready())
    {
        return;
    }

    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

    for (auto& item : ready)
    {
        auto it = entries_.find(item.key);
        if (it == entries_.end())
        {
            continue;
        }
        if (!item.ok)
        {
            it->second.state = State::Failed;
            continue;
        }

        ComPtr<ID2D1Bitmap> bitmap;
        const HRESULT hr = gfx.rt()->CreateBitmap(
            D2D1::SizeU(item.pixels.width, item.pixels.height),
            item.pixels.bgra.data(),
            item.pixels.width * 4,
            properties,
            bitmap.put());

        if (SUCCEEDED(hr) && bitmap)
        {
            it->second.bitmap = std::move(bitmap);
            it->second.state = State::Ready;
        }
        else
        {
            it->second.state = State::Failed;
        }
    }
}

void ShellIconCache::dropDeviceBitmaps()
{
    // Bitmaps belong to the lost device; clearing forces a cheap re-fetch.
    entries_.clear();
}
