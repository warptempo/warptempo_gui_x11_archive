#pragma once

// Platform backend selector. The build system (CMakeLists.txt) defines
// exactly one of WARPTEMPO_BACKEND_X11 or WARPTEMPO_BACKEND_WAYLAND on the
// warptempo_gui target, and this header pulls in the matching GuiPlatform
// declaration. Code outside the platform pair includes "platform.h"; the
// backend-specific headers are included only by this selector and by their
// own .cpp file.

#if defined(WARPTEMPO_BACKEND_X11) && defined(WARPTEMPO_BACKEND_WAYLAND)
#error "Both WARPTEMPO_BACKEND_X11 and WARPTEMPO_BACKEND_WAYLAND are defined; exactly one must be."
#elif defined(WARPTEMPO_BACKEND_X11)
#include "platform_x11.h"
#elif defined(WARPTEMPO_BACKEND_WAYLAND)
#include "platform_wayland.h"
#else
#error "No platform backend macro defined. Build must define WARPTEMPO_BACKEND_X11 or WARPTEMPO_BACKEND_WAYLAND."
#endif
