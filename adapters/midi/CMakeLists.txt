cmake_minimum_required(VERSION 3.10)
project(midi_adapter VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependency Check: midifile ---
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/midifile/include/MidiFile.h")
    message(FATAL_ERROR "\n\n"
        "-----------------------------------------------------------------\n"
        " Error: 'midifile' library not found.\n"
        " Please run the following command inside the 'adapters/midi' folder:\n\n"
        "   git clone https://github.com/craigsapp/midifile.git\n"
        "-----------------------------------------------------------------\n"
    )
endif()

# Compile midifile sources directly — bypasses midifile's own CMakeLists.txt,
# which uses cmake_minimum_required < 3.5 (removed in CMake 3.27+).
file(GLOB MIDIFILE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/midifile/src/*.cpp")

add_executable(midi_adapter main.cpp ${MIDIFILE_SOURCES})

target_include_directories(midi_adapter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/midifile/include"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(midi_adapter PRIVATE -Wall -O3)
endif()
