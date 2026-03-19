cmake_minimum_required(VERSION 3.10)
project(eqmatch_analyzer VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(EBUR128 REQUIRED libebur128)

# Include directories: point to the pffft folder you cloned
include_directories(${SNDFILE_INCLUDE_DIRS} ${EBUR128_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/pffft)

# Build Executable: Compile pffft directly into eqmatch
add_executable(eqmatch 
    main.cpp 
    pffft/pffft.c
    pffft/pffft_common.c
)

# Link libraries
target_link_libraries(eqmatch PRIVATE ${SNDFILE_LIBRARIES} ${EBUR128_LIBRARIES} m) # 'm' for math lib

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eqmatch PRIVATE -Wall -Wextra -O3 -msse2) # SSE2 is good for PFFFT
endif()
