cmake_minimum_required(VERSION 3.10)
project(warptempo_suite VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies for the DSP Engine ---
find_package(OpenMP)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(FFTW3 REQUIRED fftw3)
find_library(FFTW3_THREADS_LIB fftw3_threads HINTS ${FFTW3_LIBRARY_DIRS} REQUIRED)

# --- 1. Build the DSP Engine (Modular) ---
add_executable(warptempo_engine
    src/main.cpp
    src/engine/engine.cpp
    src/engine/phase_vocoder.cpp
    src/engine/transients.cpp
    src/engine/limiter.cpp
    src/engine/synthesis.cpp
)
target_include_directories(warptempo_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine
    ${SNDFILE_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
)
target_link_libraries(warptempo_engine PRIVATE
    ${SNDFILE_LIBRARIES}
    ${FFTW3_THREADS_LIB}
    ${FFTW3_LIBRARIES}
    m
    $<$<BOOL:${OpenMP_CXX_FOUND}>:OpenMP::OpenMP_CXX>
)

# --- 2. Build the Timemap Parser ---
add_executable(parser src/parser.cpp)
# (Assuming the parser is pure C++ and doesn't need external audio libs.
# If it does, we can add a target_link_libraries block for it here.)

# --- 3. Build the GUI (Xlib + Cairo) ---
pkg_check_modules(X11 REQUIRED x11 xrandr)
pkg_check_modules(CAIRO REQUIRED cairo)

add_executable(warptempo_gui
    src/gui_main.cpp
    src/gui_x11.cpp
    src/gui_audio.cpp
    src/gui_render.cpp
    src/gui_markers.cpp
    src/gui_text_editor.cpp
    src/gui_text_display.cpp
    src/gui_transients.cpp
    src/gui_playback.cpp
    src/gui_render_pipeline.cpp
    src/timemap.cpp
    src/miniaudio_impl.cpp
    src/engine/engine.cpp
    src/engine/phase_vocoder.cpp
    src/engine/transients.cpp
    src/engine/limiter.cpp
    src/engine/synthesis.cpp
)
target_include_directories(warptempo_gui PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine
    ${X11_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
)
target_link_libraries(warptempo_gui PRIVATE
    ${X11_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    ${FFTW3_THREADS_LIB}
    ${FFTW3_LIBRARIES}
    pthread
    dl
    m
    $<$<BOOL:${OpenMP_CXX_FOUND}>:OpenMP::OpenMP_CXX>
)

# miniaudio is vendored third-party C. Suppress the warnings that its macro
# soup trips — only on its dedicated implementation TU, so our own code stays
# under -Wall -Wextra.
set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES
    COMPILE_FLAGS "-w")

# --- Compiler Optimizations ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(warptempo_engine PRIVATE -Wall -Wextra -O3 -march=native)
    target_compile_options(parser PRIVATE -Wall -Wextra -O3 -march=native)
    target_compile_options(warptempo_gui PRIVATE -Wall -Wextra -O3 -march=native)
endif()
