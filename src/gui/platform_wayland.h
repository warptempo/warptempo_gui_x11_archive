#pragma once
#include "gui_input.h"
#include <cairo/cairo.h>
#include <functional>
#include <string>

// GuiPlatform: Wayland implementation. Mirrors the public interface of the
// X11 sibling in platform_x11.h. This is currently a stub — every method is
// defined to print a diagnostic and abort. Subsequent briefs will fill in
// window creation, the wl_display event loop, xkbcommon-driven key
// translation, wl_shm-backed Cairo surfaces, and the data-device drag/drop
// path. No Wayland headers appear here on purpose; they belong to
// platform_wayland.cpp once the real implementation lands.
class GuiPlatform {
public:
    using RedrawCallback       = std::function<void(cairo_t*, int x, int y, int w, int h)>;
    using ResizeCallback       = std::function<void(int w, int h)>;
    using KeyCallback          = std::function<void(GuiKey key, GuiInputState mods)>;
    // TODO: button parameter is still raw X11 numbering (1=left, 2=middle,
    // 3=right, 4/5=wheel). Wayland will need a neutral button enum; that's a
    // separate brief.
    using ButtonCallback       = std::function<void(unsigned int button, int x, int y, GuiInputState mods)>;
    using MotionCallback       = std::function<void(int x, int y, GuiInputState mods)>;
    using CloseCallback        = std::function<void()>;
    using FileDropCallback     = std::function<void(const std::string& path)>;
    using DropAcceptPredicate  = std::function<bool(int x, int y)>;
    using TickCallback         = std::function<void()>;
    using IdleTimeoutProvider  = std::function<int()>;

    GuiPlatform();
    ~GuiPlatform();

    bool init(int width, int height, const char* title);
    void shutdown();
    void run();
    void request_exit();
    void invalidate_region(int x, int y, int w, int h);
    void drain_events();

    int width()  const;
    int height() const;
    int playback_tick_ms() const;
    cairo_surface_t* playhead_triangle_surface() const;

    void set_on_redraw(RedrawCallback cb);
    void set_on_resize(ResizeCallback cb);
    void set_on_key(KeyCallback cb);
    void set_on_button_press(ButtonCallback cb);
    void set_on_button_release(ButtonCallback cb);
    void set_on_motion(MotionCallback cb);
    void set_on_close(CloseCallback cb);
    void set_on_file_drop(FileDropCallback cb);
    void set_drop_accept_predicate(DropAcceptPredicate p);
    void set_on_tick(TickCallback cb);
    void set_idle_timeout_provider(IdleTimeoutProvider p);

private:
};
