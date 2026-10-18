# esp32msx: MSX1 and MSX2 on the Makerfabs MaTouch ESP32-S3 7" and the M5Stack Tab5

An MSX1 (64 KB, TMS9918A/9929A) or MSX2 (512 KB memory mapper, V9938 with
128 KB VRAM, RP5C01 clock), 50 or 60 Hz: the Z80 on esp32emu's core with the
MSX's M1 wait state, the slots and subslots, the PPI and keyboard, the
AY-3-8910 with the key click, two floppy drives behind a WD2793 (Philips
style disk ROM), a Sunrise IDE hard disk with Nextor, and ROM cartridges with
their mappers and Konami's SCC. An LVGL setup screen, a web page for images
and live status. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) and the Z80 core are the shared library in `../esp32emu`, also used
by `../esp32ste`, `../esp32mac`, `../esp32cpc`, `../esp32zx`, `../esp32cbm`,
`../esp32xl` and `../esp32gs`. The Z80's MSX memory (eight 8 KB banks and the
subslot register at `$FFFF`) is its `CONFIG_Z80_MSX` option. See its
CLAUDE.md; changes there need all projects built, for both boards.

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", ESP32-S3-WROOM-1-N16R8
(16 MB flash, 8 MB octal PSRAM), 1024x600 RGB565 panel, GT911 touch, microSD,
MAX98357A amplifier. Pins: `../esp32emu/components/emu/include/emu/board.h`.
The native USB port is the console, `/dev/cu.usbmodem1101` on the user's Mac.
The Tab5 is `/dev/cu.usbmodem3101` (its hardware: `../esp32emu/CLAUDE.md`).

## Build, flash, debug

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem1101 app-flash'
# the Tab5:
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash
```

- `app-flash` writes only the app; `idf.py flash` also writes the ROMs in
  `roms/` (not in git) into 16 KB slots of the `msx_roms` partition
  (0x810000, 256 KB): `msx.rom` 0, `msx2.rom` 2, `msx2ext.rom` 4,
  `disk.rom` 5, `nextor.rom` 6 (top `CMakeLists.txt`, `main/rom_store.c`).
  The partition overlaps the other machines' ROM partitions: switching a
  board needs a full `idf.py flash` there (and here). NVS (WiFi, BLE
  pairings) is shared. Without flashed ROMs, `roms/` on the SD card (any
  case) works too.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset | profile ELF [N]`
  with the IDF venv's Python (`-I`).
- `curl http://msx.local/api/status`; disks `curl -T img http://msx.local/disks/`,
  `PUT /api/drive/<n>` (1 and 2 the floppies, 3 the hard disk, 4 the cartridge).
  Inserting while the setup screen is open is refused: resume first.
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`s` `L` `r` `F` `t` `i` `p`) and the
MSX's (`main/main.c`): `k <n>` tap a key (row x 8 + column: 63 RETURN, 64
SPACE), `K <n> 0|1` (48 SHIFT, 49 CTRL, 60 STOP), `T <text>` type (`\n`
RETURN; a key down 2 frames, up 2), `j <port> <bits>` joystick (1 up 2 down
4 left 8 right 16 A 32 B), `R <model> [hz]` restart as 0 MSX1, 1 MSX2, at 50
or 60 Hz. Touches: Start `t 850 532` (MaTouch), `t 1065 637` (Tab5).

## Layout

| Path | What |
|---|---|
| `main/main.c` | The MSX as an `emu_machine_t`: memory, ROMs, choices (model, video, floppy), key map, joystick, the drives (A:, B:, hard disk, cartridge), status, console |
| `main/settings.c`, `rom_store.c` | NVS settings (`msx`: model, video, floppy, joystick), the ROM partition |
| `components/msx/msx.c` | Slots and memory, the PPI and keyboard, the I/O ports, the frame loop, typing |
| `components/msx/vdp.c` | TMS9918A / V9938: all modes, sprites, the V9938's commands |
| `components/msx/psg.c` | AY-3-8910, SCC, key click: events replayed on the speaker's task |
| `components/msx/fdc.c` | WD2793 (Philips style) and `.dsk` images; blank formatted disks |
| `components/msx/ide.c` | Sunrise IDE with Nextor; blank FAT16 hard disks |
| `components/msx/cart.c` | Cartridges and their mappers |
| `components/msx/rtc.c` | The RP5C01 |
| `components/msx/host/` | Host build for testing on the Mac |
| `roms/` | The ROM images (not distributed; see its README) |

## Architecture

- **Memory**: the Z80 core's eight 8 KB banks (`CONFIG_Z80_MSX`); each slot
  or subslot describes its blocks (`slot_t`: read and write pointers, or
  NULL for a device's registers); `memory_update()` maps the selected ones
  after any change of the primary slots (PPI A), a subslot register (`$FFFF`
  of an expanded slot: the core hands those accesses over), a mapper segment
  or a cartridge bank. Slot 0 BIOS, 1 the cartridge, 2 the Sunrise IDE
  (Nextor; only with a hard disk inserted), 3-0 RAM / mapper, 3-1 SUB-ROM,
  3-2 disk ROM.
- **Timing**: a frame is 262 (60 Hz) or 313 (50 Hz) lines of 228 T-states at
  3.58 MHz. The Z80 gets a wait state on every M1 cycle as the MSX engine
  adds one: counted from the refresh register's advance.
- **Video**: lines drawn when the beam has passed them; a frame where nothing
  shown changed isn't drawn again. 576x240: the 512 hires pixels (256-pixel
  modes doubled) and 32 of border each side; 192 or 212 lines in the middle.
  On the board `view_width` 800: 800x600, nearest pixel. The `Video` choice
  forces 50 or 60 Hz (the V9938's NT bit; the TMS9929A for MSX1).
- **Sound**: AY at the clock / 2, SCC, key click: register writes are events
  stamped in output samples (clock / 112 = 31,960 Hz), replayed on the
  speaker's task.
- **Floppies** ("Floppy 1" and "Floppy 2" on the setup screen and web page:
  A: and B:, or B: and C: with a hard disk): `.dsk` raw sectors, read and
  written through to the file; the drive's motor runs on 3 s (the disk ROM
  waits a second for it); disk change reported once through `$7FFF` bit 5.
  New disks: formatted 720 KB or 360 KB FAT12.
- **Hard disk**: Sunrise IDE (after openMSX's) with Nextor 2.1.4, one master
  device: a raw image of 512-byte sectors, in place. New disks: an MBR with
  one FAT16 partition from sector 63 (32 or 128 MB). In slot 2, below the
  disk ROM, Nextor starts first and runs the floppies too: with a hard disk
  inserted it is **A:** and the floppy drives **B:** and **C:** (as on a
  real MSX with a Nextor cartridge); without one, the floppies are A: and B:.
- **Cartridges**: plain up to 64 KB (placed by their header) and MegaROMs
  (ASCII 8, ASCII 16, Konami, Konami SCC), the mapper guessed from the code
  as fMSX does.
- **Clock**: RP5C01 on both models (MSX1 BIOSes with a clock ask for the
  date without one); block 2 starts with the BIOS's defaults (else SCREEN 1,
  WIDTH 29).

### Persistent state

- NVS `msx`: `model`, `video`, `floppy`, `joystick`; `msxdisks`: the
  drives' images. SD card: images in `msx/`.

## Performance (measured 2026-10-18)

- MaTouch: 100%, core 1 ~32%. Tab5: 100%, core 1 ~22%.

## Lessons

- **The MSX2's clock RAM holds the BIOS's screen settings**: zeroed, it
  started in SCREEN 1 with WIDTH 29. Block 2 gets the defaults.
- **MSX1 BIOSes ask for the date** when there's no clock chip answering: the
  RP5C01 is on both models.
- **The disk ROM waits for the motor**: without the motor status in `$7FFF`
  bit 4 it waited a second on every access.
- **50 Hz BIOS, 60 Hz picture**: the V9938 takes its frame rate from R#9's
  NT bit, which the BIOS sets from its own flag; the `Video` choice holds it.
- **Test cartridges for page 2 must select their slot** (ENASLT): the BIOS
  only maps page 1 for a cartridge it starts.
- **Nextor must start before the DOS1 disk ROM**: in slot 3-3, after the disk
  ROM in 3-2, the DOS1 kernel became the master and Nextor's FAT16 hard disk
  gave "Disk I/O error" (it only worked with the floppies off). Slot 2 makes
  Nextor the master; the disk ROM's drives come after its own.

## Testing

- **Host build**: `make -C components/msx/host`, then `build/msx_host
  [frames] [out.ppm]`. Environment: `MSX_ROMS=<folder>`, `MSX_MODEL=2`,
  `MSX_RAM=<KB>`, `MSX_HZ=50|60`, `MSX_NODISK=1`, `MSX_DISKA=`, `MSX_DISKB=`,
  `MSX_NEWDISK=<path>`, `MSX_NEWHD=<path>`, `MSX_HD=<image>`,
  `MSX_CART=<rom>`, `MSX_TEXT=1`, `MSX_WAV=<file>`, `MSX_TRACE=<pc>:<n>`,
  `MSX_STATUS=1`, `MSX_SCRIPT=<file>` (lines `<frame> <command>`: `type`,
  `key <n> 0|1`, `joy <port> <bits>`, `shot <file.ppm>`, `text`,
  `insert <drive 1-2> <path>`, `eject <drive>`, `reset`, `peek <hex>`,
  `dump <hex> <n>`, `vram <hex> <n>`, `vdp`, `flush`).
- Checked: MSX1 and MSX2 BASIC, the screen modes and sprites, the V9938's
  commands, floppies (LOAD/SAVE, FILES, new blank disks), Nextor from a
  new hard disk (FAT16, SAVE/FILES), cartridges of each mapper (test ROMs),
  the SCC and AY sound (recorded on the host), on both boards.
- **Not checked by hand**: games, a Bluetooth keyboard and joystick on the
  MSX, sound on the speakers.

## Open items

- No cassette, printer, MSX-MUSIC/FM-PAC, MSX-AUDIO; no MSX2+ or turbo R.
- `.dsk` only (no `.dmk`); one hard disk.
- Public on GitHub (`github.com/rlucencic/esp32msx`, pushed to `main`).
  Licence: MIT (`LICENSE`).

## Working with the user

- Expects changes built, flashed and checked on both boards (MaTouch first),
  then a short report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
