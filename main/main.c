/*
 * MSX1 and MSX2 emulator on the Makerfabs MaTouch ESP32-S3 7" or the
 * M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * MSX into it: memory, ROMs, settings, keyboard map, joystick, the drives
 * (two floppy drives, the hard disk, the cartridge), status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/emu.h"
#include "msx.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

#define MSX2_RAM_KB     512        /* the memory mapper */

static const char *const model_names[] = { "MSX1", "MSX2", "" };
static const char *const video_names[] = { "50 Hz", "60 Hz", "" };
static const char *const floppy_names[] = { "Off", "On", "" };

static msx_config_t s_cfg;
static uint8_t *s_bios[2], *s_sub, *s_disk, *s_nextor;
static bool s_started;
static char s_rom_text[128];

static void *psram_alloc(uint32_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

/* ---- Memory ----
 *
 * At power-on, before the screen, WiFi and Bluetooth fragment PSRAM: the
 * MSX2's 512 KB of RAM (an MSX1 uses 64 KB of it), 128 KB of VRAM, the
 * ROMs, the picture. Disk images and cartridges come later.
 */
static void reserve_memory(void)
{
    settings_load();
    s_cfg.ram = heap_caps_malloc(MSX2_RAM_KB * 1024, MALLOC_CAP_SPIRAM);
    s_cfg.vram = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    s_cfg.frame = heap_caps_calloc(MSX_SCREEN_WIDTH * MSX_SCREEN_HEIGHT, 2, MALLOC_CAP_SPIRAM);
    s_bios[0] = psram_alloc(rom_store_size(ROM_MSX1));
    s_bios[1] = psram_alloc(rom_store_size(ROM_MSX2));
    s_sub = psram_alloc(rom_store_size(ROM_MSX2_SUB));
    s_disk = psram_alloc(rom_store_size(ROM_DISK));
    s_nextor = psram_alloc(rom_store_size(ROM_NEXTOR));
    s_cfg.alloc = psram_alloc;
    s_cfg.release = heap_caps_free;
}

static void storage(bool sd_card)
{
    emu_set_drive_info(sd_card ? "Disk images, hard disks and cartridges on the SD card"
                               : "No SD card: no disks or cartridges");
}

/* ---- Starting ---- */

static bool start(void)
{
    if (s_cfg.ram == NULL || s_cfg.vram == NULL || s_cfg.frame == NULL || !s_bios[0] || !s_bios[1]) {
        emu_set_message("Not enough memory: see the log.");
        return false;
    }
    bool msx2 = settings.model == MSX_MODEL_MSX2;
    if (msx2) {
        if (!rom_store_load(ROM_MSX2, s_bios[1]) || !rom_store_load(ROM_MSX2_SUB, s_sub)) {
            emu_set_message("No MSX2 ROMs: put msx2.rom and msx2ext.rom into roms/ and flash with idf.py "
                            "flash, or copy them into the \"roms\" folder on the SD card.");
            return false;
        }
    } else if (!rom_store_load(ROM_MSX1, s_bios[0])) {
        emu_set_message("No MSX1 ROM: put msx.rom into roms/ and flash with idf.py flash, "
                        "or copy it into the \"roms\" folder on the SD card.");
        return false;
    }
    bool disk = settings.floppy && s_disk && rom_store_load(ROM_DISK, s_disk);
    bool nextor = s_nextor && rom_store_load(ROM_NEXTOR, s_nextor);
    s_cfg.model = settings.model;
    s_cfg.ram_kb = msx2 ? MSX2_RAM_KB : 64;
    s_cfg.hz50 = settings.video == 0;
    s_cfg.bios = msx2 ? s_bios[1] : s_bios[0];
    s_cfg.sub_rom = msx2 ? s_sub : NULL;
    s_cfg.disk_rom = disk ? s_disk : NULL;
    s_cfg.nextor = nextor ? s_nextor : NULL;
    snprintf(s_rom_text, sizeof(s_rom_text), "%s%s%s%s", msx2 ? "msx2.rom, msx2ext.rom" : "msx.rom",
             disk ? ", disk.rom" : settings.floppy ? " (no disk.rom: no floppies)" : "",
             nextor ? ", nextor.rom" : msx_hd_path() ? " (no nextor.rom: no hard disk)" : "", "");
    if (!msx_init(&s_cfg)) {
        emu_set_message("The MSX could not be set up: see the log.");
        return false;
    }
    s_started = true;
    ESP_LOGI(TAG, "%s, %u KB, %s, %s", model_names[settings.model], (unsigned)s_cfg.ram_kb,
             video_names[settings.video], s_rom_text);
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    mode->width = MSX_SCREEN_WIDTH;
    mode->height = MSX_SCREEN_HEIGHT;
    mode->hz = msx_screen_hz();
}

static void render_line(int y, uint16_t *dst)
{
    memcpy(dst, msx_line(y), MSX_SCREEN_WIDTH * sizeof(uint16_t));
}

/* ---- Keyboard and joystick ----
 *
 * By position: the international MSX keyboard has the US layout's
 * characters where a PC has them. Ctrl, Shift as themselves, left Alt
 * GRAPH, right Alt CODE, Caps Lock CAPS, the ISO key (or Menu) the dead
 * key; F1-F5 as themselves and F6-F10 as SHIFT with them (as on the MSX),
 * Home CLS/HOME, Insert INS, Delete DEL, End or Pause STOP, Page Up
 * SELECT. The numeric keypad is the joystick (8 4 6 2 and the diagonals,
 * 0 or 5 trigger A, . trigger B) in port 1 (F11 switches to 2); F12 makes
 * the cursor keys the joystick too (Alt: A, AltGr: B).
 */
enum { J_UP = 1, J_DOWN = 2, J_LEFT = 4, J_RIGHT = 8, J_A = 16, J_B = 32 };

static uint8_t s_joy_pad, s_joy_arrows;
static bool s_arrows_joystick;
static int8_t s_fkey_shift[256];    /* F6-F10 held: SHIFT was added */

static void joystick_update(void)
{
    msx_joystick(settings.joystick, s_joy_pad | s_joy_arrows);
    msx_joystick(3 - settings.joystick, 0);
}

static bool keypad_joystick(uint8_t usage, bool pressed)
{
    static const uint8_t bits[] = {
        J_DOWN | J_LEFT, J_DOWN, J_DOWN | J_RIGHT, J_LEFT, J_A, J_RIGHT,
        J_UP | J_LEFT, J_UP, J_UP | J_RIGHT, J_A, J_B,          /* 1-9, 0, . */
    };
    if (usage < 0x59 || usage > 0x63) {
        return false;
    }
    uint8_t b = bits[usage - 0x59];
    s_joy_pad = pressed ? s_joy_pad | b : s_joy_pad & ~b;
    joystick_update();
    return true;
}

/* The MSX key at a PC key's place, -1 if none */
static int key_for(uint8_t usage)
{
    static const char us[0x39] = {
        [0x04] = 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
        's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
        [0x2D] = '-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/',
    };
    switch (usage) {
    case 0x28: case 0x58: return MSX_KEY_RETURN;
    case 0x29: return MSX_KEY_ESC;
    case 0x2A: return MSX_KEY_BS;
    case 0x2B: return MSX_KEY_TAB;
    case 0x2C: return MSX_KEY_SPACE;
    case 0x39: return MSX_KEY_CAPS;
    case 0x3A: case 0x3F: return MSX_KEY_F1;
    case 0x3B: case 0x40: return MSX_KEY_F2;
    case 0x3C: case 0x41: return MSX_KEY_F3;
    case 0x3D: case 0x42: return MSX_KEY_F4;
    case 0x3E: case 0x43: return MSX_KEY_F5;
    case 0x4A: return MSX_KEY_HOME;
    case 0x49: return MSX_KEY_INS;
    case 0x4C: return MSX_KEY_DEL;
    case 0x4D: case 0x48: return MSX_KEY_STOP;
    case 0x4B: return MSX_KEY_SELECT;
    case 0x4F: return MSX_KEY_RIGHT;
    case 0x50: return MSX_KEY_LEFT;
    case 0x51: return MSX_KEY_DOWN;
    case 0x52: return MSX_KEY_UP;
    case 0x64: case 0x65: return 2 * 8 + 5;         /* the dead key */
    case 0xE0: case 0xE4: return MSX_KEY_CTRL;
    case 0xE1: case 0xE5: return MSX_KEY_SHIFT;
    case 0xE2: return MSX_KEY_GRAPH;
    case 0xE6: return MSX_KEY_CODE;
    default: break;
    }
    if (usage < sizeof(us) && us[usage]) {
        bool shift;
        return msx_ascii_key(us[usage], &shift);
    }
    return -1;
}

static void hid_key(uint8_t usage, bool pressed)
{
    if (keypad_joystick(usage, pressed)) {
        return;
    }
    if (s_arrows_joystick) {
        static const uint8_t dir[4] = { J_RIGHT, J_LEFT, J_DOWN, J_UP };
        uint8_t b = usage >= 0x4F && usage <= 0x52 ? dir[usage - 0x4F] : usage == 0xE2 ? J_A : usage == 0xE6 ? J_B : 0;
        if (b) {
            s_joy_arrows = pressed ? s_joy_arrows | b : s_joy_arrows & ~b;
            joystick_update();
            return;
        }
    }
    if (pressed && usage == 0x44) {             /* F11: joystick port */
        msx_joystick(settings.joystick, 0);
        settings.joystick = 3 - settings.joystick;
        settings_save();
        joystick_update();
        ESP_LOGI(TAG, "joystick in port %d", settings.joystick);
        return;
    }
    if (pressed && usage == 0x45) {             /* F12: cursor keys <-> joystick */
        s_arrows_joystick = !s_arrows_joystick;
        s_joy_arrows = 0;
        joystick_update();
        return;
    }
    int key = key_for(usage);
    if (key < 0) {
        return;
    }
    if (usage >= 0x3F && usage <= 0x43) {       /* F6-F10: SHIFT F1-F5 */
        if (pressed) {
            msx_key(MSX_KEY_SHIFT, true);
            s_fkey_shift[usage] = 1;
        } else if (s_fkey_shift[usage]) {
            msx_key(MSX_KEY_SHIFT, false);
            s_fkey_shift[usage] = 0;
        }
    }
    msx_key(key, pressed);
}

/* ---- Setup screen ---- */

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= 0 && i < MSX_MODEL_COUNT) {
        settings.model = i;
        settings_save();
    }
}

static int video_get(void)
{
    return settings.video;
}

static void video_set(int i)
{
    if (i == 0 || i == 1) {
        settings.video = i;
        settings_save();
    }
}

static int floppy_get(void)
{
    return settings.floppy;
}

static void floppy_set(int i)
{
    if (i == 0 || i == 1) {
        settings.floppy = i;
        settings_save();
    }
}

static const emu_choice_t choices[] = {
    { "Model", model_names, model_get, model_set, false },
    { "Video", video_names, video_get, video_set, false },
    { "Floppy", floppy_names, floppy_get, floppy_set, false },
};

/* ---- Drives: two floppies, the hard disk, the cartridge ----
 *
 * The floppies are A: and B: in BASIC and DOS; with a hard disk Nextor runs
 * first and its disk is A:, the floppies B: and C:. Hence "Floppy 1" and
 * "Floppy 2" here.
 */

enum { DRIVE_A, DRIVE_B, DRIVE_HD, DRIVE_CART, DRIVES };

#define FLOPPY_720K     737280
#define FLOPPY_360K     368640
#define HD_32M          (32u * 1024 * 1024)
#define HD_128M         (128u * 1024 * 1024)

static const char *const extensions[] = { ".dsk", ".hdd", ".img", ".rom", ".mx1", ".mx2", NULL };
static const char *const drive_names[] = { "Floppy 1", "Floppy 2", "Hard disk", "Cartridge" };
static const disks_size_t sizes[] = {
    { FLOPPY_720K, "720 KB double-sided disk" },
    { FLOPPY_360K, "360 KB single-sided disk" },
    { HD_32M, "32 MB hard disk" },
    { HD_128M, "128 MB hard disk" },
};

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool accepts(int drive, const char *name)
{
    switch (drive) {
    case DRIVE_HD: return has_ext(name, ".hdd") || has_ext(name, ".img");
    case DRIVE_CART: return has_ext(name, ".rom") || has_ext(name, ".mx1") || has_ext(name, ".mx2");
    default: return has_ext(name, ".dsk");
    }
}

static bool takes_blank(int drive)
{
    return drive != DRIVE_CART;
}

static uint32_t default_size(int drive)
{
    return drive == DRIVE_HD ? HD_32M : FLOPPY_720K;
}

static const char *extension_for(uint32_t bytes)
{
    return bytes >= HD_32M ? ".hdd" : ".dsk";
}

static bool create(const char *path, uint32_t bytes)
{
    return bytes >= HD_32M ? msx_hd_create(path, bytes) : msx_floppy_create(path, bytes);
}

/* The hard disk and the cartridge are part of the machine: it starts again */
static void restart_if_started(void)
{
    if (s_started) {
        msx_reset();
    }
}

static bool insert(int drive, const char *path)
{
    if (!accepts(drive, path)) {
        return false;
    }
    switch (drive) {
    case DRIVE_HD:
        if (!msx_hd_insert(path)) {
            return false;
        }
        restart_if_started();
        return true;
    case DRIVE_CART:
        if (!msx_cart_insert(path)) {
            return false;
        }
        restart_if_started();
        return true;
    default:
        return msx_floppy_insert(drive, path);
    }
}

static void eject(int drive)
{
    switch (drive) {
    case DRIVE_HD:
        msx_hd_eject();
        restart_if_started();
        break;
    case DRIVE_CART:
        msx_cart_eject();
        restart_if_started();
        break;
    default:
        msx_floppy_eject(drive);
        break;
    }
}

static const char *path(int drive)
{
    switch (drive) {
    case DRIVE_HD: return msx_hd_path();
    case DRIVE_CART: return msx_cart_path();
    default: return msx_floppy_path(drive);
    }
}

static bool write_protected(int drive)
{
    return drive == DRIVE_CART || (drive < DRIVE_HD && msx_floppy_write_protected(drive));
}

static const disks_config_t disks = {
    .folder = "msx",
    .extensions = extensions,
    .description = "Images (.DSK .HDD .ROM)",
    .title = "Drives and cartridge",
    .button = "Disks",
    .nvs_namespace = "msxdisks",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".dsk",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 1024ull * 1024 * 1024,
    .insert = insert,
    .eject = eject,
    .path = path,
    .write_protected = write_protected,
    .create = create,
    .accepts = accepts,
    .takes_blank = takes_blank,
    .default_size = default_size,
    .extension_for = extension_for,
};

static void flush(void *arg)
{
    msx_floppy_flush();
    msx_hd_flush();
}

static void every_second(void)
{
    static int count;
    msx_status_t s;
    msx_get_status(&s);
    static uint32_t writes;
    uint32_t w = s.floppy_writes[0] + s.floppy_writes[1] + s.hd_writes;
    /* Written to two seconds ago and not since: into the files */
    if (w != writes) {
        writes = w;
        count = 2;
    } else if (count > 0 && --count == 0) {
        emu_paused(flush, NULL);
    }
}

/* ---- Status ---- */

static void status(emu_json_t *j, int section)
{
    char text[160];
    msx_status_t s;
    msx_get_status(&s);
    if (section == EMU_STATUS_ROWS) {
        snprintf(text, sizeof(text), "%s, %u KB, %s, floppy drives %s", model_names[settings.model],
                 settings.model == MSX_MODEL_MSX2 ? MSX2_RAM_KB : 64, video_names[settings.video],
                 settings.floppy ? "on" : "off");
        emu_json_row(j, "System", text);
        emu_json_row(j, "ROMs", s_rom_text);
        if (msx_cart_type()) {
            emu_json_row(j, "Cartridge", msx_cart_type());
        }
        snprintf(text, sizeof(text), "port %d on the numeric keypad%s (F11 switches)", settings.joystick,
                 s_arrows_joystick ? " and cursor keys" : "");
        emu_json_row(j, "Joystick", text);
        return;
    }
    for (int d = 0; d < DRIVES; d++) {
        const char *name = disks_name(d);
        uint32_t reads = 0, writes = 0;
        bool light = false;
        const char *detail = NULL;
        if (d < DRIVE_HD) {
            reads = s.floppy_reads[d];
            writes = s.floppy_writes[d];
            light = s.floppy_busy[d];
            snprintf(text, sizeof(text), "track %d", s.floppy_track[d]);
            detail = name ? text : NULL;
        } else if (d == DRIVE_HD) {
            reads = s.hd_reads;
            writes = s.hd_writes;
            light = s.hd_busy;
        } else {
            detail = msx_cart_type();
        }
        emu_json_drive(j, drive_names[d], name, write_protected(d), light, reads, writes, detail);
    }
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    msx_status_t s;
    msx_get_status(&s);
    snprintf(buf, len, "frames %lu PC $%04X slots $%02X VDP mode $%02X", (unsigned long)s.frames, s.pc, s.slots,
             s.vdp_mode);
}

/*   k <n>              tap a key (row * 8 + column: 63 RETURN, 64 SPACE)
 *   K <n> <0|1>        key down (1) or up (0); 48 SHIFT, 49 CTRL, 60 STOP
 *   T <text>           type text (\n is RETURN)
 *   j <port> <bits>    joystick (1 up 2 down 4 left 8 right 16 A 32 B)
 *   R <model> [hz]     restart as 0 MSX1, 1 MSX2; 50 or 60 Hz
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        int key = atoi(line + 1);
        msx_key(key, true);
        vTaskDelay(pdMS_TO_TICKS(100));
        msx_key(key, false);
        return true;
    }
    case 'K': {
        int key = 0, down = 0;
        if (sscanf(line + 1, "%d %d", &key, &down) == 2) {
            msx_key(key, down);
        }
        return true;
    }
    case 'T': {
        char text[256];
        const char *s = line + 1;
        while (*s == ' ') {
            s++;
        }
        size_t n = 0;
        for (; *s && n < sizeof(text) - 1; s++) {
            if (s[0] == '\\' && s[1] == 'n') {
                text[n++] = '\n';
                s++;
            } else {
                text[n++] = *s;
            }
        }
        text[n] = 0;
        msx_type(text);
        return true;
    }
    case 'j': {
        int port = 1;
        unsigned bits = 0;
        sscanf(line + 1, "%d %u", &port, &bits);
        msx_joystick(port, bits);
        return true;
    }
    case 'R': {
        int model = 0, hz = settings.video ? 60 : 50;
        int n = sscanf(line + 1, "%d %d", &model, &hz);
        if (n >= 1 && model >= 0 && model < MSX_MODEL_COUNT && (hz == 50 || hz == 60)) {
            settings.model = model;
            settings.video = hz == 60;
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "MSX",
    .short_name = "MSX",
    .view_width = 800,          /* 576 x 240: 800 x 600, a 4:3 picture; 960 x 720 on the Tab5 */
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = msx_reset,
    .run_frame = msx_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = msx_screen_signature,
    .render_line = render_line,
    .audio_hz = MSX_AUDIO_HZ,
    .audio_enable = msx_audio_enable,
    .audio_render = msx_audio_render,
    .audio_pending = msx_audio_pending,
    .hid_key = hid_key,
    .choices = choices,
    .choice_count = sizeof(choices) / sizeof(choices[0]),
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
