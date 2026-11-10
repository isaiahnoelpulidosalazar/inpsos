#include "timer.h"
#include "libc.h"
#include "io.h"

/* The original hard-coded `rdtsc / 2000000`, i.e. "every CPU is 2 GHz", so sleeps and
 * timestamps were wrong on any other machine.  We now calibrate against the PIT (a fixed
 * 1.193182 MHz crystal) and read the real-time clock for the calendar date. */

#define PIT_HZ 1193182u

static uint32_t tsc_khz;             /* zero-initialised on purpose: no mutable .data in the kernel image */
static uint64_t tsc_base;
static int64_t  unix_base_ms;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void calibrate_tsc(void) {
    tsc_khz = 2000000;                                   /* fallback if calibration times out */
    const uint16_t reload = 11932;                       /* 11932 / 1193182 Hz ~= 10 ms */
    outb(0x61, (uint8_t)((inb(0x61) & ~0x02) | 0x01));   /* gate PIT ch.2 on, speaker off */
    outb(0x43, 0xB0);                                    /* ch.2, lo/hi byte, mode 0 */
    outb(0x42, reload & 0xFF);
    outb(0x42, reload >> 8);
    uint64_t t0 = rdtsc();
    for (uint32_t guard = 0; guard < 50000000u; guard++) {
        if (inb(0x61) & 0x20) {                          /* OUT2 high: terminal count reached */
            uint64_t cycles = rdtsc() - t0;
            uint32_t khz = (uint32_t)(cycles / 10);      /* cycles per 10 ms == kHz */
            if (khz >= 100000u) tsc_khz = khz;           /* sanity: >= 100 MHz */
            return;
        }
    }
}

/* ---- CMOS real-time clock ---- */
static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static uint8_t bcd(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

typedef struct { uint8_t s, m, h, d, mo, y, c; } RtcRaw;

static void rtc_snapshot(RtcRaw* r) {
    while (cmos(0x0A) & 0x80) { }                        /* update in progress */
    r->s = cmos(0x00); r->m = cmos(0x02); r->h = cmos(0x04);
    r->d = cmos(0x07); r->mo = cmos(0x08); r->y = cmos(0x09); r->c = cmos(0x32);
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp  = m > 2 ? m - 3 : m + 9;
    unsigned doy = (153 * mp + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t rtc_unix_seconds(void) {
    RtcRaw a, b;
    rtc_snapshot(&a);
    for (int i = 0; i < 4; i++) {                        /* read until two snapshots agree */
        rtc_snapshot(&b);
        if (memcmp(&a, &b, sizeof a) == 0) break;
        a = b;
    }
    uint8_t regb = cmos(0x0B);
    bool pm = (a.h & 0x80) != 0;
    if (!(regb & 0x04)) {                                /* BCD -> binary */
        a.s = bcd(a.s); a.m = bcd(a.m); a.h = bcd(a.h & 0x7F); a.d = bcd(a.d);
        a.mo = bcd(a.mo); a.y = bcd(a.y); a.c = bcd(a.c);
    } else a.h &= 0x7F;
    if (!(regb & 0x02) && pm) a.h = (uint8_t)((a.h % 12) + 12);   /* 12-hour clock */
    int year = (a.c >= 19 && a.c <= 30 ? a.c : 20) * 100 + a.y;
    int64_t days = days_from_civil(year, a.mo ? a.mo : 1, a.d ? a.d : 1);
    return days * 86400 + a.h * 3600 + a.m * 60 + a.s;
}

void timer_init(void) {
    calibrate_tsc();
    tsc_base = rdtsc();
    unix_base_ms = rtc_unix_seconds() * 1000;
}

int64_t timer_uptime_ms(void) { return (int64_t)((rdtsc() - tsc_base) / tsc_khz); }
int64_t timer_unix_ms(void)   { return unix_base_ms + timer_uptime_ms(); }

void timer_sleep_ms(int64_t ms) {
    int64_t end = timer_uptime_ms() + ms;
    while (timer_uptime_ms() < end) __asm__ volatile ("pause");
}
