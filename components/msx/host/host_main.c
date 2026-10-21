/*
 * Host build of the MSX core, for testing on a desktop:
 *
 *   build/msx_host [frames] [out.ppm]
 *
 * Environment:
 *   MSX_ROMS=<folder>        msx.rom, msx2.rom, msx2ext.rom, disk.rom, nextor.rom
 *   MSX_MODEL=2              MSX2 (else MSX1)
 *   MSX_RAM=<KB>             MSX2 mapper (default 128)
 *   MSX_HZ=50|60             video (default: the BIOS's, $2B bit 7)
 *   MSX_NODISK=1             no disk ROM (no floppy drives)
 *   MSX_DISKA=, MSX_DISKB=   floppy images
 *   MSX_NEWDISK=<path>       make a blank 720 KB disk first; MSX_NEWHD=<path> a 32 MB hard disk
 *   MSX_HD=<image>           hard disk (with nextor.rom)
 *   MSX_CART=<rom>           cartridge
 *   MSX_SCRIPT=<file>        lines "<frame> <command> ...":
 *                              type <text> (\n is RETURN), key <row*8+col> 0|1,
 *                              joy <port> <bits>, shot <file.ppm>, text,
 *                              insert <drive 1-2> <path>, eject <drive>,
 *                              reset, peek <hex>, dump <hex> <n>, vram <hex> <n>,
 *                              vdp, flush
 *   MSX_TEXT=1               print the text screen at the end
 *   MSX_WAV=<file>           write the sound
 *   MSX_TRACE=<hex pc>:<n>   print n instructions from when pc is reached
 *   MSX_STATUS=1             machine state every 50 frames
 *   MSX_DEBUG=<port>         serve the debugger (esp32emu's docs/debugger.md) on
 *                            127.0.0.1:<port> and run in real time until killed
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msx_internal.h"
#include "emudbg.h"

static uint8_t *load_rom(const char *dir, const char *name, size_t want, bool required)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (required) {
            fprintf(stderr, "can't open %s\n", path);
            exit(1);
        }
        return NULL;
    }
    uint8_t *buf = calloc(1, want);
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    if (n != want) {
        fprintf(stderr, "%s: %zu bytes, expected %zu\n", path, n, want);
        exit(1);
    }
    return buf;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", MSX_SCREEN_WIDTH, MSX_SCREEN_HEIGHT);
    for (int y = 0; y < MSX_SCREEN_HEIGHT; y++) {
        const uint16_t *line = msx_line(y);
        for (int x = 0; x < MSX_SCREEN_WIDTH; x++) {
            uint16_t c = line[x];
            uint8_t rgb[3] = {
                (uint8_t)(((c >> 11) & 31) * 255 / 31),
                (uint8_t)(((c >> 5) & 63) * 255 / 63),
                (uint8_t)((c & 31) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* The text of SCREEN 0 (40 or 80 columns) or SCREEN 1 (32) */
static void print_text(uint32_t frame)
{
    int m = vdp_mode();
    int cols = m == 0x01 ? 40 : m == 0x09 ? 80 : 32;
    uint32_t nt = m == 0x09 ? (vdp_reg(2) & 0x7C) << 10 : (vdp_reg(2) & 0x7F) << 10;
    printf("---- frame %u, mode %02X, names at $%05X\n", frame, m, nt);
    for (int row = 0; row < 24; row++) {
        char s[81];
        for (int col = 0; col < cols; col++) {
            uint8_t c = vdp_peek(nt + row * cols + col);
            s[col] = isprint(c) ? c : '.';
        }
        int n = cols;
        s[n] = 0;
        while (n > 0 && s[n - 1] == ' ') {
            s[--n] = 0;
        }
        printf("|%s\n", s);
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[1024];
} line_t;

static line_t *s_script;
static int s_script_n, s_script_pos;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        exit(1);
    }
    char buf[1200];
    s_script = calloc(8192, sizeof(line_t));
    while (fgets(buf, sizeof(buf), f) && s_script_n < 8192) {
        buf[strcspn(buf, "\r\n")] = 0;
        line_t *l = &s_script[s_script_n];
        int used = 0;
        if (sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &used) >= 2) {
            snprintf(l->arg, sizeof(l->arg), "%s", buf + used);
            s_script_n++;
        }
    }
    fclose(f);
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *o++ = '\n';
            s++;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void run_script(uint32_t frame)
{
    while (s_script_pos < s_script_n && s_script[s_script_pos].frame <= frame) {
        line_t *l = &s_script[s_script_pos++];
        if (!strcmp(l->cmd, "type")) {
            unescape(l->arg);
            msx_type(l->arg);
        } else if (!strcmp(l->cmd, "key")) {
            int k = 0, d = 0;
            sscanf(l->arg, "%d %d", &k, &d);
            msx_key(k, d);
        } else if (!strcmp(l->cmd, "joy")) {
            int port = 1;
            unsigned bits = 0;
            sscanf(l->arg, "%d %u", &port, &bits);
            msx_joystick(port, bits);
        } else if (!strcmp(l->cmd, "shot")) {
            write_ppm(l->arg);
        } else if (!strcmp(l->cmd, "text")) {
            print_text(frame);
        } else if (!strcmp(l->cmd, "insert")) {
            int d = 1;
            char path[400];
            if (sscanf(l->arg, "%d %399s", &d, path) == 2 && !msx_floppy_insert(d - 1, path)) {
                printf("host: can't insert %s\n", path);
            }
        } else if (!strcmp(l->cmd, "eject")) {
            msx_floppy_eject(atoi(l->arg) - 1);
        } else if (!strcmp(l->cmd, "reset")) {
            msx_reset();
        } else if (!strcmp(l->cmd, "flush")) {
            msx_floppy_flush();
            msx_hd_flush();
        } else if (!strcmp(l->cmd, "peek")) {
            unsigned a = strtoul(l->arg, NULL, 16);
            printf("host: frame %u $%04X = $%02X\n", frame, a, z80_peek(a));
        } else if (!strcmp(l->cmd, "dump") || !strcmp(l->cmd, "vram")) {
            unsigned a = 0, n = 16;
            sscanf(l->arg, "%x %u", &a, &n);
            printf("host: frame %u %s $%05X:", frame, l->cmd, a);
            for (unsigned i = 0; i < n; i++) {
                printf(" %02X", l->cmd[0] == 'd' ? z80_peek(a + i) : vdp_peek(a + i));
            }
            printf("\n");
        } else if (!strcmp(l->cmd, "vdp")) {
            printf("host: frame %u mode %02X R#", frame, vdp_mode());
            for (int r = 0; r < 24; r++) {
                printf(" %02X", vdp_reg(r));
            }
            printf(" S#0-2 %02X %02X %02X slots %02X sub3 %02X\n", vdp_status_reg(0), vdp_status_reg(1),
                   vdp_status_reg(2), msx.ppi_a, msx.sub[3]);
        } else {
            printf("host: unknown command %s\n", l->cmd);
        }
    }
}

/* ---- Tracing ---- */

static int s_trace_pc = -1, s_trace_n;
static bool s_tracing;

void msx_host_hook(void)
{
    if (s_trace_pc < 0) {
        return;
    }
    if (!s_tracing && z80.pc.w == s_trace_pc) {
        s_tracing = true;
    }
    if (s_tracing && s_trace_n > 0) {
        char dis[32];
        z80_disassemble(z80.pc.w, dis, sizeof(dis));
        printf("%6u %04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X slots=%02X\n", msx.frames, z80.pc.w, dis,
               z80.af.w, z80.bc.w, z80.de.w, z80.hl.w, z80.sp.w, msx.ppi_a);
        s_trace_n--;
    }
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    const char *out = argc > 2 ? argv[2] : NULL;
    const char *dir = getenv("MSX_ROMS") ? getenv("MSX_ROMS") : "roms";

    static msx_config_t cfg;
    cfg.model = getenv("MSX_MODEL") && atoi(getenv("MSX_MODEL")) == 2 ? MSX_MODEL_MSX2 : MSX_MODEL_MSX1;
    cfg.ram_kb = getenv("MSX_RAM") ? atoi(getenv("MSX_RAM")) : 128;
    if (cfg.model == MSX_MODEL_MSX2) {
        cfg.bios = load_rom(dir, "msx2.rom", 32768, true);
        cfg.sub_rom = load_rom(dir, "msx2ext.rom", 16384, true);
    } else {
        cfg.bios = load_rom(dir, "msx.rom", 32768, true);
    }
    cfg.hz50 = getenv("MSX_HZ") ? atoi(getenv("MSX_HZ")) == 50 : (cfg.bios[0x2B] & 0x80) != 0;
    if (!getenv("MSX_NODISK")) {
        cfg.disk_rom = load_rom(dir, "disk.rom", 16384, false);
    }
    cfg.nextor = load_rom(dir, "nextor.rom", 131072, false);
    cfg.ram = calloc(1, (cfg.model == MSX_MODEL_MSX2 ? cfg.ram_kb : 64) * 1024);
    cfg.vram = calloc(1, VRAM_SIZE);
    cfg.frame = calloc(MSX_SCREEN_WIDTH * MSX_SCREEN_HEIGHT, 2);

    FILE *wav = NULL;
    if (getenv("MSX_WAV")) {
        wav = fopen(getenv("MSX_WAV"), "wb");
        if (wav) {
            fseek(wav, 44, SEEK_SET);
            msx_audio_enable(true);
        }
    }
    if (getenv("MSX_TRACE")) {
        unsigned pc = 0;
        int n = 100;
        sscanf(getenv("MSX_TRACE"), "%x:%d", &pc, &n);
        s_trace_pc = pc;
        s_trace_n = n;
    }
    if (getenv("MSX_NEWDISK") && !msx_floppy_create(getenv("MSX_NEWDISK"), 737280)) {
        fprintf(stderr, "can't make %s\n", getenv("MSX_NEWDISK"));
    }
    if (getenv("MSX_NEWHD") && !msx_hd_create(getenv("MSX_NEWHD"), 32 * 1024 * 1024)) {
        fprintf(stderr, "can't make %s\n", getenv("MSX_NEWHD"));
    }
    if (getenv("MSX_HD") && !msx_hd_insert(getenv("MSX_HD"))) {
        fprintf(stderr, "can't open %s\n", getenv("MSX_HD"));
    }
    if (getenv("MSX_CART") && !msx_cart_insert(getenv("MSX_CART"))) {
        fprintf(stderr, "can't load %s\n", getenv("MSX_CART"));
    }
    if (!msx_init(&cfg)) {
        fprintf(stderr, "msx_init failed\n");
        return 1;
    }
    for (int d = 0; d < 2; d++) {
        const char *name = d ? "MSX_DISKB" : "MSX_DISKA";
        if (getenv(name) && !msx_floppy_insert(d, getenv(name))) {
            fprintf(stderr, "can't load %s\n", getenv(name));
        }
    }
    if (msx_cart_type()) {
        printf("host: cartridge %s\n", msx_cart_type());
    }
    if (getenv("MSX_SCRIPT")) {
        load_script(getenv("MSX_SCRIPT"));
    }
    emudbg_host_run("MSX_DEBUG", msx_run_frame, msx_screen_hz());
    uint32_t samples = 0;
    for (int f = 0; f < frames; f++) {
        run_script(f);
        msx_run_frame();
        if (wav) {
            int16_t buf[4096 * 2];
            int n;
            while ((n = msx_audio_render(buf, 4096)) > 0) {
                fwrite(buf, 4, n, wav);
                samples += n;
            }
        }
        if (getenv("MSX_STATUS") && f % 50 == 0) {
            msx_status_t s;
            msx_get_status(&s);
            printf("host: frame %u pc %04X slots %02X mode %02X A: track %d %u/%u HD %u/%u\n", s.frames, s.pc,
                   s.slots, s.vdp_mode, s.floppy_track[0], s.floppy_reads[0], s.floppy_writes[0], s.hd_reads,
                   s.hd_writes);
        }
    }
    msx_floppy_flush();
    msx_hd_flush();
    for (int d = 0; d < 3; d++) {
        uint64_t free_bytes, size;
        if (d < 2 ? msx_floppy_space(d, &free_bytes, &size) : msx_hd_space(&free_bytes, &size)) {
            printf("host: %s: %llu bytes free of %llu\n", d == 0 ? "floppy 1" : d == 1 ? "floppy 2" : "hard disk",
                   (unsigned long long)free_bytes, (unsigned long long)size);
        }
    }
    if (out) {
        write_ppm(out);
    }
    if (getenv("MSX_TEXT")) {
        print_text(frames);
    }
    if (wav) {
        uint32_t rate = MSX_AUDIO_HZ, bytes = samples * 4;
        uint8_t h[45] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0........\x04\0\x10\0data....";
        uint32_t riff = bytes + 36, byte_rate = rate * 4;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 24, &rate, 4);
        memcpy(h + 28, &byte_rate, 4);
        memcpy(h + 40, &bytes, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    return 0;
}
