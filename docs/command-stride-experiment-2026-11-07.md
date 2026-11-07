# MSX2 packed-byte command loops — 2026-11-07

Retain packed-byte iteration for HMMV and HMMM. Large-command isolated
P4 costs fall 14.6–67.2%, depending on mode and operation. The generated
fill/copy stress programs also run faster. This does not establish game
or general application performance; YMMM kernel controls cost 1.3–2.5%
more, and the shared register-write function needs sixteen more stack bytes.

## Reference and change

Reference: esp32msx `ea1b834`, shared runtime `5c2a3fd`. Both builds include
the bitmap bank/palette optimizations. Only local `components/msx/vdp.c`
changes. Tab5/P4 uses 360 MHz CPU, 200 MHz PSRAM, ESP-IDF 5.5.6 / GCC 14.2
and normal component `-O2`/jump-table flags. No shared source or assembly
changes. The full reference is the preceding trial's selected app/ELF,
whose VDP source is now committed as `ea1b834`.

HMMV fills a byte and HMMM copies a byte at pixel offsets
`0, pixels_per_byte, ... < NX`. Previously both loops visited every pixel
and used `i % ppb == 0` to decide whether to access VRAM. Advance the loop
index and X coordinates by `ppb` for byte commands, retaining step one for
LMMV/LMMM logical pixel commands. SCREEN 5/7 packs two pixels per byte,
SCREEN 6 packs four and SCREEN 8 packs one; its improvement comes from
removing the byte-selection remainder/branch and compiler output changes.

This preserves the first addressed byte, partial final groups, direction,
source-read/destination-write order and overlap propagation. Skipped
iterations perform no VRAM accesses or state changes. X coordinates move
monotonically, so skipping a clipped nonwriting pixel cannot introduce a
later in-range write. The same bounds checks remain at every access.
VRAM masks and interleaved-bank mapping are unchanged, as are vertical
wrap, zero-size defaults, command/register/status updates and dirty flags.

Commands still execute synchronously at once, as the existing emulator
does; no V9938 timing accuracy change is claimed. CPU instruction timing,
CPU-transfer commands, sprites, audio and rendering algorithms are unchanged.
No bulk copy/fill primitive is introduced, so overlapping copies retain
their original ordered behavior.

P4 `reg_write` includes the inlined command dispatcher. Its text grows
from 2308 to 2412 bytes (+104), and its compiler stack frame from 112 to
128 bytes (+16), including ordinary register writes. These are function
frames, not total call-chain stack usage. `point`, `pset`, `vdp_line` and
the bitmap helpers retain their sizes. VDP global state stays 696 bytes
at the same address. No new heap allocation, table or persistent state.
These code/stack figures are P4-specific.

## Correctness and builds

- Actual-source wrappers pass 65,248 full 128 KB VRAM/state/canary
  comparisons under ASan/UBSan. Structured cases cover all four bitmap
  modes, both byte operations, four directions, aligned/unaligned and
  out-of-range X values, partial/default widths and edge rows. Seeded
  cases add vertical overlap/wrap, long/default heights, logical operations
  including transparency, and nonbitmap command-coordinate fallback.
  Full normalized VDP state includes command progress, registers, status,
  palettes and dirty flags, not just visible pixels.
- A separate scalar byte-command oracle checks all 58,224 HMMV/HMMM cases.
  It enumerates pixel offsets and independently computes packed/interleaved
  physical addresses and ordered writes, comparing the entire VRAM image.
  Tests verify existing emulator semantics rather than hardware accuracy.
- Eighteen host pictures match: MSX1/MSX2 BASIC and static/dynamic SCREEN
  5–8 bitmap fixtures after 500 frames, plus static/repeated generated
  SCREEN 6 HMMV, SCREEN 7 HMMM, SCREEN 5 LMMV and SCREEN 8 LMMM cartridges
  after 160 frames. Fixtures initialize all VRAM through real CPU ports;
  command fixtures run after that initialization.
- Candidate host/P4 and normal-production S3/P4 builds pass. Target builds
  run sequentially because managed dependencies share a checkout. Only
  P4 hardware is connected; S3 runtime remains unmeasured.

Test apps keep isolated disk namespace `msxvidtest`; production main/builds
keep `msxdisks`. No user disks/cartridges are mounted or written. The
existing full Amiga flash backup is retained outside git. Shared source
is unchanged, so the 24-sibling build matrix is unnecessary.

## Isolated P4 timing

Both actual-source implementations run in one minimal firmware with
separate 128 KB PSRAM VRAM buffers initialized identically. Each timed
pass executes four commands, resetting SY/DY/NY before each command.
Seven ABBA rounds alternate reference/candidate/candidate/reference;
report median paired-round cycles per command, including those small
register resets. Interrupts remain enabled; guest CPU, display, audio and
networking are absent. Each version receives the same number of calls.

Large commands use full width and 128 rows. Small reverse cases use 127
pixels and three rows, unaligned starts and decreasing X/Y; copy source
and destination overlap. Logical and YMMM controls use full width and
64 rows. Default NX=0 selects full width; no guest-timing claim follows
from these host execution costs.

| Command case | Reference cycles/command | Candidate cycles/command | Cost change |
| --- | ---: | ---: | ---: |
| SCREEN 5 HMMV, full × 128 | 970515 | 564272 | -41.86% |
| SCREEN 5 HMMM, full × 128 | 1382681 | 832133 | -39.82% |
| SCREEN 6 HMMV, full × 128 | 1532895 | 515042 | -66.40% |
| SCREEN 6 HMMM, full × 128 | 2037199 | 667323 | -67.24% |
| SCREEN 7 HMMV, full × 128 | 2124812 | 1289655 | -39.31% |
| SCREEN 7 HMMM, full × 128 | 3205645 | 2173922 | -32.18% |
| SCREEN 8 HMMV, full × 128 | 1640931 | 1322676 | -19.39% |
| SCREEN 8 HMMM, full × 128 | 2633436 | 2249082 | -14.60% |
| SCREEN 5 HMMV, reverse/partial | 11242 | 6818 | -39.35% |
| SCREEN 5 HMMM, reverse/overlap | 16047 | 9961 | -37.92% |
| SCREEN 6 HMMV, reverse/partial | 8550 | 3269 | -61.76% |
| SCREEN 7 HMMM, reverse/overlap | 17584 | 10721 | -39.03% |
| SCREEN 5 LMMV control | 1467930 | 1369252 | -6.72% |
| SCREEN 8 LMMM control | 2550579 | 2487987 | -2.45% |
| SCREEN 7 YMMM control | 936463 | 960139 | 2.53% |
| SCREEN 8 YMMM control | 971970 | 984906 | 1.33% |

Logical algorithms are unchanged; their costs improve 2.5–6.7% in this
build. Unchanged YMMM costs rise 1.3–2.5%. Code layout/compiler changes
can affect controls, so retain and report those measured costs rather
than claiming every command improves.

## Full firmware

Generated Z80 programs are injected into MSX2 / 60 Hz / 512 KB BASIC.
They initialize all 128 KB VRAM through CPU ports, select 212 lines,
disable sprites/interrupts and use scroll zero. Repeated workloads launch
large fill/copy commands continuously, resetting SY/DY/NY each time.
Static counterparts launch one command then loop for LCD comparison.
Fill uses color A5; copy reads from Y=256 into Y=0. No user software is used.

The reference starts from the preceding trial's restored BASIC session;
the candidate starts from a new BASIC boot. Both receive the same complete
workload initialization. These are single-run observations, not controlled
confidence intervals or representative application results.

Three 32,000-sample captures per mode/core use nominal 20 kHz sampling
(50 microseconds) and each exact flashed ELF. Core 1 covers all four
command workloads; core 0 covers SCREEN 7 HMMM. All 30 captures have the
requested count/core label. Command work appears in `reg_write` because
the dispatcher is inlined there; logical workloads also use `pset`/`point`.
The command-path share below sums those functions per capture and includes
ordinary register handling. It is not a direct command timer.

| Core-1 workload | Reference command-path share | Candidate command-path share |
| --- | ---: | ---: |
| SCREEN 6 HMMV | 98.3–98.7% | 96.6–96.8% |
| SCREEN 7 HMMM | 99.0–99.1% | 98.6–98.8% |
| SCREEN 5 LMMV | 98.3–98.6% | 98.6–98.6% |
| SCREEN 8 LMMM | 98.8–99.0% | 98.5–99.0% |

These programs remain heavily command-bound after optimization. The
candidate executes more command work per wall-clock second, so a similar
high command-path share does not imply similar execution cost. Combined
renderer shares rise from 0.5% to 1.3–1.5% in the fill stress test as more
frames are completed; the renderer algorithm is unchanged. Logical-control
profiles show `pset`/`point` as the remaining major work, without a
throughput comparison for those controls.

Two profiler-free 15-second windows per byte workload/build give:

| Generated stress workload | Reference FPS windows | Candidate FPS windows | Pooled reference → candidate FPS |
| --- | ---: | ---: | ---: |
| SCREEN 6 HMMV | 1.132 / 1.132 | 3.261 / 3.326 | 1.132 → 3.294 |
| SCREEN 7 HMMM | 0.533 / 0.533 | 0.799 / 0.799 | 0.533 → 0.799 |

These are roughly 2.9× / 1.5× rates in deliberately overloaded synthetic
programs, far below the 60 Hz target. They establish no speedup for normal
games or applications. The pooled windows contain only 34/99 completed
fill frames and 16/24 copy frames (reference/candidate), so frame-count
quantization and short-run variation limit precision. Redraw signature
increments equal frame increments here; neither measures panel FPS.

Core-0 SCREEN 7 HMMM idle is 92.9–93.1% in reference and 91.9–92.8% in
candidate. `present_task` is 0.1% versus 0.1–0.2%. More completed emulated
frames also require more presentation work; these shares establish no
presentation optimization.

All four static 1280×720 LCD captures match exactly in decoded RGB.


After testing, Tab5 was restarted as MSX2 / 60 Hz / 512 KB. Its ordinary
BASIC 2.1 / Disk BASIC prompt was visually verified. No synthetic guest
program is running and no guest disks/cartridges are mounted. The selected
uninstrumented test app keeps the isolated disk namespace; production
source/builds retain the normal namespace.

## Decision and next target

Retain byte-stride iteration: exact VRAM/state and LCD output, substantial
byte-command savings and higher throughput in the command-bound stress
programs. Record the added 104 P4 text bytes, common sixteen-byte stack
increase and YMMM kernel regressions as tradeoffs. General application
speedups remain unmeasured.

A bounded alternative converts NX to a byte count and scales DIX before
entering the loop. It passes the same native comparisons/oracles, but
keeps the 128-byte P4 register-write frame and adds 66 text bytes beyond
the selected form. It is not hardware-timed or retained.

Next investigate logical-command pixel work (`pset`/`point`) using the
existing generated controls, with operation/transparency coverage and
ordered overlaps. Representative MSX software, physical input and speaker
checks remain open. P4 timing does not predict S3 runtime gains. Scratch
source, fixtures, raw logs and firmware remain outside git under
`/tmp/msx-command-2026-11-07`.
