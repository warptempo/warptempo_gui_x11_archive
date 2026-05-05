// Single-TU implementation of miniaudio. The header is vendored in full;
// every other consumer includes the header without the implementation macro.
// Warnings are suppressed on this TU only via CMake (set_source_files_properties)
// — miniaudio.h is third-party C code that trips -Wall / -Wextra.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
