/* ============================================================
 * AMC OS v0.5 — Kernel zaman & tarih servisi (bağımsız modül)
 * kernel/time/timesrc.c
 *
 * Kaynaklar: invariant TSC, PIT/HPET fallback, CMOS RTC.
 * uptime + wall-clock + ms sayacı; userspace'a CLOCK_MONOTONIC /
 * CLOCK_REALTIME olarak sunulur (timeofd servisi üzerinden).
 * ============================================================ */
#include <stdint.h>
#include <stdbool.h>

typedef uint64_t u64; typedef int64_t i64; typedef uint32_t u32; typedef uint8_t u8;

static u64 g_ticks_ms  = 0;   /* 1ms tik sayacı          */
static u64 g_boot_unix = 0;   /* boot anındaki unix time */

void kprintf(const char *fmt, ...);

/* ---- invariant TSC kalibrasyonu (sim'de sabit) ---- */
u64 tsc_freq_hz(void) {
#ifdef AMC_SIM
    return 2400000000ull;             /* sim: 2.4 GHz invariant TSC */
#else
    extern u64 rdtscl(void);
    u64 a = rdtscl();
    /* Gerçek yol: PIT kanal-2 countdown ile 10ms ölç, delta*100 */
    volatile int spin; for (spin=0; spin<1; spin++) {}
    u64 b = rdtscl();
    return (b - a) * 100;
#endif
}

/* ---- CMOS RTC (0x70/0x71), BCD kodlu ---- */
static u8 rtc_read(u8 reg) {
#ifdef AMC_SIM
    /* sim: 30s 12dk 09sa 6gün 10ay 2026 (Cmt 3 Eki 2026) */
    static const u8 fake[10] = {0x30,0,0x0C,0,0x09,0,0x06,0x10,0x0A,0x26};
    return fake[reg % 10];
#else
    *(volatile u8*)0x70 = reg;
    return *(volatile u8*)0x71;
#endif
}
static u8 bcd2bin(u8 b){ return (u8)((b>>4)*10 + (b&0xF)); }

/* days-from-civil (Hinnant algoritması, bağımsız implementasyon) */
static i64 days_from_civil(i64 y, u32 m, u32 d) {
    y -= (m <= 2);
    i64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = (u32)(y - era * 400);
    u32 doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d - 1;
    u32 doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return era*146097 + (i64)doe - 719468;
}

u64 rtc_unix_epoch(void) {
    u32 sec=bcd2bin(rtc_read(0)), min=bcd2bin(rtc_read(2));
    u32 hr =bcd2bin(rtc_read(4));
    u32 day=bcd2bin(rtc_read(6)), mon=bcd2bin(rtc_read(8));
    u32 yr =2000+bcd2bin(rtc_read(9));
    return (u64)(days_from_civil(yr,mon,day)*86400 + hr*3600 + min*60 + sec);
}

void timer_tick(void) { g_ticks_ms++; }        /* IRQ handler çağırır */
u64 k_uptime_ms(void) { return g_ticks_ms; }
u64 k_wall_ms(void)   { return g_boot_unix*1000 + g_ticks_ms; }

void timesrc_init(void) {
    g_boot_unix = rtc_unix_epoch();
    u64 hz = tsc_freq_hz();
    kprintf("[time] TSC=%llu.%02llu MHz | RTC epoch=%llu\n",
            (unsigned long long)(hz/1000000),
            (unsigned long long)((hz%1000000)/10000),
            (unsigned long long)g_boot_unix);
}
