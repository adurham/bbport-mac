// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// bbport: a change that needs the player's agreement before it happens (macOS ports: PT asks
// before resolution and window-mode changes; the menu here is a console menu, so asking means a
// small modal over the game). Nothing is applied — and nothing is kept — until the player
// confirms; the timeout keeps what is on screen. Any thread may ask; the overlay draws the
// modal and decides (its own thread); the window loop takes confirmed window modes from here.
// The window loop reads the answers: Enter applies and Escape keeps on the keyboard, and on a
// gamepad Cross (the southern face button) applies and Circle (the eastern one) keeps — the pad's
// buttons do not move while a question is up, and the press that answered it is not also a press in
// the game (PadHeld, PadEvent, PadConsumed).
namespace BbConfirm {

enum class Kind {
    WindowMode, ///< BbSettings::WindowMode
    OutputRes,  ///< BbSettings::output_res (packed width<<32|height)
};

/// Ask (any thread): hold `value` for confirmation. `previous` is what to put back in the
/// settings if the player keeps the current one.
void Request(Kind kind, int value, int previous);

/// The overlay: is a question up? It draws the modal and resolves it (buttons, Enter, Escape, the
/// gamepad, the timeout). Pending gives the question itself for the modal's text.
bool Active();
bool Pending(Kind& kind, int& value);

/// The window loop: a window mode the player confirmed. True once per confirmation.
bool TakeConfirmedWindowMode(int& mode);

/// The overlay: seconds until an unanswered question keeps the current setting.
int SecondsLeft();

/// The overlay: resolves the question (true: apply; false: keep the current setting).
void Decide(bool apply);

/// The window loop, every poll: the gamepad buttons that are down now, one bit per
/// SDL_GAMEPAD_BUTTON_* of this process's open pads. A question takes them as already pressed — the
/// press that changed the option may still be down — so only a fresh press answers it: a button
/// that is let go and pressed again. A button that is not down here counts as released.
void PadHeld(std::uint32_t buttons);

/// The window loop, for every gamepad button event: true when the question took it and it must not
/// reach anything else (the game's own menus, the port's menu). A fresh press of the southern face
/// button (Cross) applies the change and of the eastern one (Circle) keeps the current setting;
/// every other event of a question, and the release of an answer, is taken and dropped.
bool PadEvent(int button, bool down);

/// The overlay: a pad press answered a question — or was held while one was up — and is still down.
/// The game gets no pad input until it is let go, so the answer is not a press in the game either.
bool PadConsumed();

} // namespace BbConfirm
