// mtl-split-bench — does MTL4CommandAllocator.allocatedSize scale with the number of
// MTL4CommandBuffers encoded between two resets of the same allocator?
//
// Models KosmicKrisp's gfx slot (kk_cmd_buffer.c): one MTL4CommandAllocator per encoder
// state, reset at vkBeginCommandBuffer, then N MTL4CommandBuffers begun on it sequentially
// (mtl_new_command_buffer + mtl_begin_command_buffer), each with E render encoders carrying
// D draws, ended (endCommandBuffer), committed, then allocatedSize read.
//
// Per-draw work mirrors what the driver emits per vkCmdDraw*: pipeline state, depth/stencil
// state, cull mode, front face, viewport, scissor, argument table at vertex|fragment (the
// driver rebinds its one argument table after setAddress), drawIndexedPrimitives. Encoders end
// with the kk_stop_encoder producer barrier.
//
// Modes:
//   sweep  --n LIST --total-draws T | --draws-per-cb D  [--enc-per-cb E] [--reps R]
//   Each measurement uses a FRESH allocator (reset once, then N CBs) and, separately, the
//   same allocator re-used across the whole sweep (high-water / ratchet behaviour).
//
// Build: see run.sh

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vector>
#include <string>
#include <sstream>

static void die(NSString *m) { fprintf(stderr, "error: %s\n", m.UTF8String); exit(1); }

static NSString *const kSrc = @""
"#include <metal_stdlib>\n using namespace metal;\n"
"struct V { float4 p [[position]]; };\n"
"struct Cb { float4 x; };\n"
"vertex V vs(uint vid [[vertex_id]], device const float4 *pos [[buffer(0)]]) { V o; o.p = pos[vid & 3]; return o; }\n"
"fragment float4 fs(device const Cb *c [[buffer(1)]]) { return c->x; }\n"
"kernel void ks(device float4 *b [[buffer(0)]], uint t [[thread_position_in_grid]]) { if (t == 0x7fffffff) b[0] = 1; }\n";

struct Ctx {
    id<MTLDevice> dev; id<MTL4CommandQueue> q; id<MTLRenderPipelineState> pso;
    id<MTLDepthStencilState> dss; id<MTL4ArgumentTable> tab; MTL4RenderPassDescriptor *pass;
    id<MTLBuffer> ib; id<MTLSharedEvent> ev; uint64_t evv = 0; bool commit = true;
    bool compute = false; id<MTLComputePipelineState> cps;
};

// One "recording": reset alloc, N command buffers, each with encPerCb render encoders and the
// given draws per command buffer (spread across its encoders). Returns allocatedSize after the
// last endCommandBuffer (and after GPU completion when commit).
struct Rec { uint64_t afterReset, afterEncode, afterGpu; };

static Rec record(Ctx &c, id<MTL4CommandAllocator> alloc, int N, const std::vector<int> &drawsPerCb,
                  int encPerCb) {
    Rec r{};
    [alloc reset];
    r.afterReset = alloc.allocatedSize;
    std::vector<id<MTL4CommandBuffer>> cbs;
    cbs.reserve(N);
    MTLViewport vp = {0, 0, 256, 256, 0, 1};
    MTLScissorRect sc = {0, 0, 256, 256};
    for (int i = 0; i < N; i++) {
        @autoreleasepool {
            id<MTL4CommandBuffer> cb = [c.dev newCommandBuffer];
            [cb beginCommandBufferWithAllocator:alloc];
            int D = drawsPerCb[i];
            for (int e = 0; e < encPerCb; e++) {
                int d0 = (int)((long)D * e / encPerCb), d1 = (int)((long)D * (e + 1) / encPerCb);
                if (c.compute) {   // pre/post-gfx shape: compute encoder, one dispatch per "draw"
                    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
                    [ce setArgumentTable:c.tab];
                    for (int d = d0; d < d1; d++) {
                        [ce setComputePipelineState:c.cps];
                        [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                    }
                    [ce barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
                         visibilityOptions:MTL4VisibilityOptionNone];
                    [ce endEncoding];
                    continue;
                }
                id<MTL4RenderCommandEncoder> enc =
                    [cb renderCommandEncoderWithDescriptor:c.pass options:MTL4RenderEncoderOptionNone];
                if (!enc) die(@"render encoder");
                for (int d = d0; d < d1; d++) {
                    [enc setRenderPipelineState:c.pso];
                    [enc setDepthStencilState:c.dss];
                    [enc setCullMode:(d & 1) ? MTLCullModeBack : MTLCullModeNone];
                    [enc setFrontFacingWinding:MTLWindingCounterClockwise];
                    [enc setViewport:vp];
                    [enc setScissorRect:sc];
                    [c.tab setAddress:0 atIndex:2];   // driver: mtl_set_address then rebind
                    [enc setArgumentTable:c.tab atStages:MTLRenderStageVertex | MTLRenderStageFragment];
                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6
                                     indexType:MTLIndexTypeUInt16
                                   indexBuffer:c.ib.gpuAddress indexBufferLength:12
                                 instanceCount:1 baseVertex:0 baseInstance:0];
                }
                [enc barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll
                      visibilityOptions:MTL4VisibilityOptionNone];
                [enc endEncoding];
            }
            [cb endCommandBuffer];
            cbs.push_back(cb);
        }
    }
    r.afterEncode = alloc.allocatedSize;
    if (c.commit) {
        [c.q commit:cbs.data() count:(NSUInteger)cbs.size()];
        [c.q signalEvent:c.ev value:++c.evv];
        if (![c.ev waitUntilSignaledValue:c.evv timeoutMS:600000]) die(@"gpu timeout");
    }
    r.afterGpu = alloc.allocatedSize;
    return r;
}

static std::vector<int> parseList(const char *s) {
    std::vector<int> v; std::stringstream ss(s); std::string t;
    while (std::getline(ss, t, ',')) v.push_back(atoi(t.c_str()));
    return v;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        std::vector<int> Ns = {1, 2, 4, 8, 16, 32, 51, 102, 256};
        long totalDraws = -1; int dpc = -1, epc = 1, reps = 3;
        const char *label = "sweep";
        Ctx c;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--n") Ns = parseList(argv[++i]);
            else if (a == "--total-draws") totalDraws = atol(argv[++i]);
            else if (a == "--draws-per-cb") dpc = atoi(argv[++i]);
            else if (a == "--enc-per-cb") epc = atoi(argv[++i]);
            else if (a == "--reps") reps = atoi(argv[++i]);
            else if (a == "--label") label = argv[++i];
            else if (a == "--no-commit") c.commit = false;
            else if (a == "--compute") c.compute = true;
            else die([NSString stringWithFormat:@"unknown arg %s", argv[i]]);
        }
        if ((totalDraws < 0) == (dpc < 0)) die(@"need exactly one of --total-draws / --draws-per-cb");

        c.dev = MTLCreateSystemDefaultDevice();
        if (!c.dev) die(@"no device");
        c.q = [c.dev newMTL4CommandQueue];
        if (!c.q) die(@"no MTL4 queue");
        NSError *err = nil;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:256 height:256 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [c.dev newTextureWithDescriptor:td];
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                       width:256 height:256 mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> dt = [c.dev newTextureWithDescriptor:dd];
        id<MTLBuffer> vb = [c.dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        c.ib = [c.dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        memset(vb.contents, 0, 4096);
        uint16_t *ix = (uint16_t *)c.ib.contents; for (int i = 0; i < 6; i++) ix[i] = (uint16_t)(i & 3);
        MTLResidencySetDescriptor *rsd = [MTLResidencySetDescriptor new];
        id<MTLResidencySet> rs = [c.dev newResidencySetWithDescriptor:rsd error:&err];
        [rs addAllocation:rt]; [rs addAllocation:dt]; [rs addAllocation:vb]; [rs addAllocation:c.ib];
        [rs commit]; [c.q addResidencySet:rs];

        id<MTLLibrary> lib = [c.dev newLibraryWithSource:kSrc options:nil error:&err];
        if (!lib) die([NSString stringWithFormat:@"lib %@", err]);
        MTL4LibraryFunctionDescriptor *vf = [MTL4LibraryFunctionDescriptor new]; vf.library = lib; vf.name = @"vs";
        MTL4LibraryFunctionDescriptor *ff = [MTL4LibraryFunctionDescriptor new]; ff.library = lib; ff.name = @"fs";
        MTL4RenderPipelineDescriptor *pd = [MTL4RenderPipelineDescriptor new];
        pd.vertexFunctionDescriptor = vf; pd.fragmentFunctionDescriptor = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pd.rasterSampleCount = 1;
        id<MTL4Compiler> comp = [c.dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
        c.pso = [comp newRenderPipelineStateWithDescriptor:pd compilerTaskOptions:nil error:&err];
        if (!c.pso) die([NSString stringWithFormat:@"pso %@", err]);
        {
            MTL4LibraryFunctionDescriptor *kf = [MTL4LibraryFunctionDescriptor new]; kf.library = lib; kf.name = @"ks";
            MTL4ComputePipelineDescriptor *cpd = [MTL4ComputePipelineDescriptor new];
            cpd.computeFunctionDescriptor = kf;
            c.cps = [comp newComputePipelineStateWithDescriptor:cpd compilerTaskOptions:nil error:&err];
            if (!c.cps) die([NSString stringWithFormat:@"cps %@", err]);
        }
        MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
        dsd.depthCompareFunction = MTLCompareFunctionAlways; dsd.depthWriteEnabled = NO;
        c.dss = [c.dev newDepthStencilStateWithDescriptor:dsd];
        MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new];
        atd.maxBufferBindCount = 3; atd.initializeBindings = YES;
        c.tab = [c.dev newArgumentTableWithDescriptor:atd error:&err];
        if (!c.tab) die(@"arg table");
        [c.tab setAddress:vb.gpuAddress atIndex:0];
        [c.tab setAddress:vb.gpuAddress atIndex:1];
        c.pass = [MTL4RenderPassDescriptor new];
        c.pass.colorAttachments[0].texture = rt;
        c.pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        c.pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        c.ev = [c.dev newSharedEvent];

        auto drawsFor = [&](int N) {
            std::vector<int> v(N);
            if (dpc >= 0) for (int i = 0; i < N; i++) v[i] = dpc;
            else for (int i = 0; i < N; i++) v[i] = (int)(totalDraws * (i + 1) / N - totalDraws * i / N);
            return v;
        };

        // Warm-up so first-use driver allocations don't land on the first row.
        { id<MTL4CommandAllocator> w = [c.dev newCommandAllocator]; record(c, w, 4, std::vector<int>(4, 50), 1); }

        printf("# %s  device=%s  enc=%s enc_per_cb=%d  %s  commit=%d\n", label, c.dev.name.UTF8String, c.compute ? "compute" : "render", epc,
               dpc >= 0 ? [NSString stringWithFormat:@"draws_per_cb=%d", dpc].UTF8String
                        : [NSString stringWithFormat:@"total_draws=%ld", totalDraws].UTF8String, c.commit);
        printf("ROWHDR label N encoders draws rep fresh_reset_B fresh_enc_B fresh_gpu_B reused_enc_B reused_gpu_B\n");
        id<MTL4CommandAllocator> reused = [c.dev newCommandAllocator];
        for (int rep = 0; rep < reps; rep++) {
            for (int N : Ns) {
                std::vector<int> dv = drawsFor(N);
                long draws = 0; for (int d : dv) draws += d;
                id<MTL4CommandAllocator> fresh = [c.dev newCommandAllocator];
                Rec f = record(c, fresh, N, dv, epc);
                Rec u = record(c, reused, N, dv, epc);
                printf("ROW %s %d %d %ld %d %llu %llu %llu %llu %llu\n", label, N, N * epc, draws, rep,
                       f.afterReset, f.afterEncode, f.afterGpu, u.afterEncode, u.afterGpu);
                fflush(stdout);
            }
        }
    }
    return 0;
}
