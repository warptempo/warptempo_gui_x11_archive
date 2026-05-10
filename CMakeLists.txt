cmake_minimum_required(VERSION 3.10)
project(warptempo_suite VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies for the DSP Engine ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(FFTW3 REQUIRED fftw3)
find_library(FFTW3_THREADS_LIB fftw3_threads HINTS ${FFTW3_LIBRARY_DIRS} REQUIRED)

# --- GUI platform backend selection ---
# Selects which GuiPlatform implementation (and which system libraries) the
# GUI links against. x11 is the default and the only fully-implemented
# backend today; wayland builds the stub in src/gui/platform_wayland.cpp,
# which aborts at the first platform call until later briefs land.
set(WARPTEMPO_BACKEND "x11" CACHE STRING
    "GUI platform backend: x11 (default) or wayland")
set_property(CACHE WARPTEMPO_BACKEND PROPERTY STRINGS "x11" "wayland")
if(NOT WARPTEMPO_BACKEND STREQUAL "x11" AND NOT WARPTEMPO_BACKEND STREQUAL "wayland")
    message(FATAL_ERROR
        "Invalid WARPTEMPO_BACKEND='${WARPTEMPO_BACKEND}'. "
        "Accepted values: x11, wayland (lowercase, exact).")
endif()

if(WARPTEMPO_BACKEND STREQUAL "x11")
    pkg_check_modules(X11 REQUIRED x11 xrandr)
    set(WARPTEMPO_PLATFORM_SOURCE       src/gui/platform_x11.cpp)
    set(WARPTEMPO_PLATFORM_INCLUDE_DIRS ${X11_INCLUDE_DIRS})
    set(WARPTEMPO_PLATFORM_LIBRARIES    ${X11_LIBRARIES})
    set(WARPTEMPO_BACKEND_DEFINE        WARPTEMPO_BACKEND_X11)
elseif(WARPTEMPO_BACKEND STREQUAL "wayland")
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
    pkg_check_modules(XKBCOMMON      REQUIRED xkbcommon)
    set(WARPTEMPO_PLATFORM_SOURCE       src/gui/platform_wayland.cpp)
    set(WARPTEMPO_PLATFORM_INCLUDE_DIRS
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS})
    # rt is linked here for shm_open, which the real wl_shm path will use
    # later; the stub doesn't call it, but linking it now means subsequent
    # briefs don't need to revisit CMakeLists.
    set(WARPTEMPO_PLATFORM_LIBRARIES
        ${WAYLAND_CLIENT_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        rt)
    set(WARPTEMPO_BACKEND_DEFINE        WARPTEMPO_BACKEND_WAYLAND)
endif()

# --- Build the GUI (Cairo + selected platform backend) ---
pkg_check_modules(CAIRO REQUIRED cairo)
# JACK is required: miniaudio is initialized with the JACK backend explicitly
# (no auto-fallback). On Arch this is satisfied by pipewire-jack.
pkg_check_modules(JACK REQUIRED jack)

add_executable(warptempo_gui
    src/gui/main.cpp
    src/gui/app_state.cpp
    ${WARPTEMPO_PLATFORM_SOURCE}
    src/gui/audio.cpp
    src/gui/render.cpp
    src/gui/warpmarkers.cpp
    src/gui/text_editor.cpp
    src/gui/text_display.cpp
    src/gui/phase_reset_markers.cpp
    src/gui/playback.cpp
    src/gui/render_pipeline.cpp
    src/gui/timemap.cpp
    src/gui/viewport.cpp
    src/gui/selection.cpp
    src/gui/undo.cpp
    src/gui/phase_reset_markers_ops.cpp
    src/gui/phase_reset_propagate.cpp
    src/gui/warpmarkers_ops.cpp
    src/gui/flag_editor.cpp
    src/gui/render_view.cpp
    src/gui/tab_mode.cpp
    src/gui/paint_handler.cpp
    src/gui/input_handler.cpp
    src/gui/miniaudio_impl.cpp
    src/engine/engine.cpp
    src/engine/limiter.cpp
    src/engine/synthesis.cpp
)
target_include_directories(warptempo_gui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/src/gui
    ${WARPTEMPO_PLATFORM_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
    ${JACK_INCLUDE_DIRS}
)
target_link_libraries(warptempo_gui PRIVATE
    ${WARPTEMPO_PLATFORM_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    ${FFTW3_THREADS_LIB}
    ${FFTW3_LIBRARIES}
    ${JACK_LIBRARIES}
    pthread
    dl
    m
)
target_compile_definitions(warptempo_gui PRIVATE ${WARPTEMPO_BACKEND_DEFINE})

# miniaudio is vendored third-party C. Suppress the warnings that its macro
# soup trips — only on its dedicated implementation TU, so our own code stays
# under -Wall -Wextra.
set_source_files_properties(src/gui/miniaudio_impl.cpp PROPERTIES
    COMPILE_FLAGS "-w")

# --- Compiler Optimizations ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(warptempo_gui PRIVATE -Wall -Wextra -O3 -march=native)
endif()
