# ---------------------------------------------------------------------------
# APFPV ground station — cross-platform build (Windows x64, Linux x64,
# Linux arm64, macOS arm64).
#
# The directory is still called windows-gs for history; the player itself is
# portable. Platform-specific code is confined to:
#   - LinkStats  (Wi-Fi RSSI / PHY rate)  — Windows wlanapi, Linux
#     /proc/net/wireless, macOS: unavailable -> reports "unknown"
#   - exeDir()   (GetModuleFileName vs /proc/self/exe vs _NSGetExecutablePath)
# Everything else is SDL2 + FFmpeg, which are portable as-is.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#
# Deps:
#   Debian/Ubuntu  sudo apt install build-essential cmake pkg-config \
#                    libsdl2-dev libsdl2-ttf-dev libavformat-dev libavcodec-dev \
#                    libavutil-dev libswscale-dev
#   macOS (brew)   brew install cmake pkg-config sdl2 sdl2_ttf ffmpeg
#   Windows/MSYS2  pacman -S mingw-w64-x86_64-{cmake,SDL2,SDL2_ttf,ffmpeg}
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(apfpv_gs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- dependencies ----------------------------------------------------------
# pkg-config is the reliable route for FFmpeg on every platform we target
# (Homebrew, apt, and MSYS2 all ship .pc files); FFmpeg has no official
# CMake config package, so find_package would only work by accident.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale)
pkg_check_modules(SDL2    REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

# --- the ground station ----------------------------------------------------
# WifiLink: one file per platform, selected here rather than by #ifdef inside a single
# source. The Windows/Linux/Null files self-guard too, so a stale entry cannot break a build.
set(WIFILINK_SRC WifiLinkWindows.cpp WifiLinkLinux.cpp WifiLinkNull.cpp)
if(APPLE)
  # CoreWLAN is Objective-C only, so the macOS implementation is .mm.
  list(APPEND WIFILINK_SRC WifiLinkMac.mm)
endif()

add_executable(apfpv_player apfpv_player.cpp LqFeedback.cpp ${WIFILINK_SRC})
target_link_libraries(apfpv_player PRIVATE
  PkgConfig::FFMPEG PkgConfig::SDL2 PkgConfig::SDL2TTF)

if(WIN32)
  # ws2_32: winsock. wlanapi: the RSSI/PHY-rate source in LinkStats.
  # -mconsole keeps stdout/stderr attached so the diagnostic logging is visible.
  target_link_libraries(apfpv_player PRIVATE ws2_32 wlanapi)
  target_link_options(apfpv_player PRIVATE -mconsole)
  # SDL2 redefines main on Windows; the shipped build line used -Dmain=SDL_main
  # together with -lSDL2main, and SDL2's pkg-config already pulls SDL2main in.
  target_compile_definitions(apfpv_player PRIVATE main=SDL_main)
elseif(APPLE)
  find_library(COCOA_LIB Cocoa)
  find_library(COREWLAN_LIB CoreWLAN)      # WifiLinkMac.mm
  find_library(FOUNDATION_LIB Foundation)
  target_link_libraries(apfpv_player PRIVATE ${COCOA_LIB} ${COREWLAN_LIB} ${FOUNDATION_LIB})
else()
  # Linux (x64 and arm64): pthread + libm are all the extras needed. The RSSI
  # path reads /proc/net/wireless, so no libnl dependency.
  find_package(Threads REQUIRED)
  target_link_libraries(apfpv_player PRIVATE Threads::Threads m)
endif()

# --- offline OSD renderer (portable; no SDL window needed) -----------------
# Replays a captured MSP stream and rasterises the OSD to PNG, so glyph
# placement can be verified without a live link. Keeps the font/atlas code
# honest on every platform.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/osd_render_test.cpp")
  add_executable(osd_render_test osd_render_test.cpp)
  target_link_libraries(osd_render_test PRIVATE PkgConfig::FFMPEG)
endif()

# --- Windows-only helper tools --------------------------------------------
# lqfeedback_cli and apfpv_watch use wlanapi and the Win32 process API
# (WaitForSingleObject) directly and have no portable equivalent, so they are
# deliberately NOT built elsewhere rather than stubbed into something that
# silently does nothing.
if(WIN32)
  # lqfeedback_cli drives apfpv::LqFeedback directly, so it needs that translation
  # unit too; apfpv_watch only shells out to ffmpeg and needs nothing extra.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lqfeedback_cli.cpp")
    add_executable(lqfeedback_cli lqfeedback_cli.cpp LqFeedback.cpp)
    target_link_libraries(lqfeedback_cli PRIVATE ws2_32 wlanapi)
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apfpv_watch.cpp")
    add_executable(apfpv_watch apfpv_watch.cpp)
    target_link_libraries(apfpv_watch PRIVATE ws2_32)
  endif()
endif()

# --- runtime assets --------------------------------------------------------
# Everything the player loads RELATIVE TO ITS OWN EXECUTABLE must land in the
# build dir, or a fresh clone builds fine and then fails at runtime -- which is
# exactly what happened: the binary exited on a missing .sdp before it ever
# reached audio init, and the sounds/ + conf had to be copied by hand.
#
# Directories are copied; files are copied only if absent, so a locally edited
# apfpv_sounds.conf or apfpv_settings.ini in the build dir is not clobbered on
# every rebuild.
foreach(assetDir fonts sounds)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${assetDir}")
    add_custom_command(TARGET apfpv_player POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/${assetDir}"
              "$<TARGET_FILE_DIR:apfpv_player>/${assetDir}"
      COMMENT "assets: ${assetDir}/")
  endif()
endforeach()

# apfpv_sounds.conf : the voice-alert condition table
# apfpv_h265.sdp    : the RTP stream description the player opens by default
foreach(assetFile apfpv_sounds.conf apfpv_h265.sdp)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${assetFile}")
    add_custom_command(TARGET apfpv_player POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_CURRENT_SOURCE_DIR}/${assetFile}"
              "$<TARGET_FILE_DIR:apfpv_player>/${assetFile}"
      COMMENT "assets: ${assetFile}")
  endif()
endforeach()

message(STATUS "apfpv_gs: ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}  build=${CMAKE_BUILD_TYPE}")
