/* ============================================================
 * AMC OS — Masaüstü Ortamı (basit + güzel hedefi)
 * apps/desktop/desktop.c  → "AMC Shell Desktop" oturum yöneticisi
 *
 * Görünüm felsefesi: sadelik. Ekran karışıklığı YOK:
 *   - Üstte ince bir durum çubuğu (saat, ses simgesi, pil)
 *   - Altta kayan görev çubuğu (başlat, açık pencereler)
 *   - Temiz duvar kâğıdı + masaüstü simgeleri (Medya Oynatıcı,
 *     Dosyalar, Terminal, Metin Editörü, Ayarlar)
 *
 * Teknik: kendi compositor'umuz (composd) ile GPU'da VSync'li
 *   çift tamponlu çizim; widget toolkit'i CPU'da yazılım render,
 *   GPU'ya dma-buf ile yüklenir. Tema: koyu/açık tek tuşla.
 * ============================================================ */

#include <amc/ui.h>
#include <amc/ipc.h>

#define TASKBAR_H    40
#define TOPBAR_H     26
#define ICON_SIZE    64

struct desktop_icon {
    const char *label; const char *app_path; int x, y;
};

static struct desktop_icon icons[] = {
    { "Medya Oynatic", "/opt/amc/bin/mediaplayer", 40,  60 },
    { "Dosyalar",      "/opt/amc/bin/filemanager", 40, 150 },
    { "Terminal",      "/opt/amc/bin/terminal",    40, 240 },
    { "Yazi Editor",   "/opt/amc/bin/texteditor",  40, 330 },
    { "Ayarlar",       "/opt/amc/bin/settings",    40, 420 },
};

/* ================== Duvar kâğıdı: yumuşak degrade ================== */

static void draw_wallpaper(fb_info *fb, const struct theme *t) {
    /* Dikey degrade: üst #1e1e2e -> alt #313244 (koyu tema varsayilani)
       Dithering ile bantlanma (banding) onlenir - kucuk ama sik detay */
    for (int y = 0; y < fb->height; y++) {
        float f = (float)y / fb->height;
        uint32_t c = lerp_color(t->wall_top, t->wall_bottom, f);
        hline(fb, 0, y, fb->width, c);
    }
}

/* ================== Üst durum çubuğu ================== */

static void draw_topbar(fb_info *fb, const struct theme *t) {
    rounded_rect(fb, 8, 4, fb->width-16, TOPBAR_H-8, 12, t->bar_bg);

    /* Saat ortada, buyuk ve okunakli */
    char clock[16];
    fmt_time(clock, sizeof(clock), "%H:%M");
    text_center(fb, fb->width/2, 6, clock, t->font_ui, t->text_primary);

    /* Sagda simgeler: ag, ses (kulaklik takili farkli ikon), pil */
    icon_net(fb, fb->width - 96, 8, net_is_online());
    icon_volume(fb, fb->width - 64, 8, audio_get_level());
    icon_battery(fb, fb->width - 32, 8, power_get_percent());
}

/* ================== Görev çubuğu ================== */

static void draw_taskbar(fb_info *fb, const struct theme *t) {
    int ty = fb->height - TASKBAR_H;
    rounded_rect(fb, 8, ty+4, fb->width-16, TASKBAR_H-8, 14, t->bar_bg);

    /* Baslat duggmesi: dokununca uygulama listesi acilir */
    circle_button(fb, 28, ty + TASKBAR_H/2, 14, t->accent, "A");

    /* Acik pencereler: simge + etiket, aktif olan vurgulu */
    int px = 60;
    for_each_window(w) {
        bool active = (w == wm_focused());
        rounded_rect(fb, px, ty+8, w->title_w + 40, TASKBAR_H-16, 8,
                     active ? t->accent : t->bar_fg);
        icon_from_app(fb, px+8, ty+10, 20, w->app_id);
        text(fb, px+32, ty+12, w->title, t->font_small,
             active ? white : t->text_secondary);
        px += w->title_w + 52;
    }
}

/* ================== Simge çizimi + çift tık ================== */

static void draw_icons(fb_info *fb, const struct theme *t) {
    for (unsigned i = 0; i < sizeof(icons)/sizeof(icons[0]); i++) {
        app_icon_rounded(fb, icons[i].x, icons[i].y, ICON_SIZE,
                         icons[i].label, t);
    }
}

static void on_double_click(int mx, int my) {
    for (unsigned i = 0; i < sizeof(icons)/sizeof(icons[0]); i++)
        if (inside(mx, my, icons[i].x, icons[i].y,
                   ICON_SIZE, ICON_SIZE + 20))
            spawn_async(icons[i].app_path);    /* uygulama baslat */
}

/* ================== Ana oturum döngüsü ================== */

int main(void) {
    session_login_screen_if_needed();          /* GDM benzeri giris */
    struct display d = display_open(1920, 1080, 60);
    struct theme th = theme_load_auto();       /* saat 19-07 arasi koyu */

    fb_info *back = d.back;                    /* cift tampon: tear yok */
    draw_wallpaper(back, &th);
    draw_icons(back, &th);

    event e;
    while (session_running()) {
        /* Arka plan servis olaylari: takvim guncelleme, jack degisimi */
        while (ipc_poll(SVC_SESSIOND, &e)) handle_session_event(&e, &th);

        draw_wallpaper(back, &th);
        draw_icons(back, &th);
        draw_topbar(back, &th);
        draw_taskbar(back, &th);
        wm_paint_windows(back);                /* pencereler en ustte */

        if (ui_poll_event(&e)) {
            switch (e.type) {
            case EV_MOUSE_DCLICK: on_double_click(e.mx, e.my); break;
            case EV_KEY: if (e.key == KEY_SUPER) toggle_launcher(); break;
            case EV_DRAG_ICON: move_icon(&e); break;  /* simge tasima */
            }
        }
        gpu_flip(d.gpu, back);                 /* VBlank'te sun */
    }
    return 0;
}
