#pragma once

/*
 * MSX1 and MSX2, platform independent: the Z80 (esp32emu's core, with the
 * MSX's wait state on every M1 cycle), the slots, the PPI and keyboard,
 * the AY-3-8910, the TMS9918A (MSX1) or V9938 (MSX2) with its commands,
 * the memory mapper and the RP5C01 clock (MSX2), a WD2793 floppy interface
 * (Philips style, at $7FF8-$7FFF of the disk ROM), a Sunrise IDE hard disk
 * interface (Nextor) and ROM cartridges with their mappers and the SCC.
 *
 * The slots:
 *   0     BIOS + BASIC (32 KB)
 *   1     the cartridge
 *   2     Sunrise IDE with Nextor (page 1), when there's a hard disk
 *   3-0   RAM (MSX1: 64 KB; MSX2: a memory mapper)
 *   3-1   MSX2 SUB-ROM (page 0)
 *   3-2   disk ROM (page 1), with the floppy controller
 */
#include <stdbool.h>
#include <stdint.h>

/* The picture: the 512 hires pixels of a line (256-pixel modes drawn
 * twice) with 32 of border each side, 240 lines (192 or 212 shown, the
 * rest border)
 */
#define MSX_SCREEN_WIDTH    576
#define MSX_SCREEN_HEIGHT   240
#define MSX_CLOCK_HZ        3579545
#define MSX_AUDIO_HZ        31960       /* the clock / 112 */

#define MSX_FLOPPIES        2

typedef enum {
    MSX_MODEL_MSX1,
    MSX_MODEL_MSX2,
    MSX_MODEL_COUNT
} msx_model_t;

typedef struct {
    msx_model_t model;
    uint32_t ram_kb;            /* MSX2: the mapper's size (128 ... 4096); MSX1: 64 */
    bool hz50;                  /* 50 Hz video (TMS9929A; the V9938's NT bit held set), else 60 Hz */
    const uint8_t *bios;        /* 32 KB */
    const uint8_t *sub_rom;     /* MSX2: 16 KB */
    const uint8_t *disk_rom;    /* 16 KB, or NULL: no floppy drives */
    const uint8_t *nextor;      /* the Sunrise IDE's 128 KB flash, or NULL */
    uint8_t *ram;               /* ram_kb KB, PSRAM */
    uint8_t *vram;              /* 128 KB */
    uint16_t *frame;            /* MSX_SCREEN_WIDTH * MSX_SCREEN_HEIGHT, RGB565 */
    void *(*alloc)(uint32_t bytes);     /* for disk images and cartridges (PSRAM) */
    void (*release)(void *p);
} msx_config_t;

bool msx_init(const msx_config_t *cfg);
void msx_reset(void);               /* power off and on */
void msx_run_frame(void);
int msx_screen_hz(void);            /* 50 or 60, as the VDP is set */
uint32_t msx_screen_signature(void);
const uint16_t *msx_line(int y);    /* a picture row */

/* Keys: row * 8 + column of the MSX keyboard matrix (international) */
enum {
    MSX_KEY_SHIFT = 6 * 8 + 0, MSX_KEY_CTRL, MSX_KEY_GRAPH, MSX_KEY_CAPS, MSX_KEY_CODE,
    MSX_KEY_F1, MSX_KEY_F2, MSX_KEY_F3,
    MSX_KEY_F4 = 7 * 8 + 0, MSX_KEY_F5, MSX_KEY_ESC, MSX_KEY_TAB, MSX_KEY_STOP, MSX_KEY_BS,
    MSX_KEY_SELECT, MSX_KEY_RETURN,
    MSX_KEY_SPACE = 8 * 8 + 0, MSX_KEY_HOME, MSX_KEY_INS, MSX_KEY_DEL, MSX_KEY_LEFT, MSX_KEY_UP,
    MSX_KEY_DOWN, MSX_KEY_RIGHT,
    MSX_KEY_COUNT = 11 * 8
};
void msx_key(int key, bool down);
/* The matrix key and whether it needs SHIFT for an ASCII character; -1 if none */
int msx_ascii_key(char c, bool *shift);
/* Type text: one key every few frames ('\n' is RETURN); replaces any text not typed yet */
void msx_type(const char *text);
bool msx_typing(void);

/* Joystick port 1 or 2: bits 0 up, 1 down, 2 left, 3 right, 4 trigger A, 5 trigger B */
void msx_joystick(int port, uint8_t bits);

/* Floppy drives A: and B: (.dsk: 360 or 720 KB raw sectors) */
bool msx_floppy_insert(int drive, const char *path);
void msx_floppy_eject(int drive);
const char *msx_floppy_path(int drive);
bool msx_floppy_write_protected(int drive);
bool msx_floppy_flush(void);        /* written sectors back into the files */
bool msx_floppy_create(const char *path, uint32_t bytes);   /* a formatted blank disk */

/* The hard disk (a raw image of 512-byte sectors, partitioned for Nextor):
 * takes effect at the next start
 */
bool msx_hd_insert(const char *path);
void msx_hd_eject(void);
const char *msx_hd_path(void);
bool msx_hd_flush(void);
bool msx_hd_create(const char *path, uint32_t bytes);       /* MBR + one FAT16 partition */

/* The cartridge (.rom): takes effect at the next start */
bool msx_cart_insert(const char *path);
void msx_cart_eject(void);
const char *msx_cart_path(void);
const char *msx_cart_type(void);    /* "ASCII 16", ... */

/* Sound: 16-bit stereo at MSX_AUDIO_HZ, rendered on another task */
void msx_audio_enable(bool enable);
int msx_audio_render(int16_t *dst, int frames);
int msx_audio_pending(void);

typedef struct {
    uint32_t frames;
    uint16_t pc;
    uint8_t slots;              /* PPI port A */
    uint8_t vdp_mode;
    int floppy_track[MSX_FLOPPIES];
    uint32_t floppy_reads[MSX_FLOPPIES], floppy_writes[MSX_FLOPPIES];
    bool floppy_busy[MSX_FLOPPIES];
    uint32_t hd_reads, hd_writes;
    bool hd_busy;
} msx_status_t;
void msx_get_status(msx_status_t *s);
