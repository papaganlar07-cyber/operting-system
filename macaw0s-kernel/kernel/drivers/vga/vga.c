/* MacawOS kernel — VGA text-mode console + kprintf (serial mirror) */
#include "mc/vga.h"
#include "mc/io.h"

#define VGA_MEM   ((volatile u16 *)0xB8000ULL)
#define VGA_W     80
#define VGA_H     25
#define COM1      0x3F8

static size_t g_cursor;          /* cell index                     */
static u8     g_attr = 0x0F;     /* light grey on black            */
static bool   g_serial_ok;

static void cursor_update(void)
{
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(g_cursor % VGA_W));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)(g_cursor / VGA_W));
}

static void scroll(void)
{
    if (g_cursor < VGA_W * VGA_H) return;
    memmove((void *)VGA_MEM, (const void *)(VGA_MEM + VGA_W),
            (VGA_W * (VGA_H - 1)) * sizeof(u16));
    for (int i = 0; i < VGA_W; i++) VGA_MEM[(VGA_H - 1) * VGA_W + i] = (u16)(g_attr << 8);
    g_cursor = (size_t)(VGA_H - 1) * VGA_W;
}

void kconsole_init(void)
{
    /* serial: 115200 8N1 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    g_serial_ok = true;
    kclear();
}

void kclear(void)
{
    for (size_t i = 0; i < VGA_W * VGA_H; i++) VGA_MEM[i] = (u16)(g_attr << 8);
    g_cursor = 0;
    cursor_update();
}

void kputc(char c)
{
    if (c == '\n') {
        g_cursor += VGA_W - (g_cursor % VGA_W);
    } else if (c == '\r') {
        g_cursor -= g_cursor % VGA_W;
    } else if (c == '\b') {
        if (g_cursor) { g_cursor--; VGA_MEM[g_cursor] = (u16)(g_attr << 8); }
    } else {
        VGA_MEM[g_cursor++] = (u16)(((u8)c) | (g_attr << 8));
    }
    scroll();
    cursor_update();
    if (g_serial_ok && (c == '\n' || (c >= 0x20 && c <= 0x7E))) {
        while (!(inb(COM1 + 5) & 0x20)) CPU_RELAX();
        outb(COM1, (u8)c);
    }
}

void kputs(const char *s)
{
    while (*s) kputc(*s++);
}

/* ---- minimal vsnprintf core ---------------------------------------------- */
struct out_ctx { char *buf; size_t pos, cap; };   /* cap==SIZE_MAX => direct */
#define OUT_DIRECT ((size_t)~0ULL)

static void emit(struct out_ctx *o, char c)
{
    if (o->cap != OUT_DIRECT) {
        if (o->pos + 1 < o->cap) o->buf[o->pos] = c;
        o->pos++;
    } else {
        kputc(c);
    }
}

static void emit_hex(struct out_ctx *o, u64 v, int width, int upper)
{
    static const char lo[] = "0123456789abcdef", hi[] = "0123456789ABCDEF";
    const char *xd = upper ? hi : lo;
    char tmp[17]; int n = 0;
    do { tmp[n++] = xd[v & 0xF]; v >>= 4; } while (v);
    while (n < width) tmp[n++] = '0';
    while (n--) emit(o, tmp[n]);
}

static void emit_dec(struct out_ctx *o, u64 v, int width, bool neg)
{
    char tmp[21]; int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    if (neg) tmp[n++] = '-';
    while (n < width) tmp[n++] = ' ';
    while (n--) emit(o, tmp[n]);
}

static void fmt_core(struct out_ctx *o, const char *fmt, __builtin_va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') { emit(o, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { emit(o, '%'); fmt++; continue; }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int longness = 0;               /* 0/1/2 => l/ll count */
        while (*fmt == 'l') { longness++; fmt++; }
        char c = *fmt++;
        switch (c) {
        case 'd': case 'i': {
            i64 v = longness >= 2 ? __builtin_va_arg(ap, i64)
                    : (i64)__builtin_va_arg(ap, int);
            u64 mag = v < 0 ? (u64)(-(v + 1)) + 1ULL : (u64)v; /* INT64_MIN-safe */
            emit_dec(o, mag, width, v < 0);
            break; }
        case 'u': {
            u64 v = longness >= 2 ? __builtin_va_arg(ap, u64)
                    : (u64)__builtin_va_arg(ap, unsigned int);
            emit_dec(o, v, width, false);
            break; }
        case 'x': case 'X': {
            u64 v = longness >= 1 ? __builtin_va_arg(ap, u64)
                    : (u64)__builtin_va_arg(ap, unsigned int);
            emit_hex(o, v, width, c == 'X');
            break; }
        case 'p': {
            emit(o, '0'); emit(o, 'x');
            emit_hex(o, __builtin_va_arg(ap, u64), 16, 0);
            break; }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) emit(o, *s++);
            break; }
        case 'c': emit(o, (char)__builtin_va_arg(ap, int)); break;
        default:
            emit(o, '%'); if (longness) emit(o, 'l'); emit(o, c);
        }
    }
}

int kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    struct out_ctx o = { .buf = NULL, .pos = 0, .cap = OUT_DIRECT };
    fmt_core(&o, fmt, ap);
    __builtin_va_end(ap);
    return (int)o.pos;
}

void kvprintf(const char *fmt, __builtin_va_list ap)
{
    struct out_ctx o = { .buf = NULL, .pos = 0, .cap = OUT_DIRECT };
    fmt_core(&o, fmt, ap);
}

NORETURN void kpanic(const char *fmt, ...)
{
    g_attr = 0x4F; /* white on red */
    kputs("\n*** MACAWOS KERNEL PANIC ***\n");
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
    kputs("\n-- system halted --\n");
    for (;;) cli(), hlt();
}
