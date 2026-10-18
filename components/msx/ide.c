/*
 * The Sunrise IDE interface with Nextor (slot 2), after openMSX's
 * SunriseIDE: a 128 KB flash ROM seen 16 KB at a time in page 1, its bank
 * and the IDE registers' enable in the control register (written at $4104:
 * bits 7-3 reversed are the bank, bit 0 the registers), and then
 *   $7C00-$7DFF  the data register, 16 bits through even (low) and odd
 *                (high) addresses
 *   $7E00-$7E0F  the task file (7: status/command, 14: alternate status /
 *                device control)
 * One master device: a raw image of 512-byte sectors on the SD card, read
 * and written in place, up to 64 sectors at once.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "msx_internal.h"

#define BATCH           64          /* sectors read at once */

#define ATA_ERR         0x01
#define ATA_DRQ         0x08
#define ATA_DSC         0x10
#define ATA_DRDY        0x40
#define ATA_BSY         0x80
#define ATA_ABRT        0x04

static struct {
    char path[192];
    FILE *fp;
    uint32_t sectors;
    bool read_only;
    uint8_t control;
    slot_t *slot;
    /* ATA */
    uint8_t error, features, count, lba0, lba1, lba2, devhead, status, devctl;
    uint8_t *buf;
    int pos, len;               /* bytes in buf */
    uint32_t lba;               /* the next sector */
    int left;                   /* sectors still to come in the command */
    bool writing;
    uint8_t read_latch, write_latch;
    uint32_t reads, writes;
    bool busy;                  /* a transfer is going on (for the status) */
} ide;

bool ide_present(void)
{
    return msx.cfg.nextor && ide.fp;
}

static inline bool slave(void)
{
    return ide.devhead & 0x10;
}

static uint32_t current_lba(void)
{
    if (ide.devhead & 0x40) {
        return ide.lba0 | (ide.lba1 << 8) | (ide.lba2 << 16) | ((uint32_t)(ide.devhead & 0x0F) << 24);
    }
    /* CHS: 16 heads, 63 sectors a track */
    uint32_t cyl = ide.lba1 | (ide.lba2 << 8), head = ide.devhead & 0x0F, sec = ide.lba0;
    return (cyl * 16 + head) * 63 + (sec ? sec - 1 : 0);
}

static void set_lba(uint32_t lba)
{
    if (ide.devhead & 0x40) {
        ide.lba0 = lba;
        ide.lba1 = lba >> 8;
        ide.lba2 = lba >> 16;
        ide.devhead = (ide.devhead & 0xF0) | ((lba >> 24) & 0x0F);
    } else {
        uint32_t cyl = lba / (16 * 63), r = lba % (16 * 63);
        ide.lba1 = cyl;
        ide.lba2 = cyl >> 8;
        ide.devhead = (ide.devhead & 0xF0) | (r / 63);
        ide.lba0 = r % 63 + 1;
    }
}

static void ata_done(uint8_t error)
{
    ide.error = error;
    ide.status = ATA_DRDY | ATA_DSC | (error ? ATA_ERR : 0);
    ide.len = ide.pos = 0;
    ide.left = 0;
    ide.busy = false;
}

static void ata_signature(void)
{
    ide.error = 0x01;
    ide.count = ide.lba0 = 1;
    ide.lba1 = ide.lba2 = 0;
    ide.devhead &= 0x10;
    ide.status = ATA_DRDY | ATA_DSC;
}

/* The next batch of sectors into buf */
static bool read_batch(void)
{
    int n = ide.left < BATCH ? ide.left : BATCH;
    if (ide.lba + n > ide.sectors || fseek(ide.fp, (long)ide.lba * 512, SEEK_SET) != 0 ||
        fread(ide.buf, 512, n, ide.fp) != (size_t)n) {
        return false;
    }
    ide.pos = 0;
    ide.len = n * 512;
    ide.reads += n;
    return true;
}

static void put_string(uint16_t *w, const char *s, int words)
{
    for (int i = 0; i < words; i++) {
        char a = *s ? *s++ : ' ';
        char b = *s ? *s++ : ' ';
        w[i] = (uint8_t)a << 8 | (uint8_t)b;
    }
}

static void identify(void)
{
    uint16_t w[256];
    memset(w, 0, sizeof(w));
    uint32_t cyl = ide.sectors / (16 * 63);
    if (cyl > 16383) {
        cyl = 16383;
    }
    w[0] = 0x0040;
    w[1] = cyl;
    w[3] = 16;
    w[6] = 63;
    put_string(w + 10, "ESP32MSX0001", 10);
    put_string(w + 23, "1.0", 4);
    put_string(w + 27, "ESP32MSX HARD DISK", 20);
    w[47] = 0x8001;
    w[49] = 0x0200;                         /* LBA */
    w[53] = 0x0001;
    w[54] = cyl;
    w[55] = 16;
    w[56] = 63;
    uint32_t chs = cyl * 16 * 63;
    w[57] = chs;
    w[58] = chs >> 16;
    w[60] = ide.sectors;
    w[61] = ide.sectors >> 16;
    for (int i = 0; i < 256; i++) {
        ide.buf[i * 2] = w[i];
        ide.buf[i * 2 + 1] = w[i] >> 8;
    }
    ide.pos = 0;
    ide.len = 512;
    ide.left = 1;
    ide.writing = false;
    ide.status = ATA_DRDY | ATA_DSC | ATA_DRQ;
}

static void ata_command(uint8_t cmd)
{
    if (slave()) {
        return;
    }
    int count = ide.count ? ide.count : 256;
    switch (cmd) {
    case 0xEC:                              /* IDENTIFY DEVICE */
        identify();
        return;
    case 0x20: case 0x21: case 0xC4:        /* READ SECTORS, READ MULTIPLE */
        ide.lba = current_lba();
        ide.left = count;
        ide.writing = false;
        if (!read_batch()) {
            ata_done(0x10);                 /* IDNF */
            return;
        }
        ide.busy = true;
        ide.status = ATA_DRDY | ATA_DSC | ATA_DRQ;
        return;
    case 0x30: case 0x31: case 0xC5:        /* WRITE SECTORS, WRITE MULTIPLE */
        if (ide.read_only) {
            ata_done(ATA_ABRT);
            return;
        }
        ide.lba = current_lba();
        ide.left = count;
        ide.writing = true;
        ide.pos = 0;
        ide.len = 512;
        ide.busy = true;
        ide.status = ATA_DRDY | ATA_DSC | ATA_DRQ;
        return;
    case 0x90:                              /* EXECUTE DEVICE DIAGNOSTIC */
        ata_signature();
        return;
    case 0xE5:                              /* CHECK POWER MODE */
        ide.count = 0xFF;
        ata_done(0);
        return;
    case 0x40: case 0x41: case 0x70: case 0x91: case 0xC6: case 0xEF:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6: case 0xE7:
        ata_done(0);                        /* verify, seek, parameters, features, power: fine */
        return;
    default:
        if ((cmd & 0xF0) == 0x10) {         /* RECALIBRATE */
            ata_done(0);
            return;
        }
        ata_done(ATA_ABRT);
        return;
    }
}

static uint16_t data_read(void)
{
    if (slave() || !(ide.status & ATA_DRQ) || ide.writing) {
        return 0xFFFF;
    }
    uint16_t v = ide.buf[ide.pos] | (ide.buf[ide.pos + 1] << 8);
    ide.pos += 2;
    if (ide.pos >= ide.len) {
        int n = ide.len / 512;
        ide.lba += n;
        ide.left -= n;
        set_lba(ide.lba);
        if (ide.left > 0 && read_batch()) {
            return v;
        }
        ata_done(ide.left > 0 ? 0x10 : 0);
    }
    return v;
}

static void data_write(uint16_t v)
{
    if (slave() || !(ide.status & ATA_DRQ) || !ide.writing) {
        return;
    }
    ide.buf[ide.pos] = v;
    ide.buf[ide.pos + 1] = v >> 8;
    ide.pos += 2;
    if (ide.pos < 512) {
        return;
    }
    bool ok = ide.lba < ide.sectors && fseek(ide.fp, (long)ide.lba * 512, SEEK_SET) == 0 &&
              fwrite(ide.buf, 512, 1, ide.fp) == 1;
    ide.writes++;
    ide.lba++;
    ide.left--;
    set_lba(ide.lba);
    ide.pos = 0;
    if (!ok) {
        ata_done(0x10);
    } else if (ide.left <= 0) {
        ata_done(0);
    }
}

static uint8_t reg_read(int reg)
{
    if (reg == 14) {
        reg = 7;
    }
    if (slave() && reg != 6) {
        return 0x00;                        /* no slave */
    }
    switch (reg) {
    case 0: return data_read() & 0xFF;
    case 1: return ide.error;
    case 2: return ide.count;
    case 3: return ide.lba0;
    case 4: return ide.lba1;
    case 5: return ide.lba2;
    case 6: return ide.devhead | 0xA0;
    case 7: return ide.status;
    default: return 0xFF;
    }
}

static void reg_write(int reg, uint8_t v)
{
    switch (reg) {
    case 0: data_write(v | (v << 8)); break;
    case 1: ide.features = v; break;
    case 2: ide.count = v; break;
    case 3: ide.lba0 = v; break;
    case 4: ide.lba1 = v; break;
    case 5: ide.lba2 = v; break;
    case 6: ide.devhead = v; break;
    case 7: ata_command(v); break;
    case 14:
        if ((v & 0x04) && !(ide.devctl & 0x04)) {
            ata_signature();                /* software reset */
            ide.busy = false;
        }
        ide.devctl = v;
        break;
    default: break;
    }
}

/* ---- The cartridge side ---- */

static uint8_t reverse(uint8_t b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    return (b & 0xAA) >> 1 | (b & 0x55) << 1;
}

static void control_write(uint8_t v)
{
    ide.control = v;
    int bank = reverse(v & 0xF8) & 7;
    const uint8_t *p = msx.cfg.nextor + bank * 0x4000;
    slot_t *s = ide.slot;
    s->rd[2] = (uint8_t *)p;
    s->rd[3] = (v & 1) ? NULL : (uint8_t *)p + 0x2000;     /* the registers at $7C00 */
    memory_update();
}

static uint8_t sunrise_read(uint16_t a)
{
    if ((ide.control & 1) && (a & 0xFE00) == 0x7C00) {
        if (!(a & 1)) {
            uint16_t v = data_read();
            ide.read_latch = v >> 8;
            return v;
        }
        return ide.read_latch;
    }
    if ((ide.control & 1) && (a & 0xFF00) == 0x7E00) {
        return reg_read(a & 0x0F);
    }
    if (a >= 0x4000 && a < 0x8000) {
        int bank = reverse(ide.control & 0xF8) & 7;
        return msx.cfg.nextor[bank * 0x4000 + (a & 0x3FFF)];
    }
    return 0xFF;
}

static void sunrise_write(uint16_t a, uint8_t v)
{
    if ((a & 0xBF04) == 0x0104) {
        control_write(v);
        return;
    }
    if ((ide.control & 1) && (a & 0xFE00) == 0x7C00) {
        if (!(a & 1)) {
            ide.write_latch = v;
        } else {
            data_write(ide.write_latch | (v << 8));
        }
        return;
    }
    if ((ide.control & 1) && (a & 0xFF00) == 0x7E00) {
        reg_write(a & 0x0F, v);
    }
}

void ide_setup(slot_t *slot)
{
    ide.slot = slot;
    slot->rd[2] = (uint8_t *)msx.cfg.nextor;
    slot->rd[3] = (uint8_t *)msx.cfg.nextor + 0x2000;
    slot->wr[2] = slot->wr[3] = NULL;
    slot->read = sunrise_read;
    slot->write = sunrise_write;
}

void ide_reset(void)
{
    ide.control = 0;
    ide.devhead = 0;
    ide.devctl = 0;
    ata_signature();
    ide.len = ide.pos = ide.left = 0;
    ide.busy = false;
    if (ide.slot && ide_present()) {
        control_write(0);
    }
}

void ide_status(msx_status_t *s)
{
    s->hd_reads = ide.reads;
    s->hd_writes = ide.writes;
    s->hd_busy = ide.busy;
}

/* ---- The image ---- */

bool msx_hd_insert(const char *path)
{
    msx_hd_eject();
    if (ide.buf == NULL) {
        ide.buf = msx_alloc(BATCH * 512);
        if (ide.buf == NULL) {
            return false;
        }
    }
    FILE *fp = fopen(path, "r+b");
    bool read_only = false;
    if (fp == NULL) {
        fp = fopen(path, "rb");
        read_only = true;
    }
    if (fp == NULL) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (size < 1024 * 1024) {
        fclose(fp);
        return false;
    }
    ide.fp = fp;
    ide.sectors = size / 512;
    ide.read_only = read_only;
    snprintf(ide.path, sizeof(ide.path), "%s", path);
    return true;
}

void msx_hd_eject(void)
{
    if (ide.fp) {
        fclose(ide.fp);
        ide.fp = NULL;
    }
    ide.path[0] = 0;
}

const char *msx_hd_path(void)
{
    return ide.fp ? ide.path : NULL;
}

bool msx_hd_flush(void)
{
    return ide.fp == NULL || (fflush(ide.fp) == 0 && fsync(fileno(ide.fp)) == 0);
}

static void le16(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v);
    le16(p + 2, v >> 16);
}

/* A blank image: an MBR and one FAT16 partition filling it */
bool msx_hd_create(const char *path, uint32_t bytes)
{
    const uint32_t start = 63;
    uint32_t total = bytes / 512;
    if (total < 8192) {
        return false;
    }
    uint32_t sectors = total - start;
    int sc = 1;
    while (sectors / sc > 65000) {
        sc *= 2;
    }
    uint32_t clusters = sectors / sc;
    uint32_t fat = (clusters * 2 + 2 * 2 + 511) / 512;
    uint32_t root = 512 * 32 / 512;
    uint32_t meta = start + 1 + 2 * fat + root;
    uint8_t *img = calloc(meta, 512);
    if (img == NULL) {
        return false;
    }
    /* MBR */
    uint8_t *e = img + 0x1BE;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[4] = sectors < 65536 ? 0x04 : 0x06;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    le32(e + 8, start);
    le32(e + 12, sectors);
    img[510] = 0x55;
    img[511] = 0xAA;
    /* The partition's boot sector */
    uint8_t *b = img + start * 512;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "ESP32MSX", 8);
    le16(b + 0x0B, 512);
    b[0x0D] = sc;
    le16(b + 0x0E, 1);
    b[0x10] = 2;
    le16(b + 0x11, 512);
    if (sectors < 65536) {
        le16(b + 0x13, sectors);
    } else {
        le32(b + 0x20, sectors);
    }
    b[0x15] = 0xF8;
    le16(b + 0x16, fat);
    le16(b + 0x18, 63);
    le16(b + 0x1A, 16);
    le32(b + 0x1C, start);
    b[0x24] = 0x80;
    b[0x26] = 0x29;
    le32(b + 0x27, 0x12345678 ^ bytes);
    memcpy(b + 0x2B, "NO NAME    FAT16   ", 19);
    b[510] = 0x55;
    b[511] = 0xAA;
    for (int i = 0; i < 2; i++) {
        uint8_t *f = img + (start + 1 + i * fat) * 512;
        f[0] = 0xF8; f[1] = 0xFF; f[2] = 0xFF; f[3] = 0xFF;
    }
    FILE *fp = fopen(path, "wb");
    bool ok = fp && fwrite(img, 512, meta, fp) == meta;
    free(img);
    /* The data area needs no clearing: the FAT says it's free */
    ok = ok && fseek(fp, (long)total * 512 - 1, SEEK_SET) == 0 && fputc(0, fp) == 0;
    ok = fp && fclose(fp) == 0 && ok;
    return ok;
}
