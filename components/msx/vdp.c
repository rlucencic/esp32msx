/*
 * The video chip: a TMS9918A (MSX1; TMS9929A at 50 Hz) or a V9938 (MSX2),
 * ports $98-$9B.
 *
 * Lines are drawn when the beam has passed them, with the registers as
 * they are then: a raster split made in a line interrupt lands on the
 * next line. A frame in which nothing that's shown changed (VRAM,
 * registers, palette, the blink phase) isn't drawn again; its sprite
 * status (fifth sprite, collision) is the last one drawn.
 *
 * The picture has 240 lines: 192 or 212 of display in the middle, border
 * around; each of the 512 hires pixels of a line one pixel wide (256-pixel
 * modes twice), 32 of border each side.
 *
 * V9938: the modes T1, T2 (80 columns, blink), G1-G7, MC, sprite mode 2
 * (8 a line, a colour per line, CC/IC/EC), the 512-colour palette, the
 * vertical scroll, line interrupts, and the commands. In G6 and G7 the
 * VRAM is interleaved between its two banks: CPU, commands and display
 * all go through that mapping there. Commands run at once (CE is clear
 * again by the next access); the CPU transfers (LMMC, HMMC, LMCM) go a
 * byte at a time through R#44 / S#7 with TR.
 */
#include <string.h>
#include "msx_internal.h"

#define BORDER          32
#define ACTIVE_W        512

enum {
    MODE_G1 = 0x00, MODE_T1 = 0x01, MODE_MC = 0x02, MODE_G2 = 0x04, MODE_G3 = 0x08,
    MODE_T2 = 0x09, MODE_G4 = 0x0C, MODE_G5 = 0x10, MODE_G6 = 0x14, MODE_G7 = 0x1C,
};

static const uint8_t tms_rgb[16][3] = {
    { 0, 0, 0 }, { 0, 0, 0 }, { 33, 200, 66 }, { 94, 220, 120 },
    { 84, 85, 237 }, { 125, 118, 252 }, { 212, 82, 77 }, { 66, 235, 245 },
    { 252, 85, 84 }, { 255, 121, 120 }, { 212, 193, 84 }, { 230, 206, 128 },
    { 33, 176, 59 }, { 201, 91, 186 }, { 204, 204, 204 }, { 255, 255, 255 },
};

/* The MSX2 BIOS's palette (R, G, B of 0-7) */
static const uint8_t v9938_palette[16][3] = {
    { 0, 0, 0 }, { 0, 0, 0 }, { 1, 6, 1 }, { 3, 7, 3 }, { 1, 1, 7 }, { 2, 3, 7 }, { 5, 1, 1 }, { 2, 6, 7 },
    { 7, 1, 1 }, { 7, 3, 3 }, { 6, 6, 1 }, { 6, 6, 4 }, { 1, 4, 1 }, { 6, 2, 5 }, { 5, 5, 5 }, { 7, 7, 7 },
};

/* G7's sprites have fixed colours (GRB 3-3-2) */
static const uint8_t g7_sprite_color[16] = {
    0x00, 0x02, 0x10, 0x12, 0x80, 0x82, 0x90, 0x92, 0x49, 0x4B, 0x59, 0x5B, 0xC9, 0xCB, 0xD9, 0xDB,
};

static const uint8_t reg_mask_v9938[48] = {
    0x7E, 0x7B, 0x7F, 0xFF, 0x3F, 0xFF, 0x3F, 0xFF, 0xFB, 0xBF, 0x07, 0x03, 0xFF, 0xFF, 0x07, 0x0F,
    0x0F, 0xBF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0x01, 0xFF, 0x03, 0xFF, 0x01, 0xFF, 0x03, 0xFF, 0x01, 0xFF, 0x03, 0xFF, 0x7F, 0xFF, 0x00,
};
static const uint8_t reg_mask_tms[8] = { 0x03, 0xFB, 0x0F, 0xFF, 0x07, 0x7F, 0x07, 0xFF };

static struct {
    bool v9938, pal50;
    uint8_t *vram;
    uint32_t vram_mask;
    uint8_t reg[48];
    uint8_t status[10];
    uint8_t latch;
    bool second;
    uint32_t addr;              /* 14 bits of counter (+ R#14 on the V9938) */
    uint8_t read_ahead;
    uint8_t pal_first;
    bool pal_second;
    uint16_t palette[16];       /* RGB565 */
    uint16_t rgb332[256];       /* G7 */
    bool dirty;                 /* something shown changed since the last line drawn */
    bool draw;                  /* this frame's lines are being drawn */
    bool draw_next;             /* ... and the next frame's (a change in the middle of one) */
    uint32_t signature;
    uint8_t sprite_status;      /* 5S, C and the fifth sprite of the last frame drawn */
    bool blink_on;
    int blink_count;
    /* Command engine */
    struct {
        uint8_t cmd, op;
        int sx, sy, dx, dy, nx, ny;
        int x, y, cx, cy;       /* progress */
        int dix, diy;
        bool active, transfer;  /* LMMC/HMMC/LMCM waiting for the CPU */
    } cmd;
} vdp;

static inline uint16_t rgb565(int r, int g, int b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static inline int mode(void)
{
    return ((vdp.reg[1] >> 4) & 1) | ((vdp.reg[1] >> 2) & 2) | ((vdp.reg[0] & 0x0E) << 1);
}

static inline bool planar(void)
{
    int m = mode();
    return vdp.v9938 && (m == MODE_G6 || m == MODE_G7);
}

/* A logical VRAM address as the chip stores it */
static inline uint32_t phys(uint32_t a, bool pl)
{
    a &= vdp.vram_mask;
    return pl ? ((a >> 1) | ((a & 1) << 16)) & vdp.vram_mask : a;
}

static inline uint8_t vram_rd(uint32_t a)
{
    return vdp.vram[a & vdp.vram_mask];
}

static void set_palette(int i, int r, int g, int b)
{
    vdp.palette[i] = rgb565(r * 255 / 7, g * 255 / 7, b * 255 / 7);
}

void vdp_init(bool v9938, bool pal)
{
    vdp.v9938 = v9938;
    vdp.pal50 = pal;
    vdp.vram = msx.cfg.vram;
    vdp.vram_mask = v9938 ? VRAM_SIZE - 1 : 0x3FFF;
    for (int c = 0; c < 256; c++) {
        int g = (c >> 5) & 7, r = (c >> 2) & 7, b = c & 3;
        vdp.rgb332[c] = rgb565(r * 255 / 7, g * 255 / 7, b * 255 / 3);
    }
    vdp_reset();
}

void vdp_reset(void)
{
    memset(vdp.reg, 0, sizeof(vdp.reg));
    memset(vdp.status, 0, sizeof(vdp.status));
    memset(&vdp.cmd, 0, sizeof(vdp.cmd));
    vdp.second = false;
    vdp.addr = 0;
    vdp.pal_second = false;
    if (vdp.pal50 && vdp.v9938) {
        vdp.reg[9] = 0x02;
    }
    for (int i = 0; i < 16; i++) {
        if (vdp.v9938) {
            set_palette(i, v9938_palette[i][0], v9938_palette[i][1], v9938_palette[i][2]);
        } else {
            vdp.palette[i] = rgb565(tms_rgb[i][0], tms_rgb[i][1], tms_rgb[i][2]);
        }
    }
    memset(vdp.vram, 0, vdp.vram_mask + 1);
    vdp.dirty = true;
}

int vdp_lines(void)
{
    bool pal = vdp.v9938 ? (vdp.reg[9] & 0x02) != 0 : vdp.pal50;
    return pal ? 313 : 262;
}

bool vdp_irq(void)
{
    return ((vdp.status[0] & 0x80) && (vdp.reg[1] & 0x20)) ||
           (vdp.v9938 && (vdp.status[1] & 0x01) && (vdp.reg[0] & 0x10));
}

uint32_t vdp_signature(void)
{
    return vdp.signature;
}

uint8_t vdp_mode(void)
{
    return mode();
}

/* Debugging: VRAM by logical address, a register, a status register */
uint8_t vdp_peek(uint32_t a)
{
    return vram_rd(phys(a, planar()));
}

uint8_t vdp_reg(int r)
{
    return r >= 0 && r < 48 ? vdp.reg[r] : 0xFF;
}

uint8_t vdp_status_reg(int s)
{
    return s >= 0 && s < 10 ? vdp.status[s] : 0xFF;
}

static inline int active_lines(void)
{
    return vdp.v9938 && (vdp.reg[9] & 0x80) ? 212 : 192;
}

/* ---- Commands ---- */

static inline int cmd_ppb(int m)            /* pixels per byte */
{
    return m == MODE_G4 || m == MODE_G6 ? 2 : m == MODE_G5 ? 4 : 1;
}

static inline int cmd_width(int m)
{
    return m == MODE_G5 || m == MODE_G6 ? 512 : 256;
}

/* The byte holding pixel (x, y) in the command's coordinates */
static inline uint32_t cmd_addr(int m, int x, int y)
{
    switch (m) {
    case MODE_G4: return (y & 1023) * 128 + ((x & 255) >> 1);
    case MODE_G5: return (y & 1023) * 128 + ((x & 511) >> 2);
    case MODE_G6: return (y & 511) * 256 + ((x & 511) >> 1);
    default:      return (y & 511) * 256 + (x & 255);
    }
}

static uint8_t point(int m, int x, int y)
{
    uint8_t b = vram_rd(phys(cmd_addr(m, x, y), planar()));
    switch (m) {
    case MODE_G4: case MODE_G6: return (x & 1) ? b & 15 : b >> 4;
    case MODE_G5: return (b >> ((3 - (x & 3)) * 2)) & 3;
    default:      return b;
    }
}

static void pset(int m, int x, int y, uint8_t c, uint8_t op)
{
    uint32_t a = phys(cmd_addr(m, x, y), planar());
    uint8_t mask, shift;
    switch (m) {
    case MODE_G4: case MODE_G6: shift = (x & 1) ? 0 : 4; mask = 0x0F; break;
    case MODE_G5: shift = (3 - (x & 3)) * 2; mask = 0x03; break;
    default:      shift = 0; mask = 0xFF; break;
    }
    c &= mask;
    if ((op & 8) && c == 0) {
        return;                             /* transparent */
    }
    uint8_t d = (vdp.vram[a] >> shift) & mask;
    switch (op & 7) {
    case 0: d = c; break;
    case 1: d &= c; break;
    case 2: d |= c; break;
    case 3: d ^= c; break;
    case 4: d = ~c & mask; break;
    default: break;
    }
    vdp.vram[a] = (vdp.vram[a] & ~(mask << shift)) | ((d & mask) << shift);
}

static int reg_word(int r, int bits)
{
    return (vdp.reg[r] | (vdp.reg[r + 1] << 8)) & ((1 << bits) - 1);
}

static void set_reg_word(int r, int v)
{
    vdp.reg[r] = v & 0xFF;
    vdp.reg[r + 1] = (v >> 8) & reg_mask_v9938[r + 1];
}

static void cmd_end(void)
{
    vdp.cmd.active = vdp.cmd.transfer = false;
    vdp.status[2] &= ~0x81;                 /* CE, TR */
    vdp.reg[46] &= 0x0F;
}

/* One step of a CPU transfer: the next pixel or byte to or from the CPU */
static void transfer_next(void)
{
    if (++vdp.cmd.cx >= vdp.cmd.nx) {
        vdp.cmd.cx = 0;
        vdp.cmd.x = (vdp.cmd.cmd == 0xA) ? vdp.cmd.sx : vdp.cmd.dx;
        vdp.cmd.y += vdp.cmd.diy;
        if (++vdp.cmd.cy >= vdp.cmd.ny) {
            if (vdp.cmd.cmd == 0xA) {
                set_reg_word(34, vdp.cmd.y);
            } else {
                set_reg_word(38, vdp.cmd.y);
            }
            set_reg_word(42, 0);
            cmd_end();
            return;
        }
    } else {
        vdp.cmd.x += vdp.cmd.dix;
    }
}

static void cmd_transfer_write(uint8_t v)
{
    int m = mode();
    if (vdp.cmd.cmd == 0xF) {               /* HMMC: bytes */
        vdp.vram[phys(cmd_addr(m, vdp.cmd.x * cmd_ppb(m), vdp.cmd.y), planar())] = v;
    } else {                                /* LMMC: pixels */
        pset(m, vdp.cmd.x, vdp.cmd.y, v, vdp.cmd.op);
    }
    vdp.dirty = true;
    transfer_next();
}

static void cmd_start(uint8_t v)
{
    int m = mode();
    if (!(m == MODE_G4 || m == MODE_G5 || m == MODE_G6 || m == MODE_G7)) {
        m = MODE_G7;                        /* the other modes: as G7 */
    }
    int ppb = cmd_ppb(m), width = cmd_width(m);
    uint8_t arg = vdp.reg[45];
    uint8_t clr = vdp.reg[44];
    int sx = reg_word(32, 9), sy = reg_word(34, 10), dx = reg_word(36, 9), dy = reg_word(38, 10);
    int nx = reg_word(40, 10), ny = reg_word(42, 10);
    int dix = (arg & 0x04) ? -1 : 1, diy = (arg & 0x08) ? -1 : 1;
    uint8_t op = v & 0x0F;
    vdp.cmd.cmd = v >> 4;
    vdp.cmd.op = op;
    vdp.status[2] |= 0x01;                  /* CE */
    vdp.dirty = true;

    switch (vdp.cmd.cmd) {
    case 0x0:                               /* STOP */
        cmd_end();
        return;
    case 0x4:                               /* POINT */
        vdp.status[7] = point(m, sx, sy);
        break;
    case 0x5:                               /* PSET */
        pset(m, dx, dy, clr, op);
        break;
    case 0x6: {                             /* SRCH */
        bool ne = arg & 0x02;
        vdp.status[2] &= ~0x10;
        for (int x = sx; x >= 0 && x < width; x += dix) {
            bool eq = point(m, x, sy) == (clr & (m == MODE_G7 ? 0xFF : m == MODE_G5 ? 3 : 15));
            if (eq != ne) {
                vdp.status[2] |= 0x10;      /* BD: found */
                vdp.status[8] = x & 0xFF;
                vdp.status[9] = (x >> 8) | 0xFE;
                break;
            }
        }
        break;
    }
    case 0x7: {                             /* LINE: NX the long side, NY the short */
        int asx = (nx - 1) >> 1;
        int x = dx, y = dy;
        for (int i = 0;; i++) {
            pset(m, x, y, clr, op);
            if (i == nx) {
                break;
            }
            if (arg & 0x01) {               /* long side Y */
                y += diy;
                if (asx < ny) {
                    asx += nx;
                    x += dix;
                }
            } else {
                x += dix;
                if (asx < ny) {
                    asx += nx;
                    y += diy;
                }
            }
            asx = (asx - ny) & 1023;
            if (x < 0 || x >= width) {
                break;
            }
        }
        set_reg_word(38, y);
        break;
    }
    case 0x8: case 0xC: {                   /* LMMV, HMMV */
        bool bytes = vdp.cmd.cmd == 0xC;
        if (nx == 0) nx = width;
        if (ny == 0) ny = 1024;
        int y = dy;
        for (int j = 0; j < ny; j++, y += diy) {
            int x = dx;
            for (int i = 0; i < nx; i++, x += dix) {
                if (x < 0 || x >= width) {
                    break;
                }
                if (bytes) {
                    if (i % ppb == 0) {
                        vdp.vram[phys(cmd_addr(m, x, y), planar())] = clr;
                    }
                } else {
                    pset(m, x, y, clr, op);
                }
            }
        }
        set_reg_word(38, y);
        set_reg_word(42, 0);
        break;
    }
    case 0x9: case 0xD: {                   /* LMMM, HMMM */
        bool bytes = vdp.cmd.cmd == 0xD;
        if (nx == 0) nx = width;
        if (ny == 0) ny = 1024;
        int ys = sy, yd = dy;
        for (int j = 0; j < ny; j++, ys += diy, yd += diy) {
            int xs = sx, xd = dx;
            for (int i = 0; i < nx; i++, xs += dix, xd += dix) {
                if (xs < 0 || xs >= width || xd < 0 || xd >= width) {
                    break;
                }
                if (bytes) {
                    if (i % ppb == 0) {
                        vdp.vram[phys(cmd_addr(m, xd, yd), planar())] =
                            vram_rd(phys(cmd_addr(m, xs, ys), planar()));
                    }
                } else {
                    pset(m, xd, yd, point(m, xs, ys), op);
                }
            }
        }
        set_reg_word(34, ys);
        set_reg_word(38, yd);
        set_reg_word(42, 0);
        break;
    }
    case 0xE: {                             /* YMMM: from DX to the edge, SY to DY */
        if (ny == 0) ny = 1024;
        int ys = sy, yd = dy;
        for (int j = 0; j < ny; j++, ys += diy, yd += diy) {
            for (int x = dx; x >= 0 && x < width; x += dix * ppb) {
                vdp.vram[phys(cmd_addr(m, x, yd), planar())] = vram_rd(phys(cmd_addr(m, x, ys), planar()));
            }
        }
        set_reg_word(34, ys);
        set_reg_word(38, yd);
        set_reg_word(42, 0);
        break;
    }
    case 0xA: case 0xB: case 0xF: {         /* LMCM, LMMC, HMMC: through the CPU */
        bool bytes = vdp.cmd.cmd == 0xF;
        if (nx == 0) nx = width;
        if (ny == 0) ny = 1024;
        vdp.cmd.nx = bytes ? (nx + ppb - 1) / ppb : nx;
        vdp.cmd.ny = ny;
        vdp.cmd.sx = sx;
        vdp.cmd.dx = bytes ? dx / ppb : dx;
        vdp.cmd.dix = dix;
        vdp.cmd.diy = diy;
        vdp.cmd.cx = vdp.cmd.cy = 0;
        vdp.cmd.active = vdp.cmd.transfer = true;
        if (vdp.cmd.cmd == 0xA) {
            vdp.cmd.x = sx;
            vdp.cmd.y = sy;
            vdp.status[7] = point(m, sx, sy);
        } else {
            vdp.cmd.x = vdp.cmd.dx;
            vdp.cmd.y = dy;
            vdp.status[2] |= 0x80;          /* TR */
            cmd_transfer_write(clr);        /* the first one is CLR's */
            if (vdp.cmd.active) {
                vdp.status[2] |= 0x80;
            }
            return;
        }
        vdp.status[2] |= 0x80;
        return;
    }
    default:
        break;
    }
    cmd_end();
}

/* ---- CPU side ---- */

static void reg_write(int r, uint8_t v)
{
    if (!vdp.v9938) {
        r &= 7;
        v &= reg_mask_tms[r];
    } else {
        if (r >= 47) {
            return;
        }
        v &= reg_mask_v9938[r];
    }
    if (vdp.v9938 && r == 9) {
        /* NT: the machine's video frequency (the BIOS writes 0 there) */
        v = (v & ~0x02) | (vdp.pal50 ? 0x02 : 0);
    }
    vdp.reg[r] = v;
    if (r < 24) {
        vdp.dirty = true;
    }
    if (r == 46) {
        cmd_start(v);
    } else if (r == 44 && vdp.cmd.transfer && vdp.cmd.cmd != 0xA) {
        cmd_transfer_write(v);
        if (vdp.cmd.active) {
            vdp.status[2] |= 0x80;
        }
    } else if (r == 14) {
        vdp.addr = (vdp.addr & 0x3FFF) | ((v & 7) << 14);
    }
}

static inline void addr_increment(void)
{
    uint32_t a = vdp.addr + 1;
    if (vdp.v9938) {
        int m = mode();
        if ((a & 0x3FFF) == 0 && !(m == MODE_G4 || m == MODE_G5 || m == MODE_G6 || m == MODE_G7 || m == MODE_T2)) {
            a -= 0x4000;                    /* no carry into R#14 */
        }
        vdp.addr = a & 0x1FFFF;
        vdp.reg[14] = vdp.addr >> 14;
    } else {
        vdp.addr = a & 0x3FFF;
    }
}

uint8_t vdp_read(int port)
{
    switch (port & 3) {
    case 0: {
        uint8_t v = vdp.read_ahead;
        vdp.read_ahead = vram_rd(phys(vdp.addr, planar()));
        addr_increment();
        vdp.second = false;
        return v;
    }
    case 1: {
        vdp.second = false;
        if (!vdp.v9938) {
            uint8_t v = vdp.status[0];
            vdp.status[0] &= 0x1F;          /* F, 5S, C clear when read */
            return v;
        }
        int s = vdp.reg[15] & 15;
        uint8_t v;
        switch (s) {
        case 0:
            v = vdp.status[0];
            vdp.status[0] &= 0x1F;
            break;
        case 1:
            v = (vdp.status[1] & 0x01) | 0x00;      /* ID 0: V9938 */
            vdp.status[1] &= ~0x01;
            break;
        case 2:
            /* TR, VR, BD, CE as kept; HR in the last quarter of the line */
            v = (vdp.status[2] & 0xD1) | 0x0C;
            if (msx.clock - msx.line_start > LINE_T * 3 / 4) {
                v |= 0x20;
            }
            break;
        case 7:
            v = vdp.status[7];
            if (vdp.cmd.transfer && vdp.cmd.cmd == 0xA) {
                transfer_next();
                if (vdp.cmd.active) {
                    vdp.status[7] = point(mode(), vdp.cmd.x, vdp.cmd.y);
                    vdp.status[2] |= 0x80;
                }
            }
            break;
        default:
            v = s < 10 ? vdp.status[s] : 0xFF;
            break;
        }
        return v;
    }
    default:
        return 0xFF;
    }
}

void vdp_write(int port, uint8_t v)
{
    switch (port & 3) {
    case 0:
        vdp.vram[phys(vdp.addr, planar())] = v;
        vdp.read_ahead = v;
        addr_increment();
        vdp.second = false;
        vdp.dirty = true;
        break;
    case 1:
        if (!vdp.second) {
            vdp.latch = v;
            vdp.second = true;
            break;
        }
        vdp.second = false;
        if (v & 0x80) {
            reg_write(v & 0x3F, vdp.latch);
        } else {
            vdp.addr = (vdp.addr & 0x1C000) | ((v & 0x3F) << 8) | vdp.latch;
            if (!(v & 0x40)) {              /* read setup: prefetch */
                vdp.read_ahead = vram_rd(phys(vdp.addr, planar()));
                addr_increment();
            }
        }
        break;
    case 2:
        if (!vdp.v9938) {
            break;
        }
        if (!vdp.pal_second) {
            vdp.pal_first = v;
            vdp.pal_second = true;
        } else {
            int i = vdp.reg[16] & 15;
            set_palette(i, (vdp.pal_first >> 4) & 7, v & 7, vdp.pal_first & 7);
            vdp.reg[16] = (i + 1) & 15;
            vdp.pal_second = false;
            vdp.dirty = true;
        }
        break;
    case 3:
        if (vdp.v9938) {
            int r = vdp.reg[17] & 0x3F;
            if (r != 17) {
                reg_write(r, v);
            }
            if (!(vdp.reg[17] & 0x80)) {
                vdp.reg[17] = (vdp.reg[17] & 0x80) | ((r + 1) & 0x3F);
            }
        }
        break;
    }
}

/* ---- Drawing ---- */

static inline uint16_t color(int c)
{
    c &= 15;
    if (c == 0 && !(vdp.v9938 && (vdp.reg[8] & 0x20))) {
        c = vdp.reg[7] & 15;
    }
    return vdp.palette[c];
}

static inline uint16_t backdrop(int m)
{
    if (vdp.v9938 && m == MODE_G7) {
        return vdp.rgb332[vdp.reg[7]];
    }
    return vdp.palette[vdp.reg[7] & 15];
}

static inline void fill(uint16_t *dst, int n, uint16_t c)
{
    for (int i = 0; i < n; i++) {
        dst[i] = c;
    }
}

/* 8 pixels of a pattern byte, each `w` hires pixels wide */
static inline void pattern8(uint16_t *dst, uint8_t bits, uint16_t fg, uint16_t bg, int w)
{
    for (int i = 0; i < 8; i++, bits <<= 1) {
        uint16_t c = (bits & 0x80) ? fg : bg;
        for (int k = 0; k < w; k++) {
            *dst++ = c;
        }
    }
}

static void draw_text(uint16_t *dst, int y, bool t2)
{
    uint16_t fg = color(vdp.reg[7] >> 4), bg = color(vdp.reg[7]);
    uint32_t pg = (vdp.reg[4] & 0x3F) << 11;
    int row = y >> 3, line = y & 7;
    if (!t2) {
        uint32_t nt = (vdp.reg[2] & 0x7F) << 10;
        fill(dst, 16, bg);
        uint16_t *p = dst + 16;
        for (int col = 0; col < 40; col++) {
            uint8_t bits = vram_rd(pg + vram_rd(nt + row * 40 + col) * 8 + line);
            for (int i = 0; i < 6; i++, bits <<= 1) {
                p[0] = p[1] = (bits & 0x80) ? fg : bg;
                p += 2;
            }
        }
        fill(p, 16, bg);
        return;
    }
    uint32_t nt = (vdp.reg[2] & 0x7C) << 10;
    uint32_t ct = ((vdp.reg[10] & 7) << 14) | ((vdp.reg[3] & 0xF8) << 6);
    uint16_t bfg = color(vdp.reg[12] >> 4), bbg = color(vdp.reg[12]);
    fill(dst, 16, bg);
    uint16_t *p = dst + 16;
    for (int col = 0; col < 80; col++) {
        int i = row * 80 + col;
        uint8_t bits = vram_rd(pg + vram_rd(nt + i) * 8 + line);
        bool blink = vdp.blink_on && (vram_rd(ct + (i >> 3)) & (0x80 >> (i & 7)));
        uint16_t f = blink ? bfg : fg, b = blink ? bbg : bg;
        for (int k = 0; k < 6; k++, bits <<= 1) {
            *p++ = (bits & 0x80) ? f : b;
        }
    }
    fill(p, 16, bg);
}

static void draw_tiles(uint16_t *dst, int y, int m)
{
    uint32_t nt = (vdp.reg[2] & 0x7F) << 10;
    int row = (y >> 3) & 31, line = y & 7;
    if (m == MODE_G1) {
        uint32_t pg = (vdp.reg[4] & 0x3F) << 11;
        uint32_t ct = ((vdp.reg[10] & 7) << 14) | (vdp.reg[3] << 6);
        for (int col = 0; col < 32; col++) {
            uint8_t c = vram_rd(nt + row * 32 + col);
            uint8_t cl = vram_rd(ct + (c >> 3));
            pattern8(dst + col * 16, vram_rd(pg + c * 8 + line), color(cl >> 4), color(cl), 2);
        }
        return;
    }
    if (m == MODE_MC) {
        uint32_t pg = (vdp.reg[4] & 0x3F) << 11;
        for (int col = 0; col < 32; col++) {
            uint8_t c = vram_rd(nt + row * 32 + col);
            uint8_t cl = vram_rd(pg + c * 8 + ((row & 3) * 2) + ((line >> 2) & 1));
            fill(dst + col * 16, 8, color(cl >> 4));
            fill(dst + col * 16 + 8, 8, color(cl));
        }
        return;
    }
    /* G2, G3 */
    uint32_t pg = (vdp.reg[4] & 0x3C) << 11;
    uint32_t ct = ((vdp.reg[10] & 7) << 14) | ((vdp.reg[3] & 0x80) << 6);
    int pmask = ((vdp.reg[4] & 3) << 8) | 0xFF;
    int cmask = ((vdp.reg[3] & 0x7F) << 3) | 7;
    int third = (row >> 3) << 8;
    for (int col = 0; col < 32; col++) {
        int idx = vram_rd(nt + row * 32 + col) + third;
        uint8_t bits = vram_rd(pg + ((idx & pmask) << 3) + line);
        uint8_t cl = vram_rd(ct + ((idx & cmask) << 3) + line);
        pattern8(dst + col * 16, bits, color(cl >> 4), color(cl), 2);
    }
}

static void draw_bitmap(uint16_t *dst, int y, int m)
{
    bool pl = m == MODE_G6 || m == MODE_G7;
    switch (m) {
    case MODE_G4: {
        uint32_t base = ((vdp.reg[2] & 0x60) << 10) + y * 128;
        for (int i = 0; i < 128; i++) {
            uint8_t b = vram_rd(base + i);
            uint16_t c0 = color(b >> 4), c1 = color(b);
            dst[0] = dst[1] = c0;
            dst[2] = dst[3] = c1;
            dst += 4;
        }
        break;
    }
    case MODE_G5: {
        uint32_t base = ((vdp.reg[2] & 0x60) << 10) + y * 128;
        for (int i = 0; i < 128; i++) {
            uint8_t b = vram_rd(base + i);
            *dst++ = color(b >> 6 & 3);
            *dst++ = color(b >> 4 & 3);
            *dst++ = color(b >> 2 & 3);
            *dst++ = color(b & 3);
        }
        break;
    }
    case MODE_G6: {
        uint32_t base = ((vdp.reg[2] & 0x20) << 11) + y * 256;
        for (int i = 0; i < 256; i++) {
            uint8_t b = vram_rd(phys(base + i, pl));
            *dst++ = color(b >> 4);
            *dst++ = color(b);
        }
        break;
    }
    default: {                              /* G7 */
        uint32_t base = ((vdp.reg[2] & 0x20) << 11) + y * 256;
        for (int i = 0; i < 256; i++) {
            uint16_t c = vdp.rgb332[vram_rd(phys(base + i, pl))];
            dst[0] = dst[1] = c;
            dst += 2;
        }
        break;
    }
    }
}

/* Sprites of display line `y` (scrolled) over the line's pixels */
static void draw_sprites(uint16_t *dst, int y, int m)
{
    if (m == MODE_T1 || m == MODE_T2 || (vdp.v9938 && (vdp.reg[8] & 0x02))) {
        return;
    }
    bool mode2 = vdp.v9938 && m >= MODE_G3 && m != MODE_T2;
    bool pl = planar();
    uint32_t sat = vdp.v9938 ? ((vdp.reg[5] & (mode2 ? 0xFC : 0xFF)) << 7) | ((vdp.reg[11] & 3) << 15)
                             : (vdp.reg[5] & 0x7F) << 7;
    uint32_t spg = (vdp.reg[6] & (vdp.v9938 ? 0x3F : 0x07)) << 11;
    int size = (vdp.reg[1] & 2) ? 16 : 8, mag = (vdp.reg[1] & 1) ? 2 : 1;
    int span = size * mag;
    int limit = mode2 ? 8 : 4;
    uint8_t end = mode2 ? 216 : 208;

    uint8_t pix[256 + 32];
    uint8_t owner[256 + 32];
    memset(pix, 0, sizeof(pix));
    int count = 0, group = -1;
    for (int i = 0; i < 32; i++) {
        uint32_t a = sat + i * 4;
        uint8_t sy = vram_rd(phys(a, pl));
        if (sy == end) {
            break;
        }
        int start = sy > (mode2 ? 0xF0 : 0xE0) ? sy - 256 : sy;
        int rel = y - (start + 1);
        if (rel < 0 || rel >= span) {
            continue;
        }
        if (++count > limit) {
            if (!(vdp.status[0] & 0x40)) {
                vdp.status[0] = (vdp.status[0] & 0xA0) | 0x40 | i;
            }
            break;
        }
        int sx = vram_rd(phys(a + 1, pl));
        uint8_t pat = vram_rd(phys(a + 2, pl));
        int row = rel / mag;
        uint8_t attr;
        if (mode2) {
            attr = vram_rd(phys(sat - 512 + i * 16 + row, pl));
        } else {
            attr = vram_rd(phys(a + 3, pl));
        }
        if (attr & 0x80) {
            sx -= 32;
        }
        bool cc = mode2 && (attr & 0x40), ic = mode2 && (attr & 0x20);
        if (!cc) {
            group = i;
        } else if (group < 0) {
            continue;
        }
        uint8_t col = attr & 15;
        if (size == 16) {
            pat &= 0xFC;
        }
        uint16_t bits = vram_rd(phys(spg + pat * 8 + row, pl)) << 8;
        if (size == 16) {
            bits |= vram_rd(phys(spg + pat * 8 + 16 + row, pl));
        }
        for (int k = 0; k < size; k++, bits <<= 1) {
            if (!(bits & 0x8000)) {
                continue;
            }
            for (int r = 0; r < mag; r++) {
                int x = sx + k * mag + r;
                if (x < 0 || x >= 256) {
                    continue;
                }
                if (pix[x] & 0x80) {
                    if (!cc && owner[x] == group) {
                        continue;
                    }
                    if (cc && owner[x] == group) {
                        pix[x] |= col;
                    } else if (!cc && !ic && !(pix[x] & 0x40)) {
                        vdp.status[0] |= 0x20;      /* collision */
                    }
                    continue;
                }
                pix[x] = 0x80 | col | (ic || cc ? 0x40 : 0);
                owner[x] = group;
            }
        }
    }
    if (count == 0) {
        return;
    }
    bool transparent0 = !(vdp.v9938 && (vdp.reg[8] & 0x20));
    for (int x = 0; x < 256; x++) {
        if (!(pix[x] & 0x80)) {
            continue;
        }
        int c = pix[x] & 15;
        if (c == 0 && transparent0) {
            continue;
        }
        uint16_t *d = dst + x * 2;
        if (m == MODE_G7) {
            d[0] = d[1] = vdp.rgb332[g7_sprite_color[c]];
        } else if (m == MODE_G5) {
            d[0] = vdp.palette[(c >> 2) & 3];
            d[1] = vdp.palette[c & 3];
        } else {
            d[0] = d[1] = vdp.palette[c];
        }
    }
}

static void draw_line(int row, int dl)
{
    uint16_t *dst = msx.cfg.frame + row * MSX_SCREEN_WIDTH;
    int m = mode();
    if (!vdp.v9938) {
        m &= 0x07;
    }
    uint16_t bd = backdrop(m);
    if (dl < 0 || dl >= active_lines() || !(vdp.reg[1] & 0x40)) {
        fill(dst, MSX_SCREEN_WIDTH, bd);
        return;
    }
    fill(dst, BORDER, bd);
    fill(dst + BORDER + ACTIVE_W, BORDER, bd);
    uint16_t *a = dst + BORDER;
    int y = vdp.v9938 ? (dl + vdp.reg[23]) & 0xFF : dl;
    switch (m) {
    case MODE_T1: draw_text(a, y, false); break;
    case MODE_T2: draw_text(a, y, true); break;
    case MODE_G1: case MODE_G2: case MODE_G3: case MODE_MC: draw_tiles(a, y, m); break;
    case MODE_G4: case MODE_G5: case MODE_G6: case MODE_G7: draw_bitmap(a, y, m); break;
    default: fill(a, ACTIVE_W, bd); break;
    }
    draw_sprites(a, y, m);
}

void vdp_line(int line)
{
    int active = active_lines();
    int top = (MSX_SCREEN_HEIGHT - active) / 2;
    int dl = line - top;

    if (line == 0) {
        if (vdp.v9938 && (vdp.reg[13] & 0xF0)) {
            if (--vdp.blink_count <= 0) {
                vdp.blink_on = !vdp.blink_on;
                vdp.blink_count = (vdp.blink_on ? vdp.reg[13] >> 4 : vdp.reg[13] & 15) * 10;
                vdp.dirty = true;
            }
        } else if (vdp.blink_on) {
            vdp.blink_on = false;
            vdp.dirty = true;
        }
        vdp.draw = vdp.dirty || vdp.draw_next;
        vdp.draw_next = false;
        vdp.dirty = false;
    } else if (vdp.dirty && line < MSX_SCREEN_HEIGHT) {
        /* Changed in the middle: from here on, and the lines above next time */
        vdp.draw = vdp.draw_next = true;
        vdp.dirty = false;
    }
    if (dl == 0) {
        vdp.status[2] &= ~0x40;             /* VR: the display began */
    }
    if (line < MSX_SCREEN_HEIGHT && vdp.draw) {
        draw_line(line, dl);
    }
    if (dl == active - 1) {
        /* The display ended: the frame's interrupt, VR. Sprite status as
         * last drawn when nothing was
         */
        if (vdp.draw) {
            vdp.sprite_status = vdp.status[0] & 0x7F;
        } else {
            vdp.status[0] |= vdp.sprite_status & 0x60;
            if (!(vdp.status[0] & 0x40)) {
                vdp.status[0] = (vdp.status[0] & 0xE0) | (vdp.sprite_status & 0x1F);
            }
        }
        vdp.status[0] |= 0x80;
        vdp.status[2] |= 0x40;
    }
    if (line == MSX_SCREEN_HEIGHT - 1 && vdp.draw) {
        vdp.signature++;
    }
    if (vdp.v9938 && dl >= 0 && dl < 256 && ((dl + vdp.reg[23]) & 0xFF) == vdp.reg[19]) {
        vdp.status[1] |= 0x01;              /* FH: the line interrupt */
    }
}
