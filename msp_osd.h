// ============================================================================
//  msp_osd.h — MSP_DISPLAYPORT canvas + Betaflight font-atlas renderer.
//
//  PURPOSE. The VTX burns msposd's OSD into the H.265 pixels before transmission,
//  so the ground can never obtain a clean image. This reproduces that OSD on the
//  GROUND instead, from data, so the video itself can stay clean -- which is what
//  makes "record with and without the overlay" possible at all.
//
//  WHY IT LOOKS IDENTICAL. Betaflight does not send telemetry fields for the OSD;
//  it sends the finished canvas as MSP_DISPLAYPORT (cmd 182) "write these glyph
//  indices at this row/col" commands, and msposd merely rasterises them with
//  font_btfl.png. So drawing the same glyph grid with the same font atlas is
//  pixel-identical by construction -- not an approximation of the OSD.
//
//  GEOMETRY (measured, not assumed -- msposd logs "Font file res 144:13824
//  pages:4" and "Glyph size: 36:54 on a 53:20 matrix"):
//    font_btfl.png    144 x 13824, 4-bit palette, 16 colours, tRNS: index 0 alpha=0
//    -> 4 pages across (36 px each), 256 glyph rows down (54 px each)
//    -> glyph g on page p is the source rect (p*36, g*54, 36, 54)
//    -> canvas 53 x 20 cells = 1908 x 1080, i.e. pixel-exact for 1080p
//  The _hd atlas (96 x 9216) is the same layout at 24 x 36 per glyph.
// ============================================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>

namespace mspospd {

// Betaflight's MSP_DISPLAYPORT sub-commands (observed live: 0,2,3,4,5 all occur).
enum : uint8_t {
    DP_HEARTBEAT   = 0,   // keep the OSD alive; no canvas effect
    DP_RELEASE     = 1,
    DP_CLEAR       = 2,   // blank the canvas
    DP_WRITE       = 3,   // row, col, attr, glyphs...
    DP_DRAW        = 4,   // commit: the canvas is now complete for this frame
    DP_OPTIONS     = 5,   // resolution/options hint
};

constexpr int MAX_COLS = 64;   // real canvas is 53 (SD) / 50 (HD); headroom is cheap
constexpr int MAX_ROWS = 24;   // real canvas is 20 (SD) / 18 (HD)

// One glyph cell. page comes from the WRITE attr byte: Betaflight puts the font
// page in its low bits, which is how it addresses >256 glyphs.
struct Cell { uint8_t glyph; uint8_t page; };

// Double-buffered so a torn mid-update canvas is never rendered: WRITEs accumulate
// into `back`, and DP_DRAW promotes it to `front` under the lock. `gen` lets the
// render thread cheaply skip redrawing an unchanged canvas.
class Canvas {
public:
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        std::memset(back_, 0, sizeof(back_));
    }

    void write(int row, int col, uint8_t attr, const uint8_t* g, int n) {
        if (row < 0 || row >= MAX_ROWS) return;
        std::lock_guard<std::mutex> lk(m_);
        // Betaflight's page bits live in the attr byte. Mask to the 2 bits our
        // 4-page atlas can address so a stray high bit can't index out of range.
        uint8_t page = (uint8_t)(attr & 0x03);
        for (int i = 0; i < n; i++) {
            int c = col + i;
            if (c < 0 || c >= MAX_COLS) break;
            back_[row][c] = Cell{ g[i], page };
        }
    }

    // DP_DRAW: publish the accumulated canvas.
    void commit() {
        std::lock_guard<std::mutex> lk(m_);
        std::memcpy(front_, back_, sizeof(back_));
        gen_.fetch_add(1, std::memory_order_release);
    }

    uint32_t generation() const { return gen_.load(std::memory_order_acquire); }

    // Copy the published canvas out for rendering (cheap: ~1.5 KB).
    void snapshot(Cell out[MAX_ROWS][MAX_COLS]) const {
        std::lock_guard<std::mutex> lk(m_);
        std::memcpy(out, front_, sizeof(front_));
    }

private:
    mutable std::mutex m_;
    Cell back_[MAX_ROWS][MAX_COLS]{};
    Cell front_[MAX_ROWS][MAX_COLS]{};
    std::atomic<uint32_t> gen_{ 0 };
};

// Feed one MSP_DISPLAYPORT payload (the bytes AFTER the cmd byte, length `len`).
// Returns true if this payload completed a frame (DP_DRAW), i.e. time to redraw.
inline bool feedDisplayPort(Canvas& cv, const uint8_t* pl, int len) {
    if (len < 1) return false;
    switch (pl[0]) {
        case DP_CLEAR:
            cv.clear();
            return false;
        case DP_WRITE:
            // sub, row, col, attr, then glyph indices. Seen live e.g.
            // 03 0f 02 40 96 30 2e 30 32 06 -> row 15, col 2, attr 0x40,
            // glyphs {0x96,'0','.','0','2',0x06} = the battery-voltage field.
            if (len >= 4) cv.write(pl[1], pl[2], pl[3], pl + 4, len - 4);
            return false;
        case DP_DRAW:
            cv.commit();
            return true;
        default:
            return false;   // HEARTBEAT / RELEASE / OPTIONS: nothing to draw
    }
}

// ---------------------------------------------------------------------------
// Font atlas: 4 pages across, 256 glyph rows down. Kept as plain RGBA8 so the
// caller can upload it to whatever it renders with (SDL texture, GL texture,
// Android Bitmap) without this header depending on a graphics API.
// ---------------------------------------------------------------------------
struct FontAtlas {
    int glyphW = 0, glyphH = 0;   // 36x54 (SD) or 24x36 (HD)
    int pages = 0;                // 4
    int rows = 0;                  // 256
    int w = 0, h = 0;              // full atlas pixel size
    const uint8_t* rgba = nullptr; // w*h*4, not owned

    bool valid() const { return rgba && glyphW > 0 && glyphH > 0; }

    // Source rect of glyph `g` on `page` within the atlas.
    void srcRect(uint8_t g, uint8_t page, int& x, int& y, int& sw, int& sh) const {
        int p = (page < pages) ? page : 0;
        x = p * glyphW;
        y = (int)g * glyphH;
        sw = glyphW; sh = glyphH;
    }
};

// Derive the layout from the decoded atlas dimensions. Both shipped fonts are
// 4 pages x 256 rows, so glyph size falls out of the image size -- no per-font
// table to keep in sync.
inline FontAtlas makeAtlas(const uint8_t* rgba, int w, int h) {
    FontAtlas a;
    a.rgba = rgba; a.w = w; a.h = h;
    a.pages = 4; a.rows = 256;
    a.glyphW = w / a.pages;
    a.glyphH = h / a.rows;
    return a;
}

// Canvas cell count for a given video size, matching msposd's own choice:
// 53x20 for 1080p-class (36x54 glyphs), 50x18 for the HD atlas.
inline void canvasSizeFor(int atlasGlyphW, int& cols, int& rows) {
    if (atlasGlyphW >= 32) { cols = 53; rows = 20; }   // font_btfl.png
    else                   { cols = 50; rows = 18; }   // font_btfl_hd.png
}

} // namespace mspospd
