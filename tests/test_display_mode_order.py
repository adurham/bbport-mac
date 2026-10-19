"""The display-mode ordering gate (docs/DISPLAY_MODE_2026-10-18.md).

The 2026-10-18 crashes (two of them, DCPEXT1 axi_rd_err) both happened when the port changed the
display mode while its swapchain was already alive: the boot window mode was only *queued* by the
constructor and applied at the first `PollEvents()` — after the main thread had built the presenter.

These checks are source-level on purpose: the behaviour they protect cannot be exercised without
moving a real panel, and moving it is what took the machine down. They fail if the ordering is
reverted, if the boot mode is queued again, or if a set path loses its `boot_mode_phase` guard.
"""
from paths import ROOT
import re
import unittest

WINDOW = ROOT / 'gpu/shim/window.cpp'


def ctor_body() -> str:
    src = WINDOW.read_text()
    start = src.index('WindowSDL::WindowSDL(')
    # The constructor ends at the first line that is a lone closing brace at file scope.
    end = src.index('\n}\n', start)
    return src[start:end]


def display_mode_body() -> str:
    src = WINDOW.read_text()
    start = src.index('void apply_display_mode(SDL_Window* window, const char* why) {')
    end = src.index('\n}\n', src.index('#endif', start))
    return src[start:end]


class DisplayModeOrder(unittest.TestCase):
    def test_a_moving_mode_does_not_create_the_window_fullscreen(self):
        body = ctor_body()
        self.assertIn('moves_panel', body, 'the constructor no longer derives whether the boot mode '
                                           'moves the panel')
        create = re.search(r'SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,\s*\n?\s*([^;]+);', body)
        if create is None:
            self.fail('no SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN in the constructor')
        self.assertIn('!moves_panel', create.group(1),
                      'a launch whose mode moves the panel must not be born fullscreen: SDL would '
                      'set the display mode at the first PollEvents, under a live swapchain')

    def test_boot_mode_is_applied_before_the_metal_view(self):
        body = ctor_body()
        self.assertIn('if (moves_panel) {', body,
                      'the constructor no longer applies a moving boot mode itself')
        apply_at = body.index('apply_window_mode(window, window_mode)')
        metal_at = body.index('SDL_Metal_CreateView(')
        self.assertLess(apply_at, metal_at,
                        'the display mode must be set before SDL_Metal_CreateView makes a layer the '
                        'WindowServer could scan')

    def test_the_settle_wait_is_between_the_set_and_the_metal_view(self):
        body = ctor_body()
        self.assertIn('bb_display_wait_mode_change(', body,
                      'no settle wait: SDL\'s fullscreen transition is asynchronous, so the mode may '
                      'still be in flight when the layer is created')
        apply_at = body.index('apply_window_mode(window, window_mode)')
        wait_at = body.index('bb_display_wait_mode_change(')
        metal_at = body.index('SDL_Metal_CreateView(')
        self.assertLess(apply_at, wait_at, 'the wait must come after the set')
        self.assertLess(wait_at, metal_at, 'the wait must finish before the layer exists')

    def test_boot_mode_is_not_queued(self):
        body = ctor_body()
        apply_at = body.index('apply_window_mode(window, window_mode)')
        queued = body.find('SetWindowMode(window_mode)')
        self.assertNotEqual(queued, -1, 'the non-moving boot modes still go through SetWindowMode')
        self.assertLess(apply_at, queued,
                        'the moving boot mode must be applied synchronously, not queued for the '
                        'first PollEvents')

    def test_gate_is_closed_at_the_end_of_the_constructor(self):
        body = ctor_body()
        self.assertIn('boot_mode_phase = false;', body,
                      'nothing closes boot_mode_phase: a later set would be allowed')

    def test_every_set_in_apply_display_mode_is_behind_the_gate(self):
        body = display_mode_body()
        self.assertIn('const bool may_set = boot_mode_phase;', body,
                      'apply_display_mode no longer derives may_set from boot_mode_phase')
        self.assertIn('if (!dry_run && !may_set) {', body,
                      'the refusal branch (a set at runtime logs "needs a restart" and moves '
                      'nothing) is gone')
        refusal_at = body.index('if (!dry_run && !may_set) {')
        chosen_set_at = body.index('SDL_SetWindowFullscreenMode(window, chosen)')
        self.assertLess(refusal_at, chosen_set_at,
                        'the chosen-mode set must be behind the boot_mode_phase gate')
        for line_no, line in enumerate(body.splitlines()):
            if 'SDL_SetWindowFullscreenMode(window, nullptr)' not in line:
                continue
            # The guard may sit on the call's own line or on the `if (...) {` above it. The
            # SDL-refused fallback is inside the gated branch (the set was already allowed).
            context = '\n'.join(body.splitlines()[max(0, line_no - 3):line_no + 1])
            guarded = 'may_set' in context
            fallback = 'SDL refused it' in context
            self.assertTrue(guarded or fallback,
                            f'un-gated SDL_SetWindowFullscreenMode(nullptr) at line {line_no + 1} '
                            f'of apply_display_mode: {line.strip()}')


if __name__ == '__main__':
    unittest.main()
