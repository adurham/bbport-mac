// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan/swapchain.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>

#include "bbport_settings.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "vulkan/instance.h"
#include "vulkan/platform.h"

#ifdef __APPLE__
// bbport_layer.mm: the CAMetalLayer's presentation overrides and its one-shot dump.
extern "C" void bb_layer_after_swapchain(void* metal_layer);
#endif

namespace Vulkan {

namespace {

/// Rec.2020 with the PQ curve: what HDR games output. Other HDR spaces would need conversions.
constexpr vk::SurfaceFormatKHR HdrFormat{
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

/// The mode asked for, and where the request came from: bbport.ini present_mode / vsync (or their
/// BB_PRESENT_MODE / BB_VSYNC overrides, bbport_settings.cpp), else the video core's default.
vk::PresentModeKHR RequestedPresentMode(const char*& source) {
    switch (BbSettings::Get().present_mode.load()) {
    case BbSettings::PresentFifo:
        source = "present_mode/vsync setting";
        return vk::PresentModeKHR::eFifo;
    case BbSettings::PresentImmediate:
        source = "present_mode/vsync setting";
        return vk::PresentModeKHR::eImmediate;
    case BbSettings::PresentMailbox:
        source = "present_mode/vsync setting";
        return vk::PresentModeKHR::eMailbox;
    default:
        break;
    }
    source = "default";
    const auto name = EmulatorSettings.GetPresentMode();
    if (name == "Fifo") {
        return vk::PresentModeKHR::eFifo;
    }
    if (name == "Immediate") {
        return vk::PresentModeKHR::eImmediate;
    }
    if (name != "Mailbox") {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, using Mailbox", name);
    }
    return vk::PresentModeKHR::eMailbox;
}

/// Fifo waits for the display's refresh (vsync); Immediate and Mailbox do not wait (Immediate can
/// tear: on the Mac it is CAMetalLayer.displaySyncEnabled = NO).
const char* VsyncText(vk::PresentModeKHR mode) {
    switch (mode) {
    case vk::PresentModeKHR::eFifo:
    case vk::PresentModeKHR::eFifoRelaxed:
        return "vsync on";
    case vk::PresentModeKHR::eImmediate:
        return "vsync off, may tear";
    default:
        return "vsync off";
    }
}

} // namespace

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window)
    : instance{instance_}, surface{CreateSurface(instance.GetInstance(), window)} {
    if (window.GetWindowInfo().type == Frontend::WindowSystemType::Metal) {
        metal_layer = window.GetWindowInfo().render_surface;
    }
    ChooseFormat();
    ChoosePresentMode();
    Create(u32(window.GetWidth()), u32(window.GetHeight()));
}

Swapchain::~Swapchain() {
    DestroyResources();
    if (swapchain) {
        instance.GetDevice().destroySwapchainKHR(swapchain);
    }
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::ChooseFormat() {
    const auto formats =
        Check<"query surface formats">(instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface));
    supports_hdr = EmulatorSettings.IsHdrAllowed() && std::ranges::contains(formats, HdrFormat);
    // A single undefined format: the surface takes any.
    if (formats.size() == 1 && formats[0].format == vk::Format::eUndefined) {
        sdr_format = {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
        return;
    }
    const auto it = std::ranges::find_if(formats, [](const vk::SurfaceFormatKHR& f) {
        return f.format == vk::Format::eR8G8B8A8Unorm || f.format == vk::Format::eB8G8R8A8Unorm;
    });
    ASSERT_MSG(it != formats.end(), "the surface has no RGBA8 or BGRA8 format");
    sdr_format = *it;
}

void Swapchain::ChoosePresentMode() {
    const auto [result, modes] = instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    const char* source = "default";
    const vk::PresentModeKHR requested = RequestedPresentMode(source);
    present_mode = requested;
    if (result != vk::Result::eSuccess || !std::ranges::contains(modes, present_mode)) {
        // FIFO is always there.
        LOG_WARNING(Render_Vulkan, "Present mode {} is not supported, using Fifo",
                    vk::to_string(present_mode));
        present_mode = vk::PresentModeKHR::eFifo;
    }
    // One line naming the mode in use: a tearing report (Direct scanout shows tears a composited
    // window hides) is read against it. The A/B matrix is docs/macos-title-artifact.md.
    std::string supported;
    for (const auto mode : modes) {
        supported += (supported.empty() ? "" : ", ") + vk::to_string(mode);
    }
    std::printf("Present mode: %s (%s; asked %s by %s; surface offers %s)\n",
                vk::to_string(present_mode).c_str(), VsyncText(present_mode),
                vk::to_string(requested).c_str(), source, supported.c_str());
    std::fflush(stdout);
}

void Swapchain::QuerySurface() {
    const auto caps = Check<"query surface capabilities">(
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface));
    extent = caps.currentExtent;
    if (extent.width == std::numeric_limits<u32>::max()) {
        // The surface takes the swapchain's size.
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    // One image more than the minimum: one to draw into while the others are shown.
    image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }
    transform = caps.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
                    ? vk::SurfaceTransformFlagBitsKHR::eIdentity
                    : caps.currentTransform;
    composite_alpha = caps.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque
                          ? vk::CompositeAlphaFlagBitsKHR::eOpaque
                          : vk::CompositeAlphaFlagBitsKHR::eInherit;
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    // A minimised window has a 0x0 surface (issue #31) and no swapchain can have that extent:
    // the old one stays (presents are skipped) until the window is back.
    QuerySurface();
    zero_extent = extent.width == 0 || extent.height == 0;
    if (zero_extent) {
        needs_recreation = true;
        return;
    }

    // The old swapchain is retired through oldSwapchain, not destroyed first: NVIDIA's Wayland
    // WSI keeps a tearing controller per swapchain and rejected a fresh one on the same surface
    // ("Surface already has a tearing controller", SurfaceLost).
    DestroyResources();
    const vk::Device device = instance.GetDevice();
    const std::array families = {instance.GetGraphicsQueueFamilyIndex(),
                                 instance.GetPresentQueueFamilyIndex()};
    const bool exclusive = families[0] == families[1];
    const auto format = use_hdr ? HdrFormat : sdr_format;
    const vk::SwapchainKHR old = swapchain;
    swapchain = Check<"create swapchain">(device.createSwapchainKHR({
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent,
        .queueFamilyIndexCount = exclusive ? 1u : 2u,
        .pQueueFamilyIndices = families.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = old,
    }));
    if (old) {
        device.destroySwapchainKHR(old);
    }
#ifdef __APPLE__
    // The driver configured the layer just now (opaque, colour space, drawable size): the
    // settings' presentation experiments go on top, and the layer is logged (Direct checks).
    bb_layer_after_swapchain(metal_layer);
#endif

    images = Check<"get swapchain images">(device.getSwapchainImagesKHR(swapchain));
    image_count = u32(images.size());
    views.resize(image_count);
    acquired.resize(image_count);
    present_ready.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        views[i] = Check<"create swapchain view">(device.createImageView({
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = format.format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        acquired[i] = Check<"create swapchain semaphore">(device.createSemaphore({}));
        present_ready[i] = Check<"create swapchain semaphore">(device.createSemaphore({}));
        SetObjectName(device, images[i], "swapchain image {}", i);
        SetObjectName(device, views[i], "swapchain view {}", i);
        SetObjectName(device, acquired[i], "swapchain acquired {}", i);
        SetObjectName(device, present_ready[i], "swapchain present ready {}", i);
    }
    frame_index = 0;
}

void Swapchain::DestroyResources() {
    const vk::Device device = instance.GetDevice();
    // The semaphores and views may still be in use by the last presents.
    if (const auto result = device.waitIdle(); result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Device idle wait before swapchain changes: {}",
                    vk::to_string(result));
    }
    for (const auto view : views) {
        device.destroyImageView(view);
    }
    for (const auto semaphore : acquired) {
        device.destroySemaphore(semaphore);
    }
    for (const auto semaphore : present_ready) {
        device.destroySemaphore(semaphore);
    }
    views.clear();
    acquired.clear();
    present_ready.clear();
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreating the swapchain: {}x{}, HDR {}", width_, height_, use_hdr);
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (use_hdr == hdr) {
        return;
    }
    use_hdr = hdr;
    Recreate(width, height);
}

bool Swapchain::AcquireNextImage() {
    const vk::Result result = instance.GetDevice().acquireNextImageKHR(
        swapchain, std::numeric_limits<u64>::max(), acquired[frame_index], VK_NULL_HANDLE,
        &image_index);
    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    default:
        Check<"acquire a swapchain image">(result);
    }
    return !needs_recreation;
}

bool Swapchain::Present() {
    const vk::PresentInfoKHR info{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };
    const vk::Result result = instance.GetPresentQueue().presentKHR(info);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        needs_recreation = true;
    } else {
        Check<"present">(result);
    }
    frame_index = (frame_index + 1) % image_count;
    return !needs_recreation;
}

} // namespace Vulkan
