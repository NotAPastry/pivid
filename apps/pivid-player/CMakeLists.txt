# apps/pivid-player/CMakeLists.txt -- native desktop build (Mac / Win / Linux).
# NOT the docker cross-compile: this is the viewer, it runs on the dev machine.
#
# Dev-Mac deps (verified):   brew install sdl2 ffmpeg cairo pkg-config
# Linux (apt, untested v1):  libsdl2-dev libavformat-dev libavcodec-dev libavutil-dev libcairo2-dev
# Windows (vcpkg, untested): sdl2 ffmpeg cairo  (+ -DCMAKE_TOOLCHAIN_FILE=...vcpkg.cmake)
cmake_minimum_required(VERSION 3.16)
project(pivid_player C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

option(LATPROBE "Build the latency probes" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVUTIL   REQUIRED libavutil)
pkg_check_modules(CAIRO    REQUIRED cairo)
# (See cmake/EmbedFile.cmake for configure-time embedding details.)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN})
embed_file(${CMAKE_CURRENT_SOURCE_DIR}/assets/font_btfl_dji.png
           ${GEN}/font_btfl_dji_png.c font_btfl_dji_png)

# SDL2: prefer the CONFIG package, fall back to pkg-config.
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common)

# Source list grows through Phase-D as modules land; Task 11 wires the full player.
add_executable(pivid-player
  src/main.c
  src/player_args.c
  src/player_state.c
  src/frame_slot.c
  src/frame_gate.c
  src/video_decode.c
  src/video_decode_flags.c
  src/lan_reasm.c
  src/lan_source.c
  src/au_asm.c
  src/telem_slot.c
  src/telem_recv.c
  src/control_client.c
  src/mdns_parse.c
  src/mdns_browse.c
  src/render.c
  src/osd_glyph_cache.c
  src/osd_msp_font.c
  src/osd_grid_render.c
  ${GEN}/font_btfl_dji_png.c
  ${COMMON_DIR}/osd_grid.c
  ${COMMON_DIR}/control.c
)

target_include_directories(pivid-player PRIVATE
  src ${COMMON_DIR}
  ${AVFORMAT_INCLUDE_DIRS} ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS}
  ${CAIRO_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS}
)
target_link_libraries(pivid-player PRIVATE
  ${AVFORMAT_LIBRARIES} ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES}
  ${CAIRO_LIBRARIES} ${SDL2_LIBRARIES}
)
target_link_directories(pivid-player PRIVATE
  ${AVFORMAT_LIBRARY_DIRS} ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS}
  ${CAIRO_LIBRARY_DIRS} ${SDL2_LIBRARY_DIRS}
)

# Version stamp: git short hash, "unknown" if git is unavailable. Mirrors the
# daemon Makefiles' -DBUILD_VERSION; main.c has an "unknown" fallback #define.
execute_process(
  COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
  OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET RESULT_VARIABLE GIT_RC)
if(NOT GIT_RC EQUAL 0)
  set(GIT_HASH "unknown")
endif()
target_compile_definitions(pivid-player PRIVATE BUILD_VERSION="${GIT_HASH}")

if(LATPROBE)
  target_sources(pivid-player PRIVATE ${COMMON_DIR}/resid_hist.c)
  target_compile_definitions(pivid-player PRIVATE LATPROBE)
endif()

install(TARGETS pivid-player RUNTIME DESTINATION bin)
install(FILES assets/LICENSES.md DESTINATION share/pivid-player)
