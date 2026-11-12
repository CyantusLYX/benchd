/*
 * ui.c — everything the interactive client draws (§4.1).
 *
 *   rows 1..N-1  scrolling UART output, separators (──── text ────, dim), info/help boxes
 *   row  N       status bar:  badge │ device baud 8N1 │ ctrl: alice (idle 4s) │ 3 viewers │ rx 12.3K
 *                badge colours: CTRL green, VIEW grey, DEV LOST red, RECONNECTING yellow
 *
 * The cursor always lives in the scroll region; the status bar is painted with save/restore
 * cursor (ESC 7 / ESC 8) so output never lands on it.
 */
#include "client.h"
#include "bench_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DIM   "\033[2m"
#define RESET "\033[0m"

/* ---- display width of UTF-8 text (CJK = 2 columns) ---- */
static int cp_width(uint32_t cp)
{
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

static size_t utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *cp = ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *cp = ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
        return 4;
    }
    *cp = '?';
    return 1;
}

int str_width(const char *s)
{
    int w = 0;
    uint32_t cp;
    while (*s) {
        s += utf8_next(s, &cp);
        w += cp_width(cp);
    }
    return w;
}

/* Copy at most `maxw` columns of s into out; returns the width used. */
static int str_fit(char *out, size_t outsz, const char *s, int maxw)
{
    int w = 0;
    size_t o = 0;
    uint32_t cp;
    while (*s) {
        size_t l = utf8_next(s, &cp);
        int cw = cp_width(cp);
        if (w + cw > maxw || o + l + 1 > outsz)
            break;
        memcpy(out + o, s, l);
        o += l;
        s += l;
        w += cw;
    }
    out[o] = 0;
    return w;
}

void ui_init(struct app *a)
{
    term_size(&a->rows, &a->cols);
    ob_str("\033[2J");                       /* clear */
    term_set_region(a->rows);
    ob_printf("\033[%d;1H", a->rows - 1);    /* start writing at the bottom of the region */
    a->at_bol = 1;
    ui_status(a);
}

void ui_resize(struct app *a)
{
    int old_rows = a->rows;
    term_size(&a->rows, &a->cols);
    ob_printf("\0337\033[%d;1H\033[K\0338", old_rows);   /* wipe the old status row */
    term_set_region(a->rows);
    ob_printf("\033[%d;1H", a->rows - 1);
    a->at_bol = 1;
    ui_status(a);
}

static void newline_if_needed(struct app *a)
{
    if (!a->at_bol) {
        ob_str("\r\n");
        a->at_bol = 1;
    }
}

static void ts_prefix(uint64_t ts_ns)
{
    char t[32];
    fmt_clock(ts_ns, t, sizeof t);
    ob_printf(DIM "[%s]" RESET " ", t);
}

/*
 * ANSI filter. Real shells (zsh/bash prompts, ls --color) send escape sequences; passing them
 * all through would let the device move the cursor onto the status bar, reset our scroll region
 * or make the terminal answer queries. Allowed through: SGR colours (m), erase in line (K),
 * relative cursor moves (A B C D G) and insert/delete/erase chars (@ P X); "erase display" (J)
 * becomes erase-in-line. Dropped: absolute positioning (H f), scroll region (r), private modes
 * (ESC[?...), queries (n c), OSC/DCS strings (window titles), and all other ESC sequences.
 */
static void esc_byte(struct app *a, uint8_t ch)
{
    switch (a->esc_state) {
    case ESC_GOT_ESC:
        if (ch == '[') {
            a->esc_state = ESC_CSI;
            a->esc_len = 0;
        } else if (ch == ']' || ch == 'P' || ch == '_' || ch == '^' || ch == 'X') {
            a->esc_state = ESC_STRING;          /* OSC / DCS / APC / PM / SOS: swallow */
        } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+' || ch == '#' || ch == '%') {
            a->esc_state = ESC_ONE_MORE;        /* charset designation etc.: one more byte */
        } else {
            a->esc_state = ESC_NONE;            /* ESC 7, ESC 8, ESC c, ESC M, ESC = ... dropped */
        }
        return;
    case ESC_ONE_MORE:
        a->esc_state = ESC_NONE;
        return;
    case ESC_STRING:
        if (ch == 0x07)
            a->esc_state = ESC_NONE;
        else if (ch == 0x1b)
            a->esc_state = ESC_STRING_ESC;
        return;
    case ESC_STRING_ESC:
        a->esc_state = ch == '\\' ? ESC_NONE : ESC_STRING;
        return;
    case ESC_CSI:
        if (ch >= 0x40 && ch <= 0x7e) {          /* final byte */
            a->esc_state = ESC_NONE;
            int priv = a->esc_len && (a->esc_buf[0] == '?' || a->esc_buf[0] == '>' ||
                                      a->esc_buf[0] == '<' || a->esc_buf[0] == '=');
            if (priv)
                return;
            if (ch == 'J') {                     /* never clear the status bar */
                ob_str("\033[K");
                return;
            }
            if (!strchr("mKABCDGPX@", ch))
                return;
            ob_str("\033[");
            ob_put(a->esc_buf, a->esc_len);
            ob_put(&ch, 1);
            return;
        }
        if (ch < 0x20 || a->esc_len >= sizeof a->esc_buf) {
            a->esc_state = ESC_NONE;             /* malformed or absurdly long: drop it */
            return;
        }
        a->esc_buf[a->esc_len++] = (char)ch;
        return;
    default:
        a->esc_state = ESC_NONE;
    }
}

void ui_data(struct app *a, const uint8_t *d, size_t n, uint64_t ts_ns)
{
    if (a->hex_mode) {
        newline_if_needed(a);
        char line[96];
        for (size_t i = 0; i < n; i += 16) {
            if (a->ts_mode)
                ts_prefix(ts_ns);
            hexdump_line(line, sizeof line, d + i, n - i < 16 ? n - i : 16);
            ob_str(line);
            ob_str("\r\n");
        }
        a->at_bol = 1;
        return;
    }
    size_t run = 0;   /* flush runs of plain bytes in one go */
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = d[i];
        if (a->esc_state) {                 /* inside an escape sequence (may span records) */
            esc_byte(a, ch);
            continue;
        }
        int plain = (ch >= 0x20 && ch != 0x7f) || ch == '\t' || ch == '\b';
        if ((plain || ch == 0x1b) && a->at_bol && a->ts_mode) {
            if (run) { ob_put(d + i - run, run); run = 0; }
            ts_prefix(ts_ns);
            a->at_bol = 0;
        }
        if (plain) {
            a->at_bol = 0;
            run++;
            continue;
        }
        if (run) { ob_put(d + i - run, run); run = 0; }
        if (ch == '\n') {
            ob_str("\n");           /* ONLCR turns this into CR LF */
            a->at_bol = 1;
        } else if (ch == '\r') {
            ob_str("\r");
        } else if (ch == 0x1b) {
            a->esc_state = ESC_GOT_ESC;
            a->esc_len = 0;
        }
        /* other control bytes (NUL, BEL, ...) are dropped */
    }
    if (run)
        ob_put(d + n - run, run);
}

void ui_separator(struct app *a, const char *text)
{
    newline_if_needed(a);
    int width = a->cols - 1;
    int tw = str_width(text) + 2;
    int left = (width - tw) / 2, right = width - tw - left;
    if (left < 2) left = 2;
    if (right < 2) right = 2;
    ob_str(DIM);
    for (int i = 0; i < left; i++) ob_str("─");
    ob_printf(" %s ", text);
    for (int i = 0; i < right; i++) ob_str("─");
    ob_str(RESET "\r\n");
    a->at_bol = 1;
}

void ui_note(struct app *a, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    newline_if_needed(a);
    ob_printf(DIM "%s" RESET "\r\n", b);
    a->at_bol = 1;
}

void ui_event(struct app *a, int code, const char *text, uint64_t ts_ns)
{
    char hms[16], b[300];
    fmt_hms(ts_ns, hms, sizeof hms);
    switch (code) {
    case EV_DROPPED:
        snprintf(b, sizeof b, "[%s]", text);
        break;
    default:                                  /* "device reconnected 20:44:13" */
        snprintf(b, sizeof b, "%s %s", text, hms);
    }
    ui_separator(a, b);
}

/* ---- boxes ---- */

static void box_line(int inner, const char *text)
{
    char fit[512];
    int w = str_fit(fit, sizeof fit, text, inner - 2);
    ob_printf("│ %s%*s │\r\n", fit, inner - 2 - w, "");
}

static void box_rule(int inner, const char *l, const char *title, const char *r)
{
    ob_str(l);
    int used = 0;
    if (title && *title) {
        ob_printf("─ %s ", title);
        used = str_width(title) + 3;
    }
    for (int i = used; i < inner + 2; i++)
        ob_str("─");
    ob_str(r);
    ob_str("\r\n");
}

static void draw_box(struct app *a, const char *title, char lines[][256], int n)
{
    int inner = a->cols - 4;
    if (inner > 70)
        inner = 70;
    if (inner < 20)
        inner = 20;
    newline_if_needed(a);
    box_rule(inner, "┌", title, "┐");
    for (int i = 0; i < n; i++)
        box_line(inner + 2, lines[i]);
    box_rule(inner, "└", NULL, "┘");
    a->at_bol = 1;
}

/* Turn "key=value\n" INFO text into indented section lines (also used by `bench info`). */
int info_box_lines(const char *kv, size_t len, int width, char lines[][256], int max)
{
    static const struct { const char *prefix, *title; } sec[] = {
        { "host.", "Host" }, { "server.", "Server" }, { "dev.", "Device" },
        { "hist.", "History" }, { "ctrl.", "Control" },
    };
    (void)width;
    int n = 0;
    for (size_t si = 0; si < sizeof sec / sizeof sec[0] && n < max; si++) {
        size_t pl = strlen(sec[si].prefix);
        int header_done = 0;
        const char *p = kv, *end = kv + len;
        while (p < end && n < max) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t l = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (l > pl && strncmp(p, sec[si].prefix, pl) == 0) {
                const char *eq = memchr(p, '=', l);
                if (eq) {
                    if (!header_done) {
                        if (n && n < max)
                            lines[n++][0] = 0;
                        snprintf(lines[n++], 256, "%s", sec[si].title);
                        header_done = 1;
                    }
                    if (n < max)
                        snprintf(lines[n++], 256, "  %-16.*s %.*s", (int)(eq - p - (long)pl), p + pl,
                                 (int)(l - (size_t)(eq - p) - 1), eq + 1);
                }
            }
            p = nl ? nl + 1 : end;
        }
    }
    return n;
}

void ui_info_box(struct app *a, const char *kv, size_t len)
{
    char lines[64][256];
    int n = info_box_lines(kv, len, a->cols, lines, 64);
    draw_box(a, "System info", lines, n);
}

void ui_help(struct app *a)
{
    static const char *h[] = {
        "Ctrl-] t   :take          take control (type into the UART)",
        "Ctrl-] r   :release       release control",
        "Ctrl-] i   :info          system information",
        "Ctrl-] s   :ts            toggle timestamps (server time)",
        "Ctrl-] h   :hex           toggle hex dump",
        "Ctrl-] l   :log [file]    start/stop recording UART data to a file",
        "           :baud <rate>   change baud rate (controller only)",
        "Ctrl-] :                  command line (Enter runs, Esc cancels)",
        "Ctrl-] ?   :help          this help",
        "Ctrl-] q   :quit          quit",
        "Ctrl-] Ctrl-]             send a literal 0x1D",
    };
    char lines[16][256];
    int n = 0;
    for (size_t i = 0; i < sizeof h / sizeof h[0]; i++)
        snprintf(lines[n++], 256, "%s", h[i]);
    draw_box(a, "Keys", lines, n);
}

/* ---- status bar ---- */

void ui_flash(struct app *a, int color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->flash, sizeof a->flash, fmt, ap);
    va_end(ap);
    a->flash_color = color;
    a->flash_until = mono_ms() + (color == FL_ERR || color == FL_WARN ? 4000 : 2500);
    ui_status(a);
}

void ui_status(struct app *a)
{
    char badge[64], rest[512], fit[512], sz[32];
    const char *bcol;
    uint64_t now = mono_ms();
    a->last_status_draw = now;

    if (a->cmd_mode) {
        snprintf(rest, sizeof rest, ":%.*s", (int)a->cmd_len, a->cmd);
        int w = str_fit(fit, sizeof fit, rest, a->cols - 2);
        ob_printf("\0337\033[%d;1H\033[0;97;48;5;236m%s\033[7m \033[27m%*s\033[0m\0338",
                  a->rows, fit, a->cols - 2 - w, "");
        return;
    }

    if (a->st != ST_LIVE) {
        uint64_t left = a->retry_at > now ? (a->retry_at - now + 999) / 1000 : 0;
        if (a->st == ST_HANDSHAKE || !a->ever_welcomed)
            snprintf(badge, sizeof badge, " %s ", a->ever_welcomed ? "RECONNECTING" : "CONNECTING");
        else
            snprintf(badge, sizeof badge, " RECONNECTING (%llus) ", (unsigned long long)left);
        bcol = "\033[1;30;43m";
    } else if (a->dev_state != DEV_UP) {
        snprintf(badge, sizeof badge, " DEV LOST ");
        bcol = "\033[1;97;41m";
    } else if (a->is_ctrl) {
        snprintf(badge, sizeof badge, " CTRL ");
        bcol = "\033[1;30;42m";
    } else {
        snprintf(badge, sizeof badge, " VIEW ");
        bcol = "\033[1;97;100m";
    }

    const char *fcol = "\033[97;48;5;236m";
    if (a->flash[0] && now < a->flash_until) {
        snprintf(rest, sizeof rest, " %s", a->flash);
        fcol = a->flash_color == FL_ERR ? "\033[1;91;48;5;236m" :
               a->flash_color == FL_WARN ? "\033[1;93;48;5;236m" :
               a->flash_color == FL_OK ? "\033[1;92;48;5;236m" : "\033[1;96;48;5;236m";
    } else {
        char ctrl[96];
        if (!a->ctrl_name[0])
            snprintf(ctrl, sizeof ctrl, "ctrl: none");
        else if (a->is_ctrl)
            snprintf(ctrl, sizeof ctrl, "ctrl: you (idle %us)", a->ctrl_idle);
        else
            snprintf(ctrl, sizeof ctrl, "ctrl: %s (idle %us)", a->ctrl_name, a->ctrl_idle);
        char flags[64] = "";
        if (a->ts_mode) strcat(flags, " TS");
        if (a->hex_mode) strcat(flags, " HEX");
        if (a->logf) strcat(flags, " REC");
        snprintf(rest, sizeof rest, " │ %s %u 8N1 │ %s │ %u viewer%s │ rx %s%s%s",
                 a->dev_path[0] ? a->dev_path : "?", a->baud, ctrl, a->viewers,
                 a->viewers == 1 ? "" : "s", fmt_size(a->rx_bytes, sz, sizeof sz),
                 flags[0] ? " │" : "", flags);
    }
    /* never touch the last column: avoids auto-wrap/scroll on the bottom row */
    int bw = str_width(badge);
    int w = str_fit(fit, sizeof fit, rest, a->cols - 1 - bw);
    ob_printf("\0337\033[%d;1H%s%s\033[0m%s%s%*s\033[0m\0338", a->rows, bcol, badge, fcol, fit,
              a->cols - 1 - bw - w, "");
}
