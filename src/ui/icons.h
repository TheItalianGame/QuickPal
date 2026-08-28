#pragma once

#include "../core/types.h"
#include "comptr.h"
#include "graphics.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Path-based glyphs are built once from the (device-independent) D2D factory and
// reused for the life of the process.
struct GlyphCache
{
    ComPtr<ID2D1PathGeometry> document;
    ComPtr<ID2D1PathGeometry> refresh;
    ComPtr<ID2D1PathGeometry> power;
    ComPtr<ID2D1PathGeometry> sparkle;
    bool built = false;
};

void buildGlyphCache(GlyphCache& cache, ID2D1Factory* factory);

// Draws a scalable vector mark for the command kind, centered in `box`.
void drawKindGlyph(ID2D1RenderTarget* rt, Graphics& gfx, GlyphCache& cache, CommandKind kind, D2D1_RECT_F box, const D2D1_COLOR_F& color);
void drawSearchGlyph(ID2D1RenderTarget* rt, Graphics& gfx, D2D1_RECT_F box, const D2D1_COLOR_F& color);
void drawChevron(ID2D1RenderTarget* rt, Graphics& gfx, D2D1_RECT_F box, const D2D1_COLOR_F& color, bool pointRight);

// Real shell icons, resolved on a worker thread. `acquire` never blocks: it returns
// null on the first call and posts kIconReadyMessage once the art is available.
class ShellIconCache
{
public:
    void start(HWND notifyWindow);
    void stop();

    ID2D1Bitmap* acquire(const Command& command, Graphics& gfx);
    void integrate(Graphics& gfx);
    void dropDeviceBitmaps();

private:
    struct Pixels
    {
        UINT width = 0;
        UINT height = 0;
        std::vector<uint8_t> bgra;
    };

    struct Request
    {
        std::wstring key;
        std::wstring path;
        HWND window = nullptr;
    };

    struct Completed
    {
        std::wstring key;
        Pixels pixels;
        bool ok = false;
    };

    enum class State
    {
        Queued,
        Ready,
        Failed,
    };

    struct Entry
    {
        State state = State::Queued;
        ComPtr<ID2D1Bitmap> bitmap;
    };

    void workerLoop();

    // UI thread only.
    std::unordered_map<std::wstring, Entry> entries_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::vector<Completed> completed_;
    std::thread worker_;
    std::atomic_bool running_{ false };
    HWND notifyWindow_ = nullptr;
};
