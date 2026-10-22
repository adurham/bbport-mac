// binarchive-spike: what a Metal 4 archive (MTL4Archive) buys KosmicKrisp-shaped pipelines.
//
// Mirrors KosmicKrisp's bridge exactly (src/kosmickrisp/bridge/mtl_compiler.m): one MTL4Compiler,
// MSL source -> MTL4Compiler newLibraryWithDescriptor (MSL 4.1, fast math), an
// MTL4LibraryFunctionDescriptor per stage, an MTL4RenderPipelineDescriptor with the colour formats
// and topology class, -> MTL4Compiler newRenderPipelineStateWithDescriptor. The shaders are real
// KosmicKrisp MSL out of Mesa's disk cache (see extract_msl.py), one vertex + one fragment each.
//
// usage: spike MODE MSLDIR SALT [ARCHIVE...] [-o OUT] [-lib DIR]
//   MODE  plain      compile + create, no archive                     (cold/warm baseline)
//         capture    as plain, with a pipeline data set serializer attached; write OUT
//         lookup     as plain, with ARCHIVE(s) in MTL4CompilerTaskOptions.lookupArchives
//         archive    libraries from source, PSO from [MTL4Archive newRenderPipelineStateWithDescriptor]
//         libonly    as plain, but the library comes from a precompiled .metallib in -lib DIR
//         liblookup  libonly + lookupArchives
//         libarchive libonly + PSO from the archive
//   SALT  string prepended to every source as a comment, so a fresh salt is a source Metal has
//         never seen (its compile cache is keyed on the source); the same salt again is a hit.
// Prints one CSV line per pipeline: idx,lib_ms,pso_ms,ok and a SUMMARY line.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <time.h>

static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

typedef struct { int slot; MTLPixelFormat fmt; } Att;

static NSArray<NSString *> *manifest(NSString *dir) {
   NSString *m = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"manifest.txt"]
                                           encoding:NSUTF8StringEncoding error:nil];
   NSMutableArray *a = [NSMutableArray array];
   for (NSString *l in [m componentsSeparatedByString:@"\n"])
      if (l.length) [a addObject:l];
   return a;
}

// manifest line: "07 0:float4 1:float4" -> colour attachments. Port-shaped formats: slot 0 BGRA8
// (the swapchain/UI shape) on odd pipelines, RGBA16F elsewhere (the G-buffer/HDR shape).
static int attachments(NSString *line, int idx, Att *out) {
   NSArray *f = [line componentsSeparatedByString:@" "];
   int n = 0;
   for (NSUInteger i = 1; i < f.count; i++) {
      NSArray *st = [f[i] componentsSeparatedByString:@":"];
      int slot = [st[0] intValue];
      BOOL three = [st[1] hasSuffix:@"3"];
      // A float3 output cannot feed a 4-component format; RG11B10F is the port's 3-channel shape.
      MTLPixelFormat fmt = three ? MTLPixelFormatRG11B10Float : MTLPixelFormatRGBA16Float;
      if (slot == 0 && (idx & 1) && !three) fmt = MTLPixelFormatBGRA8Unorm;
      out[n++] = (Att){slot, fmt};
   }
   return n;
}

static id<MTLLibrary> lib_from_source(id<MTL4Compiler> comp, NSString *path, NSString *salt) {
   NSString *src = [NSString stringWithContentsOfFile:path encoding:NSASCIIStringEncoding error:nil];
   if (salt.length && ![salt isEqual:@"-"])
      src = [NSString stringWithFormat:@"// salt %@\n%@", salt, src];
   MTLCompileOptions *opts = [MTLCompileOptions new];
   opts.languageVersion = MTLLanguageVersion4_1;           // kk_physical_device.c: macOS >= 27
   opts.mathMode = MTLMathModeFast;
   opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsFast;
   MTL4LibraryDescriptor *d = [MTL4LibraryDescriptor new];
   d.source = src;
   d.options = opts;
   if (getenv("SPIKE_NAME_LIBS")) d.name = [[path lastPathComponent] stringByAppendingString:salt];
   NSError *e = nil;
   id<MTLLibrary> l = [comp newLibraryWithDescriptor:d error:&e];
   if (!l) fprintf(stderr, "library %s: %s\n", path.UTF8String, e.localizedDescription.UTF8String);
   return l;
}

static id<MTLLibrary> lib_from_metallib(id<MTLDevice> dev, NSString *path) {
   NSError *e = nil;
   id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:&e];
   if (!l) fprintf(stderr, "metallib %s: %s\n", path.UTF8String, e.localizedDescription.UTF8String);
   return l;
}

int main(int argc, const char **argv) {
   @autoreleasepool {
      if (argc < 4) { fprintf(stderr, "usage: spike MODE MSLDIR SALT [ARCHIVE...] [-o OUT] [-lib DIR]\n"); return 2; }
      NSString *mode = @(argv[1]), *dir = @(argv[2]), *salt = @(argv[3]);
      NSString *outp = nil, *libdir = nil;
      NSMutableArray<NSString *> *archives = [NSMutableArray array];
      for (int i = 4; i < argc; i++) {
         if (!strcmp(argv[i], "-o") && i + 1 < argc) outp = @(argv[++i]);
         else if (!strcmp(argv[i], "-lib") && i + 1 < argc) libdir = @(argv[++i]);
         else [archives addObject:@(argv[i])];
      }
      BOOL capture = [mode hasSuffix:@"capture"];
      BOOL from_lib = [mode hasPrefix:@"lib"];
      BOOL use_lookup = [mode isEqual:@"lookup"] || [mode isEqual:@"liblookup"];
      BOOL use_archive = [mode isEqual:@"archive"] || [mode isEqual:@"libarchive"];

      double t0 = now_ms();
      id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
      double t_dev = now_ms() - t0;

      id<MTL4PipelineDataSetSerializer> ser = nil;
      MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new];
      if (capture) {
         MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new];
         // CaptureBinaries alone: with CaptureDescriptors|CaptureBinaries (what the header suggests for
         // "both") serializeAsArchiveAndFlushToURL returns NO with a nil error on macOS 27.2.
         sd.configuration = getenv("SPIKE_BOTH") ? (MTL4PipelineDataSetSerializerConfigurationCaptureDescriptors |
                                                    MTL4PipelineDataSetSerializerConfigurationCaptureBinaries)
                                                 : MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
         ser = [dev newPipelineDataSetSerializerWithDescriptor:sd];
         cd.pipelineDataSetSerializer = ser;
      }
      NSError *err = nil;
      t0 = now_ms();
      id<MTL4Compiler> comp = [dev newCompilerWithDescriptor:cd error:&err];
      double t_comp = now_ms() - t0;
      if (!comp) { fprintf(stderr, "compiler: %s\n", err.localizedDescription.UTF8String); return 1; }

      NSMutableArray<id<MTL4Archive>> *arcs = [NSMutableArray array];
      double t_arc = 0;
      for (NSString *p in archives) {
         t0 = now_ms();
         id<MTL4Archive> a = [dev newArchiveWithURL:[NSURL fileURLWithPath:p] error:&err];
         t_arc += now_ms() - t0;
         if (!a) { fprintf(stderr, "archive %s: %s\n", p.UTF8String, err.localizedDescription.UTF8String); return 1; }
         [arcs addObject:a];
      }
      MTL4CompilerTaskOptions *topts = nil;
      if (use_lookup) { topts = [MTL4CompilerTaskOptions new]; topts.lookupArchives = arcs; }

      NSArray *man = manifest(dir);
      double sum_lib = 0, sum_pso = 0; int ok = 0, n = 0;
      printf("idx,lib_ms,pso_ms,ok\n");
      int lo = 0, hi = 1 << 30;   // SPIKE_RANGE=lo:hi (inclusive) selects a subset of the manifest
      if (getenv("SPIKE_RANGE")) sscanf(getenv("SPIKE_RANGE"), "%d:%d", &lo, &hi);
      for (NSString *line in man) {
         int idx = [[line componentsSeparatedByString:@" "][0] intValue];
         if (idx < lo || idx > hi) continue;
         NSString *base = [NSString stringWithFormat:@"%02d", idx];
         double a = now_ms();
         id<MTLLibrary> vl, fl;
         if (from_lib) {
            vl = lib_from_metallib(dev, [libdir stringByAppendingFormat:@"/%@.vs.metallib", base]);
            fl = lib_from_metallib(dev, [libdir stringByAppendingFormat:@"/%@.fs.metallib", base]);
         } else {
            vl = lib_from_source(comp, [dir stringByAppendingFormat:@"/%@.vs.metal", base], salt);
            fl = lib_from_source(comp, [dir stringByAppendingFormat:@"/%@.fs.metal", base], salt);
         }
         double lib_ms = now_ms() - a;
         if (!vl || !fl) { printf("%d,%.3f,0,0\n", idx, lib_ms); n++; continue; }

         MTL4LibraryFunctionDescriptor *vf = [MTL4LibraryFunctionDescriptor new];
         vf.name = @"main_entrypoint"; vf.library = vl;
         MTL4LibraryFunctionDescriptor *ff = [MTL4LibraryFunctionDescriptor new];
         ff.name = @"main_entrypoint"; ff.library = fl;
         MTL4RenderPipelineDescriptor *pd = [MTL4RenderPipelineDescriptor new];
         pd.vertexFunctionDescriptor = vf;
         pd.fragmentFunctionDescriptor = ff;
         pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
         Att att[8]; int na = attachments(line, idx, att);
         for (int i = 0; i < na; i++) pd.colorAttachments[att[i].slot].pixelFormat = att[i].fmt;

         a = now_ms();
         id<MTLRenderPipelineState> ps;
         if (use_archive) ps = [arcs[0] newRenderPipelineStateWithDescriptor:pd error:&err];
         else ps = [comp newRenderPipelineStateWithDescriptor:pd compilerTaskOptions:topts error:&err];
         double pso_ms = now_ms() - a;
         if (!ps) fprintf(stderr, "pso %d: %s\n", idx, err.localizedDescription.UTF8String);
         printf("%d,%.3f,%.3f,%d\n", idx, lib_ms, pso_ms, ps != nil);
         sum_lib += lib_ms; sum_pso += pso_ms; ok += ps != nil; n++;
      }

      double t_ser = 0; long long size = 0;
      if (capture && outp) {
         t0 = now_ms();
         outp = [[NSURL fileURLWithPath:outp] URLByStandardizingPath].path;
         err = nil;
         if (![ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:outp] error:&err]) {
            fprintf(stderr, "serialize %s: %s (%s)\n", outp.UTF8String,
                    err.localizedDescription.UTF8String, err.description.UTF8String);
            return 1;
         }
         t_ser = now_ms() - t0;
         size = [[[NSFileManager defaultManager] attributesOfItemAtPath:outp error:nil] fileSize];
         NSData *script = [ser serializeAsPipelinesScriptWithError:&err];
         if (script) [script writeToFile:[outp stringByAppendingString:@".script"] atomically:YES];
      }
      printf("SUMMARY mode=%s salt=%s n=%d ok=%d dev_ms=%.1f compiler_ms=%.1f archive_load_ms=%.1f "
             "lib_total_ms=%.1f pso_total_ms=%.1f lib_per_ms=%.3f pso_per_ms=%.3f serialize_ms=%.1f archive_bytes=%lld gpu=%s\n",
             mode.UTF8String, salt.UTF8String, n, ok, t_dev, t_comp, t_arc, sum_lib, sum_pso,
             n ? sum_lib / n : 0, n ? sum_pso / n : 0, t_ser, size, dev.name.UTF8String);
   }
   return 0;
}
