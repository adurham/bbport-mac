// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerMetalFx = 6, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
// bbport: UltraQuality (x1.25) comes last so the stored numbers of the others stay.
enum Preset : int {
    NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, UltraQuality, PresetCount
};
/// The presets in menu order (finest first).
inline constexpr int PresetOrder[PresetCount] = {NativeAA,     UltraQuality, Quality,
                                                 Balanced,     Performance,  UltraPerformance};
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum class MenuLanguage { English, Russian };

/// The game window's mode (window.cpp): kept in bbport.ini, changed from the menu live.
enum WindowMode : int { WindowWindowed = 0, WindowBorderless, WindowFullscreen, WindowModeCount };
/// How fullscreen is done (window.cpp); bbport.ini fullscreen_mode, BB_FULLSCREEN_MODE overrides.
/// Desktop (the default): the display keeps the mode it is on — the window covers it and the
/// output resolution is rendered and scaled into the window. Nothing calls
/// CGDisplaySetDisplayMode, so nothing can move the panel (a mode switch from a HiDPI desktop
/// mode while a fullscreen drawable is being promoted panicked the display coprocessor on
/// 2026-10-18: DCPEXT1 axi_rd_err).
/// Fill: the display is put on its own native 1x mode once, and the output is scaled into it —
/// one blit, always the whole panel, and the mode never moves again.
/// Exclusive: the display itself is put on the output's mode — 1:1 pixels when the panel has that
/// size, and for every other one a mode switch: flashes, the panel's scaler and a Direct
/// renegotiation. The reference ports (P.T.) do the latter; it is opt-in here.
enum FullscreenMode : int {
    FullscreenDesktop = 0,
    FullscreenFill,
    FullscreenExclusive,
    FullscreenModeCount
};
/// bbport.ini value ("desktop", "fill", "exclusive").
const char* FullscreenModeKey(int mode);
/// The game's frame rate (bbport_frame_rate): the game's own 30, or the port's hooks at 60/90 with
/// no cap beyond the pacer's safety maximum.
enum FpsMode : int { Fps30 = 0, Fps60, Fps90, FpsUncapped, FpsModeCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label (Russian, English), default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    const char* label_ru;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", "Хроматическая аберрация", true},
    {"effect_dof", "Depth of field (DoF)", "Глубина резкости (DoF)", true},
    {"effect_motion_blur", "Motion blur", "Размытие в движении", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", "Затенение SSAO", true},
    {"effect_game_aa", "Game's own anti-aliasing", "Собственное сглаживание игры", true},
    {"effect_dynamic_shadows", "Shadows from dynamic lights", "Тени от динамических источников", true},
    {"effect_ssr", "Screen-space reflections (not in original game)", "Отражения SSR (не было в игре)", false},
    {"skip_intro", "Skip startup intros", "Пропуск заставок при запуске", false},
    {"debug_camera", "Free camera (Cross + L3)", "Свободная камера (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", "Debug menu (нужны файлы шрифтов)", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Output aspect ratios, and the output resolutions of each: the upscaler's output and the UI's
/// targets, changed live. The game keeps its 1920x1080 frame; for another aspect ratio its camera,
/// HUD markers and UI placement follow the output at run time (bbport_guest_hooks, the UI
/// composition). Auto: the group of the monitor the game is on.
enum Aspect : int { AspectAuto = 0, Aspect4x3, Aspect16x10, Aspect16x9, Aspect21x9, Aspect32x9, AspectCount };
struct OutputSize {
    int width = 1920, height = 1080;
    bool operator==(const OutputSize&) const = default;
};
inline constexpr OutputSize OutputDefault{1920, 1080}; ///< the game's own size
constexpr std::uint32_t PackSize(OutputSize s) {
    return std::uint32_t(s.width) | std::uint32_t(s.height) << 16;
}
constexpr OutputSize UnpackSize(std::uint32_t v) {
    return {int(v & 0xffff), int(v >> 16)};
}
/// An aspect ratio's outputs, smallest first.
struct OutputList {
    std::array<OutputSize, 12> sizes{};
    int count = 0;
    const OutputSize* begin() const { return sizes.data(); }
    const OutputSize* end() const { return sizes.data() + count; }
    int IndexOf(OutputSize size) const; ///< -1: not in the list
};
/// bbport.ini value ("auto", "4:3", "16:10", "16:9", "21:9", "32:9").
const char* AspectKey(int aspect);
/// The monitor the game's window is on (window.cpp): Auto follows it. 0x0 until known.
void SetMonitor(int width, int height);
OutputSize Monitor();
/// Auto resolved to the group nearest the monitor's aspect ratio (16:9 until it is known; the
/// game process has no window and never learns it, so there the output in effect stands in for it
/// — both menus then list the same sizes). 16:9 with neither.
int ResolvedAspect(int aspect);
/// The output resolutions of an aspect ratio (Auto: the monitor's group and its own size).
OutputList Outputs(int aspect);
/// The output of `aspect` nearest `size` in height: another aspect ratio keeps the height.
OutputSize NearestOutput(int aspect, OutputSize size);
/// The aspect ratio drives the output, never the other way round: the chosen aspect ratio's list
/// holds the output, keeping its height (NearestOutput). The F1 overlay's Aspect row and the game's
/// Display page both change the pair through this.
void SetAspect(int aspect);
/// The output in use: the chosen resolution in the chosen aspect ratio.
OutputSize CurrentOutput();
/// "2560x1080".
std::string SizeText(OutputSize size);

/// bbport.ini present_mode (Values::present_mode).
enum PresentModeChoice : int { PresentModeUnset = -1, PresentFifo = 0, PresentImmediate = 1, PresentMailbox = 2 };
/// "fifo", "immediate", "mailbox" or "unset".
const char* PresentModeName(int mode);

struct Values {
    std::atomic<MenuLanguage> menu_language{MenuLanguage::Russian};
    /// Chosen in the menu (or already in bbport.ini): only then is it saved, so the language
    /// the menu merely started with keeps following the launcher's (#134).
    std::atomic<bool> menu_language_chosen{false};
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// The settings menu's position (fraction of the screen), -1 until it is moved.
    std::atomic<float> menu_x{-1.0f}, menu_y{-1.0f};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    /// bbport (macOS memory governor, vulkan/instance.cpp): an upper bound, in MiB, on the GPU memory
    /// budget the texture collector works to, i.e. on the renderer's own share of graphics memory
    /// (0 = off, the default). Read once when the GPU device is created; bbport.ini only (the
    /// menu does not write it), BB_RENDERER_SHARE_CAP overrides.
    std::atomic<int> renderer_share_cap{0};
    std::atomic<int> aspect{AspectAuto};
    /// The chosen output resolution (PackSize; CurrentOutput fits it to the aspect ratio).
    std::atomic<std::uint32_t> output_res{PackSize(OutputDefault)};
    /// bbport.ini has an output_res (a valid one): unset, the window takes the display's native
    /// pixel size as the default (window.cpp).
    std::atomic<bool> output_res_chosen{false};
    /// macOS presentation experiments (bbport_layer.mm), bbport.ini layer_opaque /
    /// layer_colorspace, BB_LAYER_OPAQUE / BB_LAYER_COLORSPACE override: what the swapchain's
    /// CAMetalLayer gets after the Vulkan driver configured it. -1 leaves the driver's value;
    /// opaque 0/1; colorspace 0 nil (no colour matching), 1 sRGB, 2 the display's own.
    std::atomic<int> layer_opaque{-1};
    std::atomic<int> layer_colorspace{-1};
    /// The swapchain's present mode (vulkan/swapchain.cpp), bbport.ini present_mode =
    /// fifo | immediate | mailbox, or vsync = 1 | 0; BB_PRESENT_MODE / BB_VSYNC override for one
    /// run. -1: unset (the video core's default, Mailbox, which KosmicKrisp lacks: Fifo). On the
    /// Mac Fifo is CAMetalLayer displaySyncEnabled=YES, Immediate is NO (tears under Direct).
    std::atomic<int> present_mode{PresentModeUnset};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Drawable density (macOS: SDL high pixel density; BB_RETINA overrides). The drawable should
    /// match the display mode's *framebuffer* pixels: on a 1x mode that is the panel's size, on a
    /// scaled (HiDPI) desktop it is the backing store (6880x2880 for "looks like 3440x1440"), which
    /// is what Direct scan-out requires. Only the encode pass runs at the drawable size; the scene
    /// renders at the output resolution.
    std::atomic<bool> retina{true};
    /// The game window's mode (WindowMode; window.cpp applies it at creation and live). The
    /// legacy BB_FULLSCREEN=1 asks for fullscreen when the file does not choose a mode.
    std::atomic<int> window_mode{WindowWindowed};
    /// How fullscreen puts the picture on the display (FullscreenMode; window.cpp). Desktop —
    /// the default — keeps the display's mode and scales the output into the window; Fill puts
    /// the panel on its native 1x mode once; Exclusive switches the display to the output's mode.
    std::atomic<int> fullscreen_mode{FullscreenDesktop};
    /// Whether a resolution change that lands on the display's native 1x mode applies without the
    /// confirmation modal (bbport_confirm.cpp, BB_CONFIRM_NATIVE): the reference ports show no
    /// dialog for the native size at all, and it is the size most changes pick. Off: ask for
    /// every change, as before.
    std::atomic<bool> confirm_native_mode{true};
    /// The frame rate the port's hooks aim for (FpsMode; bbport_frame_rate.cpp changed live).
    std::atomic<int> fps_mode{FpsUncapped};
    /// Rate limit in FPS for the pacer (0: none; FIFO still follows the display's refresh).
    std::atomic<int> fps_limit{0};
    /// Apple's Metal performance HUD instead of the port's FPS counter (MTL_HUD_ENABLED for the
    /// game process, read by Metal before the first device): applies after a restart.
    std::atomic<bool> metal_hud{false};
    /// FSR 3 frame generation (upscale/vk_frame_generation): a frame interpolated between each
    /// two of the game's, shown between them (needs the upscaler's scaled path). Live.
    std::atomic<bool> frame_generation{false};
    /// Whether the GPU can (set by the renderer), or why not.
    std::atomic<bool> frame_generation_supported{false};
    std::atomic<const char*> frame_generation_problem{nullptr};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// MetalFX (macOS with KosmicKrisp only); why it cannot run, or null. Set by the renderer.
    std::atomic<bool> metalfx_supported{false};
    std::atomic<const char*> metalfx_problem{nullptr};
    /// DLSS (gpu/dlss_bridge, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    /// The output the startup patch was made for (BB_OUTPUT_RES), else 0x0.
    OutputSize startup_output{0, 0};
    int startup_live_resolution = 0;
    int startup_retina = 1;
};

Values& Get();
/// window.cpp: whether fullscreen renders the chosen output and scales it into a window on the
/// display's own mode (desktop and fill) instead of putting the display on the output's mode.
inline bool FullscreenFills() {
    return Get().fullscreen_mode.load(std::memory_order_relaxed) != FullscreenExclusive;
}
/// window.cpp: whether fullscreen leaves the display's mode exactly as the desktop has it — the
/// default, and the only one that never issues a display mode set.
inline bool FullscreenKeepsDesktopMode() {
    return Get().fullscreen_mode.load(std::memory_order_relaxed) == FullscreenDesktop;
}
/// window.cpp: whether fullscreen may set a display mode at all (fill: the panel's native 1x
/// mode once; exclusive: the output's mode). Both are opt-in; the default never does.
inline bool FullscreenMovesDisplay() {
    return !FullscreenKeepsDesktopMode();
}
/// bbport_confirm.cpp: whether a resolution change can move the display mode at all — only where
/// fullscreen puts the display itself on the output's mode. Everywhere else the display keeps its
/// own mode, so there is nothing to ask about (the reference ports' question is about the display
/// switching under the player, not about the frame's size).
inline bool ResolutionMovesDisplayMode() {
    const auto& v = Get();
    return v.window_mode.load(std::memory_order_relaxed) == WindowFullscreen &&
           v.fullscreen_mode.load(std::memory_order_relaxed) == FullscreenExclusive;
}
/// Localized overlay text; the Language selector itself stays in English.
const char* MenuText(const char* english, const char* russian);

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame: unsupported FSR 4 uses FSR 3.1, MetalFX off.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool metalfx = false);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();
/// bbport (macOS native GPU process): reads bbport.ini again into the values the game's System menu
/// pages change (output, upscaler, preset, sharpening, FPS counter, model detail, effects), so
/// that the process which did not make a change follows it. Startup values stay.
void ReloadLive();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
