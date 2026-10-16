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
#if __has_include("remote/bb_remote.h")
#include "remote/bb_remote.h" // the native GPU process: tell the game process a kept mode
#define BBPORT_CONFIRM_REMOTE 1
#endif

namespace BbConfirm {
namespace {

constexpr int TimeoutSeconds = 15;

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

} // namespace BbConfirm
