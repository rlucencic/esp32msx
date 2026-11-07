# MSX2 SCREEN 5/7 row palettes — 2026-11-07

Retain a separate sixteen-color row helper for SCREEN 5 and SCREEN 7.
Isolated P4 row cost falls 20.6–26.7% / 24.2–28.3%, respectively.
These are rendering-cost measurements, not general application speedups.

## Reference and change

Reference: esp32msx `b67b5d3`, shared runtime `984253a`. Both builds include
the preceding paired-bank reads and SCREEN 6 four-color helper. Only
local `components/msx/vdp.c` changes. Tab5/P4 runs at 360 MHz with 200 MHz
PSRAM, ESP-IDF 5.5.6 / GCC 14.2 and the component's normal `-O2`/jump-table
flags. Shared runtime, Z80 and VDP command code are unchanged.

SCREEN 5 (G4) decodes two nibbles and doubles each output pixel. SCREEN 7
(G6) decodes two nibbles from each byte of the paired interleaved banks.
Copy the sixteen RGB565 palette entries into a 32-byte automatic array,
then replace entry zero with the existing `color(0)` result. Nonzero
indices select their own entries; zero selects the background unless the
existing MSX2 TP rule disables transparency. The helper rebuilds this
array every row, preserving palette/backdrop/TP changes between rows.
There is no persistent cache, heap allocation or new global table.

A dedicated no-inline helper owns the palette. SCREEN 6 and SCREEN 8
algorithms are unchanged. Byte reads still go through `vram_rd`, with the
same addresses, masks and read/write order. Sprites, scrolling, VDP
commands/status, dirty-frame gating and guest timing are unchanged.
No assembly is added; the measured change is portable C with the existing
compiler's no-inline attribute.

| P4 renderer item | Reference | Candidate |
| --- | ---: | ---: |
| `vdp_line` text | 2586 bytes | 1996 bytes |
| `draw_four_colors` text | 164 bytes | 164 bytes |
| `draw_sixteen_colors` text | — | 304 bytes |
| Combined text | 2750 bytes | 2464 bytes |
| Common renderer stack frame | 64 bytes | 48 bytes |
| SCREEN 5/7 frame plus helper | 64 bytes | 80 bytes |
| SCREEN 6 frame plus helper | 80 bytes | 64 bytes |

Combined renderer text shrinks 286 bytes. SCREEN 5/7 needs sixteen more
stack bytes while its helper runs; the maximum across these rendering
paths stays 80 bytes. VDP global state remains 696 bytes at the same
address. The local palette occupies 32 stack bytes only during its call.
These figures describe P4 compiler output, not S3 stack/code size.

## Correctness and builds

- Actual-source reference/candidate wrappers pass 262,144 full bitmap
  pixel/canary rows under ASan and UBSan: all four bitmap modes, rows
  0–255, page choices, sixteen backdrop entries, TP on/off, varied palettes
  and both 16 KB/128 KB masks. State changes between rows check palette
  refresh. MSX1 never selects the MSX2 bitmap modes; its mask cases are
  defensive checks of these internal helpers.
- An independent scalar oracle checks 32,768 more rows: both model flags,
  SCREEN 5/7, sixteen backdrops, TP on/off and every source byte. It
  computes nibble order and effective palette indices independently,
  checking all 512 output pixels and both canary regions in both versions.
  This verifies existing emulator semantics, not new hardware accuracy.
- Ten host pictures match exactly after 500 frames: MSX1/MSX2 BASIC and
  static/dynamic generated SCREEN 5–8 cartridges. Fixtures initialize
  all 128 KB VRAM through CPU VDP ports and use vertical scroll 37.
- Candidate host/P4 and normal-production S3/P4 builds pass. Target builds
  run sequentially because the checkout shares managed dependencies.
  Only P4 hardware is connected; S3 runtime is unmeasured.

Board test apps keep the isolated `msxvidtest` disk namespace; production
main/builds retain `msxdisks`. No guest disks or user cartridges are
mounted. The existing full Amiga flash backup is retained outside git.
Shared source is unchanged, so the 24-sibling build matrix is unnecessary.

## Isolated P4 timing

Both actual-source bitmap implementations run in one minimal firmware,
sharing 128 KB PSRAM VRAM and internal-RAM output. Each pass draws 64 rows
in `(row * 37) & 255` order four times, with a volatile checksum. Seven
ABBA rounds alternate reference/candidate/candidate/reference. The table
reports median paired-round cycles per row. Interrupts remain enabled;
guest CPU, LCD, networking and audio are absent. Costs exclude sprites,
borders and scaling. They are separate from full-frame PSRAM rendering.

| Case | Reference cycles/row | Candidate cycles/row | Cost change |
| --- | ---: | ---: | ---: |
| SCREEN 5 mixed, TP=0, background 4 | 3196 | 2392 | -25.17% |
| SCREEN 5 mixed, TP=1 | 3258 | 2390 | -26.66% |
| SCREEN 5 zero, background 15 | 3010 | 2392 | -20.55% |
| SCREEN 5 FF | 3011 | 2391 | -20.59% |
| SCREEN 5 mixed, page 3, background 0 | 3183 | 2392 | -24.87% |
| SCREEN 7 mixed, TP=0, background 4 | 5516 | 3953 | -28.33% |
| SCREEN 7 mixed, TP=1 | 5411 | 3951 | -26.98% |
| SCREEN 7 zero, background 15 | 5345 | 3953 | -26.04% |
| SCREEN 7 FF | 5218 | 3953 | -24.25% |
| SCREEN 7 mixed, page 1, background 0 | 5501 | 3953 | -28.14% |
| SCREEN 6 mixed control | 3407 | 3407 | -0.02% |
| SCREEN 8 mixed control | 2891 | 2885 | -0.19% |

## Full firmware

Generated Z80 programs run after fresh MSX2 / 60 Hz / 512 KB BASIC boots.
They initialize all VRAM through real ports, select 212 lines and scroll
37, disable sprites/interrupts, then continuously write VRAM for timing
or loop after one fill for static LCD comparisons. No user software or
disks are used.

Three 32,000-sample captures per mode/core use nominal 20 kHz sampling
(50 microseconds), resolved against each exact flashed ELF. Core 1 is
sampled in all four bitmap modes and core 0 in SCREEN 7. All 30 captures
have the requested count/core label. Shares include idle and are observed
ranges, not direct function times or confidence intervals.

The comparable renderer is `vdp_line` plus `draw_four_colors` plus
`draw_sixteen_colors`, summed per capture. Outlining work into a helper
must not be counted as removing that work.

| Core-1 measure / mode | Reference | Candidate |
| --- | ---: | ---: |
| Combined renderer, SCREEN 5 | 25.5–26.5% | 23.6–23.8% |
| Idle, SCREEN 5 | 45.2–46.5% | 48.3–49.1% |
| Combined renderer, SCREEN 6 control | 28.0–28.2% | 27.8–28.1% |
| Idle, SCREEN 6 control | 44.6–44.7% | 44.6–45.2% |
| Combined renderer, SCREEN 7 | 37.0–37.2% | 33.6–33.9% |
| Idle, SCREEN 7 | 34.3–35.0% | 38.2–38.3% |
| Combined renderer, SCREEN 8 control | 27.9–28.3% | 27.7–28.6% |
| Idle, SCREEN 8 control | 43.6–44.5% | 43.7–44.6% |

SCREEN 5 candidate shares are 5.2–5.4% in `vdp_line` plus 18.4–18.5%
in the new helper; SCREEN 7 shares are 5.7–6.0% plus 27.6–28.0%.
The combined ranges above come from each capture, not independent range
endpoints. SCREEN 6/8 controls are similar between builds; these short
runs do not establish zero integration cost in other workloads.

Core-0 SCREEN 7 `present_task` is 3.1–3.4% in reference and 3.4% in the
candidate. Idle is 77.4–79.5% versus 76.0–78.1%, slightly lower in this
run. The captures do not establish the cause or a presentation benefit;
keep this observation separate from core-1 rendering savings.

Two profiler-free SCREEN 7 windows measure 59.913/59.940 FPS in reference
and 59.950/59.912 FPS in candidate. Pooled rates are 59.926/59.931 FPS
(+0.008%), near the 60 Hz cap. A single fresh run per build does not
establish a general application-speed gain. Redraw signature increments
equal frame increments in this continuously changing workload; neither
is panel FPS.

All four 1280×720 static LCD captures match exactly in decoded RGB.

After testing, Tab5 was restarted as MSX2 / 60 Hz / 512 KB. Its ordinary
BASIC 2.1 / Disk BASIC prompt was visually verified. No synthetic guest
program is running and no guest disks or cartridges are mounted. The
selected uninstrumented test app keeps the isolated disk namespace;
production source/builds retain the normal namespace.


## Decision and next target

Retain the helper: lower isolated and full-renderer cost, exact LCD
output and smaller P4 text. Record the additional SCREEN 5/7 call stack and
core-0 observations as tradeoffs. No general software
speedup follows from a capped synthetic renderer workload.

Next profile generated VDP command fill/copy workloads (HMMV/HMMM and
logical controls) before selecting a command optimization. Direction,
overlap, bank boundaries, partial widths and register updates need exact
reference comparisons. These workloads have not yet been measured.

Representative software, physical input and speaker/audio coverage remain
open. P4 timing does not predict S3 runtime gains. Scratch source,
fixtures, firmware and raw logs remain outside git under
`/tmp/msx-sixteen-2026-11-07`.
