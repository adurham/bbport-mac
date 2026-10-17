// SPDX-License-Identifier: GPL-2.0-or-later
// The smallest game library (BB_GAME_LIB, src/bbgame.h): it checks the host interface, says so
// and replaces nothing, so the game runs exactly as without it. A Paleblood build of libbbgame
// has the same shape, with install_hook calls for the functions it provides. docs/GAME_LIBRARY.md.
//
// BBGAME_NOOP_SELFTEST=1 additionally asks the loader for two hooks it must refuse (a function
// shorter than the 14-byte jump; an offset past the image) and reports both answers. Neither
// writes anything: the refusals are the check that install_hook validates what it is given.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbgame.h"

static void never_called(void) {}

__attribute__((visibility("default"))) int bbgame_init(const BbGameHost *host) {
    if (!host || host->version != BBGAME_API_VERSION || !host->image || !host->install_hook) {
        fprintf(stderr, "bbgame-noop: unsupported host interface (version %u, want %u)\n",
                host ? host->version : 0u, BBGAME_API_VERSION);
        return 1;
    }
    printf("bbgame-noop: host interface v%u, image %p, %llu bytes; no hooks requested\n",
           host->version, (void *)host->image, (unsigned long long)host->image_size);
    const char *selftest = getenv("BBGAME_NOOP_SELFTEST");
    if (selftest && !strcmp(selftest, "1")) {
        int too_short = host->install_hook(0, 13, (const void *)never_called);
        int outside = host->install_hook(host->image_size, 14, (const void *)never_called);
        printf("bbgame-noop: selftest: short function -> %d, outside the image -> %d (both must be -1)\n",
               too_short, outside);
        if (too_short != -1 || outside != -1) return 1;
    }
    return 0;
}
