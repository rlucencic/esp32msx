# MSX2 logical pixel operations — 2026-11-07

Reject both tested packed-bit rewrites of `pset`. They pass exhaustive
correctness checks and reduce code size, but improve selected operations
at the expense of ordinary COPY/NOT paths. Restore the committed command
implementation. No production code is retained from this trial.

## Reference and candidates

Reference: esp32msx `5fc331a`, shared runtime `685b4aa`. It includes the
retained bitmap bank/palette and HMMV/HMMM stride changes. Tab5/P4 uses
360 MHz CPU, 200 MHz PSRAM, ESP-IDF 5.5.6 / GCC 14.2 and normal component
`-O2`/jump-table flags. No shared runtime or assembly changes.

The reference `pset` extracts the destination pixel, applies COPY, AND,
OR, XOR or NOT to it, then packs the result while preserving neighboring
pixels. The first candidate applies operations directly to the positioned
bits in the byte. Source color is still masked before transparency;
unsupported operation codes still store the unchanged byte, as before.
Read/write order and all coordinate/address code are preserved.

The revised form additionally delays physical address calculation until
after the transparency return. This avoids address work for transparent
zero writes, including colors that become zero after masking. No VRAM
access occurs on that early return in either reference or candidate.
Both variants preserve existing emulator semantics, not new V9938 accuracy.

The first linked P4 test app reduces `pset` from 478 to 422 text bytes;
`reg_write` remains 2412 bytes. Comparable production-flags P4 component
objects have a 478-byte reference `pset` versus 382 bytes in the revised
form; both have a 2452-byte `reg_write`, giving 96 fewer bytes combined.
The revised object is not a linked full app. Size accounting was corrected
on 2026-11-08: the earlier text mixed object and linked sizes, incorrectly
attributing a 40-byte difference to the revised register writer.
Both keep `pset` as a stackless leaf and the register-write frame at 128
bytes. No new state, cache or allocation; VDP state remains 696 bytes.
It is not safe to infer execution cost from helper size or instruction
removal alone.

## Correctness and build coverage

Both forms pass the same native checks under ASan/UBSan:

- 25,165,824 independent packed-byte/pixel oracle cases: every destination
  byte, source byte and sixteen operation codes at each of four pixel
  positions in all four bitmap modes and two nonbitmap-coordinate fallback
  setups. The oracle computes the original scalar pixel result and checks
  the complete packed byte and subsequent `point` result in both versions.
- 67,296 complete 128 KB VRAM/state/canary command comparisons, covering
  both logical operations with every operation code and four directions,
  aligned/unaligned X, overlap, clipping, partial/default widths, vertical
  wrap/long heights, byte controls, POINT/PSET/SRCH/LINE and fallback cases.
- 58,224 independent byte-command VRAM oracle comparisons within that
  command suite, ensuring the retained byte paths remain unchanged.
- 49,920 CPU-transfer VRAM/state/canary checkpoints across LMCM, LMMC and
  HMMC, all sixteen operation codes, four modes and four directions.
  Checks cover command start and 64 subsequent accesses, including transfer
  completion and later register/status accesses.

The first form also passes eighteen host-picture comparisons: MSX1/MSX2
BASIC and eight static/dynamic bitmap fixtures after 500 frames, plus
static/repeated generated HMMV/HMMM/LMMV/LMMM fixtures after 160 frames.
Its full P4 test app and both minimal benchmark firmwares build. The
revised form receives native tests, production-flags P4 object compilation
and a minimal P4 benchmark build; no full-app host/LCD comparison is
claimed for it. It is rejected before those integration tests.

Neither rejected form receives an S3 build or S3 timing. The restored
baseline was built on S3/P4 in the preceding retained trial. This trial
rebuilds its normal P4 app and host executable after restoration. Shared
source is unchanged, so no sibling build matrix is needed.

## Isolated P4 measurement

Each firmware contains actual-source reference/candidate wrappers and
separate identically initialized 128 KB PSRAM VRAM buffers. Each pass
executes four commands, resetting SY/DY/NY before every command. Seven
ABBA rounds alternate reference/candidate/candidate/reference; report the
median paired-round cost change in cycles per command. Interrupts remain
enabled; guest CPU, LCD, audio and network are absent.

Each logical command uses full width and 64 rows. Eight operation codes
are tested in every bitmap mode for both fill (LMMV) and copy (LMMM), plus
twelve HMMV/HMMM/YMMM controls. Source page is Y=256 and destination Y=0.
COPY-with-transparency uses CLR=0 in fills (all pixels skipped) but mixed
source pixels in copies; it is not an all-zero copy-source test. Other
fills use CLR=A5. OR/AND settle after their warmup, XOR runs an even number
of commands per pass, and both implementations receive equal call counts.

Two separately built firmware runs compare each variant with its own
reference. There is one run per variant, not a confidence interval or
production-app throughput result. Compiler/caller/layout effects are part
of these measurements; do not attribute every difference solely to the
source-level arithmetic. Negative percentages mean lower execution cost.

### SCREEN 5

| Operation | First LMMV | Revised LMMV | First LMMM | Revised LMMM |
| --- | ---: | ---: | ---: | ---: |
| COPY | +1.21% | +1.95% | +1.14% | +1.76% |
| AND | -1.20% | -1.79% | -0.38% | +0.60% |
| OR | -3.52% | -3.81% | -1.86% | -0.72% |
| XOR | -3.52% | -3.31% | -1.90% | -1.27% |
| NOT | +1.20% | +0.71% | +1.14% | +1.39% |
| transparent COPY | +3.45% | -55.01% | +1.31% | +2.63% |
| transparent XOR | -3.43% | -3.14% | -1.50% | -0.54% |
| reserved/transparent | -5.31% | -11.97% | -3.25% | -8.25% |

### SCREEN 6

| Operation | First LMMV | Revised LMMV | First LMMM | Revised LMMM |
| --- | ---: | ---: | ---: | ---: |
| COPY | +1.32% | +7.08% | -0.82% | +0.56% |
| AND | -1.31% | +3.57% | -2.48% | -0.65% |
| OR | -3.84% | +3.01% | -4.09% | -1.84% |
| XOR | -3.84% | +2.50% | -4.09% | -1.18% |
| NOT | +1.28% | +8.46% | -0.81% | +2.64% |
| transparent COPY | +3.97% | -56.78% | -0.46% | -3.55% |
| transparent XOR | -3.79% | +0.50% | -2.99% | -6.41% |
| reserved/transparent | -5.87% | -10.29% | -3.92% | -12.22% |

### SCREEN 7

| Operation | First LMMV | Revised LMMV | First LMMM | Revised LMMM |
| --- | ---: | ---: | ---: | ---: |
| COPY | +1.15% | +2.40% | -1.42% | +3.54% |
| AND | -1.15% | +0.18% | -2.76% | +1.79% |
| OR | -3.37% | -2.15% | -4.10% | +0.57% |
| XOR | -3.37% | -2.18% | -4.12% | -0.09% |
| NOT | +1.12% | +2.44% | -1.40% | +3.39% |
| transparent COPY | +3.22% | -57.99% | -1.08% | +0.78% |
| transparent XOR | -3.33% | +0.91% | -3.56% | -1.21% |
| reserved/transparent | -5.06% | -7.59% | -6.52% | -8.67% |

### SCREEN 8

| Operation | First LMMV | Revised LMMV | First LMMM | Revised LMMM |
| --- | ---: | ---: | ---: | ---: |
| COPY | -1.17% | -1.07% | -4.69% | -3.79% |
| AND | -3.44% | -3.17% | -5.88% | -5.38% |
| OR | -5.68% | -5.09% | -7.17% | -6.16% |
| XOR | -5.68% | -5.10% | -7.18% | -6.11% |
| NOT | -1.14% | -0.78% | -4.57% | -3.91% |
| transparent COPY | +1.49% | -61.32% | -9.45% | -8.37% |
| transparent XOR | -10.56% | -9.67% | -11.78% | -10.48% |
| reserved/transparent | -12.20% | -19.49% | -12.94% | -12.90% |


| Control | First cost change | Revised cost change |
| --- | ---: | ---: |
| SCREEN 5 HMMV control | -0.13% | -0.02% |
| SCREEN 5 HMMM control | -0.02% | +0.00% |
| SCREEN 5 YMMM control | +0.05% | +0.03% |
| SCREEN 6 HMMV control | +0.02% | -0.06% |
| SCREEN 6 HMMM control | +0.11% | +0.12% |
| SCREEN 6 YMMM control | -3.11% | -3.12% |
| SCREEN 7 HMMV control | +0.01% | +0.01% |
| SCREEN 7 HMMM control | -0.56% | -0.56% |
| SCREEN 7 YMMM control | +0.06% | +0.03% |
| SCREEN 8 HMMV control | +0.01% | +0.01% |
| SCREEN 8 HMMM control | -0.33% | -0.34% |
| SCREEN 8 YMMM control | +0.05% | +0.05% |

The first form saves roughly 1.9–7.2% in ordinary OR/XOR copies, but COPY
fills in SCREEN 5–7 cost about 1.2–1.3% more; some COPY/NOT copies also
regress. Transparent-zero fills cost 1.5–4.0% more despite doing no write.
The revised form makes those transparent-zero fills 55.0–61.3% cheaper,
but ordinary COPY costs up to 7.1% more and SCREEN 6 NOT fill costs 8.5%
more. These common-path costs outweigh the selective wins for this general
helper. No application-speed conclusion follows from either kernel run.

## Reference profiles and restoration

Before the kernels, the reference runs the preceding trial's four generated
command workloads on MSX2 / 60 Hz / 512 KB. Three 32,000-sample captures
per workload/core use nominal 20 kHz sampling (50 microseconds), resolved
against the exact installed reference ELF. Core 1 covers SCREEN 6 HMMV,
SCREEN 7 HMMM, SCREEN 5 LMMV and SCREEN 8 LMMM; core 0 covers HMMM. All
fifteen reference captures have the requested count/core label.

The logical-control profiles again put most work in `pset`, `point` and
the inlined command dispatcher. Profiler-free reference LMMV windows are
1.262/1.197 emulated FPS, LMMM 0.666/0.732 FPS over about fifteen seconds
each. These intentionally overloaded programs are not real software.
There is no candidate full-firmware profile, throughput or LCD comparison:
the isolated regressions are sufficient to reject both forms. The four
reference LCD captures are retained only as fixture records.

After testing, the retained reference app was flashed again and Tab5 was
restarted as MSX2 / 60 Hz / 512 KB. Its ordinary BASIC 2.1 / Disk BASIC
prompt was visually verified. No synthetic guest program is running.
Production VDP source matches the committed reference exactly; restored
normal P4 and host builds pass.

The board keeps the prior retained, uninstrumented app with isolated
`msxvidtest` disk namespace. Production main/builds keep `msxdisks`.
No user disks/cartridges are mounted or written. The existing full Amiga
flash backup remains outside git.

## Decision and next target

Keep the committed scalar pixel helper. Both packed-bit forms are correct,
but neither gives a sufficiently balanced measured improvement across the
common operations. Transparent-zero savings alone do not establish value
for representative MSX software, and P4 timing says nothing about S3 gains.

Next profile representative MSX software on copied images before further
command specialization, retaining generated fixtures as controls. Use actual operation
mix and hot paths to choose whether another command fast path is justified.
Physical input and speaker/audio checks remain open. Reference/candidate
source, benchmark firmware, fixtures and raw logs remain outside git under
`/tmp/msx-logical-2026-11-07`.
