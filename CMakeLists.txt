cmake_minimum_required(VERSION 3.10)
project(warptempo_suite VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# NOTE: Minimum 4 GB system RAM required.
# Array footprint ~1.64 GB + HPSS temporaries (~400 MB) + OS overhead + FFTW planning buffers.

# --- Dependencies for the DSP Engine ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(EBUR128 REQUIRED libebur128)
pkg_check_modules(FFTW3 REQUIRED fftw3)

# --- 1. Build the DSP Engine (Modular) ---
add_executable(warptempo_engine
    src/main.cpp
    src/phase_vocoder.cpp
    src/hpss.cpp
    src/synthesis.cpp
)
target_include_directories(warptempo_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${SNDFILE_INCLUDE_DIRS}
    ${EBUR128_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
)
target_link_libraries(warptempo_engine PRIVATE
    ${SNDFILE_LIBRARIES}
    ${EBUR128_LIBRARIES}
    ${FFTW3_LIBRARIES}
    m
)

# --- 2. Build the Timemap Parser ---
add_executable(parser src/parser.cpp)
# (Assuming the parser is pure C++ and doesn't need external audio libs. 
# If it does, we can add a target_link_libraries block for it here.)

# --- Compiler Optimizations ---
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(warptempo_engine PRIVATE -Wall -Wextra -O3 -march=native)
    target_compile_options(parser PRIVATE -Wall -Wextra -O3 -march=native)
endif()
