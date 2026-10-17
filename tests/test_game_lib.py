"""The game library plug-in point (BB_GAME_LIB, src/bbgame.h, docs/GAME_LIBRARY.md) on tiny
synthetic images: the loader loads the library, the library's install_hook replaces a function,
and the game reaches the replacement instead of the original code. Libraries are built here."""
from paths import ROOT
from pathlib import Path
import os
import platform
import shutil
import struct
import subprocess
import tempfile
import unittest

EXE = ROOT / 'out/bb-probe'
SUFFIX = '.dylib' if platform.system() == 'Darwin' else '.so'

# Hooks the image entry (guest offset 0, a 16-byte function) with a host function that says so and
# exits 42; the original function jumps to the import 'original-path' (exit 20 with its name).
FIXTURE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "bbgame.h"
__attribute__((sysv_abi)) static void replacement(void) {
    printf("fixture: replacement reached\n"); fflush(stdout); _exit(42);
}
__attribute__((visibility("default"))) int bbgame_init(const BbGameHost *host) {
    if (host->version != BBGAME_API_VERSION) return 2;
    const char *mode = getenv("FIXTURE_MODE");
    if (mode && mode[0] == 'f') return 7;
    int short_hook = host->install_hook(0, 13, (const void *)replacement);
    int outside = host->install_hook(host->image_size, 16, (const void *)replacement);
    int data = host->install_hook(4096, 16, (const void *)replacement);
    printf("fixture: refusals %d %d %d\n", short_hook, outside, data);
    if (short_hook != -1 || outside != -1 || data != -1) return 3;
    if (mode && mode[0] == 'n') return 0;
    return host->install_hook(0, 16, (const void *)replacement);
}
'''


def compiler_args(source, output):
    args = ['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fvisibility=hidden',
            '-I' + str(ROOT / 'src'), str(source), '-o', str(output)]
    if platform.system() == 'Darwin':
        args[1:1] = ['-arch', 'x86_64', '-dynamiclib']
    else:
        args[1:1] = ['-fPIC', '-shared']
    return args


def package():
    # Segment 0 (code, r-x): jmp [rip+2]; nop; nop; .quad -> import 0. Segment 1 (data, rw-).
    code = b'\xff\x25\x02\0\0\0\x90\x90'.ljust(4096, b'\0')
    image = code + b'\0' * 4096
    header = struct.pack('<8sQQQQQ', b'BBPROBE1', len(image), 0, 2, 1, 1)
    segments = struct.pack('<QQQ', 0, 4096, 5) + struct.pack('<QQQ', 4096, 4096, 6)
    names = b'original-path'.ljust(128, b'\0')
    relocs = struct.pack('<QQqq', 8, 1, 0, 0)
    return header + segments + names + relocs + image


@unittest.skipUnless(EXE.exists() and shutil.which('clang'), 'build native probe first')
class GameLibraryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        (tmp / 'fixture.c').write_text(FIXTURE)
        (tmp / 'empty.c').write_text('int bbgame_unrelated;\n')
        cls.lib = tmp / ('libfixture' + SUFFIX)
        cls.empty = tmp / ('libempty' + SUFFIX)
        subprocess.run(compiler_args(tmp / 'fixture.c', cls.lib), check=True)
        subprocess.run(compiler_args(tmp / 'empty.c', cls.empty), check=True)
        cls.noop = ROOT / ('out/game_lib/libbbgame-noop' + SUFFIX)
        subprocess.run(['bash', str(ROOT / 'examples/game_lib/build.sh')], check=True,
                       capture_output=True)
        (tmp / 'boot.bin').write_bytes(package())
        cls.image = tmp / 'boot.bin'

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_image(self, **env):
        environment = {k: v for k, v in os.environ.items()
                       if k not in ('BB_GAME_LIB', 'BB_SKIP_GAME_CHECK', 'FIXTURE_MODE',
                                    'BBGAME_NOOP_SELFTEST')}
        environment.update(env)
        return subprocess.run([str(EXE.resolve()), str(self.image), '--cpu-only'], env=environment,
                              capture_output=True, text=True, timeout=10)

    def test_unset_runs_the_original_function(self):
        for value in (None, ''):
            with self.subTest(value=value):
                r = self.run_image(**({} if value is None else {'BB_GAME_LIB': value}))
                self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
                self.assertIn('first unsupported PS4 import: original-path', r.stdout)
                self.assertNotIn('Game library', r.stdout)

    def test_hook_replaces_the_function(self):
        r = self.run_image(BB_GAME_LIB=str(self.lib))
        self.assertEqual(r.returncode, 42, r.stdout + r.stderr)
        self.assertIn('fixture: refusals -1 -1 -1', r.stdout)
        self.assertIn(f'Game library {self.lib}: 1 hooks installed, init returned 0', r.stdout)
        self.assertIn('fixture: replacement reached', r.stdout)
        self.assertNotIn('original-path', r.stdout)

    def test_library_without_hooks_changes_nothing(self):
        r = self.run_image(BB_GAME_LIB=str(self.lib), FIXTURE_MODE='none')
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('0 hooks installed, init returned 0', r.stdout)
        self.assertIn('first unsupported PS4 import: original-path', r.stdout)

    def test_noop_example_loads_and_selftests(self):
        r = self.run_image(BB_GAME_LIB=str(self.noop), BBGAME_NOOP_SELFTEST='1')
        self.assertEqual(r.returncode, 20, r.stdout + r.stderr)
        self.assertIn('bbgame-noop: host interface v1', r.stdout)
        self.assertIn('short function -> -1, outside the image -> -1', r.stdout)
        self.assertIn('0 hooks installed, init returned 0', r.stdout)

    def test_failures_stop_before_the_game(self):
        for env, message in [
            ({'BB_GAME_LIB': str(self.lib), 'FIXTURE_MODE': 'fail'}, 'game library init failed'),
            ({'BB_GAME_LIB': str(self.empty)}, 'game library has no bbgame_init'),
            ({'BB_GAME_LIB': str(self.lib) + '.missing'}, 'cannot load the game library'),
            ({'BB_GAME_LIB': str(self.lib), 'BB_SKIP_GAME_CHECK': '1'},
             'BB_GAME_LIB needs the supported executable'),
        ]:
            with self.subTest(env=env):
                r = self.run_image(**env)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(message, r.stderr)
                self.assertNotIn('original-path', r.stdout)


if __name__ == '__main__':
    unittest.main()
