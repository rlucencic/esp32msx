/*
 * The MSX: slots and memory, the PPI and keyboard, the I/O ports, the
 * frame loop.
 *
 * Memory goes through the Z80's eight 8 KB banks (CONFIG_Z80_MSX). Each
 * slot or subslot describes its blocks (slot_t); memory_update() puts the
 * selected ones in front of the CPU after any change of the primary slots
 * (PPI port A), a secondary slot register ($FFFF of an expanded slot, the
 * Z80 core hands those accesses over), a mapper segment or a cartridge
 * bank. Blocks with registers in them are NULL and go to the device.
 *
 * A frame is 262 lines (60 Hz) or 313 (50 Hz) of 228 T-states. The Z80
 * gets a wait state on every M1 cycle, as the MSX engine adds one: they
 * are counted from the refresh register, which M1 cycles advance.
 */
#include <stdlib.h>
#include <string.h>
#include "msx_internal.h"

msx_t msx;
slot_t slots[4][4];
uint8_t msx_unmapped[8192];
uint8_t msx_sink[8192];

static slot_t *s_page[4];           /* the slot in each page */
static int s_page3_primary;

static struct {
    char text[1024];
    int pos, len;
    int phase;                      /* frames left of the key down / up */
    int key;
    bool shift;
} s_type;

void *msx_alloc(uint32_t bytes)
{
    return msx.cfg.alloc ? msx.cfg.alloc(bytes) : malloc(bytes);
}

void msx_release(void *p)
{
    if (msx.cfg.release) {
        msx.cfg.release(p);
    } else {
        free(p);
    }
}

/* ---- Memory ---- */

void memory_update(void)
{
    for (int p = 0; p < 4; p++) {
        int primary = (msx.ppi_a >> (p * 2)) & 3;
        int secondary = msx.expanded[primary] ? (msx.sub[primary] >> (p * 2)) & 3 : 0;
        slot_t *s = &slots[primary][secondary];
        s_page[p] = s;
        z80_rd[p * 2] = s->rd[p * 2];
        z80_rd[p * 2 + 1] = s->rd[p * 2 + 1];
        z80_wr[p * 2] = s->wr[p * 2];
        z80_wr[p * 2 + 1] = s->wr[p * 2 + 1];
        if (p == 3) {
            s_page3_primary = primary;
        }
    }
    z80_ffff_special = msx.expanded[s_page3_primary];
}

static uint8_t read_special(uint16_t a)
{
    if (a == 0xFFFF && z80_ffff_special) {
        return ~msx.sub[s_page3_primary];
    }
    slot_t *s = s_page[a >> 14];
    if (s->rd[a >> 13]) {
        return s->rd[a >> 13][a & 0x1FFF];
    }
    return s->read ? s->read(a) : 0xFF;
}

static void write_special(uint16_t a, uint8_t v)
{
    if (a == 0xFFFF && z80_ffff_special) {
        msx.sub[s_page3_primary] = v;
        memory_update();
        return;
    }
    slot_t *s = s_page[a >> 14];
    if (s->write) {
        s->write(a, v);
    }
}

/* MSX2: the mapper's segments into slot 3-0 */
static void mapper_update(void)
{
    slot_t *s = &slots[3][0];
    for (int p = 0; p < 4; p++) {
        uint8_t *seg = msx.cfg.ram + (uint32_t)(msx.mapper[p] % msx.mapper_segments) * 0x4000;
        s->rd[p * 2] = s->wr[p * 2] = seg;
        s->rd[p * 2 + 1] = s->wr[p * 2 + 1] = seg + 0x2000;
    }
}

static void slot_empty(slot_t *s)
{
    for (int b = 0; b < 8; b++) {
        s->rd[b] = msx_unmapped;
        s->wr[b] = msx_sink;
    }
    s->read = NULL;
    s->write = NULL;
}

static void slot_rom(slot_t *s, const uint8_t *rom, int first_block, int blocks)
{
    for (int b = 0; b < blocks; b++) {
        s->rd[first_block + b] = (uint8_t *)rom + b * 0x2000;
    }
}

/* The disk ROM in page 1, its last block with the controller's registers */
static uint8_t disk_read(uint16_t a)
{
    if (a >= 0x7FF8 && a <= 0x7FFF) {
        return fdc_read(a);
    }
    if (a >= 0x4000 && a < 0x8000) {
        return msx.cfg.disk_rom[a - 0x4000];
    }
    return 0xFF;
}

static void disk_write(uint16_t a, uint8_t v)
{
    if (a >= 0x7FF8 && a <= 0x7FFF) {
        fdc_write(a, v);
    }
}

static void setup_slots(void)
{
    for (int p = 0; p < 4; p++) {
        for (int s = 0; s < 4; s++) {
            slot_empty(&slots[p][s]);
        }
        msx.expanded[p] = false;
    }
    slot_rom(&slots[0][0], msx.cfg.bios, 0, 4);
    if (cart_present()) {
        cart_setup(&slots[1][0]);
    }
    msx.expanded[3] = true;
    slot_t *ram = &slots[3][0];
    if (msx.cfg.model == MSX_MODEL_MSX2) {
        mapper_update();
    } else {
        for (int b = 0; b < 8; b++) {
            ram->rd[b] = ram->wr[b] = msx.cfg.ram + b * 0x2000;
        }
    }
    if (msx.cfg.model == MSX_MODEL_MSX2 && msx.cfg.sub_rom) {
        slot_rom(&slots[3][1], msx.cfg.sub_rom, 0, 2);
    }
    if (msx.cfg.disk_rom) {
        slot_t *d = &slots[3][2];
        slot_rom(d, msx.cfg.disk_rom, 2, 1);
        d->rd[3] = NULL;                    /* $6000-$7FFF: the registers at the end */
        d->wr[3] = NULL;
        d->read = disk_read;
        d->write = disk_write;
    }
    if (ide_present()) {
        /* in slot 2, before the disk ROM: Nextor starts first and runs
         * the floppies too (behind the DOS1 disk ROM as master, its FAT16
         * hard disk gave "Disk I/O error")
         */
        ide_setup(&slots[2][0]);
    }
}

/* ---- I/O ---- */

static uint8_t keyboard_row(void)
{
    int row = msx.ppi_c & 0x0F;
    return row < 11 ? msx.keys[row] : 0xFF;
}

static uint8_t io_in(uint16_t port, int at)
{
    switch (port & 0xFF) {
    case 0x98: case 0x99:
        return vdp_read(port);
    case 0x9A: case 0x9B:
        return msx.cfg.model == MSX_MODEL_MSX2 ? vdp_read(port) : 0xFF;
    case 0xA2:
        return psg_read();
    case 0xA8:
        return msx.ppi_a;
    case 0xA9:
        return keyboard_row();
    case 0xAA:
        return msx.ppi_c;
    case 0xB5:
        return rtc_read();
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        if (msx.cfg.model == MSX_MODEL_MSX2) {
            return msx.mapper[port & 3] | (uint8_t)~(msx.mapper_segments - 1);
        }
        return 0xFF;
    default:
        return 0xFF;
    }
}

static void ppi_c_write(uint8_t v, int at)
{
    uint8_t old = msx.ppi_c;
    msx.ppi_c = v;
    if ((old ^ v) & 0x80) {
        click_write(v & 0x80);
    }
}

static void io_out(uint16_t port, uint8_t v, int at)
{
    switch (port & 0xFF) {
    case 0x98: case 0x99:
        vdp_write(port, v);
        break;
    case 0x9A: case 0x9B:
        if (msx.cfg.model == MSX_MODEL_MSX2) {
            vdp_write(port, v);
        }
        break;
    case 0xA0:
        psg_address(v);
        break;
    case 0xA1:
        psg_write(v, at);
        break;
    case 0xA8:
        msx.ppi_a = v;
        memory_update();
        break;
    case 0xAA:
        ppi_c_write(v, at);
        break;
    case 0xAB:
        if (!(v & 0x80)) {                  /* bit set/reset on port C */
            int bit = (v >> 1) & 7;
            ppi_c_write((v & 1) ? msx.ppi_c | (1 << bit) : msx.ppi_c & ~(1 << bit), at);
        }
        break;
    case 0xB4:
        rtc_index(v);
        break;
    case 0xB5:
        rtc_write(v);
        break;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
        if (msx.cfg.model == MSX_MODEL_MSX2) {
            msx.mapper[port & 3] = v;
            mapper_update();
            memory_update();
        }
        break;
    default:
        break;
    }
}

static uint8_t int_ack(void)
{
    return 0xFF;
}

static const z80_bus_t s_bus = {
    .in = io_in, .out = io_out, .int_ack = int_ack,
    .read_special = read_special, .write_special = write_special,
};

/* ---- Keyboard ---- */

void msx_key(int key, bool down)
{
    if (key < 0 || key >= MSX_KEY_COUNT) {
        return;
    }
    uint8_t bit = 1 << (key & 7);
    if (down) {
        msx.keys[key >> 3] &= ~bit;
    } else {
        msx.keys[key >> 3] |= bit;
    }
}

void msx_joystick(int port, uint8_t bits)
{
    if (port == 1 || port == 2) {
        msx.joy[port - 1] = ~bits & 0x3F;
    }
}

/* The international layout: rows 0-5 */
static const char s_plain[49] = "01234567" "89-=\\[];" "'`,./\0ab" "cdefghij" "klmnopqr" "stuvwxyz";
static const char s_shifted[49] = ")!@#$%^&" "*(_+|{}:" "\"~<>?\0AB" "CDEFGHIJ" "KLMNOPQR" "STUVWXYZ";

int msx_ascii_key(char c, bool *shift)
{
    *shift = false;
    switch (c) {
    case '\n': case '\r': return MSX_KEY_RETURN;
    case ' ': return MSX_KEY_SPACE;
    case '\b': return MSX_KEY_BS;
    case '\t': return MSX_KEY_TAB;
    case 27: return MSX_KEY_ESC;
    default: break;
    }
    if (c == 0) {
        return -1;
    }
    for (int i = 0; i < 48; i++) {
        if (s_plain[i] == c) {
            return i;
        }
        if (s_shifted[i] == c) {
            *shift = true;
            return i;
        }
    }
    return -1;
}

void msx_type(const char *text)
{
    snprintf(s_type.text, sizeof(s_type.text), "%s", text);
    s_type.len = strlen(s_type.text);
    s_type.pos = 0;
    s_type.phase = 0;
    s_type.key = -1;
}

bool msx_typing(void)
{
    return s_type.pos < s_type.len || s_type.key >= 0;
}

/* Each key held for 2 frames, then 2 frames up: the BIOS scans the keyboard
 * once a frame
 */
static void type_frame(void)
{
    if (s_type.phase > 0) {
        if (--s_type.phase > 0) {
            return;
        }
        if (s_type.key >= 0) {
            msx_key(s_type.key, false);
            if (s_type.shift) {
                msx_key(MSX_KEY_SHIFT, false);
            }
            s_type.key = -1;
            s_type.phase = 2;
            return;
        }
    }
    while (s_type.pos < s_type.len) {
        bool shift;
        int k = msx_ascii_key(s_type.text[s_type.pos++], &shift);
        if (k < 0) {
            continue;
        }
        s_type.key = k;
        s_type.shift = shift;
        if (shift) {
            msx_key(MSX_KEY_SHIFT, true);
        }
        msx_key(k, true);
        s_type.phase = 2;
        return;
    }
}

/* ---- Running ---- */

static void power_on(void)
{
    memset(msx.keys, 0xFF, sizeof(msx.keys));
    msx.joy[0] = msx.joy[1] = 0x3F;
    msx.ppi_a = 0;
    msx.ppi_c = 0x70;
    memset(msx.sub, 0, sizeof(msx.sub));
    for (int p = 0; p < 4; p++) {
        msx.mapper[p] = 3 - p;              /* as the BIOS sets them */
    }
    setup_slots();
    memory_update();
    vdp_reset();
    psg_reset();
    scc_reset();
    fdc_reset();
    ide_reset();
    cart_reset();
    rtc_reset();
    z80_reset();
    s_type.pos = s_type.len = 0;
    s_type.key = -1;
}

bool msx_init(const msx_config_t *cfg)
{
    if (cfg->bios == NULL || cfg->ram == NULL || cfg->vram == NULL || cfg->frame == NULL) {
        return false;
    }
    if (cfg->model == MSX_MODEL_MSX2 && cfg->sub_rom == NULL) {
        return false;
    }
    msx.cfg = *cfg;
    if (msx.cfg.model == MSX_MODEL_MSX1) {
        msx.cfg.ram_kb = 64;
    }
    uint32_t segments = msx.cfg.ram_kb / 16;
    uint32_t pow2 = 1;
    while (pow2 < segments) {
        pow2 <<= 1;
    }
    msx.mapper_segments = pow2 > 256 ? 256 : pow2;
    memset(msx_unmapped, 0xFF, sizeof(msx_unmapped));
    memset(msx.cfg.ram, 0, msx.cfg.ram_kb * 1024);
    msx.pal = cfg->hz50;
    msx.clock = msx.line_start = 0;
    msx.frames = 0;
    vdp_init(msx.cfg.model == MSX_MODEL_MSX2, msx.pal);
    audio_init();
    z80_set_bus(&s_bus);
    power_on();
    return true;
}

void msx_reset(void)
{
    memset(msx.cfg.ram, 0, msx.cfg.ram_kb * 1024);
    power_on();
}

void MSX_HOT msx_run_frame(void)
{
    int lines = vdp_lines();
    for (int line = 0; line < lines; line++) {
        uint64_t end = msx.line_start + LINE_T;
        while (msx.clock < end) {
            z80.int_line = vdp_irq();
#ifdef MSX_HOST_HOOK
            msx_host_hook();
#endif
            uint8_t r = z80.r;
            int t = z80_step();
            msx.clock += t + ((z80.r - r) & 0x7F);  /* + the M1 wait states */
        }
        msx.line_start = end;
        vdp_line(line);
    }
    audio_frame_end();
    type_frame();
    msx.frames++;
}

int msx_screen_hz(void)
{
    return vdp_lines() == 313 ? 50 : 60;
}

uint32_t msx_screen_signature(void)
{
    return vdp_signature();
}

const uint16_t *msx_line(int y)
{
    return msx.cfg.frame + (y < 0 ? 0 : y >= MSX_SCREEN_HEIGHT ? MSX_SCREEN_HEIGHT - 1 : y) * MSX_SCREEN_WIDTH;
}

void msx_get_status(msx_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->frames = msx.frames;
    s->pc = z80.pc.w;
    s->slots = msx.ppi_a;
    s->vdp_mode = vdp_mode();
    fdc_status(s);
    ide_status(s);
}
