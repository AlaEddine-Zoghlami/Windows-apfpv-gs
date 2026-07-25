// osd_render_test.cpp — offline verifier for the ground-side MSP_DISPLAYPORT OSD.
//
// Feeds a raw capture of forwarded MSP bytes through the same Canvas/FontAtlas code the
// player uses, rasterises the resulting canvas with font_btfl.png, and writes a PNG. This
// proves the DisplayPort parsing, glyph indexing and palette/transparency handling are right
// against REAL bytes before any of it is wired into the live render loop.
//
// Build (MSYS2 MinGW64):
//   g++ -std=c++17 -O2 osd_render_test.cpp -o osd_render_test.exe \
//     $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale)
// Run:
//   ./osd_render_test.exe <msp_capture.bin> <font_btfl.png> <out.png>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include "msp_osd.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

// Decode any still image (here: the 4-bit palette font PNG) to packed RGBA8 via libav,
// so tRNS palette transparency is honoured by the decoder rather than reimplemented here.
static bool decodeImageRGBA(const char* path, std::vector<uint8_t>& out, int& W, int& H) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) { fprintf(stderr, "open %s failed\n", path); return false; }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }
    int vs = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    if (vs < 0) { avformat_close_input(&fmt); return false; }
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar);
    if (avcodec_open2(ctx, dec, nullptr) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return false; }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    bool got = false;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(ctx, pkt) == 0)
            if (avcodec_receive_frame(ctx, fr) == 0) got = true;
        av_packet_unref(pkt);
    }
    if (!got) { avcodec_send_packet(ctx, nullptr); got = (avcodec_receive_frame(ctx, fr) == 0); }
    bool ok = false;
    if (got) {
        W = fr->width; H = fr->height;
        out.assign((size_t)W * H * 4, 0);
        uint8_t* dst[4] = { out.data(), nullptr, nullptr, nullptr };
        int dls[4] = { W * 4, 0, 0, 0 };
        SwsContext* sws = sws_getContext(W, H, (AVPixelFormat)fr->format, W, H,
                                         AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
        if (sws) { sws_scale(sws, fr->data, fr->linesize, 0, H, dst, dls); sws_freeContext(sws); ok = true; }
    }
    av_frame_free(&fr); av_packet_free(&pkt);
    avcodec_free_context(&ctx); avformat_close_input(&fmt);
    return ok;
}

static bool encodePNG(const char* path, const uint8_t* rgba, int W, int H) {
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!enc) return false;
    AVCodecContext* c = avcodec_alloc_context3(enc);
    c->width = W; c->height = H; c->pix_fmt = AV_PIX_FMT_RGBA; c->time_base = { 1, 1 };
    if (avcodec_open2(c, enc, nullptr) < 0) { avcodec_free_context(&c); return false; }
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_RGBA; f->width = W; f->height = H;
    av_frame_get_buffer(f, 32);
    for (int y = 0; y < H; y++) memcpy(f->data[0] + y * f->linesize[0], rgba + (size_t)y * W * 4, (size_t)W * 4);
    AVPacket* p = av_packet_alloc();
    bool ok = false;
    if (avcodec_send_frame(c, f) == 0 && avcodec_receive_packet(c, p) == 0) {
        FILE* fp = fopen(path, "wb");
        if (fp) { fwrite(p->data, 1, p->size, fp); fclose(fp); ok = true; }
    }
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&c);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <msp_capture.bin> <font.png> <out.png>\n", argv[0]); return 2; }

    // --- font atlas ---
    std::vector<uint8_t> fontRGBA; int fw = 0, fh = 0;
    if (!decodeImageRGBA(argv[2], fontRGBA, fw, fh)) { fprintf(stderr, "font decode failed\n"); return 1; }
    auto atlas = mspospd::makeAtlas(fontRGBA.data(), fw, fh);
    printf("font: %dx%d -> glyph %dx%d, %d pages, %d rows\n", fw, fh, atlas.glyphW, atlas.glyphH, atlas.pages, atlas.rows);

    // --- replay the captured MSP stream through the real parser ---
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "open capture failed\n"); return 1; }
    std::vector<uint8_t> cap; { uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) cap.insert(cap.end(), b, b + n); }
    fclose(f);

    mspospd::Canvas canvas;
    int frames = 0, dpFrames = 0, writes = 0;
    size_t i = 0;
    while (i + 5 <= cap.size()) {
        if (!(cap[i] == '$' && cap[i + 1] == 'M' && cap[i + 2] == '>')) { i++; continue; }
        int plen = cap[i + 3], cmd = cap[i + 4]; size_t ps = i + 5;
        if (ps + plen + 1 > cap.size()) break;
        int crc = plen ^ cmd;
        for (int k = 0; k < plen; k++) crc ^= cap[ps + k];
        if ((crc & 0xff) != cap[ps + plen]) { i++; continue; }
        frames++;
        if (cmd == 182) {
            dpFrames++;
            if (plen >= 1 && cap[ps] == mspospd::DP_WRITE) writes++;
            mspospd::feedDisplayPort(canvas, &cap[ps], plen);
        }
        i = ps + plen + 1;
    }
    printf("MSP frames=%d  DISPLAYPORT=%d  writes=%d  canvas generation=%u\n",
           frames, dpFrames, writes, canvas.generation());
    if (canvas.generation() == 0) {
        fprintf(stderr, "NO DP_DRAW seen -- canvas never committed; nothing to render\n");
        return 1;
    }

    // --- rasterise the canvas ---
    int cols, rows; mspospd::canvasSizeFor(atlas.glyphW, cols, rows);
    int W = cols * atlas.glyphW, H = rows * atlas.glyphH;
    printf("canvas %dx%d cells -> %dx%d px\n", cols, rows, W, H);
    std::vector<uint8_t> img((size_t)W * H * 4, 0);       // transparent background

    mspospd::Cell cells[mspospd::MAX_ROWS][mspospd::MAX_COLS];
    canvas.snapshot(cells);
    int drawn = 0;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
        mspospd::Cell cell = cells[r][c];
        if (cell.glyph == 0) continue;                    // 0 = empty cell
        int sx, sy, sw, sh; atlas.srcRect(cell.glyph, cell.page, sx, sy, sw, sh);
        if (sy + sh > fh) continue;
        drawn++;
        for (int y = 0; y < sh; y++) {
            const uint8_t* srow = fontRGBA.data() + ((size_t)(sy + y) * fw + sx) * 4;
            uint8_t* drow = img.data() + ((size_t)(r * atlas.glyphH + y) * W + c * atlas.glyphW) * 4;
            for (int x = 0; x < sw; x++) {
                uint8_t a = srow[x * 4 + 3];
                if (!a) continue;                          // tRNS index 0 -> skip
                drow[x * 4 + 0] = srow[x * 4 + 0];
                drow[x * 4 + 1] = srow[x * 4 + 1];
                drow[x * 4 + 2] = srow[x * 4 + 2];
                drow[x * 4 + 3] = a;
            }
        }
    }
    printf("glyph cells drawn: %d\n", drawn);
    if (!encodePNG(argv[3], img.data(), W, H)) { fprintf(stderr, "png encode failed\n"); return 1; }
    printf("wrote %s\n", argv[3]);
    return 0;
}
