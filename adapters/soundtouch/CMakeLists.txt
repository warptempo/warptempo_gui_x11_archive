cmake_minimum_required(VERSION 3.10)
project(soundtouch_adapter)

set(CMAKE_CXX_STANDARD 17)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(SOUNDTOUCH REQUIRED soundtouch)

add_executable(soundtouch_adapter main.cpp)

target_include_directories(soundtouch_adapter PRIVATE ${SNDFILE_INCLUDE_DIRS} ${SOUNDTOUCH_INCLUDE_DIRS})
target_link_libraries(soundtouch_adapter PRIVATE ${SNDFILE_LIBRARIES} ${SOUNDTOUCH_LIBRARIES})
