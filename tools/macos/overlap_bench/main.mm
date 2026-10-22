// kk-overlap-bench: can a Metal 4 render encoder's VERTEX phase run under the previous encoder's
// FRAGMENT phase on this GPU, and what does each synchronisation shape cost?
//
// One frame = N render encoders, each in its own MTL4CommandBuffer, all committed in one array
// (exactly KosmicKrisp's structure: kk_cmd_buffer.c cs_start_render -> one MTL4CommandBuffer per
// encoder, kk_queue.c one commit array per VkCommandBuffer). Encoder p renders into pingpong[p&1]
// and samples pingpong[(p&1)^1] in its FRAGMENT shader (a true RAW chain fragment->fragment), and
// its VERTEX shader does VTX_ITERS of ALU per vertex over VERTS vertices (no resource read), so the
// vertex phase is legally independent of the previous pass.
//
// Sync modes (between consecutive encoders):
//   full     producer barrier at encoder end: barrierAfterStages:All beforeQueueStages:All
//            (KK legacy, kk_stop_encoder)
//   lite     producer barrier, vertex excluded: after All before Fragment|Dispatch|Blit
//            (KK BB_KK_BARRIER=lite/nodep shape)
//   consumer consumer barrier at encoder start: barrierAfterQueueStages:All beforeStages:Fragment
//            (KK BB_KK_BARRIER=vk shape when the Vulkan dst stage is fragment-only)
//   fence    updateFence after Fragment at end, waitForFence before Fragment at start (the
//            MTLFence 'real edge' shape)
//   none     nothing (WRONG results; ceiling only)
//
// Build: clang++ -std=c++17 -fobjc-arc -O2 main.mm -o kk-overlap-bench -framework Metal -framework Foundation
// Run:   ./kk-overlap-bench MODE [--encoders 300] [--size 1720] [--verts 30000] [--vtx-iters 64]
//                              [--frag-iters 4] [--frames 200] [--warmup 40]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <string>

static int W = 1720, H = 720;
static const int ALLOC_SLOTS = 4;
static const int MAX_IN_FLIGHT = 2;
static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void die(NSString *m) { fprintf(stderr, "error: %s\n", m.UTF8String); exit(1); }

static NSString *const kSrc = @""
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float2 uv; };\n"
"struct P { uint vtx_iters; uint frag_iters; };\n"
"vertex VOut bb_vs(uint vid [[vertex_id]], constant P& p [[buffer(0)]]) {\n"
"    float2 q[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
"    VOut o;\n"
"    float a = float(vid) * 1e-3;\n"
"    for (uint i = 0; i < p.vtx_iters; i++) a = fma(a, 1.0001, 0.37) - floor(a);\n"
"    if (vid < 3) { o.pos = float4(q[vid], 0.0, 1.0); o.uv = q[vid] * float2(0.5,-0.5) + 0.5; }\n"
"    else { o.pos = float4(2.0 + a * 1e-9, 2.0, 0.0, 1.0); o.uv = float2(0.0); }\n"   // culled
"    return o;\n"
"}\n"
"fragment float4 bb_fs(VOut in [[stage_in]], texture2d<float> src [[texture(0)]], constant P& p [[buffer(0)]]) {\n"
"    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);\n"
"    float4 c = src.sample(s, in.uv);\n"
"    for (uint i = 0; i < p.frag_iters; i++) c = fma(c, 0.999, 0.0001);\n"
"    return c;\n"
"}\n";

int main(int argc, char **argv) {
    @autoreleasepool {
        std::string mode = "full";
        int nFrames = 200, nWarmup = 40, nEnc = 300, verts = 30000;
        uint32_t vtxIters = 64, fragIters = 4;
        double loopSeconds = 0;
        bool oneCb = false;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "full" || a == "lite" || a == "consumer" || a == "fence" || a == "none") mode = a;
            else if (a == "--frames" && i + 1 < argc) nFrames = atoi(argv[++i]);
            else if (a == "--warmup" && i + 1 < argc) nWarmup = atoi(argv[++i]);
            else if (a == "--encoders" && i + 1 < argc) nEnc = atoi(argv[++i]);
            else if (a == "--size" && i + 1 < argc) { W = atoi(argv[++i]); H = W * 720 / 1720; }
            else if (a == "--verts" && i + 1 < argc) verts = atoi(argv[++i]);
            else if (a == "--vtx-iters" && i + 1 < argc) vtxIters = atoi(argv[++i]);
            else if (a == "--frag-iters" && i + 1 < argc) fragIters = atoi(argv[++i]);
            else if (a == "--seconds" && i + 1 < argc) loopSeconds = atof(argv[++i]);
            else if (a == "--one-cb") oneCb = true;
            else die([NSString stringWithFormat:@"unknown arg %s", argv[i]]);
        }
        verts = std::max(3, verts / 3 * 3);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTL4CommandQueue> queue = [dev newMTL4CommandQueue];
        if (!queue) die(@"no MTL4CommandQueue");

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        td.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        id<MTLTexture> tex[2] = { [dev newTextureWithDescriptor:td], [dev newTextureWithDescriptor:td] };
        id<MTLBuffer> params = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
        uint32_t *pp = (uint32_t *)params.contents; pp[0] = vtxIters; pp[1] = fragIters;

        MTLResidencySetDescriptor *rsd = [MTLResidencySetDescriptor new];
        NSError *err = nil;
        id<MTLResidencySet> rs = [dev newResidencySetWithDescriptor:rsd error:&err];
        [rs addAllocation:tex[0]]; [rs addAllocation:tex[1]]; [rs addAllocation:params];
        [rs commit]; [queue addResidencySet:rs];

        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
        if (!lib) die([NSString stringWithFormat:@"%@", err]);
        MTL4LibraryFunctionDescriptor *vfn = [MTL4LibraryFunctionDescriptor new]; vfn.library = lib; vfn.name = @"bb_vs";
        MTL4LibraryFunctionDescriptor *ffn = [MTL4LibraryFunctionDescriptor new]; ffn.library = lib; ffn.name = @"bb_fs";
        MTL4RenderPipelineDescriptor *pd = [MTL4RenderPipelineDescriptor new];
        pd.vertexFunctionDescriptor = vfn; pd.fragmentFunctionDescriptor = ffn;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        id<MTL4Compiler> compiler = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
        id<MTLRenderPipelineState> pso = [compiler newRenderPipelineStateWithDescriptor:pd compilerTaskOptions:nil error:&err];
        if (!pso) die([NSString stringWithFormat:@"%@", err]);

        MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new];
        atd.maxTextureBindCount = 1; atd.maxBufferBindCount = 1; atd.initializeBindings = YES;
        id<MTL4ArgumentTable> tab[2];
        for (int i = 0; i < 2; i++) {
            tab[i] = [dev newArgumentTableWithDescriptor:atd error:&err];
            [tab[i] setTexture:tex[i].gpuResourceID atIndex:0];
            [tab[i] setAddress:params.gpuAddress atIndex:0];
        }
        MTL4RenderPassDescriptor *pass[2];
        for (int i = 0; i < 2; i++) {
            pass[i] = [MTL4RenderPassDescriptor new];
            pass[i].colorAttachments[0].texture = tex[i];
            pass[i].colorAttachments[0].loadAction = MTLLoadActionLoad;   // like the port's resumes
            pass[i].colorAttachments[0].storeAction = MTLStoreActionStore;
            pass[i].renderTargetWidth = W; pass[i].renderTargetHeight = H;
        }
        id<MTL4CommandAllocator> allocSlot[ALLOC_SLOTS];
        for (int i = 0; i < ALLOC_SLOTS; i++) allocSlot[i] = [dev newCommandAllocator];
        std::vector<id<MTL4CommandBuffer>> cbs(nEnc);
        for (int i = 0; i < nEnc; i++) cbs[i] = [dev newCommandBuffer];
        std::vector<id<MTLFence>> fences(nEnc + 1);
        for (auto &f : fences) f = [dev newFence];
        id<MTLSharedEvent> event = [dev newSharedEvent];
        uint64_t eventValue = 0;
        MTLViewport vp = { 0, 0, (double)W, (double)H, 0.0, 1.0 };

        auto encodePass = [&](id<MTL4CommandBuffer> cb, int p) {
            int even = (p & 1) == 0;
            id<MTL4RenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass[even ? 1 : 0]
                                                                                options:MTL4RenderEncoderOptionNone];
            if (mode == "consumer")
                [enc barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment
                           visibilityOptions:MTL4VisibilityOptionNone];
            if (mode == "fence" && p > 0)
                [enc waitForFence:fences[p - 1] beforeEncoderStages:MTLStageFragment];
            [enc setRenderPipelineState:pso];
            [enc setArgumentTable:tab[even ? 0 : 1] atStages:MTLRenderStageVertex | MTLRenderStageFragment];
            [enc setViewport:vp];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)verts];
            if (mode == "full")
                [enc barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionNone];
            else if (mode == "lite")
                [enc barrierAfterStages:MTLStageAll
                      beforeQueueStages:MTLStageFragment | MTLStageDispatch | MTLStageBlit
                      visibilityOptions:MTL4VisibilityOptionNone];
            if (mode == "fence")
                [enc updateFence:fences[p] afterEncoderStages:MTLStageFragment];
            [enc endEncoding];
        };
        auto renderFrame = [&](int f) {
            id<MTL4CommandAllocator> alloc = allocSlot[f % ALLOC_SLOTS];
            [alloc reset];
            if (oneCb) {
                [cbs[0] beginCommandBufferWithAllocator:alloc];
                for (int p = 0; p < nEnc; p++) encodePass(cbs[0], p);
                [cbs[0] endCommandBuffer];
                [queue commit:cbs.data() count:1];
            } else {
                for (int p = 0; p < nEnc; p++) {
                    [cbs[p] beginCommandBufferWithAllocator:alloc];
                    encodePass(cbs[p], p);
                    [cbs[p] endCommandBuffer];
                }
                [queue commit:cbs.data() count:(NSUInteger)nEnc];
            }
            [queue signalEvent:event value:++eventValue];
        };
        auto waitFor = [&](uint64_t v) { if (![event waitUntilSignaledValue:v timeoutMS:600000]) die(@"timeout"); };

        for (int f = 0; f < nWarmup; f++) { renderFrame(f); if (f >= MAX_IN_FLIGHT) waitFor(eventValue - MAX_IN_FLIGHT + 1); }
        waitFor(eventValue);
        std::vector<uint64_t> t; t.reserve(nFrames);
        for (int f = 0; f < nFrames; f++) {
            t.push_back(now_ns());
            renderFrame(f);
            if (f >= MAX_IN_FLIGHT) waitFor(eventValue - MAX_IN_FLIGHT + 1);
        }
        waitFor(eventValue);
        std::vector<double> per;
        for (size_t i = 1; i < t.size(); i++) per.push_back((t[i] - t[i - 1]) / 1e6);
        std::sort(per.begin(), per.end());
        double p50 = per[per.size() / 2];
        printf("RESULT mode=%s%s encoders=%d size=%dx%d verts=%d vtx_iters=%u frag_iters=%u p50_ms=%.3f us_per_enc=%.2f\n",
               mode.c_str(), oneCb ? "+onecb" : "", nEnc, W, H, verts, vtxIters, fragIters, p50, p50 * 1000.0 / nEnc);
        if (loopSeconds > 0) {
            printf("READY pid=%d\n", getpid()); fflush(stdout);
            uint64_t t0 = now_ns(); uint64_t n = 0;
            while ((now_ns() - t0) / 1e9 < loopSeconds) {
                renderFrame((int)n); n++;
                if (n > (uint64_t)MAX_IN_FLIGHT) waitFor(eventValue - MAX_IN_FLIGHT + 1);
            }
            waitFor(eventValue);
        }
    }
    return 0;
}
