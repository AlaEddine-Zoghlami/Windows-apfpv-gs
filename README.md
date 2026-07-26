# apfpv-pc-groundstation

Standalone **cross-platform** PC ground-station tools for APFPV — Windows,
Linux (x64/arm64), and macOS (Apple silicon). Built against each OS's stock
WLAN stack (infrastructure mode) instead of libusb/devourer, so there's no
monitor-mode dependency: the PC just associates to the VTX's AP like any Wi-Fi
client, and these tools handle the video, OSD, telemetry, and link feedback.

> Formerly `windows-gs` / `Windows-apfpv-gs` — it is no longer Windows-only.

## Components

- `apfpv_player.cpp` — FFmpeg (libav*) + SDL2 video viewer replacing ffplay:
  live FPS/resolution overlay, a clickable REC button that muxes the stream to
  MPEG-TS, hardware decode with a software fallback (`APFPV_NO_HWACCEL=1`), and
  an I/O watchdog (`AVIOInterruptCB`) that reconnects instead of hanging when
  packets stop. Renders the **MSP/Betaflight OSD** and the **aalink status line**
  over the video, and plays **voice alerts** (arm/disarm, battery, link) from
  the bundled EdgeTX pack.
- `WifiLink*` — the per-OS RSSI/link backends: Windows (`wlanapi.h`), Linux
  (`/proc/net/wireless`), macOS (`CoreWLAN`, Objective-C++ `.mm`). CI is what
  actually compiles the Linux/macOS backends.
- `aalink` (in `apfpv_player`) — receives the VTX's aalink stats over **UDP
  14551** (pushed by the VTX-side `aalink_udp` relay); `APFPV_AALINK_HTTP=1`
  reverts to the old HTTP fetch. MSP telemetry arrives on **UDP 14550** (msposd
  `-d -o <thisPC>:14550`).
- `apfpv_watch.cpp` — one-click launcher: starts the player, closes cleanly on
  exit. (Windows convenience wrapper.)
- `lqfeedback_cli.cpp` — sends live RSSI to the VTX's aalink link-quality
  service as UDP feedback (Windows-only tool; uses `wlanapi.h`).

## Features

- **Dual video recording (raw + OSD).** The REC button writes an untouched,
  lossless remux of the stream (`<name>.ts`) and, alongside it, a second copy
  with the OSD burned in (`<name>_osd.ts`) — derived from the corrected raw so
  it inherits the Overshoot-Fix LUT exactly once. "Raw" really is raw, because
  the OSD is composited on the ground, not baked in by the VTX.
- **Custom OSD fonts.** The ground-side OSD renders from a Betaflight glyph
  atlas in `fonts/`. Drop in a `font_custom.png` to override (it wins over the
  bundled `font_btfl.png` / `font_inav.png` / `font_ardu.png`); any atlas glyph
  size auto-scales, so a 24×36 or 48×72 HD atlas needs no code change.
- **Voice alerts (audio).** OpenTX/EdgeTX-style spoken alerts built from
  concatenated clips (the EdgeTX pack in `sounds/`): arm/disarm, battery
  voltage/level, and link-quality warnings, driven by an edge-triggered rule
  table (`apfpv_sounds.conf`) reading the arm bit and OSD-scraped values.
- **MSP OSD + aalink line over the link.** Renders the Betaflight/MSP OSD (from
  msposd `-d -o <thisPC>:14550`) and the aalink stats line (from the VTX
  `aalink_udp` relay on UDP 14551) over the live video.

## Prebuilt downloads

Each tagged release ships a self-contained archive per platform (binary +
`fonts/`, `sounds/`, `apfpv_sounds.conf`; Windows also bundles the MinGW
runtime DLLs) — see the **Releases** page:

- `apfpv-gs-windows-x64-*.zip`
- `apfpv-gs-linux-x64-*.zip`, `apfpv-gs-linux-arm64-*.zip`
- `apfpv-gs-macos-arm64-*.zip`

## Build from source (CMake, all platforms)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

CMake stages `fonts/`, `sounds/`, and `apfpv_sounds.conf` next to the binary,
so run the player from the build directory (or from an unpacked release zip).

**Dependencies** — SDL2, SDL2_ttf, and FFmpeg (libavformat/libavcodec/
libavutil/libswscale) dev packages:

- **Linux (Debian/Ubuntu):**
  `sudo apt-get install build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev`
- **macOS (Homebrew):** `brew install cmake pkg-config sdl2 sdl2_ttf ffmpeg`
- **Windows (MSYS2 MINGW64):**
  `pacman -S mingw-w64-x86_64-{gcc,cmake,pkgconf,SDL2,SDL2_ttf,ffmpeg,make}`,
  then `cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`.
  Copy the MinGW64 runtime DLLs next to the exe (`ldd apfpv_player.exe` lists
  them) — not tracked in git, see `.gitignore`.

All targets are verified in CI (`.github/workflows/build.yml`): Linux x64,
Linux arm64 (arm64 container), macOS arm64, and Windows x64. A version tag
(`vX.Y.Z`) additionally packages every platform and publishes a GitHub release.

## Run

Launch `apfpv_player` (or `apfpv_watch.exe` on Windows) against your VTX's RTP
stream. Edit the `.sdp` if your VTX's RTP port/codec differs. For the OSD /
voice alerts / aalink line, run msposd on the VTX with `-d -o <thisPC>:14550`
and the `aalink_udp` relay pushing to `<thisPC>:14551`.
