cmake_minimum_required(VERSION 3.10)
project(warptempo_stretchadapter VERSION 1.0 LANGUAGES CXX)

# Set C++ Standard (C++11 is minimum, C++17 recommended)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release build for optimization
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- Dependency Checks ---

# 1. Check for signalsmith-stretch
find_path(STRETCH_INCLUDE_DIR signalsmith-stretch.h
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-stretch
    NO_DEFAULT_PATH
)

if(NOT STRETCH_INCLUDE_DIR)
    message(FATAL_ERROR "\n\n"
        "-----------------------------------------------------------------\n"
        " Error: 'signalsmith-stretch' repository not found.\n"
        " Please run the following command in this directory:\n\n"
        "   git clone https://github.com/Signalsmith-Audio/signalsmith-stretch.git\n"
        "-----------------------------------------------------------------\n"
    )
endif()

# 2. Check for signalsmith-linear
# We look for the directory specifically because the header expects <signalsmith-linear/stft.h>
if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-linear")
    message(FATAL_ERROR "\n\n"
        "-----------------------------------------------------------------\n"
        " Error: 'signalsmith-linear' repository not found.\n"
        " Please run the following command in this directory:\n\n"
        "   git clone https://github.com/Signalsmith-Audio/linear.git signalsmith-linear\n"
        "-----------------------------------------------------------------\n"
    )
endif()

# --- Build Setup ---

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)

add_executable(warptempo_stretchadapter main.cpp)

target_include_directories(warptempo_stretchadapter PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}  # Allows resolving "signalsmith-linear/stft.h"
    ${STRETCH_INCLUDE_DIR}       # Allows resolving "signalsmith-stretch.h"
    ${SNDFILE_INCLUDE_DIRS}
)

target_link_libraries(warptempo_stretchadapter PRIVATE ${SNDFILE_LIBRARIES})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(warptempo_stretchadapter PRIVATE -Wall -Wextra -O3 -march=native)
endif()
