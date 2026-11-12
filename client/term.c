/*
 * term.c — terminal handling without ncurses.
 *
 * Raw mode keeps OPOST/ONLCR so '\n' from the UART still returns the cursor to column 1.
 * The screen is split with an ANSI scroll region: rows 1..N-1 scroll, row N is the status bar.
 *
 * R11 term-restore: atexit() plus handlers for SIGINT/SIGTERM/SIGHUP/SIGSEGV/SIGABRT put the
 * terminal back (termios, scroll region, cursor) before the process dies. The restore sequence
 * is pre-built so the signal handler only calls async-signal-safe write()/tcsetattr().
 * With --unsafe=term-restore none of this is installed: a crash leaves the shell in raw mode.
 */
#include "client.h"
#include "bench_util.h"

#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static int raw_on;
static char restore_seq[64];
static size_t restore_len;

static char obuf[256 * 1024];
static size_t olen;

void ob_flush(void)
{
    size_t off = 0;
    while (off < olen) {
        ssize_t w = write(STDOUT_FILENO, obuf + off, olen - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    olen = 0;
}

void ob_put(const void *p, size_t n)
{
    if (olen + n > sizeof obuf)
        ob_flush();
    if (n > sizeof obuf) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        (void)w;
        return;
    }
    memcpy(obuf + olen, p, n);
    olen += n;
}

void ob_str(const char *s) { ob_put(s, strlen(s)); }

void ob_printf(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0)
        ob_put(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

void term_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2 && ws.ws_col > 10) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

/* Pre-build the restore escape sequence for the current height (signal-safe use later). */
static void build_restore(int rows)
{
    restore_len = (size_t)snprintf(restore_seq, sizeof restore_seq,
                                   "\033[0m\033[r\033[?25h\033[%d;1H\033[K\r\n", rows);
}

void term_set_region(int rows)
{
    ob_printf("\0337\033[1;%dr\0338", rows - 1);   /* DECSTBM resets the cursor: save/restore */
    build_restore(rows);
}

void term_restore(void)
{
    if (!raw_on)
        return;
    raw_on = 0;
    ob_flush();
    ssize_t w = write(STDOUT_FILENO, restore_seq, restore_len);
    (void)w;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
}

static void on_fatal(int sig)
{
    if (raw_on) {
        ssize_t w = write(STDOUT_FILENO, restore_seq, restore_len);
        (void)w;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        raw_on = 0;
    }
    signal(sig, SIG_DFL);   /* die the way we were going to die */
    raise(sig);
}

int term_init(void)
{
    if (tcgetattr(STDIN_FILENO, &saved) < 0)
        return -1;
    struct termios t = saved;
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag |= CS8;
    t.c_oflag |= OPOST | ONLCR;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0)
        return -1;
    raw_on = 1;
    int rows, cols;
    term_size(&rows, &cols);
    build_restore(rows);

    if (!UNSAFE(R_TERM_RESTORE)) {
        atexit(term_restore);
        struct sigaction sa = { .sa_handler = on_fatal };
        sigemptyset(&sa.sa_mask);
        int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGSEGV, SIGABRT, SIGBUS, SIGQUIT };
        for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
            sigaction(sigs[i], &sa, NULL);
    }
    return 0;
}
