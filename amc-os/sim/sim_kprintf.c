/* ============================================================
 * AMC OS v0.5 — sim kprintf + MSR sahtecileri + IPC köprüsü
 * sim/sim_kprintf.c
 *
 * Kernel kodundaki `void kprintf(const char*,...)` bildirimleri
 * ile uyumlu gerçek bir tanım; ek olarak VMX modülünün istediği
 * MSR yazma/okuma kayıtları ve WiFi sürücüsünün kullandığı
 * ipc_post_msg köprüsü burada sağlanır.
 * ============================================================ */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

int kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ---- sanal MSR uzayı (lineer liste, az sayıda kayıt yeter) ---- */
#define SIM_MSR_MAX 64
static struct { uint32_t msr; uint64_t val; } g_msrs[SIM_MSR_MAX];
static int g_msr_cnt = 0;

void sim_msr_write(uint32_t msr, uint64_t val) {
    for (int i = 0; i < g_msr_cnt; i++)
        if (g_msrs[i].msr == msr) { g_msrs[i].val = val; return; }
    if (g_msr_cnt < SIM_MSR_MAX) { g_msrs[g_msr_cnt].msr = msr; g_msrs[g_msr_cnt].val = val; g_msr_cnt++; }
}
uint64_t sim_msr_read(uint32_t msr) {
    for (int i = 0; i < g_msr_cnt; i++)
        if (g_msrs[i].msr == msr) return g_msrs[i].val;
    return 0;
}

/* ---- IPC köprüsü: kernel sürücüleri → sim userspace servisi ---- */
int ipc_post_msg(int svc, int type, int a, int b) {
    printf("[ipc] -> servis=%d tip=%d a=%d b=%d (sim wpaofd teslim aldi)\n", svc, type, a, b);
    return 0; /* başarı */
}
