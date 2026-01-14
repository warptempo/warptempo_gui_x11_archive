cmake_minimum_required(VERSION 3.10)
project(midi_adapter VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependency Check: midifile ---
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/midifile/CMakeLists.txt")
    message(FATAL_ERROR "\n\n"
        "-----------------------------------------------------------------\n"
        " Error: 'midifile' library not found.\n"
        " Please run the following command inside the 'adapters/midi' folder:\n\n"
        "   git clone https://github.com/craigsapp/midifile.git\n"
        "-----------------------------------------------------------------\n"
    )
endif()

# --- FIX FOR ARCH LINUX / MODERN CMAKE ---
# The midifile library uses an old cmake_minimum_required that fails on CMake 3.27+.
# We dynamically update it to 3.10 to prevent the 'Compatibility < 3.5 removed' error.
execute_process(
    COMMAND sed -i "s/cmake_minimum_required(VERSION .*)/cmake_minimum_required(VERSION 3.10)/" 
    "${CMAKE_CURRENT_SOURCE_DIR}/midifile/CMakeLists.txt"
)

# Add the library
add_subdirectory(midifile EXCLUDE_FROM_ALL)

# --- Build Executable ---
add_executable(midi_adapter main.cpp)

target_include_directories(midi_adapter PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/midifile/include"
)

target_link_libraries(midi_adapter PRIVATE midifile)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(midi_adapter PRIVATE -Wall -O3)
endif()
