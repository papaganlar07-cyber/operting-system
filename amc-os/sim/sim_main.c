/* ============================================================
 * AMC OS v0.5 — Simülasyon ana döngüsü (make run-sim)
 * Kernel alt sistemlerini gerçek sırayla boot eder:
 *   time → PCI enumeration → driver bind → HDA → xHCI → GPU sched
 *   → NVMe → WiFi → CFS scheduler stress → VMX selftest
 * Gerçek donanım register semantiği sim_hw.c'de.
 * ============================================================ */
#include <stdio.h>
#include "posix_compat.h"
typedef unsigned long long u64;

void sim_hw_init(void); void sim_hw_tick(void);
void timesrc_init(void); u64 shim_uptime(void);
int  nvme_init(void*,int); int nvme_get_smart(void*); void nvme_shutdown(void);
void wifi_init(void); void vmx_selftest(void);
/* sürücü self-test köprüleri (sim derlemesinde aktif): */
void hda_sim_test(void); void xhci_sim_test(void); void gpu_sched_sim_test(void);
void sched_sim_stress(void); void amcfs_sim_test(void); void ports_sim_demo(void);
void personalization_sim_test(void);

u64 shim_uptime(void){ extern u64 k_uptime_ms(void); return k_uptime_ms(); }

int main(int argc, char **argv) {
    (void)argc;(void)argv;
    printf("=====================================================\n");
    printf("= AMC OS v0.6 SIM — sanal anakart + gercek kod yollari =\n");
    printf("=====================================================\n");
    sim_hw_init();
    timesrc_init();

    /* ---- PCI taraması: sim cihazlarını bul, eşleştir ---- */
    printf("\n[pci] enumerasyon:\n");
    struct { int b,d,f; const char*what; } found[] = {{0,0x1f,3,"HDA"},{0,0x1f,6,"RTL8169"},
        {0,0x14,0,"xHCI"},{0,0x1d,7,"EHCI"},{1,0,0,"IntelGPU"},{2,0,0,"AMDGPU"},{3,0,0,"NVMe"},{4,0,0,"WiFi"}};
    for (unsigned i=0;i<sizeof(found)/sizeof(found[0]);i++){
        uint32_t v = pci_conf_read(found[i].b,found[i].d,found[i].f,0);
        printf("  %02x:%02x.%d -> %04x:%04x [%s]\n",found[i].b,found[i].d,found[i].f,
               v&0xFFFF,(v>>16)&0xFFFF,found[i].what);
    }

    hda_sim_test();       /* Realtek verb motoru + widget graph + jack sense */
    xhci_sim_test();      /* port reset state machine + hiz anlasmasi */
    gpu_sched_sim_test(); /* ring submit + fence + preempt */
    printf("\n[nvme] baslatma:\n");
    if (nvme_init(0,4)==0) {
        struct { char pad[512]; } smartbuf; 
        /* health log oku (sim: zero page → temp 0K olmasın diye sahte doldur) */
        extern void sim_fill_fake_smart(void*);
        sim_fill_fake_smart(&smartbuf);
        nvme_get_smart((void*)&smartbuf);
    }
    wifi_init();
    sched_sim_stress();   /* CFS: vruntime denge + priority inversion testi */
    amcfs_sim_test();     /* journaling commit/replay */
    ports_sim_demo();     /* Linux/FreeBSD/OpenBSD port demo */
    vmx_selftest();
    personalization_sim_test(); /* v0.6: tema+animasyon+AI */

    for (int i=0;i<3;i++) sim_hw_tick();
    nvme_shutdown();
    printf("\n[SIM] boot tamamlandi — uptime %llu ms. Hos geldin!\n",
           (unsigned long long)shim_uptime());
    return 0;
}
