/* ============================================================
 * AMC OS — Pencere Animasyon Motoru (v0.6)
 * compositor'da çalışır; her pencere için animasyon durumu.
 * Efektler: scale, fade, genie (Mac tarzı büzülme), slide, bounce
 * Easing:   linear, ease-out cubic, spring (yaylı), bounce (zıplama)
 * FPS hedefi: 120 (VSync'li); hız temasından gelir (anim_speed %).
 * ============================================================ */
#ifndef AMC_ANIM_C
#define AMC_ANIM_C

#include <stdint.h>
#include <string.h>
#include <math.h>

typedef enum { FX_NONE=0, FX_SCALE, FX_FADE, FX_GENIE, FX_SLIDE, FX_BOUNCE } fx_kind;
typedef enum { EA_LINEAR=0, EA_EASEOUT, EA_SPRING, EA_BOUNCE } easing_kind;

static inline float clampf(float v, float a, float b){ return v<a?a:v>b?b:v; }

/* ---------- easing eğrileri: t∈[0,1] → p∈[0,~1] ---------- */
static float ease(easing_kind k, float t) {
    switch (k) {
    case EA_LINEAR:  return t;
    case EA_EASEOUT: { float u = 1.f - t; return 1.f - u*u*u; }          /* cubic out */
    case EA_SPRING: {                                                       /* sönümlü yay */
        float w = 8.5f, d = 0.72f;
        return 1.f - expf(-d*6.f*t) * cosf(w*t*3.14159f);
    }
    case EA_BOUNCE: {
        const float n1=7.5625f, d1=2.75f;
        if (t < 1/d1)      return n1*t*t;
        if (t < 2/d1)    { t-=1.5f/d1;  return n1*t*t+0.75f; }
        if (t < 2.5/d1)  { t-=2.25f/d1; return n1*t*t+0.9375f; }
                           t-=2.625f/d1; return n1*t*t+0.984375f;
    }}
    return t;
}

static fx_kind fx_parse(const char *s) {
    if (!strcmp(s,"scale"))  return FX_SCALE;
    if (!strcmp(s,"fade"))   return FX_FADE;
    if (!strcmp(s,"genie"))  return FX_GENIE;
    if (!strcmp(s,"slide"))  return FX_SLIDE;
    if (!strcmp(s,"bounce")) return FX_BOUNCE;
    return FX_NONE;
}

typedef struct {
    int active;
    fx_kind kind;
    easing_kind eas;
    int closing;            /* 1 = kapanış animasyonu */
    float t;                /* 0..1 ilerleme          */
    float dur_ms;           /* tema hızına bölünür    */
    float elapsed;
    /* geometri başlangıç/hedef */
    float x0,y0,w0,h0, x1,y1,w1,h1;
    /* genie hedefi (görev çubuğu simgesi merkezi) */
    float gx, gy;
    /* çıktı: bu frame çizilecek transform */
    float out_scale_x, out_scale_y, out_alpha, out_dx, out_dy;
} win_anim;

static void anim_start(win_anim *a, fx_kind k, easing_kind e, int closing,
                       float x0,float y0,float w0,float h0,
                       float x1,float y1,float w1,float h1,
                       float base_dur_ms, int speed_pct)
{
    memset(a, 0, sizeof *a);
    a->active = (k != FX_NONE);
    a->kind = k; a->eas = e; a->closing = closing;
    a->x0=x0; a->y0=y0; a->w0=w0; a->h0=h0;
    a->x1=x1; a->y1=y1; a->w1=w1; a->h1=h1;
    a->dur_ms = base_dur_ms * (100.f / clampf((float)speed_pct, 50.f, 200.f));
    a->out_scale_x = a->out_scale_y = closing ? 1.f : 1.f;
    a->out_alpha = 1.f;
}

/* her frame çağrilir (dt_ms ile). Döner: 1 hâlâ aktif, 0 bitti. */
int anim_step(win_anim *a, float dt_ms) {
    if (!a->active) return 0;
    a->elapsed += dt_ms;
    float t = clampf(a->elapsed / a->dur_ms, 0.f, 1.f);
    float p = ease(a->eas, t);
    if (a->closing) p = 1.f - p; /* kapanışta tersine */

    switch (a->kind) {
    case FX_SCALE:
        a->out_scale_x = a->out_scale_y = 0.6f + 0.4f * p;
        a->out_alpha = 0.0f + 1.0f * clampf(p*1.4f, 0.f, 1.f);
        break;
    case FX_FADE:
        a->out_scale_x = a->out_scale_y = 0.96f + 0.04f * p;
        a->out_alpha = p;
        break;
    case FX_SLIDE:
        a->out_dx = (a->x1 - a->x0) * (1.f - p) * 0.0f; /* yerleşim ayrı */
        a->out_dy = (1.f - p) * 40.f;                    /* alttan gelir */
        a->out_scale_x = a->out_scale_y = 1.f;
        a->out_alpha = clampf(p*2.f, 0.f, 1.f);
        break;
    case FX_GENIE: {
        /* üst kenar sabit, alt kenar görev çubuğundaki simgeye doğru
         * büzülür; yatayda ortalanarak daralır (Bezier yakınsama).
         * Kapanışta p tersine çevrilmiş gelir → baştan küçük olmalı. */
        /* q: burusma orani — acilista 0→1 (buyur), kapanista 1→0 (kuculur).
         * Kapanis dongusu `p` zaten tersine cevrilmis geldigi icin (p=1-p),
         * closing'de q=p dogrudur: ilk frame'de q~0 → pencere simgeye
         * dogru aninda ezilmeye baslar (macOS genie davranisi). */
        float q = a->closing ? p : 1.f - p;           /* 1→0 burusma */
        float sx = 0.15f + 0.85f * q;                 /* yatay daralma */
        float sy = 0.05f + 0.95f * q;                 /* dikey ezilme  */
        a->out_scale_x = sx; a->out_scale_y = sy;
        /* dx/dy de q uzayindan hesaplanmali (p degil!) — aksi halde
         * kapanis animasyonunda kayma yonu terste kalir (v0.6 hatasi). */
        a->out_dx = (a->gx - (a->x0 + a->w0/2)) * (1.f - q) * 0.35f;
        a->out_dy = (a->gy - (a->y0 + a->h0)) * (1.f - q);
        a->out_alpha = 0.55f + 0.45f * q;             /* sona dogru hafif solar */
        break;
    }
    case FX_BOUNCE:
        a->out_scale_x = a->out_scale_y = 0.8f + 0.2f * p;
        a->out_dy = -fabsf(sinf(t * 3.14159f * 2.f)) * (1.f - t) * 24.f;
        a->out_alpha = clampf(t*3.f, 0.f, 1.f);
        break;
    default:
        a->out_scale_x = a->out_scale_y = 1.f; a->out_alpha = 1.f;
    }
    if (t >= 1.f) { a->active = 0; return 0; }
    return 1;
}

/* Compositor'a entegrasyon pseudo-kodu (gerçek OS'ta):
 *   for each window w with anim.active:
 *       mat3 transform = translate(x+dx, y+dy) * scale(sx, sy around pivot)
 *       layer_draw(w.surface, transform, alpha=out_alpha)
 *   if !anim_step(&w.anim, frame_dt): finalize_window_state(w)
 * Kapanışta: son frame'de surface serbest bırakılır.
 * Açılışta: pencere içeriği ilk frame'den önce bir kez render edilir
 * (ilk-açılış sihirini önlemek için offscreen buffer).
 */
#endif /* AMC_ANIM_C */
