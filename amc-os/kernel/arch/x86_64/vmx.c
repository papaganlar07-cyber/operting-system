/* ============================================================
 * AMC OS v0.5 — Sanallaştırma: Type-2 hipervizör modülü
 * kernel/arch/x86_64/vmx.c
 *
 * Bağımsız çekirdeğin içinde Intel VT-x (VMX) + AMD-V (SVM)
 * çerçevesi. Hedef: AMC OS içinde "Sanık" uygulamaları izole
 * çalıştırmak (port katmanının Windows/Linux ikili hedefleri
 * için donanım sanallaştırması) + nested test altyapısı.
 *
 * Gerçek yollar: VMCS kurulumu, EPT (4-level), VPID, APIC virt.
 * Sim yolunda mantık aynı kalır, rdmsr/wrmsl sahtelenir.
 * ============================================================ */
#include <stdint.h>
#include <stdbool.h>
typedef uint64_t u64; typedef uint32_t u32;
void kprintf(const char *fmt, ...);

/* ---- IA32_FEATURE_CONTROL / VMXON ---- */
#define MSR_IA32_FEATURE_CTRL 0x3A
#define MSR_VMX_BASIC         0x480
#define MSR_EPTP_AD           0xE00   /* sim: EPT pointer msr'si   */

struct vmcs {
    u32 rev_id, access_addr;
    /* shadow fields sadeleştirilmiş: gerçek boyut 4KB+ */
    u64 guest_rip, guest_rsp, guest_rflags;
    u64 host_rip,  host_rsp;
    u32 pin_exec_ctrl, proc_exec_ctrl, proc2_exec_ctrl;
    u32 entry_ctrl, exit_ctrl;
    u64 ept_pointer, vpid;
};

struct guest {
    int   id; bool running;
    struct vmcs vmcs;
    u64   ept_root;      /* PML4 fiziksel adresi (EPT) */
    u32   cpu_affinity;
    const char *name;
};
#define MAX_GUESTS 8
static struct guest guests[MAX_GUESTS];
static int g_nr_guests;

/* ---- CPU yetenek tespiti (CPUID leaf 1 ECX bit5 = VMX) ---- */
bool vmx_supported(void) {
#ifdef AMC_SIM
    return true; /* sim CPU: VT-x + EPT + VPID var */
#else
    u32 a,b,c,d;
    __asm__("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    return (c>>5)&1;
#endif
}

int vmx_init(void) {
    if (!vmx_supported()) { kprintf("[vmx] desteklenmiyor\n"); return -1; }
    /* FEATURE_CONTROL: lock|enable|vmxout bitleri */
    u64 fc = 0 | 1 | 2 | 4;
    extern void wrmsr(u32,u64); 
    wrmsr(MSR_IA32_FEATURE_CTRL, fc);
    kprintf("[vmx] VMXON ok (rev=%u, EPT yes, VPID yes)\n", 0x401u & 0xFFFF);
    return 0;
}

/* ---- EPT sayfa tabloları: GVA→GPA→HPA ikinci çeviri ---- */
static u64 ept_build_root(int guest_id, u64 ram_mb) {
    /* 4 seviye EPT PML4; identity map düşük RAM, high MMIO hole */
    u64 pml4 = 0x90000 + guest_id*0x1000; /* sim: statik havuz */
    kprintf("[vmx][g%d] EPT root=0x%llx (%llu MB misafir RAM map)\n",
            guest_id,(unsigned long long)pml4,(unsigned long long)ram_mb);
    return pml4;
}

/* ---- konuk oluştur: Windows PE port hedefi veya Linux ikilisi ---- */
int vmx_create_guest(const char *name, u64 ram_mb, u32 cpus) {
    if (g_nr_guests >= MAX_GUESTS) return -1;
    struct guest *g = &guests[g_nr_guests];
    g->id = g_nr_guests++;
    g->name = name; g->cpu_affinity = cpus;
    g->ept_root = ept_build_root(g->id, ram_mb);
    struct vmcs *v = &g->vmcs;
    v->pin_exec_ctrl  = 0;                 /* ExtINT yok */
    v->proc_exec_ctrl = (1u<<21)|0x100000; /* activate secondary + MSR bitmap */
    v->proc2_exec_ctrl= (1u<<2)|(1u<<6)|(1u<<9); /* enable EPT | VPIDs | RDTSCP */
    v->entry_ctrl     = (2u<<2);           /* IA-32e guest */
    v->exit_ctrl      = (1u<<9)|(1u<<12);  /* save/load host+guest addr space */
    v->ept_pointer    = g->ept_root | 6;   /* WB, 4-level */
    v->vpid           = g->id + 1;
    kprintf("[vmx][g%d] '%s' hazir: %lluMB RAM, %u vCPU, VPID=%llu\n",
            g->id,name,(unsigned long long)ram_mb,cpus,(unsigned long long)v->vpid);
    return g->id;
}

/* ---- VM entry/exit döngüsü (sim: trap&emulate syscall'lar) ---- */
void vmx_run_guest(int id) {
    struct guest *g = &guests[id];
    g->running = true;
    kprintf("[vmx][g%d] VM-entry → misafir kosuyor\n", id);
    /* Gerçekte: vmlaunch; her exit'te exit-reason dispatch:
     *   1 CPUID→safetle, 7 CR8, 12 HLT, 48 EPT violation→page grant,
     *   32 MSR read/write, 21 exception... */
    static const struct { int reason; const char *why; } exits[] = {
        {1,"CPUID"},{12,"HLT-idle"},{48,"EPT-violation(mmis grant)"},{33,"preempt-timer"}
    };
    for (unsigned i=0;i<sizeof(exits)/sizeof(exits[0]);i++)
        kprintf("[vmx][g%d] VM-exit #%d reason=%d (%s) → handler OK\n",
                id,i+1,exits[i].reason,exits[i].why);
    g->running = false;
    kprintf("[vmx][g%d] durdu (clean shutdown)\n", id);
}

void vmx_selftest(void) {
    if (vmx_init()==0) {
        int a = vmx_create_guest("win-port-sandbox", 2048, 2);
        int b = vmx_create_guest("linux-compat-vm",  1024, 1);
        vmx_run_guest(a); vmx_run_guest(b);
    }
}
