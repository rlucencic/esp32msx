# MSX ROM images

Put the ROM images here (not part of this repository). `idf.py flash` writes
them into 16 KB slots of the `msx_roms` flash partition.

| File | What | Size | MD5 |
|---|---|---|---|
| `msx.rom` | MSX1 BIOS + MSX BASIC 1.0 (international, 50 Hz) | 32 KB | `95c94878c0ecf0b4c0d8bc8deaa9d7f3` |
| `msx2.rom` | MSX2 BIOS + MSX BASIC 2.1 (international, 50 Hz) | 32 KB | `ec3a01c91f24fbddcbcab0ad301bc9ef` |
| `msx2ext.rom` | MSX2 SUB-ROM 2.1 | 16 KB | `8aabde714a42256bef36ea9b04f6ef59` |
| `disk.rom` | Disk BASIC 1.0 for a WD2793 interface at $7FF8 | 16 KB | `973368692a03fca68c0e658856348460` |
| `nextor.rom` | Nextor 2.1.4 for the Sunrise IDE (`Nextor-2.1.4.SunriseIDE.ROM`) | 128 KB | `dd4d2aa4cd82c8f509a904c724f93fc8` |

These are the files commonly named `MSX.ROM`, `MSX2.ROM`, `MSX2EXT.ROM` and
`DISK.ROM` in emulator distributions (planetemu's "MSX - BIOS" set: "MSX
System v1.0", "MSX System v2.1 [a]", "MSX2 System Extension v2.1", "MSX-DOS
v2.2 [DISK.ROM]"). Nextor is at github.com/Konamiman/Nextor (releases).

The MSX1 needs `msx.rom`; the MSX2 `msx2.rom` and `msx2ext.rom`. Without
`disk.rom` there are no floppy drives, without `nextor.rom` no hard disk.
Other 50 Hz or 60 Hz BIOS versions work too: the video frequency follows
the BIOS.

Without flashing, the emulator also finds them in a `roms` folder on the SD
card under the same names (any case).
