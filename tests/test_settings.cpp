// SPDX-License-Identifier: GPL-2.0-or-later
// bbport_settings.cpp: the menu saves its keys into bbport.ini and keeps the launcher's (controls).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "gpu/shim/bbport_settings.h"
#include "gpu/shim/bbport_confirm.h"

// bbport_settings.cpp's ReloadLive asks the confirmation modal (BbConfirm::Request lives in
// bbport_confirm.cpp, which needs SDL and the remote channel): this target links neither, so the
// test stands in for it. Nothing here calls it.
namespace BbConfirm {
void Request(Kind, int, int) {}
} // namespace BbConfirm

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/// The braces of a braced initializer list would split an assert's argument: compare through here.
static bool SizeIs(BbSettings::OutputSize size, int width, int height) {
    return size == BbSettings::OutputSize{width, height};
}

int main() {
    char path[] = "/tmp/bbport-settings-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    const char ini[] = "# launcher\nupscaler=fsr3\nkey.cross=X, Space\npad.circle=a\nshow_fps=0\n"
                       "fullscreen_hint=1\n";
    assert(write(fd, ini, sizeof(ini) - 1) == ssize_t(sizeof(ini) - 1));
    close(fd);
    setenv("BB_CONFIG", path, 1);

    BbSettings::Load();
    auto& s = BbSettings::Get();
    assert(s.upscaler == BbSettings::UpscalerFsr3 && !s.show_fps && s.menu_x < 0.0f);
    s.show_fps = true;
    s.upscaler = BbSettings::UpscalerFsr411;
    s.menu_x = 0.625f;
    s.menu_y = 0.125f;
    BbSettings::Save();

    const std::string saved = Read(path);
    // The launcher's controls, its other keys and comments stay; the menu's keys are replaced
    // in place, new ones appended.
    assert(saved.find("# launcher\nupscaler=fsr411\nkey.cross=X, Space\npad.circle=a\nshow_fps=1\n"
                      "fullscreen_hint=1\n") == 0);
    assert(saved.find("menu_pos=0.6250,0.1250\n") != std::string::npos);
    assert(saved.find("upscaler=fsr3") == std::string::npos);

    s.menu_x = -1.0f;
    s.menu_y = -1.0f;
    BbSettings::Load();
    assert(s.menu_x == 0.625f && s.menu_y == 0.125f && s.upscaler == BbSettings::UpscalerFsr411);
    assert(saved.find("menu_language=") == std::string::npos); // only started with it (#134)

    // A language chosen in the menu is saved and wins over the launcher's from then on.
    s.menu_language = BbSettings::MenuLanguage::Russian;
    s.menu_language_chosen = true;
    BbSettings::Save();
    assert(Read(path).find("menu_language=ru\n") != std::string::npos);
    setenv("BB_MENU_LANGUAGE", "en", 1);
    s.menu_language_chosen = false;
    BbSettings::Load();
    assert(s.menu_language == BbSettings::MenuLanguage::Russian && s.menu_language_chosen);

    // fullscreen_mode: desktop (the default) never sets a display mode. fill and exclusive move the
    // panel, and moving it faults this machine's display coprocessor on macOS 27.2 (three panics on
    // 2026-10-18, the last one from BetterDisplay with no app running), so this build refuses them:
    // a file or env value of fill/exclusive reads back as desktop. BB_ALLOW_DISPLAY_MODE_SET=1 is
    // the escape hatch (for a test, or for the day Apple fixes the DCP firmware).
    assert(s.fullscreen_mode == BbSettings::FullscreenDesktop && BbSettings::FullscreenFills());
    assert(BbSettings::FullscreenKeepsDesktopMode() && !BbSettings::FullscreenMovesDisplay());
    s.fullscreen_mode = BbSettings::FullscreenExclusive;
    BbSettings::Save();
    assert(Read(path).find("fullscreen_mode=exclusive\n") != std::string::npos);
    BbSettings::Load();
    assert(s.fullscreen_mode == BbSettings::FullscreenDesktop && BbSettings::FullscreenFills());
    assert(BbSettings::FullscreenKeepsDesktopMode() && !BbSettings::FullscreenMovesDisplay());
    setenv("BB_FULLSCREEN_MODE", "fill", 1);
    BbSettings::Load();
    assert(s.fullscreen_mode == BbSettings::FullscreenDesktop);
    setenv("BB_ALLOW_DISPLAY_MODE_SET", "1", 1);
    BbSettings::Load();
    assert(s.fullscreen_mode == BbSettings::FullscreenFill);
    assert(BbSettings::FullscreenFills() && BbSettings::FullscreenMovesDisplay());
    setenv("BB_FULLSCREEN_MODE", "exclusive", 1);
    BbSettings::Load();
    assert(s.fullscreen_mode == BbSettings::FullscreenExclusive && !BbSettings::FullscreenFills());
    assert(!BbSettings::FullscreenKeepsDesktopMode() && BbSettings::FullscreenMovesDisplay());
    unsetenv("BB_ALLOW_DISPLAY_MODE_SET");
    setenv("BB_FULLSCREEN_MODE", "desktop", 1);
    BbSettings::Load();
    assert(s.fullscreen_mode == BbSettings::FullscreenDesktop);
    assert(BbSettings::FullscreenKeepsDesktopMode() && !BbSettings::FullscreenMovesDisplay());
    unsetenv("BB_FULLSCREEN_MODE");

    // A resolution can only move the display mode where fullscreen puts the display itself on it
    // (exclusive); everywhere else the confirmation modal has nothing to ask about.
    s.window_mode = BbSettings::WindowFullscreen;
    assert(!BbSettings::ResolutionMovesDisplayMode());
    s.fullscreen_mode = BbSettings::FullscreenExclusive;
    assert(BbSettings::ResolutionMovesDisplayMode());
    s.fullscreen_mode = BbSettings::FullscreenFill;
    assert(!BbSettings::ResolutionMovesDisplayMode());
    s.fullscreen_mode = BbSettings::FullscreenDesktop;

    // The aspect ratio drives the output, one direction: the aspect ratio's list holds it and the
    // height is kept (BbSettings::SetAspect). Entry 5 of the 16:9 list is not entry 5 of 21:9's —
    // the game's Display page mapped a bare row index across the two and picked 2560x1440 for a
    // 21:9 setting, which pillarboxed the owner's panel.
    assert(SizeIs(BbSettings::Outputs(BbSettings::Aspect16x9).sizes[5], 1920, 1080));
    assert(SizeIs(BbSettings::Outputs(BbSettings::Aspect21x9).sizes[5], 3840, 1600));
    s.aspect = BbSettings::AspectAuto;
    s.output_res = BbSettings::PackSize({3440, 1440});
    // No window in this process: Auto follows the output in effect, so the game's Display page lists
    // the same sizes as the overlay (a 16:9 fallback made an ultrawide panel offer 16:9 sizes).
    assert(BbSettings::ResolvedAspect(BbSettings::AspectAuto) == BbSettings::Aspect21x9);
    assert(SizeIs(BbSettings::CurrentOutput(), 3440, 1440));
    BbSettings::SetAspect(BbSettings::Aspect16x9);
    assert(s.aspect == BbSettings::Aspect16x9 && SizeIs(BbSettings::CurrentOutput(), 2560, 1440));
    BbSettings::SetAspect(BbSettings::Aspect21x9);
    assert(SizeIs(BbSettings::CurrentOutput(), 3440, 1440));
    assert(BbSettings::Outputs(s.aspect).IndexOf(BbSettings::CurrentOutput()) >= 0);

    unlink(path);
    std::puts("PASS: settings save keeps other keys, menu position, menu language, fullscreen_mode, "
              "aspect-to-output");
}
