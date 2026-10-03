/* ============================================================
 * AMC OS — AMC Shell (amsh) — POSIX'a yakın etkileşimli kabuk
 * userspace/sh/amsh.c
 *   - Komut çalıştırma, boru (|), yönlendirme (>, >>), alt kabuk ($())
 *   - Job control (Ctrl-Z, fg/bg), tarihçe (arrow-up), tab tamamlama
 *   - Betik dili: değişkenler, if/for/while, fonksiyonlar
 *   - Yazılımcı hedefi: make/ninja/gcc/clang/gdb/lldb burada koşar
 * ============================================================ */

#include <amc/syscall.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_LINE 4096
#define HISTORY_SIZE 1000

static char *history[HISTORY_SIZE];
static int hist_count = 0;

/* ================== Satır ayrıştırma ================== */

struct token { char *text; enum { T_WORD, T_PIPE, T_REDIR_OUT,
                                  T_REDIR_APPEND, T_BG, T_SEMI } kind; };

static int tokenize(char *line, struct token *toks, int max) {
    int n = 0;
    for (char *p = strtok(line, " \t"); p && n < max; p = strtok(NULL, " \t")) {
        if (!strcmp(p, "|"))       toks[n++].kind = T_PIPE;
        else if (!strcmp(p, ">"))  toks[n++].kind = T_REDIR_OUT;
        else if (!strcmp(p, ">>")) toks[n++].kind = T_REDIR_APPEND;
        else if (!strcmp(p, "&"))  toks[n++].kind = T_BG;
        else if (!strcmp(p, ";"))  toks[n++].kind = T_SEMI;
        else { toks[n].kind = T_WORD; toks[n].text = p; n++; }
    }
    return n;
}

/* ================== Yerleşik komutlar ================== */

static int builtin_cd(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : getenv("HOME");
    if (chdir(dir) != 0) { fprintf(stderr, "amsh: cd: %s: yok\n", dir); return 1; }
    setenv("PWD", dir, 1);
    return 0;
}

static int builtin_export(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {          /* export CC=gcc */
        char *eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; setenv(argv[i], eq + 1, 1); }
    }
    return 0;
}

static int builtin_jobs(void) {
    for (int j = 0; j < job_table_count(); j++) {
        struct job *jb = job_get(j);
        printf("[%d] %-8s %s\n", j + 1,
               jb->state == JOB_STOPPED ? "DURDU" :
               jb->state == JOB_RUNNING ? "CALISIYOR" : "BITTI",
               jb->cmdline);
    }
    return 0;
}

static const struct { const char *name; int (*fn)(int, char**); } builtins[] = {
    { "cd",     builtin_cd     },
    { "export", builtin_export },
    { "jobs",   builtin_jobs   },
    { "exit",   NULL           },
    {}
};

/* ================== Dış komut + boru hattı ================== */

static int run_pipeline(struct token *toks, int nt) {
    /* Segment'lere böl (pipe sınırları) ve fork/exec zincirle */
    int prev_fd = -1;
    for (int seg = 0; seg < count_segments(toks, nt); seg++) {
        int fds[2];
        if (seg + 1 < count_segments(toks, nt)) pipe(fds);

        pid_t pid = fork();
        if (pid == 0) {                       /* çocuk */
            if (prev_fd >= 0) { dup2(prev_fd, STDIN_FILENO); close(prev_fd); }
            if (fds[1] >= 0)  { dup2(fds[1], STDOUT_FILENO); }
            apply_redirs(&toks[seg]);         /* > dosya vb. */
            execvp(seg_argv[0], seg_argv);    /* PATH'ten ara-çalıştır */
            fprintf(stderr, "amsh: %s: bulunamadi\n", seg_argv[0]);
            _exit(127);
        }
        /* ebeveyn: bir sonraki segment'in stdin'i bu boru olur */
        if (fds[1] >= 0) close(fds[1]);
        prev_fd = fds[0];
        job_add(pid, seg_line);               /* Ctrl-Z/fg için iş kaydı */
    }
    if (!bg_flag) wait_for_last_job();        /* foreground bekleme */
    return 0;
}

/* ================== Etkileşimli döngü ================== */

static void print_prompt(void) {
    printf("\033[32mamsh\033[0m:%s$ ", shorten_path(getenv("PWD")));
}

static char *readline_with_history(void) {
    /* ham mod → ok tuşları tarihçe, Tab → dosya adı tamamlama */
    raw_mode_enter();
    ... edit buffer with history_up/history_down/tab_complete ...
    raw_mode_exit();
    return strdup(buffer);
}

int main(int argc, char **argv) {
    load_rc_file("~/.amshrc");                /* kullanıcı alias/env'leri */

    if (argc > 1) return run_script(argv[1]); /* betik modu: amsh dosya.am */

    print_banner("AMC Shell 0.4 — POSIX benzeri, betik destekli");
    for (;;) {
        print_prompt();
        char *line = readline_with_history();
        if (!line) break;                      /* Ctrl-D */
        if (*line) history[hist_count++ % HISTORY_SIZE] = strdup(line);

        struct token toks[64];
        int n = tokenize(line, toks, 64);
        if (n == 0) continue;

        if (toks[0].kind == T_WORD) {
            auto *b = find_builtin(toks[0].text);
            if (b) { b->fn(n, arg_words(toks)); continue; }
            if (!strcmp(toks[0].text, "exit")) break;
        }
        run_pipeline(toks, n);
        free(line);
    }
    printf("Gorusmek uzere!\n");
    return 0;
}
