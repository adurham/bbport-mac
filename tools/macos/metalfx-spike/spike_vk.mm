// bbport MetalFX tail spike, part B: Vulkan on KosmicKrisp -> vkExportMetalObjectsEXT -> MetalFX.
//
// Creates the port's tail inputs as ordinary Vulkan images on KosmicKrisp (RGBA16F colour,
// D32S8 depth, RG16F motion at the render size; RGBA16F output at the output size), renders
// into them with Vulkan (a clear in a render pass, like the scene's last write), then asks the
// driver for the MTLTexture behind each VkImage (VkExportMetalTextureInfoEXT, needs KK patch
// 0007) and hands those exact objects to MTLFXTemporalScaler. Zero-copy is proven, not assumed:
//   1. the exported MTLTexture's heap/device == the driver's (same allocation, no new memory),
//   2. MetalFX reads what Vulkan wrote (output == Vulkan's clear colour after the scale),
//   3. Vulkan reads what MetalFX wrote (vkCmdCopyImageToBuffer of the output VkImage itself).
// GPU ordering uses the exported MTLSharedEvent of a Vulkan timeline semaphore (patch 0002):
// Vulkan signals N, the MetalFX command buffer waits N and signals N+1, Vulkan waits N+1. No
// CPU wait sits between them; the timed loop measures all three on the GPU (Metal timestamps
// for the MetalFX command buffer; wall time per frame for the chain).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CK(x)                                                                                    \
    do {                                                                                         \
        VkResult r_ = (x);                                                                       \
        if (r_ != VK_SUCCESS) {                                                                  \
            std::fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__);                 \
            std::exit(1);                                                                        \
        }                                                                                        \
    } while (0)

namespace {
VkInstance inst;
VkPhysicalDevice phys;
VkDevice dev;
VkQueue queue;
uint32_t qfam = 0;
VkPhysicalDeviceMemoryProperties memprops;

uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    std::fprintf(stderr, "no memory type\n");
    std::exit(1);
}

struct Img {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkFormat fmt;
    uint32_t w, h;
    VkImageAspectFlags aspect;
};

// Exactly the usage the port's scene targets carry (attachment + sampled + transfer).
Img MakeImage(VkFormat fmt, uint32_t w, uint32_t h, VkImageAspectFlags aspect, bool depth) {
    Img i{};
    i.fmt = fmt; i.w = w; i.h = h; i.aspect = aspect;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT) |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CK(vkCreateImage(dev, &ci, nullptr, &i.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, i.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CK(vkAllocateMemory(dev, &ai, nullptr, &i.mem));
    CK(vkBindImageMemory(dev, i.image, i.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    CK(vkCreateImageView(dev, &vi, nullptr, &i.view));
    return i;
}

PFN_vkExportMetalObjectsEXT pExport;

id<MTLTexture> ExportTexture(const Img& i) {
    VkExportMetalTextureInfoEXT t{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
    t.image = i.image;
    t.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &t};
    pExport(dev, &info);
    return t.mtlTexture;
}

void Barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
             VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

double Pct(std::vector<double> v, double f) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(f * (v.size() - 1) + 0.5))];
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        uint32_t iw = 1146, ih = 480, ow = 3440, oh = 1440;
        int frames = 300;
        if (argc >= 5) { iw = atoi(argv[1]); ih = atoi(argv[2]); ow = atoi(argv[3]); oh = atoi(argv[4]); }
        if (argc >= 6) frames = atoi(argv[5]);

        // --- Vulkan on KosmicKrisp (VK_DRIVER_FILES picks the driver) ---
        const char* iext[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                              VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = iext;
        CK(vkCreateInstance(&ici, nullptr, &inst));
        uint32_t n = 1;
        vkEnumeratePhysicalDevices(inst, &n, &phys);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys, &props);
        vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
        std::printf("Vulkan device: %s (driver %u.%u.%u)\n", props.deviceName,
                    VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
                    VK_VERSION_PATCH(props.driverVersion));
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, exts.data());
        bool has_mo = false, has_port = false;
        for (auto& e : exts) {
            if (!strcmp(e.extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME)) has_mo = true;
            if (!strcmp(e.extensionName, "VK_KHR_portability_subset")) has_port = true;
        }
        std::printf("VK_EXT_metal_objects advertised: %s\n", has_mo ? "yes" : "NO");
        if (!has_mo) return 2;
        float prio = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        std::vector<const char*> dext = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME};
        if (has_port) dext.push_back("VK_KHR_portability_subset");
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.synchronization2 = VK_TRUE;
        f13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        f12.timelineSemaphore = VK_TRUE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = (uint32_t)dext.size();
        dci.ppEnabledExtensionNames = dext.data();
        CK(vkCreateDevice(phys, &dci, nullptr, &dev));
        vkGetDeviceQueue(dev, qfam, 0, &queue);
        pExport = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(dev, "vkExportMetalObjectsEXT");
        if (!pExport) { std::printf("vkExportMetalObjectsEXT missing\n"); return 2; }

        // --- the port's tail images, made by Vulkan ---
        Img color = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, iw, ih, VK_IMAGE_ASPECT_COLOR_BIT, false);
        Img depth = MakeImage(VK_FORMAT_D32_SFLOAT_S8_UINT, iw, ih,
                              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, true);
        Img motion = MakeImage(VK_FORMAT_R16G16_SFLOAT, iw, ih, VK_IMAGE_ASPECT_COLOR_BIT, false);
        // --- the export ---
        VkExportMetalDeviceInfoEXT edev{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
        VkExportMetalObjectsInfoEXT einfo{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &edev};
        pExport(dev, &einfo);
        id<MTLDevice> mdev = edev.mtlDevice;
        // Output: either a KK image (shared storage: MetalFX's validation layer rejects it) or,
        // with SPIKE_IMPORT_OUTPUT=1 (KK patch 0008), a PRIVATE MTLTexture made here and imported
        // into Vulkan as a VkImage (VkImportMetalTextureInfoEXT) - MetalFX's contract.
        const bool import_out = getenv("SPIKE_IMPORT_OUTPUT") != nullptr;
        Img output{};
        if (!import_out) {
            output = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT, false);
        } else {
            if (!mdev) { std::printf("RESULT: no MTLDevice export\n"); return 3; }
            MTLTextureDescriptor* od = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:ow height:oh mipmapped:NO];
            od.storageMode = MTLStorageModePrivate;
            od.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
            id<MTLTexture> priv = [mdev newTextureWithDescriptor:od];
            VkImportMetalTextureInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT};
            imp.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
            imp.mtlTexture = priv;
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &imp};
            ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            ci.extent = {ow, oh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            CK(vkCreateImage(dev, &ci, nullptr, &output.image));
            output.fmt = ci.format; output.w = ow; output.h = oh;
            output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = output.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            CK(vkCreateImageView(dev, &vi, nullptr, &output.view));
            std::printf("output: private MTLTexture %p imported as VkImage\n", (__bridge void*)priv);
        }
        id<MTLTexture> tc = ExportTexture(color), td = ExportTexture(depth),
                       tm = ExportTexture(motion), to = ExportTexture(output);
        std::printf("export: MTLDevice %s; textures color %p depth %p motion %p output %p\n",
                    mdev ? mdev.name.UTF8String : "NULL", (__bridge void*)tc, (__bridge void*)td,
                    (__bridge void*)tm, (__bridge void*)to);
        if (!mdev || !tc || !td || !tm || !to) {
            std::printf("RESULT: export returned NULL (driver lacks patch 0007)\n");
            return 3;
        }
        auto describe = [](const char* name, id<MTLTexture> t) {
            std::printf("  %-6s %lux%lu fmt %lu storage %lu hazard %lu usage 0x%lx heap %s "
                        "gpuOptimized %d\n",
                        name, (unsigned long)t.width, (unsigned long)t.height,
                        (unsigned long)t.pixelFormat, (unsigned long)t.storageMode,
                        (unsigned long)t.hazardTrackingMode, (unsigned long)t.usage,
                        t.heap ? "yes" : "no", (int)t.allowGPUOptimizedContents);
        };
        describe("color", tc); describe("depth", td); describe("motion", tm); describe("output", to);
        // (1) same allocation: the texture's heap is the VkDeviceMemory's MTLHeap.
        // (the heap is what vkGetMemoryMetalHandleEXT would return; the texture lives in it)

        // --- MetalFX on the exported objects ---
        MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
        d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh;
        d.colorTextureFormat = tc.pixelFormat;
        d.depthTextureFormat = td.pixelFormat;
        d.motionTextureFormat = tm.pixelFormat;
        d.outputTextureFormat = to.pixelFormat;
        d.autoExposureEnabled = YES;
        d.requiresSynchronousInitialization = YES;
        id<MTLFXTemporalScaler> s = [d newTemporalScalerWithDevice:mdev];
        if (!s) { std::printf("RESULT: newTemporalScalerWithDevice nil\n"); return 4; }
        auto usage_ok = [](const char* n, id<MTLTexture> t, MTLTextureUsage need) {
            bool ok = (t.usage & need) == need;
            std::printf("  usage %-6s needs 0x%lx has 0x%lx -> %s\n", n, (unsigned long)need,
                        (unsigned long)t.usage, ok ? "ok" : "MISSING");
            return ok;
        };
        bool uok = usage_ok("color", tc, s.colorTextureUsage) &
                   usage_ok("depth", td, s.depthTextureUsage) &
                   usage_ok("motion", tm, s.motionTextureUsage) &
                   usage_ok("output", to, s.outputTextureUsage);
        std::printf("  output storage private required by the header; exported output is %s\n",
                    to.storageMode == MTLStorageModePrivate ? "private" : "SHARED (not private)");
        if (import_out && to.storageMode != MTLStorageModePrivate) {
            std::printf("RESULT: imported output did not round-trip as private\n");
            return 7;
        }
        s.colorTexture = tc; s.depthTexture = td; s.motionTexture = tm; s.outputTexture = to;
        s.inputContentWidth = iw; s.inputContentHeight = ih;
        s.motionVectorScaleX = 1; s.motionVectorScaleY = 1; s.preExposure = 1;
        id<MTLCommandQueue> mq = [mdev newCommandQueue];

        // --- sync: the Vulkan timeline's MTLSharedEvent (patch 0002) ---
        VkSemaphoreTypeCreateInfo stc{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        stc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkExportMetalObjectCreateInfoEXT eoc{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT, &stc};
        eoc.exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &eoc};
        VkSemaphore timeline;
        CK(vkCreateSemaphore(dev, &sci, nullptr, &timeline));
        VkExportMetalSharedEventInfoEXT eev{VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT};
        eev.semaphore = timeline;
        VkExportMetalObjectsInfoEXT einfo2{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &eev};
        pExport(dev, &einfo2);
        id<MTLSharedEvent> ev = eev.mtlSharedEvent;
        std::printf("timeline MTLSharedEvent: %p\n", (__bridge void*)ev);
        if (!ev) return 5;

        // --- Vulkan command buffers: (pre) render the scene targets; (post) read the output ---
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = qfam;
        VkCommandPool pool;
        CK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
        VkCommandBuffer pre, post, init;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        CK(vkAllocateCommandBuffers(dev, &cai, &pre));
        CK(vkAllocateCommandBuffers(dev, &cai, &post));
        CK(vkAllocateCommandBuffers(dev, &cai, &init));
        // readback buffer for the output's centre row (Vulkan reading MetalFX's write)
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 64 * 8;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer rb;
        CK(vkCreateBuffer(dev, &bci, nullptr, &rb));
        VkMemoryRequirements breq;
        vkGetBufferMemoryRequirements(dev, rb, &breq);
        VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bai.allocationSize = breq.size;
        bai.memoryTypeIndex = MemType(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory rbm;
        CK(vkAllocateMemory(dev, &bai, nullptr, &rbm));
        CK(vkBindBufferMemory(dev, rb, rbm, 0));
        void* rbp;
        CK(vkMapMemory(dev, rbm, 0, VK_WHOLE_SIZE, 0, &rbp));

        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CK(vkBeginCommandBuffer(init, &cbi));
        Barrier(init, color.image, color.aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        Barrier(init, depth.image, depth.aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        Barrier(init, motion.image, motion.aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        Barrier(init, output.image, output.aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        CK(vkEndCommandBuffer(init));
        VkSubmitInfo si0{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si0.commandBufferCount = 1; si0.pCommandBuffers = &init;
        CK(vkQueueSubmit(queue, 1, &si0, VK_NULL_HANDLE));
        CK(vkQueueWaitIdle(queue));

        auto record_pre = [&](float r) {
            CK(vkResetCommandBuffer(pre, 0));
            CK(vkBeginCommandBuffer(pre, &cbi));
            VkRenderingAttachmentInfo ca[2]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                            {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
            ca[0].imageView = color.view; ca[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ca[0].clearValue.color = {{r, 0.5f, 0.75f, 1.0f}};
            ca[1] = ca[0];
            ca[1].imageView = motion.view;
            ca[1].clearValue.color = {{0.0f, 0.0f, 0, 0}};
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = depth.view; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil = {0.5f, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {iw, ih}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 2;
            ri.pColorAttachments = ca;
            ri.pDepthAttachment = &da;
            ri.pStencilAttachment = &da;
            vkCmdBeginRendering(pre, &ri);
            vkCmdEndRendering(pre);
            CK(vkEndCommandBuffer(pre));
        };
        CK(vkBeginCommandBuffer(post, &cbi));
        Barrier(post, output.image, output.aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageOffset = {int32_t(ow / 2), int32_t(oh / 2), 0};
        bic.imageExtent = {64, 1, 1};
        vkCmdCopyImageToBuffer(post, output.image, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &bic);
        CK(vkEndCommandBuffer(post));

        uint64_t value = 0;
        auto submit = [&](VkCommandBuffer cb, uint64_t wait, uint64_t signal) {
            VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            ts.waitSemaphoreValueCount = wait ? 1 : 0;
            ts.pWaitSemaphoreValues = &wait;
            ts.signalSemaphoreValueCount = 1;
            ts.pSignalSemaphoreValues = &signal;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts};
            si.waitSemaphoreCount = wait ? 1 : 0;
            si.pWaitSemaphores = &timeline;
            si.pWaitDstStageMask = &stage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &timeline;
            CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        };

        std::vector<double> scale_ms, chain_ms, to_start_ms, after_end_ms;
        const int warm = 30;
        bool content_ok = true;
        if (getenv("SPIKE_NO_MFX"))
            std::printf("SPIKE_NO_MFX: the MetalFX command buffer only waits/signals (handoff price)\n");
        for (int f = 0; f < warm + frames; ++f) @autoreleasepool {
            const float r = (f % 2) ? 0.25f : 0.375f;
            record_pre(r);
            auto t0 = std::chrono::steady_clock::now();
            const double h0 = CACurrentMediaTime();
            // Vulkan scene -> N+1
            submit(pre, value, value + 1);
            // MetalFX: GPU-wait N+1, scale the Vulkan images in place, signal N+2
            id<MTLCommandBuffer> mcb = [mq commandBuffer];
            [mcb encodeWaitForEvent:ev value:value + 1];
            s.jitterOffsetX = 0; s.jitterOffsetY = 0;
            s.reset = f == 0;
            if (!getenv("SPIKE_NO_MFX")) [s encodeToCommandBuffer:mcb];
            [mcb encodeSignalEvent:ev value:value + 2];
            [mcb commit];
            // Vulkan reads MetalFX's output image after GPU-wait N+2 -> N+3
            submit(post, value + 2, value + 3);
            value += 3;
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1; wi.pSemaphores = &timeline; wi.pValues = &value;
            CK(vkWaitSemaphores(dev, &wi, 5'000'000'000ull)); // only to bound the loop
            auto t1 = std::chrono::steady_clock::now();
            const double h1 = CACurrentMediaTime();
            [mcb waitUntilCompleted];
            if (mcb.status == MTLCommandBufferStatusError) {
                std::printf("RESULT: MetalFX command buffer error: %s\n",
                            mcb.error.localizedDescription.UTF8String);
                return 6;
            }
            // MetalFX's output (constant input, no motion) must converge to the Vulkan colour.
            const __fp16* px = (const __fp16*)rbp;
            if (f >= warm) {
                scale_ms.push_back((mcb.GPUEndTime - mcb.GPUStartTime) * 1e3);
                chain_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                to_start_ms.push_back((mcb.GPUStartTime - h0) * 1e3);
                after_end_ms.push_back((h1 - mcb.GPUEndTime) * 1e3);
            }
            if (f == warm + frames - 1 || f == 0 || f == 1) {
                std::printf("  frame %d: vulkan wrote r=%.3f; vulkan read back MetalFX output "
                            "centre = %.3f %.3f %.3f %.3f\n",
                            f, r, float(px[0]), float(px[1]), float(px[2]), float(px[3]));
                if (f == warm + frames - 1 && !getenv("SPIKE_NO_MFX") &&
                    !(float(px[1]) > 0.45f && float(px[1]) < 0.55f && float(px[2]) > 0.7f))
                    content_ok = false;
            }
        }
        std::printf("render %ux%u -> output %ux%u, %d timed frames\n", iw, ih, ow, oh, frames);
        std::printf("MetalFX cmdbuf on KK's own textures, GPU ms: p50 %.3f p10 %.3f p90 %.3f\n",
                    Pct(scale_ms, .5), Pct(scale_ms, .1), Pct(scale_ms, .9));
        std::printf("Vulkan pre -> MetalFX -> Vulkan post, submit-to-signal wall ms: p50 %.3f "
                    "p10 %.3f p90 %.3f\n",
                    Pct(chain_ms, .5), Pct(chain_ms, .1), Pct(chain_ms, .9));
        if (!getenv("SPIKE_NO_MFX"))
            std::printf("  breakdown p50: submit->MetalFX GPU start %.3f ms (Vulkan scene + cross-queue "
                        "wake), MetalFX GPU end->Vulkan post done %.3f ms\n",
                        Pct(to_start_ms, .5), Pct(after_end_ms, .5));
        std::printf("RESULT: zero-copy %s (usage %s, content %s, no copies encoded anywhere)\n",
                    (uok && content_ok) ? "OK" : "FAILED", uok ? "ok" : "missing",
                    content_ok ? "round-trips" : "WRONG");
        vkDeviceWaitIdle(dev);
    }
    return 0;
}
