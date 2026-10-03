/* ============================================================
 * AMC OS — Realtek HD Audio (HDA) sürücü ailesi
 * drivers/audio/realtek_hda.c
 *
 * Desteklenen codec'ler (PCI VID 0x10EC / HDA azalia uyumlu):
 *   ALC260, ALC269, ALC270, ALC272, ALC280, ALC282, ALC283,
 *   ALC269VB, ALC292, ALC3204, ALC3226, ALC3227, ALC662, ALC663,
 *   ALC665, ALC668, ALC671, ALC768, ALC880, ALC882, ALC883,
 *   ALC885, ALC887, ALC888, ALC889, ALC892, ALC898, ALC1150,
 *   ALC1200, ALC1220, ALC1220-VB, ALC4080, ALC4090...
 *
 * Mimari:
 *   - HDA denetleyici (Intel ICH / AMD SB) BAR0 MMIO register seti
 *   - CORB/RIRB komut kuyruğu ile codec konuşması (verb/response)
 *   - BDL (Buffer Descriptor List) DMA ile PCM oynatma/kaydetme
 *   - Pin kontrol + Volume Knob (V-Knob) widget graph yapılandırması
 *   - jack sense (kapalı algılama) için unsolicited responses
 * ============================================================ */

#include "realtek_hda.h"
#include "../../kernel.h"

#define HDA_MAX_CODECS      15
#define HDA_VERB_TIMEOUT_US 100000

struct hda_controller {
    volatile uint32_t *mmio;        /* BAR0 */
    uint32_t corr_pos;              /* CORB read pointer */
    uint32_t resp_pos;              /* RIRB write pointer */
    uint32_t stream_dma[16];        /* 16 ses akışı (8 out + 8 in) */
    struct hda_codec codecs[HDA_MAX_CODECS];
    int codec_count;
};

static struct hda_controller g_hda;

/* ---- Register ofsetleri (HDA spec 1.0a, §2.3) ---- */
enum hda_reg {
    REG_GCTL   = 0x00,  /* Global Control            */
    REG_ICAT   = 0x04,  /* Interrupt Control         */
    REG_IRSS   = 0x08,  /* Interrupt Status          */
    REG_VSTATE = 0x0C,  /* Verb State                */
    REG_OSTATE = 0x10,  /* Output State              */
    REG_CSTS   = 0x14,  /* Controller Status         */
    REG_CORB   = 0x20,  /* CORB base                 */
    REG_CORBRP = 0x28,  /* CORB read pointer         */
    REG_CORBWP = 0x2C,  /* CORB write pointer        */
    REG_RIRB   = 0x30,  /* RIRB base                 */
    REG_RIRBW  = 0x3C,  /* RIRB write pointer        */
    REG_RIRBRP = 0x40,  /* RIRB read pointer         */
    REG_CTL    = 0x48,  /* Controller Capabilities   */
};

/* ================== Temel verb/response mekanizması ================== */

/* Codec'e komut gönder ve yanıtı bekle (senkron) */
static uint32_t hda_send_cmd(struct hda_codec *c, uint32_t verb) {
    volatile uint32_t *corr = g_hda.mmio + (REG_CORB_DATA(c->addr) / 4);
    *corr = verb;
    g_hda.corr_pos++;
    writel(REG_CORBRP, g_hda.corr_pos & 0xFF);

    /* Yanıt gelene kadar bekle (timeout korumalı) */
    for (int us = 0; us < HDA_VERB_TIMEOUT_US; us++) {
        if ((readl(REG_VSTATE) & HDA_VSTATE_CPE)) break; /* busy */
        udelay(1);
    }
    volatile uint32_t *resp = g_hda.mmio + (REG_RIRB_DATA(c->addr) / 4);
    return *resp;
}

/* Node'a basit kontrol yaz (Set Amplitude / Pin Ctrl vb.) */
static void hda_codec_write(struct hda_codec *c, uint16_t nid,
                            uint8_t cad, uint32_t verb, uint32_t param) {
    hda_send_cmd(c, (nid << 20) | (cad << 16) | verb | param);
}

/* ================== Codec keşfi ve widget grafiği ================== */

static int hda_probe_codecs(void) {
    uint32_t flen = readl(REG_CTL) >> 24; /* Function Link Mask alt alanları */
    g_hda.codec_count = 0;
    for (uint8_t cid = 0; cid < HDA_MAX_CODECS; cid++) {
        if (!(flen & (1u << cid))) continue;
        uint32_t resp = hda_send_cmd(&(struct hda_codec){.addr=cid},
                                     AC_GET_VENDOR_ID);
        uint16_t vendor = resp & 0xFFFF;
        uint16_t devid  = (resp >> 16) & 0xFFFF;
        if (vendor == 0 || vendor == 0xFFFF) continue;

        struct hda_codec *c = &g_hda.codecs[g_hda.codec_count++];
        c->addr = cid; c->vendor = vendor; c->devid = devid;

        /* Realtek mi? */
        c->is_realtek = (vendor == HDA_VENDOR_REALTEK);
        kprintf("[hda] codec %u: vendor=0x%04x dev=0x%04x (%s)\n",
                cid, vendor, devid,
                c->is_realtek ? realtek_name(devid) : "generic");
    }
    return g_hda.codec_count;
}

/* Widget grafiğini tara: audio output mixer -> speaker/headphone pinleri */
static void hda_parse_graph(struct hda_codec *c) {
    uint8_t start = c->devid & 0x7F; /* ilk node genelde AFG'dir */
    for (uint16_t nid = start; nid < start + 0x40; nid++) {
        uint32_t wcap = hda_codec_read(c, nid, AC_GET_WCAP);
        uint8_t type = (wcap >> 20) & 0xF;
        switch (type) {
        case AC_WID_AUD_OUT:
            c->dac_nids[c->num_dacs++] = nid; break;
        case AC_WID_AUD_MIX:
            c->mixer_nids[c->num_mixers++] = nid; break;
        case AC_WID_PIN:
            if (wcap & AC_PINCAP_POUT) c->speaker_pins[c->num_spk++] = nid;
            if (wcap & AC_PINCAP_HP_OUT) c->hp_pins[c->num_hp++] = nid;
            if (wcap & AC_PINCAP_IN)     c->mic_pins[c->num_mic++] = nid;
            break;
        }
    }
}

/* ================== Ses açma/kapama + volüm ================== */

void hda_set_volume(uint8_t percent) {
    if (percent > 100) percent = 100;
    /* 0..100% -> 0dB..-50dB adım hesabı (ALC ailesi 0.5dB adımlar) */
    uint32_t amp = ((100 - percent) * 100 / 50) << 8; /* mute bit yok */
    for (int i = 0; i < g_hda.codec_count; i++) {
        struct hda_codec *c = &g_hda.codecs[i];
        for (int j = 0; j < c->num_dacs; j++)
            hda_codec_write(c, c->dac_nids[j], 0, AC_SET_AMP_GAIN_MUTE, amp);
        for (int j = 0; j < c->num_spk; j++)
            hda_codec_write(c, c->speaker_pins[j], 0, AC_SET_AMP_GAIN_MUTE, amp);
    }
}

void hda_pin_enable(struct hda_codec *c, uint16_t pin, bool out, bool sense) {
    uint32_t ctrl = (out ? AC_PINCTRL_OUT_EN : 0)
                  | (sense ? AC_PINCTRL_SENSE_EN : 0)
                  | AC_PINCTRL_ASSOC_DEFAULT;
    hda_codec_write(c, pin, 0, AC_SET_PIN_CONTROL, ctrl);
}

/* ================== PCM oynatma (BDL DMA zinciri) ================== */

struct hda_stream {
    volatile uint32_t *sdctl;   /* Stream Control/Status register */
    uint64_t bdl_phys;          /* Buffer Descriptor List fiziksel */
    void    *buf_virt;          /* DMA halka tamponu               */
    size_t   period_bytes;
    bool     active;
};

static struct hda_stream g_play_stream;

int hda_pcm_open(int rate, int channels, int fmt_bits) {
    /* Örnek: 48kHz stereo S16LE => 48000*2*2 = 192000 B/s */
    g_play_stream.period_bytes = (rate * channels * (fmt_bits/8)) / 50; /* 20ms */
    g_play_stream.buf_virt = dma_alloc_coherent(g_play_stream.period_bytes * 8,
                                                &g_play_stream.bdl_phys);
    /* Format kodlaması: HDA FORMAT bitleri (PCM, S16/S24/32, rate bits) */
    writel(STREAM_FMT(0), hda_encode_fmt(rate, channels, fmt_bits));
    writel(STREAM_CCB(0), 0); /* link count = 1 stream */
    return 0;
}

ssize_t hda_pcm_write(const void *data, size_t len) {
    /* Ring buffer'a kopyala + BDL ilerlet; kesmeyle tüketim bildirimi */
    size_t copied = ring_copy_in(&g_play_stream, data, len);
    if (!g_play_stream.active) {
        sd_set_run(&g_play_stream);   /* BDV|RUN bitleri */
        g_play_stream.active = true;
    }
    return copied;
}

/* Kaydetme (mikrofon) aynı çerçevenin tersi: BDL okuma yönü */
ssize_t hda_pcm_read(void *data, size_t len) {
    return ring_copy_out(&g_rec_stream, data, len);
}

/* ================== Jack sense (kulaklık takma/çıkarma) ================== */

static void hda_jack_event(uint16_t pin, bool present) {
    kprintf("[hda] jack %s: pin=0x%x durum=%s\n",
            present ? "TAKILI" : "CIKARILDI", pin,
            present ? "present" : "absent");
    /* Kullanıcı alanına IPC mesajı: ses yönlendirmesi otomatik değişsin */
    ipc_post_msg(SVC_AUDIOD, AUDIO_EVT_JACK, pin, present);
}

/* Unsolicited response işleyicisi (RIRB kesmesi) */
void hda_irq_unsol(uint32_t resp) {
    uint16_t tag = resp & 0xFFFF;
    uint8_t  nid = (resp >> 16) & 0x7F;
    uint8_t  seq = (tag >> 8) & 0x3F;
    if (seq == HDA_UNSOL_JACK) {
        bool present = !(tag & 0x80); /* bit 7: presence detect */
        for (int i = 0; i < g_hda.codec_count; i++)
            for (int j = 0; j < g_hda.codecs[i].num_hp; j++)
                if (g_hda.codecs[i].hp_pins[j] == nid)
                    hda_jack_event(nid, present);
    }
}

/* ================== Modül kayıt / başlatma ================== */

static int realtek_hda_init(struct pci_dev *dev) {
    g_hda.mmio = ioremap(dev->bar[0], 0x4000);

    /* Reset: GCTL.SRST toggle */
    writel(REG_GCTL, 0);
    mdelay(1);
    writel(REG_GCTL, HDA_GCTL_RESET);
    while (!(readl(REG_CSTS) & HDA_CSTS_RDY)) mdelay(1);

    hda_probe_codecs();
    for (int i = 0; i < g_hda.codec_count; i++)
        hda_parse_graph(&g_hda.codecs[i]);

    /* Varsayılan: hoparlör + kulaklık çıkış, mikrofon giriş, %70 ses */
    for (int i = 0; i < g_hda.codec_count; i++) {
        struct hda_codec *c = &g_hda.codecs[i];
        for (int j = 0; j < c->num_spk; j++)
            hda_pin_enable(c, c->speaker_pins[j], true, false);
        for (int j = 0; j < c->num_hp; j++)
            hda_pin_enable(c, c->hp_pins[j], true, true);
        for (int j = 0; j < c->num_mic; j++)
            hda_pin_enable(c, c->mic_pins[j], false, true);
    }
    hda_set_volume(70);

    /* MSI kesmesi kur */
    pci_enable_msi(dev, hda_irq_handler);
    writel(REG_ICAT, HDA_ICASE_IE);
    kprintf("[hda] Realtek HDA hazir (%d codec)\n", g_hda.codec_count);
    return 0;
}

/* PCI ID tablosu — tüm bilinen Realtek HDA controller/device çiftleri */
static const struct pci_device_id hda_ids[] = {
    { HDA_VENDOR_REALTEK, 0x0288, .driver_data = ALC269 },
    { HDA_VENDOR_REALTEK, 0x0298, .driver_data = ALC280 },
    { HDA_VENDOR_REALTEK, 0x0292, .driver_data = ALC292 },
    { HDA_VENDOR_REALTEK, 0x0668, .driver_data = ALC668 },
    { HDA_VENDOR_REALTEK, 0x0892, .driver_data = ALC892 },
    { HDA_VENDOR_REALTEK, 0x0899, .driver_data = ALC898 },
    { HDA_VENDOR_REALTEK, 0x0A0C, .driver_data = ALC887 },
    { HDA_VENDOR_REALTEK, 0x0C0C, .driver_data = ALC1150 },
    { HDA_VENDOR_REALTEK, 0x1220, .driver_data = ALC1220 },
    { HDA_VENDOR_REALTEK, 0x0408, .driver_data = ALC4080 },
    { HDA_VENDOR_REALTEK, 0x0409, .driver_data = ALC4090 },
    { HDA_VENDOR_INTEL,   0x8D20, .class_mask = CLS_HDA }, /* Azalia köprüsü */
    { HDA_VENDOR_AMD,     0x437B, .class_mask = CLS_HDA },
    {}
};

DEFINE_PCI_DRIVER(realtek_hda, hda_ids, realtek_hda_init,
                  MODULE_LICENSE("AMC-BSD"),
                  MODULE_AUTHOR("AMC OS Project"),
                  MODULE_DESCRIPTION("Realtek HD Audio family driver"));
