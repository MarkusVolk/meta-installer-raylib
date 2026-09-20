// Yocto RootFS Updater - raylib/raygui, PLATFORM_DRM.
// No X11, no Wayland. Backend logic (mount/tar/curl/lsblk/fstab) is
// completely UI-framework-independent, see backend.hpp.

#include "raylib.h"
#include "rlgl.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

extern "C" {
#include <xf86drm.h>
#include <xf86drmMode.h>
}

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "backend.hpp"

// Activates the given virtual terminal and waits for it to become
// active. Needed because systemd's TTYPath/TTYReset/TTYVHangup/
// TTYVTDisallocate only clean up the terminal session, they don't
// determine which VT is currently active - raylib's KDSKBMODE-based
// keyboard handling on STDIN_FILENO only gets scancodes if that TTY
// is the active VT (mouse works regardless, via evdev).
static void activate_vt(int vt_number) {
    int fd = open("/dev/tty0", O_RDWR);
    if (fd < 0) return;
    ioctl(fd, VT_ACTIVATE, vt_number);
    ioctl(fd, VT_WAITACTIVE, vt_number);
    close(fd);
}

// ---------------------------------------------------------------------
// Thread-safe log/progress queue
// ---------------------------------------------------------------------

struct SharedState {
    std::mutex mutex;
    std::deque<std::string> log_lines;
    int progress_pct = 0;
    std::string status = "Ready.";
    bool worker_running = false;
    bool worker_done_signaled = false;
    bool worker_had_error = false;
    std::string worker_error;
};

static SharedState g_state;
static std::atomic<bool> g_cancel_requested{false};

// Set on ESC (window close) or after a successful install - see
// main()'s loop. Reboot itself is deferred a few seconds (see
// REBOOT_DELAY_SECONDS below) so the success message/log is actually
// visible before the screen goes dark, rather than rebooting the
// instant the pipeline finishes.
static std::atomic<bool> g_reboot_requested{false};

// Directory-URL picker (see its own drawing code further down for the
// full rationale) - replaces the old silent "auto-pick the
// lexicographically latest file" behavior for directory-style HTTP/
// HTTPS URLs (ending in "/") with an explicit choice, queued so
// multiple sources needing resolution (e.g. rootfs AND kernel both
// being directory URLs at once) get handled one at a time rather than
// all trying to show a picker simultaneously.
struct PendingDirResolution {
    std::string dir_url;
    std::string suffix;
    char* target_buf;
    size_t target_buf_size;
    std::string label; // shown in the picker, e.g. "RootFS source"
};
static std::vector<PendingDirResolution> g_pending_dir_resolutions;
static std::vector<std::string> g_dir_picker_files; // current picker's own listing
static bool g_dir_picker_active = false;
static std::string g_dir_picker_error; // set if fetching/listing failed, shown instead of a file list

// WiFi screen state - see its own drawing code further down for the
// full flow. Scanning/listing/connecting all run synchronously on the
// main thread (matching the directory-URL picker's own precedent -
// see its comments), not on the existing background worker thread
// (that one is dedicated to the actual install pipeline). This causes
// a brief, noticeable UI freeze while scanning (~3 real seconds, see
// backend::list_wifi_networks()'s own comment on why) - acceptable
// for a rarely-used, explicitly-triggered action, but worth knowing
// if it ever feels sluggish on real hardware.
struct WifiScreenState {
    bool active = false;
    std::string device;
    std::vector<backend::WifiNetworkInfo> networks;
    std::string error;              // set if device/scan/list itself failed
    bool password_prompt = false;   // true once a network needing one was clicked
    std::string selected_ssid;
    char password_buf[128] = "";
    std::string connect_error;      // set if a connect attempt itself failed
    // True for exactly one frame between the WiFi button being clicked
    // and the actual (blocking, ~3+ second) scan running - lets that
    // one frame render and actually reach the screen (a real
    // BeginDrawing/EndDrawing pair completing) showing a "Scanning..."
    // message BEFORE the blocking work starts, rather than the UI
    // just freezing with no feedback for several seconds. See the
    // main loop's own comment on where the actual scan happens once
    // this is observed set (deliberately NOT run here, or anywhere
    // during draw_ui() itself - a real screen presentation has to
    // happen in between, which only the main loop, not this struct or
    // draw_ui(), can arrange).
    bool scan_pending = false;
};
static WifiScreenState g_wifi_screen;

// Debug-only "Exit" button support (see its own button/config comments
// below) - a clean app exit that deliberately does NOT call this
// project's own reboot logic afterward, distinct from
// g_reboot_requested.
static std::atomic<bool> g_exit_requested{false};
static bool g_cfg_debug_mode = false;
// Set once at startup (see main()) via a plain access() check for
// iwctl (iwd's own client tool) - this project's own opt-in WiFi
// support (see installer-minimal-raylib.conf's own comments on the
// DISTRO_FEATURES "wifi" opt-in) means iwd/its kernel modules simply
// aren't part of an image built without it at all, so the button
// this gates needs no separate config.toml flag of its own - the
// binary's mere presence already answers "does this image have WiFi
// support built in" directly and reliably.
static bool g_wifi_available = false;
// Periodically (not every frame - each check spawns a real iwctl
// subprocess) polled connection status, used only to color the WiFi
// button itself (see its own call site's comment) - separate from
// WifiScreenState's own per-network "connected" flags, which only
// exist while that screen is actually open. Device name is cached
// once found (find_wifi_device() itself also does a subprocess call)
// rather than re-resolved on every poll.
static bool g_wifi_connected = false;
static std::string g_wifi_poll_device;
static double g_wifi_last_poll_time = -1000.0;
static const double WIFI_POLL_INTERVAL_SECONDS = 5.0;

static const double REBOOT_DELAY_SECONDS = 3.0;

static void log_msg(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    g_state.log_lines.push_back(s);
    if (g_state.log_lines.size() > 2000) g_state.log_lines.pop_front();
}

static void set_progress(long long done, long long total, const std::string& status) {
    std::lock_guard<std::mutex> lk(g_state.mutex);
    g_state.progress_pct = (total <= 0) ? 0 : (int)std::min<long long>(100, done * 100 / total);
    if (!status.empty()) g_state.status = status;
}

// ---------------------------------------------------------------------
// Simple text field focus manager: raygui needs an editMode flag per
// text box that the caller toggles - this keeps only one field
// editable at a time.
// ---------------------------------------------------------------------

static int g_active_field = -1;
static float g_form_scroll = 0.0f;
static float g_ui_scale = 1.0f; // set once in main() based on actual detected resolution
static Font g_log_font = {}; // set once in main(), used by the log panel's DrawTextEx()

// Bright lilac (same as "secondary" in apply_noctalia_dark_theme(),
// much further down this file) - reconsidered on request: darker was
// tried first for supposedly better contrast, but the person found
// the bright version actually stands out far more clearly as an
// active focus indicator specifically (a highlighted/active state
// benefits from standing OUT, not blending toward the theme's own
// baseline colors) - the general, always-visible button border
// (BUTTON BORDER_COLOR_NORMAL) is what got the darker tone instead,
// so the two stay visually distinct from each other, just swapped
// from the earlier attempt. Declared up here rather than alongside
// the other theme colors (apply_noctalia_dark_theme()'s own local
// variables, defined much later in the file) since focusable_button()/
// focusable_toggle_group()/filled_checkbox(), right below, are used
// well before that point.
static const Color g_focusRingColor = GetColor(0xa9aefeffU);

// Low-resolution rendering support: on weak/old hardware (the
// motivating case: old ARM boards driving a modern 1080p/4K TV in
// software rendering, no GPU acceleration available or worth the
// complexity), drawing the whole immediate-mode UI at the display's
// full native resolution every frame can be too slow to stay usable.
// If config.toml's "low_res_height" is set, the UI is drawn into a
// smaller offscreen RenderTexture2D at that height instead (same
// aspect ratio as the detected display mode), then upscaled with a
// single DrawTexturePro() blit to the real screen - the display stays
// at its native/preferred mode (avoiding compatibility issues some
// TVs have with unusual forced lower resolutions), but the actual
// per-widget drawing work (the expensive part in software rendering)
// only happens at the smaller internal size. g_canvas_w/h is what
// draw_ui()/draw_file_browser() treat as "the screen" - either the
// internal low-res target or the real screen, transparently.
static bool g_low_res_active = false;
static int g_canvas_w = 0, g_canvas_h = 0;
static RenderTexture2D g_low_res_target = {};
static std::vector<int> g_field_order; // rebuilt every frame, for Tab navigation

// Depth-counter wrapper around GuiDisable()/GuiEnable() - found via a
// direct user report: Tab could still reach (and activate) fields
// inside a visually grayed-out, mouse-unusable section (specifically
// the kernel image update fields when that whole section is
// disabled) - GuiDisable() only affects raygui's OWN mouse/rendering
// state, this app's own separate Tab-focus system (g_field_order/
// g_active_field) doesn't know anything about it at all, so every
// focusable_*() call kept registering itself as a Tab stop
// regardless. Needs to be a genuine depth counter, not just a bool:
// this app's own disable calls nest (e.g. the whole form already
// disabled while a worker is running, with the kernel section's own
// separate disable further inside that) - raygui's own GuiEnable()
// unconditionally clears its disabled state the moment it's called,
// so a naive bool (or even calling raygui's real GuiEnable() at the
// end of the INNER pair directly) would have incorrectly re-enabled
// mouse interaction for the rest of the form after the kernel
// section, while the outer, worker-running disable should still be
// in effect - only actually calls the real GuiEnable() once the
// count returns to zero, i.e. every push has been matched by a pop.
static int g_gui_disabled_depth = 0;
static void push_gui_disabled() { g_gui_disabled_depth++; GuiDisable(); }
static void pop_gui_disabled() {
    if (g_gui_disabled_depth > 0) g_gui_disabled_depth--;
    if (g_gui_disabled_depth == 0) GuiEnable();
}

static bool text_field(int id, Rectangle rect, char* buf, int bufsize) {
    if (g_gui_disabled_depth > 0) {
        // Still drawn (grayed out, via raygui's own STATE_DISABLED
        // rendering) but not registered as a Tab stop and not
        // editable - matches the mouse's own behavior here (can't
        // click into it either), see g_gui_disabled_depth's own
        // comment above for the full story.
        GuiTextBox(rect, buf, bufsize, false);
        return false;
    }
    g_field_order.push_back(id);
    bool editing = (g_active_field == id);
    if (GuiTextBox(rect, buf, bufsize, editing)) {
        g_active_field = editing ? -1 : id;
    }
    // Drawn on top of GuiTextBox()'s own border rather than changing
    // TEXTBOX's own BORDER_COLOR_PRESSED style property directly - on
    // request, so the border matches this app's usual lilac focus
    // ring (like every other focused control) instead of the plain
    // white it used before. Deliberately NOT done by simply changing
    // that style property though: GuiTextBox() draws its own internal
    // cursor using that exact same BORDER_COLOR_PRESSED value
    // (confirmed directly in raygui.h's own source) - changing it
    // globally would have made the cursor itself lilac too, not just
    // the border, which wasn't asked for and reads worse against this
    // theme's own dark background than the plain white it already had
    // (white was deliberately chosen for the cursor specifically -
    // see BORDER_COLOR_PRESSED's own comment elsewhere in this file
    // for that decision's full history). An extra rectangle layered
    // on top like this leaves the actual GuiTextBox()-internal
    // rendering, cursor included, completely untouched.
    if (editing) {
        DrawRectangleLinesEx(rect, 2 * g_ui_scale, g_focusRingColor);
    }
    return editing;
}

// Same as text_field(), but visually masks the entered text (WiFi
// passphrase entry - see the WiFi screen further down). GuiTextBox()
// itself has no masking support at all (confirmed directly in
// raygui.h - only the separate, self-contained GuiTextInputBox()
// modal does, via a fixed-length "stars" placeholder string swapped
// in for the real buffer - not usable here without conflicting with
// this app's own g_active_field-based Tab/click focus system, since
// that dialog keeps its own separate, static, single-instance edit-
// mode state instead). Draws GuiTextBox() completely normally first
// (real buffer, real cursor/typing/undo behavior, all unaffected),
// then paints over the rendered text with an opaque rectangle and the
// real password's own length in asterisks on top - real typing logic
// untouched, only what's visually shown differs. GuiGetFont() (not a
// separate font variable of our own) so the overlay always matches
// whatever font GuiTextBox() itself is currently rendering with.
// Verified visually via a real Xvfb-rendered pixel test: border stays
// visible (the overlay is deliberately inset by 1px on each side),
// real characters fully hidden, asterisk count matches the real
// password length.
static bool password_field(int id, Rectangle rect, char* buf, int bufsize) {
    bool editing = text_field(id, rect, buf, bufsize);
    Color base = GetColor((unsigned int)GuiGetStyle(DEFAULT, BASE_COLOR_NORMAL));
    DrawRectangle((int)rect.x + 1, (int)rect.y + 1, (int)rect.width - 2, (int)rect.height - 2, base);
    std::string stars(strlen(buf), '*');
    int pad = GuiGetStyle(TEXTBOX, TEXT_PADDING);
    int text_size = GuiGetStyle(DEFAULT, TEXT_SIZE);
    DrawTextEx(GuiGetFont(), stars.c_str(), {rect.x + pad, rect.y + rect.height / 2 - text_size / 2.0f},
               (float)text_size, 1.0f, GetColor((unsigned int)GuiGetStyle(DEFAULT, TEXT_COLOR_NORMAL)));
    // GuiTextBox() draws its own cursor internally (confirmed directly
    // in raygui.h - a filled rectangle using TEXTBOX's own
    // BORDER_COLOR_PRESSED), but this overlay's own opaque background
    // fill above completely covers it, same as it covers the real
    // password characters - found via explicit user report (cursor
    // invisible here specifically, unlike every other text field).
    // Drawn here instead, in plain white for maximum contrast against
    // this field's own dark background regardless of theme - always
    // at the END of the typed text rather than tracking the real,
    // internal cursor position raygui keeps privately (not exposed by
    // its own API at all) - a deliberate simplification: this field
    // is only ever used for typing a fresh password start-to-finish,
    // not for editing arbitrary existing text mid-string, so "cursor
    // always trails the last character" matches the only way this
    // field actually gets used in practice.
    if (editing) {
        float cursor_x = rect.x + pad + MeasureTextEx(GuiGetFont(), stars.c_str(), (float)text_size, 1.0f).x + 2;
        DrawRectangle((int)cursor_x, (int)(rect.y + rect.height / 2 - text_size / 2.0f), 2, text_size, WHITE);
    }
    return editing;
}

// Same Tab-navigable pattern as text_field(), for buttons: raygui's
// GuiButton() only responds to mouse clicks, no keyboard activation or
// focus concept at all - without this, buttons (e.g. "Browse") were
// completely unreachable via Tab, forcing mouse use for anything but
// text fields. Draws a visible focus ring and activates on Enter/Space
// when focused, in addition to normal mouse clicking.
// override_border/override_base: when given (non-nullptr each),
// BORDER_COLOR_NORMAL/BASE_COLOR_NORMAL and their own FOCUSED
// counterparts are all temporarily set to these SAME two values for
// the duration of this one button - makes hover produce no visible
// change (NORMAL and FOCUSED resolve to identical colors), while
// PRESSED is deliberately left alone (a brief flash on an actual
// click is fine - only the maybe-lingering HOVER state was the
// actual complaint). Used for the WiFi button specifically (see its
// own call site's comment on why) - every other button leaves both
// at their default (nullptr), keeping their existing hover feedback
// unchanged.
// override_border/override_base/override_text: when given
// (non-nullptr each), BORDER_COLOR_NORMAL/BASE_COLOR_NORMAL/TEXT_
// COLOR_NORMAL and their own FOCUSED counterparts are all temporarily
// set to these SAME values for the duration of this one button -
// makes hover produce no visible change (NORMAL and FOCUSED resolve
// to identical colors), while PRESSED is deliberately left alone (a
// brief flash on an actual click is fine - only the maybe-lingering
// HOVER state was the actual complaint). override_text added after
// override_border/override_base on request: the WiFi button's own
// text was still switching color on hover (DEFAULT's own TEXT_COLOR_
// FOCUSED, onPrimary - dark navy, built to read on a light/yellow
// hover background) even after its border/background were already
// pinned - that dark-navy text just wasn't readable against this
// button's own now-unchanging (lilac or, when connected, peach)
// background either. Used for the WiFi button specifically (see its
// own call site's comment on why) - every other button leaves all
// three at their default (nullptr), keeping their existing hover
// feedback unchanged.
static bool focusable_button(int id, Rectangle rect, const char* text,
                              const Color* override_border = nullptr, const Color* override_base = nullptr,
                              const Color* override_text = nullptr, bool no_border = false) {
    if (g_gui_disabled_depth > 0) {
        // Same reasoning as text_field()'s own identical check - see
        // g_gui_disabled_depth's own comment. Checked FIRST, before
        // any color-override application below - color overrides
        // are pointless here anyway (raygui's own STATE_DISABLED
        // rendering ignores BASE/BORDER_COLOR_NORMAL regardless), and
        // applying-then-never-restoring them would otherwise leave
        // them stuck for whatever gets drawn next.
        GuiButton(rect, text);
        return false;
    }
    int prev_border_n = 0, prev_border_f = 0, prev_base_n = 0, prev_base_f = 0, prev_text_n = 0, prev_text_f = 0;
    if (override_border || override_base || override_text) {
        prev_border_n = GuiGetStyle(BUTTON, BORDER_COLOR_NORMAL);
        prev_border_f = GuiGetStyle(BUTTON, BORDER_COLOR_FOCUSED);
        prev_base_n = GuiGetStyle(BUTTON, BASE_COLOR_NORMAL);
        prev_base_f = GuiGetStyle(BUTTON, BASE_COLOR_FOCUSED);
        prev_text_n = GuiGetStyle(BUTTON, TEXT_COLOR_NORMAL);
        prev_text_f = GuiGetStyle(BUTTON, TEXT_COLOR_FOCUSED);
        if (override_border) {
            int c = ColorToInt(*override_border);
            GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, c);
            GuiSetStyle(BUTTON, BORDER_COLOR_FOCUSED, c);
        }
        if (override_base) {
            int c = ColorToInt(*override_base);
            GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, c);
            GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, c);
        }
        if (override_text) {
            int c = ColorToInt(*override_text);
            GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, c);
            GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, c);
        }
    }
    // On request: removes each row's own individual border entirely
    // for borderless list-style rows (file browser, WiFi network
    // list, directory-URL picker) - not just a cosmetic preference.
    // Each row already draws its own full-width border on top of the
    // row right above/below it (adjacent borders touching/nearly
    // overlapping), and the focus ring (further down) already adds
    // its own, thicker 2px border on top of that again when Tab-
    // focused - simpler, and one less thing for the software
    // rasterizer to draw/clip precisely at every row's own edge, to
    // just not draw this per-row border at all. The background color
    // change (hover/pressed) and the focus ring below still make the
    // current/focused row clear without it.
    int prev_border_width = 0;
    if (no_border) {
        prev_border_width = GuiGetStyle(BUTTON, BORDER_WIDTH);
        GuiSetStyle(BUTTON, BORDER_WIDTH, 0);
    }
    g_field_order.push_back(id);
    bool has_focus = (g_active_field == id);
    bool clicked = GuiButton(rect, text);
    if (no_border) {
        GuiSetStyle(BUTTON, BORDER_WIDTH, prev_border_width);
    }
    if (override_border || override_base || override_text) {
        GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, prev_border_n);
        GuiSetStyle(BUTTON, BORDER_COLOR_FOCUSED, prev_border_f);
        GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, prev_base_n);
        GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, prev_base_f);
        GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, prev_text_n);
        GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, prev_text_f);
    }
    if (has_focus) {
        // Same lilac (secondary, 0xa9aefe - see apply_noctalia_dark_theme())
        // as the button's own normal-state border now uses, not the
        // originally hardcoded SKYBLUE, then changed to the same
        // lilac as the button's own normal-state border (see BUTTON
        // BORDER_COLOR_NORMAL further down) - but that made the two
        // indistinguishable at first (found via explicit user
        // report: no longer visible when Tab moves focus between
        // buttons, since ring and border were now the same color).
        // Switched to mint green briefly, then explicitly reverted
        // back to lilac on request - the person confirmed they're
        // okay with keyboard-only navigation relying on the ring's
        // greater thickness (2px vs the plain border's own default
        // BORDER_WIDTH) plus raygui's own mouse-hover background
        // color change for the mouse case, rather than a distinct
        // ring color. "secondary" itself isn't reachable here (a
        // local in that other function), so the same raw hex value
        // is used directly, matching this file's existing pattern for
        // one-off color references.
        DrawRectangleLinesEx(rect, 2 * g_ui_scale, g_focusRingColor);
        if (!GuiIsLocked() && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE))) clicked = true;
    }
    return clicked;
}

// Same reasoning as focusable_button() above - GuiToggleGroup (via
// GuiToggle internally) has the identical gap: mouse-only, no
// keyboard focus/activation at all. Every toggle group in this app
// has exactly two options, so Left/Right (or Up/Down, same effect)
// simply flips between 0 and 1 when focused - no need to parse the
// semicolon-separated item text to generalize beyond that.
// Rebuilt to make each individual option its own Tab stop (id_base*10
// + option index), not the whole group as a single stop - found via a
// direct follow-up report: after the earlier fix made arrow-key
// cycling correctly reach every option once the group itself had Tab
// focus (previously broken for 3+ option groups specifically, see
// this function's own git history/comments elsewhere), Tab alone
// still only ever reached the first/currently-active option, jumping
// straight to the NEXT field afterward - arrow keys were still
// required to reach the others at all. Mirrors GuiToggleGroup()'s own
// internal per-item layout math exactly (bounds.x = initBoundsX +
// col*(bounds.width + GROUP_PADDING), confirmed directly in its own
// real source) via individual GuiToggle() calls instead of one single
// GuiToggleGroup() call, each with its own focus ring and Enter/Space
// activation - same pattern focusable_button() already uses, just
// looped per option. "rect" is still the PER-ITEM bounds (its own
// width, not the whole group's), matching every existing call site's
// own already-correct per-item width calculation - unchanged from
// before.
static void focusable_toggle_group(int id_base, Rectangle rect, const char* text, int* mode) {
    std::vector<std::string> options;
    std::string current;
    for (const char* p = text; ; p++) {
        if (*p == ';' || *p == '\0') {
            options.push_back(current);
            current.clear();
            if (*p == '\0') break;
        } else {
            current += *p;
        }
    }
    float padding = (float)GuiGetStyle(TOGGLE, GROUP_PADDING);
    for (int i = 0; i < (int)options.size(); i++) {
        Rectangle item_rect = {rect.x + i * (rect.width + padding), rect.y, rect.width, rect.height};
        int this_id = id_base * 10 + i;
        bool toggle_state = (*mode == i);
        if (g_gui_disabled_depth > 0) {
            // Same reasoning as text_field()'s own identical check -
            // see g_gui_disabled_depth's own comment.
            GuiToggle(item_rect, options[i].c_str(), &toggle_state);
            continue;
        }
        g_field_order.push_back(this_id);
        bool has_focus = (g_active_field == this_id);
        // Same "set state going in, check if GuiToggle() itself
        // flipped it, then commit back to *mode" pattern
        // GuiToggleGroup() uses internally for its own toggle/*active
        // interplay (confirmed directly in its own real source) -
        // keeps a mouse click behaving identically to before.
        GuiToggle(item_rect, options[i].c_str(), &toggle_state);
        if (toggle_state) *mode = i;
        if (has_focus) {
            // Same lilac focus ring as focusable_button() - see its
            // own comment on the thickness-not-color contrast
            // decision.
            DrawRectangleLinesEx(item_rect, 2 * g_ui_scale, g_focusRingColor);
            if (!GuiIsLocked() && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE))) {
                *mode = i;
            }
        }
    }
}

// GuiCheckBox only fills its interior when *checked is true (a hollow
// outline otherwise, BLANK fill). Against our dark app background,
// that "empty" interior blends in and is barely noticeable normally -
// but once the border highlights brightly on hover (by design, our
// theme's BORDER_COLOR_FOCUSED), the unchanged-but-now-strongly-
// contrasted dark interior reads as an unwanted "dark activated" fill
// (real hardware report). Draw a subtle background first so the box
// always has a consistent, solid appearance, independent of
// checked/hover state.
static void filled_checkbox(int id, Rectangle rect, const char* text, bool* checked) {
    DrawRectangleRec(rect, GetColor(GuiGetStyle(DEFAULT, BASE_COLOR_NORMAL)));
    if (g_gui_disabled_depth > 0) {
        // Same reasoning as text_field()'s own identical check - see
        // g_gui_disabled_depth's own comment.
        GuiCheckBox(rect, text, checked);
        return;
    }
    g_field_order.push_back(id);
    bool has_focus = (g_active_field == id);
    GuiCheckBox(rect, text, checked);
    if (has_focus) {
        // Lilac, not mint green - see focusable_button()'s own
        // comment on the thickness-not-color contrast decision.
        DrawRectangleLinesEx(rect, 2 * g_ui_scale, g_focusRingColor);
        if (!GuiIsLocked() && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE))) *checked = !*checked;
    }
}

// Tab/Shift+Tab moves to the next/previous field in the order
// text_field() was called this frame.
static void handle_tab_navigation() {
    if (g_field_order.empty() || !IsKeyPressed(KEY_TAB)) return;
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    int n = (int)g_field_order.size();
    int idx = -1;
    for (int i = 0; i < n; i++) if (g_field_order[i] == g_active_field) { idx = i; break; }
    int next = shift ? (idx - 1) : (idx + 1);
    next = ((next % n) + n) % n;
    g_active_field = g_field_order[next];
}

// ---------------------------------------------------------------------
// Simple file/directory browser
// ---------------------------------------------------------------------

struct FileBrowserState {
    bool open = false;
    std::string current_dir = "/";
    std::vector<std::pair<std::string, bool>> entries; // name, is_dir
    float scroll_offset = 0; // for manual_scroll_begin(), see draw_file_browser()
    char* target_buf = nullptr;
    size_t target_buf_size = 0;
    std::vector<std::string> allowed_extensions; // empty = no filter, show everything
    std::vector<std::string> excluded_extensions; // applied in addition to allowed_extensions
    // Substring match against the filename (case-insensitive), empty
    // = no filter. Added for kernel image files specifically - they
    // have no reliable extension to filter by (bzImage/zImage/Image/
    // uImage/vmlinuz/... vary by architecture, see the comment on
    // find_latest_matching_file()'s own kernel-specific caller further
    // down), so extension-based filtering alone (allowed_extensions
    // above) can't narrow the list down the way it does for rootfs/wic
    // - previously this meant the kernel browse dialog showed every
    // file in a directory, found via a direct user report. Matches
    // AppState::kernel_target_name (e.g. "bzImage") as a substring
    // rather than requiring an exact match - real Yocto deploy
    // directories commonly have several related filenames sharing
    // that same core name (versioned/suffixed variants, a stable
    // symlink alongside the real file, etc.), all reasonable browsing
    // candidates a person might actually want to pick from.
    std::string name_filter;
    bool show_all = false; // escape hatch: bypass both filters (e.g. an unusually-named file)

    static bool has_extension(const std::string& name, const std::vector<std::string>& exts) {
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        for (auto& ext : exts) {
            if (lower.size() >= ext.size() &&
                lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
                return true;
            }
        }
        return false;
    }

    void refresh() {
        entries.clear();
        DIR* d = opendir(current_dir.c_str());
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                std::string name = e->d_name;
                if (name == "." || name == "..") continue;
                std::string full = current_dir;
                if (full.back() != '/') full += "/";
                full += name;
                struct stat st{};
                bool is_dir = (stat(full.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
                // Directories are always shown (still need to navigate
                // through them to reach a matching file elsewhere).
                // Files: allowed_extensions is a whitelist (e.g. only
                // *.wic when browsing for a wic image) where one
                // exists; excluded_extensions is a blacklist for cases
                // with no reliable single whitelist (e.g. kernel image
                // filenames vary too much - bzImage/zImage/Image/
                // uImage/vmlinuz/... - but files clearly meant for the
                // OTHER two modes, *.wic/*.tar.gz, can still be safely
                // excluded without risking a false-negative on a real
                // kernel file); name_filter narrows further still by
                // substring match against kernel_target_name
                // specifically (see this field's own comment above).
                // show_all bypasses all three.
                if (!is_dir && !show_all) {
                    if (!allowed_extensions.empty() && !has_extension(name, allowed_extensions)) continue;
                    if (!excluded_extensions.empty() && has_extension(name, excluded_extensions)) continue;
                    if (!name_filter.empty()) {
                        std::string lower_name = name, lower_filter = name_filter;
                        for (auto& c : lower_name) c = (char)tolower((unsigned char)c);
                        for (auto& c : lower_filter) c = (char)tolower((unsigned char)c);
                        if (lower_name.find(lower_filter) == std::string::npos) continue;
                    }
                }
                entries.push_back({name, is_dir});
            }
            closedir(d);
        }
        std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        scroll_offset = 0;
    }

    void open_for(char* buf, size_t size, const char* start_dir, std::vector<std::string> exts = {},
                  std::vector<std::string> excl = {}, std::string name_flt = "") {
        target_buf = buf;
        target_buf_size = size;
        current_dir = start_dir;
        allowed_extensions = std::move(exts);
        excluded_extensions = std::move(excl);
        name_filter = std::move(name_flt);
        show_all = false; // reset each time the browser is (re)opened
        refresh();
        open = true;
    }

    void navigate(int idx) {
        if (idx == 0) { // ".."
            size_t slash = current_dir.find_last_of('/');
            current_dir = (slash == 0 || slash == std::string::npos) ? "/" : current_dir.substr(0, slash);
            if (current_dir.empty()) current_dir = "/";
            refresh();
            return;
        }
        int entry_idx = idx - 1;
        if (entry_idx < 0 || entry_idx >= (int)entries.size()) return;
        auto& [name, is_dir] = entries[entry_idx];
        std::string full = current_dir;
        if (full.back() != '/') full += "/";
        full += name;
        if (is_dir) {
            current_dir = full;
            refresh();
        } else if (target_buf) {
            strncpy(target_buf, full.c_str(), target_buf_size - 1);
            target_buf[target_buf_size - 1] = 0;
            open = false;
        }
    }
};

// Scans a directory (non-recursive, same scope as the file browser
// itself) for files matching the given extensions and returns the
// most recently modified one - used to pre-fill the "Local File"
// path fields automatically when a matching rootfs.tar.gz/.wic
// already sits in the default browser directory (e.g. /mnt/storage),
// so the common case needs no manual browsing at all. Falls back to
// showing nothing (empty string) if the directory can't be read or
// nothing matches - never a hard error, just no auto-fill.
static std::string find_latest_matching_file(const std::vector<std::string>& dirs, const std::vector<std::string>& exts) {
    std::string best;
    time_t best_mtime = 0;
    for (auto& dir : dirs) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            if (!FileBrowserState::has_extension(name, exts)) continue;
            std::string full = dir;
            if (full.back() != '/') full += "/";
            full += name;
            struct stat st{};
            if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (best.empty() || st.st_mtime > best_mtime) {
                best = full;
                best_mtime = st.st_mtime;
            }
        }
        closedir(d);
    }
    return best;
}

// Kernel images have no reliable extension to filter by (bzImage,
// zImage, Image, uImage, ... vary by architecture - deliberately not
// covered by find_latest_matching_file() above, see its own call
// sites' comments), so an extension-based auto-select like rootfs/wic
// get doesn't work here. Yocto's own deploy directory convention
// helps instead: a STABLE symlink under tmp/deploy/images/<machine>/
// named exactly after the kernel image type (typically "bzImage" for
// x86 - this project's own kernel_target_name config value already
// tracks that same name, since it's also what gets written onto the
// target's boot partition). Looks for an EXACT filename match rather
// than a "latest by mtime" scan - there's normally only one such
// stable-symlink name to find per directory anyway.
// Kernel that belongs to a given rootfs/wic file: "<name>-image.tar.gz"
// (or .tgz/.wic) next to "<name>-<kernel_target_name>", the layout a
// shared images directory with one link pair per image uses. Empty
// if no such sibling exists.
static std::string sibling_kernel_for(const std::string& image_path, const std::string& kernel_target) {
    size_t slash = image_path.rfind('/');
    std::string dir = slash == std::string::npos ? "" : image_path.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? image_path : image_path.substr(slash + 1);
    for (const char* ext : {".rootfs.tar.gz", ".rootfs.wic", ".tar.gz", ".tgz", ".wic"}) {
        size_t n = strlen(ext);
        if (name.size() > n && name.compare(name.size() - n, n, ext) == 0) {
            name.resize(name.size() - n);
            break;
        }
    }
    const std::string suffix = "-image";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        name.resize(name.size() - suffix.size());
    std::string candidate = dir + name + "-" + kernel_target;
    struct stat st{};
    if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return candidate;
    return "";
}

static std::string find_exact_named_file(const std::vector<std::string>& dirs, const std::string& filename) {
    for (auto& dir : dirs) {
        std::string full = dir;
        if (full.back() != '/') full += "/";
        full += filename;
        struct stat st{};
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return full;
    }
    return "";
}

// Replaces GuiWindowBox() everywhere in this app - on request, after
// noting every dialog already has its own explicit Cancel/Close/Back
// button, making the title bar's "x" icon purely redundant at best -
// and at worst outright broken: two of the six real call sites this
// project had (the brief "Scanning for networks..." screen and the
// WiFi password prompt) never actually checked GuiWindowBox()'s own
// return value at all, leaving that icon visibly clickable but
// completely inert there. The other four DID wire it up (matching the
// explicit button's own action) - real, if small, duplicated
// boilerplate at every single one of them. Removing the icon
// entirely resolves both issues at once, rather than fixing the two
// broken ones and leaving the duplication in the other four.
//
// Mirrors GuiWindowBox()'s own real layout math exactly (confirmed
// directly in raygui.h's own source): a GuiPanel() body plus a
// GuiStatusBar() header strip, statusBarHeight=24 (unscaled, matching
// RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT's own real default) - just without
// the close button it also draws. Visually identical apart from that
// one icon's absence.
static void draw_window_box(Rectangle bounds, const char* title) {
    const float statusBarHeight = 24;
    float statusBorderWidth = (float)GuiGetStyle(STATUSBAR, BORDER_WIDTH);
    if (bounds.height < statusBarHeight * 2.0f) bounds.height = statusBarHeight * 2.0f;
    Rectangle statusBar = {bounds.x, bounds.y, bounds.width, statusBarHeight};
    Rectangle windowPanel = {bounds.x, bounds.y + statusBarHeight - statusBorderWidth,
                              bounds.width, bounds.height - statusBarHeight + statusBorderWidth};
    GuiPanel(windowPanel, nullptr);
    int prevAlign = GuiGetStyle(STATUSBAR, TEXT_ALIGNMENT);
    GuiSetStyle(STATUSBAR, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiStatusBar(statusBar, title);
    GuiSetStyle(STATUSBAR, TEXT_ALIGNMENT, prevAlign);
}

static int round_for_scissor(float v) { return (int)std::lround(v); }

// Replaces GuiScrollPanel() entirely for this project's own bordered,
// vertically-scrolling content areas (network/file lists, the log
// panel) - built from scratch on request, after repeated real-
// hardware reports of a persistent, hard-to-pin-down gap/overlap bug
// in that same dialog despite several rounds of independently-
// verified fixes to the surrounding size calculations. Direct causes
// already found and fixed along the way (GuiScrollPanel()'s own
// 2*BORDER_WIDTH bounds-to-viewport subtraction, a stale non-
// reconstructed list_h after box_h capping) - but the bug kept
// recurring in new forms each time, strongly suggesting some further
// GuiScrollPanel()-internal quirk not yet identified rather than
// another one-off calculation slip. This sidesteps the question
// entirely: no GuiScrollPanel() call anywhere in this function, no
// returned "view" rectangle whose relationship to the requested
// bounds has to be reasoned about or re-verified - the SAME bounds
// rectangle passed in is both what gets drawn as the border AND what
// gets scissor-clipped, by construction, so the two can never
// disagree with each other the way they kept doing before.
//
// bounds: the fixed, visible viewport (its own border drawn here).
// content_h: total height of everything that WOULD be drawn inside,
// unclipped - typically item_count * (item_height + item_spacing).
// offset: persistent scroll position in pixels (0 = scrolled to top),
// owned by the caller (a simple static float at the call site, same
// pattern this project's own GuiScrollPanel() calls already used for
// their own Vector2 scroll state) - clamped in place here to
// [0, max(0, content_h - bounds.height)] every call, so it can never
// end up stuck at a stale, now-out-of-range value if content_h
// shrinks (e.g. fewer networks after a rescan).
// Returns the Y coordinate the caller's own row-drawing loop should
// start from - already scissor-clipped (BeginScissorMode() active on
// return; the caller must call EndScissorMode() once done drawing).
static Rectangle g_scroll_bar_bounds = {};
static float g_scroll_bar_content_h = 0, g_scroll_bar_offset = 0, g_scroll_bar_max_offset = 0;

// Pair of manual_scroll_begin(): ends the scissor region and then
// draws the scrollbar indicator OVER the rows the caller drew in
// between. Thin, thumb only (no track), in the same lilac as the
// focus ring - on request, after an earlier version (twice as wide,
// thumb in the plain border color, with a dark track behind it) read
// as a "strange blue bar" in a real report rather than as a
// scrollbar, especially with content overflowing only slightly (the
// thumb then nearly spans the whole list). Purely visual, not
// draggable - scrolling is mouse wheel (in manual_scroll_begin()) and
// Tab auto-scroll in the callers. Only drawn when there's genuinely
// something to scroll.
static void manual_scroll_end() {
    EndScissorMode();
    if (g_scroll_bar_max_offset > 0.5f) {
        const Rectangle& b = g_scroll_bar_bounds;
        float bar_w = 3 * g_ui_scale;
        float thumb_h = std::max(b.height * b.height / g_scroll_bar_content_h, 20 * g_ui_scale);
        float thumb_y = b.y + (g_scroll_bar_offset / g_scroll_bar_max_offset) * (b.height - thumb_h);
        DrawRectangleRec({b.x + b.width - bar_w - 2 * g_ui_scale, thumb_y, bar_w, thumb_h}, g_focusRingColor);
    }
}

static float manual_scroll_begin(Rectangle bounds, float content_h, float& offset) {
    DrawRectangleLinesEx(bounds, (float)GuiGetStyle(DEFAULT, BORDER_WIDTH),
                          GetColor((unsigned int)GuiGetStyle(DEFAULT, BORDER_COLOR_NORMAL)));
    if (CheckCollisionPointRec(GetMousePosition(), bounds)) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0) offset -= wheel * 40 * g_ui_scale;
    }
    float max_offset = content_h - bounds.height;
    if (max_offset < 0) max_offset = 0;
    if (offset < 0) offset = 0;
    if (offset > max_offset) offset = max_offset;
    // Scrollbar drawing itself lives in manual_scroll_end() so it
    // lands ON TOP of the rows the caller draws in between - drawn
    // here, before the rows, it was only visible in the gaps between
    // them. Just remember what it needs.
    g_scroll_bar_bounds = bounds;
    g_scroll_bar_content_h = content_h;
    g_scroll_bar_offset = offset;
    g_scroll_bar_max_offset = max_offset;
    // No inflated/asymmetric margin anymore - several rounds of that
    // (a small top-only nudge, then a large uniform margin, then an
    // asymmetric top/bottom split) were all workarounds for a real
    // bug that's now actually fixed at its own root: raylib's own
    // software rasterizer (src/external/rlsw.h, used whenever this
    // project's own default build - opengl off - selects
    // GRAPHICS_API_OPENGL_SOFTWARE) had a genuine scissor Y double-
    // flip bug in swScissor() itself, confirmed by compiling and
    // running that exact rasterizer directly (raylib's own
    // PLATFORM=Memory + OPENGL_VERSION=Software, no real DRM/KMS
    // needed) and fixed via this layer's own patch
    // (recipes-graphics/raylib/files/0006-software-fix-scissor-Y-
    // double-flip-causing-scissore.patch) - see that patch's own
    // commit message for the full story. With the real cause patched,
    // scissor bounds can go back to being exactly "bounds" itself,
    // same as this function's own original design.
    BeginScissorMode(round_for_scissor(bounds.x), round_for_scissor(bounds.y),
                      round_for_scissor(bounds.width), round_for_scissor(bounds.height));
    return bounds.y - offset;
}



static void draw_file_browser(FileBrowserState& st) {
    if (!st.open) return;
    // Scoped to just this dialog's own controls while open - same
    // reasoning as the confirm dialog and WiFi password screen's own
    // g_field_order.clear() calls (see their own comments): without
    // this, Tab would also cycle through the background form's own
    // fields (still registered this same frame, just hidden behind
    // this modal's overlay) - landing focus somewhere the person
    // can't see or interact with, which is what a real report of
    // "Tab doesn't work" in this dialog specifically turned out to
    // be caused by.
    g_field_order.clear();
    float sw = (float)g_canvas_w, sh = (float)g_canvas_h;
    Rectangle win = {sw / 2 - 320 * g_ui_scale, sh / 2 - 260 * g_ui_scale, 640 * g_ui_scale, 520 * g_ui_scale};
    // Clamped against the actual screen size - found via a direct
    // real-hardware photo showing the dialog's own title bar and
    // first row visually squeezed/overlapping near the top of the
    // screen. Root cause: this dialog's own size is a fixed
    // "640 x 520 * g_ui_scale" with no check at all against the
    // actual screen height - on a screen short enough (relative to
    // g_ui_scale) that this doesn't comfortably fit, "sh/2 -
    // 260*g_ui_scale" goes negative, pushing the dialog (title bar
    // included) partially above y=0, off the actual visible screen -
    // not a scissor/scroll-offset bug at all, the content itself was
    // being asked to render above the visible area. Caps the
    // dialog's own height first if the screen itself is shorter than
    // that fixed size, then clamps the position into the remaining
    // visible range.
    if (win.height > sh) win.height = sh;
    if (win.y < 0) win.y = 0;
    if (win.y + win.height > sh) win.y = sh - win.height;
    DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.5f));
    draw_window_box(win, st.current_dir.c_str());
    Rectangle list_rect = {win.x + 10 * g_ui_scale, win.y + 36 * g_ui_scale,
                            win.width - 20 * g_ui_scale, win.height - 104 * g_ui_scale};

    // Escape hatch for the extension whitelist below: only shown when
    // a filter is actually active (no point offering it otherwise,
    // e.g. when browsing for a kernel file, which has no single
    // expected extension).
    if (!st.allowed_extensions.empty() || !st.excluded_extensions.empty() || !st.name_filter.empty()) {
        bool prev_show_all = st.show_all;
        filled_checkbox(230, {win.x + 10 * g_ui_scale, win.y + win.height - 44 * g_ui_scale, 22 * g_ui_scale, 22 * g_ui_scale},
                    "Show all files", &st.show_all);
        if (st.show_all != prev_show_all) st.refresh();
    }

    // GuiListView() replaced entirely with the same per-row
    // focusable_button()+manual_scroll_begin() pattern already used
    // for the WiFi network list and directory picker - on request, so
    // every single entry becomes its own individual Tab stop (each
    // reachable/activatable with Tab+Enter alone, matching how those
    // two dialogs already worked) rather than the whole list being
    // one Tab stop with a SEPARATE, GuiListView()-internal notion of
    // "the highlighted item" that Tab itself couldn't move at all -
    // arrow keys (see the previous version of this function) were the
    // only way to change the selection with a keyboard, Tab could only
    // ever jump PAST the entire list in one step. Also sidesteps
    // GuiListView()'s own "selected" highlight color being visually
    // identical to a button's own PRESSED-state color (BASE_COLOR_
    // PRESSED for both, confirmed directly in raygui's own source) -
    // a real source of confusion in an earlier report - since
    // focusable_button()'s own, separate lilac focus ring is used
    // here instead, already visually distinct everywhere else in this
    // app.
    //
    // Row ids start at 700 (i.e. 700 + index into the "..", then
    // entries[] list) - a fresh, previously-unused range in this file,
    // distinct from the directory-URL-picker's own 300+i (a different,
    // separate dialog, even though the two are never open at the same
    // time in practice, using a clearly different range avoids any
    // confusion when reading this code later).
    int item_count_total = (int)st.entries.size() + 1; // +1 for ".."
    float row_h = 30 * g_ui_scale, row_gap = 4 * g_ui_scale;
    float row_unit = row_h + row_gap;
    float content_h = (float)item_count_total * row_unit;
    // Auto-scroll to keep whichever row currently has Tab focus
    // visible - without this, Tab could silently move focus to a row
    // scrolled out of view, defeating the entire point of making each
    // row individually reachable this way. Checked BEFORE this
    // dialog's own manual_scroll_begin() call so the adjusted offset
    // takes effect the same frame focus lands on a newly-focused row,
    // not one frame late.
    if (g_active_field >= 700 && g_active_field < 700 + item_count_total) {
        int focused_row = g_active_field - 700;
        int visible = (int)(list_rect.height / row_unit);
        float focused_row_top = focused_row * row_unit;
        if (focused_row_top < st.scroll_offset) st.scroll_offset = focused_row_top;
        else if (focused_row_top + row_unit > st.scroll_offset + visible * row_unit)
            st.scroll_offset = focused_row_top + row_unit - visible * row_unit;
    }
    float fy = manual_scroll_begin(list_rect, content_h, st.scroll_offset);
    for (int i = 0; i < item_count_total; i++) {
        std::string label = (i == 0) ? ".." :
            (st.entries[i - 1].second ? ("[" + st.entries[i - 1].first + "]") : st.entries[i - 1].first);
        if (focusable_button(700 + i, {list_rect.x, fy, list_rect.width - 4, row_h}, label.c_str(), nullptr, nullptr, nullptr, /*no_border=*/true)) {
            st.navigate(i);
            // navigate() may have called refresh() (new directory:
            // st.entries/item_count_total are now stale) or closed
            // the dialog outright (file chosen: st.open now false) -
            // either way, continuing this same loop against the old
            // entries afterward would be wrong.
            break;
        }
        fy += row_unit;
    }
    manual_scroll_end();
    if (!st.open) return;

    if (IsKeyPressed(KEY_ESCAPE)) {
        st.open = false;
        return;
    }
    // "Select" button removed entirely on request (see this file's
    // own git history for the fuller reasoning) - a structural
    // simplification: with every row now directly Tab+Enter/click
    // activatable on its own, a further, separate confirmation button
    // would only reintroduce the exact indirect, two-step interaction
    // model this whole rework was meant to get rid of. "Cancel" stays
    // as the only explicit button - a deliberate abort still needs its
    // own, unambiguous action. Right-aligned again on request (was
    // briefly centered right after "Select" was removed, since it was
    // then the only button - reverted back to match this project's
    // usual convention of right-aligning a lone dialog action button,
    // e.g. the directory-URL picker's own "Cancel").
    if (focusable_button(251, {win.x + win.width - 130 * g_ui_scale, win.y + win.height - 44 * g_ui_scale, 120 * g_ui_scale, 32 * g_ui_scale}, "Cancel")) {
        st.open = false;
    }
}

// ---------------------------------------------------------------------
// Partition selection
// ---------------------------------------------------------------------

enum class PartitionFilter { Any, RootfsLike, BootLike, DisksOnly };

static bool is_rootfs_like_fstype(const std::string& fstype) {
    static const char* ok[] = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "jfs", "reiserfs"};
    for (auto t : ok) if (fstype == t) return true;
    return false;
}

static bool is_boot_like_fstype(const std::string& fstype) {
    return fstype == "vfat" || fstype == "fat" || fstype == "fat32" || fstype == "fat16" ||
           fstype == "msdos" || fstype == "exfat";
}

// Extra safety heuristic: exclude partitions whose label suggests
// /home from the rootfs list. Not a guarantee (only works if actually
// labeled that way).
static bool label_looks_like_home(const std::string& label) {
    std::string lower = label;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    return lower.find("home") != std::string::npos;
}

// Per-device cache for backend::probe_is_rootfs() - a real report
// (console photo) showed every rootfs-like partition being mounted,
// probed and unmounted TWICE during startup: once by rootfs_sel's own
// refresh(), then again by boot_sel's refresh() to work out which
// disks carry an OpenEmbedded rootfs at all. Each probe of a btrfs
// volume without OE markers additionally walks a few subvolumes (a
// Fedora root, say), and btrfs logs a handful of kernel lines per
// mount - so startup was both slow and loud for no gain. The second
// pass now reuses the first pass's answers. Cleared by the explicit
// Refresh button so a deliberate rescan still probes fresh (e.g.
// after plugging in a disk).
static std::map<std::string, bool> g_probe_cache;
static bool cached_probe_is_rootfs(const std::string& dev) {
    auto it = g_probe_cache.find(dev);
    if (it != g_probe_cache.end()) return it->second;
    bool r = backend::probe_is_rootfs(dev, log_msg);
    g_probe_cache[dev] = r;
    return r;
}

struct PartitionSelector {
    std::vector<backend::PartitionInfo> partitions;
    int selected_idx = 0;
    char mountpoint_buf[256] = "";
    PartitionFilter filter = PartitionFilter::Any;

    void refresh() {
        auto all = backend::list_partitions(log_msg);
        partitions.clear();

        // For DisksOnly, find which disk the live installer system
        // itself is running from (PKNAME of whatever partition is
        // mounted at "/") - never offer it as a wic-install target.
        // Writing a wic image to it would dd over the very system
        // currently performing the write, mid-operation - previously
        // unguarded, found via real-world use (it showed up in the
        // disk list).
        std::string own_boot_disk;
        if (filter == PartitionFilter::DisksOnly) {
            for (auto& p : all) {
                if (p.mountpoint == "/") { own_boot_disk = p.pkname; break; }
            }
        }

        // For BootLike, first find which disks (PKNAME) actually carry
        // a verified OpenEmbedded rootfs; a boot partition matches if
        // it's on the same disk. More robust than searching boot
        // loader entries for a specific kernel parameter.
        std::set<std::string> oe_disk_pknames;
        if (filter == PartitionFilter::BootLike) {
            log_msg("Searching disks with OpenEmbedded rootfs (for boot partition matching)...");
            for (auto& p : all) {
                if (p.type == "part" && is_rootfs_like_fstype(p.fstype) && !label_looks_like_home(p.label)) {
                    std::string dev = p.path.empty() ? p.name : p.path;
                    if (cached_probe_is_rootfs(dev)) oe_disk_pknames.insert(p.pkname);
                }
            }
        }

        if (filter == PartitionFilter::RootfsLike) log_msg("Searching rootfs partitions (briefly probing read-only)...");
        for (auto& p : all) {
            bool keep;
            // Anything currently mounted belongs to the running live
            // system itself (e.g. the boot USB stick) and must never
            // be offered as a target.
            bool currently_mounted = !p.mountpoint.empty();
            switch (filter) {
                case PartitionFilter::DisksOnly:
                    keep = (p.type == "disk") && (own_boot_disk.empty() || p.name != own_boot_disk);
                    break;
                case PartitionFilter::RootfsLike: {
                    keep = !currently_mounted && (p.type == "part") &&
                           is_rootfs_like_fstype(p.fstype) && !label_looks_like_home(p.label);
                    if (keep) {
                        std::string dev = p.path.empty() ? p.name : p.path;
                        keep = cached_probe_is_rootfs(dev);
                    }
                    break;
                }
                case PartitionFilter::BootLike: {
                    keep = !currently_mounted && (p.type == "part") && is_boot_like_fstype(p.fstype) &&
                           oe_disk_pknames.count(p.pkname) > 0;
                    break;
                }
                default:                           keep = true; break;
            }
            if (keep) partitions.push_back(p);
        }
        selected_idx = partitions.empty() ? -1 : 0;
    }

    std::string get_device() const {
        // Manual device/UUID override text field removed on request
        // ("we don't need that") - always uses the auto-detected
        // dropdown selection now.
        if (selected_idx < 0 || selected_idx >= (int)partitions.size()) return "";
        if (filter == PartitionFilter::DisksOnly) {
            const auto& p = partitions[selected_idx];
            return p.path.empty() ? p.name : p.path; // raw device, not UUID: we overwrite the partition table
        }
        return backend::partition_device_id(partitions[selected_idx]);
    }

    std::string get_mountpoint() const { return mountpoint_buf; }
};

static float draw_partition_selector(const char* title, PartitionSelector& sel, float x, float y,
                                      float w, int field_id_base, bool enabled,
                                      bool show_mountpoint = true) {
    const float ROW = 30 * g_ui_scale, GAP = 6 * g_ui_scale;
    const float BTNW = 100 * g_ui_scale, LBLW = 140 * g_ui_scale, FIELDX = 150 * g_ui_scale;
    if (!enabled) push_gui_disabled();

    GuiLabel({x, y, w, ROW}, title);
    y += ROW + GAP;

    std::string combo_text;
    for (auto& p : sel.partitions) { combo_text += backend::partition_label(p); combo_text += ";"; }
    if (!combo_text.empty()) combo_text.pop_back();
    if (combo_text.empty()) {
        switch (sel.filter) {
            case PartitionFilter::DisksOnly:  combo_text = "(no disk found)"; break;
            case PartitionFilter::RootfsLike: combo_text = "(no partition with a rootfs found)"; break;
            case PartitionFilter::BootLike:   combo_text = "(no boot partition (FAT) found)"; break;
            default:                          combo_text = "(no partition found)"; break;
        }
    }
    int active = sel.selected_idx < 0 ? 0 : sel.selected_idx;
    GuiComboBox({x, y, w - BTNW - 10 * g_ui_scale, ROW}, combo_text.c_str(), &active);
    if (!sel.partitions.empty()) sel.selected_idx = active;
    if (focusable_button(field_id_base + 2, {x + w - BTNW, y, BTNW, ROW}, "Refresh")) { g_probe_cache.clear(); sel.refresh(); }
    y += ROW + GAP;

    if (show_mountpoint) {
        GuiLabel({x, y, LBLW, ROW}, "Mount point:");
        text_field(field_id_base + 1, {x + FIELDX, y, w - FIELDX, ROW}, sel.mountpoint_buf, sizeof(sel.mountpoint_buf));
        y += ROW + GAP;
    }

    if (!enabled) pop_gui_disabled();
    return y;
}

// ---------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------

struct AppState {
    int install_mode = 0; // 0 = rootfs update (tar.gz), 1 = full install (wic image)

    int rootfs_source_kind = 0; // 0 = local, 1 = HTTP, 2 = HTTPS
    char rootfs_local_path[512] = "";
    char rootfs_http_url[512] = "";
    char rootfs_url[512] = ""; // HTTPS URL - kept this field's name as-is (not renamed to rootfs_https_url) to avoid a risky mass-rename of every existing reference
    char rootfs_sha256[128] = "";

    PartitionSelector rootfs_sel;
    char excludes_buf[256] = "";

    bool kernel_enabled = true;
    PartitionSelector boot_sel;
    int kernel_source_kind = 0; // 0 = local, 1 = HTTP, 2 = HTTPS
    char kernel_local_path[512] = "";
    char kernel_http_url[512] = "";
    char kernel_url[512] = ""; // HTTPS URL - see rootfs_url's own comment on the naming
    char kernel_target_name[64] = "bzImage";
    char kernel_sha256[128] = "";

    // Full install (wic image onto a whole disk)
    int wic_source_kind = 0; // 0 = local, 1 = HTTP, 2 = HTTPS
    char wic_local_path[512] = "";
    char wic_http_url[512] = "";
    char wic_url[512] = ""; // HTTPS URL - see rootfs_url's own comment on the naming
    char wic_sha256[128] = "";
    PartitionSelector disk_sel; // only the device part is used, no mountpoint

    bool show_confirm = false;
    FileBrowserState browser;
};

static AppState g_app;

// Loaded from /etc/yocto-rootfs-updater-raylib/config.toml at startup
// (see load_config() in main()), with these hardcoded fallbacks if
// the file is missing or a key isn't set.
static std::string g_cfg_filebrowser_start_dir = "/";

// Optional, opt-in extra candidate directory for the auto-pick-
// newest-file convenience below - "/mnt/storage/" + config.toml's own
// "local_default_dir" (only set/considered at all if that config key
// is actually present and non-empty; deliberately NOT a plain default
// path itself, since a bare directory isn't something the source-path
// fields below can use directly - they need an actual matching FILE
// within it, same as filebrowser_start_dir already works). Distinct
// from http_default_dir (see its own comment further down) - that one
// prefills a URL text field directly instead, since a directory-style
// URL is already handled at download time by
// backend::resolve_possible_directory_url().
static std::string g_cfg_local_default_dir; // empty = not configured, not used

static std::vector<std::string> discover_local_file_candidate_dirs() {
    std::vector<std::string> dirs = { g_cfg_filebrowser_start_dir };
    if (!g_cfg_local_default_dir.empty()) dirs.push_back(g_cfg_local_default_dir);
    DIR* mnt = opendir("/mnt");
    if (mnt) {
        struct dirent* e;
        while ((e = readdir(mnt)) != nullptr) {
            std::string name = e->d_name;
            // Exactly "usb" is the stable symlink usb-automount-helper.sh
            // keeps pointing at whichever real usb-<dev> mount came
            // first - same content as one of the real mount points
            // matched below, so listing it too would only produce a
            // duplicate candidate.
            if (name == "usb") continue;
            if (name.rfind("usb", 0) == 0) dirs.push_back("/mnt/" + name);
            // /mnt/storage itself, plus the numbered /mnt/storage2,
            // /mnt/storage3 ... that initramfs-home-mount adds for a
            // "home"-labeled partition on any FURTHER disk - on a
            // machine carrying two complete build systems on separate
            // disks, the image to install is just as likely to be the
            // newest one on the disk that wasn't booted from. Skipped
            // when it already is the browser's start directory (the
            // list's first entry above), which would only produce a
            // duplicate candidate.
            if (name.rfind("storage", 0) == 0) {
                std::string path = "/mnt/" + name;
                if (path != g_cfg_filebrowser_start_dir) dirs.push_back(path);
            }
        }
        closedir(mnt);
    }
    return dirs;
}

// ---------------------------------------------------------------------
// Worker pipeline
// ---------------------------------------------------------------------

// /tmp is tmpfs (RAM-backed) by default in Yocto/systemd images unless
// VOLATILE_TMP_DIR disables it - unsuitable as a staging area for a
// large HTTPS download (could exhaust RAM before extraction even
// starts). Prefer the storage-partition-helper partition if it's
// mounted, same reasoning as the filebrowser_start_dir auto-detection.
static std::string download_staging_dir() {
    // is_mountpoint() alone isn't enough anymore - /mnt/storage can
    // now genuinely be mounted read-only (initramfs-home-mount, on
    // the initramfs image specifically) - see is_writable_mountpoint()
    // itself for the full story (a real user report: HTTP(S) rootfs
    // downloads failing with curl exit code 23 once that started
    // happening). Falls back to /tmp in that case, same as when
    // /mnt/storage isn't mounted at all.
    return (backend::is_mountpoint("/mnt/storage") && backend::is_writable_mountpoint("/mnt/storage"))
               ? "/mnt/storage" : "/tmp";
}

static void run_pipeline(AppState snapshot) {
    std::string tmp_rootfs_download, tmp_kernel_download;
    bool do_kernel = snapshot.kernel_enabled;

    auto finish = [&](bool had_error, const std::string& err) {
        std::lock_guard<std::mutex> lk(g_state.mutex);
        g_state.worker_running = false;
        g_state.worker_done_signaled = true;
        g_state.worker_had_error = had_error;
        g_state.worker_error = err;
    };

    try {
        std::vector<std::string> excludes;
        {
            std::string s = snapshot.excludes_buf;
            size_t pos = 0;
            while (pos <= s.size()) {
                size_t comma = s.find(',', pos);
                std::string part = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!part.empty()) excludes.push_back(part);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }

        std::string tar_path;
        if (snapshot.rootfs_source_kind == 0) {
            tar_path = snapshot.rootfs_local_path;
            struct stat st{};
            if (stat(tar_path.c_str(), &st) != 0)
                throw backend::OperationError("File not found: " + tar_path);
        } else {
            std::string staging_dir = download_staging_dir();
            log_msg("Download staging directory: " + staging_dir);
            tmp_rootfs_download = staging_dir + "/yocto_rootfs_update.tar.gz.part";
            std::string rootfs_source_url = (snapshot.rootfs_source_kind == 1) ? snapshot.rootfs_http_url : snapshot.rootfs_url;
            std::string rootfs_url_resolved =
                backend::resolve_possible_directory_url(rootfs_source_url, ".tar.gz", log_msg);
            log_msg("Downloading rootfs: " + rootfs_url_resolved);
            backend::download(rootfs_url_resolved, tmp_rootfs_download, set_progress, &g_cancel_requested);
            tar_path = tmp_rootfs_download;
            log_msg("Rootfs download complete.");
        }

        if (snapshot.rootfs_sha256[0] != '\0') {
            log_msg("Checking rootfs SHA256...");
            set_progress(0, 1, "Checking checksum...");
            std::string actual = backend::sha256_of(tar_path);
            std::string expected = snapshot.rootfs_sha256;
            for (auto& c : expected) c = (char)tolower((unsigned char)c);
            std::string actual_lower = actual;
            for (auto& c : actual_lower) c = (char)tolower((unsigned char)c);
            if (actual_lower != expected)
                throw backend::OperationError("SHA256 mismatch!\nexpected: " + expected +
                                               "\nactual:   " + actual);
            log_msg("Checksum OK.");
        }

        std::string kernel_path;
        if (do_kernel) {
            if (snapshot.kernel_source_kind == 0) {
                kernel_path = snapshot.kernel_local_path;
                struct stat st{};
                if (stat(kernel_path.c_str(), &st) != 0)
                    throw backend::OperationError("Kernel file not found: " + kernel_path);
            } else {
                std::string kernel_source_url = (snapshot.kernel_source_kind == 1) ? snapshot.kernel_http_url : snapshot.kernel_url;
                tmp_kernel_download = download_staging_dir() + "/yocto_kernel_update.part";
                log_msg("Downloading kernel: " + kernel_source_url);
                backend::download(kernel_source_url, tmp_kernel_download, set_progress, &g_cancel_requested);
                kernel_path = tmp_kernel_download;
                log_msg("Kernel download complete.");
            }

            if (snapshot.kernel_sha256[0] != '\0') {
                log_msg("Checking kernel SHA256...");
                set_progress(0, 1, "Checking checksum...");
                std::string actual = backend::sha256_of(kernel_path);
                std::string expected = snapshot.kernel_sha256;
                for (auto& c : expected) c = (char)tolower((unsigned char)c);
                std::string actual_lower = actual;
                for (auto& c : actual_lower) c = (char)tolower((unsigned char)c);
                if (actual_lower != expected)
                    throw backend::OperationError("SHA256 mismatch (kernel)!\nexpected: " + expected +
                                                   "\nactual:   " + actual);
                log_msg("Checksum OK.");
            }
        }

        std::string rootfs_device = snapshot.rootfs_sel.get_device();
        std::string rootfs_mp = snapshot.rootfs_sel.get_mountpoint();
        std::string kernel_device = do_kernel ? snapshot.boot_sel.get_device() : "";
        std::string kernel_mp = do_kernel ? snapshot.boot_sel.get_mountpoint() : "";

        log_msg("Mounting rootfs partition " + rootfs_device + " at " + rootfs_mp + "...");
        set_progress(0, 1, "Mounting rootfs partition...");
        backend::do_mount(rootfs_device, rootfs_mp, log_msg);

        if (do_kernel) {
            log_msg("Mounting boot partition " + kernel_device + " at " + kernel_mp + "...");
            backend::do_mount(kernel_device, kernel_mp, log_msg);
        }

        log_msg("Scanning rootfs tarball...");
        set_progress(0, 1, "Scanning tarball...");
        backend::TarStats tar_stats = backend::scan_tar(tar_path, set_progress, &g_cancel_requested);
        long long capacity = backend::filesystem_capacity(rootfs_mp);
        log_msg("Tarball: " + std::to_string(tar_stats.entries) + " entries, at least " +
                backend::human_size((double)tar_stats.bytes) + " unpacked; partition holds " +
                backend::human_size((double)capacity) + ".");
        if (tar_stats.bytes > capacity)
            throw backend::OperationError("Rootfs does not fit: needs at least " +
                                           backend::human_size((double)tar_stats.bytes) + ", " +
                                           rootfs_device + " holds " +
                                           backend::human_size((double)capacity) + ".");

        std::vector<backend::NestedExcludeBackup> nested_backups = backend::backup_nested_excludes(rootfs_mp, excludes, log_msg);

        log_msg("Clearing rootfs partition (protected mountpoints are skipped)...");
        set_progress(0, 1, "Clearing rootfs partition...");
        backend::clear_directory(rootfs_mp, excludes, log_msg);

        long long available = backend::filesystem_available(rootfs_mp);
        if (tar_stats.bytes > available)
            throw backend::OperationError("Rootfs does not fit: needs at least " +
                                           backend::human_size((double)tar_stats.bytes) + ", only " +
                                           backend::human_size((double)available) +
                                           " free on " + rootfs_device + " after clearing.");

        log_msg("Extracting rootfs...");
        backend::extract_tar(tar_path, rootfs_mp, tar_stats.entries, set_progress, log_msg, &g_cancel_requested);

        backend::restore_nested_excludes(rootfs_mp, nested_backups, log_msg);

        if (do_kernel) {
            std::string target_name = snapshot.kernel_target_name[0] ? snapshot.kernel_target_name : "bzImage";
            std::string dest = kernel_mp + "/" + target_name;
            log_msg("Copying kernel to " + dest + "...");
            backend::copy_file(kernel_path, dest);
        }

        log_msg("Syncing write cache...");
        backend::run_checked({"sync"}, log_msg);
        // Unconditional now (used to be a user-toggleable checkbox) -
        // the reboot that follows shortly after uses "systemctl
        // reboot", a graceful, systemd-managed shutdown that already
        // unmounts every filesystem as part of its own normal
        // sequence - reconsidered on request: making this a
        // no-effect-either-way OPTION never made much sense, since
        // skipping it wouldn't actually leave anything mounted at
        // reboot regardless. Kept as an explicit step anyway (not
        // removed outright) purely for its own diagnostic value -
        // doing it here, before the reboot even starts, means a
        // genuine unmount failure (something still holding a file
        // open, say) surfaces in this app's own log where it's
        // actually visible, rather than happening silently/being
        // force-unmounted somewhere inside systemd's own shutdown
        // sequence instead.
        if (do_kernel) { log_msg("Unmounting " + kernel_mp + "..."); backend::do_unmount(kernel_mp, log_msg); }
        log_msg("Unmounting " + rootfs_mp + "..."); backend::do_unmount(rootfs_mp, log_msg);

        set_progress(1, 1, "Done. Rebooting shortly...");
        finish(false, "");
        g_reboot_requested = true;
    } catch (const std::exception& e) {
        log_msg(std::string("ERROR: ") + e.what());
        finish(true, e.what());
    }

    if (!tmp_rootfs_download.empty()) unlink(tmp_rootfs_download.c_str());
    if (!tmp_kernel_download.empty()) unlink(tmp_kernel_download.c_str());
}

// Separate, simpler pipeline for a full install: no mount/fstab/clear
// needed, dd writes directly to the raw disk including the partition
// table.
static void run_wic_pipeline(AppState snapshot) {
    std::string tmp_wic_download;

    auto finish = [&](bool had_error, const std::string& err) {
        std::lock_guard<std::mutex> lk(g_state.mutex);
        g_state.worker_running = false;
        g_state.worker_done_signaled = true;
        g_state.worker_had_error = had_error;
        g_state.worker_error = err;
    };

    try {
        bool is_url = (snapshot.wic_source_kind != 0);
        std::string source;
        if (snapshot.wic_source_kind == 0) source = snapshot.wic_local_path;
        else if (snapshot.wic_source_kind == 1) source = snapshot.wic_http_url;
        else source = snapshot.wic_url;
        if (is_url) source = backend::resolve_possible_directory_url(source, ".wic", log_msg);
        std::string device = snapshot.disk_sel.get_device();
        bool want_checksum = snapshot.wic_sha256[0] != '\0';

        if (!is_url) {
            struct stat st{};
            if (stat(source.c_str(), &st) != 0)
                throw backend::OperationError("File not found: " + source);
        }

        // Streaming curl straight into dd (the default, no SHA256
        // given) can't be checksummed beforehand - by the time the
        // whole file has been received, the disk has already been
        // overwritten. Only download to a temp file first (losing the
        // "works for arbitrarily large images with no local storage"
        // property) if the user actually asked for verification - a
        // real pre-write check is worth that trade-off, a
        // this-only-warns-after-the-fact one wouldn't be.
        if (is_url && want_checksum) {
            std::string staging_dir = download_staging_dir();
            log_msg("Download staging directory: " + staging_dir);
            tmp_wic_download = staging_dir + "/yocto_wic_install.img.part";
            log_msg("Downloading wic image: " + source);
            backend::download(source, tmp_wic_download, set_progress, &g_cancel_requested);
            log_msg("wic image download complete.");
            source = tmp_wic_download;
            is_url = false; // now a local file, write_disk_image reads it directly
        }

        if (want_checksum) {
            log_msg("Checking wic image SHA256...");
            set_progress(0, 1, "Checking checksum...");
            std::string actual = backend::sha256_of(source);
            std::string expected = snapshot.wic_sha256;
            for (auto& c : expected) c = (char)tolower((unsigned char)c);
            std::string actual_lower = actual;
            for (auto& c : actual_lower) c = (char)tolower((unsigned char)c);
            if (actual_lower != expected)
                throw backend::OperationError("SHA256 mismatch!\nexpected: " + expected +
                                               "\nactual:   " + actual);
            log_msg("Checksum OK.");
        }

        log_msg("Writing wic image to " + device + " ...");
        set_progress(0, 1, "Starting...");
        backend::write_disk_image(source, is_url, device, log_msg, set_progress, &g_cancel_requested);

        set_progress(1, 1, "Done. Rebooting shortly...");
        finish(false, "");
        g_reboot_requested = true;
    } catch (const std::exception& e) {
        log_msg(std::string("ERROR: ") + e.what());
        finish(true, e.what());
    }

    if (!tmp_wic_download.empty()) unlink(tmp_wic_download.c_str());
}

// ---------------------------------------------------------------------
// UI (one frame)
// ---------------------------------------------------------------------

// Display string for whichever source is currently selected (local
// path, HTTP URL, or HTTPS URL) - used so the confirmation dialog can
// show exactly what will be read from, not just the destination. Also
// serves double duty for the directory-URL picker further down: by
// the time that picker has run (see its own comments), the relevant
// *_http_url/_url field already holds the specific, chosen file - not
// a directory URL anymore - so this same helper naturally reports the
// resolved file with no extra bookkeeping needed for that case.
// GuiLabel() does not wrap text at all - a line wider than its box
// simply gets clipped, confirmed directly via a real rendered-pixel
// test (a long source URL/path was silently cut off mid-word). Used
// for the confirmation dialog specifically, since correctly reading
// the full source path/URL there is exactly the safety property that
// matters - found via the person's own doubt about this, worth
// getting right rather than truncating silently. Word-wraps each
// "\n"-separated paragraph independently (doesn't merge across an
// existing explicit line break) to the given pixel width, breaking at
// space boundaries; a single word wider than max_width on its own is
// left unbroken (better to overflow slightly than mangle a URL/path
// with no natural break point).
// BeginScissorMode() takes integers - a plain (int) cast on a float
// TRUNCATES (rounds toward zero) rather than rounding to the nearest
// integer. For a fractional (non-integer) g_ui_scale (this project's
// own ultrawide-display report used 1.333333, for one real example),
// downstream values like a scroll viewport's own height routinely end
// up with a fractional part (226.666626, confirmed via a real
// calculation elsewhere in this file) - truncating that away loses up
// to just under one full pixel off the BOTTOM edge of whatever's
// being scissor-clipped, which is exactly enough to make the last
// visible row's own bottom border look cut off/incomplete, even when
// the row-count/height math driving it is otherwise exactly right.
// Found via a direct user report against the WiFi network list
// dialog specifically (the frame it displays uses g_ui_scale=2.0,
// where this exact issue seems to not really apply since 2.0 keeps
// most such values as clean integers - but this project's own
// scissor calls are shared code, and other, fractional scale values
// are already confirmed in real use elsewhere in this project's own
// conversation history, so this is fixed generally, not scale-
// specifically). Used for BeginScissorMode() bounds specifically -
// rounding to the NEAREST integer here, rather than always rounding
// up/down, avoids overcorrecting in the opposite direction (a bounds
// rectangle very slightly LARGER than intended, which could then
// reveal a sliver of whatever's meant to stay hidden just outside it).



static std::string word_wrap_text(const std::string& text, float max_width, int font_size) {
    std::string result;
    size_t para_start = 0;
    while (para_start <= text.size()) {
        size_t nl = text.find('\n', para_start);
        std::string para = text.substr(para_start, nl == std::string::npos ? std::string::npos : nl - para_start);

        std::string line;
        size_t word_start = 0;
        while (word_start <= para.size()) {
            size_t sp = para.find(' ', word_start);
            std::string word = para.substr(word_start, sp == std::string::npos ? std::string::npos : sp - word_start);

            // A single word (no spaces - typically a URL/path) wider
            // than the whole available width on its own: hard-break it
            // character by character rather than leaving it to
            // overflow/get clipped by GuiLabel (which doesn't wrap at
            // all) - this is exactly the case a plain space-only wrap
            // can't help with, and confirmed via a real rendered test
            // that URLs hit it in practice (no spaces to break at).
            // Prefers breaking right after a URL-typical delimiter
            // (/ - . _ :) when one occurred reasonably recently in the
            // current chunk, purely for readability - a pure
            // character-width break is correct (nothing is lost
            // either way, confirmed via a real rendered test) but
            // reads awkwardly when it lands mid-number/mid-word, e.g.
            // splitting a timestamp as "...20260828120" / "000".
            if (MeasureText(word.c_str(), font_size) > max_width) {
                if (!line.empty()) { result += line + "\n"; line.clear(); }
                std::string chunk;
                int last_delim = -1; // index within chunk after which it's OK to break, if set
                for (char c : word) {
                    std::string candidate = chunk + c;
                    if (!chunk.empty() && MeasureText(candidate.c_str(), font_size) > max_width) {
                        if (last_delim >= 0 && last_delim >= (int)chunk.size() / 3) {
                            result += chunk.substr(0, (size_t)last_delim + 1) + "\n";
                            chunk = chunk.substr((size_t)last_delim + 1) + c;
                        } else {
                            result += chunk + "\n";
                            chunk = std::string(1, c);
                        }
                        last_delim = -1;
                    } else {
                        chunk = candidate;
                    }
                    if (c == '/' || c == '-' || c == '.' || c == '_' || c == ':') last_delim = (int)chunk.size() - 1;
                }
                line = chunk;
            } else {
                std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && MeasureText(candidate.c_str(), font_size) > max_width) {
                    result += line + "\n";
                    line = word;
                } else {
                    line = candidate;
                }
            }
            if (sp == std::string::npos) break;
            word_start = sp + 1;
        }
        result += line;

        if (nl == std::string::npos) break;
        result += "\n";
        para_start = nl + 1;
    }
    return result;
}

// Starts resolving the next queued directory URL (fetches its
// listing), or - once the queue is empty - proceeds to the normal
// confirmation dialog. Called both to kick off the whole chain (after
// Install is clicked) and after each picker selection to move on to
// the next pending one, if any.
// Opens the WiFi screen (or refreshes it if already open, e.g. after
// a failed connect attempt going "back" to the list) - finds the
// wireless device once per open, then scans/lists networks. Resets
// any in-progress password entry, but deliberately not device (no
// point re-detecting a device that already resolved successfully).
// Called directly from the WiFi button's click handler - deliberately
// does NOT do the actual (blocking) scan itself. Just opens the
// screen and requests one (see WifiScreenState::scan_pending's own
// comment on why the real work is deferred instead of happening
// here).
static void open_wifi_screen() {
    g_wifi_screen.active = true;
    g_wifi_screen.password_prompt = false;
    g_wifi_screen.connect_error.clear();
    g_wifi_screen.error.clear();
    g_wifi_screen.networks.clear();
    g_wifi_screen.scan_pending = true;
}

// Does the actual, blocking device-detection + scan work -
// deliberately called from the MAIN LOOP, one frame after
// scan_pending was first observed set (see that flag's own comment,
// and the main loop's own call site) - never from inside draw_ui()
// or the button click handler directly, both of which run BEFORE the
// frame requesting this has actually reached the screen.
static void run_pending_wifi_scan() {
    g_wifi_screen.scan_pending = false;
    if (g_wifi_screen.device.empty()) {
        // Driver auto-loads at boot again (kernel PCI hotplug
        // matching, no explicit modprobe here or a blacklist against
        // it anymore) - the modprobe-on-demand approach was a
        // debugging measure for a boot-time hang that was later
        // ruled out as unrelated to this specific driver's auto-load
        // timing; rolled back once that was established.
        g_wifi_screen.device = backend::find_wifi_device(log_msg);
        if (g_wifi_screen.device.empty()) {
            g_wifi_screen.error = "No WiFi device found (iwctl device list returned none).";
            return;
        }
    }
    g_wifi_screen.networks = backend::list_wifi_networks(g_wifi_screen.device, log_msg);
    if (g_wifi_screen.networks.empty()) {
        g_wifi_screen.error = "No networks found.";
    }
}

// Starts resolving the next queued directory URL (fetches its
// listing), or - once the queue is empty - proceeds to the normal
// confirmation dialog. Called both to kick off the whole chain (after
// Install is clicked) and after each picker selection to move on to
// the next pending one, if any.
static void advance_dir_picker_queue() {
    if (g_pending_dir_resolutions.empty()) {
        g_dir_picker_active = false;
        g_app.show_confirm = true;
        return;
    }
    auto& next = g_pending_dir_resolutions.front();
    g_dir_picker_error.clear();
    try {
        g_dir_picker_files = backend::list_directory_url_matches(next.dir_url, next.suffix, log_msg);
        if (g_dir_picker_files.empty()) {
            g_dir_picker_error = "No " + next.suffix + " file found in the directory listing at " + next.dir_url +
                                  " - go back and enter the exact file URL instead.";
        }
    } catch (const std::exception& e) {
        g_dir_picker_error = e.what();
    }
    g_dir_picker_active = true;
}

static std::string current_source_display(int source_kind, const char* local_path, const char* http_url, const char* https_url) {
    if (source_kind == 0) return local_path;
    if (source_kind == 1) return http_url;
    return https_url;
}

static void draw_ui() {
    float sw = (float)g_canvas_w, sh = (float)g_canvas_h;
    g_field_order.clear();
    bool modal_open = g_app.browser.open || g_dir_picker_active || g_app.show_confirm || g_wifi_screen.active;
    if (modal_open) GuiLock();

    bool worker_active;
    int progress_pct;
    std::string status;
    std::vector<std::string> visible_log;
    {
        std::lock_guard<std::mutex> lk(g_state.mutex);
        worker_active = g_state.worker_running;
        progress_pct = g_state.progress_pct;
        status = g_state.status;
        visible_log.assign(g_state.log_lines.begin(), g_state.log_lines.end());
        g_state.worker_done_signaled = false;
    }

    const float MARGIN = 20 * g_ui_scale, ROW = 30 * g_ui_scale, GAP = 6 * g_ui_scale,
                FORM_W = sw * 0.55f - 2 * MARGIN;
    const float FORM_PANEL_W = sw * 0.58f;
    float x = MARGIN, y = MARGIN - g_form_scroll;

    BeginScissorMode(0, 0, (int)FORM_PANEL_W, (int)sh);

    if (worker_active) push_gui_disabled();

    GuiLabel({x, y, FORM_W, ROW}, "Installation Mode");
    y += ROW + GAP;
    int mode = g_app.install_mode;
    focusable_toggle_group(200, {x, y, FORM_W / 2 - 4, ROW}, "RootFS Update;Full Install", &mode);
    if (mode != g_app.install_mode) g_form_scroll = 0; // avoid a stale scroll offset after switching
    g_app.install_mode = mode;
    y += ROW + GAP * 2;

    if (g_app.install_mode == 0) {
    GuiLabel({x, y, FORM_W, ROW}, "RootFS Source (.tar.gz)");
    y += ROW + GAP;
    int src_kind = g_app.rootfs_source_kind;
    // Item width computed so 3 items + 2 gaps exactly equals FORM_W -
    // GuiToggleGroup() places items at "bounds.width + GROUP_PADDING"
    // apart (confirmed directly in raygui.h's own source, default
    // GROUP_PADDING=2), so a per-item width of just "FORM_W/3" (or an
    // approximate "-4" guess, as this used to be) leaves the whole
    // row short of FORM_W by 2*GROUP_PADDING - found via a direct
    // visual-alignment report against this row's own progress bar
    // (which uses FORM_W directly, with no such gap to account for).
    float toggle_item_w = (FORM_W - 2 * GuiGetStyle(TOGGLE, GROUP_PADDING)) / 3;
    focusable_toggle_group(201, {x, y, toggle_item_w, ROW}, "Local File;HTTP;HTTPS", &src_kind);
    g_app.rootfs_source_kind = src_kind;
    y += ROW + GAP;

    if (g_app.rootfs_source_kind == 0) {
        // "110" now scaled - a direct report against the new, thicker
        // Tab-focus ring (text_field()'s own DrawRectangleLinesEx, see
        // its comment) made a real, long-standing bug obvious: this
        // width used a bare "110" while the Browse button right next
        // to it uses "100 * g_ui_scale" for its own position -
        // matched only by coincidence at g_ui_scale=1.0 (a 10px gap),
        // but at higher scale (2.0, confirmed on real 4K hardware)
        // this field was drawn 90px too wide, genuinely overlapping
        // the button - just far less noticeable with the thin default
        // hover border than with the new ring. Same fix applied at
        // the two other local-path fields below (kernel, wic) with
        // the identical pattern.
        text_field(1, {x, y, FORM_W - 110 * g_ui_scale, ROW}, g_app.rootfs_local_path, sizeof(g_app.rootfs_local_path));
        if (focusable_button(100, {x + FORM_W - 100 * g_ui_scale, y, 100 * g_ui_scale, ROW}, "Browse")) {
            g_app.browser.open_for(g_app.rootfs_local_path, sizeof(g_app.rootfs_local_path), g_cfg_filebrowser_start_dir.c_str(), {".tar.gz", ".tgz"});
        }
        // A newly picked rootfs takes its own kernel along when the
        // sibling naming of a shared images directory applies.
        static std::string last_rootfs_local_path;
        if (last_rootfs_local_path != g_app.rootfs_local_path) {
            last_rootfs_local_path = g_app.rootfs_local_path;
            std::string k = sibling_kernel_for(last_rootfs_local_path, g_app.kernel_target_name[0] ? g_app.kernel_target_name : "bzImage");
            if (!k.empty()) strncpy(g_app.kernel_local_path, k.c_str(), sizeof(g_app.kernel_local_path) - 1);
        }
    } else if (g_app.rootfs_source_kind == 1) {
        // Plain HTTP - deliberately no certificate/TLS involved at all
        // (see the field's own comment in AppState), requested
        // specifically for convenient same-LAN use where setting up a
        // trusted certificate for the HTTPS tab (see the ca-certificates
        // bbappend and scripts/serve-https.py) is more hassle than it's
        // worth.
        text_field(8, {x, y, FORM_W, ROW}, g_app.rootfs_http_url, sizeof(g_app.rootfs_http_url));
    } else {
        text_field(2, {x, y, FORM_W, ROW}, g_app.rootfs_url, sizeof(g_app.rootfs_url));
    }
    y += ROW + GAP;

    GuiLabel({x, y, 140 * g_ui_scale, ROW}, "SHA256 (optional):");
    text_field(3, {x + 150 * g_ui_scale, y, FORM_W - 150 * g_ui_scale, ROW}, g_app.rootfs_sha256, sizeof(g_app.rootfs_sha256));
    y += ROW + GAP * 2;

    y = draw_partition_selector("RootFS / \"platform\" Partition", g_app.rootfs_sel, x, y, FORM_W, 10, true, /*show_mountpoint=*/false);

    GuiLabel({x, y, 140 * g_ui_scale, ROW}, "Excludes:");
    text_field(4, {x + 150 * g_ui_scale, y, FORM_W - 150 * g_ui_scale, ROW}, g_app.excludes_buf, sizeof(g_app.excludes_buf));
    y += ROW + GAP * 2;

    GuiLabel({x, y, FORM_W, ROW}, "Also update kernel image (optional)");
    y += ROW + GAP;
    filled_checkbox(232, {x, y, ROW, ROW}, "enable", &g_app.kernel_enabled);
    y += ROW + GAP;

    y = draw_partition_selector("Boot Partition", g_app.boot_sel, x, y, FORM_W, 20, g_app.kernel_enabled, /*show_mountpoint=*/false);

    if (!g_app.kernel_enabled) push_gui_disabled();
    int ksrc = g_app.kernel_source_kind;
    // Same exact-width calculation as the RootFS source toggle above
    // (id 201) - see its own comment for the full reasoning.
    float ksrc_toggle_item_w = (FORM_W - 2 * GuiGetStyle(TOGGLE, GROUP_PADDING)) / 3;
    focusable_toggle_group(202, {x, y, ksrc_toggle_item_w, ROW}, "Local File;HTTP;HTTPS", &ksrc);
    g_app.kernel_source_kind = ksrc;
    y += ROW + GAP;
    if (g_app.kernel_source_kind == 0) {
        text_field(5, {x, y, FORM_W - 110 * g_ui_scale, ROW}, g_app.kernel_local_path, sizeof(g_app.kernel_local_path));
        if (focusable_button(101, {x + FORM_W - 100 * g_ui_scale, y, 100 * g_ui_scale, ROW}, "Browse")) {
            g_app.browser.open_for(g_app.kernel_local_path, sizeof(g_app.kernel_local_path), g_cfg_filebrowser_start_dir.c_str(), {}, {".tar.gz", ".tgz", ".wic"}, g_app.kernel_target_name);
        }
    } else if (g_app.kernel_source_kind == 1) {
        text_field(9, {x, y, FORM_W, ROW}, g_app.kernel_http_url, sizeof(g_app.kernel_http_url));
    } else {
        text_field(6, {x, y, FORM_W, ROW}, g_app.kernel_url, sizeof(g_app.kernel_url));
    }
    y += ROW + GAP;
    GuiLabel({x, y, 140 * g_ui_scale, ROW}, "SHA256 (optional):");
    text_field(11, {x + 150 * g_ui_scale, y, FORM_W - 150 * g_ui_scale, ROW}, g_app.kernel_sha256, sizeof(g_app.kernel_sha256));
    y += ROW + GAP;
    GuiLabel({x, y, 220 * g_ui_scale, ROW}, "Target filename on boot partition:");
    text_field(7, {x + 230 * g_ui_scale, y, FORM_W - 230 * g_ui_scale, ROW}, g_app.kernel_target_name, sizeof(g_app.kernel_target_name));
    y += ROW + GAP;
    if (!g_app.kernel_enabled) pop_gui_disabled();
    y += GAP;

    } else {
        // --- Full install: wic image onto a whole disk ---
        GuiLabel({x, y, FORM_W, ROW}, "wic Image Source (.wic)");
        y += ROW + GAP;
        int wsrc = g_app.wic_source_kind;
        // Same exact-width calculation as the RootFS source toggle
        // above (id 201) - see its own comment for the full reasoning.
        float wsrc_toggle_item_w = (FORM_W - 2 * GuiGetStyle(TOGGLE, GROUP_PADDING)) / 3;
        focusable_toggle_group(203, {x, y, wsrc_toggle_item_w, ROW}, "Local File;HTTP;HTTPS", &wsrc);
        g_app.wic_source_kind = wsrc;
        y += ROW + GAP;

        if (g_app.wic_source_kind == 0) {
            text_field(30, {x, y, FORM_W - 110 * g_ui_scale, ROW}, g_app.wic_local_path, sizeof(g_app.wic_local_path));
            if (focusable_button(102, {x + FORM_W - 100 * g_ui_scale, y, 100 * g_ui_scale, ROW}, "Browse")) {
                g_app.browser.open_for(g_app.wic_local_path, sizeof(g_app.wic_local_path), g_cfg_filebrowser_start_dir.c_str(), {".wic"});
            }
        } else if (g_app.wic_source_kind == 1) {
            text_field(10, {x, y, FORM_W, ROW}, g_app.wic_http_url, sizeof(g_app.wic_http_url));
        } else {
            text_field(31, {x, y, FORM_W, ROW}, g_app.wic_url, sizeof(g_app.wic_url));
        }
        y += ROW + GAP;

        GuiLabel({x, y, 140 * g_ui_scale, ROW}, "SHA256 (optional):");
        text_field(32, {x + 150 * g_ui_scale, y, FORM_W - 150 * g_ui_scale, ROW}, g_app.wic_sha256, sizeof(g_app.wic_sha256));
        y += ROW + GAP;
        if (g_app.wic_source_kind != 0 && g_app.wic_sha256[0] != '\0') {
            GuiLabel({x, y, FORM_W, ROW}, "Note: checksum with a URL downloads fully before writing (no more direct streaming).");
            y += ROW + GAP;
        }
        y += ROW + GAP;

        y = draw_partition_selector("Target Disk (WILL BE COMPLETELY OVERWRITTEN)", g_app.disk_sel,
                                     x, y, FORM_W, 40, true, /*show_mountpoint=*/false);
        y += ROW + GAP;
    }

    if (worker_active) pop_gui_disabled();

    // --- Progress & buttons ---
    GuiLabel({x, y, FORM_W, ROW}, status.c_str());
    y += ROW + GAP;
    float progress = progress_pct / 100.0f;
    std::string progress_pct_text = std::to_string(progress_pct) + "%";
    GuiProgressBar({x, y, FORM_W, ROW}, nullptr, progress_pct_text.c_str(), &progress, 0, 1);
    y += ROW + GAP * 2;

    // Whether all fields required for the current mode/source are
    // actually filled in - found via user report: Install was
    // clickable (and the confirmation dialog would open) even with
    // no rootfs.tar.gz/.wic source selected at all. The underlying
    // operations DO fail gracefully on an empty/missing path (stat()
    // fails, throws "File not found") rather than doing anything
    // actually destructive with empty data - but there's no reason to
    // let it get that far when it can be caught proactively, removing
    // any doubt about what an empty source might do.
    bool ready_to_install = true;
    if (g_app.install_mode == 0) {
        bool has_rootfs_source = (g_app.rootfs_source_kind == 0) ? (g_app.rootfs_local_path[0] != '\0')
            : (g_app.rootfs_source_kind == 1) ? (g_app.rootfs_http_url[0] != '\0') : (g_app.rootfs_url[0] != '\0');
        if (!has_rootfs_source) { ready_to_install = false; }
        if (ready_to_install && g_app.rootfs_sel.get_device().empty()) {
            ready_to_install = false;
        }
        if (ready_to_install && g_app.kernel_enabled) {
            bool has_kernel_source = (g_app.kernel_source_kind == 0) ? (g_app.kernel_local_path[0] != '\0')
                : (g_app.kernel_source_kind == 1) ? (g_app.kernel_http_url[0] != '\0') : (g_app.kernel_url[0] != '\0');
            if (!has_kernel_source) { ready_to_install = false; }
            if (ready_to_install && g_app.boot_sel.get_device().empty()) {
                ready_to_install = false;
            }
        }
    } else {
        bool has_wic_source = (g_app.wic_source_kind == 0) ? (g_app.wic_local_path[0] != '\0')
            : (g_app.wic_source_kind == 1) ? (g_app.wic_http_url[0] != '\0') : (g_app.wic_url[0] != '\0');
        if (!has_wic_source) { ready_to_install = false; }
        if (ready_to_install && g_app.disk_sel.get_device().empty()) {
            ready_to_install = false;
        }
    }

    if (!worker_active) {
        if (!ready_to_install) {
            push_gui_disabled();
            GuiButton({x, y, 140 * g_ui_scale, ROW + 6}, "Install");
            pop_gui_disabled();
        } else
        if (focusable_button(220, {x, y, 140 * g_ui_scale, ROW + 6}, "Install")) {
            bool safe_to_proceed = true;
            if (g_app.install_mode == 1) {
                auto partitions = backend::list_partitions(log_msg);
                std::string target_disk = g_app.disk_sel.get_device();

                // Independent of how the target was selected - even
                // though the dropdown itself already excludes the
                // running system's own boot disk, this re-verifies
                // against a freshly re-queried partition list right
                // here at install time rather than only trusting
                // whatever the dropdown had selected whenever it was
                // last populated/refreshed. Found while reasoning
                // through a user's question about the installer
                // needing to run entirely from RAM if its own disk
                // gets wiped (it doesn't - this project boots a real,
                // disk-backed rootfs, no initramfs) - realized the
                // dropdown-only exclusion wasn't actually a complete
                // guarantee on its own. wic-installing onto the disk
                // the installer is currently running FROM would risk
                // the exact same mid-operation corruption as the
                // local-file-source check below, just for the
                // installer's own running code/libraries instead of a
                // source file - the kernel demand-pages executable
                // pages in on first use, not all at once at startup,
                // so a not-yet-touched code path could try to page in
                // from already-overwritten disk content mid-install.
                std::string own_boot_disk = backend::disk_containing_path("/", partitions);
                if (!own_boot_disk.empty() && !target_disk.empty() &&
                    target_disk.size() >= own_boot_disk.size() &&
                    target_disk.compare(target_disk.size() - own_boot_disk.size(), own_boot_disk.size(), own_boot_disk) == 0) {
                    log_msg("ERROR: The selected target (" + target_disk + ") is the disk this installer is "
                            "currently running from - installing onto it would risk corrupting the installer's "
                            "own running code mid-operation. Choose a different target disk.");
                    safe_to_proceed = false;
                }

                // Safety check specific to wic-install with a local file
                // source: dd reads the source sequentially while writing
                // the target sequentially - if source and target
                // physically overlap (the local .wic file sits on the
                // very disk about to be wiped, e.g. via /mnt/storage in
                // the embedded installer pointing at the desktop's own
                // disk), the write would progressively overwrite parts of
                // the source file it hasn't read yet, corrupting the copy
                // mid-operation. Found via user question before it could
                // bite in practice.
                if (safe_to_proceed && g_app.wic_source_kind == 0) {
                    std::string source_disk = backend::disk_containing_path(g_app.wic_local_path, partitions);
                    // target_disk is a full path like /dev/sda; source_disk
                    // (from PKNAME) is a bare name like "sda" - compare by
                    // suffix rather than requiring an exact match.
                    if (!source_disk.empty() && !target_disk.empty() &&
                        target_disk.size() >= source_disk.size() &&
                        target_disk.compare(target_disk.size() - source_disk.size(), source_disk.size(), source_disk) == 0) {
                        log_msg("ERROR: The selected local .wic file is on the same disk (" + source_disk +
                                ") as the selected target - writing would overwrite the source file while still "
                                "reading it. Choose a different target disk, or move/copy the source file elsewhere first.");
                        safe_to_proceed = false;
                    }
                }
            }
            if (safe_to_proceed) {
                // Directory-style HTTP/HTTPS URLs (ending in "/") now
                // go through an explicit picker instead of silently
                // auto-selecting the lexicographically latest file -
                // see advance_dir_picker_queue()'s own comments and
                // the picker's drawing code further down for why.
                // Local paths and exact file URLs need no resolution
                // at all and skip straight to the confirmation
                // dialog, same as before.
                g_pending_dir_resolutions.clear();
                auto maybe_queue = [](int source_kind, char* http_buf, size_t http_size,
                                       char* https_buf, size_t https_size,
                                       const char* suffix, const std::string& label) {
                    char* buf = nullptr; size_t size = 0;
                    if (source_kind == 1) { buf = http_buf; size = http_size; }
                    else if (source_kind == 2) { buf = https_buf; size = https_size; }
                    if (!buf) return;
                    std::string val = buf;
                    if (!val.empty() && val.back() == '/') {
                        g_pending_dir_resolutions.push_back({val, suffix, buf, size, label});
                    }
                };
                if (g_app.install_mode == 0) {
                    maybe_queue(g_app.rootfs_source_kind, g_app.rootfs_http_url, sizeof(g_app.rootfs_http_url),
                                g_app.rootfs_url, sizeof(g_app.rootfs_url), ".tar.gz", "RootFS source");
                    // Kernel deliberately NOT queued here, consistent
                    // with the existing worker-thread download path
                    // (main.cpp's own kernel download code never calls
                    // resolve_possible_directory_url() either) - kernel
                    // filenames have no reliable extension to filter a
                    // directory listing by (bzImage, zImage, Image,
                    // uImage, ... vary by architecture), unlike
                    // rootfs.tar.gz/.wic. A directory-style URL entered
                    // for the kernel field is simply used as-is/fails at
                    // download time, same as before this picker existed.
                } else {
                    maybe_queue(g_app.wic_source_kind, g_app.wic_http_url, sizeof(g_app.wic_http_url),
                                g_app.wic_url, sizeof(g_app.wic_url), ".wic", "wic image source");
                }
                advance_dir_picker_queue();
            }
        }
    } else {
        // Disabled while a worker is running - not registered with
        // focusable_button()/Tab order at all, same reasoning as
        // elsewhere in this app: a disabled control has nothing
        // meaningful to activate, so it shouldn't consume a Tab stop.
        push_gui_disabled();
        GuiButton({x, y, 140 * g_ui_scale, ROW + 6}, "Install");
        pop_gui_disabled();
    }
    if (worker_active) {
        if (focusable_button(221, {x + 150 * g_ui_scale, y, 140 * g_ui_scale, ROW + 6}, "Cancel")) {
            g_cancel_requested = true;
            log_msg("Cancellation requested...");
        }
    } else {
        // Nothing running to cancel - repurpose the same button slot
        // as a manual reboot trigger (same effect as ESC) instead of
        // just greying it out. More discoverable/reachable than ESC
        // on a touchscreen or mouse-only kiosk setup.
        if (focusable_button(222, {x + 150 * g_ui_scale, y, 140 * g_ui_scale, ROW + 6}, "Reboot")) {
            log_msg("Reboot requested via button.");
            g_reboot_requested = true;
        }
        // Debug-only "Exit" button, gated behind config.toml's own
        // "debug_mode" - a clean app exit for development/testing
        // iteration speed, deliberately distinct from Reboot: it does
        // NOT call this project's own reboot logic afterward (see
        // g_exit_requested's own handling further down and its
        // comment on why). This genuinely avoids a forced reboot on
        // BOTH boot variants now - the app's own systemd service has
        // "Restart=on-failure", so a clean (exit code 0) return
        // neither restarts the app nor reboots the machine, just
        // leaves the system running with the app gone (e.g. to drop
        // to a shell on another VT). Previously only true on the
        // disk-backed image - the initramfs variant used to have its
        // own hand-written /init unconditionally rebooting after ANY
        // app exit regardless of why, before that image was rebuilt
        // for full parity with the disk-backed one (systemd, same
        // IMAGE_INSTALL - see that recipe's own comments).
        if (g_cfg_debug_mode) {
            if (focusable_button(223, {x + 300 * g_ui_scale, y, 100 * g_ui_scale, ROW + 6}, "Exit")) {
                log_msg("Exit requested via debug button (no reboot).");
                g_exit_requested = true;
            }
        }
        // Bottom-right of this row, fixed offset regardless of
        // whether Exit (above) happens to be shown - simpler than
        // conditionally shifting position based on debug_mode too,
        // and there's room for both either way at this row's current
        // width. Only shown at all if this image was actually built
        // with WiFi support (see g_wifi_available's own comment).
        //
        // Special-cased on request: no hover-color change at all
        // (override_color makes NORMAL/FOCUSED/PRESSED all resolve to
        // the same color - see focusable_button()'s own comment), and
        // turns orange - Catppuccin Mocha's own "Peach" (#fab387,
        // verified directly against the real palette.json from
        // catppuccin/palette on request, not guessed at - Noctalia's
        // own theme is itself Catppuccin-sourced per this project's
        // config.toml) - whenever a WiFi connection currently exists.
        // Status polled periodically (WIFI_POLL_INTERVAL_SECONDS),
        // not every frame - see g_wifi_connected's own comment on why
        // (each check is a real iwctl subprocess call).
        if (g_wifi_available) {
            double now = GetTime();
            if (now - g_wifi_last_poll_time >= WIFI_POLL_INTERVAL_SECONDS) {
                g_wifi_last_poll_time = now;
                if (g_wifi_poll_device.empty()) g_wifi_poll_device = backend::find_wifi_device(log_msg);
                g_wifi_connected = backend::is_wifi_connected(g_wifi_poll_device);
            }
            // Hover-suppression (identical NORMAL/FOCUSED colors, see
            // focusable_button()'s own comment) now ONLY applied in
            // the connected (orange/Peach) state - reconsidered on
            // request: every other button in this app already turns
            // green on hover, and there's no longer a strong reason
            // for this one to be the sole exception while showing its
            // own default, unconnected appearance. Still suppressed
            // specifically while connected, so the orange "connected"
            // indicator itself doesn't flicker to green on hover -
            // that state benefits from staying visually stable/
            // unambiguous, unlike the plain default state below.
            Color border_color = GetColor(0xfab387ffU);
            Color base_color = GetColor(0xfab387ffU);
            Color text_color = GetColor(0xf3edf7ffU);
            // Right-aligned with the progress bar above (that one
            // spans {x, ..., FORM_W, ...} - same x origin, same
            // FORM_W-wide right edge) rather than a fixed offset from
            // the row's own left edge, on request.
            float wifi_btn_w = 100 * g_ui_scale;
            bool wifi_clicked = g_wifi_connected
                ? focusable_button(224, {x + FORM_W - wifi_btn_w, y, wifi_btn_w, ROW + 6}, "WiFi", &border_color, &base_color, &text_color)
                : focusable_button(224, {x + FORM_W - wifi_btn_w, y, wifi_btn_w, ROW + 6}, "WiFi");
            if (wifi_clicked) {
                if (g_wifi_connected) {
                    // Orange = connected - clicking it again disconnects
                    // directly, no confirmation dialog (matches how
                    // clicking a network in the list connects directly
                    // for open networks too - a WiFi connection isn't a
                    // destructive/hard-to-undo action worth an extra
                    // click to confirm).
                    log_msg("WiFi button clicked (connected - disconnecting).");
                    try {
                        backend::disconnect_wifi_network(g_wifi_poll_device, log_msg);
                        g_wifi_connected = false;
                    } catch (const std::exception& e) {
                        log_msg(std::string("WARNING: disconnect failed: ") + e.what());
                    }
                } else {
                    log_msg("WiFi button clicked.");
                    open_wifi_screen();
                }
            }
        }
    }

    EndScissorMode();

    // Form scrolling via mouse wheel, clamped to actual content height.
    float content_height = y - (MARGIN - g_form_scroll) + ROW + GAP;
    float max_scroll = content_height - sh + MARGIN;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll > 0) {
        Rectangle form_rect = {0, 0, FORM_PANEL_W, sh};
        if (!modal_open && CheckCollisionPointRec(GetMousePosition(), form_rect)) {
            g_form_scroll -= GetMouseWheelMove() * 40.0f;
        }
        if (IsKeyPressed(KEY_HOME)) g_form_scroll = 0;
        if (g_form_scroll < 0) g_form_scroll = 0;
        if (g_form_scroll > max_scroll) g_form_scroll = max_scroll;
        float thumb_h = std::max(20.0f, sh * (sh / content_height));
        float thumb_y = (sh - thumb_h) * (g_form_scroll / max_scroll);
        DrawRectangle((int)FORM_PANEL_W - (int)(8 * g_ui_scale), (int)thumb_y, (int)(6 * g_ui_scale), (int)thumb_h, Fade(RAYWHITE, 0.6f));

        // Fixed "scroll to top" button, always in the same spot.
        if (g_form_scroll > 0) {
            if (focusable_button(240, {FORM_PANEL_W - 110 * g_ui_scale, 5 * g_ui_scale, 100 * g_ui_scale, 26 * g_ui_scale}, "▲ Top")) g_form_scroll = 0;
        }
    } else {
        g_form_scroll = 0;
    }

    // --- Log panel ---
    float log_x = sw * 0.58f, log_y = MARGIN, log_w = sw - log_x - MARGIN, log_h = sh - 2 * MARGIN;
    GuiGroupBox({log_x, log_y, log_w, log_h}, "Log");
    static Vector2 log_scroll = {0, 0};
    Rectangle log_view{};
    // Same size as the rest of the UI (DEFAULT TEXT_SIZE) rather than
    // a separate, much smaller fixed value - there isn't enough log
    // text on screen at once to need the extra density, and larger
    // text is easier to read. line_h scales along with it to avoid
    // cramped/overlapping lines. Noctalia's own light green (hover,
    // 0x9bfece - see apply_noctalia_dark_theme()) instead of
    // DARKGRAY, which read as too dark/low-contrast against the dark
    // log background - originally used the theme's yellow (primary)
    // here too, changed to green on explicit request since so much
    // of the rest of the UI was already yellow.
    float log_font_size = 16 * g_ui_scale;
    float line_h = log_font_size * 1.6f;
    float content_h = std::max(log_h - 20, (float)visible_log.size() * line_h);
    Rectangle content = {0, 0, log_w - 20, content_h};
    GuiScrollPanel({log_x + 5, log_y + 20, log_w - 10, log_h - 25}, nullptr, content, &log_scroll, &log_view);
    BeginScissorMode(round_for_scissor(log_view.x), round_for_scissor(log_view.y), round_for_scissor(log_view.width), round_for_scissor(log_view.height));
    float ly = log_view.y + log_scroll.y + 4;
    for (auto& line : visible_log) {
        DrawTextEx(g_log_font, line.c_str(), {log_view.x + 4, ly}, log_font_size, 1.0f, GetColor(0x9bfeceffU));
        ly += line_h;
    }
    EndScissorMode();

    if (modal_open) GuiUnlock();
    draw_file_browser(g_app.browser);

    // --- Directory-URL picker ---
    // Shown instead of silently auto-selecting the "latest" file when
    // a directory-style HTTP/HTTPS URL was entered - see
    // advance_dir_picker_queue()'s own comments for why this replaced
    // the earlier silent-heuristic approach. Modal, same spirit as
    // the confirmation dialog below (and mutually exclusive with it -
    // show_confirm only ever gets set once this queue is fully
    // drained).
    if (g_dir_picker_active) {
        g_field_order.clear(); // same reasoning as the confirm dialog's own scoping - see its comment
        // Escape as a global shortcut, same convention as this app's
        // other dialogs (file browser, confirmation dialog, WiFi
        // screen) - same action as the explicit "Cancel" button below.
        if (IsKeyPressed(KEY_ESCAPE)) {
            g_pending_dir_resolutions.clear();
            g_dir_picker_active = false;
        }
    }
    // Deliberately re-checked (not "else") - Escape just above may
    // have turned this false for this same frame, and the rest of
    // this dialog's own drawing below reads g_pending_dir_resolutions.
    // front() unconditionally, which would be undefined behavior on
    // the now-just-cleared vector without this guard.
    if (g_dir_picker_active) {
        DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.5f));

        auto& current = g_pending_dir_resolutions.front();
        float box_w = 560 * g_ui_scale;
        float inner_w = box_w - 24;
        std::string header = "Choose " + current.label + " (" + current.dir_url + ")";
        std::string wrapped_header = word_wrap_text(header, inner_w, GuiGetStyle(DEFAULT, TEXT_SIZE));
        int header_lines = 1;
        for (char c : wrapped_header) if (c == '\n') header_lines++;
        float header_h = (header_lines + 1.5f) * GuiGetStyle(DEFAULT, TEXT_SIZE) * 1.6f; // +1.5 slack - see the confirm dialog's own comment on why an exact fit clips the last line

        float row_h = 30 * g_ui_scale, row_gap = 4 * g_ui_scale;
        float list_h; // height available for the file list / error message
        float row_unit = row_h + row_gap;
        if (!g_dir_picker_error.empty()) {
            std::string wrapped_err = word_wrap_text(g_dir_picker_error, inner_w, GuiGetStyle(DEFAULT, TEXT_SIZE));
            int err_lines = 1;
            for (char c : wrapped_err) if (c == '\n') err_lines++;
            list_h = (err_lines + 1.5f) * GuiGetStyle(DEFAULT, TEXT_SIZE) * 1.6f; // same +1.5 slack reasoning
        } else {
            // Same simplified, direct calculation as the WiFi network
            // list dialog - see its own comment for the full story on
            // why the previous "screen percentage, then cap, then
            // round" chain was abandoned in favor of this, and later
            // GuiScrollPanel() itself replaced entirely with
            // manual_scroll_begin() - no +border_compensation here
            // anymore, unneeded for the same reason.
            const int MAX_VISIBLE_ROWS = 6;
            int visible_rows = std::min((int)g_dir_picker_files.size(), MAX_VISIBLE_ROWS);
            if (visible_rows < 1) visible_rows = 1;
            list_h = (float)visible_rows * row_unit;
        }
        float btn_h = 30 * g_ui_scale;
        float box_h = 28 + header_h + 12 + list_h + 24 + btn_h + 12;
        // Same real bug/fix as the WiFi network list dialog - see its
        // own comment for the full story (a navy-colored block, the
        // Select/Cancel buttons' own background, covering the last one
        // or two list rows on a short enough screen, found via a
        // direct user report against that other dialog - this one has
        // the identical structure, so the identical fix applies here).
        if (g_dir_picker_error.empty() && box_h > sh * 0.85f) {
            float capped_box_h = sh * 0.85f;
            float available_for_list = capped_box_h - (28 + header_h + 12 + 24 + btn_h + 12);
            int fitted_rows = (int)std::floor(available_for_list / row_unit);
            if (fitted_rows < 1) fitted_rows = 1;
            list_h = (float)fitted_rows * row_unit;
            box_h = 28 + header_h + 12 + list_h + 24 + btn_h + 12;
            // Deliberately NOT re-capped against sh*0.85f again after
            // this - box_h here is already exactly consistent with
            // list_h (both freshly recomputed together, right above).
            // Re-capping box_h alone a second time, without also
            // reducing list_h a second time, is exactly what
            // reintroduced this same Close-button-overlaps-the-list
            // bug on an extremely short screen (one where even a
            // single row - the fitted_rows floor above - doesn't fit
            // within 85% of the screen height) - found via a real,
            // deliberately extreme test case. On a screen that short,
            // this dialog running slightly past the 85% mark is the
            // lesser problem compared to internal overlap.
        } else if (!g_dir_picker_error.empty()) {
            // Error-message case doesn't get the row-based shrink-to-
            // fit treatment above - still capped directly the simple
            // way, same as before this whole fix. See the WiFi network
            // list dialog's own identical comment for the full
            // reasoning.
            box_h = std::min(box_h, sh * 0.85f);
        }
        Rectangle box = {sw / 2 - box_w / 2, sh / 2 - box_h / 2, box_w, box_h};

        draw_window_box(box, "Select File");
        int prevAlign = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
        GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
        GuiLabel({box.x + 12, box.y + 28, inner_w, header_h}, wrapped_header.c_str());
        GuiSetStyle(LABEL, TEXT_ALIGNMENT, prevAlign);

        float list_y = box.y + 28 + header_h + 12;
        if (!g_dir_picker_error.empty()) {
            int prevAlign2 = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
            std::string wrapped_err = word_wrap_text(g_dir_picker_error, inner_w, GuiGetStyle(DEFAULT, TEXT_SIZE));
            GuiLabel({box.x + 12, list_y, inner_w, list_h}, wrapped_err.c_str());
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, prevAlign2);
        } else {
            static float picker_scroll_offset = 0;
            float content_h = (float)g_dir_picker_files.size() * (row_h + row_gap);
            Rectangle list_rect = {box.x + 12, list_y, inner_w, list_h};
            float fy = manual_scroll_begin(list_rect, content_h, picker_scroll_offset);
            for (size_t i = 0; i < g_dir_picker_files.size(); i++) {
                if (focusable_button(300 + (int)i, {list_rect.x, fy, list_rect.width - 4, row_h}, g_dir_picker_files[i].c_str(), nullptr, nullptr, nullptr, /*no_border=*/true)) {
                    std::string chosen_url = current.dir_url + g_dir_picker_files[i];
                    strncpy(current.target_buf, chosen_url.c_str(), current.target_buf_size - 1);
                    current.target_buf[current.target_buf_size - 1] = '\0';
                    log_msg(current.label + " resolved to: " + chosen_url);
                    g_pending_dir_resolutions.erase(g_pending_dir_resolutions.begin());
                    advance_dir_picker_queue();
                }
                fy += row_h + row_gap;
            }
            manual_scroll_end();
        }

        if (focusable_button(299, {box.x + box.width / 2 - 60 * g_ui_scale, box.y + box.height - btn_h - 12, 120 * g_ui_scale, btn_h}, "Cancel")) {
            // Aborts the WHOLE install attempt, not just this one
            // picker - matches clicking Cancel on the confirmation
            // dialog: back to the main form with nothing changed.
            g_pending_dir_resolutions.clear();
            g_dir_picker_active = false;
        }
    }

    // --- WiFi screen ---
    // Same modal spirit/scoping as the directory-URL picker and
    // confirmation dialog above - see open_wifi_screen()'s own
    // comment on why scanning/connecting run synchronously here
    // rather than on the background worker thread.
    if (g_wifi_screen.active) {
        g_field_order.clear();
        // Escape as a global shortcut regardless of which specific
        // field/button currently has Tab focus - same convention this
        // app's file browser and confirmation dialog already use.
        // Deliberately skipped during scan_pending specifically (the
        // one-frame "Scanning..." state) - that state has nothing
        // meaningful to back out of yet, the blocking scan itself is
        // about to run regardless.
        if (IsKeyPressed(KEY_ESCAPE) && !g_wifi_screen.scan_pending) {
            if (g_wifi_screen.password_prompt) {
                g_wifi_screen.password_prompt = false;
                g_wifi_screen.connect_error.clear();
                memset(g_wifi_screen.password_buf, 0, sizeof(g_wifi_screen.password_buf));
            } else {
                g_wifi_screen.active = false;
            }
        }
        DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.5f));

        float box_w = 560 * g_ui_scale;
        float inner_w = box_w - 24;
        float row_h = 30 * g_ui_scale, row_gap = 4 * g_ui_scale;
        float btn_h = 30 * g_ui_scale;

        if (g_wifi_screen.scan_pending) {
            // Simple, deliberately minimal "please wait" screen for
            // the one frame between the button click and the actual
            // (blocking) scan running - see WifiScreenState::
            // scan_pending's own comment and the main loop's own call
            // site for how/why this is timed the way it is. No list/
            // buttons here at all - nothing useful to interact with
            // yet, and the actual scan is about to start the moment
            // this frame finishes being presented.
            float box_w = 400 * g_ui_scale, box_h = 100 * g_ui_scale;
            Rectangle box = {sw / 2 - box_w / 2, sh / 2 - box_h / 2, box_w, box_h};
            draw_window_box(box, "WiFi");
            int prevAlign = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
            GuiLabel({box.x + 12, box.y + 28, box.width - 24, box.height - 40}, "Scanning for networks...");
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, prevAlign);
        } else if (g_wifi_screen.password_prompt) {
            // --- Password entry for the selected network ---
            std::string header = "Connect to \"" + g_wifi_screen.selected_ssid + "\"";
            std::string wrapped_header = word_wrap_text(header, inner_w, GuiGetStyle(DEFAULT, TEXT_SIZE));
            int header_lines = 1;
            for (char c : wrapped_header) if (c == '\n') header_lines++;
            float header_h = (header_lines + 1.5f) * GuiGetStyle(DEFAULT, TEXT_SIZE) * 1.6f;

            float err_h = 0;
            std::string wrapped_err;
            if (!g_wifi_screen.connect_error.empty()) {
                wrapped_err = word_wrap_text(g_wifi_screen.connect_error, inner_w, GuiGetStyle(DEFAULT, TEXT_SIZE));
                int err_lines = 1;
                for (char c : wrapped_err) if (c == '\n') err_lines++;
                err_h = (err_lines + 1.5f) * GuiGetStyle(DEFAULT, TEXT_SIZE) * 1.6f;
            }

            float box_h = 28 + header_h + 12 + err_h + row_h + 12 + btn_h + 12;
            // Same overflow fix as the other two dialogs using this
            // same box-height-capping pattern (see the WiFi network
            // list dialog's own comment on how this was found) -
            // err_h is the one variable-sized element here (the
            // password field/buttons themselves are fixed size), so
            // it's what gets reduced if the cap ever triggers.
            float capped_box_h = std::min(box_h, sh * 0.85f);
            if (capped_box_h < box_h) err_h -= (box_h - capped_box_h);
            box_h = capped_box_h;
            Rectangle box = {sw / 2 - box_w / 2, sh / 2 - box_h / 2, box_w, box_h};

            draw_window_box(box, "WiFi Password");
            int prevAlign = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
            GuiLabel({box.x + 12, box.y + 28, inner_w, header_h}, wrapped_header.c_str());
            float next_y = box.y + 28 + header_h + 12;
            if (!g_wifi_screen.connect_error.empty()) {
                GuiLabel({box.x + 12, next_y, inner_w, err_h}, wrapped_err.c_str());
                next_y += err_h + 12;
            }
            GuiSetStyle(LABEL, TEXT_ALIGNMENT, prevAlign);

            bool password_field_had_focus = (g_active_field == 401);
            password_field(401, {box.x + 12, next_y, inner_w, row_h}, g_wifi_screen.password_buf, sizeof(g_wifi_screen.password_buf));
            next_y += row_h + 12;

            float btn_w = 140 * g_ui_scale;
            // Shared between the explicit "Connect" button click below
            // AND pressing Enter while the password field itself still
            // has focus (checked right here, using the field's focus
            // state from BEFORE this frame's own password_field() call
            // above - that call's own GuiTextBox() already consumes a
            // same-frame Enter press internally to exit edit mode, so
            // checking g_active_field itself post-call would already
            // read as unfocused) - on request, a standard "Enter
            // submits the form" pattern that was missing here
            // (previously Enter while typing the password just exited
            // editing, going nowhere further without an extra Tab/
            // click over to Connect specifically).
            auto do_connect = [&]() {
                try {
                    backend::connect_wifi_network(g_wifi_screen.device, g_wifi_screen.selected_ssid,
                                                   g_wifi_screen.password_buf, log_msg);
                    g_wifi_screen.active = false;
                } catch (const std::exception& e) {
                    g_wifi_screen.connect_error = e.what();
                }
                memset(g_wifi_screen.password_buf, 0, sizeof(g_wifi_screen.password_buf));
            };
            bool connect_clicked = focusable_button(402, {box.x + box.width / 2 - btn_w - 6, next_y, btn_w, btn_h}, "Connect");
            if (connect_clicked || (password_field_had_focus && IsKeyPressed(KEY_ENTER))) {
                do_connect();
            }
            if (focusable_button(403, {box.x + box.width / 2 + 6, next_y, btn_w, btn_h}, "Back")) {
                g_wifi_screen.password_prompt = false;
                g_wifi_screen.connect_error.clear();
                memset(g_wifi_screen.password_buf, 0, sizeof(g_wifi_screen.password_buf));
            }
        } else {
            // --- Network list ---
            // No longer showing a separate "WiFi Networks (wlan0)"
            // header line above the list - on request, since the
            // title bar right above it already says "Select Network"
            // and the device name alone (without also repeating
            // "WiFi Networks") wasn't informative enough on its own to
            // be worth a dedicated line. header_h now just a small,
            // fixed gap between the title bar and the list itself,
            // matching the spacing this dialog already uses elsewhere
            // (12 between other stacked elements) - not zero, so the
            // list doesn't start flush against the title bar.
            float header_h = 12 * g_ui_scale;

            float list_h;
            std::string wrapped_err;
            float row_unit = row_h + row_gap;
            if (!g_wifi_screen.error.empty()) {
                wrapped_err = word_wrap_text(g_wifi_screen.error, inner_w, GuiGetStyle(DEFAULT, TEXT_SIZE));
                int err_lines = 1;
                for (char c : wrapped_err) if (c == '\n') err_lines++;
                list_h = (err_lines + 1.5f) * GuiGetStyle(DEFAULT, TEXT_SIZE) * 1.6f;
            } else {
                // Rebuilt from scratch on request, after the previous
                // "percentage of screen height, then cap box_h, then
                // round list_h back down to a row multiple" chain kept
                // producing real, hard-to-predict edge cases across
                // several rounds of real-hardware reports (a cut-off
                // bottom row, a single invisible-but-clickable network,
                // barely-there scrolling, a large empty gap below only
                // 2 rendered rows, and finally the whole GuiScrollPanel()
                // approach abandoned entirely in favor of
                // manual_scroll_begin() - see its own comment for that
                // full story). No +border_compensation here anymore -
                // that existed specifically to counteract
                // GuiScrollPanel()'s own internal 2*BORDER_WIDTH bounds-
                // to-viewport subtraction, which manual_scroll_begin()
                // simply doesn't do (its own returned/scissor-clipped
                // area IS exactly the bounds passed in, by construction).
                //
                // Directly decides how many rows to show at once
                // instead - capped at a fixed, generous maximum (6)
                // that comfortably fits on any screen this project
                // targets, or the actual network count if fewer are
                // found - then builds list_h directly from that count,
                // with no intermediate screen-percentage step and
                // nothing left to round afterwards, since it's already
                // an exact multiple by construction. Scrolling still
                // works completely normally beyond 6 networks (content_h,
                // computed separately below from the real network
                // count, is what actually drives manual_scroll_begin()'s
                // own scroll range - capping the VISIBLE row count here
                // doesn't cap how many networks the list can hold).
                const int MAX_VISIBLE_ROWS = 6;
                int visible_rows = std::min((int)g_wifi_screen.networks.size(), MAX_VISIBLE_ROWS);
                if (visible_rows < 1) visible_rows = 1;
                list_h = (float)visible_rows * row_unit;
            }

            float box_h = 28 + header_h + 12 + list_h + 24 + btn_h + 12;
            // Still capped against the screen height as a final safety
            // net (e.g. a very short/small display where even 6 rows'
            // worth of list_h wouldn't fit).
            //
            // Feeds back into list_h when this triggers - see this same
            // comment's own longer history in this project for the
            // real bug (a navy-colored block, the Close button's own
            // background, covering the last one or two network rows)
            // that not doing this caused previously. Reconstructs
            // list_h EXACTLY from however much space is actually left
            // once box_h is capped (floor division), then box_h itself
            // is recomputed one more time from that adjusted list_h,
            // so the two stay perfectly consistent no matter what.
            if (g_wifi_screen.error.empty() && box_h > sh * 0.85f) {
                float capped_box_h = sh * 0.85f;
                float available_for_list = capped_box_h - (28 + header_h + 12 + 24 + btn_h + 12);
                int fitted_rows = (int)std::floor(available_for_list / row_unit);
                if (fitted_rows < 1) fitted_rows = 1;
                list_h = (float)fitted_rows * row_unit;
                box_h = 28 + header_h + 12 + list_h + 24 + btn_h + 12;
                // Deliberately NOT re-capped against sh*0.85f again
                // after this - see the directory-URL-picker dialog's
                // own, identical comment for the full reasoning (a
                // real, deliberately extreme test case found this
                // exact spot reintroducing the same overlap bug this
                // whole block exists to fix).
            } else if (!g_wifi_screen.error.empty()) {
                // Error-message case doesn't get the row-based
                // shrink-to-fit treatment above (it isn't row-based at
                // all) - still capped directly the simple way, same as
                // before this whole fix. No Close-button-overlaps-list
                // risk here specifically (a GuiLabel showing wrapped
                // text, not a GuiScrollPanel with individually
                // positioned row buttons), so the simpler cap alone is
                // fine for this specific sub-case.
                box_h = std::min(box_h, sh * 0.85f);
            }
            Rectangle box = {sw / 2 - box_w / 2, sh / 2 - box_h / 2, box_w, box_h};

            draw_window_box(box, "Select Network");

            float list_y = box.y + 28 + header_h + 12;
            if (!g_wifi_screen.error.empty()) {
                int prevAlign2 = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
                GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
                GuiLabel({box.x + 12, list_y, inner_w, list_h}, wrapped_err.c_str());
                GuiSetStyle(LABEL, TEXT_ALIGNMENT, prevAlign2);
            } else {
                static float wifi_scroll_offset = 0;
                float content_h = (float)g_wifi_screen.networks.size() * (row_h + row_gap);
                Rectangle list_rect = {box.x + 12, list_y, inner_w, list_h};
                float fy = manual_scroll_begin(list_rect, content_h, wifi_scroll_offset);
                for (size_t i = 0; i < g_wifi_screen.networks.size(); i++) {
                    auto& net = g_wifi_screen.networks[i];
                    std::string label = (net.connected ? "> " : "") + net.name + "  [" + net.security + "]";
                    if (focusable_button(500 + (int)i, {list_rect.x, fy, list_rect.width - 4, row_h}, label.c_str(), nullptr, nullptr, nullptr, /*no_border=*/true)) {
                        if (net.security == "open") {
                            try {
                                backend::connect_wifi_network(g_wifi_screen.device, net.name, "", log_msg);
                                g_wifi_screen.active = false;
                            } catch (const std::exception& e) {
                                g_wifi_screen.error = e.what();
                            }
                        } else {
                            g_wifi_screen.selected_ssid = net.name;
                            g_wifi_screen.password_prompt = true;
                            g_wifi_screen.connect_error.clear();
                            memset(g_wifi_screen.password_buf, 0, sizeof(g_wifi_screen.password_buf));
                            // Straight to typing the password, no
                            // extra click/Tab needed first - id 401,
                            // matching the password_field() call
                            // itself further down.
                            g_active_field = 401;
                        }
                    }
                    fy += row_h + row_gap;
                }
                manual_scroll_end();
            }

            if (focusable_button(404, {box.x + box.width / 2 - 60 * g_ui_scale, box.y + box.height - btn_h - 12, 120 * g_ui_scale, btn_h}, "Close")) {
                g_wifi_screen.active = false;
            }
        }
    }

    // --- Confirmation dialog ---
    if (g_app.show_confirm) {
        // Scope Tab navigation to just this dialog's own two buttons
        // while it's open - without this, the main form's fields
        // (drawn above, still visible dimmed behind the overlay)
        // would remain in g_field_order too, since draw_ui() draws
        // the whole form unconditionally regardless of show_confirm -
        // Tab would then cycle through both the background form AND
        // the dialog, rather than being scoped to the modal the way
        // a confirmation dialog should be. handle_tab_navigation()
        // runs once at the very end of draw_ui(), after this block,
        // so clearing here discards whatever the form already added
        // this same frame and leaves only what follows.
        g_field_order.clear();
        DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.5f));
        std::string msg;
        if (g_app.install_mode == 0) {
            std::string dev = g_app.rootfs_sel.get_device();
            std::string mp = g_app.rootfs_sel.get_mountpoint();
            std::string rootfs_src = current_source_display(g_app.rootfs_source_kind, g_app.rootfs_local_path, g_app.rootfs_http_url, g_app.rootfs_url);
            msg = "Source: " + rootfs_src + "\n\n"
                  "WARNING: All data on " + dev + " (" + mp + ") will be erased!\n"
                  "Foreign mountpoints inside it (e.g. /home) are skipped automatically.";
            if (g_app.kernel_enabled) {
                std::string kernel_src = current_source_display(g_app.kernel_source_kind, g_app.kernel_local_path, g_app.kernel_http_url, g_app.kernel_url);
                msg += "\nThe kernel on " + g_app.boot_sel.get_device() + " will also be updated, from: " + kernel_src;
            }
        } else {
            std::string dev = g_app.disk_sel.get_device();
            std::string wic_src = current_source_display(g_app.wic_source_kind, g_app.wic_local_path, g_app.wic_http_url, g_app.wic_url);
            msg = "Source: " + wic_src + "\n\n"
                  "WARNING: " + dev + " WILL BE COMPLETELY OVERWRITTEN!\n"
                  "The entire disk, including its partition table, will be lost -\n"
                  "including /home and any other partitions on it.\n"
                  "This is not an update, but a fresh installation.";
        }
        int btn_active = -1;
        // Box height sized to the actual wrapped message content, not
        // a fixed guess - a fixed size either wastes space for a
        // short message or (confirmed via a real rendered-pixel test)
        // cuts content off/overflows the window border for a longer
        // one, e.g. a long source URL. Width stays fixed (the wrap
        // width itself), only height adapts. Capped at 80% of screen
        // height as a sanity ceiling for a pathologically long
        // message - not expected in practice, but better to cap than
        // let a dialog exceed the screen entirely.
        float box_w = 560 * g_ui_scale;
        float msg_area_w = box_w - 24;
        std::string wrapped_msg = word_wrap_text(msg, msg_area_w, GuiGetStyle(DEFAULT, TEXT_SIZE));
        int msg_line_count = 1;
        for (char c : wrapped_msg) if (c == '\n') msg_line_count++;
        float msg_line_h = GuiGetStyle(DEFAULT, TEXT_SIZE) * 1.6f; // matches the log panel's own proven value (see its comments) - an earlier, unverified 1.3x guess was confirmed too small via a real rendered-pixel test (content still overflowed the computed box)
        float msg_area_h = (msg_line_count + 1.5f) * msg_line_h; // +1.5 lines of slack - an exact fit still clipped the last line in a real rendered-pixel test (GuiLabel's actual internal line spacing doesn't precisely match msg_line_h)
        // Space reserved at the bottom for the button row - matches
        // the buttons' own actual size (see their own comment further
        // down: same fixed size as this app's other buttons, not
        // raygui's GuiMessageBox() proportions this replaced) plus
        // padding above/below. Computed here, ahead of both box_h and
        // the message GuiLabel() below, so all three (box height,
        // label height, and the buttons themselves) stay consistent
        // with each other - found via reviewing this exact "capped
        // height without updating downstream space allocations"
        // pattern already fixed once before, for the WiFi network
        // list dialog (see that dialog's own comments) - the same
        // mismatch here would have made the message text overlap the
        // now-taller buttons.
        float btn_row_h = (ROW + 6) + 12 * g_ui_scale * 2;
        float box_h = std::min(sh * 0.8f, 28 + msg_area_h + btn_row_h);
        box_h = std::max(box_h, 150 * g_ui_scale); // floor: never smaller than a short message needs anyway
        Rectangle box = {sw / 2 - box_w / 2, sh / 2 - box_h / 2, box_w, box_h};
        // Manual layout replacing GuiMessageBox() - same constants it
        // uses internally (RAYGUI_MESSAGEBOX_BUTTON_HEIGHT=24,
        // RAYGUI_MESSAGEBOX_BUTTON_PADDING=12), so this looks
        // identical, but the buttons are now focusable_button() calls
        // instead of GuiMessageBox's own internal plain GuiButton()
        // calls - GuiMessageBox as a composite control had no
        // keyboard focus/activation of its own, same gap as
        // GuiButton/GuiCheckBox/GuiToggleGroup elsewhere in this app.
        draw_window_box(box, "Confirmation");
        int prevAlign = GuiGetStyle(LABEL, TEXT_ALIGNMENT);
        GuiSetStyle(LABEL, TEXT_ALIGNMENT, TEXT_ALIGN_CENTER);
        GuiLabel({box.x + 12, box.y + 28, msg_area_w, box.height - 28 - btn_row_h}, wrapped_msg.c_str());
        GuiSetStyle(LABEL, TEXT_ALIGNMENT, prevAlign);
        // Same fixed size as this app's other buttons (Install/Reboot/
        // Exit/WiFi, e.g. line ~1511's own "140 * g_ui_scale, ROW + 6")
        // rather than raygui's own GuiMessageBox() proportions
        // (RAYGUI_MESSAGEBOX_BUTTON_HEIGHT=24 unscaled, width
        // stretched to half the dialog) - those made these two
        // buttons noticeably wider and thinner than every other
        // button in the app, found via a direct visual consistency
        // report.
        float mb_btn_h = ROW + 6, mb_pad = 12 * g_ui_scale;
        float mb_btn_w = 140 * g_ui_scale;
        float btn_pair_w = mb_btn_w * 2 + mb_pad;
        float btn_start_x = box.x + box.width / 2 - btn_pair_w / 2;
        Rectangle install_btn = {btn_start_x, box.y + box.height - mb_btn_h - 12 * g_ui_scale, mb_btn_w, mb_btn_h};
        Rectangle cancel_btn = {install_btn.x + mb_btn_w + mb_pad, install_btn.y, mb_btn_w, mb_btn_h};
        if (focusable_button(210, install_btn, "Install")) btn_active = 1;
        if (focusable_button(211, cancel_btn, "Cancel")) btn_active = 2;
        if (btn_active == -1) {
            if (IsKeyPressed(KEY_ENTER)) btn_active = 1;
            else if (IsKeyPressed(KEY_ESCAPE)) btn_active = 2;
        }
        if (btn_active == 0 || btn_active == 2) {
            g_app.show_confirm = false;
        } else if (btn_active == 1) {
            g_app.show_confirm = false;
            g_cancel_requested = false;
            {
                std::lock_guard<std::mutex> lk(g_state.mutex);
                g_state.worker_running = true;
                g_state.progress_pct = 0;
                g_state.status = "Starting...";
            }
            AppState snapshot = g_app;
            if (g_app.install_mode == 0) std::thread(run_pipeline, snapshot).detach();
            else std::thread(run_wic_pipeline, snapshot).detach();
        }
    }

    handle_tab_navigation();

    // Custom cursor: PLATFORM_DRM draws no system cursor. Must be
    // drawn last so it stays on top.
    Vector2 mp = GetMousePosition();
    DrawTriangle((Vector2){mp.x, mp.y}, (Vector2){mp.x, mp.y + 16 * g_ui_scale}, (Vector2){mp.x + 10 * g_ui_scale, mp.y + 12 * g_ui_scale}, BLACK);
    DrawTriangleLines((Vector2){mp.x, mp.y}, (Vector2){mp.x, mp.y + 16 * g_ui_scale}, (Vector2){mp.x + 10 * g_ui_scale, mp.y + 12 * g_ui_scale}, RAYWHITE);
}

// ---------------------------------------------------------------------
// Dark theme: colors from noctalia-shell's "Noctalia" default palette
// (github.com/noctalia-dev/noctalia-shell, builtin_palettes.cpp).
// ---------------------------------------------------------------------

static Color g_bgColor;

static void apply_noctalia_dark_theme() {
    const Color primary        = GetColor(0xfff59bffU);
    const Color onPrimary      = GetColor(0x0e0e43ffU);
    const Color secondary      = GetColor(0xa9aefeffU);
    const Color surface        = GetColor(0x070722ffU);
    const Color onSurface      = GetColor(0xf3edf7ffU);
    const Color surfaceVariant = GetColor(0x11112dffU);
    const Color onSurfaceVar   = GetColor(0x7c80b4ffU);
    const Color outline        = GetColor(0x21215fffU);
    const Color hover          = GetColor(0x9bfeceffU);

    g_bgColor = surface;

    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(outline));
    // Deliberately the DARKER of the two lilac tones now (swapped on
    // request - see g_focusRingColor's own comment, much earlier in
    // this file, for the full story) - this is the general, always-
    // visible button border, distinct from the keyboard-focus ring
    // itself (g_focusRingColor, bright) drawn on top of a focused
    // control specifically. Keeping this one less prominent than that
    // is what makes the actual focus indicator stand out clearly
    // against it, rather than blending in with borders every other
    // button already has all the time regardless of focus.
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(GetColor(0x6d71a5ffU)));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(surfaceVariant));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(onSurface));

    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(secondary));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt(hover));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(onPrimary));

    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(primary));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt(primary));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(onPrimary));

    GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, ColorToInt(outline));
    GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, ColorToInt(surface));
    GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, ColorToInt(onSurfaceVar));

    // raygui's GuiTextBox() sets state = STATE_PRESSED unconditionally
    // whenever editMode is true (not just during an actual mouse
    // click, confirmed directly in raygui.h) - meaning while a field
    // is being edited, its own background used to use BASE_COLOR_
    // PRESSED (our DEFAULT override above: primary/yellow), and the
    // cursor itself is drawn using BORDER_COLOR_PRESSED.
    //
    // Reconsidered further on request: the yellow edit-mode background
    // itself was found visually jarring/unwanted, not just a cursor-
    // contrast problem - the blinking-cursor confirmation the person
    // asked to rely on instead doesn't actually blink in raygui either
    // (GuiTextBox()'s own blink logic is fully commented out in its
    // real source - "//if (autoCursorMode || ((blinkCursorFrameCounter
    // /40)%2 == 0))" - always drawn, never toggled), but stays a
    // clear, static, on/off presence indicator regardless, which is
    // what actually matters here.
    //
    // TEXTBOX-specific overrides only (not touching the global DEFAULT
    // PRESSED colors, which stay yellow for a BUTTON's own brief,
    // genuinely momentary click-flash elsewhere - that's a different,
    // still-wanted use of the same underlying state) - background now
    // stays the SAME as the field's own normal state (no fill change
    // at all while editing), and the cursor/border switches to plain
    // white instead of onPrimary - onPrimary (dark navy) was
    // purpose-built to read against the OLD yellow background
    // specifically; with that background gone, a dark cursor on this
    // theme's own dark surface would just reintroduce the exact same
    // invisibility problem this was originally fixing, on every text
    // field rather than just the one (password_field()) it was first
    // found on.
    GuiSetStyle(TEXTBOX, BASE_COLOR_PRESSED, ColorToInt(surfaceVariant));
    GuiSetStyle(TEXTBOX, TEXT_COLOR_PRESSED, ColorToInt(onSurface));
    GuiSetStyle(TEXTBOX, BORDER_COLOR_PRESSED, ColorToInt(WHITE));
    // Belt-and-braces beyond the PRESSED override above (re-examined
    // on request after a real report that yellow was still showing,
    // on both click AND Tab specifically): explicitly pins TEXTBOX's
    // own NORMAL and FOCUSED base colors to this exact same value too
    // (FOCUSED already renders BLANK/transparent per GuiTextBox()'s
    // own source rather than actually reading this style property at
    // all, and NORMAL already matched via DEFAULT's own cascade - but
    // setting all three explicitly, to the identical value, removes
    // any possible doubt or hidden path rather than relying on that
    // chain of reasoning holding in every case).
    GuiSetStyle(TEXTBOX, BASE_COLOR_NORMAL, ColorToInt(surfaceVariant));
    GuiSetStyle(TEXTBOX, BASE_COLOR_FOCUSED, ColorToInt(surfaceVariant));

    // A SEPARATE issue from the cursor one above: on plain hover (not
    // editing - raygui's GuiTextBox() sets state = STATE_FOCUSED here,
    // confirmed directly in raygui.h), the text colour switches to
    // TEXT_COLOR_FOCUSED, but the field's own background stays BLANK/
    // transparent for that state (only STATE_PRESSED/STATE_DISABLED
    // get an actual fill) - showing through to this app's own dark
    // backdrop. DEFAULT's TEXT_COLOR_FOCUSED is onPrimary (dark navy,
    // meant for the LIGHT hover background other controls like
    // buttons get via BASE_COLOR_FOCUSED=hover/mint-green) - with no
    // corresponding light background here, dark-navy text on this
    // project's already-dark surface is barely readable. Found via
    // direct user report + confirmed by reading raygui's actual
    // source. Lilac (secondary) instead - reads clearly against the
    // dark backdrop, unlike onPrimary.
    GuiSetStyle(TEXTBOX, TEXT_COLOR_FOCUSED, ColorToInt(secondary));

    GuiSetStyle(DEFAULT, LINE_COLOR, ColorToInt(outline));
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(surface));

    // GuiCheckBox only applies the hover/pressed BACKGROUND to the
    // checkbox glyph itself, not to the label text next to it - but
    // DEFAULT's TEXT_COLOR_FOCUSED/PRESSED (a dark navy, tuned for
    // text sitting on top of that light hover background) still gets
    // used for the label too, against the app's normal dark
    // background. Dark-on-dark = invisible label while hovering.
    // Override just for CHECKBOX so the label stays readable.
    // GuiCheckBox draws its own box border/check via CHECKBOX style,
    // but its label TEXT via the LABEL control's style (verified in
    // raygui.h: "GuiDrawText(text, ..., GetColor(GuiGetStyle(LABEL,
    // TEXT + state*3)))") - NOT CHECKBOX, despite what the checkbox's
    // own visible highlight suggests. DEFAULT's TEXT_COLOR_FOCUSED/
    // PRESSED (dark navy, tuned for text on top of the light hover
    // background) still gets picked up via LABEL's fallback to
    // DEFAULT, rendered against the app's normal dark background -
    // invisible. GuiComboBox, by contrast, correctly uses its own
    // COMBOBOX style for its text, so that override (still present
    // below) was already correct.
    GuiSetStyle(LABEL, TEXT_COLOR_FOCUSED, ColorToInt(onSurface));
    GuiSetStyle(LABEL, TEXT_COLOR_PRESSED, ColorToInt(onSurface));
    // CHECKBOX itself (the check-mark fill, via GuiGetStyle(CHECKBOX,
    // TEXT + state*3)) - separate from LABEL above, which only covers
    // the text next to it. Without this, a CHECKED box's fill fell
    // back to DEFAULT's FOCUSED/PRESSED colors (dark navy, tuned for
    // buttons' light hover background) - flipping from white
    // (resting) to dark (hovering) and back. Confirmed with a real
    // pixel-level render test (raylib PLATFORM_DESKTOP under Xvfb):
    // without this fix, center pixel of a checked box went from
    // RGB(243,237,247) resting to RGB(14,14,67) hovered; with it,
    // both stay RGB(243,237,247).
    GuiSetStyle(CHECKBOX, TEXT_COLOR_FOCUSED, ColorToInt(onSurface));
    GuiSetStyle(CHECKBOX, TEXT_COLOR_PRESSED, ColorToInt(onSurface));
    GuiSetStyle(COMBOBOX, TEXT_COLOR_FOCUSED, ColorToInt(onSurface));
    GuiSetStyle(COMBOBOX, TEXT_COLOR_PRESSED, ColorToInt(onSurface));
    // GuiComboBox() - unlike GuiTextBox() - genuinely fills its own
    // background using BASE + state*3 for EVERY state including
    // FOCUSED/PRESSED (confirmed directly in raygui.h - no BLANK-fill
    // exception here), so it was still falling back to DEFAULT's own
    // BASE_COLOR_FOCUSED (hover, mint green) whenever hovered/clicked -
    // this is what the partition-selector dropdowns (draw_partition_
    // selector(), GuiComboBox() call) actually use for their own
    // clickable "cycle through found partitions" fields. Found via
    // explicit user report - other text fields' own similar fix
    // didn't cover this, different widget/style category entirely.
    // Pinned to the same navy as everywhere else now, not a new color.
    GuiSetStyle(COMBOBOX, BASE_COLOR_NORMAL, ColorToInt(surfaceVariant));
    GuiSetStyle(COMBOBOX, BASE_COLOR_FOCUSED, ColorToInt(surfaceVariant));
    GuiSetStyle(COMBOBOX, BASE_COLOR_PRESSED, ColorToInt(surfaceVariant));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_FOCUSED, ColorToInt(onSurface));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_PRESSED, ColorToInt(onSurface));
}

// Queries the display's preferred/native resolution directly via
// libdrm, instead of relying on raylib's own InitWindow(0,0,...)
// fallback - that picks up the CURRENTLY active CRTC mode (often a
// low-resolution boot/console default), not the mode the display
// reports as preferred via EDID (DRM_MODE_TYPE_PREFERRED).
//
// TVs are less reliable at EDID/HDMI handshake than monitors: the
// PREFERRED flag isn't always set, and mode order isn't guaranteed
// "best first". Hence two extra measures: (1) also compare the
// largest available mode by area as a candidate, (2) briefly retry if
// the connector isn't "connected" yet on the first attempt (late
// HDMI handshake).
static bool try_get_preferred_display_mode(int* outW, int* outH) {
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        if (!res) { close(fd); continue; }
        for (int c = 0; c < res->count_connectors; c++) {
            drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
            if (!conn) continue;
            if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                drmModeModeInfo best = conn->modes[0]; // fallback: first mode
                long bestArea = (long)best.hdisplay * best.vdisplay;
                bool foundPreferred = false;
                for (int m = 0; m < conn->count_modes; m++) {
                    if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                        best = conn->modes[m];
                        foundPreferred = true;
                        break;
                    }
                }
                if (!foundPreferred) {
                    for (int m = 0; m < conn->count_modes; m++) {
                        long area = (long)conn->modes[m].hdisplay * conn->modes[m].vdisplay;
                        if (area > bestArea) { best = conn->modes[m]; bestArea = area; }
                    }
                }
                *outW = best.hdisplay;
                *outH = best.vdisplay;
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                close(fd);
                return true;
            }
            drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);
        close(fd);
    }
    return false;
}

static bool get_preferred_display_mode(int* outW, int* outH) {
    for (int attempt = 0; attempt < 10; attempt++) {
        if (try_get_preferred_display_mode(outW, outH)) return true;
        usleep(300000); // 300ms
    }
    return false;
}

// Debug-only screenshot (config.toml "debug_mode = true", then F12) -
// on request, so a real capture can be taken directly on the actual
// target instead of reconstructing the UI elsewhere. Called from
// end_frame() right before its own EndDrawing(), i.e. after every
// draw call of the frame has landed but before the buffer swap - the
// one point where the fully composed frame is reliably readable on
// both the software and the hardware-accelerated backend (after the
// swap, on real double-buffered DRM/EGL the readable buffer is no
// longer the one just drawn).
//
// ImageFlipVertical() only on the software backend - a real, confirmed
// raylib bug there (separate from the swScissor() one this layer
// already patches, see 0006-*.patch): rlReadScreenPixels() (rlgl.h,
// shared code) always applies its own vertical un-flip on top of
// glReadPixels(), correct for real hardware OpenGL (which genuinely
// returns bottom-up data) but wrong for the software rasterizer's own
// swReadPixels(), whose sw_framebuffer_output_fast() already returns
// top-down - the two combine into a double flip, so
// LoadImageFromScreen() comes back upside down. Confirmed by building
// and running that exact rasterizer directly (raylib's own
// PLATFORM=Memory + OPENGL_VERSION=Software): a marker drawn at screen
// (0,0) ends up at (0, height-1) via LoadImageFromScreen(), yet at the
// correct (0,0) via rlCopyFramebuffer() (the real DRM display path,
// which calls swReadPixels() directly and is therefore unaffected).
// rlGetVersion() == RL_OPENGL_SOFTWARE is the clean runtime check for
// "are we on that backend", so the same binary stays correct if this
// layer is ever built with the opengl DISTRO_FEATURE on instead.
static void maybe_take_screenshot() {
    if (!g_cfg_debug_mode || !IsKeyPressed(KEY_F12)) return;
    Image img = LoadImageFromScreen();
    if (rlGetVersion() == RL_OPENGL_SOFTWARE) ImageFlipVertical(&img);
    char name[128];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(name, sizeof(name), "screenshot-%Y%m%d-%H%M%S.png", &tmv);
    std::string path = download_staging_dir() + "/" + name;
    bool ok = ExportImage(img, path.c_str());
    UnloadImage(img);
    log_msg(ok ? ("Screenshot saved: " + path) : ("Screenshot FAILED: " + path));
}

// Wraps BeginDrawing()/EndDrawing() with the low-res render-to-texture
// + upscale-blit dance when g_low_res_active, otherwise draws directly
// to the screen as before - draw_ui()/draw_file_browser() don't need
// to know or care which mode is active, they just use g_canvas_w/h.
static void begin_frame() {
    if (g_low_res_active) BeginTextureMode(g_low_res_target);
    else BeginDrawing();
}
static void end_frame() {
    if (g_low_res_active) {
        EndTextureMode();
        BeginDrawing();
        ClearBackground(BLACK);
        // Render textures are Y-flipped relative to the screen
        // (raylib/OpenGL convention) - negative source height flips
        // it back the right way up for the blit.
        DrawTexturePro(g_low_res_target.texture,
                        {0, 0, (float)g_canvas_w, -(float)g_canvas_h},
                        {0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()},
                        {0, 0}, 0.0f, WHITE);
        maybe_take_screenshot();
        EndDrawing();
    } else {
        maybe_take_screenshot();
        EndDrawing();
    }
}

int main(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "WARNING: not started as root - mount/umount/tar will probably fail.\n");
    }

    activate_vt(1); // must match TTYPath=/dev/tty1 in the systemd service

    // Config file is optional: missing file or missing keys just keep
    // the hardcoded fallbacks below. Loaded early (before InitWindow)
    // so low_res_height (see below) is available before the display
    // mode / render target setup that needs it.
    auto cfg = backend::read_simple_toml("/etc/yocto-rootfs-updater-raylib/config.toml");
    auto cfg_get = [&](const char* key, const std::string& fallback) {
        auto it = cfg.find(key);
        return (it != cfg.end() && !it->second.empty()) ? it->second : fallback;
    };

    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    int prefW = 0, prefH = 0;
    if (!get_preferred_display_mode(&prefW, &prefH)) {
        log_msg("WARNING: preferred resolution could not be determined via libdrm, using raylib's default.");
        prefW = 0;
        prefH = 0;
    } else {
        log_msg("Preferred resolution detected: " + std::to_string(prefW) + "x" + std::to_string(prefH));
    }
    InitWindow(prefW, prefH, "Yocto RootFS Updater");

    // Low-resolution rendering, if configured (see g_low_res_active's
    // own comment above for the full reasoning) - draw into a smaller
    // offscreen texture, upscaled to the real screen with a single
    // blit, instead of drawing every widget at full native
    // resolution every frame. Same aspect ratio as the detected
    // display mode, just scaled down to the configured height.
    int cfg_low_res_height = atoi(cfg_get("low_res_height", "0").c_str());
    int screenW = GetScreenWidth(), screenH = GetScreenHeight();
    if (cfg_low_res_height > 0 && cfg_low_res_height < screenH) {
        g_canvas_h = cfg_low_res_height;
        g_canvas_w = (int)((double)screenW * cfg_low_res_height / screenH);
        g_low_res_target = LoadRenderTexture(g_canvas_w, g_canvas_h);
        if (g_low_res_target.id != 0) {
            SetTextureFilter(g_low_res_target.texture, TEXTURE_FILTER_BILINEAR);
            g_low_res_active = true;
            SetMouseScale((float)g_canvas_w / (float)screenW, (float)g_canvas_h / (float)screenH);
            log_msg("Low-resolution rendering active: drawing at " + std::to_string(g_canvas_w) + "x" +
                    std::to_string(g_canvas_h) + ", upscaled to " + std::to_string(screenW) + "x" +
                    std::to_string(screenH) + ".");
        } else {
            log_msg("WARNING: could not create low-resolution render target, using full resolution instead.");
            g_canvas_w = screenW;
            g_canvas_h = screenH;
        }
    } else {
        g_canvas_w = screenW;
        g_canvas_h = screenH;
    }

    // Scale all UI dimensions relative to 1080p as the reference
    // resolution our fixed pixel values (ROW=30, MARGIN=20, etc.) were
    // originally tuned for - otherwise a 4K display renders everything
    // correctly but tiny (same pixel counts, 4x the physical pixels).
    // Height-based rather than width-based to stay sane on ultrawide
    // aspect ratios; clamped to avoid degenerate cases on very small
    // or unusually reported resolutions. Uses g_canvas_h (the internal
    // drawing resolution), not the physical screen height directly -
    // with low-res rendering active, the UI should be scaled for the
    // smaller canvas it's actually being drawn into, not the larger
    // physical display it gets upscaled to afterwards.
    g_ui_scale = (float)g_canvas_h / 1080.0f;
    if (g_ui_scale < 0.5f) g_ui_scale = 0.5f;
    if (g_ui_scale > 4.0f) g_ui_scale = 4.0f;
    log_msg("UI scale: " + std::to_string(g_ui_scale) + "x (at " +
             std::to_string(g_canvas_w) + "x" + std::to_string(g_canvas_h) + ")");
    SetTargetFPS(30);

    // Real TTF font instead of raylib's blocky bitmap default.
    // Codepoints 32-255 cover Latin-1, including German umlauts.
    static int codepoints[224];
    for (int i = 0; i < 224; i++) codepoints[i] = 32 + i;
    Font uiFont = LoadFontEx("/usr/share/fonts/ttf/LiberationSans-Regular.ttf", 32, codepoints, 224);
    if (uiFont.texture.id != 0) {
        SetTextureFilter(uiFont.texture, TEXTURE_FILTER_BILINEAR);
        GuiSetFont(uiFont);
    } else {
        log_msg("WARNING: TTF font not found, falling back to raylib's bitmap default.");
    }

    // Separate monospace font just for the log panel - it was using
    // plain DrawText() with no font argument, which falls back to
    // raylib's own built-in default font (a distinctive, somewhat
    // decorative bitmap font meant for game UIs) rather than either
    // TTF font loaded here - hard to read for dense log text. Same
    // liberation-fonts package already depended on, no new
    // dependency needed.
    Font logFont = LoadFontEx("/usr/share/fonts/ttf/LiberationMono-Regular.ttf", 32, codepoints, 224);
    if (logFont.texture.id != 0) {
        SetTextureFilter(logFont.texture, TEXTURE_FILTER_BILINEAR);
    } else {
        log_msg("WARNING: Monospace TTF font not found, log panel falling back to the UI font.");
        logFont = uiFont;
    }
    g_log_font = logFont;

    GuiSetStyle(DEFAULT, TEXT_SIZE, (int)(16 * g_ui_scale));
    apply_noctalia_dark_theme();

    // cfg/cfg_get already loaded earlier (before InitWindow/the
    // low-res render target setup, which needs low_res_height from
    // it) - reused here, not reloaded.
    std::string cfg_rootfs_mp = cfg_get("rootfs_mountpoint", "/mnt/target");
    std::string cfg_boot_mp = cfg_get("boot_mountpoint", "/mnt/boot");
    // Default filebrowser_start_dir: if storage-partition-helper (this
    // layer's own systemd service) already mounted its partition for
    // large local payload files, start the file browser there instead
    // of "/" - no config.toml edit needed for the common case. An
    // explicit filebrowser_start_dir in config.toml still wins.
    std::string browser_dir_fallback = backend::is_mountpoint("/mnt/storage") ? "/mnt/storage" : "/";
    // With a second storage mount present - initramfs-home-mount
    // found a "home"-labeled partition on another disk as well and
    // put it at /mnt/storage2 - start one level up instead, where
    // /mnt lists both of them (and the usb-<dev> mounts) side by
    // side: with more than one of them, which one to take IS the
    // choice to make, and starting inside just one would hide the
    // other behind an upward navigation there is no reason to expect.
    if (backend::is_mountpoint("/mnt/storage2")) browser_dir_fallback = "/mnt";
    g_cfg_filebrowser_start_dir = cfg_get("filebrowser_start_dir", browser_dir_fallback);
    std::string cfg_kernel_target = cfg_get("kernel_target_name", "bzImage");
    // Unlike rootfs_mountpoint/boot_mountpoint (internal, temporary
    // detail - "Mount point" was removed from the GUI entirely, see
    // draw_partition_selector()'s show_mountpoint=false above),
    // excludes has no sensible one-size-fits-all default and stays
    // editable per run in the GUI - this only pre-fills it, avoiding
    // re-typing the same list every single time for someone who
    // always wants the same paths preserved (e.g. /home,
    // /etc/hostname), while still leaving it fully overridable for a
    // one-off run that needs something different.
    std::string cfg_excludes_default = cfg_get("excludes_default", "");
    if (!cfg_excludes_default.empty()) {
        strncpy(g_app.excludes_buf, cfg_excludes_default.c_str(), sizeof(g_app.excludes_buf) - 1);
    }
    // Applies regardless of source kind (local/HTTP/HTTPS) - the
    // SHA256 fields themselves aren't tied to a specific source type
    // either (see their own GUI code further up). Best suited for a
    // stable "golden image" that rarely changes - a frequently
    // rebuilt dev payload would need this updated on every single
    // build to stay accurate, at which point it's arguably no safer
    // than leaving it blank and pasting the hash in manually per run.
    // Still fully overridable/clearable per run in the GUI either
    // way, same as every other _default config.toml entry.
    std::string cfg_rootfs_sha256_default = cfg_get("rootfs_sha256_default", "");
    if (!cfg_rootfs_sha256_default.empty()) {
        strncpy(g_app.rootfs_sha256, cfg_rootfs_sha256_default.c_str(), sizeof(g_app.rootfs_sha256) - 1);
    }
    std::string cfg_wic_sha256_default = cfg_get("wic_sha256_default", "");
    if (!cfg_wic_sha256_default.empty()) {
        strncpy(g_app.wic_sha256, cfg_wic_sha256_default.c_str(), sizeof(g_app.wic_sha256) - 1);
    }
    // Third of the three - previously missing entirely (rootfs and
    // wic both had this, kernel never did) despite the kernel image
    // update path otherwise mirroring rootfs's own structure closely.
    // Found via a direct user report.
    std::string cfg_kernel_sha256_default = cfg_get("kernel_sha256_default", "");
    if (!cfg_kernel_sha256_default.empty()) {
        strncpy(g_app.kernel_sha256, cfg_kernel_sha256_default.c_str(), sizeof(g_app.kernel_sha256) - 1);
    }
    g_app.kernel_enabled = cfg_get("kernel_enabled_default", "true") != "false";
    g_cfg_debug_mode = cfg_get("debug_mode", "false") == "true";
    g_wifi_available = (access("/usr/bin/iwctl", X_OK) == 0);
    if (g_wifi_available) log_msg("WiFi support detected (iwctl present).");
    // "/mnt/storage/" + local_default_dir - only if the config key is
    // actually set at all (cfg_get's own fallback "" here doubles as
    // that opt-in check, matching "natuerlich nur wenn ... gesetzt
    // wurden" from the original request - not on by default just
    // because /mnt/storage happens to exist).
    std::string cfg_local_default_dir = cfg_get("local_default_dir", "");
    if (!cfg_local_default_dir.empty()) {
        std::string combined = "/mnt/storage/" + cfg_local_default_dir;
        g_cfg_local_default_dir = combined;
        log_msg("local_default_dir configured: " + combined);
    }
    // Full URL prefix (e.g. "http://192.168.1.50:8000/builds/"), used
    // as-is - NOT combined with /mnt/storage (that's a local
    // filesystem concept, meaningless for a URL). Shared between the
    // HTTP and HTTPS tabs deliberately (both would typically point at
    // the same network location, just a different scheme) rather than
    // a separate https_default_dir - if it's a directory-style URL
    // (ends with "/"), backend::resolve_possible_directory_url()
    // already resolves it to the latest matching file at download
    // time, same as typing it in manually would.
    std::string cfg_http_default_dir = cfg_get("http_default_dir", "");
    if (!cfg_http_default_dir.empty()) {
        log_msg("http_default_dir configured: " + cfg_http_default_dir);
    }
    log_msg("Configuration loaded (" + std::to_string(cfg.size()) + " values from config.toml).");
    if (browser_dir_fallback != "/" && !cfg.count("filebrowser_start_dir")) {
        log_msg("Storage partition detected, file browser will start in " + browser_dir_fallback + ".");
    }

    // Auto-pick the most recently modified matching file already
    // sitting in the default browser directory, or a removable USB
    // stick this image's own /init may have mounted (see
    // discover_local_file_candidate_dirs()), if any - the common
    // case (a stick built via build-payload-image.sh, a file dropped
    // into /mnt/storage from a desktop system, or a separate USB
    // stick with the file in its root directory) then needs no
    // manual browsing at all. Only pre-fills the path text field;
    // still fully editable/re-browsable, this is a convenience
    // default, not a lock-in.
    auto local_file_candidate_dirs = discover_local_file_candidate_dirs();
    std::string auto_rootfs = find_latest_matching_file(local_file_candidate_dirs, {".tar.gz", ".tgz"});
    if (!auto_rootfs.empty()) {
        strncpy(g_app.rootfs_local_path, auto_rootfs.c_str(), sizeof(g_app.rootfs_local_path) - 1);
        log_msg("Auto-selected local rootfs: " + auto_rootfs);
    }
    std::string auto_wic = find_latest_matching_file(local_file_candidate_dirs, {".wic"});
    if (!auto_wic.empty()) {
        strncpy(g_app.wic_local_path, auto_wic.c_str(), sizeof(g_app.wic_local_path) - 1);
        log_msg("Auto-selected local wic image: " + auto_wic);
    }
    std::string auto_kernel;
    if (!auto_rootfs.empty()) auto_kernel = sibling_kernel_for(auto_rootfs, cfg_kernel_target);
    if (auto_kernel.empty() && !auto_wic.empty()) auto_kernel = sibling_kernel_for(auto_wic, cfg_kernel_target);
    if (auto_kernel.empty()) auto_kernel = find_exact_named_file(local_file_candidate_dirs, cfg_kernel_target);
    if (!auto_kernel.empty()) {
        strncpy(g_app.kernel_local_path, auto_kernel.c_str(), sizeof(g_app.kernel_local_path) - 1);
        log_msg("Auto-selected local kernel image: " + auto_kernel);
    }

    // http_default_dir prefill - same "only if actually configured"
    // opt-in as local_default_dir above, applied consistently across
    // all three source-kind toggles (rootfs/kernel/wic) added
    // alongside the HTTP tab itself. Only pre-fills the text field
    // (both HTTP and HTTPS URL fields get the same value, per its own
    // comment above); still fully editable, same convenience-default-
    // not-lock-in spirit as the local auto-select above.
    if (!cfg_http_default_dir.empty()) {
        strncpy(g_app.rootfs_http_url, cfg_http_default_dir.c_str(), sizeof(g_app.rootfs_http_url) - 1);
        strncpy(g_app.rootfs_url, cfg_http_default_dir.c_str(), sizeof(g_app.rootfs_url) - 1);
        strncpy(g_app.kernel_http_url, cfg_http_default_dir.c_str(), sizeof(g_app.kernel_http_url) - 1);
        strncpy(g_app.kernel_url, cfg_http_default_dir.c_str(), sizeof(g_app.kernel_url) - 1);
        strncpy(g_app.wic_http_url, cfg_http_default_dir.c_str(), sizeof(g_app.wic_http_url) - 1);
        strncpy(g_app.wic_url, cfg_http_default_dir.c_str(), sizeof(g_app.wic_url) - 1);
    }

    g_app.rootfs_sel.filter = PartitionFilter::RootfsLike;
    g_app.rootfs_sel.refresh();
    strncpy(g_app.rootfs_sel.mountpoint_buf, cfg_rootfs_mp.c_str(), sizeof(g_app.rootfs_sel.mountpoint_buf));
    g_app.boot_sel.filter = PartitionFilter::BootLike;
    g_app.boot_sel.refresh();
    strncpy(g_app.boot_sel.mountpoint_buf, cfg_boot_mp.c_str(), sizeof(g_app.boot_sel.mountpoint_buf));
    strncpy(g_app.kernel_target_name, cfg_kernel_target.c_str(), sizeof(g_app.kernel_target_name));
    g_app.disk_sel.filter = PartitionFilter::DisksOnly;
    g_app.disk_sel.refresh();

    if (geteuid() != 0) log_msg("WARNING: not started as root.");

    double reboot_requested_at = -1.0;
    bool ever_rendered = false;
    while (true) {
        // WindowShouldClose() is raylib's ESC/close-signal check - but
        // WindowShouldClose()'s own real DRM-backend implementation
        // (confirmed directly in rcore_drm.c's own source) is level-
        // triggered, not edge-triggered - true for as long as the
        // exit key stays physically held down, not a single-frame
        // "was it just pressed this frame" check. Used here ONLY for
        // detecting a platform init failure before any frame has ever
        // been rendered (see ever_rendered's own comment further
        // below for what happens next in that case) - genuinely safe
        // to rely on for that one specific, narrow purpose regardless
        // of platform/input state.
        //
        // ESC no longer triggers a reboot on its own at all anymore -
        // removed entirely on request, after a real, reported bug
        // this same mechanism caused: IsKeyPressed(KEY_ESCAPE)
        // reads as true for a whole frame, not just once - so on the
        // very first ESC press, this trigger check and the SEPARATE
        // cancel check further below (inside "if (g_reboot_requested)")
        // both fired within that same single frame, immediately
        // reversing the very reboot this trigger had just requested -
        // logged as "rebooting" instantly followed by "cancelled".
        // Removing the trigger side entirely (rather than patching
        // the double-fire) also matches the person's own reasoning:
        // an explicit Reboot button already exists for actually
        // wanting to reboot - ESC accidentally doing the same thing
        // was never intentional, only ever a shortcut nobody asked
        // for. ESC's only remaining job is cancelling an
        // already-running countdown, which was already working
        // correctly on its own.
        bool platform_failed_before_first_frame = !ever_rendered && WindowShouldClose();
        if (platform_failed_before_first_frame && !g_reboot_requested && !g_app.show_confirm) {
            log_msg("Platform failed to initialize - rebooting shortly...");
            g_reboot_requested = true;
        }

        // Debug exit - breaks out immediately, no countdown overlay
        // (unlike reboot below): this is specifically for fast
        // development iteration, a delay would defeat the point.
        if (g_exit_requested) break;

        if (g_reboot_requested) {
            // A second, later ESC press - while THIS reboot is already
            // counting down - cancels it instead of triggering another
            // one (the WindowShouldClose() check above deliberately
            // excludes this same state via its own "!g_reboot_requested"
            // guard, so this is the only place that reads ESC while
            // counting down). On request: since ESC is what starts a
            // reboot here in the first place (no confirmation dialog
            // for it specifically), a same-key "press again to change
            // your mind" during the short window it's actually
            // avoidable in is a reasonable, low-risk safety net -
            // checked directly via IsKeyPressed(), not WindowShouldClose()
            // again, to stay clear of that function's own "platform
            // init failed" edge case mentioned in its own comment
            // above (irrelevant here, but no reason to risk conflating
            // the two).
            if (IsKeyPressed(KEY_ESCAPE)) {
                log_msg("Reboot cancelled (ESC pressed again).");
                g_reboot_requested = false;
                reboot_requested_at = -1;
            } else {
            if (reboot_requested_at < 0) reboot_requested_at = GetTime();
            double remaining = REBOOT_DELAY_SECONDS - (GetTime() - reboot_requested_at);
            if (remaining <= 0 || !ever_rendered) break;

            begin_frame();
            ClearBackground(g_bgColor);
            draw_ui();
            float sw = (float)g_canvas_w, sh = (float)g_canvas_h;
            DrawRectangle(0, 0, (int)sw, (int)sh, Fade(BLACK, 0.6f));
            std::string msg = "Rebooting in " + std::to_string((int)(remaining + 0.99)) + "...";
            int fontSize = (int)(28 * g_ui_scale);
            int textW = MeasureText(msg.c_str(), fontSize);
            DrawText(msg.c_str(), (int)(sw / 2 - textW / 2), (int)(sh / 2 - fontSize / 2), fontSize, RAYWHITE);
            end_frame();
            continue;
            }
        }

        begin_frame();
        ClearBackground(g_bgColor);
        draw_ui();
        end_frame();
        ever_rendered = true;

        // Runs the actual (blocking, ~3+ second) WiFi scan exactly
        // one frame after it was requested - the "Scanning..." frame
        // drawn just above (see WifiScreenState::scan_pending's own
        // comment) has now genuinely reached the screen via the
        // end_frame() call right above this, which is the whole point
        // of doing this HERE rather than inside draw_ui() or the
        // button click handler itself - either of those would run
        // BEFORE this same frame's own end_frame(), meaning the
        // blocking work would start before the person ever sees the
        // "Scanning..." message at all.
        if (g_wifi_screen.scan_pending) {
            run_pending_wifi_scan();
        }
    }

    if (g_exit_requested) {
        // Goal (per explicit clarification): NOT just a clean app
        // exit, but actually landing on a usable console - to read
        // logs or, on the initramfs image, edit/poke around the
        // running (RAM-resident) rootfs directly. This app's own
        // service has "Conflicts=getty@tty1.service" (so the two
        // don't fight over the same TTY while the app is running) -
        // actively start getty@tty1 back up before exiting, so the
        // person lands on a real login prompt on the same TTY rather
        // than an idle, nothing-running one. Exits with code 0
        // deliberately (not a distinct/nonzero one) - the service has
        // "Restart=on-failure", and a nonzero code here would make
        // systemd immediately relaunch the GUI right back on top of
        // the console we just opened up, defeating the whole point.
        //
        // A separate marker-file mechanism used to exist here for the
        // initramfs image specifically, back when that variant had no
        // systemd at all (a hand-written /init script as PID 1
        // instead) - removed as part of that image being rebuilt for
        // full parity with the disk-backed one (systemd, same
        // IMAGE_INSTALL, see that recipe's own comments) - both
        // variants now share this exact same systemd-based path,
        // nothing initramfs-specific left to special-case here.
        log_msg("Exit (debug) requested - dropping to console, no reboot.");
        // Cleanup BEFORE the getty call, and "--no-block" on it -
        // this service has "Conflicts=getty@tty1.service", so
        // starting getty@tty1 while THIS service is still active
        // means systemd must stop THIS one first (send us SIGTERM) to
        // satisfy that conflict - and we are the ones issuing this
        // very "systemctl start" call, blocking on our own child
        // process's exit via run_checked(). Without "--no-block",
        // that child wouldn't return until the job (including our own
        // conflict-triggered SIGTERM) has been processed - meaning we
        // could be killed while still blocked waiting on it, before
        // ever reaching cleanup or "return 0". "--no-block" makes
        // systemctl return as soon as the job is queued rather than
        // completed, and doing cleanup first (not after) means even
        // if SIGTERM does arrive moments later, nothing important is
        // left unfinished by then. Found via re-examining this exact
        // mechanism on request, not via an observed failure.
        if (logFont.texture.id != 0 && logFont.texture.id != uiFont.texture.id) UnloadFont(logFont);
        if (uiFont.texture.id != 0) UnloadFont(uiFont);
        if (g_low_res_active) UnloadRenderTexture(g_low_res_target);
        CloseWindow();
        if (access("/usr/bin/systemctl", X_OK) == 0 || access("/bin/systemctl", X_OK) == 0) {
            try {
                backend::run_checked({"systemctl", "--no-block", "start", "getty@tty1.service"}, log_msg);
            } catch (const std::exception& e) {
                log_msg(std::string("WARNING: could not start getty@tty1: ") + e.what());
            }
        }
        return 0;
    }

    log_msg("Rebooting now.");
    // Cleanup moved BEFORE the reboot attempt, not after - same
    // reasoning as the debug-exit path above, though lower stakes
    // here: "systemctl reboot" stops every running service (including
    // this one) as part of the shutdown sequence, so this process
    // could in principle be SIGTERM'd while still blocked waiting on
    // its own "systemctl reboot" child process, before ever reaching
    // the cleanup code that used to follow this block. Fairly benign
    // either way here specifically (the whole system is coming down
    // regardless, the kernel reclaims everything on its own), but
    // doing it in this order removes any doubt and matches the
    // debug-exit path for consistency.
    // logFont only needs its own unload if it's a genuinely separate
    // texture from uiFont (not the same one via the fallback path
    // above, which would double-free the same GPU texture).
    if (logFont.texture.id != 0 && logFont.texture.id != uiFont.texture.id) UnloadFont(logFont);
    if (uiFont.texture.id != 0) UnloadFont(uiFont);
    if (g_low_res_active) UnloadRenderTexture(g_low_res_target);
    CloseWindow();
    try {
        // Both boot variants this project builds now have systemd
        // ("systemctl reboot") - the initramfs image used to lack it
        // entirely (a hand-written /init script as PID 1 instead),
        // needing a "reboot" fallback here specifically for that
        // variant; that image has since been rebuilt for full parity
        // with the disk-backed one (systemd, same IMAGE_INSTALL - see
        // that recipe's own comments), so this path is no longer
        // initramfs-specific. The plain "reboot" fallback is kept
        // anyway as a general defensive measure (still harmless, and
        // still correct if systemctl is ever unavailable for some
        // other reason) rather than removed outright. Checked
        // directly via access() rather than shelling out to
        // "which"/"command -v" first just to decide - avoids spawning
        // an extra process purely for the check, and avoids depending
        // on either of those two tools being present at all.
        if (access("/usr/bin/systemctl", X_OK) == 0 || access("/bin/systemctl", X_OK) == 0) {
            backend::run_checked({"systemctl", "reboot"}, log_msg);
        } else {
            backend::run_checked({"reboot"}, log_msg);
        }
    } catch (const std::exception& e) {
        // Don't crash if this ever fails on real hardware for some
        // reason - log it and exit cleanly, same as any other error
        // path in this app.
        log_msg(std::string("ERROR: reboot failed: ") + e.what());
    }

    return 0;
}
