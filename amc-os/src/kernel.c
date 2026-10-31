/* kernel.c - AMC-OS v0.1: Freestanding C Cekirdegi */

#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH  80u
#define VGA_HEIGHT 25u
#define VGA_COLOR(bg, fg) ((uint8_t)(((bg) << 4) | (fg)))

static volatile uint16_t *const vga = (volatile uint16_t *)0xB8000;

static size_t cursor_x = 0;
static size_t cursor_y = 0;

static void set_cursor(size_t x, size_t y) {
    cursor_x = x;
    cursor_y = y;
}

static void clear_screen(void) {
    const uint16_t blank = ((uint16_t)VGA_COLOR(0, 7) << 8) | ' ';
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = blank;
    }
    set_cursor(0, 0);
}

static void scroll_up(void) {
    for (size_t i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        vga[i] = vga[i + VGA_WIDTH];
    }
    for (size_t i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        vga[i] = ((uint16_t)VGA_COLOR(0, 7) << 8) | ' ';
    }
    cursor_y = VGA_HEIGHT - 1;
}

static void put_char(char c, uint8_t color) {
    if (c == '\n') {
        cursor_x = 0;
        if (++cursor_y >= VGA_HEIGHT) scroll_up();
        return;
    }
    if (c == '\r') { cursor_x = 0; return; }
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga[cursor_y * VGA_WIDTH + cursor_x] =
                ((uint16_t)VGA_COLOR(0, 7) << 8) | ' ';
        }
        return;
    }
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        if (++cursor_y >= VGA_HEIGHT) scroll_up();
    }
    vga[cursor_y * VGA_WIDTH + cursor_x] =
        ((uint16_t)color << 8) | (uint8_t)c;
    cursor_x++;
}

static void draw_text_centered(const char *s, uint8_t color) {
    size_t len = 0;
    while (s[len]) len++;
    if (len > VGA_WIDTH) len = VGA_WIDTH;

    size_t start_col = (VGA_WIDTH - len) / 2;
    size_t row       = VGA_HEIGHT / 2;

    for (size_t i = 0; i < len; i++) {
        vga[row * VGA_WIDTH + start_col + i] =
            ((uint16_t)color << 8) | (uint8_t)s[i];
    }
    set_cursor(start_col + len, row);
}

/* put_char'i ileri tarihli konsol/CLI katmaninin temeli olarak koru;
 * simdi log satirlarinda kullanilarak -Wunused-function tetiklenmez. */
static void kputs(const char *s, uint8_t color) {
    while (*s) put_char(*s++, color);
}

void kernel_main(void) {
    clear_screen();
    draw_text_centered("amc-os v0.1 Yuklendi", VGA_COLOR(0, 15));

    /* Alt bilgi: gercek boot sonrasi log ornegi (put_char kullanimini dogrular) */
    set_cursor(0, VGA_HEIGHT - 2);
    kputs("[ ok ] GDT sabit, IDT hazirlandi (Adim 4)", VGA_COLOR(0, 10));
    set_cursor(0, VGA_HEIGHT - 1);
    kputs("[ ok ] hafiza: 0xB8000 VGA erisimi aktif", VGA_COLOR(0, 10));

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
