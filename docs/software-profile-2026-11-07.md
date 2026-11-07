# MSX software validation and profiling — 2026-11-07

The retained VDP matches the original VDP across six real-software replays:
114 pictures, eighteen visible-memory/VRAM snapshots and six supported-audio
captures. Gameplay in the two MSX2 titles uses SCREEN 4 tiles/sprites,
not the previously optimized bitmap converters, and issues no VDP commands
after frame 3,000 in these replays. Defer further command specialization;
measure SCREEN 4 tile expansion and sprites next. No production code changes
are retained from this software follow-up.

## Builds and sources

Original VDP: esp32msx `c486454`, before the bitmap bank/palette and packed-byte
command optimizations. Retained VDP: `5fc331a`, also the unchanged production
source at documentation commit `0600971`; shared runtime `fcbeab2`.
The host original uses the current machine/host/runtime sources with only
`vdp.c` replaced by the original version, so this comparison isolates the
retained VDP changes. The additional counter build uses a temporary copy
of the retained source and is checked separately for identical output.

The P4 comparison reuses the exact original/retained test app and ELF
artifacts saved by the earlier trials. Their saved VDP sources match the
stated commits. Both apps use isolated disk namespace `msxvidtest` and
normal component `-O2`/jump-table flags, ESP-IDF 5.5.6 / GCC 14.2. Tab5 runs
at 360 MHz with 200 MHz PSRAM. ROM partitions remain unchanged; only the
app is switched. Production source/builds retain `msxdisks`.

The board's MSX image inventory was empty. Games are downloaded from their
official distribution pages into scratch storage; no user software images
are modified, and no game binaries or artwork are committed:

- [Lilly’s Saga 1.2](https://www.msxdev.org/2022/10/05/msxdev22-31-lillys-saga-the-stones-of-evergreen/),
  Fabulous 8bit: the official page lists MSX2 and MSX-Music requirements.
  The emulator reaches its intro and gameplay, but implements no MSX-Music;
  FM music and complete sound compatibility are not validated.
- [ΧΙ∧∃Χ / Zevimodoki 1.10](https://www.msxdev.org/2017/03/22/msxdev17-3rd-entry-%CF%87%CE%B9%E2%88%A7%E2%88%83%CF%87/),
  GW’s Workshop: use the disk supplied with the official archive.
- [The Menace from Triton 1.2](https://github.com/santiontanon/triton/releases/tag/v1.2)
  and [Tales of Popolon 1.3.1](https://github.com/santiontanon/talesofpopolon/releases/tag/v1.3.1),
  Santiago Ontañón: English cartridge releases, tested as MSX1 controls
  and on MSX2.

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| lilly.rom | 1245184 | `8a0bcad0f800f2fd309b4b68230abcd187f4842623b62238bb759d2253656e92` |
| zevi.rom | 65536 | `f176881b41339b02fd7c6922c72624d04aa7af5dd06f07aa732537b17fd771b5` |
| zevi.dsk | 737280 | `1dcc76a6659152a45446a82b867e5b54366f84d584b16b0d7ddda63f55490d2c` |
| triton.rom | 49152 | `5af58afabbb0d527756e2b1277ac263b58699e8a7030be9f5d67612a25cb6d4d` |
| popolon.rom | 32768 | `711a6b69f7d57549573af3ee308868ba58c292cc5ca3484875e33c8a13b3f630` |

Zevimodoki's 64 KB cartridge returns to BASIC in both original and retained
host builds, with identical final pictures. This is a pre-existing boot
limitation, not a retained-optimization regression. Its disk version boots
and reaches gameplay. Cartridge placement/mapper investigation remains a
separate compatibility task; no loader change is made here.

## Deterministic host comparison

Six cases: Lilly/MSX2, Zevimodoki disk/MSX2, Triton/MSX1 and MSX2,
Popolon/MSX1 and MSX2. All use 60 Hz; MSX2 uses 512 KB mapper RAM, MSX1
64 KB. Cartridge cases omit the disk ROM; Zevimodoki enables it. Each
runs 9,001 emulated frames, with the same scripted keyboard/joystick input
at emulated-frame boundaries. Host `time()` is interposed at fixed epoch 1794052800
and timezone UTC to remove RTC variation; guest instruction/device timing
is unchanged. This host-only clock treatment is not used on the board.

The final input schedule reaches Lilly gameplay without repeatedly pressing
SPACE/trigger B, which can pause it. Zevimodoki and Popolon reach gameplay;
Triton's tested sequence covers its introduction and galaxy-map menu,
not an active shooting mission. Results do not establish complete game
compatibility or later-level coverage.

- Eighteen pictures per case at frames 500, 1000, ... 9000, plus one final
  picture: 114 original/retained pairs, all byte-identical.
- At frames 3000, 6000 and 9000: identical 64 KB visible CPU address-space
  dumps and 128 KB logical VRAM dumps, eighteen paired checkpoints. These
  are not dumps of every bank of the 512 KB mapper. VDP registers 0–23,
  status 0–2 and periodic PC/slot/mode/status logs also match.
- Six byte-identical WAV files, each containing 19,202,988 PCM bytes and
  nonzero samples. This validates the supported sound path; Lilly's missing
  FM implementation and physical speaker output remain outside coverage.
- The working disk is freshly copied for each build/run. Original, retained
  and counter-build disk copies match each other and their source after
  execution; all downloaded ROM/disk/archive hashes remain unchanged.

The temporary host counter build also matches all 114 pictures, memory/
status logs and six WAVs against retained output. It records mode-specific
active converted rows, command launches and pixel operations, split before
and after frame 3000. Those counts are workload evidence, not host or P4
execution-time measurements. No instrumentation enters production source
or either board firmware.

| Case | Active rows at frames 3000–9000 | VDP commands / pixel operations in that interval |
| --- | ---: | ---: |
| Lilly / MSX2 | 1,140,190, SCREEN 4 | 0 / 0 |
| Zevimodoki / MSX2 | 1,143,821, SCREEN 4 | 0 / 0 |
| Triton / MSX1 | 289,895, SCREEN 2 | 0 / 0 |
| Triton / MSX2 | 1,152,192, SCREEN 2 | 0 / 0 |
| Popolon / MSX1 | 514,546, SCREEN 2 | 0 / 0 |
| Popolon / MSX2 | 517,136, SCREEN 2 | 0 / 0 |

These are rows actually converted after display/dirty gating, not all guest
scanlines. Different model counts do not imply performance differences.
In Lilly's first 3000 frames, 354,113 converted rows are SCREEN 5, and
there are twenty HMMV and 43 HMMM launches; all of those command launches
precede the measured gameplay interval. Thus the retained changes have
eligible startup/presentation work, but no gameplay speedup follows from
the earlier kernel percentages.

## Tab5 gameplay comparison

Only Lilly is profiled on the board in this pass. A temporary cartridge
copy is uploaded, then original and retained apps are flashed in sequence.
Both boot MSX2 / 60 Hz / 512 KB with the disk ROM present. The debugger's
reset/stop and frame-running commands reproduce the initial input schedule
to frame 3300, then release all controls. Gameplay at the first-level
starting area is visually verified; the subsequent measurements observe
that animated, stationary-player scene. No synthetic guest loop is injected.
The board's real RTC is retained; host and board boot configurations differ,
so no exact host-to-board state equivalence is asserted.

Two profiler-free windows of about fifteen seconds measure emulated frame
and redraw-counter rates. Three 32,000-sample profiles per core use nominal
20 kHz sampling (50 microseconds), resolved against the exact flashed ELF.
All twelve original/retained captures have the requested count/core label.
Shares include idle and are short-run observed ranges, not direct function
times or confidence intervals.

| Observed share | Original | Retained |
| --- | ---: | ---: |
| Core 1 `vdp_line` | 27.9–28.6% | 28.4–29.2% |
| Core 1 `draw_sprites` | 12.4–13.0% | 12.7–12.8% |
| Core 1 combined renderer | 40.5–41.2% | 41.2–41.9% |
| Core 1 idle | 34.8–35.4% | 33.0–34.6% |
| Core 1 `z80_step` | 8.7–8.9% | 8.9–9.2% |
| Core 1 `exec_op` | 6.0–6.4% | 5.8–6.1% |
| Core 0 `present_task` | 3.0–3.2% | 3.0–3.2% |
| Core 0 idle | 80.1–81.3% | 80.6–80.8% |

Combined renderer is `vdp_line` plus `draw_sprites` and any bitmap helpers,
summed per capture rather than independent range endpoints. Neither bitmap
helper appears in the reported gameplay samples. The similar scene is
SCREEN 4, whose algorithm is unchanged by the retained bitmap/command work.
Its retained renderer share is slightly higher and core-1 idle lower in
this run. This is a control observation, not proof of zero integration
cost or a quantified application slowdown; the short runs do not isolate
its cause. Retain this tradeoff alongside the earlier bitmap savings.

Profiler-free original FPS windows are 59.901/59.955; retained windows are
59.966/59.952. Pooled rates are 59.928/59.959 FPS (+0.051%), near the 60 Hz
cap. A one-frame difference over about thirty seconds does not establish
a gameplay speedup. Frame/redraw increments are 1801/1801 in original and
1802/1802 in retained; these counters do not measure panel FPS.

At the stopped gameplay entry, the 576×480 machine-screen PNGs match exactly
in decoded RGB and the 64 KB visible CPU memory dumps are byte-identical.
End-of-run 1280×720 LCD captures visually confirm the active first level;
they occur at different guest frames and are not pixel-aligned comparisons.
Together with the host counters, the profiles favor investigating tiles
and sprites over more command specialization. The result applies to this
stationary-player first-level scene, not every game phase.


After profiling, the temporary cartridge's downloaded board copy is
verified byte-for-byte against its source, ejected and deleted. The board
image inventory returns to its original empty state. Tab5 is restarted as
MSX2 / 60 Hz / 512 KB, and its ordinary BASIC 2.1 / Disk BASIC prompt is
visually verified. The retained, uninstrumented app remains installed;
no cartridge, disk or synthetic guest program remains mounted/running.

## Decision and next target

Keep the previously retained VDP changes; this pass finds no output or
supported-audio difference in its bounded host replays. It does not prove
full compatibility, a general application-speed gain or S3 runtime gains.
No shared source changes, new firmware implementation, or sibling build
matrix are needed for this validation-only pass. Existing S3/P4 production
build coverage is unchanged.

The next measured optimization target is SCREEN 4 tile expansion
(`draw_tiles` / `pattern8`, folded into `vdp_line`) and sprite work
(`draw_sprites`). Start with pattern expansion before changing sprite
priority/collision semantics. Retain this real-software replay and the
synthetic bitmap/command controls. Further VDP command specialization is
deferred until another real workload demonstrates that cost.

Zevimodoki cartridge boot, later game stages, MSX-Music, physical input,
speaker output and S3 hardware measurements remain open. Downloaded games,
source snapshots, replays, images, audio and raw logs stay outside git under
`/tmp/msx-software-2026-11-07`; the prior full Amiga flash backup is retained.
