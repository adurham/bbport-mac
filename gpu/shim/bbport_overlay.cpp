// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "bbport_frame_rate.h"
#include "bbport_confirm.h"
#include "bbport_capture.h"
#include "sdl_window.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "vulkan/instance.h"
#include "render/scheduler.h"

// The menu's own text is DejaVu Sans (Cyrillic, third_party/fonts, Bitstream Vera license); the
// confirmation modal asks in EB Garamond (third_party/fonts/EBGaramond.ttf, SIL OFL 1.1, the
// license next to the file), whose lettering sits with the game's own. Both are embedded:
// the port is a library next to the game, with no font files of its own to install.
// The paths come from CMake (gpu/CMakeLists.txt, absolute: .incbin is resolved by the
// assembler); the fallbacks are for editors that read this file without the target's flags.
#ifndef BB_FONT_PATH
#define BB_FONT_PATH "third_party/fonts/DejaVuSans.ttf"
#endif
#ifndef BB_SERIF_FONT_PATH
#define BB_SERIF_FONT_PATH "third_party/fonts/EBGaramond.ttf"
#endif
#define BB_EMBED_FONT(symbol, path)                                                              \
    asm(".section __TEXT,__const\n"                                                             \
        ".p2align 4\n"                                                                          \
        ".private_extern _" symbol "\n"                                                         \
        ".globl _" symbol "\n"                                                                  \
        "_" symbol ":\n"                                                                        \
        ".incbin \"" path "\"\n"                                                                \
        ".private_extern _" symbol "_end\n"                                                    \
        ".globl _" symbol "_end\n"                                                              \
        "_" symbol "_end:\n"                                                                    \
        ".text\n")
#ifdef __APPLE__
BB_EMBED_FONT("bb_font_ttf", BB_FONT_PATH);
BB_EMBED_FONT("bb_serif_font_ttf", BB_SERIF_FONT_PATH);
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
extern "C" const unsigned char bb_serif_font_ttf[];
extern "C" const unsigned char bb_serif_font_ttf_end[];
#else
#undef BB_EMBED_FONT
#ifdef _WIN32
#define BB_FONT_SECTION ".section .rdata,\"dr\"\n"
#define BB_FONT_HIDDEN(name) ""
#define BB_FONT_END ".text\n"
#else
#define BB_FONT_SECTION ".section .rodata\n"
#define BB_FONT_HIDDEN(name) ".hidden " name "\n"
#define BB_FONT_END ".previous\n"
#endif
#define BB_EMBED_FONT(symbol, path)                                                             \
    asm(BB_FONT_SECTION ".balign 16\n" BB_FONT_HIDDEN(symbol) ".global " symbol "\n" symbol     \
        ":\n"                                                                                   \
        ".incbin \"" path "\"\n" BB_FONT_HIDDEN(symbol "_end") ".global " symbol "_end\n" symbol \
        "_end:\n" BB_FONT_END)
BB_EMBED_FONT("bb_font_ttf", BB_FONT_PATH);
BB_EMBED_FONT("bb_serif_font_ttf", BB_SERIF_FONT_PATH);
#endif

extern "C" void runtime_restart(void); // bb-probe (probe.c)
extern Frontend::WindowSDL* g_window; // bbgpu.cpp: the game's window (the menu's window mode)
namespace Vulkan { extern std::atomic<std::uint32_t> g_bb_compiles; } // pipeline_cache.cpp (the notice below)
extern "C" __attribute__((weak)) void runtime_toypad_lights(uint32_t out[3]); // runtime_toypad.c

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;
/// The confirmation modal's font (EB Garamond, Init): the game's own lettering, where the menu
/// keeps DejaVu Sans. Null if the atlas refused it (the modal then falls back to the menu's font).
ImFont* serif_font = nullptr;
// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
/// A module's line over the game (bbgpu_overlay_banner: BBSC's "joining the host" during its
/// loads, where the game itself shows a black screen).
std::mutex banner_mutex;
std::string banner_text;
std::atomic<bool> banner_active{false};
std::string prompt_title, prompt_text;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The USB toy pad: the figures on the emulated pad, put on and lifted off from
// the menu with the gamepad. The runtime names the figures file (BB_TOYPAD_FILE) once the game
// opens the pad and reads it again when it changes; this section edits that file.
struct ToyPadFigure {
    int zone;
    unsigned id;
};
struct ToyPadName {
    unsigned id;
    std::string name;
};

std::vector<ToyPadFigure> ReadToyPad(const char* path) {
    std::vector<ToyPadFigure> figures;
    FILE* file = std::fopen(path, "r");
    char line[256];
    while (file && std::fgets(line, sizeof(line), file)) {
        if (char* hash = std::strchr(line, '#')) {
            *hash = 0;
        }
        int zone;
        unsigned id;
        if (std::sscanf(line, "%d %u", &zone, &id) == 2 && zone >= 1 && zone <= 3 && id) {
            figures.push_back({zone, id});
        }
    }
    if (file) {
        std::fclose(file);
    }
    return figures;
}

// Figure names: "<id> <name>" lines in figure_ids.txt next to the figures file (BB_TOYPAD_NAMES
// names another one); without it the ids are shown.
const std::vector<ToyPadName>& ToyPadNames(const char* figures_path) {
    static std::vector<ToyPadName> names;
    static bool loaded;
    if (!loaded) {
        loaded = true;
        std::string path;
        if (const char* env = std::getenv("BB_TOYPAD_NAMES"); env && *env) {
            path = env;
        } else {
            path = figures_path;
            const size_t slash = path.rfind('/');
            path = (slash == std::string::npos ? std::string{} : path.substr(0, slash + 1)) + "figure_ids.txt";
        }
        FILE* file = std::fopen(path.c_str(), "r");
        char line[256];
        while (file && std::fgets(line, sizeof(line), file)) {
            unsigned id;
            int used = 0;
            if (std::sscanf(line, "%u %n", &id, &used) == 1 && id) {
                std::string name = line + used;
                while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
                    name.pop_back();
                }
                names.push_back({id, name});
            }
        }
        if (file) {
            std::fclose(file);
        }
    }
    return names;
}

std::string ToyPadLabel(const std::vector<ToyPadName>& names, unsigned id) {
    for (const auto& n : names) {
        if (n.id == id) {
            return n.name + " (" + std::to_string(id) + ")";
        }
    }
    return "#" + std::to_string(id);
}

void WriteToyPad(const char* path, const std::vector<ToyPadFigure>& figures,
                 const std::vector<ToyPadName>& names) {
    const std::string temp = std::string{path} + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "w");
    if (!file) {
        return;
    }
    std::fputs("# Figures on the virtual Toy Pad: \"<zone> <figure id>\", one per line.\n"
               "# Zone 1: centre (1 figure), 2: left (3), 3: right (3). Also edited from the menu "
               "(Insert / L3+R3).\n", file);
    for (const auto& f : figures) {
        std::fprintf(file, "%d %u  # %s\n", f.zone, f.id, ToyPadLabel(names, f.id).c_str());
    }
    std::fclose(file);
    std::rename(temp.c_str(), path);
}

void ToyPadSection() {
    const char* path = std::getenv("BB_TOYPAD_FILE");
    if (!path || !*path) {
        return;
    }
    const auto& names = ToyPadNames(path);
    auto figures = ReadToyPad(path);
    bool changed = false;
    u32 lights[3] = {};
    if (runtime_toypad_lights) {
        runtime_toypad_lights(lights);
    }
    static unsigned pick_id = 1; // the figure to put on
    ImGui::SeparatorText(BbSettings::MenuText("Toy Pad", "Toy Pad (фигурки)"));
    // Choosing a figure: characters, then vehicles and gadgets (ids from 1000).
    const std::string current = ToyPadLabel(names, pick_id);
    if (ImGui::BeginCombo(BbSettings::MenuText("Figure", "Фигурка"), current.c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        for (int vehicles = 0; vehicles < 2; ++vehicles) {
            ImGui::SeparatorText(vehicles ? BbSettings::MenuText("Vehicles and gadgets", "Техника")
                                          : BbSettings::MenuText("Characters", "Персонажи"));
            for (const auto& n : names) {
                if ((n.id >= 1000) != bool(vehicles)) {
                    continue;
                }
                const std::string label = ToyPadLabel(names, n.id);
                if (ImGui::Selectable(label.c_str(), n.id == pick_id)) {
                    pick_id = n.id;
                }
                if (n.id == pick_id && ImGui::IsWindowAppearing()) {
                    ImGui::SetScrollHereY();
                }
            }
        }
        ImGui::EndCombo();
    }
    // The zones in the pad's order (left, centre, right), each lit as the game lights it: the
    // zone it wants a figure on glows.
    static const int order[] = {2, 1, 3};
    static const char* zone_en[] = {"Centre", "Left", "Right"};
    static const char* zone_ru[] = {"Центр", "Левая", "Правая"};
    for (const int zone : order) {
        ImGui::PushID(zone);
        const u32 c = lights[zone - 1];
        const ImVec4 colour = c ? ImVec4(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f,
                                         (c & 255) / 255.0f, 1.0f)
                                : ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(colour, "%s", c ? "●" : "○");
        ImGui::SameLine();
        ImGui::TextUnformatted(BbSettings::MenuText(zone_en[zone - 1], zone_ru[zone - 1]));
        int count = 0;
        for (size_t i = 0; i < figures.size(); ++i) {
            if (figures[i].zone != zone) {
                continue;
            }
            ++count;
            ImGui::PushID(int(i));
            ImGui::SameLine();
            const std::string label = ToyPadLabel(names, figures[i].id) + "  ×";
            if (ImGui::SmallButton(label.c_str())) {
                figures.erase(figures.begin() + long(i));
                changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        const bool full = count >= (zone == 1 ? 1 : 3) || figures.size() >= 7;
        if (!full) {
            ImGui::SameLine();
            if (ImGui::SmallButton(BbSettings::MenuText("+ put here", "+ поставить сюда"))) {
                figures.push_back({zone, pick_id});
                changed = true;
            }
        }
        ImGui::PopID();
    }
    Hint(BbSettings::MenuText(
        "Choose a figure, then \"+ put here\" on a zone; press a figure to lift it. The dot is the "
        "zone's light: when the game asks for a figure it lights the zone to put it on. If the "
        "game misses a figure, lift it and put it on again.",
        "Выберите фигурку и нажмите «+ поставить сюда» у зоны; нажатие на фигурку снимает её. "
        "Кружок — подсветка зоны: когда игра просит фигурку, она подсвечивает зону, куда её "
        "ставить. Если игра не заметила фигурку — снимите и поставьте снова."));
    if (changed) {
        WriteToyPad(path, figures, names);
    }
}

// The settings menu as in 0.3 and 0.4: one window (moved with the mouse, its place kept in
// bbport.ini), sections one under the other, ImGui's own widgets and keyboard/gamepad navigation.
void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where it was moved last (bbport.ini menu_pos, a fraction of the screen), kept on screen.
    ImVec2 pos(viewport->WorkPos.x + 40.0f * base_scale, viewport->WorkPos.y + 40.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
#ifdef __APPLE__ // bbport: Mac keyboards have no Insert key (HandleEvent)
    const char* menu_title =
        BbSettings::MenuText("Bloodborne - Graphics  (F1 / ` / L3+R3)###bbport_settings",
                             "Bloodborne — настройки  (F1 / ` / L3+R3)###bbport_settings");
#else
    const char* menu_title =
        BbSettings::MenuText("Bloodborne - Graphics  (Insert / L3+R3)###bbport_settings",
                             "Bloodborne — настройки  (Insert / L3+R3)###bbport_settings");
#endif
    if (!ImGui::Begin(menu_title, &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // Moved: remembered (saved with the settings when the menu closes).
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }
    // Always readable, even when the rest of the menu is in Russian.
    static const char* languages[] = {"English", "Русский"};
    int language = s.menu_language == BbSettings::MenuLanguage::Russian ? 1 : 0;
    if (ImGui::Combo("Language", &language, languages, 2)) {
        s.menu_language =
            language == 1 ? BbSettings::MenuLanguage::Russian : BbSettings::MenuLanguage::English;
        s.menu_language_chosen = true;
        BbSettings::Save();
    }
    ImGui::Text(BbSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);
    ToyPadSection();

    ImGui::SeparatorText(BbSettings::MenuText("Temporal upscaler", "Временной апскейлер"));
    const char* upscalers[] = {
        BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1",
        BbSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)"),
        "DLSS (NVIDIA RTX)", "MetalFX (Apple)"};
    static_assert(sizeof(upscalers) / sizeof(upscalers[0]) == BbSettings::UpscalerCount);
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(BbSettings::MenuText("Upscaler", "Апскейлер"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4      ? s.fsr4_supported.load()
                                   : i == BbSettings::UpscalerFsr411  ? s.fsr411_supported.load()
                                   : i == BbSettings::UpscalerDlss    ? s.dlss_supported.load()
                                   : i == BbSettings::UpscalerMetalFx ? s.metalfx_supported.load()
                                                                      : true;
#ifdef __APPLE__
            // bbport: FSR 4 needs INT8 dot products and compute derivatives that KosmicKrisp
            // lacks; DLSS needs an NVIDIA GPU.
            if ((BbSettings::IsFsr4(i) || i == BbSettings::UpscalerDlss) && !supported) {
                continue;
            }
#else
            if (i == BbSettings::UpscalerMetalFx) {
                continue; // macOS only
            }
#endif
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", BbSettings::MenuText("— not supported by this GPU",
                                                         "— не поддерживается видеокартой"));
            }
        }
#ifndef __APPLE__ // bbport: DLSS needs an NVIDIA GPU and driver, XeSS is not ported to macOS.
        static const char* later[] = {"XeSS"};
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", BbSettings::MenuText("— in development", "— в работе"));
        }
#endif
        ImGui::EndCombo();
    }
    if (s.upscaler == BbSettings::UpscalerDlss) {
        Hint(BbSettings::MenuText(
            "NVIDIA DLSS Super Resolution (RTX GPUs; Native AA is DLAA). Needs the DLSS bridge "
            "and NVIDIA's library next to the game; otherwise FSR 3.1 is used.",
            "NVIDIA DLSS Super Resolution (видеокарты RTX; Native AA — это DLAA). Нужны мост DLSS "
            "и библиотека NVIDIA рядом с игрой; без них используется FSR 3.1."));
    }
#ifdef __APPLE__
    if (const char* problem = s.metalfx_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           BbSettings::MenuText("MetalFX unavailable: %s", "MetalFX недоступен: %s"),
                           problem);
        ImGui::PopTextWrapPos();
    }
    if (s.upscaler == BbSettings::UpscalerMetalFx) {
        Hint(BbSettings::MenuText(
            "MetalFX: Apple's temporal upscaler, on the same GPU as KosmicKrisp. Slower than "
            "FSR 3.1 here (each frame goes from Vulkan to Metal and back). Native AA (x1.0) works "
            "only if the GPU supports a scale of 1; otherwise choose another preset.",
            "MetalFX: временной апскейлер Apple на том же GPU, что и KosmicKrisp. Здесь медленнее "
            "FSR 3.1 (кадр переходит из Vulkan в Metal и обратно). Native AA (x1.0) работает, "
            "только если GPU поддерживает масштаб 1: иначе выберите другой пресет."));
    }
#endif
    const auto problem_text = [](const char* text) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", text);
        ImGui::PopTextWrapPos();
    };
    if (const char* problem = s.dlss_problem.load(); problem && s.upscaler == BbSettings::UpscalerDlss) {
        char line[192];
        std::snprintf(line, sizeof(line), "DLSS: %s", problem);
        problem_text(line);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      BbSettings::MenuText("FSR 4 unavailable: %s", "FSR 4 недоступен: %s"), problem);
        problem_text(line);
        if (!BbSettings::IsFsr4(s.upscaler)) {
            ImGui::PushTextWrapPos();
            ImGui::TextUnformatted(BbSettings::MenuText(
                "The mode selected above is active. You can select FSR 4 again.",
                "Активен режим, выбранный выше. FSR 4 можно выбрать снова."));
            ImGui::PopTextWrapPos();
        }
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(BbSettings::MenuText(
                "FSR 4.1.1: the model from AMD's 4.1.1 DLL, reproduced in Vulkan (output matches "
                "the DLL). Assets are built from your DLL in the launcher.",
                "FSR 4.1.1: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan (результат совпадает "
                "с DLL). Ассеты собираются из вашей DLL в лаунчере."));
        } else {
            Hint(BbSettings::MenuText(
                "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                "model (a brief pause).",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                "пауза)."));
        }
        Checkbox(BbSettings::MenuText("FSR 4: auto exposure", "FSR 4: авто-экспозиция"),
                 s.fsr4_auto_exposure);
        Checkbox(BbSettings::MenuText("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter"),
                 s.fsr4_invert_jitter);
        Hint(BbSettings::MenuText(
            "For diagnosing ghosting: the FSR 4 network normalizes color by exposure and uses "
            "it to decide when to discard previous frames. Changes apply immediately, without a "
            "restart.",
            "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
            "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.3g)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(BbSettings::MenuText("Preset", "Пресет"), preset_label)) {
        for (const int i : BbSettings::PresetOrder) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const auto output = BbSettings::CurrentOutput();
            std::snprintf(
                label, sizeof(label),
                // bbport (macOS port): %.3g shows Ultra Quality's x1.25 (x%.1f made it x1.2).
                BbSettings::MenuText("%s (x%.3g, render %dx%d)", "%s (x%.3g, рендер %dx%d)"),
                BbSettings::PresetName(i), scale, int(std::lround(output.width / scale / 2) * 2),
                int(std::lround(output.height / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("%s", BbSettings::MenuText(
            "TAA anti-aliases the scene at output resolution, without an FSR model or upscaling. "
            "Your saved FSR preset is restored when you select FSR.",
            "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. "
            "Сохранённый пресет FSR восстановится при выборе FSR."));
    }
    ImGui::Text(
        BbSettings::MenuText("Active scene render: %d x %d", "Активный рендер сцены: %d x %d"),
        s.active_render_width.load(), s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text(BbSettings::MenuText("Startup preset: %s", "Пресет при запуске: %s"),
                    BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(BbSettings::MenuText(
                "When output is not 1080p, the entire game renders at the preset resolution "
                "(startup patch): this is fastest on Steam Deck and weaker GPUs. Preset or output "
                "resolution changes require a restart. Enable Live resolution changes below to "
                "change them without restarting (post-processing then stays at 1080p, which is "
                "slower).",
                "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): "
                "это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения "
                "вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает "
                "смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее)."));
        } else {
            Hint(BbSettings::MenuText(
                "BB_RENDER_RES fixes the scene size at startup. Remove this explicit environment "
                "variable to change resolution and presets without restarting the game.",
                "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную "
                "для смены разрешения и пресетов без перезапуска игры."));
        }
    } else {
        Hint(BbSettings::MenuText(
            "Native AA: the upscaler acts as anti-aliasing. Other presets reduce the scene render "
            "resolution relative to the output. The UI renders at output resolution. "
            "The preset applies from the next frame without restarting the game.",
            "Native AA: апскейлер работает как сглаживание. Остальные пресеты уменьшают разрешение "
            "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
            "Пресет применяется со следующего кадра без перезапуска игры."));
    }
    Checkbox(BbSettings::MenuText("Sharpening (RCAS)", "Резкость (RCAS)"), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(BbSettings::MenuText("Sharpness", "Сила резкости"), s.sharpness, 0.0f, 2.0f);
    Hint(BbSettings::MenuText(
        "Up to 1: the upscaler's own sharpening (RCAS). Above 1 adds another RCAS pass. "
        "Ctrl+click the slider (Cmd+click on a Mac) to enter an exact value.",
        "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
        "Ctrl+клик по ползунку — ввести точное значение."));
    ImGui::EndDisabled();
    // Frame generation: hidden until its artifacts are fixed (BB_FRAME_GEN=1 for tests).
    if (std::getenv("BB_FRAME_GEN")) {
        ImGui::BeginDisabled(!s.frame_generation_supported.load(std::memory_order_relaxed));
        Checkbox(BbSettings::MenuText("Frame generation (FSR 3)", "Генерация кадров (FSR 3)"),
                 s.frame_generation);
        ImGui::EndDisabled();
        if (const char* problem = s.frame_generation_problem.load(std::memory_order_relaxed)) {
            Hint(problem);
        } else {
            Hint(BbSettings::MenuText(
                "Shows a frame interpolated by FSR 3 between each two of the game's: twice the frames "
                "on screen, a little more input lag. Works with every upscaler preset except Off.",
                "Между каждыми двумя кадрами игры показывается кадр, интерполированный FSR 3: на "
                "экране вдвое больше кадров, задержка ввода чуть выше. Работает со всеми пресетами "
                "апскейлера, кроме «Выкл»."));
        }
    }
    Checkbox(BbSettings::MenuText("Subpixel jitter", "Субпиксельный сдвиг (jitter)"), s.jitter);
    Hint(BbSettings::MenuText(
        "Each frame shifts the scene by a fraction of a pixel, letting the upscaler reconstruct "
        "more detail from multiple frames. Without it, only history-based anti-aliasing remains.",
        "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
        "кадров больше деталей. Без него получается только сглаживание по истории."));

    ImGui::SeparatorText(BbSettings::MenuText("Reactive mask", "Маска реактивности"));
    ImGui::BeginDisabled(taa);
    Checkbox(BbSettings::MenuText("Enable mask", "Включить маску"), s.reactive);
    Hint(BbSettings::IsFsr4(s.upscaler)
             ? BbSettings::MenuText(
                   "Marks transparent effects (haze, light, water, particles). FSR 4 takes no "
                   "mask itself: its output is blended with the current frame there, so the "
                   "effects do not drag previous frames along.",
                   "Помечает прозрачные эффекты (дымку, свет, воду, частицы). FSR 4 сам маску не "
                   "принимает: там его результат смешивается с текущим кадром, и эффекты не "
                   "тянут за собой прошлые кадры.")
             : BbSettings::MenuText(
                   "Marks transparent effects (particles, haze) so the upscaler relies less on "
                   "previous frames. Reduces trails behind effects, but shimmering returns "
                   "underneath them.",
                   "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше "
                   "опирался на прошлые кадры. Меньше шлейфов за эффектами, но под ними "
                   "возвращается дрожание."));
    ImGui::BeginDisabled(!s.reactive);
    Slider(BbSettings::MenuText("Scale", "Масштаб"), s.reactive_scale, 0.0f, 4.0f);
    Slider(BbSettings::MenuText("Threshold", "Порог"), s.reactive_threshold, 0.0f, 1.0f);
    Slider(BbSettings::MenuText("Maximum", "Максимум"), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(BbSettings::MenuText("Show mask (debug)", "Показать маску (отладка)"),
                        &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(BbSettings::MenuText("Character motion vectors", "Векторы движения персонажей"),
             s.object_motion);
    Hint(BbSettings::MenuText(
        "Accurate vectors for animated objects: clothing and weapons break up less "
        "during movement. The static scene does not get an additional pass. "
        "Changes apply after restarting the game.",
        "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
        "при движении. Статичная сцена не получает дополнительный проход. "
        "Изменение применяется после перезапуска игры."));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(BbSettings::MenuText("Show motion vectors (debug)",
                                             "Показать векторы движения (отладка)"),
                        &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint(BbSettings::MenuText(
        "Red/green: horizontal/vertical motion (8 pixels = full brightness). "
        "Blue: the pixel received an accurate object vector, not just camera motion. "
        "The upscaler treats a moving object with no blue or red/green as "
        "stationary, causing trails.",
        "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
        "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
        "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
        "неподвижным, отсюда шлейф."));
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText(BbSettings::MenuText("Output", "Вывод"));
    // The aspect ratio first; its resolutions follow (another one keeps the height).
    const auto aspect_label = [](int a) {
        if (a != BbSettings::AspectAuto) return std::string(BbSettings::AspectKey(a));
        return std::string(BbSettings::MenuText("Auto", "Авто")) + " (" +
               BbSettings::AspectKey(BbSettings::ResolvedAspect(a)) + ")";
    };
    const int aspect = s.aspect;
    if (ImGui::BeginCombo(BbSettings::MenuText("Aspect ratio", "Соотношение сторон"),
                          aspect_label(aspect).c_str())) {
        for (int a = 0; a < BbSettings::AspectCount; ++a) {
            if (ImGui::Selectable(aspect_label(a).c_str(), a == aspect)) {
                // One direction: the chosen aspect ratio's list holds the output resolution, and
                // the height is kept (BbSettings::SetAspect — the game's Display page does the
                // same). The Resolution row below is that list.
                BbSettings::SetAspect(a);
                dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    const auto size_label = [](BbSettings::OutputSize o) {
        return std::to_string(o.width) + " x " + std::to_string(o.height);
    };
    const auto current = BbSettings::CurrentOutput();
    if (ImGui::BeginCombo(BbSettings::MenuText("Resolution", "Разрешение"), size_label(current).c_str())) {
        for (const auto& o : BbSettings::Outputs(s.aspect)) {
            if (ImGui::Selectable(size_label(o).c_str(), o == current)) {
                Store(s.output_res, BbSettings::PackSize(o), true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(BbSettings::MenuText(
            "Size of the final frame and UI. The preset sets the scene size relative to "
            "the output: 4K Performance = 1920x1080. The list follows the aspect ratio; at the "
            "display's own size the picture and the UI are pixel-for-pixel and sharpest. Another "
            "aspect ratio widens the view (the UI keeps to the screen edges). Applies after "
            "restarting the game.",
            "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
            "вывода: 4K Performance = 1920x1080. Список следует за соотношением сторон; на "
            "собственном размере дисплея картинка и интерфейс пиксель в пиксель — всего чётче. "
            "Другое соотношение сторон расширяет обзор (интерфейс у краёв экрана). Применяется "
            "после перезапуска игры."));
    } else {
        Hint(BbSettings::MenuText(
            "The final frame and UI size changes at the next frame boundary. The preset sets the "
            "scene size relative to the output: 4K Performance = 1920x1080. The list follows the "
            "aspect ratio (another one keeps the height); at the display's own size the picture "
            "and the UI are pixel-for-pixel and sharpest, and a smaller one is rendered and scaled "
            "up to fill the screen. Another aspect ratio widens the view (the UI keeps to the "
            "screen edges). Changing the size resets the upscaler's history and may cause a brief "
            "pause.",
            "Размер готового кадра и интерфейса меняется на границе следующего кадра. Пресет "
            "задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. Список следует "
            "за соотношением сторон (другое сохраняет высоту); на собственном размере дисплея "
            "картинка и интерфейс пиксель в пиксель — всего чётче, а меньший размер рисуется и "
            "растягивается на весь экран. Другое соотношение сторон расширяет обзор (интерфейс у "
            "краёв экрана). Смена размера сбрасывает историю апскейлера и может вызвать "
            "короткую паузу."));
    }
    const char* live_modes[] = {BbSettings::MenuText("Auto (based on GPU)", "Авто (по видеокарте)"),
                                BbSettings::MenuText("Off (faster)", "Выключена (быстрее)"),
                                BbSettings::MenuText("On", "Включена")};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(
            BbSettings::MenuText("Live resolution changes", "Смена разрешения на лету"),
            live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Checkbox(BbSettings::MenuText("Retina-density window (restart required)",
                                  "Окно в Retina-плотности (после перезапуска)"),
             s.retina);
    Hint(BbSettings::MenuText(
        "On a display macOS already scales, on makes the window's surface twice the panel and the "
        "compositor scales it back (heavy, and never Direct). Off matches the panel's pixels.",
        "На дисплее, который macOS уже масштабирует, «вкл» делает поверхность окна вдвое больше панели "
        "и композитор масштабирует её обратно (дорого и никогда не Direct). «выкл» совпадает с пикселями панели."));
    Hint(BbSettings::MenuText(
        "On: output resolution and preset change without restarting, but game post-processing "
        "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
        "Off: everything renders at the preset resolution, and changes require a restart. Auto "
        "enables this on powerful discrete GPUs. Applies after restarting the game.",
        "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
        "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
        "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
        "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    // Window mode (window.cpp): applied to the window at once; kept in bbport.ini.
    ImGui::SeparatorText(BbSettings::MenuText("Window", "Окно"));
    {
        static const char* modes[] = {BbSettings::MenuText("Windowed", "В окне"),
                                      BbSettings::MenuText("Borderless", "Без рамки"),
                                      BbSettings::MenuText("Fullscreen", "Во весь экран")};
        const int wm = s.window_mode;
        if (ImGui::BeginCombo(BbSettings::MenuText("Window mode", "Режим окна"), modes[wm])) {
            for (int i = 0; i < BbSettings::WindowModeCount; ++i) {
                if (ImGui::Selectable(modes[i], i == wm)) {
                    Store(s.window_mode, i, true);
                    if (g_window) {
                        g_window->SetWindowMode(i);
                    }
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Borderless fills the display without a title bar; fullscreen gives the display to "
            "the game. Switch back from this menu (F1).",
            "Без рамки — окно во весь экран без заголовка; во весь экран — игра занимает дисплей. "
            "Вернуться можно из этого же меню (F1)."));
        // How fullscreen reaches the display (window.cpp, bbport.ini fullscreen_mode): fill — the
        // default — renders the output and scales it into a window on the display's own mode; the
        // exclusive semantics puts the display itself on the output's mode.
        static const char* fullscreen_modes[] = {
            BbSettings::MenuText("Fill the screen (render and scale)",
                                 "Заполнять экран (рендер и масштаб)"),
            BbSettings::MenuText("Switch the display mode", "Переключать режим дисплея")};
        const int fullscreen_mode = std::clamp(int(s.fullscreen_mode), 0, BbSettings::FullscreenModeCount - 1);
        if (ImGui::BeginCombo(BbSettings::MenuText("Fullscreen", "Во весь экран"),
                              fullscreen_modes[fullscreen_mode])) {
            for (int i = 0; i < BbSettings::FullscreenModeCount; ++i) {
                if (ImGui::Selectable(fullscreen_modes[i], i == fullscreen_mode)) {
                    Store(s.fullscreen_mode, i, true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Fill (opt-in): the game is drawn at the chosen resolution and scaled up to the "
            "whole screen; the display keeps the mode it is on, so changing the resolution never "
            "blanks the screen and Direct keeps working. Text and UI are sharpest when the "
            "resolution is the display's own size. Switch the display mode: the display itself is "
            "put on the chosen resolution — pixel-for-pixel where the panel has it, and for other "
            "sizes the screen goes black for a moment, the panel's own scaler stretches the "
            "picture and Direct has to be set up again.",
            "Заполнять (опция): кадр рисуется в выбранном разрешении и растягивается на "
            "весь экран; дисплей остаётся в своём режиме, поэтому смена разрешения не гасит "
            "экран и Direct продолжает работать. Текст и интерфейс всего чётче, когда "
            "разрешение совпадает с собственным разрешением дисплея. Переключать режим "
            "дисплея: дисплей переходит в выбранное разрешение — пиксель в пиксель там, где "
            "панель его имеет, а для остальных размеров экран на мгновение гаснет, картинку "
            "растягивает собственный скалер панели и Direct приходится настраивать заново."));
    }
    // Frame rate (bbport_frame_rate.cpp): the port's own hooks; changed live.
    ImGui::SeparatorText(BbSettings::MenuText("Frame rate", "Частота кадров"));
    {
        const auto fps_target = [](int mode) {
            return mode == BbSettings::Fps60 ? 60 : mode == BbSettings::Fps90 ? 90
                : mode == BbSettings::Fps30 ? 30 : 0; // 0: uncapped
        };
        static const char* rates[] = {BbSettings::MenuText("30 (the game's own)", "30 (как в игре)"),
                                      "60", "90",
                                      BbSettings::MenuText("Uncapped", "Без ограничения")};
        const int fm = s.fps_mode;
        if (ImGui::BeginCombo(BbSettings::MenuText("Mode", "Режим"), rates[fm])) {
            for (int i = 0; i < BbSettings::FpsModeCount; ++i) {
                if (ImGui::Selectable(rates[i], i == fm)) {
                    Store(s.fps_mode, i, true);
                    BbFrameRate::SetRate(fps_target(i), s.fps_limit);
                }
            }
            ImGui::EndCombo();
        }
        static constexpr int limit_values[] = {0, 30, 60, 90, 120};
        static const char* limit_labels[] = {BbSettings::MenuText("Off (display refresh)", "Выкл (по экрану)"),
                                             "30", "60", "90", "120"};
        int limit_at = 0;
        for (int i = 0; i < 5; ++i) {
            if (limit_values[i] == s.fps_limit) {
                limit_at = i;
            }
        }
        if (ImGui::BeginCombo(BbSettings::MenuText("Rate limit", "Ограничение"), limit_labels[limit_at])) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(limit_labels[i], i == limit_at)) {
                    Store(s.fps_limit, limit_values[i], true);
                    BbFrameRate::SetRate(fps_target(fm), limit_values[i]);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "30 is the game's own rate; 60/90/uncapped run the game's frame rate free through the "
            "port's hooks. Uncapped is bounded by the display (FIFO) and a 480 FPS safety cap. "
            "Changes apply at once.",
            "30 — собственная частота игры; 60/90/без ограничения раскрывают частоту кадров игры "
            "хуками порта. Без ограничения упирается в частоту экрана (FIFO) и защитный предел "
            "480 FPS. Изменения применяются сразу."));
    }

    ImGui::SeparatorText(BbSettings::MenuText("Game effects (restart required)",
                                              "Эффекты игры (после перезапуска)"));
    const char* lods[] = {BbSettings::MenuText("Highest (-2)", "Максимальная (-2)"),
                          BbSettings::MenuText("Game default", "Как в игре"),
                          BbSettings::MenuText("Lower (1)", "Ниже (1)"),
                          BbSettings::MenuText("Lowest (2)", "Минимальная (2)")};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo(BbSettings::MenuText("Model detail", "Детализация моделей"),
                          lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        Checkbox(BbSettings::MenuText(effect.label, effect.label_ru), s.effects[e]);
        if (std::string_view(effect.key) == "debug_camera") {
            Hint(BbSettings::MenuText("Hold Cross and press L3 (keyboard: Space + Z).",
                                      "Удерживайте Cross и нажмите L3 (клавиатура: Space + Z)."));
        } else if (std::string_view(effect.key) == "debug_menu") {
            Hint(BbSettings::MenuText(
                "Left side of the touchpad (Tab); right side: Backspace. Needs the adhoc folder "
                "from Nexus mod #253 (the fonts in adhoc/font).",
                "Левая сторона тачпада (Tab), правая — Backspace. Нужна папка adhoc из мода "
                "Nexus #253 (шрифты в adhoc/font)."));
        }
    }
    Hint(BbSettings::MenuText(
        "Effects are enabled and disabled by game patches at startup (patches/Bloodborne.xml). "
        "Motion blur and shadows from dynamic lights place a significant load on the GPU.",
        "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
        "Размытие в движении и тени от динамических источников заметно нагружают GPU."));

    bool restart =
        s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || s.retina != s.startup_retina ||
        BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           BbSettings::MenuText("Changes apply after restarting the game",
                                                "Изменения применятся после перезапуска игры"));
        if (ImGui::Button(
                BbSettings::MenuText("Apply and restart game", "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::SeparatorText(BbSettings::MenuText("Other", "Прочее"));
    {
        bool hud = s.metal_hud;
        if (ImGui::Checkbox(BbSettings::MenuText("Metal HUD instead of the corner counter (restart required)",
                                                 "Metal HUD вместо счётчика в углу (после перезапуска)"),
                            &hud)) {
            Store(s.metal_hud, hud, true);
            if (hud) {
                Store(s.show_fps, false, true); // "instead": one counter, not both
            }
        }
        Hint(BbSettings::MenuText(
            "Apple's Metal performance HUD for the game (MTL_HUD_ENABLED): frame rate and frame "
            "times. It is read at start-up, so it applies after a restart.",
            "Metal HUD Apple для игры (MTL_HUD_ENABLED): кадры в секунду и времена кадров. "
            "Читается при запуске — применяется после перезапуска."));
    }
    Checkbox(BbSettings::MenuText("FPS counter in corner", "Счётчик FPS в углу"), s.show_fps);
    Hint(BbSettings::MenuText(
        "Ctrl+Shift+C saves a screenshot of the frame (out/screenshots) and, to the log and the "
        "frame log (BB_FRAME_LOG), the state a \"wonky resolution\" or graphical glitch report "
        "needs: display mode, output and scene resolution, the swapchain layer's opacity and "
        "colour space, and the confirmation's state.",
        "Ctrl+Shift+C сохраняет снимок кадра (out/screenshots) и записывает в журнал и в журнал "
        "кадров (BB_FRAME_LOG) состояние для отчёта о «странном разрешении» или графическом "
        "глюке: режим экрана, разрешение вывода и сцены, прозрачность и цветовое пространство "
        "слоя, состояние подтверждения."));
    Hint(BbSettings::MenuText(
        "A resolution change that lands on the display's native 1x mode applies without a "
        "question (confirm_native_mode in bbport.ini, 0 asks again).",
        "Смена разрешения на нативный режим 1x экрана применяется без вопроса (confirm_native_mode "
        "в bbport.ini, 0 — спрашивать снова)."));

    ImGui::Spacing();
    if (ImGui::Button(BbSettings::MenuText("Close", "Закрыть"))) {
        keep_open = false;
    }
    // Issues #74, #116: leaving the game from the pad (this menu opens with L3+R3). A second
    // press confirms; the window thread then closes like the window's own close button.
    static bool quit_armed;
    ImGui::SameLine();
    if (ImGui::Button(quit_armed ? BbSettings::MenuText("Press again to quit", "Нажмите ещё раз для выхода")
                                 : BbSettings::MenuText("Quit game", "Выйти из игры"))) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemFocused() && !ImGui::IsItemHovered()) {
        quit_armed = false;
    }
    ImGui::TextDisabled("%s", BbSettings::MenuText("Settings are saved to bbport.ini",
                                             "Настройки сохраняются в bbport.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text(BbSettings::MenuText("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3     ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa    ? "TAA"
                : s.upscaler == BbSettings::UpscalerMetalFx ? "MetalFX"
                : s.upscaler == BbSettings::UpscalerDlss   ? "DLSS"
                                                           : "");
    ImGui::End();
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel");
    ImGui::End();
}

/// BB_MENU_KEYS_FILE=<file> (scripted tests): tokens toggle up down left right enter back l1 r1,
/// consumed when the file appears (it is removed), one key press per frame.
void ScriptedKeys() {
    static const char* path = std::getenv("BB_MENU_KEYS_FILE");
    static std::chrono::steady_clock::time_point last_check{};
    static std::vector<std::string> queue;
    static bool release = false;
    static ImGuiKey held = ImGuiKey_None;
    if (!path) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (release) {
        io.AddKeyEvent(held, false);
        release = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (queue.empty() && now - last_check > std::chrono::milliseconds(100)) {
        last_check = now;
        if (FILE* f = std::fopen(path, "r")) {
            char token[32];
            while (std::fscanf(f, "%31s", token) == 1) {
                queue.emplace_back(token);
            }
            std::fclose(f);
            std::remove(path);
        }
    }
    if (queue.empty()) {
        return;
    }
    const std::string token = queue.front();
    queue.erase(queue.begin());
    if (token == "toggle") {
        SetOpen(!menu_open);
        return;
    }
    held = token == "up" ? ImGuiKey_GamepadDpadUp : token == "down" ? ImGuiKey_GamepadDpadDown
         : token == "left" ? ImGuiKey_GamepadDpadLeft : token == "right" ? ImGuiKey_GamepadDpadRight
         : token == "enter" ? ImGuiKey_GamepadFaceDown : token == "l1" ? ImGuiKey_GamepadL1
         : token == "r1" ? ImGuiKey_GamepadR1 : token == "kdown" ? ImGuiKey_DownArrow
         : token == "kup" ? ImGuiKey_UpArrow : ImGuiKey_None;
    if (token == "back") {
        SetOpen(false);
        return;
    }
    if (held != ImGuiKey_None) {
        io.AddKeyEvent(held, true);
        release = true;
    }
}

// ---- The capture hotkey's screenshot (window.cpp, Ctrl+Shift+C) ------------------------------
//
// The frame that is on screen is the swapchain image the presenter hands Render (bbport_overlay.h,
// RequestCapture). Nothing here reads the screen itself: a CoreGraphics window capture would ask
// for the screen-recording permission and could catch another window's frame; this copies the
// image the port is about to present, so the file is the frame the owner saw.
std::atomic<bool> capture_requested{false}; ///< window thread asked: copy the next frame
std::atomic<bool> capture_recorded{false};  ///< the copy is in the frame being presented: read it
std::mutex capture_mutex;
std::string capture_path; ///< where the PNG goes (RequestCapture)

/// The device the readback runs on, from Init (the overlay has no other use for them).
vk::Device capture_device{};
vk::PhysicalDevice capture_physical{};
vk::Buffer capture_buffer{};
vk::DeviceMemory capture_memory{};
vk::Extent2D capture_extent{};    ///< the size the buffer was made for
bool capture_bgra = true;         ///< the bytes' order, as the frame's format says

/// The buffer the presented image is copied into; remade when the frame size changes. False when
/// the device has no host-visible coherent memory (then there is no screenshot, and the log says
/// so) or the frame is not a plain 8-bit B,G,R,A image (HDR10's A2B10G10R10 has no simple bytes:
/// the owner's reports are about the default SDR path).
bool MakeCaptureBuffer(vk::Format format, vk::Extent2D extent) {
    const bool bgra8 = format == vk::Format::eB8G8R8A8Unorm ||
                       format == vk::Format::eB8G8R8A8Srgb;
    const bool rgba8 = format == vk::Format::eR8G8B8A8Unorm ||
                       format == vk::Format::eR8G8B8A8Srgb;
    if (!capture_device || (!bgra8 && !rgba8)) {
        // HDR10 (A2B10G10R10, swapchain.SetHDR) has no plain 8-bit bytes: a screenshot of it
        // would be a file of nonsense, so it is left out and said so.
        std::printf("Capture: no screenshot: the presented image is format %d (only 8-bit B,G,R,A "
                    "is read back)\n",
                    int(format));
        std::fflush(stdout);
        return false;
    }
    capture_bgra = bgra8;
    if (capture_buffer && capture_extent == extent) {
        return true;
    }
    if (capture_buffer) {
        capture_device.destroyBuffer(capture_buffer);
        capture_device.freeMemory(capture_memory);
        capture_buffer = nullptr;
        capture_memory = nullptr;
    }
    const vk::DeviceSize size = vk::DeviceSize(extent.width) * extent.height * 4;
    const auto result = capture_device.createBuffer(vk::BufferCreateInfo{
        .size = size, .usage = vk::BufferUsageFlagBits::eTransferDst});
    if (result.result != vk::Result::eSuccess) {
        return false;
    }
    capture_buffer = result.value;
    const auto properties = capture_physical.getMemoryProperties();
    const auto wanted = vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 type = ~0u;
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if ((properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
            break;
        }
    }
    const auto allocated =
        type != ~0u
            ? capture_device.allocateMemory(vk::MemoryAllocateInfo{.allocationSize = size,
                                                                   .memoryTypeIndex = type})
            : vk::ResultValue<vk::DeviceMemory>{vk::Result::eErrorOutOfDeviceMemory, {}};
    if (allocated.result != vk::Result::eSuccess) {
        capture_device.destroyBuffer(capture_buffer);
        capture_buffer = nullptr;
        return false;
    }
    capture_memory = allocated.value;
    capture_device.bindBufferMemory(capture_buffer, capture_memory, 0);
    capture_extent = extent;
    return true;
}

/// The image is the swapchain's, whose layout the presenter set to ColorAttachmentOptimal just
/// before Render (presenter.cpp): the copy takes it to TransferSrcOptimal and puts it back there,
/// so the presenter's own barrier to PresentSrcKHR still sees what it expects. Copies cannot be
/// recorded inside a render pass, so this is recorded before the overlay's own BeginRendering.
void RecordCapture(vk::CommandBuffer cmdbuf, vk::Image image, vk::Format format,
                    vk::Extent2D extent) {
    if (!image || !MakeCaptureBuffer(format, extent)) {
        capture_recorded.store(false, std::memory_order_relaxed); // nothing to read back
        return;
    }
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 to_source{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_source});
    const vk::BufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0, // tightly packed: width * 4 bytes per row
        .bufferImageHeight = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {extent.width, extent.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, capture_buffer, region);
    const vk::ImageMemoryBarrier2 back{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &back});
}

/// The frame RecordCapture copied has been presented: map the buffer and hand the bytes to the
/// PNG writer. The copy belongs to a submission this thread does not own, so the wait is on the
/// device (one frame's idle, once per hotkey press).
void ReadBackCapture() {
    capture_recorded.store(false, std::memory_order_relaxed);
    if (!capture_buffer || !capture_memory || capture_extent.width == 0) {
        return;
    }
    std::string path;
    {
        std::scoped_lock lock{capture_mutex};
        path = capture_path;
    }
    const vk::DeviceSize size =
        vk::DeviceSize(capture_extent.width) * capture_extent.height * 4;
    capture_device.waitIdle();
    const auto mapped = capture_device.mapMemory(capture_memory, 0, size);
    if (mapped.result != vk::Result::eSuccess) {
        std::printf("Capture: could not map the frame's buffer\n");
        std::fflush(stdout);
        return;
    }
    bb_capture_write_png_async(path.c_str(), mapped.value, int(capture_extent.width),
                               int(capture_extent.height), long(capture_extent.width) * 4,
                               capture_bgra);
    capture_device.unmapMemory(capture_memory);
}

} // namespace

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
    // The confirmation modal's own lettering (EB Garamond, the serif embedded above): a second
    // font, so the question over the game reads as the game's prompt and not as this menu. Larger
    // than the menu's text, as the game's own notices are.
    ImFontConfig serif_config;
    serif_config.FontDataOwnedByAtlas = false;
    serif_font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_serif_font_ttf),
                                               int(bb_serif_font_ttf_end - bb_serif_font_ttf),
                                               28.0f, &serif_config);
    if (!serif_font) {
        std::printf("Overlay: the confirmation modal's serif font did not load; its text falls "
                    "back to the menu's own\n");
    } else {
        std::printf("Overlay: fonts ready (menu 18 px DejaVu Sans, confirmation modal 28 px EB "
                    "Garamond, %d KB embedded)\n",
                    int((bb_serif_font_ttf_end - bb_serif_font_ttf) / 1024));
    }
    // The capture hotkey reads the presented frame back (RecordCapture): the device it needs.
    capture_device = instance.GetDevice();
    capture_physical = instance.GetPhysicalDevice();

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
#ifdef __APPLE__
    std::printf("Overlay: menu ready (F1, ` or §, or L3+R3)\n");
#else
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
#endif
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        // bbport: Mac keyboards have no Insert key: F1 (fn+F1 unless F-keys are standard), ` or §.
#ifdef __APPLE__
        const bool menu_key = event.key.key == SDLK_INSERT || event.key.key == SDLK_F1 ||
                              event.key.key == 0x60 /* ` */ || event.key.key == 0xA7 /* § */;
#else
        const bool menu_key = event.key.key == SDLK_INSERT;
#endif
        if (down && !event.key.repeat && (menu_key || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(menu_key ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
#ifdef __APPLE__
        // ImGui swaps Ctrl and Cmd on macOS: Cmd is its Ctrl (Cmd+click types a slider value).
        io.AddKeyEvent(ImGuiMod_Super, (event.key.mod & SDL_KMOD_GUI) != 0);
#endif
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

std::atomic<std::uint32_t> warming_count{0}; ///< compiles so far when the count last changed

/// True while shaders/pipelines are actually being compiled (real cache misses in the last
/// seconds). Cached pipelines are re-created every launch in milliseconds and are not compiles:
/// a warm launch shows nothing, the first run after an update says what it is doing instead of
/// leaving a black window that reads as a hang (bbport).
bool Warming() {
    static std::uint32_t last_count = 0;
    static std::chrono::steady_clock::time_point last_change = std::chrono::steady_clock::now();
    static bool ever = false;
    const std::uint32_t count = Vulkan::g_bb_compiles.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (count != last_count) {
        last_count = count;
        last_change = now;
        ever = true;
    }
    warming_count.store(count, std::memory_order_relaxed);
    return initialized && ever && now - last_change < std::chrono::seconds(5);
}

void SetBanner(const char* text) {
    {
        std::scoped_lock lock{banner_mutex};
        banner_text = text ? text : "";
    }
    banner_active = text != nullptr && text[0] != 0;
}

bool Visible() {
    // The capture hotkey's frame too: with nothing else drawn the presenter still has to call
    // Render for the copy to be recorded and read back (bbport_overlay.h, RequestCapture).
    return initialized && (menu_open || prompt_active || banner_active ||
                           BbSettings::Get().show_fps || Warming() || BbConfirm::Active() ||
                           capture_requested.load(std::memory_order_relaxed) ||
                           capture_recorded.load(std::memory_order_relaxed));
}

void RequestCapture(const char* path) {
    {
        std::scoped_lock lock{capture_mutex};
        capture_path = path ? path : "";
    }
    capture_requested.store(true, std::memory_order_relaxed);
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    // A pending confirmation too: Enter and Escape are its answers, and the pad's Cross and Circle.
    // And while the pad press that answered one is still held (BbConfirm::PadConsumed): the game
    // gets its input back when the button is let go, so the answer is not a press in the game too.
    return menu_open || prompt_active || BbConfirm::Active() || BbConfirm::PadConsumed();
}

void Render(vk::CommandBuffer cmdbuf, vk::Image presented, vk::Format presented_format,
            vk::ImageView view, vk::Extent2D extent) {

    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    // BB_FPS_PRINT=1: the present rate every 5 s on stdout, without the cost of BB_FRAME_STATS
    // (its timers are in the draw path): to measure as players run the game.
    static const bool fps_print = [] {
        const char* env = std::getenv("BB_FPS_PRINT");
        return env && env[0] == '1';
    }();
    if (fps_print) {
        static auto window_start = now;
        static std::uint64_t presents = 0;
        ++presents;
        if (const double s = std::chrono::duration<double>(now - window_start).count(); s >= 5.0) {
            std::printf("Present rate: %.1f FPS over %.1f s\n", presents / s, s);
            std::fflush(stdout);
            window_start = now;
            presents = 0;
        }
    }
    if (std::getenv("BB_MENU_KEYS_FILE") && initialized) {
        std::scoped_lock lock{imgui_mutex};
        ScriptedKeys();
    }
    if (capture_recorded.load(std::memory_order_acquire)) {
        // The frame the capture hotkey asked for has been presented (RecordCapture recorded the
        // copy in that frame's command buffer): take its pixels now.
        ReadBackCapture();
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    if (!menu_open && Warming()) {
        // The first launch after an update compiles pipelines for a while; say so over the
        // loading screens instead of leaving a black window that reads as a hang (bbport).
        char text[160];
        std::snprintf(text, sizeof(text),
                      BbSettings::MenuText("Warming shaders... %u compiled",
                                           "Прогрев шейдеров... скомпилировано %u"),
                      (unsigned)warming_count.load());
        const ImVec2 size = ImGui::CalcTextSize(text);
        auto* draw = ImGui::GetForegroundDrawList();
        const ImVec2 at{(io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y * 0.30f};
        draw->AddRectFilled({at.x - 24.0f, at.y - 12.0f}, {at.x + size.x + 24.0f, at.y + size.y + 12.0f},
                            IM_COL32(0, 0, 0, 170), 6.0f);
        draw->AddText(at, IM_COL32(230, 220, 200, 255), text);
    }
    if (banner_active) {
        std::string text;
        {
            std::scoped_lock banner_lock{banner_mutex};
            text = banner_text;
        }
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        auto* draw = ImGui::GetForegroundDrawList();
        const ImVec2 at{(io.DisplaySize.x - size.x) * 0.5f, io.DisplaySize.y * 0.82f};
        draw->AddRectFilled({at.x - 24.0f, at.y - 12.0f}, {at.x + size.x + 24.0f, at.y + size.y + 12.0f},
                            IM_COL32(0, 0, 0, 170), 6.0f);
        draw->AddText(at, IM_COL32(230, 220, 200, 255), text.c_str());
    }
    if (BbConfirm::Active() && BbConfirm::SecondsLeft() == 0) {
        BbConfirm::Decide(false); // no answer in time: the current setting stays
    }
    if (BbConfirm::Active()) {
        // A change asked first (BbConfirm): say what it would switch to, and take the answer.
        // Enter or Cross (the window loop reads them) applies it; Escape or Circle keeps the current
        // setting. The dimming says the game is waiting for the answer.
        BbConfirm::Kind kind{};
        int value = 0;
        if (BbConfirm::Pending(kind, value)) {
            char question[192];
            if (kind == BbConfirm::Kind::WindowMode) {
                static const char* modes[] = {BbSettings::MenuText("Windowed", "В окне"),
                                              BbSettings::MenuText("Borderless", "Без рамки"),
                                              BbSettings::MenuText("Fullscreen", "Во весь экран")};
                std::snprintf(question, sizeof(question),
                              BbSettings::MenuText("Switch to %s?", "Переключиться в «%s»?"),
                              modes[std::clamp(value, 0, BbSettings::WindowModeCount - 1)]);
            } else {
                const auto size = BbSettings::UnpackSize(std::uint32_t(value));
                std::snprintf(question, sizeof(question),
                              BbSettings::MenuText("Switch to %d x %d?", "Переключиться на %d x %d?"),
                              size.width, size.height);
            }
            auto* draw = ImGui::GetForegroundDrawList();
            // The game's own notices dim what is behind them; the same, so the question reads as
            // the game waiting for an answer rather than as a settings window over it.
            draw->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(0, 0, 0, 130));

            // Bloodborne's prompt: a near-black parchment panel, one thin pale-gold rule around
            // it, parchment text, and its own Cross/Circle wording — not this port's menu (which
            // stays DejaVu Sans on its own dark blue-grey). Sizes are already scaled to the
            // display (style.ScaleAllSizes above), so the units here are the display's pixels.
            const float unit = base_scale;
            const ImVec4 parchment(228.0f / 255.0f, 222.0f / 255.0f, 208.0f / 255.0f, 1.0f);
            const ImVec4 quiet(168.0f / 255.0f, 158.0f / 255.0f, 138.0f / 255.0f, 1.0f);
            const ImVec4 rule(196.0f / 255.0f, 178.0f / 255.0f, 130.0f / 255.0f, 1.0f);
            const ImVec4 button(28.0f / 255.0f, 24.0f / 255.0f, 20.0f / 255.0f, 0.85f);
            const float panel_width = 660.0f * unit;
            const float padding = 38.0f * unit;
            const float gap = 22.0f * unit;
            const float content = panel_width - padding * 2.0f;
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(12.0f / 255.0f, 10.0f / 255.0f,
                                                            9.0f / 255.0f, 235.0f / 255.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, rule);
            ImGui::PushStyleColor(ImGuiCol_Text, parchment);
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, quiet);
            ImGui::PushStyleColor(ImGuiCol_Separator,
                                  ImVec4(rule.x, rule.y, rule.z, 0.4f));
            ImGui::PushStyleColor(ImGuiCol_Button, button);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(rule.x, rule.y, rule.z, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(rule.x, rule.y, rule.z, 0.32f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, 30.0f * unit));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 16.0f * unit));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20.0f * unit, 9.0f * unit));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            const bool serif = serif_font != nullptr;
            if (serif) {
                ImGui::PushFont(serif_font, 0.0f); // the modal's own size, from the atlas
            }
            ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                                    ImGuiCond_Always, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({panel_width, 0.0f}, ImGuiCond_Always); // height: fit the text
            ImGui::Begin("##bbconfirm", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoScrollbar);
            // Centred lines: the panel is a fixed width, so each line finds its own middle.
            const auto centred = [&](const char* text) {
                ImGui::SetCursorPosX(padding + (content - ImGui::CalcTextSize(text).x) * 0.5f);
            };
            centred(question);
            ImGui::TextUnformatted(question);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            const char* switch_text = BbSettings::MenuText("Switch  (Cross)", "Переключить  (Крест)");
            const char* keep_text =
                BbSettings::MenuText("Keep current  (Circle)", "Оставить текущее  (Круг)");
            const float switch_width = ImGui::CalcTextSize(switch_text).x + 40.0f * unit;
            const float keep_width = ImGui::CalcTextSize(keep_text).x + 40.0f * unit;
            bool answer = false;
            bool answered = false;
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(rule.x, rule.y, rule.z, 0.55f));
            ImGui::SetCursorPosX(padding + (content - switch_width - keep_width - gap) * 0.5f);
            if (ImGui::Button(switch_text, {switch_width, 0.0f})) {
                answer = true;
                answered = true;
            }
            ImGui::SameLine(0.0f, gap);
            if (ImGui::Button(keep_text, {keep_width, 0.0f})) {
                answered = true;
            }
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::Spacing();
            // The keyboard, and the countdown, as one quiet line under the prompts.
            char subtitle[192];
            std::snprintf(subtitle, sizeof(subtitle),
                          BbSettings::MenuText(
                              "Keeping the current setting in %d s  —  Enter switches, Esc keeps",
                              "Текущее останется через %d с  —  Enter — переключить, Esc — оставить"),
                          BbConfirm::SecondsLeft());
            ImGui::PushFont(nullptr, 17.0f); // smaller than the question, the same serif
            centred(subtitle);
            ImGui::TextDisabled("%s", subtitle);
            ImGui::PopFont();
            ImGui::End();
            if (serif) {
                ImGui::PopFont();
            }
            ImGui::PopStyleVar(7);
            ImGui::PopStyleColor(8);
            if (answered) {
                BbConfirm::Decide(answer);
            }
        }
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
    if (capture_requested.exchange(false, std::memory_order_acq_rel)) {
        // The capture hotkey (window.cpp, Ctrl+Shift+C): copy the frame that is about to be
        // presented into host memory. It is the swapchain image and this is after the overlay's
        // own draw, so the file is the frame on screen with the port's menu, counter or question
        // over it, exactly as the owner saw it. Copies cannot be recorded inside a render pass,
        // which is why this is here and not with the drawing; the image goes back to the layout
        // the presenter's barrier to PresentSrcKHR expects, and the pixels are taken next frame.
        RecordCapture(cmdbuf, presented, presented_format, extent);
        if (capture_buffer) {
            capture_recorded.store(true, std::memory_order_release);
        }
    }
}

} // namespace BbOverlay

extern "C" void bbgpu_overlay_banner(const char* text) {
    BbOverlay::SetBanner(text);
}
