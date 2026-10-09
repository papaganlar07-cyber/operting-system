/* MacawOS kernel — POSIX-style syscall ABI + IPC (channels, futex) */
#ifndef MC_SYS_H
#define MC_SYS_H
#include "mc/types.h"

/* ---- syscall numbers (MacawOS POSIX-flavoured ABI) --------------- */
#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_GETPID      6
#define SYS_BRK         7
#define SYS_MMAP        8
#define SYS_MUNMAP      9
#define SYS_DUP         10
#define SYS_PIPE        11
#define SYS_SCHED_YIELD 12
#define SYS_NANOSLEEP   13
#define SYS_GETPPID     14
#define SYS_SENDFILE    15
#define SYS_FUTEX       16
#define SYS_SBRK_INFO   17
#define SYS_CHANNEL     18   /* create IPC channel -> returns fd      */
#define SYS_MSGSEND     19
#define SYS_MSGRECV     20
#define SYS_UNAME       21
#define SYS_MAX         64

/* mmap protection/flags */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10

/* futex ops */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

typedef struct {
    char sysname[16];    /* "MacawOS"                                */
    char nodename[32];
    char release[16];    /* e.g. "0.7.0-stage1"                      */
    char version[32];
    char machine[16];    /* "x86_64"                                 */
} utsname_t;

void syscall_init(void);
u64  syscall_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);

/* IPC channels (ipc/channel.c): ring buffer of fixed-size messages */
int  chan_create(size_t msg_size, u32 capacity);
int  chan_send(int ch, const void *msg, size_t len);
int  chan_recv(int ch, void *msg, size_t max_len);
void chan_close(int ch);

/* futex-lite (ipc/futex.c) */
int  kfutex_wait(u32 *addr, u32 expect, i64 timeout_ms);
int  kfutex_wake(u32 *addr, int count);

#endif
