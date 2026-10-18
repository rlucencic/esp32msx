/*
 * The floppy interface of the disk ROM's slot, as Philips and Sony built
 * it: a WD2793 at $7FF8-$7FFB, the side at $7FFC, drive and motor at
 * $7FFD, and $7FFF reading the controller's INTRQ (bit 6) and DRQ (bit 7),
 * both active low, and the drive's: bit 4 set while its motor is off (it
 * runs on for 3 s after it's switched off; the common DISK.ROM waits a
 * second for it to spin up), bit 5 clear once after a disk change (else
 * DSKCHG says "not changed").
 * Drives A: and B: with .dsk images (raw sectors).
 *
 * The CPU moves every byte through the data register, polling DRQ: here a
 * sector's next byte is there as soon as the last one was taken. Commands
 * take a little time (steps 1 ms each, the rest 0.1 ms) before they're
 * done; the index pulse comes round every 200 ms while a disk turns.
 * Write Track (formatting) is parsed for its ID and data fields.
 */
#include <stdlib.h>
#include <string.h>
#include "msx_internal.h"

#define MS              (MSX_CLOCK_HZ / 1000)
#define REVOLUTION      (200 * MS)
#define INDEX_PULSE     (4 * MS)
#define TRACK_BYTES     6250

#define ST_BUSY         0x01
#define ST_INDEX        0x02        /* type I */
#define ST_DRQ          0x02        /* type II/III */
#define ST_TRACK0       0x04
#define ST_LOST         0x04
#define ST_CRC          0x08
#define ST_RNF          0x10
#define ST_HEAD         0x20
#define ST_PROTECT      0x40
#define ST_NOT_READY    0x80

typedef struct {
    char path[192];
    uint8_t *data;
    uint32_t size;
    int sides, spt, tracks;
    bool read_only, dirty;
    bool changed;                   /* a disk went in or out since $7FFF last said so */
    int head;                       /* where the head is */
    uint32_t reads, writes;
} floppy_t;

static floppy_t s_fd[MSX_FLOPPIES];

enum { XFER_NONE, XFER_READ, XFER_WRITE, XFER_ADDRESS, XFER_WRITE_TRACK, XFER_READ_TRACK };

static struct {
    uint8_t command, status, track, sector, data;
    uint8_t side, drive_reg;
    bool type1, intrq, drq;
    uint64_t done_at;               /* the command finishes */
    int xfer;
    uint8_t buf[TRACK_BYTES];
    int pos, len;
    bool multi;
    int step_dir;                   /* the last step's: Step repeats it */
    uint64_t spins_until;           /* the motor runs on after it's switched off */
} fdc;

static floppy_t *drive(void)
{
    int d = fdc.drive_reg & 3;
    return d == 0 || d == 2 ? &s_fd[0] : d == 1 ? &s_fd[1] : NULL;
}

static bool ready(void)
{
    floppy_t *f = drive();
    return f && f->data;
}

static uint32_t offset(floppy_t *f, int track, int side, int sector)
{
    return ((uint32_t)(track * f->sides + side) * f->spt + (sector - 1)) * 512;
}

static bool sector_exists(floppy_t *f, int side, int sector)
{
    return f && f->data && side < f->sides && sector >= 1 && sector <= f->spt && f->head < f->tracks &&
           fdc.track == f->head;
}

static void finish(uint8_t status)
{
    fdc.status = status & ~ST_BUSY;
    fdc.drq = false;
    fdc.intrq = true;
    fdc.xfer = XFER_NONE;
    fdc.done_at = 0;
}

/* Type I status bits as the drive shows them now */
static uint8_t type1_bits(void)
{
    floppy_t *f = drive();
    uint8_t s = 0;
    if (!ready()) {
        s |= ST_NOT_READY;
    } else {
        if (f->read_only) {
            s |= ST_PROTECT;
        }
        if ((fdc.drive_reg & 0x80) && (msx.clock % REVOLUTION) < INDEX_PULSE) {
            s |= ST_INDEX;
        }
    }
    if (f && f->head == 0) {
        s |= ST_TRACK0;
    }
    return s;
}

/* A command's time is up */
static void complete(void)
{
    floppy_t *f = drive();
    if (fdc.type1) {
        uint8_t s = (fdc.status & (ST_HEAD | ST_RNF | ST_CRC)) | type1_bits();
        finish(s);
        return;
    }
    switch (fdc.xfer) {
    case XFER_READ:
    case XFER_WRITE:
    case XFER_ADDRESS:
    case XFER_WRITE_TRACK:
    case XFER_READ_TRACK:
        fdc.drq = true;                     /* the data is there now */
        fdc.done_at = 0;
        break;
    default:
        (void)f;
        finish(fdc.status);
        break;
    }
}

static void update(void)
{
    if (fdc.done_at && msx.clock >= fdc.done_at) {
        complete();
    }
}

static void start_read(void)
{
    floppy_t *f = drive();
    int side = fdc.side & 1;
    if (!sector_exists(f, side, fdc.sector)) {
        finish(ST_RNF);
        return;
    }
    memcpy(fdc.buf, f->data + offset(f, f->head, side, fdc.sector), 512);
    fdc.pos = 0;
    fdc.len = 512;
    fdc.xfer = XFER_READ;
    f->reads++;
}

static void start_write(void)
{
    floppy_t *f = drive();
    int side = fdc.side & 1;
    if (f && f->data && f->read_only) {
        finish(ST_PROTECT);
        return;
    }
    if (!sector_exists(f, side, fdc.sector)) {
        finish(ST_RNF);
        return;
    }
    fdc.pos = 0;
    fdc.len = 512;
    fdc.xfer = XFER_WRITE;
}

/* The CRC-CCITT of the ID field (A1 A1 A1 FE t s r n) */
static uint16_t crc16(uint16_t crc, const uint8_t *p, int len)
{
    while (len--) {
        crc ^= *p++ << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static void build_track(floppy_t *f)
{
    /* Gap, then per sector: sync, ID, gap, sync, data; as a format leaves it */
    int n = 0, side = fdc.side & 1;
    memset(fdc.buf, 0x4E, TRACK_BYTES);
    n = 80;
    for (int s = 1; s <= f->spt && n + 700 < TRACK_BYTES; s++) {
        memset(fdc.buf + n, 0, 12);
        n += 12;
        uint8_t id[8] = { 0xA1, 0xA1, 0xA1, 0xFE, (uint8_t)f->head, (uint8_t)side, (uint8_t)s, 2 };
        memcpy(fdc.buf + n, id, 8);
        uint16_t crc = crc16(0xFFFF, id, 8);
        n += 8;
        fdc.buf[n++] = crc >> 8;
        fdc.buf[n++] = crc;
        n += 22;
        memset(fdc.buf + n, 0, 12);
        n += 12;
        memset(fdc.buf + n, 0xA1, 3);
        fdc.buf[n + 3] = 0xFB;
        n += 4;
        memcpy(fdc.buf + n, f->data + offset(f, f->head, side, s), 512);
        n += 512;
        fdc.buf[n++] = 0;
        fdc.buf[n++] = 0;
        n += 54;
    }
}

/* What Write Track left: its sectors into the image */
static void parse_track(floppy_t *f)
{
    int side = fdc.side & 1;
    int i = 0;
    while (i < fdc.len) {
        if (fdc.buf[i] != 0xFE || i + 4 >= fdc.len) {
            i++;
            continue;
        }
        int sector = fdc.buf[i + 3], size = 128 << (fdc.buf[i + 4] & 3);
        i += 5;
        while (i < fdc.len && fdc.buf[i] != 0xFB && fdc.buf[i] != 0xFE) {
            i++;
        }
        if (i >= fdc.len || fdc.buf[i] != 0xFB) {
            continue;
        }
        i++;
        if (size == 512 && i + 512 <= fdc.len && side < f->sides && sector >= 1 && sector <= f->spt &&
            f->head < f->tracks) {
            memcpy(f->data + offset(f, f->head, side, sector), fdc.buf + i, 512);
            f->dirty = true;
            f->writes++;
        }
        i += size;
    }
}

static void command(uint8_t cmd)
{
    floppy_t *f = drive();
    fdc.command = cmd;
    fdc.intrq = false;
    if ((cmd & 0xF0) == 0xD0) {             /* Force Interrupt */
        fdc.type1 = true;
        fdc.xfer = XFER_NONE;
        fdc.drq = false;
        fdc.done_at = 0;
        fdc.status = type1_bits();
        fdc.intrq = (cmd & 0x08) != 0;
        return;
    }
    if (fdc.status & ST_BUSY) {
        return;
    }
    fdc.status = ST_BUSY;
    fdc.drq = false;
    fdc.xfer = XFER_NONE;

    if (!(cmd & 0x80)) {                    /* type I: restore, seek, step */
        fdc.type1 = true;
        int steps = 0, target;
        if (cmd & 0x08) {
            fdc.status |= ST_HEAD;
        }
        switch (cmd >> 5) {
        case 0:
            if (cmd & 0x10) {               /* seek: from the track register to the data register */
                target = fdc.data;
                steps = abs(target - fdc.track);
                if (f) {
                    int head = f->head + (target - fdc.track);
                    f->head = head < 0 ? 0 : head > 83 ? 83 : head;
                }
                fdc.step_dir = target < fdc.track ? -1 : 1;
            } else {                        /* restore */
                target = 0;
                steps = f ? f->head : 0;
                if (f) {
                    f->head = 0;
                }
                fdc.step_dir = -1;
            }
            fdc.track = target;
            break;
        default: {                          /* step (1), step in (2), step out (3) */
            int dir = (cmd >> 5) == 2 ? 1 : (cmd >> 5) == 3 ? -1 : fdc.step_dir;
            fdc.step_dir = dir;
            if (f) {
                f->head = f->head + dir < 0 ? 0 : f->head + dir > 83 ? 83 : f->head + dir;
            }
            if (cmd & 0x10) {
                fdc.track += dir;
            }
            steps = 1;
            break;
        }
        }
        /* Verify: the head must be on the track the register says */
        if ((cmd & 0x04) && f && f->data && f->head != fdc.track) {
            fdc.status |= ST_RNF;
        }
        fdc.done_at = msx.clock + MS / 10 + (uint64_t)steps * MS;
        return;
    }

    fdc.type1 = false;
    if (!ready()) {
        finish(ST_NOT_READY);
        return;
    }
    switch (cmd >> 4) {
    case 0x8: case 0x9:                     /* read sector */
        fdc.multi = cmd & 0x10;
        start_read();
        break;
    case 0xA: case 0xB:                     /* write sector */
        fdc.multi = cmd & 0x10;
        start_write();
        break;
    case 0xC: {                             /* read address */
        int s = (int)((msx.clock / (REVOLUTION / 9)) % f->spt) + 1;
        uint8_t id[8] = { 0xA1, 0xA1, 0xA1, 0xFE, (uint8_t)f->head, fdc.side & 1, (uint8_t)s, 2 };
        uint16_t crc = crc16(0xFFFF, id, 8);
        memcpy(fdc.buf, id + 4, 4);
        fdc.buf[4] = crc >> 8;
        fdc.buf[5] = crc;
        fdc.sector = f->head;               /* as the WD does */
        fdc.pos = 0;
        fdc.len = 6;
        fdc.xfer = XFER_ADDRESS;
        break;
    }
    case 0xE:                               /* read track */
        build_track(f);
        fdc.pos = 0;
        fdc.len = TRACK_BYTES;
        fdc.xfer = XFER_READ_TRACK;
        break;
    case 0xF:                               /* write track */
        if (f->read_only) {
            finish(ST_PROTECT);
            return;
        }
        fdc.pos = 0;
        fdc.len = TRACK_BYTES;
        fdc.xfer = XFER_WRITE_TRACK;
        break;
    default:
        finish(0);
        return;
    }
    if (fdc.xfer != XFER_NONE) {
        fdc.done_at = msx.clock + MS / 10;
    }
}

/* A sector (or track) moved through the data register */
static void transfer_done(void)
{
    floppy_t *f = drive();
    switch (fdc.xfer) {
    case XFER_READ:
        if (fdc.multi && sector_exists(f, fdc.side & 1, fdc.sector + 1)) {
            fdc.sector++;
            start_read();
            return;
        }
        finish(0);
        break;
    case XFER_WRITE:
        if (f && f->data) {
            memcpy(f->data + offset(f, f->head, fdc.side & 1, fdc.sector), fdc.buf, 512);
            f->dirty = true;
            f->writes++;
        }
        if (fdc.multi && sector_exists(f, fdc.side & 1, fdc.sector + 1)) {
            fdc.sector++;
            start_write();
            fdc.drq = true;
            return;
        }
        finish(0);
        break;
    case XFER_WRITE_TRACK:
        if (f && f->data) {
            parse_track(f);
        }
        finish(0);
        break;
    default:
        finish(0);
        break;
    }
}

uint8_t fdc_read(uint16_t addr)
{
    update();
    switch (addr) {
    case 0x7FF8:
        fdc.intrq = false;
        if (fdc.type1) {
            return (fdc.status & (ST_BUSY | ST_HEAD | ST_RNF | ST_CRC)) | type1_bits();
        }
        return (fdc.status & ~(ST_DRQ | ST_NOT_READY)) | (fdc.drq ? ST_DRQ : 0) |
               (ready() ? 0 : ST_NOT_READY);
    case 0x7FF9:
        return fdc.track;
    case 0x7FFA:
        return fdc.sector;
    case 0x7FFB:
        if (fdc.drq && (fdc.xfer == XFER_READ || fdc.xfer == XFER_ADDRESS || fdc.xfer == XFER_READ_TRACK)) {
            fdc.data = fdc.buf[fdc.pos++];
            if (fdc.pos >= fdc.len) {
                transfer_done();
            }
        }
        return fdc.data;
    case 0x7FFC:
        return fdc.side | 0xFE;
    case 0x7FFD:
        return (fdc.drive_reg & 0x83) | 0x7C;
    case 0x7FFF: {
        floppy_t *f = drive();
        bool spinning = (fdc.drive_reg & 0x80) || msx.clock < fdc.spins_until;
        uint8_t v = 0x0F | (spinning ? 0 : 0x10) | (fdc.intrq ? 0 : 0x40) | (fdc.drq ? 0 : 0x80);
        if (f && f->changed) {
            f->changed = false;
        } else {
            v |= 0x20;
        }
        return v;
    }
    default:
        return 0xFF;
    }
}

void fdc_write(uint16_t addr, uint8_t v)
{
    update();
    switch (addr) {
    case 0x7FF8:
        command(v);
        break;
    case 0x7FF9:
        fdc.track = v;
        break;
    case 0x7FFA:
        fdc.sector = v;
        break;
    case 0x7FFB:
        fdc.data = v;
        if (fdc.drq && (fdc.xfer == XFER_WRITE || fdc.xfer == XFER_WRITE_TRACK)) {
            fdc.buf[fdc.pos++] = v;
            if (fdc.pos >= fdc.len) {
                transfer_done();
            }
        }
        break;
    case 0x7FFC:
        fdc.side = v & 1;
        break;
    case 0x7FFD:
        if ((fdc.drive_reg & 0x80) && !(v & 0x80)) {
            fdc.spins_until = msx.clock + 3000ull * MS;
        }
        fdc.drive_reg = v;
        break;
    default:
        break;
    }
}

void fdc_reset(void)
{
    memset(&fdc, 0, sizeof(fdc));
    fdc.type1 = true;
    for (int d = 0; d < MSX_FLOPPIES; d++) {
        s_fd[d].head = 0;
    }
}

void fdc_status(msx_status_t *s)
{
    floppy_t *f = drive();
    for (int d = 0; d < MSX_FLOPPIES; d++) {
        s->floppy_track[d] = s_fd[d].head;
        s->floppy_reads[d] = s_fd[d].reads;
        s->floppy_writes[d] = s_fd[d].writes;
        s->floppy_busy[d] = f == &s_fd[d] && (fdc.status & ST_BUSY);
    }
}

/* ---- Images ---- */

static void geometry(floppy_t *f)
{
    const uint8_t *b = f->data;
    int spt = b[0x18] | (b[0x19] << 8), heads = b[0x1A] | (b[0x1B] << 8);
    uint32_t total = b[0x13] | (b[0x14] << 8);
    if ((spt == 8 || spt == 9) && (heads == 1 || heads == 2) && total * 512 == f->size) {
        f->spt = spt;
        f->sides = heads;
    } else {
        switch (f->size) {
        case 368640: f->spt = 9; f->sides = 1; break;
        case 327680: f->spt = 8; f->sides = 1; break;
        case 655360: f->spt = 8; f->sides = 2; break;
        default: f->spt = 9; f->sides = 2; break;
        }
    }
    f->tracks = f->size / (512u * f->spt * f->sides);
}

bool msx_floppy_insert(int d, const char *path)
{
    if (d < 0 || d >= MSX_FLOPPIES) {
        return false;
    }
    msx_floppy_eject(d);
    floppy_t *f = &s_fd[d];
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
    fseek(fp, 0, SEEK_SET);
    if (size < 163840 || size > 1474560 || size % 512) {
        fclose(fp);
        return false;
    }
    f->data = msx_alloc(size);
    if (f->data == NULL) {
        fclose(fp);
        return false;
    }
    bool ok = fread(f->data, 1, size, fp) == (size_t)size;
    fclose(fp);
    if (!ok) {
        msx_release(f->data);
        f->data = NULL;
        return false;
    }
    f->size = size;
    f->read_only = read_only;
    f->dirty = false;
    f->changed = true;
    snprintf(f->path, sizeof(f->path), "%s", path);
    geometry(f);
    return true;
}

static bool flush_one(floppy_t *f)
{
    if (!f->data || !f->dirty || f->read_only) {
        return true;
    }
    FILE *fp = fopen(f->path, "r+b");
    if (fp == NULL) {
        return false;
    }
    bool ok = fwrite(f->data, 1, f->size, fp) == f->size;
    ok = fclose(fp) == 0 && ok;
    if (ok) {
        f->dirty = false;
    }
    return ok;
}

bool msx_floppy_flush(void)
{
    bool ok = true;
    for (int d = 0; d < MSX_FLOPPIES; d++) {
        ok = flush_one(&s_fd[d]) && ok;
    }
    return ok;
}

void msx_floppy_eject(int d)
{
    if (d < 0 || d >= MSX_FLOPPIES || !s_fd[d].data) {
        return;
    }
    flush_one(&s_fd[d]);
    msx_release(s_fd[d].data);
    s_fd[d].data = NULL;
    s_fd[d].path[0] = 0;
    s_fd[d].changed = true;
}

const char *msx_floppy_path(int d)
{
    return d >= 0 && d < MSX_FLOPPIES && s_fd[d].data ? s_fd[d].path : NULL;
}

bool msx_floppy_write_protected(int d)
{
    return d >= 0 && d < MSX_FLOPPIES && s_fd[d].data && s_fd[d].read_only;
}

/* A blank disk formatted as MSX-DOS formats it (the boot sector without
 * its code: a data disk)
 */
bool msx_floppy_create(const char *path, uint32_t bytes)
{
    bool ds = bytes == 737280;
    if (!ds && bytes != 368640) {
        return false;
    }
    uint8_t *img = calloc(1, bytes);
    if (img == NULL) {
        return false;
    }
    uint32_t sectors = bytes / 512;
    uint8_t media = ds ? 0xF9 : 0xF8;
    int fat = ds ? 3 : 2;
    uint8_t *b = img;
    b[0] = 0xEB; b[1] = 0xFE; b[2] = 0x90;
    memcpy(b + 3, "ESP32MSX", 8);
    b[0x0B] = 0x00; b[0x0C] = 0x02;         /* 512 bytes a sector */
    b[0x0D] = 2;                            /* sectors a cluster */
    b[0x0E] = 1;                            /* reserved */
    b[0x10] = 2;                            /* FATs */
    b[0x11] = 112;                          /* root entries */
    b[0x13] = sectors & 0xFF; b[0x14] = sectors >> 8;
    b[0x15] = media;
    b[0x16] = fat;
    b[0x18] = 9;
    b[0x1A] = ds ? 2 : 1;
    b[0x1E] = 0xC9;                         /* boot code: RET */
    for (int i = 0; i < 2; i++) {
        uint8_t *f = img + (1 + i * fat) * 512;
        f[0] = media;
        f[1] = 0xFF;
        f[2] = 0xFF;
    }
    FILE *fp = fopen(path, "wb");
    bool ok = fp && fwrite(img, 1, bytes, fp) == bytes;
    ok = fp && fclose(fp) == 0 && ok;
    free(img);
    return ok;
}
