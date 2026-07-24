# windows-gs

Standalone Windows ground-station tools for APFPV, built against the stock
Windows WLAN driver (infrastructure mode) instead of libusb/devourer -- no
monitor-mode dependency.

- `apfpv_player.cpp` -- custom FFmpeg (libav*) + SDL2 video viewer, replacing
  ffplay. Real FPS/resolution overlay, a clickable REC button that muxes the
  live stream to MPEG-TS, D3D11VA hardware decode (AMD/Intel/Nvidia) with a
  software fallback (`APFPV_NO_HWACCEL=1`), and an I/O watchdog
  (`AVIOInterruptCB`) that reconnects the input instead of hanging forever
  when packets stop arriving.
- `apfpv_watch.cpp` -- one-click launcher: starts the player, closes cleanly
  on exit.
- `lqfeedback_cli.cpp` -- sends live RSSI (read via the Windows WLAN API,
  `wlanapi.h`) to the VTX's aalink link-quality service as UDP feedback.

## Build (MSYS2 MinGW64 shell)

```sh
g++ -std=c++17 -O2 apfpv_player.cpp -o apfpv_player.exe \
  -IC:/msys64/mingw64/include -IC:/msys64/mingw64/include/SDL2 -Dmain=SDL_main \
  -LC:/msys64/mingw64/lib -lavformat -lavcodec -lswscale -lavutil \
  -lmingw32 -lSDL2main -lSDL2 -lSDL2_ttf -mconsole

g++ -std=c++17 -O2 apfpv_watch.cpp -o apfpv_watch.exe -mconsole

g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ \
  lqfeedback_cli.cpp -o lqfeedback_cli.exe -lws2_32 -lwlanapi -lole32
```

Then copy the required MinGW64 runtime DLLs next to the exes (`ldd
apfpv_player.exe` lists them) -- not tracked in git, see `.gitignore`.

## Run

`apfpv_watch.exe` launches the player against `apfpv_h265.sdp` (edit the SDP
if your VTX's RTP port/codec differs) and closes both together on exit.
