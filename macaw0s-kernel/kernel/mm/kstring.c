/* MacawOS kernel — freestanding string/memory intrinsics (no libc) */
#include "mc/io.h"

void *memset(void *dst, int c, size_t n)
{
    u8 *d = (u8 *)dst;
    /* fast path: 8-byte fill for aligned bulk clears */
    if (n >= 64 && (((uintptr_t)dst) & 7u) == 0) {
        u64 pat = (u64)(u8)c * 0x0101010101010101ULL;
        u64 *w = (u64 *)d;
        size_t words = n / 8;
        for (size_t i = 0; i < words; i++) w[i] = pat;
        d += words * 8;
        n -= words * 8;
    }
    while (n--) *d++ = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (n >= 32 && ((((uintptr_t)dst) | ((uintptr_t)src)) & 7u) == 0) {
        const u64 *sw = (const u64 *)s;
        u64 *dw = (u64 *)d;
        size_t words = n / 8;
        for (size_t i = 0; i < words; i++) dw[i] = sw[i];
        d += words * 8;
        s += words * 8;
        n -= words * 8;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(d, s, n);
    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(u8)a[i] - (int)(u8)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}
