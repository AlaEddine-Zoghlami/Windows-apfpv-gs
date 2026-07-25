// apfpv_player.cpp — custom minimal APFPV video viewer.
// FFmpeg (libav*) decode + SDL2 render, replacing ffplay so we can have real
// UI: an FPS/resolution overlay and a genuine clickable REC button (top-left)
// that muxes the live stream to an MP4 in the same directory as the exe.
// ffplay has no button/mouse support at all -- that's why this exists.
//
// Build (MSYS2 MinGW64 shell, so pkg-config resolves the ffmpeg/sdl2 flags):
//   g++ -std=c++17 -O2 apfpv_player.cpp -o apfpv_player.exe \
//     $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale sdl2) -lSDL2_ttf
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <SDL.h>
#include <SDL_ttf.h>
#ifdef _WIN32
// winsock2.h MUST precede windows.h: windows.h pulls in the ancient winsock.h and the two
// redefine each other (sockaddr_in, FD_SET, ...) if the order is reversed.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <linux/limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <vector>
#include <cmath>

#ifdef _WIN32
  #define APFPV_PATH_SEP       "\\"
  #define APFPV_HWACCEL_TYPE   AV_HWDEVICE_TYPE_D3D11VA
  #define APFPV_HWACCEL_PIXFMT AV_PIX_FMT_D3D11
  #define APFPV_HWACCEL_NAME   "D3D11VA"
#else
  #define APFPV_PATH_SEP       "/"
  // VAAPI is the Linux equivalent of D3D11VA -- Intel/AMD GPU-backed decode
  // via the same generic FFmpeg hwaccel API, no vendor-specific code needed
  // here. (Not Rockchip MPP: that's tied to specific ARM SoC silicon that
  // simply doesn't exist on an x86_64 desktop -- a different chip, not a
  // library path to swap.)
  #define APFPV_HWACCEL_TYPE   AV_HWDEVICE_TYPE_VAAPI
  #define APFPV_HWACCEL_PIXFMT AV_PIX_FMT_VAAPI
  #define APFPV_HWACCEL_NAME   "VAAPI"
#endif

// Sustained-corruption recovery: FFmpeg's own HEVC decoder logs "Could not
// find ref", "PPS id out of range", "Error constructing the frame RPS" etc.
// at AV_LOG_ERROR/WARNING whenever the reference-picture state we relaxed
// error_recognition for is actually broken. An occasional one is normal
// (survivable loss); a sustained run of them means the decoder's internal
// state is corrupted and won't self-correct until it sees a clean IDR --
// which may never arrive cleanly if loss keeps happening. Counting them here
// (av_log's own callback, called from FFmpeg's internals) lets the main loop
// decide when to stop waiting and force a resync itself.
static std::atomic<int> g_decodeErrorCount{ 0 };
static void ffmpegLogCallback(void* avcl, int level, const char* fmt, va_list args)
{
    if (level <= AV_LOG_WARNING) {
        // av_log_default_callback suppresses repeats of an EXACTLY IDENTICAL
        // message, printing "Last message repeated N times" instead of
        // calling back for every occurrence. Messages with a varying number
        // in them (POC, etc.) aren't identical so aren't suppressed, but a
        // constant message like "PPS id out of range: 0" is -- confirmed:
        // real logs showed exactly that "Last message repeated" pattern, and
        // this was silently undercounting that error type, which is why the
        // flush threshold below was never reached despite ongoing corruption.
        // Format the message ourselves and check for that pattern so a
        // suppressed run counts for its real total instead of being missed.
        va_list argsCopy;
        va_copy(argsCopy, args);
        char buf[256];
        vsnprintf(buf, sizeof(buf), fmt, argsCopy);
        va_end(argsCopy);
        int repeated = 0;
        if (sscanf(buf, " Last message repeated %d times", &repeated) == 1 && repeated > 0) {
            g_decodeErrorCount.fetch_add(repeated, std::memory_order_relaxed);
        } else {
            g_decodeErrorCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Still print it (same visibility as the default handler) so the existing
    // stderr-based diagnosis/log files keep working unchanged.
    av_log_default_callback(avcl, level, fmt, args);
}

// I/O-hang guard: av_read_frame() on a live UDP/RTP source blocks inside the
// OS's own recv() call with no way to return early once packets stop
// arriving -- confirmed directly by watchdog logs: step=READ_FRAME sat
// frozen for 6+ seconds with frames stuck at the same count, and separately
// the whole process went Not-Responding at t=300s and was dead by t=315s,
// while memory stayed flat the entire run (~150-185MB, no leak). That is the
// real "permanently bad video" mechanism: the last decoded frame (possibly
// mid-corruption) stays on screen forever because nothing ever calls
// av_read_frame() again to replace it. The "timeout" AVDictionary option set
// in openInput() below evidently does NOT reach the actual blocking read --
// SDP-based rtp/udp input doesn't reliably propagate that option down to the
// nested per-stream AVIOContext. AVIOInterruptCB is the one mechanism
// FFmpeg's own I/O core checks directly during every blocking protocol call
// (probe, open, and each read), so it's used here as the real fix.
static std::atomic<long long> g_lastIoActivityMs{ 0 };
static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void ioNotedProgress() { g_lastIoActivityMs.store(nowMs(), std::memory_order_relaxed); }
static int ioInterruptCallback(void*) {
    constexpr long long kIoTimeoutMs = 2500;
    return (nowMs() - g_lastIoActivityMs.load(std::memory_order_relaxed)) > kIoTimeoutMs ? 1 : 0;
}

// Opens (or reopens, after a stall/error) the SDP input. Self-contained --
// doesn't touch the decoder -- so a reconnect mid-stream is just "throw away
// the old AVFormatContext, get a new one"; the AVCodecContext, hwDeviceCtx
// and codec pointer all stay valid and untouched across it.
//
// isReconnect=true (every call after the first) skips the thorough stream
// probe: confirmed by trace data that a real network gap big enough to hit
// the I/O timeout can cascade into a RECONNECT LOOP that makes the outage
// much longer than the underlying gap actually was --
//   av_read_frame blocks 2.5s -> timeout -> reconnect
//   "Could not find codec parameters ... unspecified size" (default probing
//     wants more live data than a still-recovering link is handing it)
//   "Reconnected." anyway, but av_read_frame blocks another 2.5s immediately
//   -> repeat, each cycle adding another ~2.5s+ on top of the original gap
// We already know the codec/dimensions from the first successful connect
// (createDecoder() in main() was built from it and is never touched by a
// reconnect) -- a reconnect only needs the video stream's INDEX, which the
// SDP's own "m=video" line establishes as soon as avformat_open_input
// creates the stream, before any packets are even read. So skip
// avformat_find_stream_info's data-hungry analysis on a reconnect and take
// the first video stream directly instead of waiting on it to fully parse.
static AVFormatContext* openInput(const std::string& sdpPath, int& videoStreamOut, bool isReconnect)
{
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "protocol_whitelist", "file,rtp,udp", 0);
    av_dict_set(&opts, "timeout", "2000000", 0);
    if (isReconnect) {
        av_dict_set(&opts, "probesize", "32768", 0);
        av_dict_set(&opts, "analyzeduration", "0", 0);
    }
    // Absorb bursty Wi-Fi delivery (A-MPDU aggregation hands the OS many RTP
    // packets in a tight cluster, not evenly spaced) instead of dropping them
    // outright when a receive buffer fills or the jitter window is too tight
    // -- both of these are consumed as demuxer-level options by the SDP/RTP
    // demuxer that owns the per-stream sockets it's about to open, not
    // protocol-level options threaded through to an already-open nested
    // AVIOContext (that's the "timeout" propagation failure noted above), so
    // they're expected to actually take effect here.
    av_dict_set(&opts, "reorder_queue_size", "500", 0);   // packets held for RTP reordering (default lower)
    av_dict_set(&opts, "buffer_size", "4194304", 0);      // 4MB UDP socket recv buffer (OS default is far smaller)

    AVFormatContext* fmt = avformat_alloc_context();
    fmt->interrupt_callback.callback = ioInterruptCallback;
    fmt->interrupt_callback.opaque = nullptr;
    // Set directly on the struct (read by rtpdec's own jitter buffer as
    // ic->max_delay) rather than only via the AVDictionary above -- this field
    // is guaranteed to reach the demuxer since it's the SAME AVFormatContext
    // instance, sidestepping any nested-context propagation question entirely.
    // Default ~0.5s was tight enough that real jitter regularly hit it
    // ("max delay reached. need to consume packet" in the logs) and force-fed
    // the decoder out-of-order/incomplete data instead of waiting the extra
    // moment for the missing packet to actually arrive.
    fmt->max_delay = 1000000;   // 1s, up from the ~500ms default
    ioNotedProgress();

    int rc = avformat_open_input(&fmt, sdpPath.c_str(), nullptr, &opts);
    // Anything still IN opts after open wasn't recognized/consumed by this
    // demuxer/protocol -- e.g. if reorder_queue_size/buffer_size above turn
    // out not to apply here the way max_delay's direct struct field does,
    // this is how to actually tell instead of silently assuming they worked.
    if (av_dict_count(opts) > 0) {
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)))
            fprintf(stderr, "openInput: option not consumed by demuxer: %s=%s\n", e->key, e->value);
        fflush(stderr);
    }
    av_dict_free(&opts);
    if (rc < 0) return nullptr;   // avformat_open_input frees fmt itself on failure

    ioNotedProgress();
    // The SDP demuxer's read_header (invoked by avformat_open_input above)
    // already creates one AVStream per "m=..." line and sets its
    // codec_type/codec_id straight from the SDP (e.g. "m=video ... RTP/AVP
    // 97" + "a=rtpmap:97 H265/90000") -- avformat_find_stream_info's job is
    // filling in the REST of codecpar (width/height/etc.) by reading and
    // analyzing actual packets, which is exactly the part a reconnect can't
    // afford to wait on. Skip it and scan fmt->streams directly.
    if (!isReconnect) {
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            avformat_close_input(&fmt);
            return nullptr;
        }
        ioNotedProgress();
    }

    int videoStream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { videoStream = (int)i; break; }
    }
    if (videoStream < 0) { avformat_close_input(&fmt); return nullptr; }

    videoStreamOut = videoStream;
    return fmt;
}

// Crash/hang diagnostics: a cheap atomic step marker updated at every stage
// of the main loop (no I/O on the hot path), plus a separate watchdog thread
// that logs it once a second. A hang shows up as the same step number
// repeating in watchdog_log.txt for many consecutive seconds -- telling us
// exactly which call the main thread is stuck in when it stops responding,
// instead of just knowing "it eventually dies".
enum Step {
    STEP_LOOP_TOP = 0, STEP_POLL_EVENTS, STEP_READ_FRAME, STEP_WRITE_REC_PKT,
    STEP_SEND_PACKET, STEP_RECEIVE_FRAME, STEP_HW_TRANSFER, STEP_DIM_CHECK,
    STEP_SWS_GETCONTEXT, STEP_SDL_CREATETEXTURE, STEP_SWS_SCALE, STEP_SDL_UPDATETEX,
    STEP_UNREF_FRAME, STEP_FPS_CALC, STEP_RENDER_CLEAR, STEP_RENDER_COPY,
    STEP_DRAW_TEXT, STEP_DRAW_BUTTON, STEP_RENDER_PRESENT
};
static const char* stepName(int s) {
    static const char* n[] = { "LOOP_TOP","POLL_EVENTS","READ_FRAME","WRITE_REC_PKT",
        "SEND_PACKET","RECEIVE_FRAME","HW_TRANSFER","DIM_CHECK",
        "SWS_GETCONTEXT","SDL_CREATETEXTURE","SWS_SCALE","SDL_UPDATETEX",
        "UNREF_FRAME","FPS_CALC","RENDER_CLEAR","RENDER_COPY",
        "DRAW_TEXT","DRAW_BUTTON","RENDER_PRESENT" };
    return (s >= 0 && s < (int)(sizeof(n)/sizeof(n[0]))) ? n[s] : "?";
}
static std::atomic<int> g_step{ STEP_LOOP_TOP };
static std::atomic<long long> g_frameCounter{ 0 };
#define STEP(x) g_step.store(x, std::memory_order_relaxed)

static void watchdogThread(const std::string& logPath)
{
    FILE* f = fopen(logPath.c_str(), "w");
    if (!f) return;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        int s = g_step.load(std::memory_order_relaxed);
        long long fc = g_frameCounter.load(std::memory_order_relaxed);
        fprintf(f, "t=%.0fs step=%s frames=%lld\n", t, stepName(s), fc);
        fflush(f);
    }
}

// The muxer write (av_interleaved_write_frame + disk I/O) runs on its own
// thread: doing it inline in the main loop stalled network reads badly
// enough to cause catastrophic packet loss (confirmed: zero "missed packets"
// warnings over a clean run with recording untouched, vs. hundreds-to-
// thousands per gap as soon as REC was exercised). The main loop only ever
// clones a packet and pushes it onto a queue -- fast, non-blocking.
//
// Each recording gets its own session object, owned jointly by RecordState
// (while active) and its writer thread (which keeps its own shared_ptr
// copy). stopRecording() hands its reference to the thread and forgets it
// immediately -- draining the queue and finalizing the file (av_write_trailer
// etc.) happen in the background, so stop is non-blocking too. A shared
// session (instead of reusing fields on RecordState directly) means a quick
// stop-then-start-again can't race the still-finishing previous session's
// queue/mutex/file handle.
//
// Writer threads are NOT detached: the OS kills detached threads outright the
// instant the process exits, which could cut off a still-recording (or still-
// draining) session mid-write and leave a corrupt .ts with no trailer. They're
// kept in g_writerThreads and explicitly joined during shutdown instead, so a
// recording active when the window closes still gets a chance to finalize
// properly. Interactive stop/start from the REC button stays non-blocking --
// only program exit waits.
// OpenIPC "Overshoot Fix" colortrans reversal, ported from PixelPilot's Android GL shader
// (GLFanoutRenderer.h): rev = clamp((rgb + offset) * gain, 0, 1), the identical affine transform
// on R, G and B. Because all three channels get the same transform, it factors exactly into YUV
// space -- Y' = gain*(Y+offset), U' = gain*(U-mid)+mid, V' = gain*(V-mid)+mid (mid = 128) -- so it
// can be applied straight to the decoded YUV420P planes via an 8-bit lookup table, no RGB
// round-trip needed. Defaults (gain 2.5, offset -0.15) match PixelPilot_rk/Android.
struct ColortransLut {
    uint8_t y[256], c[256];
    void build(float gain, float offset) {
        int off8 = (int)lrintf(offset * 255.0f);
        for (int i = 0; i < 256; i++) {
            int yv = (int)lrintf((i + off8) * gain);
            y[i] = (uint8_t)(yv < 0 ? 0 : (yv > 255 ? 255 : yv));
            int cv = (int)lrintf((i - 128) * gain) + 128;
            c[i] = (uint8_t)(cv < 0 ? 0 : (cv > 255 ? 255 : cv));
        }
    }
};
static void applyColortrans(AVFrame* f, const ColortransLut& lut) {
    for (int y = 0; y < f->height; y++) {
        uint8_t* row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < f->width; x++) row[x] = lut.y[row[x]];
    }
    int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    for (int p = 1; p <= 2; p++) {
        for (int y = 0; y < ch; y++) {
            uint8_t* row = f->data[p] + y * f->linesize[p];
            for (int x = 0; x < cw; x++) row[x] = lut.c[row[x]];
        }
    }
}

// Post-process pass, run only when a recording was stopped with Overshoot Fix active (see
// stopRecording): decode the just-finalized .ts, apply the same YUV LUT the live view uses,
// re-encode, and replace the original file. Keeps the LIVE recording path exactly as fast and
// lossless as before (still a straight packet remux, see startRecording/writeRecordingPacket) --
// only recordings actually made with the fix on pay this one-time background re-encode cost.
static void reencodeWithOvershootFix(const std::string& path, const ColortransLut& lut)
{
    std::string tmpPath = path + ".fixing.ts";
    AVFormatContext* inFmt = nullptr;
    AVFormatContext* outFmt = nullptr;
    AVCodecContext* decCtx = nullptr;
    AVCodecContext* encCtx = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* frameYuv = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    AVPacket* outPkt = av_packet_alloc();
    AVStream* outStream = nullptr;
    bool ok = false;

    auto cleanup = [&]() {
        if (outFmt) { if (outFmt->pb) avio_closep(&outFmt->pb); avformat_free_context(outFmt); }
        if (decCtx) avcodec_free_context(&decCtx);
        if (encCtx) avcodec_free_context(&encCtx);
        if (sws) sws_freeContext(sws);
        av_frame_free(&frame);
        av_frame_free(&frameYuv);
        av_packet_free(&pkt);
        av_packet_free(&outPkt);
        if (inFmt) avformat_close_input(&inFmt);
    };

    if (avformat_open_input(&inFmt, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(inFmt, nullptr) < 0) {
        fprintf(stderr, "Overshoot Fix re-encode: failed to open/probe %s\n", path.c_str());
        cleanup(); return;
    }
    int vs = -1;
    for (unsigned i = 0; i < inFmt->nb_streams; i++)
        if (inFmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    if (vs < 0) {
        fprintf(stderr, "Overshoot Fix re-encode: no video stream in %s\n", path.c_str());
        cleanup(); return;
    }

    const AVCodec* dec = avcodec_find_decoder(inFmt->streams[vs]->codecpar->codec_id);
    decCtx = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!decCtx || avcodec_parameters_to_context(decCtx, inFmt->streams[vs]->codecpar) < 0 ||
        avcodec_open2(decCtx, dec, nullptr) < 0) {
        fprintf(stderr, "Overshoot Fix re-encode: failed to open decoder for %s\n", path.c_str());
        cleanup(); return;
    }

    // Explicit software encoders by name, not avcodec_find_encoder(codec_id) -- that can hand
    // back a hardware encoder (nvenc/amf/mf) that then fails avcodec_open2 on a machine without
    // the matching GPU. libx264/libx265 are always available in this build (verified separately).
    bool isHevc = inFmt->streams[vs]->codecpar->codec_id == AV_CODEC_ID_HEVC;
    const AVCodec* enc = avcodec_find_encoder_by_name(isHevc ? "libx265" : "libx264");
    if (!enc) {
        fprintf(stderr, "Overshoot Fix re-encode: no %s encoder available\n", isHevc ? "libx265" : "libx264");
        cleanup(); return;
    }
    encCtx = avcodec_alloc_context3(enc);
    encCtx->width = decCtx->width;
    encCtx->height = decCtx->height;
    encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    encCtx->time_base = inFmt->streams[vs]->time_base;
    encCtx->framerate = inFmt->streams[vs]->avg_frame_rate;
    encCtx->gop_size = 60;
    encCtx->bit_rate = inFmt->streams[vs]->codecpar->bit_rate > 0
                         ? inFmt->streams[vs]->codecpar->bit_rate : 8000000;
    // Match the ORIGINAL stream's colorimetry tags exactly. Without this, libx264/libx265 falls
    // back to its own defaults (which don't necessarily match), so a player decodes numerically
    // correct pixels through the WRONG YUV->RGB matrix/range -- looks like "corrupted" colors even
    // though the LUT math itself is right.
    encCtx->color_range     = decCtx->color_range;
    encCtx->color_primaries = decCtx->color_primaries;
    encCtx->color_trc       = decCtx->color_trc;
    encCtx->colorspace      = decCtx->colorspace;
    av_opt_set(encCtx->priv_data, "preset", "veryfast", 0);

    if (avformat_alloc_output_context2(&outFmt, nullptr, "mpegts", tmpPath.c_str()) < 0 || !outFmt) {
        fprintf(stderr, "Overshoot Fix re-encode: failed to allocate output for %s\n", tmpPath.c_str());
        cleanup(); return;
    }
    if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(encCtx, enc, nullptr) < 0) {
        fprintf(stderr, "Overshoot Fix re-encode: failed to open %s encoder\n", isHevc ? "libx265" : "libx264");
        cleanup(); return;
    }
    outStream = avformat_new_stream(outFmt, nullptr);
    if (!outStream || avcodec_parameters_from_context(outStream->codecpar, encCtx) < 0) {
        fprintf(stderr, "Overshoot Fix re-encode: failed to set up output stream\n");
        cleanup(); return;
    }
    outStream->time_base = encCtx->time_base;
    if (avio_open(&outFmt->pb, tmpPath.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(outFmt, nullptr) < 0) {
        fprintf(stderr, "Overshoot Fix re-encode: failed to open/write header for %s\n", tmpPath.c_str());
        cleanup(); return;
    }

    int decodedCount = 0, encodedCount = 0;
    char errbuf[128];

    // Converts (if needed), LUTs, encodes and muxes one decoded frame. Shared by the main read
    // loop and both end-of-stream flushes below so the encode/write logic isn't triplicated.
    auto encodeFrame = [&](AVFrame* f) {
        decodedCount++;
        AVFrame* useFrame = f;
        if (f->format != AV_PIX_FMT_YUV420P) {
            if (!sws) sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format,
                                            f->width, f->height, AV_PIX_FMT_YUV420P,
                                            SWS_BILINEAR, nullptr, nullptr, nullptr);
            av_frame_unref(frameYuv);
            frameYuv->format = AV_PIX_FMT_YUV420P;
            frameYuv->width = f->width; frameYuv->height = f->height;
            av_frame_get_buffer(frameYuv, 32);
            sws_scale(sws, f->data, f->linesize, 0, f->height, frameYuv->data, frameYuv->linesize);
            frameYuv->pts = f->pts;
            useFrame = frameYuv;
        } else {
            // f comes straight from the decoder and may be a pooled/shared buffer (refcount > 1)
            // -- writing the LUT into it in-place without this is unsafe (undefined/corrupted
            // results), unlike frameYuv above which is always a fresh, exclusively-owned buffer.
            av_frame_make_writable(f);
        }
        applyColortrans(useFrame, lut);
        int sendErr = avcodec_send_frame(encCtx, useFrame);
        if (sendErr < 0) {
            av_strerror(sendErr, errbuf, sizeof(errbuf));
            fprintf(stderr, "Overshoot Fix re-encode: avcodec_send_frame failed: %s\n", errbuf);
            return;
        }
        int rc;
        while ((rc = avcodec_receive_packet(encCtx, outPkt)) == 0) {
            outPkt->stream_index = 0;
            av_packet_rescale_ts(outPkt, encCtx->time_base, outStream->time_base);
            av_interleaved_write_frame(outFmt, outPkt);
            av_packet_unref(outPkt);
            encodedCount++;
        }
        if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
            av_strerror(rc, errbuf, sizeof(errbuf));
            fprintf(stderr, "Overshoot Fix re-encode: avcodec_receive_packet failed: %s\n", errbuf);
        }
    };

    while (av_read_frame(inFmt, pkt) >= 0) {
        if (pkt->stream_index == vs) {
            int sendErr = avcodec_send_packet(decCtx, pkt);
            if (sendErr < 0) {
                av_strerror(sendErr, errbuf, sizeof(errbuf));
                fprintf(stderr, "Overshoot Fix re-encode: avcodec_send_packet failed: %s\n", errbuf);
            } else {
                while (avcodec_receive_frame(decCtx, frame) == 0) {
                    encodeFrame(frame);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(decCtx, nullptr);           // flush decoder
    while (avcodec_receive_frame(decCtx, frame) == 0) { encodeFrame(frame); av_frame_unref(frame); }
    avcodec_send_frame(encCtx, nullptr);             // flush encoder
    while (avcodec_receive_packet(encCtx, outPkt) == 0) {
        outPkt->stream_index = 0;
        av_packet_rescale_ts(outPkt, encCtx->time_base, outStream->time_base);
        av_interleaved_write_frame(outFmt, outPkt);
        av_packet_unref(outPkt);
        encodedCount++;
    }
    av_write_trailer(outFmt);
    fprintf(stderr, "Overshoot Fix re-encode: decoded %d frames, encoded %d packets\n", decodedCount, encodedCount);
    ok = (encodedCount > 0);
    cleanup();

    if (ok) {
        remove(path.c_str());
        if (rename(tmpPath.c_str(), path.c_str()) == 0)
            fprintf(stderr, "Overshoot Fix: re-encoded recording -> %s\n", path.c_str());
        else
            fprintf(stderr, "Overshoot Fix: re-encode finished but rename to %s failed\n", path.c_str());
    } else {
        remove(tmpPath.c_str());
    }
    fflush(stderr);
}

struct RecordingSession {
    AVFormatContext* outFmt = nullptr;
    std::string outPath;                // needed after the fact for the Overshoot Fix re-encode pass
    bool applyOvershootFix = false;     // snapshot of the toggle at stop time (see stopRecording)
    int64_t startPts = AV_NOPTS_VALUE;
    int64_t lastDts = AV_NOPTS_VALUE;   // muxers require strictly increasing dts; heavy packet
                                        // loss/reordering can hand us a non-monotonic sequence,
                                        // which some muxers (mpegts included) crash on rather
                                        // than reject -- checked before ever reaching the muxer.
    std::mutex qMutex;
    std::condition_variable qCv;
    std::queue<AVPacket*> pktQueue;
    std::atomic<bool> running{ true };
};

struct RecordState {
    bool recording = false;
    std::shared_ptr<RecordingSession> session;
};

static std::vector<std::thread> g_writerThreads;

static std::string exeDir()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string s(buf);
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string s(buf, n > 0 ? (size_t)n : 0);
#endif
    return s.substr(0, s.find_last_of("\\/"));
}

// Records by muxing the SAME compressed packets we're already receiving/
// decoding, in-process -- NOT by spawning a second ffmpeg pointed at the SDP,
// which would try to bind UDP 5600 a second time and fail (our own player is
// already listening there; that was the original bug: silent "bind failed"
// in the child, so nothing was ever written).
//
// Container is MPEG-TS, not MP4: the RTP HEVC depacketizer hands packets to
// the decoder in Annex-B form (start-code prefixed), which is exactly what
// MPEG-TS wants. MP4 needs length-prefixed NALUs + hvcC extradata instead --
// `ffmpeg -c copy` converts that automatically via an implicit bitstream
// filter; writing Annex-B packets into an MP4 muxer ourselves without that
// filter would produce an invalid file. TS sidesteps the whole conversion.
static void startRecording(RecordState& rs, const std::string& dir, AVFormatContext* inFmt, int videoStreamIdx)
{
    if (rs.recording) return;

    time_t now = time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char fname[64];
    snprintf(fname, sizeof(fname), "apfpv_rec_%04d%02d%02d_%02d%02d%02d.ts",
             tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
             tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    std::string outPath = dir + APFPV_PATH_SEP + fname;

    auto session = std::make_shared<RecordingSession>();

    if (avformat_alloc_output_context2(&session->outFmt, nullptr, "mpegts", outPath.c_str()) < 0 || !session->outFmt) {
        fprintf(stderr, "Failed to allocate output context for recording.\n");
        return;
    }
    AVStream* outStream = avformat_new_stream(session->outFmt, nullptr);
    if (!outStream || avcodec_parameters_copy(outStream->codecpar, inFmt->streams[videoStreamIdx]->codecpar) < 0) {
        fprintf(stderr, "Failed to set up recording output stream.\n");
        avformat_free_context(session->outFmt);
        return;
    }
    outStream->codecpar->codec_tag = 0;
    outStream->time_base = inFmt->streams[videoStreamIdx]->time_base;

    if (avio_open(&session->outFmt->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "Failed to open recording file %s\n", outPath.c_str());
        avformat_free_context(session->outFmt);
        return;
    }
    if (avformat_write_header(session->outFmt, nullptr) < 0) {
        fprintf(stderr, "Failed to write recording header.\n");
        avio_closep(&session->outFmt->pb);
        avformat_free_context(session->outFmt);
        return;
    }

    session->outPath = outPath;
    rs.session = session;
    rs.recording = true;

    // Keeps its own shared_ptr copy of the session, so it finishes draining +
    // finalizing the file on its own schedule, fully decoupled from
    // RecordState (which stopRecording() lets go of immediately). Pushed onto
    // g_writerThreads (not detached) so shutdown can still join it cleanly.
    g_writerThreads.emplace_back([session]() {
        while (true) {
            AVPacket* recPkt = nullptr;
            {
                std::unique_lock<std::mutex> lk(session->qMutex);
                session->qCv.wait(lk, [&] { return !session->pktQueue.empty() || !session->running; });
                if (!session->running && session->pktQueue.empty()) break;
                recPkt = session->pktQueue.front();
                session->pktQueue.pop();
            }
            if (av_interleaved_write_frame(session->outFmt, recPkt) < 0) {
                fprintf(stderr, "Warning: dropped a frame while writing the recording.\n");
            }
            // av_interleaved_write_frame takes ownership of recPkt's buffer reference.
        }
        av_write_trailer(session->outFmt);
        avio_closep(&session->outFmt->pb);
        avformat_free_context(session->outFmt);
        if (session->applyOvershootFix) {
            ColortransLut lut;
            lut.build(2.5f, -0.15f);
            reencodeWithOvershootFix(session->outPath, lut);
        }
    }).detach();

    printf("Recording -> %s\n", outPath.c_str());
    fflush(stdout);
}

// Called from the main loop for every incoming video packet while recording
// is active. Only clones the packet and hands it to the writer thread's
// queue -- must stay fast, since this runs inline with the network read/
// decode/render loop (see the RecordingSession comment for why that matters).
static void writeRecordingPacket(RecordState& rs, const AVPacket* pkt)
{
    if (!rs.recording || !rs.session) return;
    RecordingSession* s = rs.session.get();
    AVPacket* recPkt = av_packet_clone(pkt);
    if (!recPkt) return;
    if (s->startPts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) s->startPts = pkt->pts;
    int64_t off = (s->startPts == AV_NOPTS_VALUE) ? 0 : s->startPts;
    if (recPkt->pts != AV_NOPTS_VALUE) recPkt->pts -= off;
    if (recPkt->dts != AV_NOPTS_VALUE) recPkt->dts -= off;
    recPkt->stream_index = 0;

    // Reject non-monotonic/duplicate dts before it ever reaches the muxer
    // rather than trusting av_interleaved_write_frame to reject it gracefully
    // -- heavy RTP loss/reordering can hand us exactly this, and some muxers
    // (mpegts included) crash on it instead of erroring out cleanly.
    int64_t checkTs = (recPkt->dts != AV_NOPTS_VALUE) ? recPkt->dts : recPkt->pts;
    if (checkTs != AV_NOPTS_VALUE && s->lastDts != AV_NOPTS_VALUE && checkTs <= s->lastDts) {
        av_packet_free(&recPkt);
        return;
    }
    if (checkTs != AV_NOPTS_VALUE) s->lastDts = checkTs;

    // Hard cap: regardless of *why* the writer thread might fall behind (slow
    // disk, muxer overhead, anything else), an unbounded queue can exhaust
    // memory in seconds at this packet rate. Drop the newest packet instead
    // of ever growing past this -- the recording loses a frame, which is far
    // better than the whole process (including the live view) going down.
    constexpr size_t kMaxQueuedPackets = 300;
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lk(s->qMutex);
        if (s->pktQueue.size() >= kMaxQueuedPackets) {
            dropped = true;
        } else {
            s->pktQueue.push(recPkt);
        }
    }
    if (dropped) {
        av_packet_free(&recPkt);
        static int dropCount = 0;
        if ((dropCount++ % 100) == 0) {
            fprintf(stderr, "Warning: recording queue full, dropping frames (writer thread falling behind).\n");
        }
        return;
    }
    s->qCv.notify_one();
}

// Non-blocking: hands the session off to its writer thread (which already
// holds its own reference) and forgets it immediately. Draining the queue
// and finalizing the file happen in the background.
static void stopRecording(RecordState& rs, bool overshootFixEnabled)
{
    if (!rs.recording) return;
    {
        std::lock_guard<std::mutex> lk(rs.session->qMutex);
        rs.session->running = false;
        rs.session->applyOvershootFix = overshootFixEnabled;
    }
    rs.session->qCv.notify_one();
    rs.session.reset();
    rs.recording = false;
    printf("Recording stopped (finalizing in background).\n");
    fflush(stdout);
}

// Extracted so a sustained-corruption recovery can fully close and reopen the
// decoder (a new hardware decode SESSION on the GPU, not just FFmpeg's own
// software-side bookkeeping) instead of only avcodec_flush_buffers(), which
// resets our side but may not reach whatever state the D3D11VA session itself
// keeps -- confirmed: flushing alone made FFmpeg's own error log go quiet
// while the video stayed visually broken, meaning the corruption was surviving
// somewhere flush doesn't reach. hwDeviceCtx is reused across recreations
// (same GPU device); only the decoder context/session itself is torn down.
static AVCodecContext* createDecoder(const AVCodec* codec, AVCodecParameters* par,
                                      AVBufferRef* hwDeviceCtx, bool hwOk)
{
    AVCodecContext* c = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(c, par);
    if (hwOk) {
        c->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
        c->get_format = [](AVCodecContext*, const AVPixelFormat* fmts) -> AVPixelFormat {
            for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
                if (*p == APFPV_HWACCEL_PIXFMT) return *p;
            return fmts[0];
        };
    }
    c->err_recognition = 0;
    c->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    if (avcodec_open2(c, codec, nullptr) < 0) {
        avcodec_free_context(&c);
        return nullptr;
    }
    return c;
}

static TTF_Font* openFont(int size)
{
    const char* candidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\consola.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
#endif
    };
    for (auto p : candidates) {
        TTF_Font* f = TTF_OpenFont(p, size);
        if (f) return f;
    }
    return nullptr;
}

// Caches the rendered texture across calls and only redoes the (CPU-side glyph
// render + GPU texture upload) work when the string actually changes. The two
// callers redraw every render-loop iteration (i.e. at the VIDEO frame rate,
// 60-120fps) but their text only actually changes once a second (the FPS/res
// line) or on REC toggle -- doing the full TTF_RenderText_Blended +
// SDL_CreateTextureFromSurface + SDL_DestroyTexture cycle unconditionally on
// every frame was measurable, avoidable per-frame CPU/GPU work sitting
// directly in the same loop that must keep up with incoming RTP packets to
// avoid the OS socket buffer overflowing under a burst.
struct CachedText {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    std::string last;
};
static void drawTextCached(SDL_Renderer* ren, TTF_Font* font, CachedText& cache,
                           const std::string& text, int x, int y, SDL_Color color)
{
    if (!font) return;
    if (!cache.tex || text != cache.last) {
        if (cache.tex) { SDL_DestroyTexture(cache.tex); cache.tex = nullptr; }
        SDL_Surface* surf = TTF_RenderText_Blended(font, text.c_str(), color);
        if (!surf) return;
        cache.tex = SDL_CreateTextureFromSurface(ren, surf);
        cache.w = surf->w; cache.h = surf->h;
        SDL_FreeSurface(surf);
        cache.last = text;
    }
    if (!cache.tex) return;
    SDL_Rect dst{ x, y, cache.w, cache.h };
    SDL_RenderCopy(ren, cache.tex, NULL, &dst);
}

// Watches for MSP telemetry forwarded from the VTX and tracks flight-controller ARM state, so
// recording can cover exactly the armed window. Mirrors the Android MspArmListener; see that class
// for the full protocol notes. The two things worth repeating here:
//
//  - msposd on the VTX MUST run with `-d` as well as `-o <thisPC>:14550`. Without `-d` msposd never
//    ASKS the FC for anything and the stream is 100% MSP_DISPLAYPORT (cmd 182) -- pre-rendered
//    "draw these characters here" OSD commands with no structured telemetry, so no arm bit exists
//    on the wire at all. With `-d` msposd polls and MSP_STATUS (cmd 101) appears.
//  - This uses an ordinary OS UDP socket, which is correct for the Windows GS because it talks to
//    the VTX over the stock Wi-Fi driver in infrastructure mode (same as lqfeedback_cli).
struct MspArmWatcher {
    std::thread th;
    std::atomic<bool> run{ false };
    std::atomic<int>  armed{ -1 };      // -1 = unknown (no MSP_STATUS yet), 0 = disarmed, 1 = armed
    std::atomic<uint32_t> gen{ 0 };     // bumped on every state CHANGE so the UI loop can see edges
    int port = 14550;

    void start() {
        if (run.exchange(true)) return;
        th = std::thread([this]() {
#ifdef _WIN32
            WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
            int s = (int)::socket(AF_INET, SOCK_DGRAM, 0);
            if (s < 0) { fprintf(stderr, "MSP: socket() failed\n"); return; }
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
                fprintf(stderr, "MSP: bind(%d) failed -- auto-record on arm disabled\n", port);
#ifdef _WIN32
                ::closesocket(s);
#else
                ::close(s);
#endif
                return;
            }
            // 1s timeout so `run` is re-checked and shutdown is prompt.
#ifdef _WIN32
            DWORD tv = 1000; ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
            timeval tv{ 1, 0 }; ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            fprintf(stderr, "MSP: listening for arm state on UDP %d\n", port);
            std::vector<uint8_t> buf(2048);
            while (run.load()) {
                int n = ::recv(s, (char*)buf.data(), (int)buf.size(), 0);
                if (n > 0) parse(buf.data(), n);
            }
#ifdef _WIN32
            ::closesocket(s);
#else
            ::close(s);
#endif
        });
    }

    void stop() {
        if (!run.exchange(false)) return;
        if (th.joinable()) th.join();
    }

    // Walk every MSP v1 frame in the datagram (msposd aggregates several per packet).
    void parse(const uint8_t* b, int len) {
        int i = 0;
        while (i + 5 <= len) {
            if (!(b[i] == '$' && b[i + 1] == 'M' && b[i + 2] == '>')) { i++; continue; }
            int plen = b[i + 3], cmd = b[i + 4], ps = i + 5;
            if (ps + plen + 1 > len) break;                  // truncated -- don't guess
            int crc = plen ^ cmd;
            for (int k = 0; k < plen; k++) crc ^= b[ps + k];
            if ((crc & 0xff) != b[ps + plen]) { i++; continue; }   // false "$M>" match in payload
            // MSP_STATUS(101) / MSP_STATUS_EX(150) share a prefix: u16 cycleTime, u16 i2cErrors,
            // u16 sensor, u32 flightModeFlags. Bit 0 = Betaflight box id 0 = ARM. Verified live:
            // flags read 0x104 while disarmed (bits 2+8; box id 2 is HORIZON, matching the OSD's
            // "HOR" mode field at that moment) -- which confirms this offset and bit numbering.
            if ((cmd == 101 || cmd == 150) && plen >= 10) {
                uint32_t f = (uint32_t)b[ps + 6] | ((uint32_t)b[ps + 7] << 8)
                           | ((uint32_t)b[ps + 8] << 16) | ((uint32_t)b[ps + 9] << 24);
                int now = (f & 1u) ? 1 : 0;
                int prev = armed.exchange(now);
                if (prev != now) {
                    fprintf(stderr, "MSP: %s (flags=0x%08x)\n", now ? "ARMED" : "DISARMED", f);
                    fflush(stderr);
                    gen.fetch_add(1);
                }
            }
            i = ps + plen + 1;
        }
    }
};

int main(int argc, char** argv)
{
    // Dev/test-only hook: exercise reencodeWithOvershootFix directly on an existing recording,
    // no SDL/GUI/live-stream needed. Not part of the normal app flow.
    if (argc >= 3 && std::string(argv[1]) == "--reencode-test") {
        ColortransLut lut; lut.build(2.5f, -0.15f);
        reencodeWithOvershootFix(argv[2], lut);
        return 0;
    }
    std::string dir = exeDir();
    std::string sdpPath = dir + APFPV_PATH_SEP + "apfpv_h265.sdp";

    // Unique filename per run: a restart after a crash must not overwrite the
    // previous run's watchdog log before it can be read.
    {
        time_t now = time(nullptr);
        struct tm tmBuf;
#ifdef _WIN32
        localtime_s(&tmBuf, &now);
#else
        localtime_r(&now, &tmBuf);
#endif
        char wname[64];
        snprintf(wname, sizeof(wname), "watchdog_log_%04d%02d%02d_%02d%02d%02d.txt",
                 tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                 tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
        std::thread(watchdogThread, dir + APFPV_PATH_SEP + wname).detach();
    }

    av_log_set_callback(ffmpegLogCallback);
    avformat_network_init();

    int videoStream = -1;
    AVFormatContext* fmt = openInput(sdpPath, videoStream, /*isReconnect=*/false);
    if (!fmt) { fprintf(stderr, "Failed to open %s\n", sdpPath.c_str()); return 1; }

    AVCodecParameters* par = fmt->streams[videoStream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    // Real dimensions from the stream's own declared parameters -- used below
    // to reject frames with corrupted/garbage width or height (a real risk
    // given the relaxed err_recognition/SHOW_ALL settings below, combined
    // with the confirmed heavy packet loss) instead of treating them as a
    // legitimate resolution change and recreating the SDL texture + SwsContext
    // for them, which over many minutes of sustained bad frames could be
    // exactly the kind of repeated GPU resource churn that leaks without
    // showing up in ordinary process memory counters.
    const int expectedW = par->width, expectedH = par->height;

    // Hardware decode via D3D11VA (works for AMD/Intel/Nvidia alike on Windows,
    // backed by the GPU's own decode block -- RDNA3's VCN here). Offloads decode
    // off the CPU so we can keep up with the incoming rate, and -- like VLC and
    // Android's MediaCodec, both hardware-accelerated -- tends to conceal/continue
    // through a missing reference far more gracefully than libavcodec's software
    // HEVC decoder, which drops every frame in a GOP once one reference is missing.
    // hwDeviceCtx (the GPU device itself) is created once and reused across any
    // later decoder recreation -- only the decode SESSION gets torn down/rebuilt.
    AVBufferRef* hwDeviceCtx = nullptr;
    bool hwOk = !getenv("APFPV_NO_HWACCEL")
             && av_hwdevice_ctx_create(&hwDeviceCtx, APFPV_HWACCEL_TYPE, nullptr, nullptr, 0) >= 0;
    if (hwOk) printf("Using %s hardware decode.\n", APFPV_HWACCEL_NAME);
    else      fprintf(stderr, "%s hw device unavailable, falling back to software decode.\n", APFPV_HWACCEL_NAME);

    AVCodecContext* ctx = createDecoder(codec, par, hwDeviceCtx, hwOk);
    if (!ctx) { fprintf(stderr, "Failed to open decoder\n"); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    TTF_Init();

    SDL_Window* win = SDL_CreateWindow("APFPV", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* tex = nullptr;
    int texW = 0, texH = 0;

    TTF_Font* font = openFont(22);
    if (!font) fprintf(stderr, "Warning: no font found, overlay text disabled.\n");

    SwsContext* sws = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVFrame* hwSwFrame = av_frame_alloc();   // hwaccel frame transferred to CPU memory
    AVFrame* frameYuv = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    // REC button geometry (top-left, below the FPS/res text line) -- sized
    // generously (a first attempt at 90x34 was missed in testing).
    SDL_Rect recBtn{ 12, 44, 160, 60 };
    // Overshoot Fix button (top-right, mirrors recBtn). x is recomputed every frame from the
    // current renderer output size (the window is resizable/fullscreen-desktop, so it isn't a
    // fixed logical size) so it stays pinned to the top-right corner.
    SDL_Rect ctBtn{ 0, 44, 160, 60 };
    // AUTO button: sits under REC (top-left). When on, ARM starts recording and DISARM stops it,
    // so a clip is exactly one flight instead of the whole session.
    SDL_Rect autoBtn{ 12, 112, 160, 60 };
    CachedText statLineText, recBtnText, ctBtnText, autoBtnText;
    ColortransLut ctLut;
    ctLut.build(2.5f, -0.15f);
    bool overshootFixEnabled = false;
    bool autoRecordEnabled = false;
    MspArmWatcher msp;
    uint32_t lastArmGen = 0;

    RecordState rec;
    bool running = true;
    int frameCount = 0;
    double fps = 0.0;
    auto fpsWindowStart = std::chrono::steady_clock::now();
    // Set whenever the decoder is flushed (see the health-check block below):
    // avcodec_flush_buffers() discards every reference picture, so the very
    // next packet fed to it is decoding against NO reference at all. Feeding
    // it a P/B-frame there either errors again immediately (re-triggering the
    // same flush next second -- an unrecoverable flush/error loop, confirmed
    // in the field as the "same issue... corrupt permanently" symptom) or
    // decodes silently-wrong output with no error at all. Drop every packet
    // here until a real IDR (AV_PKT_FLAG_KEY) arrives, exactly like a fresh
    // decoder start -- the flush is only a real resync if paired with this.
    bool waitingForKeyframe = false;

    while (running) {
        STEP(STEP_LOOP_TOP);
        {
            int outW, outH;
            SDL_GetRendererOutputSize(ren, &outW, &outH);
            ctBtn.x = outW - ctBtn.w - 12;
        }
        SDL_Event e;
        STEP(STEP_POLL_EVENTS);
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                // Fullscreen-desktop windows can report mouse events in a
                // different coordinate space than the renderer's actual output
                // (DPI scaling) -- SDL_RenderWindowToLogical converts window
                // coordinates into renderer coordinates so they line up with
                // recBtn, which is defined in renderer space.
                float lx, ly;
                SDL_RenderWindowToLogical(ren, e.button.x, e.button.y, &lx, &ly);
                SDL_Point p{ (int)lx, (int)ly };
                bool hit = SDL_PointInRect(&p, &recBtn);
                printf("click: window=(%d,%d) logical=(%d,%d) recBtn=(%d,%d,%d,%d) hit=%d\n",
                       e.button.x, e.button.y, p.x, p.y,
                       recBtn.x, recBtn.y, recBtn.w, recBtn.h, hit);
                fflush(stdout);
                if (hit) {
                    if (rec.recording) stopRecording(rec, overshootFixEnabled);
                    else startRecording(rec, dir, fmt, videoStream);
                }
                bool ctHit = SDL_PointInRect(&p, &ctBtn);
                if (ctHit) {
                    overshootFixEnabled = !overshootFixEnabled;
                    printf("Overshoot Fix -> %s\n", overshootFixEnabled ? "ON" : "OFF");
                    fflush(stdout);
                }
                if (SDL_PointInRect(&p, &autoBtn)) {
                    autoRecordEnabled = !autoRecordEnabled;
                    if (autoRecordEnabled) { msp.start(); lastArmGen = msp.gen.load(); }
                    else                   { msp.stop(); }
                    printf("Auto record (on arm) -> %s%s\n", autoRecordEnabled ? "ON" : "OFF",
                           autoRecordEnabled ? "  [needs msposd -d -o <thisPC>:14550 on the VTX]" : "");
                    fflush(stdout);
                }
            }
        }

        // Act on ARM/DISARM edges. Done here (not in the socket thread) so recording start/stop
        // stays on the same thread that owns fmt/rec, exactly like the REC button path.
        if (autoRecordEnabled) {
            uint32_t g = msp.gen.load();
            if (g != lastArmGen) {
                lastArmGen = g;
                bool nowArmed = msp.armed.load() == 1;
                if (nowArmed && !rec.recording)      startRecording(rec, dir, fmt, videoStream);
                else if (!nowArmed && rec.recording) stopRecording(rec, overshootFixEnabled);
            }
        }

        STEP(STEP_READ_FRAME);
        // fmt can be null here after a PRIOR reconnect attempt itself failed
        // (openInput() returned nullptr below, and the previous fmt was
        // already closed) -- av_read_frame(NULL, ...) is undefined behavior,
        // not a documented-safe no-op, so this must be checked explicitly
        // rather than assumed harmless. Treat it exactly like any other read
        // failure: fall straight into the reconnect-retry branch.
        int rc = -1;
        if (fmt) {
            long long __t0 = nowMs();
            rc = av_read_frame(fmt, pkt);
            long long __dt = nowMs() - __t0;
            if (__dt > 15) fprintf(stderr, "[trace] av_read_frame took %lldms\n", __dt);
        }
        if (rc >= 0) {
            ioNotedProgress();
            if (pkt->stream_index == videoStream && waitingForKeyframe && (pkt->flags & AV_PKT_FLAG_KEY)) {
                waitingForKeyframe = false;
                fprintf(stderr, "Resync: keyframe arrived, resuming decode.\n");
                fflush(stderr);
            }
            if (pkt->stream_index == videoStream && !waitingForKeyframe) {
                STEP(STEP_WRITE_REC_PKT);
                writeRecordingPacket(rec, pkt);
                STEP(STEP_SEND_PACKET);
                if (avcodec_send_packet(ctx, pkt) == 0) {
                    STEP(STEP_RECEIVE_FRAME);
                    while (avcodec_receive_frame(ctx, frame) == 0) {
                        // The hwaccel hands back a GPU-resident frame (format ==
                        // APFPV_HWACCEL_PIXFMT); pull it into normal CPU memory
                        // (usually NV12) so the existing sws_scale/SDL path below
                        // can use it unchanged.
                        AVFrame* useFrame = frame;
                        if (frame->format == APFPV_HWACCEL_PIXFMT) {
                            STEP(STEP_HW_TRANSFER);
                            av_frame_unref(hwSwFrame);
                            long long __t0 = nowMs();
                            int __hwrc = av_hwframe_transfer_data(hwSwFrame, frame, 0);
                            { long long __dt = nowMs() - __t0; if (__dt > 15) fprintf(stderr, "[trace] av_hwframe_transfer_data took %lldms (rc=%d)\n", __dt, __hwrc); }
                            if (__hwrc < 0) {
                                av_frame_unref(frame);
                                STEP(STEP_RECEIVE_FRAME);
                                continue;
                            }
                            useFrame = hwSwFrame;
                        }

                        // Only reject truly invalid dimensions (<=0, which would
                        // crash sws_getContext/SDL_CreateTexture outright). A
                        // MISMATCHED-but-still-positive size is logged, not
                        // dropped -- silently dropping it risks a *permanent*
                        // freeze if corruption puts the decoder into a state
                        // where every subsequent frame reports the same mismatch
                        // (confirmed: this is worse than the occasional garbled
                        // frame it was meant to prevent, since a garbled frame at
                        // least gets replaced by the next good one).
                        STEP(STEP_DIM_CHECK);
                        if (useFrame->width <= 0 || useFrame->height <= 0) {
                            av_frame_unref(frame);
                            STEP(STEP_RECEIVE_FRAME);
                            continue;
                        }
                        if (expectedW > 0 && expectedH > 0 &&
                            (useFrame->width != expectedW || useFrame->height != expectedH)) {
                            static int dimWarnCount = 0;
                            if ((dimWarnCount++ % 100) == 0) {
                                fprintf(stderr, "Warning: frame dims %dx%d != expected %dx%d (rendering anyway)\n",
                                        useFrame->width, useFrame->height, expectedW, expectedH);
                            }
                        }
                        if (!sws || texW != useFrame->width || texH != useFrame->height) {
                            STEP(STEP_SWS_GETCONTEXT);
                            if (sws) sws_freeContext(sws);
                            sws = sws_getContext(useFrame->width, useFrame->height, (AVPixelFormat)useFrame->format,
                                                  useFrame->width, useFrame->height, AV_PIX_FMT_YUV420P,
                                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
                            STEP(STEP_SDL_CREATETEXTURE);
                            if (tex) SDL_DestroyTexture(tex);
                            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                                     useFrame->width, useFrame->height);
                            texW = useFrame->width; texH = useFrame->height;
                            av_frame_unref(frameYuv);
                            frameYuv->format = AV_PIX_FMT_YUV420P;
                            frameYuv->width = useFrame->width; frameYuv->height = useFrame->height;
                            av_frame_get_buffer(frameYuv, 32);
                        }
                        STEP(STEP_SWS_SCALE);
                        {
                            long long __t0 = nowMs();
                            sws_scale(sws, useFrame->data, useFrame->linesize, 0, useFrame->height,
                                      frameYuv->data, frameYuv->linesize);
                            long long __dt = nowMs() - __t0;
                            if (__dt > 15) fprintf(stderr, "[trace] sws_scale took %lldms\n", __dt);
                        }
                        if (overshootFixEnabled) applyColortrans(frameYuv, ctLut);
                        STEP(STEP_SDL_UPDATETEX);
                        {
                            long long __t0 = nowMs();
                            SDL_UpdateYUVTexture(tex, NULL,
                                                 frameYuv->data[0], frameYuv->linesize[0],
                                                 frameYuv->data[1], frameYuv->linesize[1],
                                                 frameYuv->data[2], frameYuv->linesize[2]);
                            long long __dt = nowMs() - __t0;
                            if (__dt > 15) fprintf(stderr, "[trace] SDL_UpdateYUVTexture took %lldms\n", __dt);
                        }
                        frameCount++;
                        g_frameCounter.fetch_add(1, std::memory_order_relaxed);
                        STEP(STEP_UNREF_FRAME);
                        av_frame_unref(frame);
                        STEP(STEP_RECEIVE_FRAME);
                    }
                }
            }
            av_packet_unref(pkt);
        } else {
            // Interrupt-callback abort (idle > kIoTimeoutMs) or a genuine
            // protocol error both land here as a negative return. Either way
            // the AVIOContext underneath fmt is now left broken/EOF -- every
            // further av_read_frame() call on it would just fail instantly,
            // forever, which is exactly the "permanently bad video" symptom
            // (whatever frame was on screen when this happened never gets
            // replaced). Close it and open a fresh one so the stream can
            // resync once packets resume, instead of leaving it dead for the
            // rest of the run. The decoder (ctx), hwDeviceCtx and codec are
            // untouched -- only the input needs replacing.
            fprintf(stderr, "Read failed/stalled (rc=%d) -- reconnecting...\n", rc);
            fflush(stderr);
            avformat_close_input(&fmt);
            int newVideoStream = -1;
            AVFormatContext* newFmt = openInput(sdpPath, newVideoStream, /*isReconnect=*/true);
            if (newFmt) {
                fmt = newFmt;
                videoStream = newVideoStream;
                fprintf(stderr, "Reconnected.\n");
            } else {
                SDL_Delay(200);   // don't hammer reopen attempts in a tight spin
            }
            fflush(stderr);
        }

        STEP(STEP_FPS_CALC);
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fpsWindowStart).count();
        if (elapsed >= 1.0) {
            fps = frameCount / elapsed;
            frameCount = 0;
            fpsWindowStart = now;

            // A handful of decode-error log lines in a second is normal
            // survivable loss. Dozens means the reference-picture state is
            // genuinely corrupted and, left alone, will stay that way
            // indefinitely (the exact "permanently bad video" behavior this
            // is meant to fix) -- recreate the decoder so it discards that
            // state and gets a clean shot at resyncing on the next keyframe,
            // instead of limping along broken until a clean IDR happens to
            // arrive on its own (which, under sustained loss, may never
            // happen). A full recreation (createDecoder), not just
            // avcodec_flush_buffers(): flushing alone was confirmed to reset
            // only FFmpeg's own bookkeeping while a hwaccel (D3D11VA) session
            // can keep corrupted GPU-side state flush never reaches -- the
            // error log would go quiet but the video stayed visually broken.
            // Pairs with waitingForKeyframe below: a freshly (re)created
            // decoder has NO reference pictures at all, so the very next
            // packet handed to it MUST be a real IDR, not whatever P/B-frame
            // happens to arrive next -- feeding it anything else just
            // re-corrupts (or re-errors) immediately, which is exactly the
            // flush/error loop this two-part fix replaces.
            int errs = g_decodeErrorCount.exchange(0, std::memory_order_relaxed);
            fprintf(stderr, "[health] decode_errors_last_sec=%d\n", errs);   // always visible, not just on trigger
            constexpr int kFlushThreshold = 5;   // lowered from 30 pending real observed-rate data
            if (errs > kFlushThreshold) {
                // Build the replacement before freeing the old one -- a failed
                // avcodec_open2 here must not leave ctx null (every send_packet/
                // receive_frame call below assumes a live decoder); keep limping
                // along on the old, possibly-corrupted one rather than crash.
                AVCodecContext* newCtx = createDecoder(codec, fmt->streams[videoStream]->codecpar, hwDeviceCtx, hwOk);
                if (newCtx) {
                    avcodec_free_context(&ctx);
                    ctx = newCtx;
                    waitingForKeyframe = true;
                    fprintf(stderr, "Recovery: %d decode errors in the last second -- recreated decoder, waiting for keyframe.\n", errs);
                } else {
                    fprintf(stderr, "Recovery: %d decode errors, but decoder recreation failed -- keeping existing decoder.\n", errs);
                }
            }
            fflush(stderr);
        }

        STEP(STEP_RENDER_CLEAR);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        STEP(STEP_RENDER_COPY);
        if (tex && texW > 0 && texH > 0) {
            int winW, winH;
            SDL_GetRendererOutputSize(ren, &winW, &winH);
            double srcAspect = (double)texW / texH;
            double dstAspect = (double)winW / winH;
            SDL_Rect dst;
            if (srcAspect > dstAspect) {          // video wider than window -> letterbox top/bottom
                dst.w = winW;
                dst.h = (int)(winW / srcAspect);
                dst.x = 0;
                dst.y = (winH - dst.h) / 2;
            } else {                              // video narrower than window -> pillarbox left/right
                dst.h = winH;
                dst.w = (int)(winH * srcAspect);
                dst.y = 0;
                dst.x = (winW - dst.w) / 2;
            }
            SDL_RenderCopy(ren, tex, NULL, &dst);
        }

        STEP(STEP_DRAW_TEXT);
        char statLine[64];
        snprintf(statLine, sizeof(statLine), "%dx%d  %.1f fps", texW, texH, fps);
        drawTextCached(ren, font, statLineText, statLine, 12, 10, SDL_Color{ 255, 255, 255, 255 });

        STEP(STEP_DRAW_BUTTON);
        SDL_SetRenderDrawColor(ren, rec.recording ? 200 : 60, rec.recording ? 30 : 60, 30, 220);
        SDL_RenderFillRect(ren, &recBtn);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderDrawRect(ren, &recBtn);
        drawTextCached(ren, font, recBtnText, rec.recording ? "REC ||" : "REC", recBtn.x + 20, recBtn.y + 18,
                       SDL_Color{ 255, 255, 255, 255 });

        SDL_SetRenderDrawColor(ren, overshootFixEnabled ? 30 : 60, overshootFixEnabled ? 160 : 60, 60, 220);
        SDL_RenderFillRect(ren, &ctBtn);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderDrawRect(ren, &ctBtn);
        drawTextCached(ren, font, ctBtnText, overshootFixEnabled ? "O.FIX ON" : "O.FIX", ctBtn.x + 16, ctBtn.y + 18,
                       SDL_Color{ 255, 255, 255, 255 });

        // AUTO: green when enabled. Label reflects the live arm state once MSP is flowing, so it
        // doubles as a "is telemetry actually arriving?" indicator -- "AUTO --" means enabled but
        // no MSP_STATUS seen yet (msposd missing -d/-o, or the IP route isn't up).
        SDL_SetRenderDrawColor(ren, autoRecordEnabled ? 30 : 60, autoRecordEnabled ? 160 : 60, 60, 220);
        SDL_RenderFillRect(ren, &autoBtn);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderDrawRect(ren, &autoBtn);
        const char* autoLbl = !autoRecordEnabled ? "AUTO"
                            : (msp.armed.load() < 0 ? "AUTO --"
                            : (msp.armed.load() == 1 ? "AUTO ARM" : "AUTO OFF"));
        drawTextCached(ren, font, autoBtnText, autoLbl, autoBtn.x + 12, autoBtn.y + 18,
                       SDL_Color{ 255, 255, 255, 255 });

        STEP(STEP_RENDER_PRESENT);
        {
            long long __t0 = nowMs();
            SDL_RenderPresent(ren);
            long long __dt = nowMs() - __t0;
            if (__dt > 15) fprintf(stderr, "[trace] SDL_RenderPresent took %lldms\n", __dt);
        }
    }

    msp.stop();   // join the MSP listener before we start tearing down
    stopRecording(rec, overshootFixEnabled);   // signals any still-active session to stop
    for (auto& t : g_writerThreads) if (t.joinable()) t.join();   // let every writer finish/finalize before exit

    if (sws) sws_freeContext(sws);
    av_frame_free(&frame);
    av_frame_free(&hwSwFrame);
    av_frame_free(&frameYuv);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (hwDeviceCtx) av_buffer_unref(&hwDeviceCtx);
    avformat_close_input(&fmt);
    if (font) TTF_CloseFont(font);
    TTF_Quit();
    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
