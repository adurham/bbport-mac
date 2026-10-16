// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include <string>
#include "common/types.h"
#include "vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();
/// A line of text over the game until called with null (bbgpu_overlay_banner).
void SetBanner(const char* text);

/// Window thread (the capture hotkey, window.cpp): the next presented frame is written to `path`
/// as a PNG. The pixels are the swapchain image the presenter hands the overlay, so the file is
/// the frame the port drew, at its own resolution, with no screen-recording permission asked.
void RequestCapture(const char* path);

/// Present thread: draws into `view` (layout ColorAttachmentOptimal). `presented` is the swapchain
/// image behind it, `presented_format` its format: the capture hotkey copies that one back
/// (RequestCapture) and has to know the bytes' order to write them (HDR frames are left out).
void Render(vk::CommandBuffer cmdbuf, vk::Image presented, vk::Format presented_format,
            vk::ImageView view, vk::Extent2D extent);

/// The menu or the text dialog is open: the game's input is held neutral.
bool CapturesInput();

/// The settings menu is open: the window shows the system cursor over it.
bool MenuOpen();

/// Window thread: the game's text dialog (ImeDialog) state, drawn as a box over the frame.
void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

} // namespace BbOverlay
