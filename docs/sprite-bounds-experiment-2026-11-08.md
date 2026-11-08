# MSX sprite composite bounds — 2026-11-08

## Reference and candidate

Reference: esp32msx `2a845bd`, shared runtime `7eae154`. This follows the
[doubled tile trial](tile-pairs-experiment-2026-11-08.md); its Lilly profiles
still attribute about 12–13% of core 1 to `draw_sprites`. The sprite candidate changes only the MSX VDP. Tab5/P4: 360 MHz CPU, 200 MHz PSRAM, ESP-IDF 5.5.6 / GCC 14.2,
normal component optimization flags. No assembly is added. Both full-app builds also include the shared
display correctness fixes described below.

The renderer first accumulates sprite colors, ownership and collision
metadata in a temporary pixel array. It then scans all 256 positions to
composite visible colors onto the output. The candidate tracks the union
of processed sprites' clipped horizontal spans and bounds that last pass.
It still clears the full temporary pixel array and performs all original
attribute scanning, pixel expansion, priority and collision processing.
Bounds are conservative: transparent or empty patterns may extend them.
A color-combine sprite ignored before any base group does not extend them.
All-left-of-screen sprites leave an empty interval safely.

The linked P4 `draw_sprites` grows from 1304 to 1366 text bytes (+62).
Its stack frame remains 688 bytes. `vdp_line` stays 1994 bytes with an
80-byte frame and `pattern8` stays 316 bytes as a stackless leaf. VDP state
remains 696 bytes; no new persistent state, heap allocation or VRAM read.
These are exact linked apps, avoiding mixed object/link size accounting.

A separate native diagnostic over all 256 captured Lilly rows records
7952 SAT Y reads, 336 eligible entries and 96 rows reaching the final
composite. Final visits fall from 24,576 to 18,288, with 1071 occupied and
visible output positions. Its pixels/status match the reference. These
are operation counts, not phase timings; the timed firmware contains no
counter instrumentation.

## Correctness and build coverage

Actual-source wrappers pass ASan/UBSan:

- 1,310,720 row/pixel/status/canary comparisons: two model flags, ten modes,
  sixteen size/magnification/transparency/disable configurations, 256 rows,
  both word and halfword output alignments, and eight VRAM fills (zero,
  FF and six deterministic random buffers). All ten status bytes match.
- 344,064 independent single-sprite pixel/status/canary oracle cases cover
  size 8/16, normal/doubled magnification, early-clock clipping, X edges,
  Y wrap at 255, pattern halves, transparent color zero, orphan combine,
  collision inhibit and normal / SCREEN 6 / SCREEN 8 color selection.
- 381,312 independent interaction/overflow pixel/status oracle cases cover
  two overlapping sprites, priority, color combine/inhibit/early-clock
  combinations, three colors including zero, transparency and sprite
  disable, plus all end-marker positions, fifth/ninth-sprite overflow
  indices and preservation of an already latched overflow/status value.
  Both output alignments occur; the single-sprite suite tests each case
  at both alignments.

These preserve existing emulator semantics rather than establishing new
TMS9918A/V9938 hardware accuracy. All source VRAM is shared/read-only within
a comparison; complete output and surrounding canaries are checked.

Six deterministic official-software host replay pairs also pass: Lilly
and Zevimodoki disk on MSX2, Triton and Popolon on MSX1/MSX2. All 114 picture
pairs, eighteen visible CPU-memory/logical-VRAM checkpoints, periodic logs
and six supported-audio WAVs match. Each run covers 9001 frames using the
preceding software report's fixed host clock and input scripts. Lilly,
Zevimodoki and Popolon reach gameplay; Triton covers intro/galaxy menu.
MSX-Music remains unsupported; no later-level coverage is claimed.

The full candidate P4 test app, host executable and normal S3/P4 apps
build successfully. S3 runtime is unmeasured. The two shared display fixes also pass all 22 sibling builds, making
24 successful board/project builds including MSX. The test app uses
isolated `msxvidtest` disk settings; normal source uses `msxdisks`.

## Isolated P4 measurements

A minimal firmware contains actual-source reference/candidate wrappers and
one shared 128 KB PSRAM VRAM buffer. Each timed call executes four passes
of 64 rows, `(i * 37) & 255`, with a volatile output checksum. Seven ABBA
rounds alternate reference/candidate/candidate/reference. Report medians
of paired cycles per row and paired relative changes. Interrupts remain
enabled; guest CPU, LCD, audio and network are absent.

Captured data is Lilly's frame-6000 VRAM and 24 registers from the preceding
trial, with a deterministic random RGB565 palette rather than the game's
palette. The MSX1 SCREEN 2 captured case reinterprets that MSX2 data; it is
not a separate captured MSX1 game. Synthetic lists use 16×16 all-set
patterns at Y=80, with early-clock/orphan combine, spread-out positions,
end markers and overflow as indicated. Full output background is initialized
once per case; status accumulates equally through warmups and timed calls.
Negative change means lower cost. This is one firmware run with repeated
paired rounds, not confidence intervals or whole-app throughput.

| Sprite workload / output | Reference cycles | Candidate cycles | Change |
| --- | ---: | ---: | ---: |
| SCREEN 4 captured / internal word | 2258.75 | 2020.45 | -10.55% |
| SCREEN 4 captured / PSRAM word | 2258.70 | 2020.37 | -10.55% |
| SCREEN 4 captured / internal halfword | 2258.57 | 2020.31 | -10.55% |
| SCREEN 2 MSX1 / reinterpreted captured data | 1080.85 | 857.75 | -20.64% |
| Empty list / end marker | 264.58 | 268.39 | +1.44% |
| One sprite / normal | 517.01 | 347.43 | -32.80% |
| One sprite / magnified | 646.49 | 432.17 | -33.15% |
| Eight spread sprites / normal | 982.29 | 969.95 | -1.27% |
| Eight spread sprites / magnified | 1526.44 | 1521.52 | -0.33% |
| Overflow list / spread sprites | 1358.05 | 1297.15 | -4.49% |
| Eight orphan combine sprites / early clock | 616.31 | 424.87 | -31.06% |
| SCREEN 2 MSX1 / one sprite | 496.30 | 326.44 | -34.23% |
| SCREEN 6 / one sprite | 529.45 | 359.67 | -32.07% |
| SCREEN 8 / one sprite | 538.00 | 362.61 | -32.60% |
| Sprites disabled control | 49.40 | 48.27 | -2.21% |
| Text mode control | 18.36 | 18.23 | -0.71% |

The benchmark ends with checksum 1383602120. Captured SCREEN 4 costs about
10.5% less; sparse single-sprite controls save 31–34%. Spread/magnified
sprites covering most/all of the width gain little. The empty-list control
regresses about 1.4% (3.8 cycles/row). Disabled/text early-return differences
are small and do not demonstrate a changed rendering algorithm there.
Compiler and layout effects are included in all observed changes.

## Full firmware checks

Only Lilly is used for fresh gameplay board measurements. Both apps run
MSX2 / 60 Hz / 512 KB with the disk ROM, using a temporary cartridge copy
and the same debugger reset/stop and frame/input schedule to frame 3300.
Controls are then released. Whole-LCD output is checked before timing.
Two profiler-free ~15-second frame/redraw windows precede three nominal
20 kHz, 32,000-sample captures on each core, resolved against the exact
flashed ELF. The guest's real RTC remains active; no exact host-to-board
state equivalence is asserted.

Bitmap correctness controls use fresh candidate SCREEN 6/8 captures and
the preceding tile trial's decoded reference image goldens. Those goldens
have identical deterministic palettes/VRAM and are used only for pixels;
old control timings are not compared across the runtime fixes.

Earlier unpatched gameplay attempts are superseded after diagnosing a
PPA completion wait with JTAG. A repeat also lacks presenter samples even
while guest redraws continue, so it is invalid as an LCD-on comparison.
Some other initial preview interpretations of missing image regions are
wrong: decoded PNG pixels contain the sky/scene. They are not evidence of
corruption. Diagnostic pixel samples match expected scaling/copy output
before and after PPA submission. Preserve the real task-stall evidence
separately from those rejected preview interpretations.

Full-app comparison is restarted with the same shared display fixes in
both reference and candidate: Tab5 flush geometry avoids small rotated DMA
tails; presenter hashes now describe the actual scaled source read. See
[display reliability](../../esp32emu/docs/display-reliability-2026-11-08.md)
for the JTAG evidence, upstream PPA report and actual-source regression
harnesses. The hash race is reproduced natively, not proved as the cause of
a particular captured board image. No SDK files are modified.

Artifact validation forces a fresh source modification time before each
VDP build and verifies a `vdp.c` compile plus linked routine sizes. An
earlier copied baseline source kept its old timestamp, leaving a candidate
object in the nominal reference app; its full-app measurements are discarded.
The valid full-app ELF SHA-256 values are:

- reference: `82cf1a78b6758f2bfa3ec8a2665648e543401323b5cfc57db310dfde76358b58`
- candidate: `8762731aec315008cd8cb319781773b05a37cab9f40891ae666dec89ceba5aba`

The verified full-app run gives the following ranges across three captures
per core. Shares are rounded profiler percentages, not confidence intervals.

| Metric | Reference | Candidate |
| --- | ---: | ---: |
| Emulated FPS, two profiler-free windows | 59.89–59.94 | 59.89–59.94 |
| Core 1 sprite share | 12.5–12.6% | 12.3–12.5% |
| Core 1 combined renderer share | 38.4–38.8% | 37.7–38.5% |
| Core 1 idle | 37.5–37.6% | 36.4–38.2% |
| Core 0 presenter share | 3.2% | 3.1–3.3% |
| Core 0 idle | 80.9–82.1% | 81.1–81.7% |

Both apps advance 1801 guest frames and 1801 redraw signatures during their
~30.06-second windows. A redraw signature counts guest redraws, not panel
FPS. Every core profile contains the requested 32,000 samples and the
presenter remains active in all six core-0 captures. Whole-LCD PNGs decode
to a complete scene. At frame 3300, the guest picture and all sixteen 4 KB
visible-memory blocks match exactly between builds.

Retain the bounded pass for its isolated captured/sparse-row savings and
small code cost. The full-game result is modest and overlapping: it does
not establish a statistically significant whole-emulator speedup, and no
FPS increase occurs at the real-time cap. The game continues changing its
sprite positions after the common start point; the live capture is not
the same workload as the isolated frame-6000 kernel. Broad sprite coverage
and the empty-list regression remain relevant tradeoffs. S3 performance
requires measurements on an attached S3 board.

Fresh candidate SCREEN 6 and SCREEN 8 static whole-LCD captures each match
the preceding trial's reference golden at all 1280×720 decoded RGB pixels.
Their new core-1 profiles also contain all requested samples; only pixels
are compared with the old-runtime controls.

After testing, the owned temporary cartridge is ejected and deleted.
The normal `msxdisks` app is flashed and restored to MSX2 / 60 Hz / 512 KB.
The final guest capture and decoded 1280×720 LCD picture show Disk BASIC
at its `Ok` prompt, with guest frames continuing at 60 FPS. No test hooks,
profiler counters or isolated disk namespace remain in production source.
