/*
 * ROM cartridges in slot 1: plain ROMs of up to 64 KB, placed by their
 * header, and the MegaROM mappers, guessed from the code as fMSX does
 * (counting LD (nnnn),A to each kind's bank registers):
 *   ASCII 8      8 KB banks at $4000-$BFFF, registers $6000/$6800/$7000/$7800
 *   ASCII 16     16 KB banks at $4000, $8000, registers $6000/$7000
 *   Konami       8 KB banks, $4000 fixed, registers $6000/$8000/$A000
 *   Konami SCC   8 KB banks, registers $5000/$7000/$9000/$B000; bank 3F
 *                written at $9000 shows the SCC at $9800-$98FF
 */
#include <stdlib.h>
#include <string.h>
#include "msx_internal.h"

enum { CART_PLAIN, CART_ASCII8, CART_ASCII16, CART_KONAMI, CART_KONAMI_SCC };

static const char *const type_names[] = { "Plain", "ASCII 8", "ASCII 16", "Konami", "Konami SCC" };

static struct {
    char path[192];
    uint8_t *rom;
    uint32_t size;
    uint32_t banks;             /* 8 KB banks (a power of two) */
    int type;
    int base;                   /* plain: first block */
    uint8_t bank[4];            /* blocks 2-5 ($4000-$BFFF) */
    bool scc;                   /* the SCC is mapped */
    slot_t *slot;
} cart;

bool cart_present(void)
{
    return cart.rom != NULL;
}

static uint8_t *block(int b)
{
    return cart.rom + (uint32_t)(b & (cart.banks - 1)) * 0x2000;
}

static void banks_update(void)
{
    slot_t *s = cart.slot;
    if (cart.type == CART_ASCII16) {
        for (int i = 0; i < 2; i++) {
            s->rd[2 + i * 2] = block(cart.bank[i] * 2);
            s->rd[3 + i * 2] = block(cart.bank[i] * 2 + 1);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            s->rd[2 + i] = block(cart.bank[i]);
        }
    }
    if (cart.type == CART_KONAMI_SCC && cart.scc) {
        s->rd[4] = NULL;                    /* $8000-$9FFF: the SCC in it */
    }
    for (int b = 2; b < 6; b++) {
        s->wr[b] = NULL;                    /* bank registers */
    }
    memory_update();
}

static uint8_t mapper_read(uint16_t a)
{
    if (cart.type == CART_KONAMI_SCC && cart.scc && a >= 0x9800 && a < 0xA000) {
        return scc_read(a);
    }
    if (a >= 0x4000 && a < 0xC000) {
        int i = (a - 0x4000) >> 13;
        if (cart.type == CART_ASCII16) {
            return block(cart.bank[i >> 1] * 2 + (i & 1))[a & 0x1FFF];
        }
        return block(cart.bank[i])[a & 0x1FFF];
    }
    return 0xFF;
}

static void mapper_write(uint16_t a, uint8_t v)
{
    switch (cart.type) {
    case CART_ASCII8:
        if (a >= 0x6000 && a < 0x8000) {
            cart.bank[(a >> 11) & 3] = v;
            banks_update();
        }
        break;
    case CART_ASCII16:
        if (a >= 0x6000 && a < 0x6800) {
            cart.bank[0] = v;
            banks_update();
        } else if (a >= 0x7000 && a < 0x7800) {
            cart.bank[1] = v;
            banks_update();
        }
        break;
    case CART_KONAMI:
        if (a >= 0x6000 && a < 0xC000) {
            cart.bank[(a - 0x4000) >> 13] = v;
            banks_update();
        }
        break;
    case CART_KONAMI_SCC:
        if (cart.scc && a >= 0x9800 && a < 0xA000) {
            scc_write(a, v);
            return;
        }
        if ((a & 0x1800) == 0x1000 && a >= 0x5000 && a < 0xC000) {
            int i = (a - 0x4000) >> 13;
            cart.bank[i] = v;
            if (i == 2) {
                cart.scc = (v & 0x3F) == 0x3F;
            }
            banks_update();
        }
        break;
    default:
        break;
    }
}

static int guess(const uint8_t *rom, uint32_t size)
{
    int count[5] = { 0 };
    for (uint32_t i = 0; i + 2 < size; i++) {
        if (rom[i] != 0x32) {
            continue;
        }
        switch (rom[i + 1] | (rom[i + 2] << 8)) {
        case 0x5000: case 0x9000: case 0xB000:
            count[CART_KONAMI_SCC]++;
            break;
        case 0x4000: case 0x8000: case 0xA000:
            count[CART_KONAMI]++;
            break;
        case 0x6800: case 0x7800:
            count[CART_ASCII8]++;
            break;
        case 0x6000:
            count[CART_KONAMI]++;
            count[CART_ASCII8]++;
            count[CART_ASCII16]++;
            break;
        case 0x7000:
            count[CART_KONAMI_SCC]++;
            count[CART_ASCII8]++;
            count[CART_ASCII16]++;
            break;
        case 0x77FF:
            count[CART_ASCII16]++;
            break;
        default:
            break;
        }
    }
    static const int order[] = { CART_KONAMI_SCC, CART_KONAMI, CART_ASCII8, CART_ASCII16 };
    int best = CART_ASCII8;
    for (int i = 0; i < 4; i++) {
        if (count[order[i]] > count[best]) {
            best = order[i];
        }
    }
    /* $6000/$7000 alone are ASCII 16's: ASCII 8 also writes $6800/$7800 */
    if (best == CART_ASCII8 && count[CART_ASCII16] >= count[CART_ASCII8]) {
        best = CART_ASCII16;
    }
    return best;
}

void cart_setup(slot_t *s)
{
    cart.slot = s;
    s->read = mapper_read;
    s->write = mapper_write;
    if (cart.type != CART_PLAIN) {
        return;                             /* cart_reset() sets the banks */
    }
    /* Plain: 8 KB twice, 16 KB, 32 KB from the header's page, 48/64 KB from 0 */
    int blocks = (cart.size + 0x1FFF) / 0x2000;
    for (int b = 0; b < blocks && cart.base + b < 8; b++) {
        s->rd[cart.base + b] = cart.rom + b * 0x2000;
    }
    if (blocks == 1) {
        s->rd[cart.base + 1] = cart.rom;
    }
}

void cart_reset(void)
{
    if (!cart.rom || cart.type == CART_PLAIN || !cart.slot) {
        return;
    }
    cart.scc = false;
    for (int i = 0; i < 4; i++) {
        cart.bank[i] = cart.type == CART_ASCII8 || cart.type == CART_ASCII16 ? 0 : i;
    }
    banks_update();
}

bool msx_cart_insert(const char *path)
{
    msx_cart_eject();
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 8192 || size > 4 * 1024 * 1024) {
        fclose(fp);
        return false;
    }
    uint32_t alloc = 0x2000;
    while (alloc < (uint32_t)size) {
        alloc <<= 1;
    }
    uint8_t *rom = msx_alloc(alloc);
    if (rom == NULL) {
        fclose(fp);
        return false;
    }
    memset(rom, 0xFF, alloc);
    bool ok = fread(rom, 1, size, fp) == (size_t)size;
    fclose(fp);
    if (!ok) {
        msx_release(rom);
        return false;
    }
    cart.rom = rom;
    cart.size = size;
    cart.banks = alloc / 0x2000;
    snprintf(cart.path, sizeof(cart.path), "%s", path);
    if (size <= 65536) {
        cart.type = CART_PLAIN;
        uint16_t init = rom[2] | (rom[3] << 8);
        if (size <= 32768) {
            /* "AB" at the start: from $4000, or $8000 if it starts there */
            cart.base = (rom[0] == 'A' && rom[1] == 'B' && init >= 0x8000 && init < 0xC000 && size <= 16384) ? 4 : 2;
            if (size > 16384 && !(rom[0] == 'A' && rom[1] == 'B') && rom[0x4000] == 'A' && rom[0x4001] == 'B') {
                cart.base = 0;
            }
        } else {
            cart.base = (rom[0] == 'A' && rom[1] == 'B') ? 2 : 0;
            if (cart.base == 2 && size > 49152) {
                cart.base = 0;
            }
        }
    } else {
        cart.type = guess(rom, size);
    }
    return true;
}

void msx_cart_eject(void)
{
    if (cart.rom) {
        msx_release(cart.rom);
        cart.rom = NULL;
    }
    cart.path[0] = 0;
}

const char *msx_cart_path(void)
{
    return cart.rom ? cart.path : NULL;
}

const char *msx_cart_type(void)
{
    return cart.rom ? type_names[cart.type] : NULL;
}
