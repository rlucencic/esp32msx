/*
 * Sound: the AY-3-8910 (ports $A0-$A2), Konami's SCC (in SCC cartridges)
 * and the key click (PPI port C bit 7); the PSG's I/O ports read the
 * joysticks.
 *
 * As in ../esp32cpc, register writes become events stamped with their time
 * in output samples (the clock / 112: 31,960 Hz); the speaker's task replays
 * them into its own copy of the chips. The AY runs at the clock / 2, its
 * tone and noise counters at a sixteenth of that: 7 ticks per sample. The
 * SCC's five channels step through their 32-byte waveforms every
 * frequency + 1 clocks; channel 5 plays channel 4's waveform.
 */
#include <stdlib.h>
#include <string.h>
#include "msx_internal.h"

#define TICKS_PER_SAMPLE    7
#define EVENT_QUEUE         8192        /* power of two */
#define EV_SCC              0x100       /* + the SCC register ($00-$8F) */
#define EV_CLICK            0x200

static const uint8_t reg_mask[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
    0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

/* ---- Emulation side ---- */

typedef struct {
    uint64_t time;                      /* output sample */
    uint16_t reg;
    uint8_t value;
} audio_event_t;

static struct {
    uint8_t reg[16];
    uint8_t sel;
    uint8_t scc[0x90];                  /* waveforms $00-$7F, frequencies, volumes, enable */
    bool enabled;
    audio_event_t *queue;
    volatile uint32_t head, tail;
    volatile uint64_t now;              /* samples the render side may produce */
    uint32_t dropped;
} ay;

static void push_event(uint16_t reg, uint8_t value, int at)
{
    uint64_t t = msx_now(at) / T_PER_SAMPLE;
    if (t > ay.now) {
        __atomic_store_n(&ay.now, t, __ATOMIC_RELEASE);
    }
    if (!ay.enabled || ay.queue == NULL) {
        return;
    }
    uint32_t head = ay.head;
    if (head - __atomic_load_n(&ay.tail, __ATOMIC_ACQUIRE) >= EVENT_QUEUE) {
        ay.dropped++;
        return;
    }
    audio_event_t *e = &ay.queue[head % EVENT_QUEUE];
    e->time = t;
    e->reg = reg;
    e->value = value;
    __atomic_store_n(&ay.head, head + 1, __ATOMIC_RELEASE);
}

void psg_address(uint8_t v)
{
    ay.sel = v & 0x0F;
}

void psg_write(uint8_t v, int at)
{
    uint8_t r = ay.sel;
    ay.reg[r] = v & reg_mask[r];
    if (r < 14) {
        push_event(r, ay.reg[r], at);
    }
}

uint8_t psg_read(void)
{
    if (ay.sel == 14) {
        /* The joystick port register 15 selects; bit 6 the keyboard
         * layout (1: JIS), bit 7 the cassette input
         */
        int port = (ay.reg[15] >> 6) & 1;
        return 0x40 | msx.joy[port];
    }
    return ay.reg[ay.sel];
}

void psg_reset(void)
{
    for (int r = 0; r < 14; r++) {
        ay.sel = r;
        psg_write(r == 7 ? 0xB8 : 0, 0);
    }
    ay.reg[14] = 0xFF;
    ay.reg[15] = 0x0F;
    ay.sel = 0;
}

void scc_reset(void)
{
    memset(ay.scc, 0, sizeof(ay.scc));
    for (int r = 0x80; r < 0x90; r++) {
        push_event(EV_SCC + r, 0, 0);
    }
}

/* $9800-$98FF (mirrored up to $9FFF) */
uint8_t scc_read(uint16_t addr)
{
    int r = addr & 0xFF;
    if (r < 0x80) {
        return ay.scc[r];
    }
    if (r >= 0xA0 && r < 0xC0) {
        return ay.scc[0x60 + (r & 0x1F)];   /* channel 5: channel 4's waveform */
    }
    return 0xFF;
}

void scc_write(uint16_t addr, uint8_t v)
{
    int r = addr & 0xFF;
    if (r >= 0xA0) {
        return;
    }
    if (r >= 0x90) {
        r -= 0x10;                          /* $90-$9F mirror $80-$8F */
    }
    ay.scc[r] = v;
    push_event(EV_SCC + r, v, 0);
}

void click_write(bool level)
{
    push_event(EV_CLICK, level, 0);
}

void audio_frame_end(void)
{
    uint64_t t = msx.clock / T_PER_SAMPLE;
    if (t > ay.now) {
        __atomic_store_n(&ay.now, t, __ATOMIC_RELEASE);
    }
}

/* ---- Render side ---- */

static struct {
    uint8_t reg[14];
    uint16_t tone_count[3];
    uint8_t tone_out[3];
    uint16_t noise_count;
    uint32_t lfsr;
    uint8_t noise_out;
    uint32_t env_count;
    uint8_t env_pos;
    uint8_t env_attack, env_holding, env_hold_value;
    int8_t wave[4][32];
    uint16_t freq[5];
    uint8_t vol[5];
    uint8_t enable;
    uint32_t phase[5];                  /* 16.16 steps into the waveform */
    uint32_t step[5];
    int16_t click;
    int32_t dc;
    uint64_t pos;
} ren = { .lfsr = 1 };

static const uint16_t ay_level[16] = {
    0, 104, 151, 222, 327, 484, 675, 1103, 1299, 2088, 2917, 3661, 4621, 5802, 6899, 8191,
};

static void scc_step(int ch)
{
    uint32_t f = ren.freq[ch];
    /* Below 9 the SCC is silent */
    ren.step[ch] = f < 9 ? 0 : (uint32_t)(((uint64_t)T_PER_SAMPLE << 16) / (f + 1));
}

static void synth_write(uint16_t reg, uint8_t value)
{
    if (reg < 14) {
        ren.reg[reg] = value;
        if (reg == 13) {
            ren.env_count = 0;
            ren.env_pos = 0;
            ren.env_attack = (value & 0x04) != 0;
            ren.env_holding = 0;
        }
        return;
    }
    if (reg == EV_CLICK) {
        ren.click = value ? 3000 : 0;
        return;
    }
    int r = reg - EV_SCC;
    if (r < 0x80) {
        ren.wave[r >> 5][r & 31] = (int8_t)value;
    } else if (r < 0x8A) {
        int ch = (r - 0x80) >> 1;
        if (r & 1) {
            ren.freq[ch] = (ren.freq[ch] & 0xFF) | ((value & 15) << 8);
        } else {
            ren.freq[ch] = (ren.freq[ch] & 0xF00) | value;
        }
        scc_step(ch);
    } else if (r < 0x8F) {
        ren.vol[r - 0x8A] = value & 15;
    } else {
        ren.enable = value & 0x1F;
    }
}

static inline uint8_t env_level(void)
{
    uint8_t p = ren.env_holding ? ren.env_hold_value : ren.env_attack ? ren.env_pos : 31 - ren.env_pos;
    return p >> 1;
}

static inline void env_advance(uint32_t ticks)
{
    if (ren.env_holding) {
        return;
    }
    uint32_t ep = ren.reg[11] | (ren.reg[12] << 8);
    ep = ep ? ep : 1;
    ren.env_count += ticks;
    while (ren.env_count >= ep && !ren.env_holding) {
        ren.env_count -= ep;
        if (++ren.env_pos > 31) {
            uint8_t shape = ren.reg[13];
            bool cont = shape & 0x08, att = shape & 0x04, alt = shape & 0x02, hold = shape & 0x01;
            ren.env_pos = 0;
            if (!cont) {
                ren.env_holding = 1;
                ren.env_hold_value = 0;
            } else if (hold) {
                ren.env_holding = 1;
                ren.env_hold_value = (alt ^ att) ? 31 : 0;
            } else if (alt) {
                ren.env_attack ^= 1;
            }
        }
    }
}

static inline void noise_advance(uint32_t ticks)
{
    uint32_t np = ren.reg[6] ? ren.reg[6] * 2 : 2;
    ren.noise_count += ticks;
    while (ren.noise_count >= np) {
        ren.noise_count -= np;
        uint32_t bit = (ren.lfsr ^ (ren.lfsr >> 3)) & 1;
        ren.lfsr = (ren.lfsr >> 1) | (bit << 16);
    }
    ren.noise_out = ren.lfsr & 1;
}

static inline uint32_t tone_advance(int ch, uint32_t ticks)
{
    uint32_t period = ren.reg[ch * 2] | (ren.reg[ch * 2 + 1] << 8);
    period = period ? period : 1;
    uint32_t count = ren.tone_count[ch];
    if (count >= period) {
        count = period - 1;
    }
    uint8_t out = ren.tone_out[ch];
    uint32_t high = 0;
    while (ticks) {
        uint32_t to_toggle = period - count;
        if (to_toggle > ticks) {
            high += out ? ticks : 0;
            count += ticks;
            break;
        }
        high += out ? to_toggle : 0;
        ticks -= to_toggle;
        count = 0;
        out ^= 1;
    }
    ren.tone_count[ch] = count;
    ren.tone_out[ch] = out;
    return high;
}

static inline int16_t clip16(int32_t v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static void MSX_HOT render_span(int16_t *dst, int n)
{
    uint8_t mixer = ren.reg[7];
    bool scc = ren.enable != 0;
    for (int i = 0; i < n; i++) {
        env_advance(TICKS_PER_SAMPLE);
        noise_advance(TICKS_PER_SAMPLE);
        uint8_t env = env_level();
        int32_t sum = 0;
        for (int c = 0; c < 3; c++) {
            bool tone_off = (mixer >> c) & 1;
            bool noise_off = (mixer >> (c + 3)) & 1;
            uint32_t high = tone_off ? TICKS_PER_SAMPLE : tone_advance(c, TICKS_PER_SAMPLE);
            if ((noise_off || ren.noise_out) && high) {
                uint8_t vol = ren.reg[8 + c];
                sum += ay_level[(vol & 0x10) ? env : vol & 15] * high / TICKS_PER_SAMPLE;
            }
        }
        if (scc) {
            for (int c = 0; c < 5; c++) {
                ren.phase[c] += ren.step[c];
                if ((ren.enable >> c) & 1) {
                    sum += ren.wave[c < 4 ? c : 3][(ren.phase[c] >> 16) & 31] * ren.vol[c] * 2;
                }
            }
        }
        sum += ren.click;
        ren.dc += ((sum << 8) - ren.dc) >> 10;
        int16_t s = clip16(sum - (ren.dc >> 8));
        *dst++ = s;
        *dst++ = s;
    }
}

int MSX_HOT msx_audio_render(int16_t *dst, int frames)
{
    uint64_t limit = __atomic_load_n(&ay.now, __ATOMIC_ACQUIRE);
    if (ren.pos + frames > limit) {
        frames = limit > ren.pos ? limit - ren.pos : 0;
    }
    int done = 0;
    while (done < frames) {
        uint32_t tail = ay.tail;
        uint32_t head = __atomic_load_n(&ay.head, __ATOMIC_ACQUIRE);
        while (tail != head && ay.queue[tail % EVENT_QUEUE].time <= ren.pos) {
            synth_write(ay.queue[tail % EVENT_QUEUE].reg, ay.queue[tail % EVENT_QUEUE].value);
            tail++;
        }
        __atomic_store_n(&ay.tail, tail, __ATOMIC_RELEASE);
        int n = frames - done;
        if (tail != head) {
            uint64_t next = ay.queue[tail % EVENT_QUEUE].time;
            if (next - ren.pos < (uint64_t)n) {
                n = next - ren.pos;
            }
        }
        render_span(dst + done * 2, n);
        ren.pos += n;
        done += n;
    }
    return done;
}

int msx_audio_pending(void)
{
    return __atomic_load_n(&ay.now, __ATOMIC_ACQUIRE) - ren.pos;
}

void msx_audio_enable(bool enable)
{
    ay.enabled = enable;
    if (enable && ay.queue == NULL) {
        ay.queue = msx_alloc(EVENT_QUEUE * sizeof(audio_event_t));
    }
}

void audio_init(void)
{
    if (ay.enabled && ay.queue == NULL) {
        ay.queue = msx_alloc(EVENT_QUEUE * sizeof(audio_event_t));
    }
    ay.head = ay.tail = 0;
    uint64_t now = msx.clock / T_PER_SAMPLE;
    ay.now = now;
    memset(&ren, 0, sizeof(ren));
    ren.lfsr = 1;
    ren.pos = now;
}
