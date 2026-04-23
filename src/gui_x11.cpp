#include "gui_x11.h"

#include <cairo/cairo-xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int x_error_handler(Display* dpy, XErrorEvent* ev) {
    char buf[256];
    XGetErrorText(dpy, ev->error_code, buf, sizeof(buf));
    std::fprintf(stderr,
                 "X error: %s (request %u.%u, resource 0x%lx)\n",
                 buf,
                 (unsigned)ev->request_code,
                 (unsigned)ev->minor_code,
                 ev->resourceid);
    return 0;
}

} // namespace

bool GuiX11::init(int width, int height, const char* title) {
    XSetErrorHandler(x_error_handler);

    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) {
        std::fprintf(stderr,
                     "warptempo_gui: XOpenDisplay failed "
                     "(is DISPLAY set? currently \"%s\")\n",
                     std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "");
        return false;
    }

    screen_ = DefaultScreen(dpy_);
    Window root = RootWindow(dpy_, screen_);

    win_ = XCreateSimpleWindow(
        dpy_, root,
        0, 0, width, height,
        0,
        BlackPixel(dpy_, screen_),
        BlackPixel(dpy_, screen_));

    // Prevent the server from auto-clearing to the background pixel on expose
    // and XClearArea. We draw every pixel ourselves out of the pixmap.
    XSetWindowBackgroundPixmap(dpy_, win_, None);

    XStoreName(dpy_, win_, title);

    XSelectInput(dpy_, win_,
                 ExposureMask | StructureNotifyMask |
                 KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask);

    wm_delete_window_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy_, win_, &wm_delete_window_, 1);

    gc_ = XCreateGC(dpy_, win_, 0, nullptr);

    width_  = width;
    height_ = height;
    recreate_buffer(width_, height_);

    XMapWindow(dpy_, win_);
    XFlush(dpy_);

    return true;
}

void GuiX11::shutdown() {
    destroy_buffer();
    if (gc_ && dpy_) {
        XFreeGC(dpy_, gc_);
        gc_ = nullptr;
    }
    if (win_ && dpy_) {
        XDestroyWindow(dpy_, win_);
        win_ = 0;
    }
    if (dpy_) {
        XCloseDisplay(dpy_);
        dpy_ = nullptr;
    }
}

void GuiX11::request_exit() {
    should_exit_ = true;
}

void GuiX11::recreate_buffer(int w, int h) {
    destroy_buffer();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    pixmap_ = XCreatePixmap(dpy_, win_, w, h, DefaultDepth(dpy_, screen_));
    surface_ = cairo_xlib_surface_create(dpy_, pixmap_,
                                         DefaultVisual(dpy_, screen_),
                                         w, h);
    cr_ = cairo_create(surface_);
}

void GuiX11::destroy_buffer() {
    if (cr_) {
        cairo_destroy(cr_);
        cr_ = nullptr;
    }
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
    if (pixmap_ && dpy_) {
        XFreePixmap(dpy_, pixmap_);
        pixmap_ = 0;
    }
}

void GuiX11::blit(int x, int y, int w, int h) {
    if (!pixmap_ || !dpy_) return;
    XCopyArea(dpy_, pixmap_, win_, gc_, x, y, w, h, x, y);
}

void GuiX11::invalidate_region(int x, int y, int w, int h) {
    if (!dpy_ || !win_) return;
    XEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type             = Expose;
    ev.xexpose.type     = Expose;
    ev.xexpose.display  = dpy_;
    ev.xexpose.window   = win_;
    ev.xexpose.x        = x;
    ev.xexpose.y        = y;
    ev.xexpose.width    = w;
    ev.xexpose.height   = h;
    ev.xexpose.count    = 0;
    XSendEvent(dpy_, win_, False, ExposureMask, &ev);
    XFlush(dpy_);
}

void GuiX11::dispatch_event(XEvent& ev) {
    switch (ev.type) {
    case Expose: {
        const int x = ev.xexpose.x;
        const int y = ev.xexpose.y;
        const int w = ev.xexpose.width;
        const int h = ev.xexpose.height;
        if (on_redraw_ && cr_) {
            on_redraw_(cr_, x, y, w, h);
            cairo_surface_flush(surface_);
        }
        blit(x, y, w, h);
        break;
    }
    case ConfigureNotify: {
        const int nw = ev.xconfigure.width;
        const int nh = ev.xconfigure.height;
        if (nw != width_ || nh != height_) {
            width_  = nw;
            height_ = nh;
            recreate_buffer(width_, height_);
            if (on_resize_) on_resize_(width_, height_);
            invalidate_region(0, 0, width_, height_);
        }
        break;
    }
    case KeyPress: {
        KeySym keysym = XLookupKeysym(&ev.xkey, 0);
        if (on_key_) on_key_(keysym, ev.xkey.state);
        break;
    }
    case ButtonPress: {
        if (on_button_press_) {
            on_button_press_(ev.xbutton.button,
                             ev.xbutton.x, ev.xbutton.y,
                             ev.xbutton.state);
        }
        break;
    }
    case ButtonRelease: {
        if (on_button_release_) {
            on_button_release_(ev.xbutton.button,
                               ev.xbutton.x, ev.xbutton.y,
                               ev.xbutton.state);
        }
        break;
    }
    case MotionNotify: {
        if (on_motion_) {
            on_motion_(ev.xmotion.x, ev.xmotion.y, ev.xmotion.state);
        }
        break;
    }
    case ClientMessage: {
        if ((Atom)ev.xclient.data.l[0] == wm_delete_window_) {
            if (on_close_) on_close_();
        }
        break;
    }
    default:
        break;
    }
}

void GuiX11::run() {
    XEvent ev;
    while (!should_exit_) {
        XNextEvent(dpy_, &ev);
        dispatch_event(ev);
    }
}

void GuiX11::drain_events() {
    if (!dpy_) return;
    // XPending implicitly flushes the output buffer, so any invalidate_region
    // calls made just before this become visible to the server here.
    while (XPending(dpy_) > 0) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        dispatch_event(ev);
    }
    // Ensure the resulting XCopyArea blits reach the server promptly.
    XFlush(dpy_);
}
