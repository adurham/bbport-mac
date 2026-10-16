// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// bbport: a change that needs the player's agreement before it happens (macOS ports: PT asks
// before resolution and window-mode changes; the menu here is a console menu, so asking means a
// small modal over the game). Nothing is applied — and nothing is kept — until the player
// confirms; the timeout keeps what is on screen. Any thread may ask; the overlay draws the
// modal and decides (its own thread); the window loop takes confirmed window modes from here.
namespace BbConfirm {

enum class Kind {
    WindowMode, ///< BbSettings::WindowMode
    OutputRes,  ///< BbSettings::output_res (packed width<<32|height)
};

/// Ask (any thread): hold `value` for confirmation. `previous` is what to put back in the
/// settings if the player keeps the current one.
void Request(Kind kind, int value, int previous);

/// The overlay: is a question up? It draws the modal and resolves it (buttons, Enter, Escape,
/// timeout). Pending gives the question itself for the modal's text.
bool Active();
bool Pending(Kind& kind, int& value);

/// The window loop: a window mode the player confirmed. True once per confirmation.
bool TakeConfirmedWindowMode(int& mode);

/// The overlay: seconds until an unanswered question keeps the current setting.
int SecondsLeft();

/// The overlay: resolves the question (true: apply; false: keep the current setting).
void Decide(bool apply);

} // namespace BbConfirm
