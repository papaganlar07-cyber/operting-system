/* MacawOS kernel — PIT timer (IRQ0), PS/2 keyboard (IRQ1) */
#include "mc/cpu.h"
#include "mc/io.h"
#include "mc/vga.h"

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define KBD_DATA  0x60
#define KBD_STAT  0x64

static volatile u64 g_ticks;          /* 1193182 Hz ticks since boot      */
static volatile u64 g_ms;             /* milliseconds (tick/~1.193k)      */

/* scancode set 1 → ASCII (make code only) */
static const char kbd_normal[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'\\',
    'a','s','d','f','g','h','j','k','l',';',0,'`',0,'\'','z','x',
    'c','v','b','n','m',',','.','/',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static volatile bool g_shift;
static volatile char g_last_key;      /* last printable, for console echo */

void kbdbuf_putc(char c);             /* serial/VGA hook in kernel.c      */

static void timer_irq(regs_t *r)
{
    UNUSED(r);
    g_ticks++;
    if ((g_ticks % 119u) == 0) g_ms++;
}

static void kbd_irq(regs_t *r)
{
    UNUSED(r);
    u8 sc = inb(KBD_DATA);
    if (sc == 0x2A || sc == 0x36) { g_shift = true; return; }
    if (sc == 0xAA || sc == 0xB6) { g_shift = false; return; }
    if (sc & 0x80) return;                          /* break code         */
    char c = kbd_normal[sc & 0x7F];
    if (g_shift && c >= 'a' && c <= 'z') c -= 32;
    if (c) { g_last_key = c; kbdbuf_putc(c); }
}

void timer_init_100hz(void)
{
    /* 1193182 / 1193 ≈ 1000 Hz for ms precision */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(1193 & 0xFF));
    outb(PIT_CH0, (u8)(1193 >> 8));
    isr_install(32 + 0, timer_irq);
    pic_mask(0, false);
}

void kbd_init(void)
{
    /* disable mouse port aux (many BIOS leave it enabled with no device) */
    outb(KBD_STAT, 0xA8);
    outb(KBD_STAT, 0x20);
    u8 cfg = inb(KBD_DATA);
    outb(KBD_DATA, (u8)(cfg | 0x02));               /* IRQ1 on scan codes */
    isr_install(32 + 1, kbd_irq);
    pic_mask(1, false);
}

u64 timer_ticks(void) { return g_ticks; }

void sleep_ms(u32 ms)
{
    u64 target = g_ms + ms;
    while (g_ms < target) hlt();
}

char kbd_get_last(void) { char c = g_last_key; g_last_key = 0; return c; }
