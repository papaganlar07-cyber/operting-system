/* ============================================================
 * AMC OS — Tema & Kişiselleştirme Motoru (v0.6)
 * Hedef: Linux'tan daha derin kişiselleştirme.
 *  - 12 hazır tema + kullanıcı tanımlı tema (JSON benzeri .amcthem)
 *  - Duvar kağıdı: degrade, resim dosyası (PNG/BMP yükleyici), "çiz" modu
 *  - Pencere süslemeleri: başlık çubuğu rengi/hacmi, köşe yarıçapı,
 *    gölge yumuşaklığı, şeffaflık (opacity)
 *  - Simge setleri, yazı tipi boyutu, görev çubuğu konumu/boyutu
 *  - Animasyon motoru ayarları (speed, easing curve, efekt seçimi)
 *  - Ayarlar CLI'dan da değiştirilebilir: amctheme set accent #ff5555
 * ============================================================ */
#ifndef AMC_THEME_C
#define AMC_THEME_C

#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef uint32_t argb_t; /* 0xAARRGGBB */

static inline argb_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((argb_t)r << 16) | ((argb_t)g << 8) | b;
}
static inline argb_t mix(argb_t a, argb_t b, int t) { /* t: 0..256 */
    uint8_t ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    uint8_t br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return rgb((ar * (256 - t) + br * t) >> 8,
               (ag * (256 - t) + bg * t) >> 8,
               (ab * (256 - t) + bb * t) >> 8);
}
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ---------- pencere dekorasyon stilleri ---------- */
typedef enum {
    WIN_DEC_CIRCLES = 0,   /* macOS tarzı solda 3 nokta        */
    WIN_DEC_LINES,         /* ince çizgi butonlar              */
    WIN_DEC_GLYPHS,        /* klasik ✕ ─ □                     */
    WIN_DEC_CUSTOM_IMG     /* kullanıcı resmi (kapat/min/max)  */
} win_deco_style;

typedef struct {
    char     path[64];      /* /usr/share/wallpapers/x.png     */
    int      is_generated;  /* 1 = prosedürel degrade/çizim    */
    /* prosedürel "çiz" parametreleri (kullanıcı tasarlayabilir)*/
    argb_t   gen_c1, gen_c2;
    int      gen_angle;     /* 0-360                           */
    int      gen_pattern;   /* 0 düz 1 dalga 2 yıldızlar 3 ızgara */
} wallpaper_desc;

typedef struct {
    char name[32];
    /* renk paleti */
    argb_t bg, fg, accent, surface, topbar, taskbar, selection, shadow;
    /* duvar kağıdı */
    wallpaper_desc wall;
    /* pencere süsleme */
    win_deco_style deco;
    int      corner_radius;   /* 0..24 px                      */
    int      titlebar_height; /* 24..48                        */
    int      opacity;         /* 200..255 (pencere şeffaflığı) */
    int      shadow_softness; /* 0..32                         */
    /* simge/yazı */
    char     iconset[32];     /* "AMC-Filled","AMC-Line","Custom" */
    int      font_scale;      /* 90..140 (%)                   */
    /* görev çubuğu */
    int      taskbar_pos;     /* 0 alt 1 üst 2 sol 3 sağ       */
    int      taskbar_size;    /* 36..64                        */
    int      taskbar_autohide;
    /* animasyon */
    int      anim_enabled;
    int      anim_speed;      /* 50..200 (%)                   */
    int      anim_easing;     /* 0 linear 1 ease-out 2 spring 3 bounce */
    char     anim_open[24];   /* "scale","fade"," genie","slide" */
    char     anim_close[24];
    /* açık/koyu otomatik */
    int      auto_dark;       /* saat 19-07 arası koyuya geç   */
} amc_theme;

/* ---------- 12 hazır tema ---------- */
static const amc_theme BUILTINS[] = {
 /* isim        bg        fg        accent    surface   topbar    taskbar   sel       shadow */
 {"AmcNight",  0xFF1E1E2Eu, 0xFFCDD6F4u, 0xFF89B4FAu, 0xFF313244u, 0xFF11111Bu, 0xFF181825u, 0xFF45476Au, 0xFF000000u},
 {"AmcDay",    0xFFEFF0F5u, 0xFF4C4F68u, 0xFF1E66DCu, 0xFFCCD0E4u, 0xFFE6E6EDu, 0xFFDCDDE6u, 0xFF9CB2FEu, 0xFF000000u},
 {"Turkuaz",   0xFF0F2E33u, 0xFFD7F0EEu, 0xFF2ED4A2u, 0xFF1B444Au, 0xFF081F23u, 0xFF0B262Au, 0xFF1F635Cu, 0xFF000000u},
 {"Pusula",    0xFF241B0Fu, 0xFFF2E4C9u, 0xFFF0A54Au, 0xFF3A2C1Au, 0xFF181108u, 0xFF1D150Au, 0xFF5C4324u, 0xFF000000u},
 {"Kiraz",     0xFF2A0F1Cu, 0xFFFFE6F0u, 0xFFFF5C8Au, 0xFF401A2Cu, 0xFF1A0811u, 0xFF200A15u, 0xFF6B2A44u, 0xFF000000u},
 {"Orman",     0xFF102414u, 0xFFD8F0D8u, 0xFF6CD46Cu, 0xFF1D3A22u, 0xFF08160Au, 0xFF0A1C0Cu, 0xFF2C5C33u, 0xFF000000u},
 {"Lav",       0xFF260D08u, 0xFFFFE0D0u, 0xFFFF6B35u, 0xFF3D160Eu, 0xFF160603u, 0xFF1C0804u, 0xFF7A2E18u, 0xFF000000u},
 {"Buz",       0xFF0D1B2Eu, 0xFFE0F0FFu, 0xFF5CBCFFu, 0xFF162C44u, 0xFF061220u, 0xFF081626u, 0xFF2A4C72u, 0xFF000000u},
 {"Nane",      0xFFF2FBF5u, 0xFF224030u, 0xFF00A86Du, 0xFFD8EFE0u, 0xFFE8F5ECu, 0xFFDCEDE2u, 0xFF9FD8B5u, 0xFF000000u},
 {"Kahve",     0xFF2B2016u, 0xFFF0E2CFu, 0xFFCA9A56u, 0xFF3E2F21u, 0xFF1C140Du, 0xFF221810u, 0xFF5C462Cu, 0xFF000000u},
 {"Mor Sis",   0xFF1A122Bu, 0xFFE8DFF7u, 0xFFB16AFFu, 0xFF2A1E42u, 0xFF100A1Cu, 0xFF140D22u, 0xFF4A3370u, 0xFF000000u},
 {"Gümüş",     0xFF24272Au, 0xFFE4E7EBu, 0xFFA0A8B0u, 0xFF353A40u, 0xFF1B1E21u, 0xFF202326u, 0xFF454B52u, 0xFF000000u},
};
#define N_BUILTINS (sizeof(BUILTINS)/sizeof(BUILTINS[0]))

/* varsayılan dolgu: deco/anim alanları */
static void theme_defaults(amc_theme *t) {
    t->deco = WIN_DEC_CIRCLES;
    t->corner_radius = 12;
    t->titlebar_height = 34;
    t->opacity = 245;
    t->shadow_softness = 18;
    strcpy(t->iconset, "AMC-Filled");
    t->font_scale = 100;
    t->taskbar_pos = 0;
    t->taskbar_size = 44;
    t->taskbar_autohide = 0;
    t->anim_enabled = 1;
    t->anim_speed = 100;
    t->anim_easing = 2; /* spring */
    strcpy(t->anim_open, "scale");
    strcpy(t->anim_close, "fade");
    t->auto_dark = 1;
    /* prosedürel duvar kağıdı: gece mavisi degrade + yıldız */
    t->wall.is_generated = 1;
    t->wall.gen_c1 = rgb(0x10,0x10,0x20);
    t->wall.gen_c2 = rgb(0x31,0x32,0x44);
    t->wall.gen_angle = 90;
    t->wall.gen_pattern = 2;
    strcpy(t->wall.path, "(generated)");
}

int theme_count(void) { return (int)N_BUILTINS; }
const char *theme_name_at(int i) { return BUILTINS[i].name; }

amc_theme theme_load_named(const char *name) {
    amc_theme t; memset(&t, 0, sizeof t);
    for (unsigned i = 0; i < N_BUILTINS; i++)
        if (!strcmp(name, BUILTINS[i].name)) { t = BUILTINS[i]; break; }
    if (!*t.name) strcpy(t.name, "AmcNight"), t = BUILTINS[0];
    theme_defaults(&t);
    /* renk temalı duvar kağıdı: accent ile uyumlu degrade */
    t.wall.gen_c1 = mix(t.bg, rgb(0,0,0), 40);
    t.wall.gen_c2 = mix(t.surface, t.accent, 32);
    return t;
}

/* otomatik: saat 19-07 arası koyu */
amc_theme theme_load_auto(void) {
    /* gerçek OS'ta RTC'den okunur; burada sabit saat akışı simüle */
    extern int sim_hour_of_day(void);
    int h = sim_hour_of_day();
    int dark = (h >= 19 || h < 7);
    return theme_load_named(dark ? "AmcNight" : "AmcDay");
}

/* ---------- .amcthem dosya formatı (basit key=value) ----------
 * tema kaydet/yükle: ~/.config/amc/theme.amcthem
 * satır örnekleri:
 *   name=MaviRuya
 *   accent=#4cc2ff
 *   corner=16
 *   opacity=230
 *   anim=open:genie,ease:spring,speed:120
 *   wall=image:/home/amc/resim.png   VEYA   wall=gen:#0b1026,#2a4d8f,120,stars
 *   deco=circles|lines|glyphs|img:kapat.png,min.png,max.png
 *   taskbar=bottom,48,autohide
 *   icons=AMC-Line
 *   font=110
 */
static argb_t parse_hex(const char *s) {
    unsigned v = 0; sscanf(s, "#%x", &v); return (argb_t)v | 0xFF000000u;
}

int theme_save(const amc_theme *t, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "name=%s\n", t->name);
    fprintf(f, "bg=#%06x\nfg=#%06x\naccent=#%06x\nsurface=#%06x\n",
            t->bg & 0xFFFFFF, t->fg & 0xFFFFFF, t->accent & 0xFFFFFF, t->surface & 0xFFFFFF);
    fprintf(f, "topbar=#%06x\ntaskbar=#%06x\nsel=#%06x\n",
            t->topbar & 0xFFFFFF, t->taskbar & 0xFFFFFF, t->selection & 0xFFFFFF);
    if (t->wall.is_generated)
        fprintf(f, "wall=gen:#%06x,#%06x,%d,%d\n",
                t->wall.gen_c1 & 0xFFFFFF, t->wall.gen_c2 & 0xFFFFFF,
                t->wall.gen_angle, t->wall.gen_pattern);
    else
        fprintf(f, "wall=image:%s\n", t->wall.path);
    fprintf(f, "deco=%d\ncorner=%d\ntitlebar=%d\nopacity=%d\nshadow=%d\n",
            t->deco, t->corner_radius, t->titlebar_height, t->opacity, t->shadow_softness);
    fprintf(f, "icons=%s\nfont=%d\n", t->iconset, t->font_scale);
    fprintf(f, "taskbar=%d,%d,%d\n", t->taskbar_pos, t->taskbar_size, t->taskbar_autohide);
    fprintf(f, "anim=%d,%d,%d,%s,%s\n", t->anim_enabled, t->anim_speed,
            t->anim_easing, t->anim_open, t->anim_close);
    fclose(f);
    return 0;
}

int theme_load_file(amc_theme *t, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "name")) snprintf(t->name, 32, "%s", eq);
        else if (!strcmp(line, "bg")) t->bg = parse_hex(eq);
        else if (!strcmp(line, "fg")) t->fg = parse_hex(eq);
        else if (!strcmp(line, "accent")) t->accent = parse_hex(eq);
        else if (!strcmp(line, "surface")) t->surface = parse_hex(eq);
        else if (!strcmp(line, "topbar")) t->topbar = parse_hex(eq);
        else if (!strcmp(line, "taskbar") && strchr(eq, '#')) t->taskbar = parse_hex(eq);
        else if (!strcmp(line, "sel")) t->selection = parse_hex(eq);
        else if (!strcmp(line, "corner")) t->corner_radius = atoi(eq);
        else if (!strcmp(line, "titlebar")) t->titlebar_height = atoi(eq);
        else if (!strcmp(line, "opacity")) t->opacity = atoi(eq);
        else if (!strcmp(line, "shadow")) t->shadow_softness = atoi(eq);
        else if (!strcmp(line, "font")) t->font_scale = atoi(eq);
        else if (!strcmp(line, "icons")) snprintf(t->iconset, 32, "%s", eq);
        else if (!strcmp(line, "deco")) t->deco = (win_deco_style)atoi(eq);
        else if (!strcmp(line, "wall")) {
            if (!strncmp(eq, "image:", 6)) {
                t->wall.is_generated = 0;
                snprintf(t->wall.path, 64, "%s", eq + 6);
            } else if (!strncmp(eq, "gen:", 4)) {
                t->wall.is_generated = 1;
                char c1[16], c2[16]; int ang = 90, pat = 0;
                sscanf(eq + 4, "%15[^,],%15[^,],%d,%d", c1, c2, &ang, &pat);
                t->wall.gen_c1 = parse_hex(c1);
                t->wall.gen_c2 = parse_hex(c2);
                t->wall.gen_angle = ang; t->wall.gen_pattern = pat;
            }
        }
        else if (!strcmp(line, "taskbar") && !strchr(eq, '#'))
            sscanf(eq, "%d,%d,%d", &t->taskbar_pos, &t->taskbar_size, &t->taskbar_autohide);
        else if (!strcmp(line, "anim"))
            sscanf(eq, "%d,%d,%d,%23[^,],%23[^\n]", &t->anim_enabled, &t->anim_speed,
                   &t->anim_easing, t->anim_open, t->anim_close);
    }
    fclose(f);
    return 0;
}

/* amctheme komut satırı aracının mantığı (Ayarlar app'si de çağırır):
 *   amctheme list
 *   amctheme use Kiraz
 *   amctheme set accent #ff5555
 *   amctheme set corner 16
 *   amctheme wall /yol/resim.png
 *   amctheme save mytema ; amctheme load mytema
 */
int amctheme_cli(int argc, char **argv, amc_theme *live) {
    if (argc < 2) { printf("kullanim: amctheme list|use|set|wall|save|load\n"); return 1; }
    if (!strcmp(argv[1], "list")) {
        for (unsigned i = 0; i < N_BUILTINS; i++)
            printf("%2u %-10s accent=#%06x\n", i, BUILTINS[i].name, BUILTINS[i].accent & 0xFFFFFF);
        return 0;
    }
    if (!strcmp(argv[1], "use") && argc >= 3) { *live = theme_load_named(argv[2]); return 0; }
    if (!strcmp(argv[1], "set") && argc >= 4) {
        const char *k = argv[2], *v = argv[3];
        if (!strcmp(k, "accent")) live->accent = parse_hex(v);
        else if (!strcmp(k, "bg")) live->bg = parse_hex(v);
        else if (!strcmp(k, "corner")) live->corner_radius = atoi(v);
        else if (!strcmp(k, "opacity")) live->opacity = atoi(v);
        else if (!strcmp(k, "font")) live->font_scale = atoi(v);
        else if (!strcmp(k, "anim_speed")) live->anim_speed = atoi(v);
        else if (!strcmp(k, "deco")) live->deco = (win_deco_style)atoi(v);
        else { printf("bilinmeyen anahtar: %s\n", k); return 2; }
        return 0;
    }
    if (!strcmp(argv[1], "wall") && argc >= 3) {
        live->wall.is_generated = 0;
        snprintf(live->wall.path, 64, "%s", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "save") && argc >= 3) {
        char p[96]; snprintf(p, sizeof p, "/home/amc/.config/amc/%s.amcthem", argv[2]);
        snprintf(live->name, 32, "%s", argv[2]);
        return theme_save(live, p);
    }
    if (!strcmp(argv[1], "load") && argc >= 3) {
        char p[96]; snprintf(p, sizeof p, "/home/amc/.config/amc/%s.amcthem", argv[2]);
        return theme_load_file(live, p);
    }
    return 1;
}
#endif /* AMC_THEME_C */
