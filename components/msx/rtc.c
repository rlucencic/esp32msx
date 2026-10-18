/*
 * The MSX2's RP5C01 clock (ports $B4 index, $B5 data, 4 bits): block 0
 * the time (from the board's clock; setting it keeps an offset), block 1
 * alarm, 12/24 hours and the leap year, blocks 2 and 3 battery-backed RAM
 * where the BIOS keeps its settings (screen, colours, beep). Register 13
 * selects the block, 14 is the test register, 15 resets.
 */
#include <string.h>
#include <time.h>
#include "msx_internal.h"

static struct {
    uint8_t index;
    uint8_t mode;               /* register 13 */
    uint8_t block1[13];
    uint8_t ram[2][13];
    int64_t offset;             /* seconds the MSX's time is from the board's */
} rtc;

static void now_tm(struct tm *tm)
{
    time_t t = time(NULL) + rtc.offset;
    localtime_r(&t, tm);
}

static uint8_t time_reg(int r)
{
    struct tm tm;
    now_tm(&tm);
    int year = tm.tm_year + 1900 - 1980;
    switch (r) {
    case 0: return tm.tm_sec % 10;
    case 1: return tm.tm_sec / 10;
    case 2: return tm.tm_min % 10;
    case 3: return tm.tm_min / 10;
    case 4: return tm.tm_hour % 10;
    case 5: return tm.tm_hour / 10;
    case 6: return tm.tm_wday;
    case 7: return tm.tm_mday % 10;
    case 8: return tm.tm_mday / 10;
    case 9: return (tm.tm_mon + 1) % 10;
    case 10: return (tm.tm_mon + 1) / 10;
    case 11: return year % 10;
    case 12: return (year / 10) % 10;
    default: return 0;
    }
}

/* The MSX sets a time field: the offset follows */
static void set_time_reg(int r, uint8_t v)
{
    struct tm tm;
    now_tm(&tm);
    int year = tm.tm_year + 1900 - 1980;
    int mon = tm.tm_mon + 1;
    switch (r) {
    case 0: tm.tm_sec = tm.tm_sec / 10 * 10 + v; break;
    case 1: tm.tm_sec = tm.tm_sec % 10 + v * 10; break;
    case 2: tm.tm_min = tm.tm_min / 10 * 10 + v; break;
    case 3: tm.tm_min = tm.tm_min % 10 + v * 10; break;
    case 4: tm.tm_hour = tm.tm_hour / 10 * 10 + v; break;
    case 5: tm.tm_hour = tm.tm_hour % 10 + (v & 3) * 10; break;
    case 7: tm.tm_mday = tm.tm_mday / 10 * 10 + v; break;
    case 8: tm.tm_mday = tm.tm_mday % 10 + (v & 3) * 10; break;
    case 9: mon = mon / 10 * 10 + v; break;
    case 10: mon = mon % 10 + (v & 1) * 10; break;
    case 11: year = year / 10 * 10 + v; break;
    case 12: year = year % 10 + v * 10; break;
    default: return;
    }
    if (tm.tm_mday < 1 || mon < 1 || mon > 12 || tm.tm_sec > 59 || tm.tm_min > 59 || tm.tm_hour > 23) {
        return;                             /* in the middle of setting digits */
    }
    tm.tm_mon = mon - 1;
    tm.tm_year = year + 1980 - 1900;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t != (time_t)-1) {
        rtc.offset += (int64_t)t - (int64_t)(time(NULL) + rtc.offset);
    }
}

/* Block 2 as a European MSX2 leaves the factory: ID $A, no adjust, SCREEN
 * 0, WIDTH 40, COLOR 15,4,4 (without the ID the BIOS writes its own
 * defaults: SCREEN 1, WIDTH 29, COLOR 15,4,7)
 */
static const uint8_t block2_default[13] = { 0xA, 0, 0, 0, 8, 2, 15, 4, 4, 3, 0, 0, 0 };

void rtc_reset(void)
{
    rtc.index = 0;
    rtc.mode = 0x08;
    memset(rtc.block1, 0, sizeof(rtc.block1));
    rtc.block1[10] = 1;                     /* 24 hours */
    if (rtc.ram[0][0] != 0xA) {
        memcpy(rtc.ram[0], block2_default, sizeof(block2_default));
    }
}

void rtc_index(uint8_t v)
{
    rtc.index = v & 15;
}

uint8_t rtc_read(void)
{
    int r = rtc.index;
    if (r == 13) {
        return rtc.mode | 0xF0;
    }
    if (r >= 14) {
        return 0xFF;
    }
    switch (rtc.mode & 3) {
    case 0: return time_reg(r) | 0xF0;
    case 1: return rtc.block1[r] | 0xF0;
    default: return rtc.ram[(rtc.mode & 3) - 2][r] | 0xF0;
    }
}

void rtc_write(uint8_t v)
{
    int r = rtc.index;
    v &= 15;
    if (r == 13) {
        rtc.mode = v;
        return;
    }
    if (r >= 14) {
        return;
    }
    switch (rtc.mode & 3) {
    case 0: set_time_reg(r, v); break;
    case 1: rtc.block1[r] = v; break;
    default: rtc.ram[(rtc.mode & 3) - 2][r] = v; break;
    }
}
