#include "platform_wayland.h"

#include <cstdlib>
#include <iostream>

// ---------------------------------------------------------------------------
// Run-loop architecture
//
// The X11 backend's run loop uses a poll() with timeout: the timeout is the
// IdleTimeoutProvider's value, sized via XRandR-detected refresh rate to
// oversample the active display's vblank by 2x (1000 / refresh_hz / 2). When
// the poll wakes — either because an X event arrived or the timeout elapsed
// — the loop drains pending X events, then invokes the per-iteration tick
// callback (which advances the playback cursor, updates the playhead column,
// and may call invalidate_region on changed pixel ranges). Paint actually
// reaches the screen via XPutImage from the cairo image surface to the X
// server, on whatever schedule X chooses to honor invalidations. This works,
// but the present-time of XPutImage is compositor-mediated and has jitter
// that the client cannot directly observe — under raw Xorg without a
// sympathetic compositor this manifests as occasional horizontal jumps in
// playhead motion. Under XWayland the compositor enforces honest frame
// timing and the horizontal jitter is absorbed; only the inherent vblank-
// quantization of vertical pixel updates remains.
//
// wl_surface.frame is a request the client makes after each commit asking
// the compositor to deliver a callback when the surface's next presentation
// is appropriate. This is a more honest paint-pacing primitive than X11
// offers: the compositor knows when it's ready to display a new frame and
// tells the client directly, rather than the client inferring it from
// refresh rate. Paint pacing on the Wayland side therefore drives off frame
// callbacks rather than off a polled timeout.
//
// The playback tick — the cadence at which the GUI queries the audio
// engine's current sample position and updates the playhead column — is a
// separate concern from paint pacing. Conflating them by driving both off
// frame callbacks would mean that whenever the compositor pauses frame
// delivery (window occlusion, throttling during heavy compositor load, the
// user dragging another window over ours), the playback cursor would freeze
// in the GUI's representation while the audio engine continues, then the
// cursor would jump on the first frame callback after the pause. That jump
// is honest in the sense that it reflects where the audio actually is, but
// it is exactly the kind of discontinuity that user-initiated change does
// not mask, and the project's principle is to mask discontinuities with
// user-initiated change rather than expose them at compositor whim.
//
// The chosen design is two cadences. The playback tick runs on a timerfd,
// polled alongside the wl_display fd in a single poll(). The timerfd's
// interval is set by the same idle-timeout-provider mechanism the X11
// backend already uses, sized to 2x vblank oversample, with the refresh
// rate detected at init time via Wayland-side facilities (xdg_output
// logical-refresh or the toplevel's preferred-mode hint, with a sensible
// default fallback). wl_surface.frame callbacks drive paint commits only —
// when a callback fires, the loop checks whether the invalidate region is
// non-empty, and if so, paints into the next wl_shm buffer and commits.
// The two cadences drift independently and re-sync at moments of user
// action (zoom, scroll, pan, marker edit) where invalidation already
// forces a full repaint and the playhead column is recomputed afresh from
// the audio engine's current sample position.
//
// This separation is structurally identical to the project's approach to
// phase coherence in the phase vocoder. The Laroche-Dolson phase vocoder
// sacrifices phase coherence at transients, accepting the discontinuity
// rather than coloring the signal with transient enhancement. The user
// manually places phase reset markers at transients, where the natural
// energy spike masks the phase reset's perceptual cost. Here, the GUI
// sacrifices continuous synchronization between paint cadence and playback
// cadence, accepting drift rather than calculating a unified clock.
// Moments of user-initiated change mask the re-sync's perceptual cost.
// Both are applications of the same minimalist principle: do not
// synthesize coherence the underlying mechanism cannot honestly provide;
// arrange for the inevitable discontinuity to occur where the user already
// expects change.
// ---------------------------------------------------------------------------

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
