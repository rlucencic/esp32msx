#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "msx.h"
#include "z80.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define MSX_HOT     IRAM_ATTR
#else
#define MSX_HOT
#endif

#define LINE_T          228         /* T-states per scan line (15.7 kHz) */
#define T_PER_SAMPLE    112         /* MSX_CLOCK_HZ / MSX_AUDIO_HZ */
#define VRAM_SIZE       0x20000

/* One slot or subslot: its 8 KB blocks as the CPU sees them (NULL: through
 * read/write: registers in a ROM area, a mapper's bank switch), and the
 * device's own functions
 */
typedef struct {
    uint8_t *rd[8];
    uint8_t *wr[8];
    uint8_t (*read)(uint16_t addr);
    void (*write)(uint16_t addr, uint8_t v);
} slot_t;

typedef struct {
    msx_config_t cfg;
    uint64_t clock;             /* T-states since power-on */
    uint64_t line_start;        /* the current line began then */
    uint32_t frames;
    uint8_t ppi_a, ppi_c;       /* primary slots; keyboard row, LEDs, click */
    uint8_t sub[4];             /* secondary slot registers */
    bool expanded[4];
    uint8_t mapper[4];          /* MSX2: segment in each page */
    uint32_t mapper_segments;
    uint8_t keys[11];           /* matrix rows, 1 = up (as the PPI reads them) */
    uint8_t joy[2];             /* 1 = released, as the PSG reads them */
    bool pal;                   /* 50 Hz video */
} msx_t;

extern msx_t msx;
extern slot_t slots[4][4];
extern uint8_t msx_unmapped[8192];  /* $FF: reads of nothing */
extern uint8_t msx_sink[8192];      /* writes to ROM or nothing */

/* msx.c */
void memory_update(void);           /* after a slot, subslot or bank change */
static inline uint64_t msx_now(int at)
{
    return msx.clock + at;
}

/* vdp.c */
void vdp_init(bool v9938, bool pal);
void vdp_reset(void);
uint8_t vdp_read(int port);
void vdp_write(int port, uint8_t v);
void vdp_line(int line);            /* at the end of each frame line */
int vdp_lines(void);                /* per frame */
bool vdp_irq(void);
uint32_t vdp_signature(void);
uint8_t vdp_mode(void);
uint8_t vdp_peek(uint32_t a);
uint8_t vdp_reg(int r);
uint8_t vdp_status_reg(int s);

#ifdef MSX_HOST_HOOK
void msx_host_hook(void);           /* before every instruction (host build tracing) */
#endif

/* psg.c: the AY-3-8910, the SCC, the key click and the sound output */
void psg_reset(void);
void psg_address(uint8_t v);
void psg_write(uint8_t v, int at);
uint8_t psg_read(void);
void scc_reset(void);
uint8_t scc_read(uint16_t addr);    /* $9800-$98FF */
void scc_write(uint16_t addr, uint8_t v);
void click_write(bool level);
void audio_init(void);
void audio_frame_end(void);

/* fdc.c: WD2793 and drives A:, B: */
void fdc_reset(void);
uint8_t fdc_read(uint16_t addr);    /* $7FF8-$7FFF */
void fdc_write(uint16_t addr, uint8_t v);
void fdc_status(msx_status_t *s);

/* ide.c: Sunrise IDE (Nextor) */
bool ide_present(void);
void ide_setup(slot_t *slot);       /* its slot's blocks and functions */
void ide_reset(void);
void ide_status(msx_status_t *s);

/* cart.c */
bool cart_present(void);
void cart_setup(slot_t *slot);
void cart_reset(void);

/* rtc.c: RP5C01 */
void rtc_reset(void);
void rtc_index(uint8_t v);
uint8_t rtc_read(void);
void rtc_write(uint8_t v);

void *msx_alloc(uint32_t bytes);
void msx_release(void *p);
