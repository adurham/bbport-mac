// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a change the player must agree to before it happens. The ports' reference is P.T.'s
// behaviour (macOS): choosing a resolution or a window mode asks first, and nothing moves until
// the player says so. This module holds the question; the overlay draws it and decides.
#include "bbport_confirm.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "bbport_settings.h"
#include <SDL3/SDL.h> // the pad's face buttons, as the window reports them (PadEvent)
#if __has_include("remote/bb_remote.h")
#include "remote/bb_remote.h" // the native GPU process: tell the game process a kept mode
#define BBPORT_CONFIRM_REMOTE 1
#endif

namespace BbConfirm {
namespace {

// Long enough to read the question and pick up a pad: the reference port's 15 s were gone before a
// player who had put the controller down found it again.
constexpr int TimeoutSeconds = 30;

std::mutex mutex;
bool active = false;
Kind kind = Kind::WindowMode;
int value = 0;
int previous = 0;
std::chrono::steady_clock::time_point deadline;

/// A window mode the player confirmed; the window loop applies it (applying window state must
/// happen on the window's own thread).
std::atomic<bool> window_confirmed{false};
int window_confirmed_value = 0;

/// The pad (PadHeld, PadEvent): `pad_held` are the buttons that were down when the window last
/// looked — a question does not take its answer from those (the press that changed the option may
/// still be down), so each has to be let go and pressed again. `pad_consumed` are the buttons a
/// question has taken an event from: their press is the modal's, and so is their release, and the
/// game gets no pad input until none of them is down.
std::uint32_t pad_held = 0;
std::uint32_t pad_consumed = 0;

const char* KindName(Kind k) {
    return k == Kind::WindowMode ? "window mode" : "resolution";
}

} // namespace

void Request(Kind k, int v, int prev) {
    std::scoped_lock lock{mutex};
    if (active) {
        if (kind == k && value == v) {
            return; // already asking this exact question
        }
        if (kind != k) {
            previous = prev; // a different question: its own baseline
        }
        // Same kind, a newer value: keep the original "current" value as the baseline.
    } else {
        previous = prev;
    }
    printf("Confirm: %s %d asked for (keeping the current one in %d s if unanswered)\n",
           KindName(k), v, TimeoutSeconds);
    std::fflush(stdout);
    kind = k;
    value = v;
    active = true;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(TimeoutSeconds);
}

bool Active() {
    std::scoped_lock lock{mutex};
    return active;
}

bool Pending(Kind& k, int& v) {
    std::scoped_lock lock{mutex};
    if (!active) {
        return false;
    }
    k = kind;
    v = value;
    return true;
}

int SecondsLeft() {
    std::scoped_lock lock{mutex};
    if (!active) {
        return 0;
    }
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline -
                                                                       std::chrono::steady_clock::now());
    return left.count() < 0 ? 0 : int(left.count());
}

void Decide(bool apply) {
    Kind k;
    int v = 0, prev = 0;
    {
        std::scoped_lock lock{mutex};
        if (!active) {
            return;
        }
        k = kind;
        v = value;
        prev = previous;
        active = false;
    }
    auto& s = BbSettings::Get();
    switch (k) {
    case Kind::WindowMode:
        if (apply) {
            // The menu saved the new mode before asking; it stays, and the window loop switches.
            s.window_mode = v;
            BbSettings::Save();
            window_confirmed_value = v;
            window_confirmed.store(true);
            printf("Confirm: window mode %d confirmed\n", v);
        } else {
            s.window_mode = prev;
            BbSettings::Save();
#ifdef BBPORT_CONFIRM_REMOTE
            if (BbRemote::BackActive()) {
                BbRemote::Back::WindowModeKept(prev); // the game process's copy of the settings
            }
#endif
            printf("Confirm: window mode kept (%d)\n", prev);
        }
        break;
    case Kind::OutputRes:
        if (apply) {
            // Nothing was stored while the question was up: write it now and the renderer's
            // live path picks it up.
            s.output_res = v;
            BbSettings::Save();
            printf("Confirm: resolution %dx%d confirmed\n", int(v & 0xffff), int(unsigned(v) >> 16));
        } else {
            // The menu that asked already wrote the new size to bbport.ini (the game process's
            // System menu saves before the GPU process sees it): the kept one goes back, so the
            // next start (run.sh reads output_res) and the game process's copy agree with what
            // is on screen. The display mode never moved (window.cpp derives it from output_res).
            s.output_res = std::uint32_t(prev);
            BbSettings::Save();
#ifdef BBPORT_CONFIRM_REMOTE
            if (BbRemote::BackActive()) {
                BbRemote::Back::OutputResKept(std::uint32_t(prev));
            }
#endif
            printf("Confirm: resolution kept (%dx%d)\n", int(prev & 0xffff), int(unsigned(prev) >> 16));
        }
        break;
    }
    std::fflush(stdout);
}

bool TakeConfirmedWindowMode(int& mode) {
    if (window_confirmed.exchange(false)) {
        mode = window_confirmed_value;
        return true;
    }
    return false;
}

void PadHeld(std::uint32_t buttons) {
    static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32, "the buttons are a bit set");
    std::scoped_lock lock{mutex};
    // Read before the events of this poll are pumped, so it is what was down when the question
    // appeared (or a poll ago, at most): exactly what must not answer it.
    pad_held = buttons;
    // A button that is not down any more has been let go, whether its release event was seen or the
    // pad was unplugged: nothing of a question is left hanging over the game's input.
    pad_consumed &= buttons;
}

bool PadEvent(int button, bool down) {
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false; // not a button SDL told us about
    }
    const std::uint32_t bit = 1u << unsigned(button);
    int decide = 0; // 1: apply, -1: keep the current one
    {
        std::scoped_lock lock{mutex};
        if (!active && !pad_consumed) {
            return false; // no question, and nothing of one is still held: the event goes on
        }
        if (down) {
            // A fresh press answers the question: Cross applies the change, Circle keeps the
            // current setting. A button that was already down when the question appeared has to be
            // let go and pressed again (the press that changed the option may still be down).
            if (active && !(pad_held & bit)) {
                decide = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1
                         : button == SDL_GAMEPAD_BUTTON_EAST  ? -1
                                                             : 0;
            }
            pad_consumed |= bit; // the press is the modal's, and so is its release
        } else {
            pad_consumed &= ~bit;
            pad_held &= ~bit;
        }
    }
    if (decide != 0) {
        Decide(decide > 0); // the answer, as the modal's own buttons give it
    }
    return true;
}

bool PadConsumed() {
    std::scoped_lock lock{mutex};
    return pad_consumed != 0;
}

} // namespace BbConfirm
