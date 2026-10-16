// bbport (macOS warm runs): a hidden, non-frontmost process is App-Napped and QoS-demoted,
// which slows down the shader compiles a warm run exists to do. One activity assertion keeps
// the hidden run at full speed; the process exits before it would ever need to end it.
#import <Foundation/Foundation.h>

extern "C" void bb_warm_disable_app_nap(void) {
    static id activity = nil;
    if (activity) {
        return;
    }
    activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical |
                                   NSActivityIdleSystemSleepDisabled)
                          reason:@"bbport warm run (BB_WARM_ONLY)"];
}
