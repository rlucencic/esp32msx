#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The MSX ROMs: from the msx_roms flash partition (roms/<name> in the
 * project, written by idf.py flash into 16 KB slots), else from the SD
 * card's "roms" folder under the same names.
 */
typedef enum {
    ROM_MSX1,                   /* msx.rom: MSX1 BIOS + BASIC, 32 KB */
    ROM_MSX2,                   /* msx2.rom: MSX2 BIOS + BASIC, 32 KB */
    ROM_MSX2_SUB,               /* msx2ext.rom: MSX2 SUB-ROM, 16 KB */
    ROM_DISK,                   /* disk.rom: Disk BASIC (WD2793), 16 KB */
    ROM_NEXTOR,                 /* nextor.rom: Nextor for the Sunrise IDE, 128 KB */
    ROM_COUNT
} rom_id_t;

const char *rom_store_name(rom_id_t id);    /* "msx.rom" */
uint32_t rom_store_size(rom_id_t id);

/* The whole ROM into buf; false if there's none */
bool rom_store_load(rom_id_t id, uint8_t *buf);
