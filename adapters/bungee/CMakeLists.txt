cmake_minimum_required(VERSION 3.14)
project(bungee_adapter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Check if Bungee library exists
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bungee/CMakeLists.txt")
    message(FATAL_ERROR "\n\nBungee library not found!\nPlease run the following command inside the 'adapters/bungee' folder:\n\n    git clone --recurse-submodules https://github.com/bungee-audio-stretch/bungee.git\n")
endif()

add_subdirectory(bungee EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# Versioning logic
find_package(Git)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bungee/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bungee"
        OUTPUT_VARIABLE BUNGEE_GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    set(BUNGEE_GIT_VERSION "unknown-version")
endif()

# Pass this string to C++ code
add_definitions(-DBUNGEE_VERSION="${BUNGEE_GIT_VERSION}")

add_executable(bungee_adapter main.cpp)

# Link against the library
target_link_libraries(bungee_adapter PRIVATE bungee_library ${SNDFILE_LIBRARIES})

# Include directories
target_include_directories(bungee_adapter PRIVATE 
    ${SNDFILE_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/bungee/bungee" 
)

# Optional: Add a 'Pro' flag comment for future reference
# To use Pro, one would replace the clone URL with the private Pro repo URL

