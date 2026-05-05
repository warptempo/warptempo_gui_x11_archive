#include "x11.h"

#include "playhead_cursor_data.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>

#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// Decode a %HH URI escape. Returns -1 on malformed.
int hex_pair(char a, char b) {
    auto dig = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    const int ha = dig(a), hb = dig(b);
    if (ha < 0 || hb < 0) return -1;
    return (ha << 4) | hb;
}

// Decode a single file:// URI into a filesystem path. Returns empty string on
// parse error (not a file URI, malformed escape, etc.).
std::string decode_file_uri(const std::string& uri) {
    static const char prefix[] = "file://";
    const size_t plen = sizeof(prefix) - 1;
    if (uri.size() < plen || uri.compare(0, plen, prefix) != 0) return {};

    // file://host/path -- skip optional host (empty for local files).
    size_t i = plen;
    while (i < uri.size() && uri[i] != '/') i++;
    std::string out;
    out.reserve(uri.size() - i);
    while (i < uri.size()) {
        const char c = uri[i];
        if (c == '%' && i + 2 < uri.size()) {
            const int v = hex_pair(uri[i + 1], uri[i + 2]);
            if (v < 0) return {};
            out.push_back(static_cast<char>(v));
            i += 3;
        } else {
            out.push_back(c);
            i++;
        }
    }
    return out;
}

struct PngMemReader {
    const unsigned char* data;
    unsigned int         len;
    unsigned int         pos;
};

cairo_status_t png_mem_read(void* closure, unsigned char* out, unsigned int n) {
    auto* r = static_cast<PngMemReader*>(closure);
    if (r->pos + n > r->len) return CAIRO_STATUS_READ_ERROR;
    std::memcpy(out, r->data + r->pos, n);
    r->pos += n;
    return CAIRO_STATUS_SUCCESS;
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

    // One-shot sanity check: cairo's CAIRO_FORMAT_RGB24 produces BGRX
    // little-endian pixel data, which assumes a 24-bit TrueColor visual
    // with R=0xff0000, G=0xff00, B=0xff masks. Flag, don't fall back —
    // anything else is rare in 2026 but produces visibly wrong colors.
    {
        Visual* vis      = DefaultVisual(dpy_, screen_);
        const int depth  = DefaultDepth(dpy_, screen_);
        if (depth != 24 ||
            vis->c_class    != TrueColor ||
            vis->red_mask   != 0xff0000UL ||
            vis->green_mask != 0x00ff00UL ||
            vis->blue_mask  != 0x0000ffUL) {
            std::fprintf(stderr,
                "warptempo_gui: unexpected default visual "
                "(depth=%d class=%d masks=%lx/%lx/%lx); colors may be wrong\n",
                depth, vis->c_class,
                vis->red_mask, vis->green_mask, vis->blue_mask);
        }
    }

    // Brief L: detect the active display's refresh rate via XRandR and
    // size the playback idle-poll timeout to oversample vblank by 2×
    // (1000 / refresh_hz / 2). Guarantees every vblank receives at least
    // one tick worth of playhead advance, regardless of refresh rate,
    // even with timer slack. Falls back to Brief K's fixed 8 ms if any
    // XRandR call fails.
    {
        bool detected = false;
        double detected_hz = 0.0;
        XRRScreenResources* res = XRRGetScreenResources(dpy_, root);
        if (res) {
            RROutput primary = XRRGetOutputPrimary(dpy_, root);
            XRROutputInfo* oi = nullptr;
            RRCrtc crtc = None;
            if (primary != None) {
                oi = XRRGetOutputInfo(dpy_, res, primary);
                if (oi) crtc = oi->crtc;
            }
            if (crtc == None) {
                for (int i = 0; i < res->ncrtc; i++) {
                    XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy_, res, res->crtcs[i]);
                    if (!ci) continue;
                    if (ci->mode != None) {
                        crtc = res->crtcs[i];
                        XRRFreeCrtcInfo(ci);
                        break;
                    }
                    XRRFreeCrtcInfo(ci);
                }
            }
            XRRCrtcInfo* ci = (crtc != None)
                ? XRRGetCrtcInfo(dpy_, res, crtc)
                : nullptr;
            if (ci) {
                const RRMode mode_id = ci->mode;
                for (int i = 0; i < res->nmode; i++) {
                    const XRRModeInfo& m = res->modes[i];
                    if (m.id != mode_id) continue;
                    if (m.dotClock == 0 || m.hTotal == 0 || m.vTotal == 0) break;
                    const double hz =
                        static_cast<double>(m.dotClock) /
                        (static_cast<double>(m.hTotal) *
                         static_cast<double>(m.vTotal));
                    if (hz <= 0.0) break;
                    int t = static_cast<int>(1000.0 / hz / 2.0);
                    if (t < 1)  t = 1;
                    if (t > 16) t = 16;
                    playback_tick_ms_ = t;
                    detected_hz = hz;
                    detected = true;
                    break;
                }
                XRRFreeCrtcInfo(ci);
            }
            if (oi) XRRFreeOutputInfo(oi);
            XRRFreeScreenResources(res);
        }
        if (detected) {
            std::fprintf(stderr,
                "Display refresh rate detected: %.0f Hz, "
                "playback tick timeout: %d ms\n",
                detected_hz, playback_tick_ms_);
        } else {
            std::fprintf(stderr,
                "XRandR refresh detection failed, using fallback (%d ms)\n",
                playback_tick_ms_);
        }
    }

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

    init_xdnd();

    PngMemReader reader{playhead_cursor_png, playhead_cursor_png_len, 0};
    playhead_triangle_surface_ = cairo_image_surface_create_from_png_stream(
        png_mem_read, &reader);
    const cairo_status_t st = cairo_surface_status(playhead_triangle_surface_);
    if (st != CAIRO_STATUS_SUCCESS) {
        std::fprintf(stderr,
            "warptempo_gui: failed to decode embedded playhead triangle (%s)\n",
            cairo_status_to_string(st));
        return false;
    }

    XMapWindow(dpy_, win_);
    XFlush(dpy_);

    return true;
}

void GuiX11::shutdown() {
    destroy_buffer();
    if (playhead_triangle_surface_) {
        cairo_surface_destroy(playhead_triangle_surface_);
        playhead_triangle_surface_ = nullptr;
    }
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
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cr_      = cairo_create(surface_);
    image_   = XCreateImage(
        dpy_,
        DefaultVisual(dpy_, screen_),
        DefaultDepth(dpy_, screen_),
        ZPixmap,
        0,
        reinterpret_cast<char*>(cairo_image_surface_get_data(surface_)),
        w, h,
        32,
        cairo_image_surface_get_stride(surface_));
}

void GuiX11::destroy_buffer() {
    if (cr_) {
        cairo_destroy(cr_);
        cr_ = nullptr;
    }
    if (image_) {
        // Cairo owns the pixel buffer; null XImage's data pointer first so
        // XDestroyImage doesn't free cairo's memory and corrupt the heap.
        image_->data = nullptr;
        XDestroyImage(image_);
        image_ = nullptr;
    }
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
}

void GuiX11::blit(int x, int y, int w, int h) {
    if (!image_ || !dpy_) return;
    XPutImage(dpy_, win_, gc_, image_, x, y, x, y, w, h);
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

void GuiX11::init_xdnd() {
    xdnd_aware_       = XInternAtom(dpy_, "XdndAware",       False);
    xdnd_selection_   = XInternAtom(dpy_, "XdndSelection",   False);
    xdnd_enter_       = XInternAtom(dpy_, "XdndEnter",       False);
    xdnd_position_    = XInternAtom(dpy_, "XdndPosition",    False);
    xdnd_status_      = XInternAtom(dpy_, "XdndStatus",      False);
    xdnd_leave_       = XInternAtom(dpy_, "XdndLeave",       False);
    xdnd_drop_        = XInternAtom(dpy_, "XdndDrop",        False);
    xdnd_finished_    = XInternAtom(dpy_, "XdndFinished",    False);
    xdnd_action_copy_ = XInternAtom(dpy_, "XdndActionCopy",  False);
    xdnd_type_list_   = XInternAtom(dpy_, "XdndTypeList",    False);
    uri_list_atom_    = XInternAtom(dpy_, "text/uri-list",   False);

    // Advertise XDND protocol version 5 on our window. Sources read this to
    // decide whether we're drop-aware.
    const long version = 5;
    XChangeProperty(dpy_, win_,
                    xdnd_aware_, XA_ATOM, 32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&version), 1);
}

bool GuiX11::accept_at_root(int root_x, int root_y) {
    if (!drop_accept_) return false;
    int wx = 0, wy = 0;
    Window child = 0;
    Window root = RootWindow(dpy_, screen_);
    XTranslateCoordinates(dpy_, root, win_, root_x, root_y, &wx, &wy, &child);
    return drop_accept_(wx, wy);
}

void GuiX11::send_xdnd_status(Window source, bool accept) {
    XEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.display      = dpy_;
    ev.xclient.window       = source;
    ev.xclient.message_type = xdnd_status_;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = static_cast<long>(win_);
    ev.xclient.data.l[1]    = accept ? 1L : 0L;
    // No sub-region optimisation: force source to re-ask on every move so our
    // waveform-area hit test is exercised continuously.
    ev.xclient.data.l[2]    = 0;
    ev.xclient.data.l[3]    = 0;
    ev.xclient.data.l[4]    = accept ? static_cast<long>(xdnd_action_copy_) : 0L;
    XSendEvent(dpy_, source, False, NoEventMask, &ev);
    XFlush(dpy_);
}

void GuiX11::send_xdnd_finished(Window source, bool accepted) {
    XEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.display      = dpy_;
    ev.xclient.window       = source;
    ev.xclient.message_type = xdnd_finished_;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = static_cast<long>(win_);
    ev.xclient.data.l[1]    = accepted ? 1L : 0L;
    ev.xclient.data.l[2]    = accepted ? static_cast<long>(xdnd_action_copy_) : 0L;
    XSendEvent(dpy_, source, False, NoEventMask, &ev);
    XFlush(dpy_);
}

void GuiX11::handle_client_message(XClientMessageEvent& ev) {
    const Atom mt = ev.message_type;

    if (mt == xdnd_enter_) {
        xdnd_source_       = static_cast<Window>(ev.data.l[0]);
        xdnd_version_      = static_cast<int>((ev.data.l[1] >> 24) & 0xff);
        xdnd_offered_uris_ = false;
        xdnd_last_accept_  = false;

        const bool has_more = (ev.data.l[1] & 1L) != 0;
        if (has_more) {
            Atom actual_type = 0;
            int actual_format = 0;
            unsigned long nitems = 0, bytes_after = 0;
            unsigned char* prop = nullptr;
            if (XGetWindowProperty(dpy_, xdnd_source_,
                                   xdnd_type_list_, 0, 1024, False,
                                   XA_ATOM,
                                   &actual_type, &actual_format,
                                   &nitems, &bytes_after, &prop) == Success
                && prop) {
                Atom* types = reinterpret_cast<Atom*>(prop);
                for (unsigned long i = 0; i < nitems; i++) {
                    if (types[i] == uri_list_atom_) {
                        xdnd_offered_uris_ = true;
                        break;
                    }
                }
                XFree(prop);
            }
        } else {
            for (int i = 2; i <= 4; i++) {
                if (static_cast<Atom>(ev.data.l[i]) == uri_list_atom_) {
                    xdnd_offered_uris_ = true;
                    break;
                }
            }
        }
        return;
    }

    if (mt == xdnd_position_) {
        const Window source = static_cast<Window>(ev.data.l[0]);
        const int root_x = static_cast<int>((ev.data.l[2] >> 16) & 0xffff);
        const int root_y = static_cast<int>( ev.data.l[2]        & 0xffff);
        const bool accept = xdnd_offered_uris_ && accept_at_root(root_x, root_y);
        xdnd_last_accept_ = accept;
        send_xdnd_status(source, accept);
        return;
    }

    if (mt == xdnd_leave_) {
        xdnd_source_       = 0;
        xdnd_offered_uris_ = false;
        xdnd_last_accept_  = false;
        return;
    }

    if (mt == xdnd_drop_) {
        const Window source = static_cast<Window>(ev.data.l[0]);
        xdnd_drop_time_ = static_cast<Time>(ev.data.l[2]);
        if (!xdnd_last_accept_) {
            send_xdnd_finished(source, false);
            xdnd_source_       = 0;
            xdnd_offered_uris_ = false;
            xdnd_last_accept_  = false;
            return;
        }
        // Target property: reuse XdndSelection atom as the property name on
        // our window where the source will deliver the data.
        XConvertSelection(dpy_, xdnd_selection_, uri_list_atom_,
                          xdnd_selection_, win_, xdnd_drop_time_);
        return;
    }

    if (ev.format == 32 &&
        static_cast<Atom>(ev.data.l[0]) == wm_delete_window_) {
        if (on_close_) on_close_();
        return;
    }
}

void GuiX11::handle_selection_notify(XSelectionEvent& ev) {
    if (ev.property == None || ev.selection != xdnd_selection_) {
        // Source declined to provide data; tell it we're done.
        if (xdnd_source_) send_xdnd_finished(xdnd_source_, false);
        xdnd_source_       = 0;
        xdnd_offered_uris_ = false;
        xdnd_last_accept_  = false;
        return;
    }

    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy_, win_, ev.property,
                           0, 0x7fffffff, True, AnyPropertyType,
                           &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) != Success
        || !data) {
        if (xdnd_source_) send_xdnd_finished(xdnd_source_, false);
        xdnd_source_       = 0;
        xdnd_offered_uris_ = false;
        xdnd_last_accept_  = false;
        return;
    }

    std::string text(reinterpret_cast<const char*>(data), nitems);
    XFree(data);

    // Split text/uri-list on LF (handle CRLF), skip blank lines and comments.
    std::vector<std::string> uris;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r') end--;
            if (end > start) {
                std::string line = text.substr(start, end - start);
                if (line[0] != '#') uris.push_back(std::move(line));
            }
            start = i + 1;
        }
    }

    std::vector<std::string> paths;
    paths.reserve(uris.size());
    for (const auto& u : uris) {
        std::string p = decode_file_uri(u);
        if (!p.empty()) paths.push_back(std::move(p));
    }

    Window source = xdnd_source_;
    xdnd_source_       = 0;
    xdnd_offered_uris_ = false;
    xdnd_last_accept_  = false;

    if (paths.empty()) {
        if (source) send_xdnd_finished(source, false);
        return;
    }
    if (paths.size() > 1) {
        std::fprintf(stderr,
                     "warptempo_gui: multi-file drop rejected (%zu files)\n",
                     paths.size());
        if (source) send_xdnd_finished(source, false);
        return;
    }

    if (source) send_xdnd_finished(source, true);
    if (on_file_drop_) on_file_drop_(paths[0]);
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
        handle_client_message(ev.xclient);
        break;
    }
    case SelectionNotify: {
        handle_selection_notify(ev.xselection);
        break;
    }
    default:
        break;
    }
}

void GuiX11::run() {
    XEvent ev;
    const int xfd = ConnectionNumber(dpy_);
    while (!should_exit_) {
        // If there are no events already queued, wait on the X connection
        // (plus an optional idle timer so the tick callback can drive
        // playback cursor updates while input is quiet).
        if (XPending(dpy_) == 0) {
            XFlush(dpy_);
            int timeout_ms = idle_timeout_ ? idle_timeout_() : -1;
            struct pollfd pfd{};
            pfd.fd     = xfd;
            pfd.events = POLLIN;
            (void)poll(&pfd, 1, timeout_ms);
        }
        while (XPending(dpy_) > 0) {
            XNextEvent(dpy_, &ev);
            dispatch_event(ev);
            if (should_exit_) return;
        }
        if (on_tick_) on_tick_();
    }
}

void GuiX11::drain_events() {
    if (!dpy_) return;
    // Force a round-trip so any synthesized Expose events sent via
    // invalidate_region (which uses XSendEvent and only XFlush) have
    // come back into our input queue and are visible to XPending.
    // Without this, drain_events called immediately after a series of
    // invalidate_region calls can return without dispatching the
    // synthesized expose, leaving the corresponding paints deferred
    // until some later natural event arrival.
    XSync(dpy_, False);
    while (XPending(dpy_) > 0) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        dispatch_event(ev);
    }
    // Ensure the resulting XPutImage blits reach the server promptly.
    XFlush(dpy_);
}
