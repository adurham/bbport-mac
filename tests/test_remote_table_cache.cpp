// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the GPU process's mapping table caches each thread's last mapping for one generation
// (bb_vma_table.h): a change on another thread invalidates it, tables don't share it, and readers
// on several threads while one maps and unmaps stay consistent (also run under ThreadSanitizer).
//   c++ -std=c++20 -O2 -pthread -Igpu/shim tests/test_remote_table_cache.cpp -o /tmp/table-cache-test && /tmp/table-cache-test
#include <cassert>
#include <cstdio>
#include <thread>
#include "remote/bb_vma_table.h"
using namespace BbRemote;
int main() {
    VmaTable t;
    VmaEntry e{0x2000, MapDirect, 3, 0, 0};
    t.Map(0x1000, 0x2000, e);
    assert(t.Covered(0x1100, 0x100) == 1);   // fills the cache
    assert(t.Covered(0x1100, 0x100) == 1);   // hit
    assert(t.Covered(0x1f00, 0x200) == 0);   // past the end: not from the cache
    uintptr_t s, en; int m;
    assert(t.Region(0x1800, &s, &en, &m) && m == 1 && s == 0x1000 && en == 0x2000);
    std::thread([&] { t.Unmap(0x1000, 0x2000); }).join(); // another thread changes it
    assert(t.Covered(0x1100, 0x100) == 0);   // the cache is stale now
    assert(t.Region(0x1800, &s, &en, &m) == 0 || m == 0);
    // Another table at the same generation with other contents: no hit from this one's cache.
    VmaTable other;
    for (int i = 0; i < 3; ++i) {            // t has had three writes (map, unmap, map below)
        other.Map(0x5000, 0x6000, e);
    }
    t.Map(0x1000, 0x2000, e);
    assert(*t.Generation() == *other.Generation());
    assert(t.Covered(0x1100, 0x10) == 1);    // fills this thread's cache from t
    assert(other.Covered(0x1100, 0x10) == 0);
    assert(t.Covered(0xfffffffffffff000ull, 0x2000) == 0); // wraps: refused
    // Readers on several threads while one thread maps and unmaps: never a crash or a hit
    // on a range unmapped before the reader started (checked loosely: the final state).
    std::atomic<bool> stop{false};
    std::thread w([&] { for (int i = 0; i < 20000; ++i) { t.Unmap(0x1000, 0x2000); t.Map(0x1000, 0x2000, e); } stop = true; });
    std::thread r[4];
    for (auto& th : r) th = std::thread([&] { uintptr_t a, b; int c; while (!stop) { (void)t.Covered(0x1100, 0x10); (void)t.Region(0x1100, &a, &b, &c); } });
    w.join(); for (auto& th : r) th.join();
    t.Unmap(0x1000, 0x2000);
    assert(t.Covered(0x1100, 0x10) == 0);
    std::puts("vma table cache: PASS");
}
