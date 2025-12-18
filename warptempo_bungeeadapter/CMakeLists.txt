cmake_minimum_required(VERSION 3.14)
project(warptempo_bungeeadapter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check if Bungee library exists
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bungee/CMakeLists.txt")
    message(FATAL_ERROR "\n\nBungee library not found!\nPlease run the following command inside the 'bungee_adapter' folder:\n\n    git clone --recurse-submodules https://github.com/bungee-audio-stretch/bungee.git\n")
endif()

add_subdirectory(bungee EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)

add_executable(warptempo_bungeeadapter main.cpp)

# Link against the library
target_link_libraries(warptempo_bungeeadapter PRIVATE bungee_library ${SNDFILE_LIBRARIES})

# Include directories
target_include_directories(warptempo_bungeeadapter PRIVATE 
    ${SNDFILE_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/bungee/bungee" 
)

# Optional: Add a 'Pro' flag comment for future reference
# To use Pro, one would replace the clone URL with the private Pro repo URL
