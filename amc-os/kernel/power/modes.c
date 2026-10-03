/* ============================================================
 * MacawOS — Kullanım Modu Motoru (v0.7)
 * 4 profil + Tablet Modu katmanı. Her mod; zamanlayıcı dilimi,
 * GPU önceliği, animasyon bütçesi, çalışan servis listesi ve
 * güç politikasını değiştirir. Windows Server gibi "sade" değil:
 * görsellik korunur, sadece işe yaramayan şeyler kapatılır.
 *
 * Gerçek OS'ta bu tablo powerd + sessiond'a IPC ile yayınlanır;
 * composd animasyon bütçesinden, sched QoS sınıfından etkilenir.
 * ============================================================ */
#ifndef MACAW_MODES_C
#define MACAW_MODES_C

#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef enum {
    MODE_NORMAL = 0,   /* günlük kullanım: dengeli              */
    MODE_OFFICE,       /* çalışma/ofis: doküman+maile odaklı     */
    MODE_SERVER,       /* sunucu: masaüstü katmanı minimum       */
    MODE_DEV,          /* yazılımcı: derleme/terminal öncelikli  */
    N_MODES
} os_mode;

/* Katman olarak üstüne biner: tablet dokunmatik iyileştirmeleri
 * hangi modda olursa olsun uygulanır (parmak hedefleri büyür). */
typedef struct {
    int      enabled;          /* 1 = tablet modu açık           */
    int      hit_scale_pct;    /* dokunma hedefi büyütme (%)     */
    int      gesture_nav;      /* kenar kaydırma = geri/ana ekran*/
    int      onscreen_kb;      /* sanal klavye                   */
    int      auto_rotate;      /* jiroskop yön değişimi          */
    int      pen_support;      /* stylus + basınç eğrisi         */
    int      dock_as_desktop;  /* harici klavye takılınca masaüstü düzenine geç */
} tablet_profile;

typedef struct {
    os_mode  mode;
    const char *name;          /* kullanıcıya görünen ad          */
    const char *desc;

    /* ---- zamanlayıcı / CPU ---- */
    uint32_t sched_slice_us;   /* normal preemptiyon dilimi       */
    int      idle_deep_sleep;  /* C-state agresifliği 0..3        */
    int      max_perf_cores;   /* 0 = hepsi                       */

    /* ---- GPU & görsel ---- */
    int      anim_budget_pct;  /* 0 => animasyonlar kapalı (sade
                                  ama ÇİRKİN değil: anında snap)  */
    int      compositor_on;    /* 0 = drectx benzeri hafif yol    */
    int      gpu_priority;     /* 0 düşük .. 10 (ring ağırlığı)   */
    int      vsync;            /* 0 = düşük gecikme (derleme)     */

    /* ---- servisler ---- */
    const char *services_off;  /* virgülle: kapatılan daemonlar   */
    const char *services_extra;/* virgülle: eklenen servisler     */

    /* ---- güç/ısı ---- */
    int      tdp_watt_cap;     /* 0 = sınırsız                    */
    int      fan_curve;        /* 0 sessiz 1 dengeli 2 performans */

    /* ---- arayüz ---- */
    int      taskbar_widgets;  /* hava/cihaz kartları             */
    int      notifications;    /* bildirim merkezi                */
    int      screensaver;      /* boşta ekran koruma              */

    tablet_profile tablet;     /* bu modun tablet ayarları        */
} mode_profile;

/* ---------------- 4 ana mod tablosu ---------------- */
static mode_profile MODES[N_MODES] = {
 [MODE_NORMAL] = {
    .mode = MODE_NORMAL, .name = "Normal",
    .desc = "Gunluk kullanim: medya, web, oyun. Dengeli gorunum+performans.",
    .sched_slice_us = 4000, .idle_deep_sleep = 2, .max_perf_cores = 0,
    .anim_budget_pct = 100, .compositor_on = 1, .gpu_priority = 5, .vsync = 1,
    .services_off = "", .services_extra = "",
    .tdp_watt_cap = 0, .fan_curve = 1,
    .taskbar_widgets = 1, .notifications = 1, .screensaver = 1,
    .tablet = {0, 130, 1, 1, 1, 0, 1},
 },
 [MODE_OFFICE] = {
    .mode = MODE_OFFICE, .name = "Ofis",
    .desc = "Calisma modu: dokuman/mail/toplanti oncelikli. Dikkat bozanca her sey sessiz.",
    .sched_slice_us = 6000, .idle_deep_sleep = 3, .max_perf_cores = 0,
    .anim_budget_pct = 80, .compositor_on = 1, .gpu_priority = 3, .vsync = 1,
    /* medya oynatıcı/hazırlık süreçleri askıya alınır, pil öne çıkar */
    .services_off = "mediapreloader,gamelauncher", .services_extra = "focusd,dndd",
    .tdp_watt_cap = 28, .fan_curve = 0,
    .taskbar_widgets = 1, .notifications = 0 /* toplantıda sustur */, .screensaver = 1,
    .tablet = {0, 140, 1, 1, 0, 1, 1},
 },
 [MODE_SERVER] = {
    .mode = MODE_SERVER, .name = "Server",
    .desc = "Sunucu: arayuz minimum ama duzgun (koyu, sade panel). Gereksiz her sey KAPALI.",
    .sched_slice_us = 2000, .idle_deep_sleep = 0, .max_perf_cores = 0,
    .anim_budget_pct = 0, .compositor_on = 0, .gpu_priority = 1, .vsync = 0,
    /* masaüstü süsleri kapalı; sshd/docker tarzı servisler açık */
    .services_off = "wallpaperd,notificationd,screensaverd,gamelauncher,mediapreloader,searchindexd",
    .services_extra = "sshd,webconsoled,logaggd",
    .tdp_watt_cap = 0, .fan_curve = 2,
    .taskbar_widgets = 0, .notifications = 0, .screensaver = 0,
    .tablet = {0, 100, 0, 0, 0, 0, 0},
 },
 [MODE_DEV] = {
    .mode = MODE_DEV, .name = "Yazilimci",
    .desc = "Gelistirme: derleme tum cekirdeklere yayilir, terminal+editor hizli, GPU is yuku dusuk.",
    .sched_slice_us = 2000, .idle_deep_sleep = 1, .max_perf_cores = 0,
    .anim_budget_pct = 60, .compositor_on = 1, .gpu_priority = 2, .vsync = 0,
    .services_off = "gamelauncher,searchindexd", .services_extra = "amcdbd,containerd,lintd",
    .tdp_watt_cap = 0, .fan_curve = 2,
    .taskbar_widgets = 1, .notifications = 1, .screensaver = 0,
    .tablet = {0, 120, 1, 1, 0, 0, 1},
 },
};

static os_mode g_current = MODE_NORMAL;
static int     g_tablet_override = 0;   /* kullanıcı tablet modunu elle açtı */
static int     g_anim_off_user   = 0;   /* kullanıcı animasyonları elle kapattı */

const mode_profile *mode_current(void) { return &MODES[g_current]; }
const char *mode_name(os_mode m) { return MODES[m].name; }

os_mode mode_parse(const char *s) {
    for (int i = 0; i < N_MODES; i++) {
        /* büyük/küçük harf duyarsız + kısaltma kabulü:
         * "normal|n", "ofis|office|o", "server|srv|s", "yazilimci|dev|developer|d" */
        const char *b = MODES[i].name;
        int ok = 1;
        for (const char *a = s; *a && *b; a++, b++)
            if ((*a | 0x20) != (*b | 0x20)) { ok = 0; break; }
        if (ok && !*b) return (os_mode)i;
        static const char *alias[N_MODES] = {"n", "o", "srv", "dev"};
        for (const char *a = s, *c = alias[i]; ; a++, c++)
            if (((*a | 0x20) != *c) || (!*a && *c)) { ok = 0; break; }
            else if (!*a && !*c) return (os_mode)i;
    }
    return N_MODES;
}

/* Mod geçişi: eski modun kapattığı servisler geri, yenisininki uygulanır.
 * Gerçek OS'ta: powerd → ACPI _PTC, sched → QoS sınıfı, composd → bütçe. */
int mode_switch(os_mode to, int quiet) {
    if (to >= N_MODES) return -1;
    const mode_profile *old = &MODES[g_current], *np = &MODES[to];
    if (!quiet) {
        printf("[modes] %s -> %s\n", old->name, np->name);
        printf("  slice=%uus deepsleep=%d anim%%%d compositor=%d gpu_pri=%d tdp=%dW fan=%d\n",
               np->sched_slice_us, np->idle_deep_sleep, np->anim_budget_pct,
               np->compositor_on, np->gpu_priority, np->tdp_watt_cap, np->fan_curve);
        if (*np->services_off)    printf("  kapatilan servisler : %s\n", np->services_off);
        if (*np->services_extra)  printf("  eklenen servisler   : %s\n", np->services_extra);
        if (np->anim_budget_pct == 0)
            printf("  not: animasyon butcesi 0 → pencereler ANINDA acilir (sade ama duzenli)\n");
    }
    g_current = to;
    return 0;
}

/* ---------- Tablet modu (katman) ---------- */
void tablet_set(int on) { g_tablet_override = on; }
int  tablet_is_on(void) { return g_tablet_override || mode_current()->tablet.enabled; }

const tablet_profile *tablet_active(void) {
    static tablet_profile eff;
    const tablet_profile *base = &mode_current()->tablet;
    eff = *base;
    if (g_tablet_override) {
        eff.enabled = 1;
        if (eff.hit_scale_pct < 130) eff.hit_scale_pct = 130;
        eff.gesture_nav = 1; eff.onscreen_kb = 1; eff.auto_rotate = 1;
    }
    return &eff;
}

/* USB/termal olaydan otomatik geçiş (gerçek OS'ta powerd'ın event kuyruğu):
 *   - dokunmatik ekran + klavye YOK            → tablet açılır
 *   - harici klavye/dock TAKILI                → tablet kapanır, masaüstü düzeni
 *   - kapak kapatıldı                          → Server benzeri başsız moda öneri
 * Sim'de sürücüler (xHCI HID / HDA jack gibi) bu fonksiyonu çağırır. */
void tablet_event(int touch_available, int keyboard_attached) {
    if (touch_available && !keyboard_attached && !g_tablet_override) {
        tablet_set(1);
        printf("[modes] otomatik: dokunmatik algilandi, klavye yok -> TABLET modu ACIK\n");
    } else if (!touch_available && keyboard_attached && g_tablet_override) {
        tablet_set(0);
        printf("[modes] otomatik: dock/klavye takili -> tablet kapandi, masaustu duzeni\n");
    }
}

/* UI'ın sorduğu pratik sorular — tek yerden karar: */
int ui_anim_enabled(void)   { return !g_anim_off_user && mode_current()->anim_budget_pct > 0; }
int ui_anim_budget(void)    { return g_anim_off_user ? 0 : mode_current()->anim_budget_pct; }
int ui_hit_scale(void)      { return tablet_active()->enabled ? tablet_active()->hit_scale_pct : 100; }
int ui_gesture_nav(void)    { return tablet_active()->gesture_nav; }
int ui_onscreen_kb(void)    { return tablet_active()->enabled; }

/* CLI: `macawmode server`, `macawmode tablet on|off`, `macawmode anim off` */
int macawmode_cli(int argc, char **argv, int quiet) {
    if (argc < 2) {
        const mode_profile *m = mode_current();
        printf("aktif mod: %s (%s)\n", m->name, m->desc);
        printf("tablet   : %s\n", tablet_active()->enabled ? "ACIK" : "kapali");
        return 0;
    }
    if (!strcmp(argv[1], "anim")) {
        /* animasyonları tamamen kapatma/açma (Ayarlar panelinin CLI'ı) */
        if (argc >= 3 && !strcmp(argv[2], "off")) { g_anim_off_user = 1; if(!quiet)printf("[modes] animasyonlar KAPALI — pencereler aninda acilip kapanir (duzen korunur)\n"); return 0; }
        if (argc >= 3 && !strcmp(argv[2], "on"))  { g_anim_off_user = 0; if(!quiet)printf("[modes] animasyonlar ACIK\n"); return 0; }
        printf("[modes] animasyon: %s\n", ui_anim_enabled() ? "acik" : "kapali"); return 0;
    }
    if (!strcmp(argv[1], "tablet")) {
        if (argc >= 3 && !strcmp(argv[2], "on"))  { tablet_set(1); if(!quiet)printf("[modes] tablet modu ACIK: parmak hedefleri x%.2f, jest navigasyonu, sanal klavye\n", tablet_active()->hit_scale_pct/100.0); return 0; }
        if (argc >= 3 && !strcmp(argv[2], "off")) { tablet_set(0); if(!quiet)printf("[modes] tablet modu kapandi\n"); return 0; }
        printf("[modes] tablet: %s\n", tablet_is_on() ? "acik" : "kapali"); return 0;
    }
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; i < N_MODES; i++)
            printf("%-9s %s%s\n", MODES[i].name, MODES[i].desc,
                   i == (int)g_current ? "   <= aktif" : "");
        return 0;
    }
    os_mode m = mode_parse(argv[1]);
    if (m >= N_MODES) { printf("bilinmeyen mod: %s (Normal|Ofis|Server|Yazilimci)\n", argv[1]); return 2; }
    return mode_switch(m, quiet);
}
#endif /* MACAW_MODES_C */
