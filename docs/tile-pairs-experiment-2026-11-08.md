# MSX doubled tile pixels — 2026-11-07–08

## Implementation and scope

Reference: esp32msx `9e2e8f7`, shared runtime `000c8f9`. This follows
[real-software profiling](software-profile-2026-11-07.md), which identifies
SCREEN 4 tiles and sprites as gameplay work. Only `components/msx/vdp.c`
changes; no shared runtime, assembly, VRAM mapping or command changes.
Tab5/P4 uses 360 MHz CPU, 200 MHz PSRAM, ESP-IDF 5.5.6 / GCC 14.2 and
normal component optimization flags.

Both `pattern8` callers expand each source bit into two identical RGB565
pixels. The candidate combines those halfwords into one 32-bit value and
uses `memcpy` for alias-safe stores. An explicit word-alignment check keeps
a halfword fallback. Normal framebuffer rows are word-aligned: allocation
is aligned, the 576-pixel stride is 1152 bytes, the border is 64 bytes and
tile increments are 32 bytes. No widened VRAM reads occur.

The first variant keeps the helper inline with an ordinary loop. It saves
less than 1% for captured SCREEN 4 rows and regresses several other tile
cases. The selected second variant outlines the helper and explicitly
unrolls its eight aligned stores. It trades code and common stack space
for lower measured tile cost. No lookup table, allocation or new state.

Exact linked P4 apps: reference `vdp_line` is 1996 text bytes with a
48-byte frame; selected `vdp_line` is 1994 bytes with an 80-byte frame,
plus a 316-byte stackless `pattern8`. Combined text grows 314 bytes;
the common line frame grows 32 bytes. `draw_sprites` remains 1304 bytes
and VDP state 696 bytes. The first linked variant grows line text by
144 bytes and its frame by 16. Component-object sizes differ because of
link relaxation and are not mixed into these comparisons. The emulation
task stack is 8192 bytes; runtime high-water is not measured.

## Correctness and builds

Actual-source reference/candidate wrappers pass ASan/UBSan:

- 524,288 tile-row/canary comparisons: two model flags, four tile modes,
  128 register/palette configurations, 256 rows and two output alignments.
- 33,554,432 independent pattern-oracle cases: both alignments, all 256
  pattern bytes and all 65,536 foreground values, with deterministic random
  background values. Complete output and surrounding canaries are checked;
  this is not exhaustive over every foreground/background pair.
- The selected variant also passes 262,144 bitmap pixel/canary controls
  spanning all four bitmap modes and relevant masks/pages/transparency.

Six real-software host replays use the preceding report's official copied
releases and deterministic inputs: Lilly and Zevimodoki disk on MSX2,
Triton and Popolon on MSX1/MSX2. All 114 picture pairs, eighteen visible
64 KB CPU / logical 128 KB VRAM snapshots, periodic state logs and six
supported-audio WAVs match. Lilly, Zevimodoki and Popolon reach gameplay;
Triton covers intro/galaxy menu. MSX-Music remains unsupported.

Selected host, isolated P4 test app and normal S3/P4 builds pass. Normal
app storage namespace remains `msxdisks`; test firmware uses isolated
settings. S3 runtime is unmeasured. No shared production change requires a
sibling-emulator build matrix.

## Isolated P4 costs

Each case uses seven ABBA rounds, four passes of 64 rows per timed call,
and median paired cycles per row. Rows follow `(i * 37) & 255`; a volatile
checksum prevents removal. Actual-source wrappers share initialized 128 KB
PSRAM VRAM; output is internal RAM or PSRAM. Captured data is Lilly's frame
6000 VRAM and registers, with a fixed-seed random RGB565 palette rather
than the game's palette. Guest CPU, LCD, audio and network are absent.
Interrupts remain enabled. Negative change means lower execution cost.

| Row workload / output | Reference cycles | Selected cycles | First change | Selected change |
| --- | ---: | ---: | ---: | ---: |
| SCREEN 4 captured / internal word | 4084.4 | 3059.6 | -0.87% | -25.09% |
| SCREEN 4 captured / PSRAM word | 4084.2 | 3060.1 | -0.86% | -25.07% |
| SCREEN 4 captured / internal halfword | 4085.8 | 4574.6 | +2.88% | +11.96% |
| SCREEN 4 captured / PSRAM halfword | 4085.3 | 4572.0 | +2.86% | +11.91% |
| SCREEN 4 random / internal word | 4430.9 | 3659.3 | +1.47% | -17.41% |
| SCREEN 4 zero / internal word | 4006.3 | 2953.9 | -0.43% | -26.27% |
| SCREEN 4 FF / internal word | 4220.2 | 3361.2 | -2.28% | -20.35% |
| SCREEN 2 MSX1 captured / internal word | 3882.9 | 3185.5 | +8.37% | -17.96% |
| SCREEN 2 MSX2 captured / PSRAM word | 4084.8 | 3060.3 | -0.81% | -25.08% |
| SCREEN 1 MSX1 captured / internal word | 3956.8 | 3034.8 | +5.39% | -23.30% |
| Multicolor control | 2817.7 | 2822.2 | +0.97% | +0.16% |
| SCREEN 5 control | 2388.0 | 2393.1 | +0.17% | +0.21% |
| SCREEN 6 control | 3401.4 | 3402.9 | +0.01% | +0.04% |
| SCREEN 7 control | 3954.2 | 3952.3 | -0.06% | -0.05% |
| SCREEN 8 control | 2897.1 | 2891.7 | -0.23% | -0.19% |

Both benchmark variants end with checksum 822622784. The selected aligned
tile cases save 17.4–26.3%; unchanged mode controls stay within 0.22%.
The halfword-only fallback costs about 12% more. That fallback is correct
but does not describe the normal aligned framebuffer. These kernel results
are one firmware run per variant, not confidence intervals or whole-app
speedups. Compiler and layout effects are included.

## Full firmware validation

An initial selected-app run produced a stale whole-LCD image while guest
frames continued. Core-0 profiles lacked `present_task`. Its gameplay and
bitmap timing results are invalid for a presentation-enabled comparison
and were preserved separately in temporary trial artifacts. Passing guest
pixel checks does not validate LCD presentation. A fresh selected-app boot
to BASIC displays correctly; the repeat loads the temporary game after
that check and verifies LCD activity before timing. The cause of the first
run is not established.

The repeat uses the preceding Lilly first-level input schedule to frame
3300, then releases controls. Stopped 576×480 guest pictures and visible
64 KB CPU memory match exactly. Whole-LCD captures verify gameplay before
and after measurement. Two profiler-free ~15-second windows precede three
32,000-sample captures per core at nominal 20 kHz, resolved against each
exact flashed ELF. All twelve retained captures have the requested sample
count and core label. Shares include idle and are observed short-run
ranges, not confidence intervals or isolated function execution times.

| Observed share | Reference | Selected repeat |
| --- | ---: | ---: |
| Core 1 `vdp_line` | 28.9–29.0% | 20.6–20.7% |
| Core 1 `pattern8` | inlined | 5.7–5.9% |
| Core 1 `draw_sprites` | 12.4–13.1% | 12.2–12.6% |
| Core 1 combined renderer | 41.3–42.1% | 38.8–39.1% |
| Core 1 idle | 34.2–34.7% | 35.3–37.0% |
| Core 1 `z80_step` | 8.6–8.9% | 8.7–8.8% |
| Core 1 `exec_op` | 6.1–6.2% | 6.0–6.3% |
| Core 0 `present_task` | 3.1–3.3% | 3.1–3.3% |
| Core 0 idle | 79.8–81.1% | 79.9–80.6% |

Combined renderer sums `vdp_line`, `pattern8`, sprites and any bitmap
helpers per capture. Outlining alone therefore does not count as savings.
The repeat supports reduced rendering work in this scene, alongside the
isolated kernel results. It does not establish a speedup for every game,
level or mode, nor an exact causal attribution for every sample difference.

Reference FPS windows are 59.965/59.893, selected 59.960/59.891; pooled
rates are 59.929 and 59.925. Each run advances 1801 frames and 1801 redraw
signatures over about thirty seconds. Both are capped near 60 Hz, with no
established application FPS improvement. Signatures do not measure panel
FPS. There is no physical-control or speaker acceptance test.

After ejecting the copied cartridge, generated SCREEN 6 and SCREEN 8
controls initialize all sixteen palette entries and write 128 KB through
real CPU VDP ports. A repeating write loop drives rendering for three
32,000-sample core-1 captures per mode/build. Separate static loops give
exactly matching decoded 1280×720 LCD images in both modes. The candidate
control repeat uses the working presenter session; the original candidate
control captures with stale LCD are excluded too.

| Control | Reference renderer | Selected renderer | Reference idle | Selected idle |
| --- | ---: | ---: | ---: | ---: |
| SCREEN 6 | 28.0–28.4% | 27.5–28.1% | 44.7–44.9% | 44.4–45.5% |
| SCREEN 8 | 28.4–28.6% | 28.1–28.5% | 43.7–44.3% | 41.9–43.9% |

These controls show no consistent renderer regression. SCREEN 8 includes
one lower-idle capture without higher renderer share; the short profiles
do not isolate that variation. They are not profiler-free throughput
measurements. All twelve control captures have the requested count/core.

A further app flash/restart with the cartridge already mounted displays
Lilly's intro correctly too. Thus both clean BASIC-to-cartridge startup
and mounted-cartridge startup have a working repeat. This does not identify
or fix the initial intermittent presenter failure; preserve it as an open
integration observation rather than attributing it to a particular cause.

## Decision and next target

Retain the second, outlined/unrolled variant. Its aligned kernel gains and
valid real-game renderer reduction justify the measured code/stack cost.
The first inline variant is discarded. Keep the halfword fallback penalty
and unresolved initial presenter observation visible; S3 needs its own
runtime measurement before claiming the same benefit there.

Next investigate `draw_sprites`, still about 12–13% of core 1 in Lilly.
Measure attribute scanning, pattern expansion and final 256-pixel composite
separately before choosing a rewrite. Tests must cover early-clock clipping,
magnification, sprite priority, color-combine groups, overflow index and
collision/status effects, including transparent color zero and bitmap-mode
palette differences. No sprite algorithm change is part of this trial.

The temporary cartridge is verified byte-for-byte, ejected and deleted;
the isolated disk inventory is empty. The normal `msxdisks` P4 app is
flashed, MSX2 / 60 Hz / 512 KB restored, and its whole LCD visually checked
at ordinary BASIC. Normal S3/P4 binaries both contain the normal storage
namespace. Scratch media, firmware and raw measurements stay outside git.
