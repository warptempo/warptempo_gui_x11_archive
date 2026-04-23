#pragma once
#include <cairo/cairo.h>
#include <X11/Xlib.h>
#include <functional>
#include <string>

// X11 + Cairo window/event-loop plumbing. Owns the display connection, the
// window, an off-screen pixmap for double-buffering, and the Cairo surface
// bound to that pixmap. Turns X11 events into application-level callbacks.
class GuiX11 {
public:
    using RedrawCallback       = std::function<void(cairo_t*, int x, int y, int w, int h)>;
    using ResizeCallback       = std::function<void(int w, int h)>;
    using KeyCallback          = std::function<void(KeySym keysym, unsigned int modifiers)>;
    using ButtonCallback       = std::function<void(unsigned int button, int x, int y, unsigned int modifiers)>;
    using MotionCallback       = std::function<void(int x, int y, unsigned int modifiers)>;
    using CloseCallback        = std::function<void()>;
    using FileDropCallback     = std::function<void(const std::string& path)>;
    using DropAcceptPredicate  = std::function<bool(int x, int y)>;

    bool init(int width, int height, const char* title);
    void shutdown();
    void run();
    void request_exit();
    void invalidate_region(int x, int y, int w, int h);

    // Process any X events already on the queue without blocking. Intended for
    // use during a long synchronous operation on the UI thread so that expose,
    // resize, and close-request events still land while work is in progress.
    void drain_events();

    int width()  const { return width_;  }
    int height() const { return height_; }

    void set_on_redraw(RedrawCallback cb)          { on_redraw_         = std::move(cb); }
    void set_on_resize(ResizeCallback cb)          { on_resize_         = std::move(cb); }
    void set_on_key(KeyCallback cb)                { on_key_            = std::move(cb); }
    void set_on_button_press(ButtonCallback cb)    { on_button_press_   = std::move(cb); }
    void set_on_button_release(ButtonCallback cb)  { on_button_release_ = std::move(cb); }
    void set_on_motion(MotionCallback cb)          { on_motion_         = std::move(cb); }
    void set_on_close(CloseCallback cb)            { on_close_          = std::move(cb); }
    void set_on_file_drop(FileDropCallback cb)     { on_file_drop_      = std::move(cb); }
    void set_drop_accept_predicate(DropAcceptPredicate p)
                                                   { drop_accept_       = std::move(p); }

private:
    Display*         dpy_      = nullptr;
    Window           win_      = 0;
    int              screen_   = 0;
    GC               gc_       = nullptr;
    Pixmap           pixmap_   = 0;
    cairo_surface_t* surface_  = nullptr;
    cairo_t*         cr_       = nullptr;
    Atom             wm_delete_window_ = 0;
    int              width_    = 0;
    int              height_   = 0;
    bool             should_exit_ = false;

    // --- XDND (drag-and-drop) protocol state ---
    Atom xdnd_aware_       = 0;
    Atom xdnd_selection_   = 0;
    Atom xdnd_enter_       = 0;
    Atom xdnd_position_    = 0;
    Atom xdnd_status_      = 0;
    Atom xdnd_leave_       = 0;
    Atom xdnd_drop_        = 0;
    Atom xdnd_finished_    = 0;
    Atom xdnd_action_copy_ = 0;
    Atom xdnd_type_list_   = 0;
    Atom uri_list_atom_    = 0;

    // Per-drag state (reset on Enter/Leave/Drop).
    Window xdnd_source_       = 0;
    bool   xdnd_offered_uris_ = false;
    int    xdnd_version_      = 0;
    bool   xdnd_last_accept_  = false;
    Time   xdnd_drop_time_    = 0;

    RedrawCallback       on_redraw_;
    ResizeCallback       on_resize_;
    KeyCallback          on_key_;
    ButtonCallback       on_button_press_;
    ButtonCallback       on_button_release_;
    MotionCallback       on_motion_;
    CloseCallback        on_close_;
    FileDropCallback     on_file_drop_;
    DropAcceptPredicate  drop_accept_;

    void recreate_buffer(int w, int h);
    void destroy_buffer();
    void blit(int x, int y, int w, int h);
    void dispatch_event(XEvent& ev);

    void init_xdnd();
    void handle_client_message(XClientMessageEvent& ev);
    void handle_selection_notify(XSelectionEvent& ev);
    void send_xdnd_status(Window source, bool accept);
    void send_xdnd_finished(Window source, bool accepted);
    bool accept_at_root(int root_x, int root_y);
};
