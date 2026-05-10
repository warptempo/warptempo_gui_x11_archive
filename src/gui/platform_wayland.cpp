#include "platform_wayland.h"

#include <cstdlib>
#include <iostream>

namespace {
[[noreturn]] void wayland_stub_abort(const char* method_name) {
    std::cerr << "GuiPlatform (Wayland backend): method '" << method_name
              << "' not yet implemented\n";
    std::abort();
}
}

GuiPlatform::GuiPlatform()  { wayland_stub_abort("GuiPlatform"); }
GuiPlatform::~GuiPlatform() { wayland_stub_abort("~GuiPlatform"); }

bool GuiPlatform::init(int, int, const char*)        { wayland_stub_abort("init"); }
void GuiPlatform::shutdown()                         { wayland_stub_abort("shutdown"); }
void GuiPlatform::run()                              { wayland_stub_abort("run"); }
void GuiPlatform::request_exit()                     { wayland_stub_abort("request_exit"); }
void GuiPlatform::invalidate_region(int, int, int, int) { wayland_stub_abort("invalidate_region"); }
void GuiPlatform::drain_events()                     { wayland_stub_abort("drain_events"); }

int GuiPlatform::width()  const                      { wayland_stub_abort("width"); }
int GuiPlatform::height() const                      { wayland_stub_abort("height"); }
int GuiPlatform::playback_tick_ms() const            { wayland_stub_abort("playback_tick_ms"); }
cairo_surface_t* GuiPlatform::playhead_triangle_surface() const {
    wayland_stub_abort("playhead_triangle_surface");
}

void GuiPlatform::set_on_redraw(RedrawCallback)              { wayland_stub_abort("set_on_redraw"); }
void GuiPlatform::set_on_resize(ResizeCallback)              { wayland_stub_abort("set_on_resize"); }
void GuiPlatform::set_on_key(KeyCallback)                    { wayland_stub_abort("set_on_key"); }
void GuiPlatform::set_on_button_press(ButtonCallback)        { wayland_stub_abort("set_on_button_press"); }
void GuiPlatform::set_on_button_release(ButtonCallback)      { wayland_stub_abort("set_on_button_release"); }
void GuiPlatform::set_on_motion(MotionCallback)              { wayland_stub_abort("set_on_motion"); }
void GuiPlatform::set_on_close(CloseCallback)                { wayland_stub_abort("set_on_close"); }
void GuiPlatform::set_on_file_drop(FileDropCallback)         { wayland_stub_abort("set_on_file_drop"); }
void GuiPlatform::set_drop_accept_predicate(DropAcceptPredicate) {
    wayland_stub_abort("set_drop_accept_predicate");
}
void GuiPlatform::set_on_tick(TickCallback)                  { wayland_stub_abort("set_on_tick"); }
void GuiPlatform::set_idle_timeout_provider(IdleTimeoutProvider) {
    wayland_stub_abort("set_idle_timeout_provider");
}
