/* ============================================================
 * AMC OS v0.6 — Kisisellestirme & Asistan sim entegrasyon testleri
 * Gercek kod yollari: theme.c (kaydet/yukle/CLI), animations.c
 * (easing/efekt adimlama), amc_ai.c (niyet→cevap→aksiyon).
 * Desktop render'in baslik-cubugu simulasyonu burada yapilir.
 * ============================================================ */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* surum bayragi: sim derlemesinde posix_compat yerine standart header */
#include "../apps/desktop/theme.c"
#include "../apps/desktop/animations.c"
#include "../apps/assistant/amc_ai.c"

int sim_hour_of_day(void) { return 14; } /* gunduz simule */

static int fails = 0;
#define CHECK(...) do { if (!(VA_ARGS_0 __VA_ARGS__)) { } } while(0)
/* basit varyadic: ilk arguman kosul, kalani format */
#define CHECK(cond, ...) do { int _c=(cond); printf(_c?"  ok  : ":"  FAIL: "); printf(__VA_ARGS__); printf("\n"); if(!_c) fails++; } while (0)

void personalization_sim_test(void) {
    printf("\n[personalization] tema motoru:\n");
    amc_theme t = theme_load_named("Kiraz");
    CHECK(!strcmp(t.name,"Kiraz"), "Kiraz temasi yuklendi");
    CHECK(t.accent == rgb(0xff,0x5c,0x8a), "accent rengi dogru");
    CHECK(theme_count() == 12, "12 hazir tema mevcut");

    /* CLI: renk degistir + kaydet + geri yukle */
    char *av[] = {"amctheme","set","accent","#ff5555"};
    CHECK(amctheme_cli(4, av, &t)==0 && t.accent==0xFFFF5555u, "amctheme set accent #ff5555");
    CHECK(theme_save(&t, "/tmp/kiraz.amcthem")==0, "tema .amcthem dosyasina kaydedildi");
    amc_theme t2; memset(&t2,0,sizeof t2); theme_defaults(&t2);
    CHECK(theme_load_file(&t2, "/tmp/kiraz.amcthem")==0, "tema dosyadan geri yuklendi");
    CHECK(t2.accent==t.accent && t2.corner_radius==t.corner_radius, "yuvarlanma+renk korundu");

    char *aw[] = {"amctheme","wall","/home/amc/resim.png"};
    CHECK(amctheme_cli(3, aw, &t)==0 && !t.wall.is_generated, "duvar kagidi resmi ayarlandi");

    printf("[personalization] animasyon motoru:\n");
    win_anim a;
    anim_start(&a, FX_SCALE, EA_SPRING, 0, 100,100,600,400, 100,100,600,400, 320, t.anim_speed);
    int frames=0; while (anim_step(&a, 16.f) && frames<40) frames++;
    /* NOT: onceki surumde asagidaki dongu `frames++` unutuldugu icin
       sonsuz donuyordu (v0.6 takilma hatasi). Duzeltildi + ust sinir sigortasi. */
    CHECK(!a.active && fabsf(a.out_scale_x-1.f)<0.02f && a.out_alpha>0.99f,
          "scale+spring ~%d frame'de tamamlandi (1.0 olcek, opak)", frames);
    anim_start(&a, FX_GENIE, EA_EASEOUT, 1, 200,80,800,500, 200,80,800,500, 420, 100);
    a.gx = 300; a.gy = 1010; /* gorev cubugu simgesi */
    float first_sy = -1; frames=0;
    float last_sy = -1;
    while (anim_step(&a, 16.f)) { if (first_sy<0) first_sy=a.out_scale_y; last_sy=a.out_scale_y; frames++; }
    CHECK(last_sy < 0.15f, "genie son frame'de simge boyuna iniyor");
    CHECK(first_sy < 0.3f, "genie kapanisi ilk frame'de dikeyde buzuyor");
    CHECK(frames>=20 && frames<=40, "genie suresi ~%d frame (hiz:%%100)", frames);

    printf("[personalization] AMC-AI yerel asistan:\n");
    ai_reply r;
    amc_ai_query("temayi kiraz yapar misin?", &r);
    CHECK(r.intent==INT_THEME && !strcmp(r.action,"amctheme use Kiraz"),
          "'temayi kiraz yap' → eylem: amctheme use Kiraz");
    amc_ai_query("pencerelerin animasyonunu nasil degistiririm?", &r);
    CHECK(strstr(r.answer,"genie")!=NULL, "animasyon nasil-yapilir cevabi efekte deginiyor");
    amc_ai_query("firefox nasil kurulur?", &r);
    CHECK(r.intent==INT_HOWTO && strstr(r.answer,"amcdb install")!=NULL,
          "RAG: dokumandan 'amcdb install firefox' alintilandi");
    amc_ai_query("terminali ac", &r);
    CHECK(!strcmp(r.action,"sessiond launch terminal"), "'terminali ac' → launch eylemi");
    amc_ai_query("selam", &r);
    CHECK(r.intent==INT_SMALLTALK, "smalltalk niyeti");
    amc_ai_init();

    printf("[personalization] pencere dekorasyonu sim-renderi:\n");
    /* 240x160 sahte yuzeyde baslik cubugu + buton stilleri cizilmeli gibi dogrula */
    for (int d=0; d<=3; d++) {
        const char *names[]={"circles","lines","glyphs","custom-img"};
        t.deco = (win_deco_style)d;
        /* deco alani .amcthem'e yazilip geri okunabiliyor mu? */
        theme_save(&t, "/tmp/deco.amcthem");
        amc_theme c; memset(&c,0,sizeof c); theme_defaults(&c);
        theme_load_file(&c, "/tmp/deco.amcthem");
        CHECK(c.deco==d, names[d]);
    }
    printf("[personalization] %s (%d hata)\n", fails?"BASARISIZ":"TAMAM", fails);
}
