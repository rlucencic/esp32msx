# MSX2 SCREEN 6 row palette — 2026-11-07

Retain a row-local four-color palette for SCREEN 6, replacing repeated
per-pixel zero/backdrop selection. The selected helper reduces isolated
P4 row cost 37.9–39.5% on mixed bytes and 15.6–29.1% on flat bytes, with
higher measured core-1 idle headroom. These are rendering-cost results,
not general application speedups.

## Reference and change

Reference: esp32msx `fa5d2d0`, shared runtime `dddfe50`. The paired SCREEN 7/8
bank-read optimization is present in both builds. Only local
`components/msx/vdp.c` changes; shared runtime, Z80 and VDP command code
are unchanged. Tests use Tab5/P4 at 360 MHz, PSRAM at 200 MHz,
ESP-IDF 5.5.6 / GCC 14.2 and the component's normal `-O2`/jump-table flags.

In mode G5 (MSX BASIC SCREEN 6), each input byte contains four 2-bit
indices. The existing `color()` rule maps zero to the current background
palette entry unless V9938 transparency is disabled by TP. Indices 1–3
always read their matching palette entries. Construct those four effective
RGB565 values once, then use the same four indices to select them. The
first shift needs no `& 3` because its unsigned input byte is at most 255.

A dedicated no-inline helper owns the eight-byte automatic palette and
rebuilds it for every SCREEN 6 row. Keeping that helper separate prevents
its local array from enlarging the common renderer's stack frame. Palette,
backdrop and TP changes between rows therefore take effect as before;
there is no persistent color cache. VRAM addressing/masking, sprites,
scrolling, interrupts/status, command operations and dirty-frame skipping
are unchanged. No guest timing or assembly code changes.

P4 `vdp_line` shrinks from 3,068 to 2,586 bytes; the separate helper adds
164 bytes, for 2,750 combined (318 fewer text bytes). The common renderer
keeps its 64-byte compiler stack frame. The helper uses 16 additional
stack bytes only during SCREEN 6 drawing (80 bytes combined at that point).
There is no new heap allocation, global table or persistent RAM state.
VDP global state remains 696 bytes at the same address.

## Correctness and builds

- Actual-source reference/candidate bitmap wrappers pass 262,144 full
  pixel/canary comparisons under ASan and UBSan, covering all four bitmap
  modes, all source rows, page choices, sixteen backdrop indices, TP
  on/off and varied palettes. State changes between comparisons exercise
  row-local palette refresh. Both 16 KB and 128 KB masks are covered;
  MSX1 does not actually select the MSX2 bitmap modes.
- A separate scalar oracle checks 16,384 additional rows: every source
  byte × sixteen backdrop values × TP on/off × both model flags. It
  computes expected palette indices independently and checks every output
  pixel and both canary regions against both implementations. This verifies
  the existing emulator palette rule, not new hardware accuracy.
- Ten host pictures match exactly after 500 frames: MSX1/MSX2 BASIC and
  static/animated generated SCREEN 5–8 cartridges. The cartridges initialize
  all 128 KB VRAM through real CPU VDP ports and use vertical scroll 37.
  They are the same fixtures as the preceding bank-read trial.
- Candidate host/P4 and normal-production S3/P4 builds pass. Target builds
  run sequentially because their managed dependency sets share a checkout.
  Only P4 hardware is connected; S3 runtime and LCD behavior are unmeasured.

Production main keeps `msxdisks`; board test apps use `msxvidtest`. No guest
disks or user cartridges are mounted. The existing full Amiga flash backup
is retained outside git. Shared source is unchanged, so the 24-sibling
build matrix is unnecessary.

## Isolated P4 timing

Both actual-source bitmap versions run in one minimal firmware, sharing
128 KB PSRAM VRAM with internal-RAM output. A pass draws 64 rows in
`(row * 37) & 255` order four times, with a volatile checksum. Seven ABBA
rounds alternate reference/candidate/candidate/reference. Report median
paired-round cycles per row. Interrupts remain enabled; guest CPU, LCD,
audio and networking are absent. These costs exclude sprites, borders
and display scaling.

| Case | Reference cycles/row | Helper cycles/row | Cost change |
| --- | ---: | ---: | ---: |
| SCREEN 6 mixed TP=0 background 4 | 5626 | 3401 | -39.55% |
| SCREEN 6 mixed TP=1 | 5479 | 3401 | -37.93% |
| SCREEN 6 mixed background 0 | 5621 | 3401 | -39.49% |
| SCREEN 6 mixed page 3 background 15 | 5586 | 3405 | -39.05% |
| SCREEN 6 zero TP=0 | 4160 | 3405 | -18.16% |
| SCREEN 6 zero TP=1 | 4031 | 3401 | -15.63% |
| SCREEN 6 FF | 4804 | 3405 | -29.13% |
| SCREEN 6 55 | 4804 | 3405 | -29.13% |
| SCREEN 6 AA | 4804 | 3405 | -29.13% |
| SCREEN 5 mixed control | 3107 | 3104 | -0.08% |
| SCREEN 7 mixed control | 5447 | 5440 | -0.12% |
| SCREEN 8 mixed control | 2888 | 2884 | -0.12% |

The initial inlined version also passes the same correctness and host
checks. Its mixed-color savings are 37.8–39.6%, but SCREEN 5/7 controls
cost 2.34%/0.94% more, and its common renderer's stack frame grows to 80
bytes for every mode. Reject that form. The selected helper retains the
eligible savings while the three isolated controls stay within 0.13% of
reference. This is why inlining is explicitly disabled for the helper.
Kernel percentages are not whole-emulator performance results.


## Full firmware

Synthetic Z80 programs run after a fresh MSX2 / 60 Hz / 512 KB BASIC
boot, using the previous trial's SCREEN 5–8 workloads. They initialize all
VRAM, select 212 lines and scroll 37, disable sprites/interrupts, then
continuously write VRAM for timing or loop after one fill for static LCD
comparison. No user disks or cartridges are mounted.

Three 32,000-sample captures per mode/core use nominal 20 kHz sampling
(50 microseconds), resolved against each exact flashed ELF. Core 1 is
sampled in all four modes; core 0 in SCREEN 6. All 30 reference/helper
captures have the requested count/core label. These are observed sample
shares including idle, not direct function times or confidence intervals.

The selected version moves bitmap conversion into a separate function,
so its share must be added to `vdp_line` for comparison with reference.
The combined range is calculated per capture, not by adding independent
range endpoints.

| Core-1 function / mode | Reference share | Selected helper share |
| --- | ---: | ---: |
| `vdp_line`, SCREEN 6 | 33.9–34.4% | 5.3–5.5% |
| `draw_four_colors`, SCREEN 6 | — | 22.3–22.4% |
| Combined renderer, SCREEN 6 | 33.9–34.4% | 27.6–27.9% |
| Idle, SCREEN 6 | 37.5–38.8% | 43.2–44.9% |
| `vdp_line`, SCREEN 5 control | 26.0–26.2% | 26.0–26.3% |
| Idle, SCREEN 5 control | 47.0–47.2% | 45.2–46.8% |
| `vdp_line`, SCREEN 7 control | 38.1–38.3% | 36.6–37.2% |
| Idle, SCREEN 7 control | 32.2–34.1% | 33.6–35.4% |
| `vdp_line`, SCREEN 8 control | 27.6–28.0% | 27.9–28.3% |
| Idle, SCREEN 8 control | 44.2–44.8% | 43.9–44.4% |

Control shares vary despite unchanged algorithms; notably SCREEN 5 idle
is lower in this run while its renderer share stays similar. Do not infer
that unaffected modes have an established speed gain or zero integration
cost from the near-neutral isolated controls. These short captures do not
establish the cause of the differences. Report them with the primary
rendering improvement.

The full firmware writes its frame in PSRAM; the isolated kernel uses
internal output RAM and excludes border/status work. Kernel savings and
full-renderer sample shares consequently have different scopes.

Two profiler-free SCREEN 6 windows per build: reference 59.922/59.911 FPS;
selected helper 59.943/59.924 FPS. Pooled rates are 59.916/59.933 FPS
(+0.029%), near the 60 Hz cap. This single run per build establishes no
general application-speed gain. Redraw signature increments equal frame
increments in this continuously changing workload; neither is panel FPS.

All four 1280×720 static LCD captures match exactly in decoded RGB.
Core-0 `present_task` remains 3.1–3.3% in both builds. Its idle share is
80.4–81.2% in reference and 79.0–81.1% in the selected build; these are
separate from the core-1 rendering measurements.

After testing, the board was restarted as MSX2 / 60 Hz / 512 KB and its
ordinary BASIC 2.1 / Disk BASIC prompt was visually verified. No synthetic
guest program is running and no guest disks or cartridges are mounted.
The board keeps the selected uninstrumented helper app with the isolated
test disk namespace; production source/builds keep the normal namespace.

## Decision and next target

Retain the separate helper: substantial SCREEN 6 cost reduction, exact
pixels, smaller P4 text, and no stack-frame growth in other modes. Record
the additional 16 stack bytes only while SCREEN 6's helper is executing,
along with the integration control observations above. The initial inlined
variant is not retained.

Next investigate effective sixteen-color row palettes for SCREEN 5/7,
using the same backdrop/TP rules, paired-bank behavior and controls.
Measure them independently; a larger row palette has a different setup
cost. VDP command fill/copy workloads remain a separate profiling target.

Synthetic tests do not establish game/application performance or complete
compatibility. Physical input and speaker/audio checks remain open. P4
timing does not predict S3 runtime gains. Scratch source snapshots,
fixtures, raw logs and firmware remain outside git under
`/tmp/msx-palette-2026-11-07`.
