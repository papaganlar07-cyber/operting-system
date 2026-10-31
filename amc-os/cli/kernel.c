/* ============================================================
 * amc-os  -  kernel.c  (Guncellenmis tam hali)
 * Adim 4: Klavye (IRQ1) + Basit Komut Satiri (CLI) altyapisi
 * Freestanding C - hicbir standart kutuphane (<stdio.h> YOK)
 * Derleme: gcc -ffreestanding -m32 -nostdlib -c kernel.c
 * ============================================================ */

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

/* ---------- I/O port erisimleri (inline assembly) ---------- */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* ---------- VGA text mode bellegi (0xB8000) ---------- */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((volatile uint16_t *)0xB8000)

/* Renk: on plan / arka plan. 0x1F = mavi zemin uzerine parlak beyaz */
#define COL_DEFAULT 0x1F
#define COL_PROMPT  0x1A   /* yesil prompt */
#define COL_ERR     0x1C   /* kirmizi hata */

static volatile uint16_t *vga = VGA_MEM;
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;

/* Ekrani bosluk (space) ile temizle */
static void screen_clear(void)
{
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = (uint16_t)(COL_DEFAULT << 8) | ' ';
    cursor_x = 0;
    cursor_y = 0;
}

/* Tek karakter yaz, satir sonu / ekran sonu takibini yap */
static void putch(char c, uint8_t color)
{
    if (c == '\n' || cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
        if (c != '\n') { /* kelime kaymasi yerine yeni satira gec */ }
    }
    if (c == '\n') { cursor_x = 0; }
    else {
        if (cursor_y >= VGA_HEIGHT) {
            /* Ekran dolustu: bir satir yukari kaydir (scroll) */
            for (uint32_t y = 1; y < VGA_HEIGHT; y++)
                for (uint32_t x = 0; x < VGA_WIDTH; x++)
                    vga[(y-1)*VGA_WIDTH + x] = vga[y*VGA_WIDTH + x];
            for (uint32_t x = 0; x < VGA_WIDTH; x++)
                vga[(VGA_HEIGHT-1)*VGA_WIDTH + x] =
                    (uint16_t)(COL_DEFAULT << 8) | ' ';
            cursor_y = VGA_HEIGHT - 1;
        }
        vga[cursor_y * VGA_WIDTH + cursor_x] =
            (uint16_t)(color << 8) | (uint8_t)c;
        cursor_x++;
    }
}

static void puts(const char *s, uint8_t color)
{
    while (*s) putch(*s++, color);
}

/* ---------- Kendi mini yardimcilarimiz (freestanding) ---------- */
__attribute__((unused))
static void put_hex(uint32_t v, uint8_t color)
{
    const char *digs = "0123456789ABCDEF";
    putch('0', color); putch('x', color);
    for (int i = 28; i >= 0; i -= 4)
        putch(digs[(v >> i) & 0xF], color);
}

/* ============================================================
 * KLAVYE: PS/2 kontrolcu, IRQ1 (interrupt 0x21)
 * Tarayici (scancode set 1) -> ASCII cevirimi
 * ============================================================ */
#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64

static const char keymap_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    '\n', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0, ' ', 0
};
static const char keymap_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    '\n', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0, ' ', 0
};

static uint8_t shift_pressed = 0;

/* CLI satir tamponu */
#define LINE_MAX 128
static char line_buf[LINE_MAX];
static uint32_t line_len = 0;

/* Ileri bildirim: kesme isleyicisi komut calistiriciyi kullaniyor */
void cli_execute(const char *cmd);

/* IRQ1 kesme isleyicisi: IDT stub'i buraya atlar. Stub "call handler ;
 * iretd" seklinde oldugu icin, C tarafi cagri ABI'sinin bozabilecegi
 * eax/ecx/edx registerlarini elle kaydeder ve geri yukler. */
void keyboard_irq_handler(void)
{
    __asm__ volatile ("pushl %%eax; pushl %%ecx; pushl %%edx" ::: "memory");

    /* Kuyrukta veri var mi? */
    if (inb(KBD_STATUS_PORT) & 0x01) {
        uint8_t sc = inb(KBD_DATA_PORT);
        outb(0x20, 0x20);                        /* PIC'e EOI */

        /* Sol Shift: 0x2A bas, 0xAA birak */
        if (sc == 0x2A) shift_pressed = 1;
        if (sc == 0xAA) shift_pressed = 0;

        /* Basilan tusun birakma kodlari (bit7 set) yok sayilir */
        if (!(sc & 0x80)) {
            char c = shift_pressed ? keymap_shift[sc & 0x7F]
                                   : keymap_normal[sc & 0x7F];
            if (c) {
                if (c == '\b') {
                    if (line_len > 0) {
                        line_len--;
                        line_buf[line_len] = 0;
                        /* ekrandan da geri sil */
                        if (cursor_x > 0) {
                            cursor_x--;
                            vga[cursor_y*VGA_WIDTH + cursor_x] =
                                (uint16_t)(COL_DEFAULT << 8) | ' ';
                        }
                    }
                } else if (c == '\n') {
                    putch('\n', COL_DEFAULT);
                    line_buf[line_len] = 0;
                    /* ---- SATIR GONDERILDI: komutu calistir ---- */
                    cli_execute(line_buf);
                    line_len = 0;
                    for (int i = 0; i < LINE_MAX; i++) line_buf[i] = 0;
                    puts("amc> ", COL_PROMPT);
                } else {
                    if (line_len < LINE_MAX - 1) {
                        line_buf[line_len++] = c;
                        line_buf[line_len]   = 0;
                        putch(c, COL_DEFAULT);
                    }
                }
            }
        }
    } else {
        outb(0x20, 0x20);                        /* bos kuyruk: yine EOI */
    }

    __asm__ volatile ("popl %%edx; popl %%ecx; popl %%eax" ::: "memory");
}

/* ============================================================
 * IDT (Interrupt Descriptor Table) - kernel icinde kurulur
 * 256 girislik tablo; INT 0x21 (IRQ1 klavye) haric hepsi duba.
 * Stub kodu inline-asm ile calisma aninda bellege yazilir,
 * boylece C sembol adresleri (keyboard_irq_handler) dogru baglanir.
 * ============================================================ */
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;     /* 0x8E = present, ring0, 32-bit call gate */
    uint16_t offset_high;
} idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256] __attribute__((aligned(8)));

/* Duba isleyici: istenmeyen kesmede ekrana yaz, PIC'e EOI gonder */
static void isr_dummy(void)
{
    puts("\n[KESME: beklenmeyen vektor]", COL_ERR);
    outb(0x20, 0x20);                 /* master PIC EOI */
}

/* Calisma aninda bellege yazilacak stub'lar:
 *   mov eax, imm32 (B8 hh hh hh hh) ; call eax (FF D0) ; iretd (CF)
 * Stub 9 bayt; IDT bu stub'larin baslangic adresini gosterir. */
static uint8_t code_stub_kbd[9];        /* -> keyboard_irq_handler */
static uint8_t code_stub_dummy[9];      /* -> isr_dummy            */

static void build_stubs(void)
{
    uint32_t h1 = (uint32_t)&keyboard_irq_handler;
    uint32_t h2 = (uint32_t)&isr_dummy;

    code_stub_kbd[0] = 0xB8;
    code_stub_kbd[1] = (uint8_t)(h1); code_stub_kbd[2] = (uint8_t)(h1>>8);
    code_stub_kbd[3] = (uint8_t)(h1>>16); code_stub_kbd[4] = (uint8_t)(h1>>24);
    code_stub_kbd[5] = 0xFF; code_stub_kbd[6] = 0xD0;   /* call eax */
    code_stub_kbd[7] = 0xCF;                            /* iretd    */
    code_stub_kbd[8] = 0x90;                            /* nop      */

    code_stub_dummy[0] = 0xB8;
    code_stub_dummy[1] = (uint8_t)(h2); code_stub_dummy[2] = (uint8_t)(h2>>8);
    code_stub_dummy[3] = (uint8_t)(h2>>16); code_stub_dummy[4] = (uint8_t)(h2>>24);
    code_stub_dummy[5] = 0xFF; code_stub_dummy[6] = 0xD0;
    code_stub_dummy[7] = 0xCF;
    code_stub_dummy[8] = 0x90;
}

static void idt_set(uint32_t vec, uint32_t handler)
{
    idt[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[vec].offset_high = (uint16_t)(handler >> 16);
    idt[vec].selector    = 0x08;          /* bootloader GDT kod secicisi */
    idt[vec].zero        = 0;
    idt[vec].type_attr   = 0x8E;          /* P=1 DPL=0 Type=32bit int gate */
}

static void idt_load(void)
{
    idt_ptr_t p;
    p.limit = sizeof(idt) - 1;
    p.base  = (uint32_t)&idt;
    __asm__ volatile ("lidt %0" :: "m"(p));
}

/* ============================================================
 * Calisma aninda yazilan stub kodlari icin ayrik, YAZILABILIR+
 * CALISILABILIR (RX) bolge. Linker script bu section'i .text'in
 * hemen sonrasina koyar; boylece sayfalar RX haklarina sahip olur.
 * ============================================================ */
__attribute__((section(".data_code"), aligned(16)))
static uint8_t data_code_area[64];

/* Stub'lari data_code_area'ya (RX sayfa) tasi ve IDT'yi oraya yonlendir.
 * build_stubs() ile olusturulan makine kodu buradan son halini alir. */
static void init_interrupts(void)
{
    build_stubs();

    /* kbd stub: ilk 9 bayt */
    for (int i = 0; i < 9; i++) data_code_area[i] = code_stub_kbd[i];
    /* dummy stub: 16. bayttan itibaren (hizalama) */
    for (int i = 0; i < 9; i++) data_code_area[16 + i] = code_stub_dummy[i];

    /* IDT girislerini yeni adreslere yonlendir */
    for (int i = 0; i < 256; i++)
        idt_set(i, (uint32_t)&data_code_area[16]);
    idt_set(0x21, (uint32_t)&data_code_area[0]);   /* IRQ1 -> klavye */
    idt_load();
}

/* ============================================================
 * PIC (8259) baslangici - klavye kesmelerinin gelmesine izin ver
 * NOT: IDT kurulumundan SONRA cagrilir ki beklenmedik IRQ'lar
 * duba stub'a dusup sistemi kilitlemesin.
 * ============================================================ */
static void pic_init(void)
{
    outb(0x20, 0x11); io_wait();      /* ICW1: init */
    outb(0xA0, 0x11); io_wait();      /* ICW1: init */
    outb(0x21, 0x20); io_wait();      /* ICW2: master IRQ0 -> INT 0x20 */
    outb(0xA1, 0x28); io_wait();      /* ICW2: slave  IRQ8 -> INT 0x28 */
    outb(0x21, 0x04); io_wait();      /* ICW3: master cascade line 2 */
    outb(0xA1, 0x02); io_wait();      /* ICW3: slave cascade */
    outb(0x21, 0x01); io_wait();      /* ICW4: 8086 modu */
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFE);                 /* OCW1: sadece IRQ1 (klavye) acik */
    outb(0xA1, 0xFF);                 /* slave tamamen kapali */
}

/* ============================================================
 * CLI: basit komut yurutmek
 * ============================================================ */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int str_start(const char *s, const char *pre)
{
    while (*pre) { if (*s++ != *pre++) return 0; }
    return 1;
}

void cli_execute(const char *cmd)
{
    while (*cmd == ' ') cmd++;

    if (*cmd == 0)               return;              /* bos satir */
    if (str_eq(cmd, "help")) {
        puts("Komutlar: help, clear, echo <metin>, ver, reboot\n", COL_DEFAULT);
    } else if (str_eq(cmd, "clear")) {
        screen_clear();
    } else if (str_start(cmd, "echo ")) {
        puts(cmd + 6, COL_DEFAULT); putch('\n', COL_DEFAULT);
    } else if (str_eq(cmd, "ver")) {
        puts("AMC-OS v0.4 - freestanding C kernel - IRQ1 klavye aktif\n", COL_DEFAULT);
    } else if (str_eq(cmd, "reboot")) {
        puts("Yeniden baslatiliyor...\n", COL_ERR);
        /* BIOS reset vectorune uc at */
        void (*reset)(void) = (void (*)(void))0xFFFF0;
        reset();
    } else {
        puts("Bilinmeyen komut: ", COL_ERR);
        puts(cmd, COL_ERR);
        putch('\n', COL_ERR);
    }
}

/* ============================================================
 * Giris noktasi - bootloader protected mode'dan sonra burayi cagrir
 * ============================================================ */
void kernel_main(void)
{
    screen_clear();
    puts("amc-os Basariyla Baslatildi!\n", COL_DEFAULT);
    puts("Klavye icin: PS/2 IRQ1 tarayici modu aktif.\n", COL_DEFAULT);

    init_interrupts();                /* once IDT + stub'lar hazirla */
    pic_init();                       /* sonra PIC'i ac (klavyeye izin) */

    __asm__ volatile ("sti");         /* Kesmeleri ac! */

    puts("\namc> ", COL_PROMPT);      /* ilk istem satiri */

    for (;;)                          /* boot thread: hlt ile bekle */
        __asm__ volatile ("hlt");
}
