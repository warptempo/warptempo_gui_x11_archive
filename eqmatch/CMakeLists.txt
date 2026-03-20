cmake_minimum_required(VERSION 3.10)
project(eqmatch_analyzer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(EBUR128 REQUIRED libebur128)
pkg_check_modules(FFTW3 REQUIRED fftw3)

add_executable(eqmatch main.cpp)

target_include_directories(eqmatch PRIVATE 
    ${SNDFILE_INCLUDE_DIRS} 
    ${EBUR128_INCLUDE_DIRS} 
    ${FFTW3_INCLUDE_DIRS}
)

target_link_libraries(eqmatch PRIVATE 
    ${SNDFILE_LIBRARIES} 
    ${EBUR128_LIBRARIES} 
    ${FFTW3_LIBRARIES} 
    m
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eqmatch PRIVATE -Wall -Wextra -O3 -march=native)
endif()
