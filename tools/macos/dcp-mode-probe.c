/* dcp-mode-probe.c — minimal reproducer for the DCPEXT1 axi_rd_err panic.
 *
 * WHY: every one of the three panics so far was "some app changed the external
 * display between a 2x HiDPI mode and a 1x mode". That leaves Apple (and us)
 * guessing which part of the request matters. This tool does nothing but that
 * one operation, from a process with no window, no Metal surface and no
 * swapchain, so the next report can say "this and only this".
 *
 * HARD RULE BAKED IN: it refuses to cross the scale while the link is running
 * Display Stream Compression, because that is the class that has panicked this
 * machine three times (2026-10-18). Pass --allow-dsc to override — on this
 * hardware that is a deliberate, reboot-capable experiment, not a toggle.
 *
 * The DSC state is read the same way BetterDisplay reports it: the active link
 * PPS. We do not have a public API for it, so we shell out to
 * `betterdisplaycli get -name=AW3425DW -DSCReport` and look for "DSC): No".
 *
 * Build:  clang -O2 -framework CoreGraphics -o dcp-mode-probe dcp-mode-probe.c
 * Use:    ./dcp-mode-probe                 show current mode + the pair it would switch to (read-only)
 *         ./dcp-mode-probe --to 1x         cross to the 1x mode at the same refresh (guarded)
 *         ./dcp-mode-probe --to 2x         cross back to the 2x mode (guarded)
 *         ./dcp-mode-probe --allow-dsc     override the guard (DANGEROUS on this panel)
 *
 * Exit codes: 0 ok, 2 refused (DSC guard), 3 no pair found, 4 switch failed.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef CGError (*begin_t)(CGDisplayConfigRef *);
typedef CGError (*configmode_t)(CGDisplayConfigRef, CGDirectDisplayID, CGDisplayModeRef);
typedef CGError (*complete_t)(CGDisplayConfigRef, CGConfigureOption);

/* The AW3425DW's EDID identity. Everything here keys off it so the tool can
 * never move the built-in panel by accident. */
#define AW_VENDOR 4268      /* 0x10AC, Dell */
#define AW_MODEL  41610     /* 0xA28A */

static int looks_like_aw(CGDirectDisplayID d)
{
    return CGDisplayVendorNumber(d) == AW_VENDOR && CGDisplayModelNumber(d) == AW_MODEL;
}

/* "DSC): No" from BetterDisplay's PPS report; -1 when the readout is unavailable. */
static int link_has_dsc(void)
{
    FILE *p = popen("/opt/homebrew/bin/betterdisplaycli get -name=AW3425DW -DSCReport 2>/dev/null", "r");
    if (!p) return -1;
    char buf[4096]; int dsc = -1;
    while (fgets(buf, sizeof buf, p)) {
        if (strstr(buf, "Display Stream Compression (DSC):")) {
            dsc = strstr(buf, ": No") ? 0 : 1;
        }
    }
    pclose(p);
    return dsc;
}

int main(int argc, char **argv)
{
    int want = 0;             /* 1 = 1x, 2 = 2x, 0 = dry run */
    int allow_dsc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--to") && i + 1 < argc) {
            if (!strcmp(argv[++i], "1x")) want = 1;
            else if (!strcmp(argv[i], "2x")) want = 2;
            else { fprintf(stderr, "--to takes 1x or 2x\n"); return 1; }
        } else if (!strcmp(argv[i], "--allow-dsc")) allow_dsc = 1;
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 1; }
    }

    uint32_t n = 0; CGDirectDisplayID ds[16];
    CGGetOnlineDisplayList(16, ds, &n);
    CGDirectDisplayID aw = 0;
    for (uint32_t i = 0; i < n; i++) if (looks_like_aw(ds[i])) aw = ds[i];
    if (!aw) { fprintf(stderr, "AW3425DW not attached\n"); return 3; }

    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(aw);
    if (!cur) { fprintf(stderr, "no current mode\n"); return 3; }
    size_t cw = CGDisplayModeGetPixelWidth(cur), ch = CGDisplayModeGetPixelHeight(cur);
    size_t pw = CGDisplayModeGetWidth(cur),     ph = CGDisplayModeGetHeight(cur);
    double hz = CGDisplayModeGetRefreshRate(cur);
    unsigned id = (unsigned)CGDisplayModeGetIODisplayModeID(cur);
    int cur_is_2x = (cw >= pw * 2 - 2) && (cw > 0) && (pw > 0) && (cw != pw);

    printf("display 0x%x  current mode %u: pixel %zux%zu point %zux%zu %.2f Hz (%s)\n",
           aw, id, cw, ch, pw, ph, hz, cur_is_2x ? "2x" : "1x");

    /* Find the twin: same point size, same refresh, opposite pixel size. */
    const void *keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
    const void *vals[] = { kCFBooleanTrue };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(aw, opts);
    CGDisplayModeRef pair = NULL;
    if (modes) {
        for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            if (CGDisplayModeGetWidth(m) != pw || CGDisplayModeGetHeight(m) != ph) continue;
            double mh = CGDisplayModeGetRefreshRate(m);
            if (fabs(mh - hz) > 0.5) continue;
            size_t mw = CGDisplayModeGetPixelWidth(m);
            int m_is_2x = (mw >= CGDisplayModeGetWidth(m) * 2 - 2) && (mw != CGDisplayModeGetWidth(m));
            if (m_is_2x != cur_is_2x) {
                if (want == 0 || (want == 1 && !m_is_2x) || (want == 2 && m_is_2x)) { pair = m; break; }
            }
        }
    }

    if (want == 0) {
        if (pair) printf("would switch to mode %u: pixel %zux%zu point %zux%zu %.2f Hz (%s)\n",
                         (unsigned)CGDisplayModeGetIODisplayModeID(pair),
                         CGDisplayModeGetPixelWidth(pair), CGDisplayModeGetPixelHeight(pair),
                         CGDisplayModeGetWidth(pair), CGDisplayModeGetHeight(pair),
                         CGDisplayModeGetRefreshRate(pair), cur_is_2x ? "1x" : "2x");
        else printf("no cross-scale twin at this refresh\n");
        return pair ? 0 : 3;
    }
    if (!pair) { fprintf(stderr, "no cross-scale twin at this refresh\n"); return 3; }

    int dsc = link_has_dsc();
    printf("link DSC state: %s\n", dsc == 0 ? "No" : dsc == 1 ? "Yes" : "unknown");
    if (dsc != 0 && !allow_dsc) {
        fprintf(stderr,
            "REFUSED: the link is DSC-coded (or unreadable). Crossing the source plane while DSC\n"
            "is live is the class that has panicked this machine three times (DCPEXT1 axi_rd_err).\n"
            "Drop the connection mode to one that reports 'DSC: No' first, then re-run.\n"
            "--allow-dsc overrides; expect a hard reboot if the hypothesis is right.\n");
        return 2;
    }

    void *sl = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    begin_t p_begin = (begin_t)dlsym(RTLD_DEFAULT, "CGBeginDisplayConfiguration");
    complete_t p_complete = (complete_t)dlsym(RTLD_DEFAULT, "CGCompleteDisplayConfiguration");
    configmode_t p_configmode = sl ? (configmode_t)dlsym(sl, "CGSConfigureDisplayMode") : NULL;
    if (!p_begin || !p_complete || !p_configmode) {
        fprintf(stderr, "missing symbols (SkyLight handle %p)\n", sl);
        return 4;
    }

    unsigned target = (unsigned)CGDisplayModeGetIODisplayModeID(pair);
    printf(">>> switching display 0x%x to mode %u\n", aw, target); fflush(stdout);

    CGDisplayConfigRef cfg = NULL;
    if (p_begin(&cfg) != kCGErrorSuccess) { fprintf(stderr, "begin failed\n"); return 4; }
    CGError e = p_configmode(cfg, aw, pair);   /* the CONFIG REF is the first arg */
    if (e != kCGErrorSuccess) { fprintf(stderr, "configure failed: %d\n", (int)e); CGCancelDisplayConfiguration(cfg); return 4; }
    e = p_complete(cfg, kCGConfigureForSession);
    if (e != kCGErrorSuccess) { fprintf(stderr, "complete failed: %d\n", (int)e); return 4; }

    printf("switch returned success; display stays captured-for-session in this process\n");
    return 0;
}
