/* MacawOS kernel — VGA text console + kprintf/kpanic */
#ifndef MC_VGA_H
#define MC_VGA_H
#include "mc/types.h"

void kconsole_init(void);
void kclear(void);
void kputc(char c);
void kputs(const char *s);
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, __builtin_va_list ap);
NORETURN void kpanic(const char *fmt, ...);

#endif
