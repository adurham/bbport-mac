// bbport: SDL3 window for the Vulkan swapchain (X11, Wayland, or Metal on macOS).
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include "bbgpu.h"
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_confirm.h"
#include "bbport_capture.h"                  // the capture hotkey: the PNG and the layer's state
#include "platform/videoout/frame_stats.h"   // bb_frame_log_marker: the marker in the frame log
#if __has_include("remote/bb_remote.h")
#include "remote/bb_remote.h" // the native GPU process: bb-gpu's window opens the modal's pads itself
#define BBPORT_WINDOW_REMOTE 1
#endif
#ifdef __APPLE__
// bbport_appnap.mm: keeps a hidden warm run out of App Nap (its compiles at full speed).
extern "C" void bb_warm_disable_app_nap(void);
// bbport_layer.mm: the display's mode after a switch, and the panel's native pixel size.
extern "C" void bb_log_display_mode(void* nswindow, const char* why);
extern "C" int bb_display_native_pixels(void* nswindow, int* width, int* height);
// bbport_layer.mm: the display's mode now, and waiting for a mode set to have landed. SDL's Cocoa
// fullscreen transition is asynchronous, so entering fullscreen returns before CoreGraphics has
// applied the mode — the drawable must not be created until it has.
extern "C" int bb_display_current_pixels(void* nswindow, int* width, int* height);
extern "C" int bb_display_wait_mode_change(void* nswindow, int old_width, int old_height, int timeout_ms);
#endif


namespace {
std::array<std::atomic<BbGpuEventFilter>, 4> event_filters{};

bool FilteredByModule(const SDL_Event* event, SDL_Window* window) {
    for (const auto& slot : event_filters) {
        const BbGpuEventFilter filter = slot.load(std::memory_order_acquire);
        if (filter && filter(event, window)) {
            return true;
        }
    }
    return false;
}
} // namespace

extern "C" void bbgpu_add_event_filter(BbGpuEventFilter filter) {
    for (auto& slot : event_filters) {
        BbGpuEventFilter expected = nullptr;
        if (slot.compare_exchange_strong(expected, filter)) {
            return;
        }
    }
}

namespace {
std::array<std::atomic<BbGpuPointerQuery>, 4> pointer_queries{};
std::array<std::atomic<BbGpuPadFilter>, 4> pad_filters{};

template <typename T>
void AddTo(std::array<std::atomic<T>, 4>& slots, T value) {
    for (auto& slot : slots) {
        T expected = nullptr;
        if (slot.compare_exchange_strong(expected, value)) {
            return;
        }
    }
}
} // namespace

extern "C" void bbgpu_add_pointer_query(BbGpuPointerQuery query) {
    AddTo(pointer_queries, query);
}

extern "C" int bbgpu_pointer_wanted(void) {
    for (const auto& slot : pointer_queries) {
        const BbGpuPointerQuery query = slot.load(std::memory_order_acquire);
        if (query && query()) {
            return 1;
        }
    }
    return 0;
}

extern "C" void bbgpu_add_pad_filter(BbGpuPadFilter filter) {
    AddTo(pad_filters, filter);
}

extern "C" void bbgpu_filter_pad(uint32_t* buttons, const uint8_t* sticks) {
    for (const auto& slot : pad_filters) {
        if (const BbGpuPadFilter filter = slot.load(std::memory_order_acquire)) {
            filter(buttons, sticks);
        }
    }
}

namespace {
// bbport: the gamepads the confirmation modal takes its answers from (BbConfirm::PadEvent). SDL
// sends gamepad button events only for gamepads this process has opened: the game process's
// runtime_pad.c opens the one it reads, and bb-gpu — whose window draws the modal — has nothing
// else to open them, so the window does it here (without an open pad its loop never sees one).
bool OpensPadsHere() {
#ifdef BBPORT_WINDOW_REMOTE
    return BbRemote::BackActive(); // bb-gpu: the game's pads are another process's
#else
    return false; // in process: runtime_pad.c opens them, and its open is what makes them arrive
#endif
}

constexpr int MaxConfirmPads = 8;
SDL_Gamepad* confirm_pads[MaxConfirmPads] = {};

void OpenConfirmPad(SDL_JoystickID id) {
    if (!OpensPadsHere() || SDL_GetGamepadFromID(id)) {
        return; // already open: ours, or the game process's own (in process)
    }
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        std::printf("Window: gamepad not opened for the confirmation modal: %s\n", SDL_GetError());
        return;
    }
    std::printf("Window: gamepad opened for the confirmation modal: %s\n", SDL_GetGamepadName(pad));
    for (SDL_Gamepad*& slot : confirm_pads) {
        if (!slot) {
            slot = pad;
            return;
        }
    }
    SDL_CloseGamepad(pad); // more pads than the modal answers with: the rest stay closed
}

void RefreshConfirmPads() {
    for (SDL_Gamepad*& pad : confirm_pads) {
        if (pad && !SDL_GamepadConnected(pad)) {
            SDL_CloseGamepad(pad); // unplugged: the handle goes, and a new pad can take the slot
            pad = nullptr;
        }
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        OpenConfirmPad(ids[i]);
    }
    SDL_free(ids);
}

// What is down right now on this process's open pads, a bit per SDL_GAMEPAD_BUTTON_* (BbConfirm::
// PadHeld). Read before a poll's events are pumped, so it is what was down when the question
// appeared, not what its own press made.
std::uint32_t ConfirmPadsDown() {
    static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32, "a button is a bit");
    std::uint32_t down = 0;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) {
            continue;
        }
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b))) {
                down |= 1u << unsigned(b);
            }
        }
    }
    SDL_free(ids);
    return down;
}
} // namespace

namespace Frontend {

namespace {

// Issue #69: the monitor the window (and fullscreen) goes to. BB_DISPLAY: its number in SDL's
// order (1, 2, ...; bb-gpu-capabilities --displays lists them) or a part of its name; without it
// SDL's primary display. The monitors are logged so a report says which one was taken.
SDL_DisplayID ChooseDisplay() {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    const char* wanted = std::getenv("BB_DISPLAY");
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    SDL_DisplayID chosen = 0;
    if (wanted && *wanted) {
        char* end = nullptr;
        const long number = std::strtol(wanted, &end, 10);
        if (end && *end == '\0') {
            if (number >= 1 && number <= count) {
                chosen = ids[number - 1];
            }
        } else {
            for (int i = 0; i < count && !chosen; ++i) {
                const char* name = SDL_GetDisplayName(ids[i]);
                if (name && strcasestr(name, wanted)) {
                    chosen = ids[i];
                }
            }
        }
    }
    const SDL_DisplayID display = chosen ? chosen : primary;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetDisplayName(ids[i]);
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]);
        std::printf("Display %d: %s %dx%d%s%s\n", i + 1, name ? name : "?", mode ? mode->w : 0,
                    mode ? mode->h : 0, ids[i] == primary ? " (primary)" : "",
                    ids[i] == display ? " <- the game's (BB_DISPLAY)" : "");
    }
    if (wanted && *wanted && !chosen) {
        std::printf("Display: BB_DISPLAY=%s matches none, the primary one is used\n", wanted);
    }
    SDL_free(ids);
    return display;
}

bool WarmRun() {
    const char* warm = std::getenv("BB_WARM_ONLY");
    return warm && warm[0] == '1';
}

void* CocoaWindow(SDL_Window* window) {
    return SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                  nullptr);
}

/// A display mode's size in pixels: SDL lists macOS modes in points with a pixel density (a
/// scaled "looks like 3440x1440" mode is 3440x1440 at 2.0, i.e. 6880x2880 pixels).
BbSettings::OutputSize ModePixels(const SDL_DisplayMode& mode) {
    const float density = mode.pixel_density > 0.0f ? mode.pixel_density : 1.0f;
    return {int(mode.w * density + 0.5f), int(mode.h * density + 0.5f)};
}

/// The panel's own pixel size: the native mode (macOS: the mode CoreGraphics flags native, not
/// the scaled desktop's backing store), else the largest 1x mode, else the desktop's pixels.
BbSettings::OutputSize NativePixels(SDL_Window* window) {
#ifdef __APPLE__
    if (int w = 0, h = 0; bb_display_native_pixels(CocoaWindow(window), &w, &h) && w > 0 && h > 0) {
        return {w, h};
    }
#endif
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    BbSettings::OutputSize best{0, 0};
    int count = 0;
    SDL_DisplayMode** listed = SDL_GetFullscreenDisplayModes(display, &count);
    for (int i = 0; listed && i < count; ++i) {
        const auto px = ModePixels(*listed[i]);
        if (listed[i]->pixel_density <= 1.0f && px.width * px.height > best.width * best.height) {
            best = px;
        }
    }
    SDL_free(listed);
    if (best.width == 0) {
        if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display)) {
            best = ModePixels(*mode);
        }
    }
    return best;
}

/// Whether the display has a mode of exactly this pixel size.
bool HasModeOfPixels(SDL_Window* window, BbSettings::OutputSize size) {
    int count = 0;
    SDL_DisplayMode** listed = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window), &count);
    bool found = false;
    for (int i = 0; listed && i < count && !found; ++i) {
        found = ModePixels(*listed[i]) == size;
    }
    SDL_free(listed);
    return found;
}

// The monitor the window is on, in pixels: the output's aspect ratio "Auto" follows it. Its
// native pixels: a scaled HiDPI desktop's backing store (6880x2880 for "looks like 3440x1440")
// is not a size anything should render at.
void TellMonitor(SDL_Window* window) {
    const auto native = NativePixels(window);
    BbSettings::SetMonitor(native.width, native.height);
}

/// The output resolution's default: the display's native pixel size, when bbport.ini has none
/// or has one no display mode has (the old default, 1920x1080 fitted to 21:9, made 2560x1080 and
/// a 2560x1440 fullscreen mode on a 3440x1440 panel). A startup-patched session keeps its value:
/// run.sh already built the game for it. With fullscreen_mode=fill a chosen size is a render
/// resolution (scaled into the window), so it is kept whether the display has a mode of it or not.
void DefaultOutputToNative(SDL_Window* window) {
    auto& s = BbSettings::Get();
    if (BbSettings::FixedRenderSession()) {
        return;
    }
    const auto native = NativePixels(window);
    const auto current = BbSettings::CurrentOutput();
    const bool chosen = s.output_res_chosen.load();
    // A chosen size without a display mode only matters in fullscreen (a window or borderless
    // scales its output anyway: 5120x2160 there is a deliberate supersample, kept).
    const bool fullscreen = s.window_mode.load() == BbSettings::WindowFullscreen;
    if (native.width <= 0 || current == native ||
        (chosen && (!fullscreen || BbSettings::FullscreenFills() || HasModeOfPixels(window, current)))) {
        if (BbSettings::FullscreenFills() && fullscreen && !HasModeOfPixels(window, current)) {
            // Desktop/fill: no display mode of that size is needed — the output is rendered at it
            // and the renderer scales it into the window (the display keeps its own mode).
            std::printf("Output: %s kept (fullscreen_mode=%s: rendered at this size and scaled "
                        "into the display's own mode; was it a mode, it would be switched)\n",
                        BbSettings::SizeText(current).c_str(),
                        BbSettings::FullscreenModeKey(s.fullscreen_mode.load()));
        }
        return;
    }
    const int aspect = s.aspect.load();
    const auto fitted = BbSettings::NearestOutput(aspect, native);
    if (fitted != native) {
        // An explicit aspect ratio without the panel's size (16:9 on an ultrawide): it stays.
        std::printf("Output: %s kept (no %dx%d in aspect ratio %s)\n", BbSettings::SizeText(current).c_str(),
                    native.width, native.height, BbSettings::AspectKey(aspect));
        return;
    }
    std::printf("Output: %s, the display's native pixels (was %s: %s)\n", BbSettings::SizeText(native).c_str(),
                BbSettings::SizeText(current).c_str(),
                chosen ? "no display mode has that size" : "never chosen");
    s.output_res = BbSettings::PackSize(native);
    s.output_res_chosen = true;
    BbSettings::Save(); // run.sh reads output_res at the next start
}

/// The derived display mode (below, with the window modes).
void apply_display_mode(SDL_Window* window, const char* why);

/// The window-mode apply (below, with the window modes): the constructor calls it synchronously
/// for the boot mode, while nothing of ours can be on screen yet.
void apply_window_mode(SDL_Window* window, int mode);

/// BB_DISPLAY_DRY=1: derive and log the pick, set nothing. A fullscreen launch then enters
/// fullscreen on the desktop's mode — the ordering is exercised, the panel is not moved.
bool DryDisplayRun() {
    static const bool on = [] {
        const char* env = std::getenv("BB_DISPLAY_DRY");
        return env && env[0] && env[0] != '0';
    }();
    return on;
}

} // namespace

namespace {

/// The window the display-mode picker works on — this process has exactly one (window.cpp's
/// bb_output_is_native_mode and the capture hotkey ask for it from outside these functions).
SDL_Window* last_window = nullptr;
#ifdef __APPLE__
/// Its swapchain's CAMetalLayer, taken out of the window in the constructor: the capture's log
/// line asks it for its opacity and colour space, as bbport_layer.mm's own dump does.
void* window_metal_layer = nullptr;
#endif

/// The constructor's synchronous window-mode apply is the only moment a display mode may be set:
/// the window exists, and nothing of ours has been presented (SDL_Metal_CreateView comes after
/// it), so no scanout surface is re-pointed under the display coprocessor. Cleared at the end of
/// the constructor; apply_display_mode refuses every later set (it logs, and nothing moves).
bool boot_mode_phase = true;

/// apply_display_mode's last pick asked for a mode the display was not already on, i.e. a real
/// CGDisplaySetDisplayMode is in flight. The constructor waits for it to land before the layer is
/// created; the picker choosing the current mode again (the common case) must not cost a wait.
bool mode_set_pending = false;

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // A window that loses focus must not leave fullscreen: SDL would restore the desktop mode under
    // a live drawable, mid-session — the same hazard class as a mode set at launch (DCPEXT1
    // axi_rd_err, 2026-10-18). A notification or Cmd-Tab is enough to trigger it by default.
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    // SDL sends gamepad events only for gamepads that have been opened; in bb-gpu nobody else does,
    // and the confirmation modal's answers are these events (BbConfirm::PadEvent).
    RefreshConfirmPads();
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    const SDL_DisplayID display = ChooseDisplay();
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
#ifdef __APPLE__
    // bbport (macOS): the drawable's pixels. A scaled desktop (a HiDPI mode: 6880x2880 backing for
    // a 3440x1440 panel) is scanned out from that backing store, so a drawable of the backing
    // store's pixels is what the WindowServer can scan directly ("Direct" in the Metal HUD);
    // anything smaller is composited, upscaled to the backing store and then scaled down to the
    // panel. On such a display "on" is the correct setting: only the encode pass (a full-screen
    // textured quad) runs at the backing size — the scene renders at the output resolution. On a
    // 1x desktop mode the drawable is the panel's pixels either way. The setting (bbport.ini
    // retina; BB_RETINA overrides) turns it off for experiments.
    const char* retina = std::getenv("BB_RETINA");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN,
                           retina ? retina[0] != '0' : BbSettings::Get().retina.load());
#endif
    // The window mode (bbport.ini window_mode / BB_WINDOW_MODE; BB_FULLSCREEN=1 is legacy):
    // applied at creation and, for changes from the menu, live (SetWindowMode below).
    const int window_mode = BbSettings::Get().window_mode.load();
    // A warm run (BB_WARM_ONLY): hidden window, and it stays windowed (the pipeline caches do not
    // care about the mode, and switching a hidden window would flicker the owner's screen).
    const bool warm_window = WarmRun();
    // A fullscreen launch whose mode MOVES THE PANEL must not be born fullscreen: SDL then sets the
    // display mode at the first PollEvents, i.e. after the swapchain exists and is presenting.
    // CGDisplaySetDisplayMode with a drawable of ours scanout-eligible is the sequence that panicked
    // the display coprocessor on 2026-10-18 (DCPEXT1 axi_rd_err, twice). Such a launch is created
    // as a plain window and moved to its mode + fullscreen by apply_window_mode below — still
    // before SDL_Metal_CreateView makes a layer the WindowServer could scan.
    const bool boot_fullscreen = window_mode == BbSettings::WindowFullscreen && !warm_window;
    const bool moves_panel = boot_fullscreen && !BbSettings::FullscreenKeepsDesktopMode();
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           window_mode == BbSettings::WindowFullscreen && !moves_panel);
    // A warm run (BB_WARM_ONLY): hidden window; the caches are compiled for later sessions and
    // presenter.cpp asks the game to quit after BB_WARM_SECONDS.
    if (const char* warm = std::getenv("BB_WARM_ONLY"); warm && warm[0] == '1') {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
#ifdef __APPLE__
        bb_warm_disable_app_nap(); // a hidden process is App-Napped: compiles would be throttled
#endif
        std::printf("Warm run: hidden window (BB_WARM_ONLY)\n");
    }
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    last_window = window; // the display-mode picker's and the capture hotkey's window
    if (moves_panel) {
        // Synchronous, and before SDL_Metal_CreateView below: the panel is moved with no drawable of
        // ours in existence, and the swapchain is then born on the new mode — no surface is
        // re-pointed under the display coprocessor.
#ifdef __APPLE__
        int before_w = 0, before_h = 0;
        bb_display_current_pixels(CocoaWindow(window), &before_w, &before_h);
#endif
        apply_window_mode(window, window_mode);
#ifdef __APPLE__
        // SDL's Cocoa fullscreen transition is asynchronous: entering fullscreen returns before
        // CoreGraphics has applied the mode. Waiting here is the point of the ordering — the layer
        // must be created ON the new mode, not during the switch.
        if (!DryDisplayRun() && mode_set_pending) {
            const int settle = bb_display_wait_mode_change(CocoaWindow(window), before_w, before_h, 3000);
            if (settle == 1) {
                std::printf("Display mode: the panel is on the new mode before the first drawable "
                            "exists (was %dx%d)\n", before_w, before_h);
            } else {
                std::printf("Display mode: the panel did not report a new mode within 3 s (was %dx%d); "
                            "the drawable is created anyway — the layer dump below says what it is\n",
                            before_w, before_h);
            }
        } else if (!DryDisplayRun()) {
            std::printf("Display mode: the pick already was the current mode; no swap to wait for\n");
        }
#endif
    } else if (window_mode != BbSettings::WindowWindowed && !warm_window) {
        SetWindowMode(window_mode); // borderless/fullscreen at start; the menu changes it live
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
#ifdef __APPLE__
    } else if (driver && !std::strcmp(driver, "cocoa")) {
        // MoltenVK presents through a CAMetalLayer (as upstream shadPS4 on macOS).
        window_info.type = WindowSystemType::Metal;
        window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
        window_metal_layer = window_info.render_surface; // the capture's log line asks it for its state
#endif
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
#ifdef __APPLE__
    std::printf("Window: %dx%d pixels (pixel density %.2f)\n", w, h,
                SDL_GetWindowPixelDensity(window));
#endif
    TellMonitor(window);
    DefaultOutputToNative(window);
    if (warm_window && window_mode == BbSettings::WindowFullscreen) {
        apply_display_mode(window, "fullscreen at start"); // warm: logged, not applied
    }
    // Past this point a drawable of ours can exist: apply_display_mode refuses every set from here
    // on (the constructor's synchronous apply above is the one moment the panel may be moved).
    boot_mode_phase = false;
}

WindowSDL::~WindowSDL() {
    last_window = nullptr;
    SDL_DestroyWindow(window);
}

namespace {
int applied_window_mode = -1; ///< BbSettings::WindowMode last applied
SDL_Point saved_windowed_size{0, 0}; ///< windowed size/position to come back to
SDL_Point saved_windowed_pos{0, 0};
std::atomic<int> pending_window_mode{-1}; ///< a mode from a menu, waiting for the window loop

/// The display mode is derived, never stored: mode = pick(window mode, output resolution). These
/// are the inputs it was last derived from; PollEvents derives it again when either differs.
std::uint32_t mode_output = 0;    ///< BbSettings::PackSize of the output it was picked for
SDL_DisplayID mode_display = 0;   ///< the display it was picked on
bool mode_fill = true;            ///< the fullscreen semantics (BbSettings::FullscreenMode) it was picked for
int mode_setting = -1;            ///< BbSettings::FullscreenMode value it was picked for (desktop/fill/exclusive)

/// The display mode for an output of `wanted` pixels: compared in pixels (SDL lists macOS modes
/// in points with a density: "looks like 3440x1440" is 3440x1440 at 2.0 = 6880x2880 pixels),
/// 1x modes first — a 2x mode's backing is twice the drawable of a retina=0 window, so the
/// WindowServer would scale every frame (Composited) — then the closest size, then the highest
/// refresh rate at or below `refresh_cap` (0 = no cap; see apply_display_mode's source-width rule).
const SDL_DisplayMode* pick_display_mode(SDL_DisplayMode** listed, int count, BbSettings::OutputSize wanted,
                                         float refresh_cap = 0.0f) {
    const SDL_DisplayMode* chosen = nullptr;
    long best_distance = 0;
    bool best_1x = false;
    for (int i = 0; listed && i < count; ++i) {
        const SDL_DisplayMode* mode = listed[i];
        const BbSettings::OutputSize px = ModePixels(*mode);
        const long distance = std::labs(long(px.width) - wanted.width) + std::labs(long(px.height) - wanted.height);
        const bool is_1x = mode->pixel_density <= 1.0f;
        // Under a cap the mode at or below it wins over one above it, whatever their refresh order:
        // the cap exists to keep a panel move off a link the move must not land on.
        const bool under_cap = refresh_cap > 0.0f && mode->refresh_rate <= refresh_cap + 0.5f;
        const bool best_under_cap = refresh_cap > 0.0f && chosen && chosen->refresh_rate <= refresh_cap + 0.5f;
        const bool better = !chosen || (is_1x != best_1x ? is_1x
                                        : distance != best_distance ? distance < best_distance
                                        : under_cap != best_under_cap ? under_cap
                                                                    : mode->refresh_rate > chosen->refresh_rate);
        if (better) {
            chosen = mode;
            best_distance = distance;
            best_1x = is_1x;
        }
    }
    return chosen;
}

/// Derives the display mode and sets it as the window's fullscreen mode (SDL switches at once when
/// the window is fullscreen already, else on entering it). The window's thread only. A warm run
/// never switches modes (its window is hidden and windowed).
///
/// fullscreen_mode=desktop (the default) never sets a display mode: the window covers the display
/// on the mode the desktop is on and the renderer scales the output into it, so no
/// CGDisplaySetDisplayMode is issued at any point. With fullscreen_mode=fill the display is put on
/// its own native 1x mode once, and the chosen output resolution is then a render resolution the
/// renderer scales into the window. With fullscreen_mode=exclusive the mode is the output's own,
/// as it was before. Both of those move the panel, so both are opt-in.
void apply_display_mode(SDL_Window* window, const char* why) {
    if (!window) {
        return;
    }
    const bool dry_run = WarmRun() || DryDisplayRun(); // logs the pick (the picker's check), switches nothing
    // A display mode may only be set from the constructor, before SDL_Metal_CreateView makes a layer
    // the WindowServer could scan. Every later request is logged and dropped: setting one under a
    // live swapchain is what took the display coprocessor down (DCPEXT1 axi_rd_err, 2026-10-18).
    const bool may_set = boot_mode_phase || !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const BbSettings::OutputSize output = BbSettings::CurrentOutput();
    mode_output = BbSettings::PackSize(output);
    mode_fill = BbSettings::FullscreenFills();
    mode_setting = BbSettings::Get().fullscreen_mode.load();
    mode_display = display;
    // Belt and braces removed: the panel-moving modes are selectable again (the ordering fix in
    // the constructor is what makes them safe from this process's side). The may_set gate below
    // still keeps every set inside the constructor, before the Metal layer exists.
    if (BbSettings::FullscreenKeepsDesktopMode()) {
        // The panel is never switched. The window covers the desktop's mode; the output is a
        // render size scaled into it (window.cpp: apply_window_mode enters fullscreen with a null
        // mode, so SDL sets no display mode either).
        std::printf("Display mode: the desktop's mode is kept (%s; fullscreen_mode=desktop: the panel "
                    "is never switched, %s is rendered and scaled into it)\n",
                    why, BbSettings::SizeText(output).c_str());
        if (!dry_run && !may_set) {
            // A restart is what actually puts the panel back: SDL would restore the desktop's mode
            // here, i.e. switch it under a live swapchain, which is the sequence the gate above
            // exists to refuse.
            std::printf("Display mode: the panel stays where startup put it until a restart "
                        "(fullscreen_mode=desktop takes effect at the next start)\n");
        }
        if (!dry_run && may_set) {
            SDL_SetWindowFullscreenMode(window, nullptr);
        }
        return;
    }
    const bool fill = BbSettings::FullscreenFills();
    const BbSettings::OutputSize native = fill ? NativePixels(window) : BbSettings::OutputSize{0, 0};
    const BbSettings::OutputSize wanted = fill && native.width > 0 ? native : output;
    int count = 0;
    SDL_DisplayMode** listed = SDL_GetFullscreenDisplayModes(display, &count);
    const SDL_DisplayMode* chosen = pick_display_mode(listed, count, wanted);
    // A set that changes the pipe's SOURCE WIDTH — 2x (6880x2880 backing) to 1x (3440x1440) or back
    // — must not land on the 240 Hz 10-bit HDR (DSC) link in the same set. Measured 2026-10-20
    // 09:57:53: a launch's exclusive fullscreen asked for 772 -> 724, WindowServer re-piped the
    // display and then took the link to 239 Hz RGB_PQ_10bit; within 1.3 s the reconfigure handshake
    // broke ("AppleDisplayManagerReconfigComplete missing", "swap aborted", and
    // kCGSDisplayDidReconfigure2 timing out for every client), the compositor's main thread never
    // ran again, the watchdog killed it, and its restart hung in IOMobileFramebuffer — both panels
    // stayed black until the machine was rebooted. The display log's rule: a source-width change is
    // survivable while the link is NOT the 240 Hz 10-bit one, so keep the refresh at or below
    // 120 Hz for that set (this panel's 120 Hz 10-bit link took ten such sets on 2026-10-16 and has
    // taken every one since; the three 2026-10-18 DCPEXT1 panics were the same change with the DSC
    // link up). The owner's standing config is 120 Hz DSC-free, so this picks what he wants anyway.
    if (chosen) {
        const SDL_DisplayMode* now_mode = SDL_GetDesktopDisplayMode(display);
        const bool source_width_changes =
            now_mode && ModePixels(*chosen).width != ModePixels(*now_mode).width;
        if (source_width_changes && chosen->refresh_rate > 120.5f) {
            const float cap = now_mode->refresh_rate < 120.0f ? now_mode->refresh_rate : 120.0f;
            const SDL_DisplayMode* safe = pick_display_mode(listed, count, wanted, cap);
            if (safe && safe->refresh_rate <= 120.5f &&
                ModePixels(*safe).width == ModePixels(*chosen).width) {
                std::printf("Display mode: %dx%d at %.0f Hz would change the pipe source width onto "
                            "the 240 Hz 10-bit HDR link; %.0f Hz picked instead (%s)\n",
                            ModePixels(*chosen).width, ModePixels(*chosen).height,
                            chosen->refresh_rate, safe->refresh_rate, why);
                chosen = safe;
            } else {
                std::printf("Display mode: %dx%d at %.0f Hz changes the pipe source width onto the "
                            "240 Hz 10-bit HDR link and this display lists no mode of that size at or "
                            "below 120 Hz: the desktop's mode is kept (%s)\n",
                            ModePixels(*chosen).width, ModePixels(*chosen).height,
                            chosen->refresh_rate, why);
                chosen = nullptr;
            }
        }
    }
    if (fill && chosen && ModePixels(*chosen) != native) {
        // Nothing on this display is its own pixel size at 1x (a scaled desktop may list it at 2x
        // only): the desktop's mode already fills the panel and the renderer scales into it. The
        // panel is not put on a mode that is not its own.
        std::printf("Display mode: no 1x mode of %dx%d pixels on this display: the desktop's own "
                    "mode is kept (%s)\n",
                    native.width, native.height, why);
        chosen = nullptr;
    }
    if (chosen) {
        const BbSettings::OutputSize px = ModePixels(*chosen);
        std::printf("Display mode: %dx%d pixels (SDL %dx%d, pixel density %.2f, %.2f Hz) for output %s (%s)"
                    "%s%s\n",
                    px.width, px.height, chosen->w, chosen->h, chosen->pixel_density, chosen->refresh_rate,
                    BbSettings::SizeText(output).c_str(), why, fill ? " - fullscreen_mode=fill: the "
                    "display's own mode, the output scaled into the window" : " - fullscreen_mode="
                    "exclusive: the output's own mode", dry_run ? (WarmRun() ? " - warm run: not applied"
                                                                              : " - BB_DISPLAY_DRY: not applied")
                                                                  : "");
        if (!dry_run && !may_set) {
            // Mechanically refused: apply_display_mode only ever runs with the panel safe to move
            // from the constructor (boot_mode_phase). Under a live swapchain the set below is the
            // sequence that panicked the display coprocessor, so nothing moves and the log says why.
            std::printf("Display mode: %s needs a restart (fullscreen_mode=%s moves the panel at "
                        "startup only, before the first drawable; the display is never switched "
                        "under a live swapchain)\n",
                        BbSettings::SizeText(output).c_str(),
                        BbSettings::FullscreenModeKey(BbSettings::Get().fullscreen_mode.load()));
            SDL_free(listed);
            return;
        }
        const SDL_DisplayMode* now = SDL_GetDesktopDisplayMode(display);
        mode_set_pending = !now || now->w != chosen->w || now->h != chosen->h ||
                           now->pixel_density != chosen->pixel_density;
        if (!dry_run && !SDL_SetWindowFullscreenMode(window, chosen)) {
            std::printf("Display mode: SDL refused it (%s): the desktop's mode\n", SDL_GetError());
            SDL_SetWindowFullscreenMode(window, nullptr);
        }
    } else {
        std::printf("Display mode: none listed for output %s (%s): the desktop's mode\n",
                    BbSettings::SizeText(wanted).c_str(), why);
        if (!dry_run && may_set) {
            SDL_SetWindowFullscreenMode(window, nullptr);
        }
    }
    SDL_free(listed);
#ifdef __APPLE__
    if (!dry_run && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
        bb_log_display_mode(CocoaWindow(window), why); // the mode CoreGraphics reports now
    }
#endif
}

/// PollEvents: the derived mode follows its inputs. Fullscreen only (windowed and borderless
/// use the desktop's mode); a changed output (a confirmed resolution, the overlay's, the native
/// default) or another display picks again. A resolution the player did not confirm never
/// reaches BbSettings, so cancelling or the timeout leaves the mode as it is.
///
/// Desktop (the default) and fill: the display keeps its own mode, so a resolution change switches
/// nothing at all — the renderer scales the new output into the same window (no flash, and no mode
/// of the panel's own scaler in between). Exclusive: the mode is the output's own, applied at
/// startup; a change made while the game runs is NOT applied to the display. Setting a display
/// mode under a live swapchain — the drawable torn down and re-created around
/// CGDisplaySetDisplayMode — is the sequence that panicked the display coprocessor on 2026-10-18
/// (DCPEXT1 axi_rd_err), so that switch waits for the next start.
void follow_display_mode(SDL_Window* window, int applied_mode) {
    // A warm run stays windowed, so its applied mode is never fullscreen: it follows the saved
    // one instead and logs each pick without applying it (apply_display_mode's dry run) — the
    // live path checked without touching the screen.
    if (WarmRun()) {
        applied_mode = BbSettings::Get().window_mode.load();
    }
    if (applied_mode != BbSettings::WindowFullscreen) {
        return;
    }
    const bool fill = BbSettings::FullscreenFills();
    if (BbSettings::Get().fullscreen_mode.load() != mode_setting) {
        apply_display_mode(window, "the fullscreen setting changed");
        return;
    }
    const bool output_changed = BbSettings::PackSize(BbSettings::CurrentOutput()) != mode_output;
    const bool display_changed = SDL_GetDisplayForWindow(window) != mode_display;
    if (!display_changed && output_changed && fill) {
        // The resolution is a render resolution inside the window: nothing to switch, and nothing
        // to re-derive (the mode does not depend on the output in fill/desktop mode).
        mode_output = BbSettings::PackSize(BbSettings::CurrentOutput());
        std::printf("Display mode: %s rendered and scaled into the window (fullscreen_mode=%s); "
                    "the display stays on its own mode\n",
                    BbSettings::SizeText(BbSettings::CurrentOutput()).c_str(),
                    BbSettings::FullscreenModeKey(BbSettings::Get().fullscreen_mode.load()));
    }
    if (display_changed) {
        mode_display = SDL_GetDisplayForWindow(window);
        if (BbSettings::FullscreenMovesDisplay()) {
            std::printf("Display mode: the window moved to another display: fullscreen_mode=%s "
                        "applies its mode at the next start (the display is never switched under a "
                        "running swapchain)\n",
                        BbSettings::FullscreenModeKey(BbSettings::Get().fullscreen_mode.load()));
        }
    }
    if (output_changed && !fill && display_changed == false && BbSettings::FullscreenMovesDisplay()) {
        // Exclusive: the new output is a display mode, and the panel follows the game live — that
        // is the point of exclusive fullscreen (the owner, 2026-10-20). The window leaves
        // fullscreen FIRST, so no drawable of ours is fullscreen-scanned while the mode moves, then
        // the mode is set and fullscreen is entered again: SDL builds the new drawable inside the
        // new mode.
        std::printf("Display mode: output %s: the panel follows the game (exclusive, live)\n",
                    BbSettings::SizeText(BbSettings::CurrentOutput()).c_str());
        mode_output = BbSettings::PackSize(BbSettings::CurrentOutput());
        SDL_SetWindowFullscreen(window, false);
        apply_display_mode(window, "output resolution changed (live)");
        SDL_SetWindowFullscreen(window, true);
#ifdef __APPLE__
        if (!WarmRun()) {
            SDL_SyncWindow(window);
            bb_log_display_mode(CocoaWindow(window), "live mode change");
        }
#endif
    }
}

/// The window mode itself. Window calls change AppKit state, so this runs on the window's own
/// thread (PollEvents): menus can be anywhere, and AppKit throws from everywhere else
/// ("NSWindow geometry should only be modified on the main thread!", from the settings relay).
void apply_window_mode(SDL_Window* window, int mode) {
    if (!window || mode == applied_window_mode) {
        return;
    }
    if (applied_window_mode == -1 || applied_window_mode == BbSettings::WindowWindowed) {
        SDL_GetWindowSize(window, &saved_windowed_size.x, &saved_windowed_size.y);
        SDL_GetWindowPosition(window, &saved_windowed_pos.x, &saved_windowed_pos.y);
    }
    switch (mode) {
    case BbSettings::WindowBorderless: {
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowFullscreenMode(window, nullptr); // back to the desktop's own mode
        SDL_SetWindowBordered(window, false);
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        if (const SDL_DisplayMode* display_mode = SDL_GetDesktopDisplayMode(display)) {
            SDL_SetWindowSize(window, display_mode->w, display_mode->h);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                  SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        }
        break;
    }
    case BbSettings::WindowFullscreen: {
        // The fullscreen window runs on an explicit display mode, never on whatever mode the
        // desktop happens to use — except with fullscreen_mode=desktop (the default), which is
        // the desktop's mode and never sets one: the panel is not touched, and the chosen output
        // resolution is a render size scaled into the window. fill: the display's own native 1x
        // mode, and the chosen output resolution is scaled into it (one blit, the panel always
        // full — a resolution change never switches the display). exclusive: the output's own
        // mode, so the window's surface matches the mode pixel-for-pixel, which is what Direct
        // scan-out is made of; P.T.'s "exclusive" fullscreen does the same.
        apply_display_mode(window, BbSettings::FullscreenKeepsDesktopMode() ? "fullscreen (desktop mode kept)"
                                         : BbSettings::FullscreenFills()        ? "fullscreen (fill)"
                                                                                : "fullscreen (exclusive)");
        SDL_SetWindowBordered(window, true);
        SDL_SetWindowFullscreen(window, true);
#ifdef __APPLE__
        if (!WarmRun()) {
            SDL_SyncWindow(window); // the switch has happened: what CoreGraphics reports now
            bb_log_display_mode(CocoaWindow(window), "entered fullscreen");
        }
#endif
        break;
    }
    case BbSettings::WindowWindowed:
    default:
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowFullscreenMode(window, nullptr); // back to the desktop's own mode
        SDL_SetWindowBordered(window, true);
        if (saved_windowed_size.x > 0) {
            SDL_SetWindowSize(window, saved_windowed_size.x, saved_windowed_size.y);
            SDL_SetWindowPosition(window, saved_windowed_pos.x, saved_windowed_pos.y);
        }
        break;
    }
    applied_window_mode = mode;
    std::printf("Window mode: %s\n", mode == BbSettings::WindowBorderless ? "borderless"
                                  : mode == BbSettings::WindowFullscreen ? "fullscreen" : "windowed");
}
} // namespace

/// bbport (bbport_confirm.cpp): whether a resolution change lands on the panel's own native 1x
/// mode — the one the reference ports apply without a word. The display mode is derived from the
/// output resolution here (pick_display_mode prefers 1x modes, then the closest size): the answer
/// is the pick itself, not a guess from the numbers, so a size whose closest 1x mode is something
/// else (or which the display does not have at all) keeps its question. False with no window yet.
extern "C" int bb_output_is_native_mode(int width, int height) {
    SDL_Window* window = last_window;
    if (!window || width <= 0 || height <= 0) {
        return 0;
    }
    const BbSettings::OutputSize wanted{width, height};
    const BbSettings::OutputSize native = NativePixels(window);
    if (native.width <= 0 || wanted != native) {
        return 0;
    }
    int count = 0;
    SDL_DisplayMode** listed =
        SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window), &count);
    const SDL_DisplayMode* chosen = pick_display_mode(listed, count, wanted);
    const bool native_1x = chosen && chosen->pixel_density <= 1.0f && ModePixels(*chosen) == native;
    SDL_free(listed);
    return native_1x ? 1 : 0;
}

namespace {

/// Where a capture goes: out/screenshots (BB_CAPTURE_DIR overrides it), made if it is missing.
std::string CaptureDirectory() {
    const char* env = std::getenv("BB_CAPTURE_DIR");
    const std::string directory = env && env[0] ? env : "out/screenshots";
    if (!SDL_CreateDirectory(directory.c_str())) {
        std::printf("Capture: cannot use %s (%s)\n", directory.c_str(), SDL_GetError());
        std::fflush(stdout);
    }
    return directory;
}

/// A fresh name: the same second twice (or a capture from a previous session) gets a suffix.
std::string CapturePath() {
    const std::string directory = CaptureDirectory();
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    std::string path = directory + "/capture-" + stamp + ".png";
    for (int n = 2; FILE* existing = std::fopen(path.c_str(), "rb"); ++n) {
        std::fclose(existing);
        path = directory + "/capture-" + stamp + "-" + std::to_string(n) + ".png";
    }
    return path;
}

/// What the confirmation modal is asking right now, for the capture's log line.
std::string ConfirmStateText() {
    BbConfirm::Kind kind{};
    int value = 0;
    if (!BbConfirm::Pending(kind, value)) {
        return "none";
    }
    char text[96];
    if (kind == BbConfirm::Kind::WindowMode) {
        std::snprintf(text, sizeof(text), "window mode %d asked for, %d s left", value,
                      BbConfirm::SecondsLeft());
    } else {
        const auto size = BbSettings::UnpackSize(std::uint32_t(value));
        std::snprintf(text, sizeof(text), "resolution %d x %d asked for, %d s left", size.width,
                      size.height, BbConfirm::SecondsLeft());
    }
    return text;
}

} // namespace

/// The owner's bug report (Ctrl+Shift+C): the frame that is on screen as a PNG in out/screenshots,
/// a marker line in the frame log, and one line carrying everything a "wonky resolution" or
/// graphical glitch report needs — the display mode as the display itself reports it, the output
/// and the scene's render size, the swapchain layer's opacity and colour space, and the
/// confirmation modal's state. Pressed at the moment of the glitch, so the numbers are that frame's.
void CaptureBugReport(SDL_Window* window) {
    if (!window) {
        return;
    }
    const std::string path = CapturePath();
    BbOverlay::RequestCapture(path.c_str()); // the presented frame is written there (bbport_overlay)

    int window_w = 0, window_h = 0;
    SDL_GetWindowSizeInPixels(window, &window_w, &window_h);
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode* mode = SDL_GetWindowFullscreenMode(window);
    if (!mode) {
        mode = SDL_GetDesktopDisplayMode(display); // windowed or borderless: the desktop's mode
    }
    const auto& s = BbSettings::Get();
    const auto output = BbSettings::CurrentOutput();
    const char* window_mode = applied_window_mode == BbSettings::WindowBorderless ? "borderless"
                              : applied_window_mode == BbSettings::WindowFullscreen ? "fullscreen"
                                                                                   : "windowed";
    BbCapture::LayerState layer{};
    bb_capture_layer_state(window_metal_layer, &layer);
    const std::string confirm = ConfirmStateText();
    char state[768];
    std::snprintf(state, sizeof(state),
                  "window %dx%d pixels (density %.2f, %.2f Hz, %s, display %u); output %s "
                  "(aspect %s, fullscreen %s), scene render %dx%d (preset %s); layer opaque %d "
                  "colorspace %s drawable %dx%d pixelFormat %d contentsScale %.2f framebufferOnly "
                  "%d; confirm: %s",
                  window_w, window_h, SDL_GetWindowPixelDensity(window),
                  mode ? mode->refresh_rate : 0.0f, window_mode, unsigned(display),
                  BbSettings::SizeText(output).c_str(), BbSettings::AspectKey(s.aspect.load()),
                  BbSettings::FullscreenModeKey(s.fullscreen_mode.load()),
                  s.active_render_width.load(), s.active_render_height.load(),
                  BbSettings::PresetName(BbSettings::RenderPreset()), layer.opaque,
                  layer.colorspace[0] ? layer.colorspace : "(none)", layer.drawable_width,
                  layer.drawable_height, layer.pixel_format, layer.contents_scale,
                  layer.framebuffer_only, confirm.c_str());
#ifdef __APPLE__
    bb_log_display_mode(CocoaWindow(window), "capture"); // the mode CoreGraphics reports now
#endif
    std::printf("Capture: %s\nCapture: %s\n", path.c_str(), state);
    std::fflush(stdout);
    const std::string marker = path + " | " + state;
    bb_frame_log_marker(marker.c_str()); // "# CAPTURE ..." inside the frame log's own stream
}

void WindowSDL::SetWindowMode(int mode) {
    // A change the player makes asks first: the overlay shows the question, and nothing moves
    // until it is answered (BbConfirm). At startup there is no one to ask: the boot mode queues
    // and the first PollEvents applies it.
    if (applied_window_mode == -1) {
        pending_window_mode.store(mode, std::memory_order_relaxed);
    } else if (mode != applied_window_mode) {
        BbConfirm::Request(BbConfirm::Kind::WindowMode, mode, applied_window_mode);
    }
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::TakeMouse(double& dx, double& dy, int& up, int& down) {
    std::scoped_lock lock{mouse_mutex};
    dx = mouse_dx;
    dy = mouse_dy;
    up = wheel_up;
    down = wheel_down;
    mouse_dx = mouse_dy = 0;
    wheel_up = wheel_down = 0;
}

// Mouse look: relative mode while the window holds the mouse (runtime_pad.c turns its motion into
// the right stick). Motion from before is dropped, so taking the mouse does not turn the camera.
void WindowSDL::CaptureMouse(bool capture) {
    if (capture == mouse_captured.load(std::memory_order_relaxed)) {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(window, capture) && capture) {
        std::printf("Mouse look: relative mouse mode failed: %s\n", SDL_GetError());
        return;
    }
    {
        std::scoped_lock lock{mouse_mutex};
        mouse_dx = mouse_dy = 0;
        wheel_up = wheel_down = 0;
    }
    mouse_captured.store(capture, std::memory_order_relaxed);
    if (capture) {
        capture_settle_ms = SDL_GetTicks() + 100;
    }
    static bool told;
    if (capture && !told) {
        told = true;
#ifdef __APPLE__
        // bbport (macOS): F1, ` and § open the overlay menu first, which lets the mouse go too.
        std::printf("Mouse look: on (F1, ` or \u00a7 opens the menu and releases the mouse, a click "
                    "takes it again)\n");
#else
        std::printf("Mouse look: on (F1 releases the mouse, a click takes it again)\n");
#endif
    }
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextPrompt(text_active, text_prompt, text);
}

#ifdef __APPLE__
void WindowSDL::AppendTypedKey(int scancode, unsigned mod) {
    if (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
        return;
    }
    const SDL_Keycode key =
        SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), static_cast<SDL_Keymod>(mod), false);
    if ((key & SDLK_SCANCODE_MASK) || key < 0x20 || key == 0x7F || (key >= 0x80 && key < 0xA0) ||
        key > 0xFFFF) {
        return; // not a printable character (arrows, function keys, controls)
    }
    // UTF-8 encode one code point (<= U+FFFF).
    if (key < 0x80) {
        text += static_cast<char>(key);
    } else if (key < 0x800) {
        text += static_cast<char>(0xC0 | (key >> 6));
        text += static_cast<char>(0x80 | (key & 0x3F));
    } else {
        text += static_cast<char>(0xE0 | (key >> 12));
        text += static_cast<char>(0x80 | ((key >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (key & 0x3F));
    }
}
#endif

bool WindowSDL::PollEvents() {
    if (quit_requested.load(std::memory_order_relaxed)) {
        return false; // RequestQuit: a warm run (BB_WARM_ONLY) asking to close
    }
    if (int confirmed = 0; BbConfirm::TakeConfirmedWindowMode(confirmed)) {
        apply_window_mode(window, confirmed); // the player agreed: switch, on the window's thread
    }
    if (const int mode = pending_window_mode.exchange(-1, std::memory_order_relaxed); mode >= 0) {
        apply_window_mode(window, mode); // the boot mode, or a menu's (confirmed) window mode
    }
    follow_display_mode(window, applied_window_mode); // a confirmed resolution: its own mode
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    // A game module's menu wants the mouse as a pointer (bbgpu_add_pointer_query).
    const bool pointer = bbgpu_pointer_wanted() != 0;
    // The pad's buttons, as the gamepad events below are read: what is down now is what a question
    // that has just appeared must not take its answer from (BbConfirm::PadHeld).
    BbConfirm::PadHeld(ConfirmPadsDown());
    // BB_CAPTURE_AT=<seconds>: a capture that long after the start, once. The hotkey's own path,
    // without a hand on the keyboard: a hidden warm run (BB_WARM_ONLY) presses nothing.
    static const double capture_at = [] {
        const char* env = std::getenv("BB_CAPTURE_AT");
        return env && env[0] ? std::atof(env) : 0.0;
    }();
    static bool captured_at_start = false;
    if (capture_at > 0.0 && !captured_at_start && double(SDL_GetTicks()) >= capture_at * 1000.0) {
        captured_at_start = true;
        CaptureBugReport(window);
    }
    // BB_CONFIRM_AT=<seconds>: put a question up that long after the start, once — how the modal
    // is drawn is then checked without a hand on the game's own menu (BB_CAPTURE_AT photographs it).
    // A size that is not the display's native one: the case that keeps its question.
    static const double confirm_at = [] {
        const char* env = std::getenv("BB_CONFIRM_AT");
        return env && env[0] ? std::atof(env) : 0.0;
    }();
    static bool asked_at_start = false;
    if (confirm_at > 0.0 && !asked_at_start && double(SDL_GetTicks()) >= confirm_at * 1000.0) {
        asked_at_start = true;
        BbConfirm::Request(BbConfirm::Kind::OutputRes, int(BbSettings::PackSize({2560, 1080})),
                           int(BbSettings::PackSize(BbSettings::CurrentOutput())));
    }
    // BB_BUGREPORT_TRIGGER=<file>: a capture whenever the file appears (it is removed first) — the
    // hotkey's own path for tools/macos/capture_condition.sh, which takes the real screen capture
    // (post-scanout: tears show) next to this pre-present frame (tears never do). Checked at most
    // every 200 ms.
    static const char* capture_trigger = [] {
        const char* env = std::getenv("BB_BUGREPORT_TRIGGER");
        return env && env[0] ? env : nullptr;
    }();
    static Uint64 trigger_checked_ms = 0;
    if (capture_trigger && SDL_GetTicks() - trigger_checked_ms >= 200) {
        trigger_checked_ms = SDL_GetTicks();
        if (std::remove(capture_trigger) == 0) {
            std::printf("Capture: requested by %s\n", capture_trigger);
            CaptureBugReport(window);
        }
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // The capture hotkey (Ctrl+Shift+C): the frame that is on screen, its state in the log and
        // a marker in the frame log — the owner presses it at the moment of a glitch. F9 is taken
        // by the pad route recorder (src/runtime_pad.c, BB_PAD_RECORD); the port's menu keys are
        // F1, ` , § and Escape, and the game keeps C, Shift and Ctrl free (L3, R3 alone).
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_C &&
            (event.key.mod & SDL_KMOD_CTRL) != 0 && (event.key.mod & SDL_KMOD_SHIFT) != 0) {
            CaptureBugReport(window);
            continue;
        }
        if (BbConfirm::Active() &&
            (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)) {
            // A change is asking first: Enter applies it, Escape keeps the current setting.
            // The game does not see these keys (Escape would open its own menu).
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_RETURN) {
                    BbConfirm::Decide(true);
                } else if (event.key.key == SDLK_ESCAPE) {
                    BbConfirm::Decide(false);
                }
            }
            continue;
        }
        if ((event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
            BbConfirm::PadEvent(event.gbutton.button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
            // A change is asking first, on a pad: Cross applies it and Circle keeps the current
            // setting, as Enter and Escape above do. Neither the game's own menus nor the port's see
            // the event, and while the press that answered is held the game's input stays neutral
            // (BbConfirm::PadConsumed, the overlay's CapturesInput): the answer is not also a menu.
            continue;
        }
        if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_JOYSTICK_ADDED ||
            event.type == SDL_EVENT_GAMEPAD_REMOVED || event.type == SDL_EVENT_JOYSTICK_REMOVED) {
            RefreshConfirmPads(); // a pad appears (the modal needs its events) or is gone
        }
        if (FilteredByModule(&event, window)) {
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            last_mouse_motion_ms = SDL_GetTicks();
            // Just taken: the pointer's jump into relative mode is not a turn of the camera.
            if (mouse_captured.load(std::memory_order_relaxed) && SDL_GetTicks() >= capture_settle_ms) {
                std::scoped_lock lock{mouse_mutex};
                mouse_dx += event.motion.xrel;
                mouse_dy += event.motion.yrel;
            }
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
#ifndef __APPLE__
                text += event.text.text;
#endif
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
#ifdef __APPLE__
            else if (event.type == SDL_EVENT_KEY_DOWN) {
                // macOS: SDL's text-input events (Cocoa's text input system) did not arrive in
                // the game window, while key events do, so the name is built from key presses
                // with the current layout and Shift/Option state. Shortcuts are left alone.
                AppendTypedKey(static_cast<int>(event.key.scancode), static_cast<unsigned>(event.key.mod));
            }
#endif
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        // Mouse look: a click in the game takes the mouse (that click is not passed on as a
        // button: runtime_pad.c reads the buttons only while it is held); F1 and leaving the
        // window let it go.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_captured.load(std::memory_order_relaxed) &&
            mouse_look.load(std::memory_order_relaxed) && !BbOverlay::MenuOpen() && !pointer) {
            CaptureMouse(true);
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F1 &&
            mouse_captured.load(std::memory_order_relaxed)) {
            CaptureMouse(false);
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && mouse_captured.load(std::memory_order_relaxed)) {
            const float notches = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            // bbport: whole notches only. A trackpad or Magic Mouse sends many small fractions
            // (and momentum after a swipe); each counted as a notch cycled several items at once.
            static float wheel_sum = 0.0f;
            wheel_sum += notches;
            std::scoped_lock lock{mouse_mutex};
            for (; wheel_sum >= 1.0f; wheel_sum -= 1.0f) {
                ++wheel_up;
            }
            for (; wheel_sum <= -1.0f; wheel_sum += 1.0f) {
                ++wheel_down;
            }
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            CaptureMouse(false);
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            // Another display: its monitor size (Auto follows it), and the derived mode is
            // picked again from that display's list (follow_display_mode, next poll) — SDL
            // itself only carries the old mode over by its closest match.
            TellMonitor(window);
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            // In the log: a session that ends here was closed, not lost to a crash.
            if (is_open) {
                std::printf("Window: closed (%s): the game ends\n",
                            event.type == SDL_EVENT_QUIT ? "quit" : "close button");
            }
            is_open = false;
            break;
        default:
            break;
        }
    }
    // A request from the game's side (runtime_pad.c: the spray can taken): as a click would.
    if (capture_requested.exchange(false, std::memory_order_relaxed) && !BbOverlay::MenuOpen() &&
        !text_active) {
        CaptureMouse(true);
    }
    // The menu, the name dialog and mouse_look=0 want the cursor.
    if (mouse_captured.load(std::memory_order_relaxed) &&
        (BbOverlay::MenuOpen() || text_active || !mouse_look.load(std::memory_order_relaxed))) {
        CaptureMouse(false);
    }
    // The game's pointer menus: the mouse goes, and comes back when they close.
    if (pointer && mouse_captured.load(std::memory_order_relaxed)) {
        CaptureMouse(false);
        released_for_pointer = true;
    } else if (!pointer && released_for_pointer) {
        released_for_pointer = false;
        if (mouse_look.load(std::memory_order_relaxed) && !BbOverlay::MenuOpen() && !text_active &&
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
            CaptureMouse(true);
        }
    }
    pointer_shown = pointer;
    UpdateCursor();
    return is_open;
}

// Issue #3: the OS cursor over the game. Hidden in fullscreen, and in a window after 3 s without
// moving the mouse; always shown while the settings menu or a game's pointer menu is open.
void WindowSDL::UpdateCursor() {
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const bool hide = !BbOverlay::MenuOpen() && !pointer_shown &&
                      (fullscreen || SDL_GetTicks() - last_mouse_motion_ms > 3000);
    if (hide != cursor_hidden) {
        cursor_hidden = hide;
        hide ? SDL_HideCursor() : SDL_ShowCursor();
    }
}

} // namespace Frontend
