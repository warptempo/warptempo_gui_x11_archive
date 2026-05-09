cmake_minimum_required(VERSION 3.10)
project(warptempo_suite VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies for the DSP Engine ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(FFTW3 REQUIRED fftw3)
find_library(FFTW3_THREADS_LIB fftw3_threads HINTS ${FFTW3_LIBRARY_DIRS} REQUIRED)

# --- Build the GUI (Xlib + Cairo) ---
pkg_check_modules(X11 REQUIRED x11 xrandr)
pkg_check_modules(CAIRO REQUIRED cairo)
# JACK is required: miniaudio is initialized with the JACK backend explicitly
# (no auto-fallback). On Arch this is satisfied by pipewire-jack.
pkg_check_modules(JACK REQUIRED jack)

add_executable(warptempo_gui
    src/gui/main.cpp
    src/gui/app_state.cpp
    src/gui/x11.cpp
    src/gui/audio.cpp
    src/gui/render.cpp
    src/gui/warpmarkers.cpp
    src/gui/text_editor.cpp
    src/gui/text_display.cpp
    src/gui/transientmarkers.cpp
    src/gui/playback.cpp
    src/gui/render_pipeline.cpp
    src/gui/timemap.cpp
    src/gui/viewport.cpp
    src/gui/selection.cpp
    src/gui/undo.cpp
    src/gui/transientmarkers_ops.cpp
    src/gui/transient_propagate.cpp
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
    ${X11_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
    ${JACK_INCLUDE_DIRS}
)
target_link_libraries(warptempo_gui PRIVATE
    ${X11_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    ${FFTW3_THREADS_LIB}
    ${FFTW3_LIBRARIES}
    ${JACK_LIBRARIES}
    pthread
    dl
    m
)

# miniaudio is vendored third-party C. Suppress the warnings that its macro
# soup trips — only on its dedicated implementation TU, so our own code stays
# under -Wall -Wextra.
set_source_files_properties(src/gui/miniaudio_impl.cpp PROPERTIES
    COMPILE_FLAGS "-w")

# --- Compiler Optimizations ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(warptempo_gui PRIVATE -Wall -Wextra -O3 -march=native)
endif()
