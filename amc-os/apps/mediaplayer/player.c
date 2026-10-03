/* ============================================================
 * AMC OS — Medya Oynatıcı (Media Player) — kullanıcı alanı uygulaması
 * apps/mediaplayer/player.c
 *
 * Özellikler:
 *   - Ses: MP3, AAC, FLAC, OGG/Vorbis, WAV  → audiod (HDA/USB ses)
 *   - Video: H.264, VP8/VP9, AV1 (yazılımlı + GPU hızlandırmalı VA-API)
 *   - Altyazı (SRT/VTT), oynatma listesi, eşitleyici (EQ)
 *   - Compositor'a dma-buf ile sıfır-kopya kare teslimi
 *
 * Çekirdeğe dokunmaz; yalnızca syscall + IPC kullanır:
 *   open/read → dosya; ioctl(AUDIO_WRITE); ipc_call(composd, FLIP)
 * ============================================================ */

#include <amc/syscall.h>
#include <amc/ipc.h>
#include <amc/audio.h>
#include <amc/video.h>
#include <stdlib.h>
#include <string.h>

struct playlist {
    char paths[512][PATH_MAX];
    int  count, current;
    bool shuffle, repeat_one;
};

struct player_state {
    enum { ST_STOPPED, ST_PLAYING, ST_PAUSED } state;
    int   volume;            /* 0..100 */
    long  position_ms, duration_ms;
    struct playlist pl;
};

static struct player_state ps = { .volume = 70 };

/* ================== Demux + Decode hattı ================== */

/* Dosyadan paket oku: konteyner (MP4/MKV/OGG) ayrıştırıcısı */
static int demux_next_packet(struct media_file *mf, struct av_packet *pkt) {
    if (mf->buf_pos >= mf->buf_len) {
        ssize_t n = read(mf->fd, mf->buf, sizeof(mf->buf));
        if (n <= 0) return -1;      /* EOF */
        mf->buf_len = n; mf->buf_pos = 0;
    }
    return container_parse_frame(mf, pkt);   /* başlık + payload işaretçisi */
}

/* Ses paketi çöz → PCM → audiod'a gönder */
static void audio_decode_loop(struct media_file *mf) {
    struct av_packet pkt;
    static int16_t pcm[48000 / 25 * 2];       /* 40ms stereo */

    while (demux_next_packet(mf, &pkt) == PKT_AUDIO && ps.state != ST_STOPPED) {
        if (ps.state == ST_PAUSED) { sleep_ms(20); continue; }

        int samples = 0;
        switch (mf->audio_codec) {
        case CODEC_MP3:   mp3_decode_frame(&pkt, pcm, &samples);  break;
        case CODEC_AAC:   aac_decode_frame(&pkt, pcm, &samples);  break;
        case CODEC_FLAC:  flac_decode_frame(&pkt, pcm, &samples); break;
        case CODEC_VORBIS:vorbis_decode_frame(&pkt, pcm, &samples);break;
        case CODEC_PCM:   memcpy(pcm, pkt.data, pkt.size);
                          samples = pkt.size / 4; break;
        }
        apply_eq(pcm, samples);              /* 10 bant yazılım EQ */

        struct audio_write_req req = { .data = pcm, .bytes = samples * 4 };
        ioctl(mf->audio_fd, AUDIO_WRITE, &req);   /* çekirdek HDA DMA'ya yazar */
        ps.position_ms += samples * 1000 / 48000;
    }
}

/* Video paketi çöz → YUV → GPU scale/YUV→RGB → compositor flip */
static void video_decode_loop(struct media_file *mf) {
    struct av_packet pkt;
    uint32_t bo_handle;             /* GPU buffer (GEM/dma-buf) */

    vaapi_context *hw = vaapi_open();      /* donanım hızlandırma dener */
    while (demux_next_packet(mf, &pkt) == PKT_VIDEO && ps.state != ST_STOPPED) {
        struct frame f;
        if (hw && !vaapi_decode(hw, &pkt, &f))
            ; /* GPU çözdü */
        else
            sw_decode_frame(&pkt, &f);     /* CPU yedek yol */

        gpu_map_frame(mf->gpu_fd, &f, &bo_handle);
        /* Compositor'a "bu karenin VBlank'te ekrana çıkması" emri */
        struct comp_flip_msg m = { .bo = bo_handle, .pts_us = f.pts_us };
        ipc_send(SVC_COMPOSITOR, COMP_MSG_FLIP, &m, sizeof(m));
        wait_vblank(mf->gpu_fd);           /* tear-free: vblank senkron */
    }
}

/* ================== Oynatma komutları (UI'den gelir) ================== */

void player_play(const char *path) {
    struct media_file *mf = media_open(path);
    if (!mf) { ui_error("Dosya acilamadi: %s", path); return; }

    mf->audio_fd = open("/dev/audio0", O_WRONLY);
    ioctl(mf->audio_fd, AUDIO_SET_FORMAT,
          (struct audio_format){ 48000, 2, FMT_S16LE });

    thread_create(audio_decode_loop, mf);  /* iş parçacıkları paralel */
    if (mf->has_video) thread_create(video_decode_loop, mf);
    ps.state = ST_PLAYING;
}

void player_next(void) {
    if (++ps.pl.current >= ps.pl.count)
        ps.pl.current = ps.pl.repeat_one ? ps.pl.current : 0;
    player_play(ps.pl.paths[ps.pl.current]);
}

void player_set_volume(int v) {
    ps.volume = CLAMP(v, 0, 100);
    ipc_call_rpc(SVC_AUDIOD, AUDIO_RPC_VOLUME, &ps.volume, sizeof(int),
                 NULL, 0, 100 /*timeout ms*/);
}

/* ================== Ana (UI döngüsü) ================== */

int main(int argc, char **argv) {
    ui_init("AMC Media Player");            /* pencere + widget toolkit */
    for (int i = 1; i < argc; i++)
        strcpy(ps.pl.paths[ps.pl.count++], argv[i]);

    event e;
    while (ui_poll_event(&e)) {             /* fare/klavye/IPC olayları */
        switch (e.type) {
        case EV_KEY:
            if (e.key == KEY_SPACE) ps.state = (ps.state == ST_PLAYING)
                                             ? ST_PAUSED : ST_PLAYING;
            if (e.key == KEY_N)     player_next();
            if (e.key == KEY_UP)    player_set_volume(ps.volume + 5);
            if (e.key == KEY_DOWN)  player_set_volume(ps.volume - 5);
            break;
        case EV_IPC:  handle_ipc_event(&e); break;  /* jack çıkarıldı vb. */
        case EV_CLOSE: goto shutdown;
        }
    }
shutdown:
    ps.state = ST_STOPPED;
    media_close_all();
    return 0;
}
