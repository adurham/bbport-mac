#!/bin/sh
# Build + run every sweep, then analyse -> results.txt. Headless, no window, ~1 min.
set -eu
cd "$(dirname "$0")"
clang++ -std=c++17 -fobjc-arc -O2 main.mm -o mtl-split-bench -framework Metal -framework Foundation
B=./mtl-split-bench
NS=1,2,4,8,16,32,51,102,256
{
  # A: total draws held constant (51 x 200 = 10200, the max-split recording at ~200 draws/CB)
  $B --label A_const_total --total-draws 10200 --reps 3 --n $NS
  # B: draws proportional to N (200 draws per command buffer)
  $B --label B_200_per_cb --draws-per-cb 200 --reps 3 --n $NS
  # B50: draws proportional to N (50 draws per command buffer)
  $B --label B50_50_per_cb --draws-per-cb 50 --reps 3 --n $NS
  # C: compute encoders (pre/post-gfx shape), constant total of 10200 dispatches
  $B --label C_compute_const --compute --total-draws 10200 --reps 3 --n $NS
  # D: one command buffer, vary draws (isolates MB/draw)
  for d in 0 100 200 500 1000 2000 5000 10200 20000 50000; do
    $B --label D_1cb_draws --draws-per-cb $d --reps 3 --n 1
  done
  # E: one command buffer, vary encoders at 0 draws (isolates MB/encoder)
  for e in 1 2 8 51 102 256; do
    $B --label E_1cb_encoders --draws-per-cb 0 --enc-per-cb $e --reps 3 --n 1
  done
  # F: no GPU commit (capacity is set at encode time, not by submission)
  $B --label F_nocommit --no-commit --total-draws 10200 --reps 3 --n $NS
} > raw.txt
python3 analyze.py raw.txt | tee results.txt
# G: empty command buffers / single empty encoder (block attribution), appended raw
{ $B --label G_empty_cb --draws-per-cb 0 --enc-per-cb 0 --reps 3 --n 1,2,51,256
  $B --label G_compute_empty --compute --draws-per-cb 0 --reps 3 --n 1,51; } | grep '^ROW ' | tee -a results.txt
