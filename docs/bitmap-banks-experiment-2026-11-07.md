# MSX2 paired VRAM bank reads — 2026-11-07

This trial reads adjacent logical bytes as a pair in SCREEN 7/8 bitmap
rendering. Isolated P4 bitmap row cost falls 18.5–21.8% in SCREEN 7 and
about 38.5% in SCREEN 8. Full-firmware observations and retention decision
are recorded below; these kernel results are not application speedups.

## Reference and change

Reference: esp32msx `c486454`, shared runtime `bc8592a`. Only local
`components/msx/vdp.c` changes. Tests use Tab5/P4 at 360 MHz, 32 MB PSRAM
at 200 MHz, ESP-IDF 5.5.6 / GCC 14.2 and the component's normal `-O2`
with jump tables enabled. No shared-runtime or Z80 source changes.

In V9938 G6/G7 (MSX BASIC SCREEN 7/8), consecutive logical bytes alternate
between two physical 64 KB VRAM banks. A row is 256 logical bytes, starting
at a 256-byte boundary, so its two streams each advance 128 physical
bytes without crossing a bank end. Map the row base once, then read bank
0 / bank 1 at matching offsets. This removes repeated physical-address
mapping and halves loop iterations. The existing masked byte accessor
remains in use, with each byte read before its pixels are written.

SCREEN 7 keeps the exact nibble-to-palette and transparency/backdrop
rules. SCREEN 8 keeps the RGB332 lookup and doubled output pixels. The
SCREEN 5/6 source branches, sprites, scrolling, VDP commands, status flags,
Z80 waits and dirty-frame skipping are unchanged. There is no assembly.

The inlined P4 `vdp_line` grows from 2,670 to 3,068 bytes (+398). The VDP
state remains 696 bytes at the same address; there are no new tables,
allocations or internal-RAM requirements.

## Correctness and builds

- Actual-source reference/candidate bitmap wrappers pass 262,144 complete
  pixel/canary comparisons under ASan and UBSan. They cover modes G4–G7,
  all 256 source rows, page bits, sixteen backdrop indices, transparency
  on/off, varied palettes and both 16 KB and 128 KB masks. The 16 KB cases
  defensively exercise the helper; MSX1 does not select MSX2 bitmap modes.
- An independent scalar address calculation checks all 262,144 combinations
  of both masks, two page choices, every row and every byte against paired
  bank addressing. Boundary rows/pages and bank wrapping match.
- Ten host pictures match exactly: MSX1/MSX2 BASIC plus static/animated
  generated cartridges for SCREEN 5, 6, 7 and 8, each after 500 frames.
  The cartridges initialize all 128 KB through the actual CPU VDP ports,
  select a vertical scroll of 37, disable sprites and exercise palette
  zero/backdrop behavior. Final pictures were visually checked to confirm
  the cartridges reached their intended bitmap modes.
- Candidate host, S3 and P4 test builds pass, as does the normal-production
  P4 build. S3 hardware is not connected: its runtime timing, LCD, input
  and audio are unmeasured. Both target builds share managed dependencies
  and must run sequentially in this checkout; an overlapping build failed
  when the target-specific dependency set changed, then passed sequentially.

Production main retains `msxdisks`; temporary board apps use an isolated
`msxvidtest` disk namespace. No guest disks or user cartridges are mounted.
A full 16 MB Amiga flash backup was verified by size and recorded SHA-256
before switching projects. ROMs and backups remain outside git. No shared
source changes mean the 24-sibling build matrix is unnecessary.

## Isolated P4 timing

Both actual-source bitmap implementations run in one minimal firmware.
VRAM is a shared 128 KB PSRAM allocation; lookup state and output are in
internal RAM. Each pass draws 64 rows, in `(row * 37) & 255` order, four
times. Seven ABBA rounds alternate reference/candidate/candidate/reference,
with a volatile checksum. Values are medians of paired-round row costs.
Interrupts remain enabled; guest CPU, LCD, audio and networking are absent.
These costs exclude sprites, borders and scaling.

| Case | Reference cycles/row | Candidate cycles/row | Cost change |
| --- | ---: | ---: | ---: |
| SCREEN 5 mixed control | 3231 | 3106 | -3.85% |
| SCREEN 6 mixed control | 5601 | 5628 | +0.47% |
| SCREEN 7 mixed TP=0 | 6670 | 5424 | -18.68% |
| SCREEN 7 mixed TP=1 | 6685 | 5347 | -20.01% |
| SCREEN 8 mixed | 4686 | 2883 | -38.47% |
| SCREEN 7 zero | 6495 | 5081 | -21.76% |
| SCREEN 8 zero | 4688 | 2883 | -38.50% |
| SCREEN 7 FF | 6492 | 5213 | -19.70% |
| SCREEN 8 FF | 4686 | 2883 | -38.47% |
| SCREEN 7 page 1 background 15 | 6664 | 5431 | -18.51% |
| SCREEN 8 page 1 | 4686 | 2883 | -38.47% |
| SCREEN 7 page 1 TP=1 background 0 | 6669 | 5337 | -19.97% |

SCREEN 5/6 are unchanged-source controls. Their measured differences include
compiler layout effects and are checked again in full-firmware profiles.

## Full firmware

Synthetic Z80 programs run after an MSX2 / 60 Hz / 512 KB BASIC boot.
They configure SCREEN 5–8, disable interrupts/sprites, select 212 active
lines and vertical scroll 37, then initialize all 128 KB VRAM through the
CPU ports. Animated versions continuously write VRAM; static versions
finish initialization and loop in place for deterministic LCD capture.
The real VDP addressing, line drawing and dirty-frame path remain in use.

Three 32,000-sample captures per workload/core use nominal 20 kHz sampling
(50 microseconds), resolved against each exact flashed ELF. Core 1 is
sampled in all four modes; core 0 in SCREEN 7/8. All 36 captures have the
requested count/core label. Ranges below are observed sample shares,
including idle time, not isolated function times or confidence intervals.

| Core-1 function / mode | Reference share | Candidate share |
| --- | ---: | ---: |
| `vdp_line`, SCREEN 5 control | 25.3–25.7% | 25.8–26.2% |
| Idle, SCREEN 5 control | 45.4–47.4% | 45.3–47.1% |
| `vdp_line`, SCREEN 6 control | 33.7–33.8% | 34.3% |
| Idle, SCREEN 6 control | 38.7–39.3% | 38.8–39.1% |
| `vdp_line`, SCREEN 7 | 40.7–41.7% | 38.5–38.8% |
| Idle, SCREEN 7 | 29.1–30.5% | 31.9–34.2% |
| `vdp_line`, SCREEN 8 | 34.4–34.7% | 27.7–28.2% |
| Idle, SCREEN 8 | 36.3–37.8% | 44.0–44.5% |

`vdp_line` includes borders, dirty/status processing and bitmap conversion.
Full firmware also writes its framebuffer in PSRAM, whereas the isolated
benchmark uses internal output RAM; the two measurements have different
memory traffic and scope. The kernel percentage savings therefore do not
predict the full-function sample-share reduction.

The control functions' shares rise about half a percentage point, despite
unchanged source branches. Their idle ranges overlap, so no control speed
improvement or established whole-emulator regression is claimed. Report
both the small isolated SCREEN 6 cost and these integration observations.

Core-0 idle remains about 78–80% and `present_task` about 3.2–3.4% in the
two primary modes. These workloads are primarily an emulation-core target.

Two approximately 30-second profiler-free SCREEN 8 windows per build:
reference 59.903/59.932 FPS; candidate 59.929/59.939 FPS. Pooled rates are
59.918/59.934 FPS (+0.027%), near the 60 Hz cap. This one fresh run per
build establishes no application-speed gain. `vdp_signature()` increments
when a frame is redrawn: redraw rates equal these frame rates in this
always-changing workload, rather than measuring panel FPS. Static frames
retain the existing skipping behavior by unchanged control code.

All four 1280×720 static LCD captures match byte for byte in decoded RGB.
The synthetic picture is visually verified in each mode.

After measurement, the board was restarted as MSX2 / 60 Hz / 512 KB.
Its ordinary MSX BASIC 2.1 / Disk BASIC prompt was visually verified, with
60 Hz status. No synthetic guest program is running and no guest disks or
cartridges are mounted. The board keeps the uninstrumented candidate app
and isolated test disk namespace; production source/builds retain the
normal namespace. The pre-switch Amiga full-flash backup remains outside
git for restoration.

## Decision and limits

Retain for lower SCREEN 7/8 rendering cost and increased measured core-1
idle headroom, at a cost of 398 P4 text bytes and the small recorded
control differences. There is no new RAM allocation/table or guest timing
change. No panel-FPS or general game/application-speed increase is claimed.

Next investigate SCREEN 6's four-color expansion: compute the effective
zero/backdrop color once per row, then decode through four corrected
palette entries, preserving transparency and per-line palette changes.
Benchmark it independently with SCREEN 5/7/8 controls. VDP command fill/copy
workloads remain a separate profiling target.

Synthetic workloads do not establish game performance or compatibility.
Physical input and speaker/audio-by-ear checks remain open. P4 measurements
do not predict S3 runtime gains. Scratch fixtures, full logs, firmware and
backups remain outside git under `/tmp/msx-video-2026-11-07`.
