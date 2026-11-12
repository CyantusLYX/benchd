/*
 * cmd.c — keyboard handling (§4.2).
 *
 * Ctrl-] (0x1D) is the escape key; the next key selects a command. Everything else goes to the
 * UART if we hold control (batched: flush after 5 ms or 64 bytes), otherwise the status bar
 * flashes a read-only hint and the key is not sent.
 */
#include "client.h"
#include "bench_util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void log_toggle(struct app *a, const char *file)
{
    if (a->logf && (!file || !*file)) {
        fclose(a->logf);
        a->logf = NULL;
        ui_flash(a, FL_INFO, "recording stopped: %s", a->logname);
        return;
    }
    if (a->logf) {
        fclose(a->logf);
        a->logf = NULL;
    }
    if (file && *file) {
        bench_strlcpy(a->logname, file, sizeof a->logname);
    } else {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(a->logname, sizeof a->logname, "bench-%Y%m%d-%H%M%S.log", &tm);
    }
    a->logf = fopen(a->logname, "ab");
    if (!a->logf)
        ui_flash(a, FL_ERR, "cannot open %s: %s", a->logname, strerror(errno));
    else
        ui_flash(a, FL_OK, "recording UART data to %s", a->logname);
}

static void send_simple(struct app *a, uint8_t type)
{
    if (a->st != ST_LIVE) {
        ui_flash(a, FL_WARN, "not connected");
        return;
    }
    app_send(a, type, NULL, 0);
}

void cmd_exec(struct app *a, const char *line)
{
    while (*line == ' ' || *line == ':')
        line++;
    char word[32] = "";
    const char *arg = line;
    size_t wl = strcspn(line, " ");
    if (wl >= sizeof word)
        wl = sizeof word - 1;
    memcpy(word, line, wl);
    word[wl] = 0;
    arg = line + wl;
    while (*arg == ' ')
        arg++;

    if (!word[0])
        return;
    if (!strcmp(word, "take")) {
        send_simple(a, BN_TAKE);
    } else if (!strcmp(word, "release")) {
        if (!a->is_ctrl)
            ui_flash(a, FL_INFO, "you do not hold control");
        app_flush_input(a);
        send_simple(a, BN_RELEASE);
    } else if (!strcmp(word, "info")) {
        a->info_pending = 1;
        send_simple(a, BN_INFO_REQ);
    } else if (!strcmp(word, "ts")) {
        a->ts_mode = !a->ts_mode;
        ui_flash(a, FL_INFO, "timestamps %s", a->ts_mode ? "on" : "off");
    } else if (!strcmp(word, "hex")) {
        a->hex_mode = !a->hex_mode;
        ui_flash(a, FL_INFO, "hex dump %s", a->hex_mode ? "on" : "off");
    } else if (!strcmp(word, "log")) {
        log_toggle(a, arg);
    } else if (!strcmp(word, "baud")) {
        char *end;
        unsigned long b = strtoul(arg, &end, 10);
        if (!*arg || *end || b == 0) {
            ui_flash(a, FL_ERR, "usage: :baud <rate>");
        } else if (!a->is_ctrl) {
            ui_flash(a, FL_WARN, "only the controller can change baud — Ctrl-] t first");
        } else if (a->st == ST_LIVE) {
            uint32_t nb = htobe32((uint32_t)b);
            app_send(a, BN_BAUD, &nb, sizeof nb);
        }
    } else if (!strcmp(word, "help") || !strcmp(word, "?")) {
        ui_help(a);
    } else if (!strcmp(word, "quit") || !strcmp(word, "q")) {
        a->quit = 1;
    } else {
        ui_flash(a, FL_ERR, "unknown command '%s' — :help", word);
    }
}

static void escape_key(struct app *a, uint8_t k)
{
    switch (k) {
    case 't': cmd_exec(a, "take"); break;
    case 'r': cmd_exec(a, "release"); break;
    case 'i': cmd_exec(a, "info"); break;
    case 's': cmd_exec(a, "ts"); break;
    case 'h': cmd_exec(a, "hex"); break;
    case 'l': log_toggle(a, NULL); break;
    case '?': cmd_exec(a, "help"); break;
    case 'q': a->quit = 1; break;
    case ':':
        a->cmd_mode = 1;
        a->cmd_len = 0;
        ui_status(a);
        break;
    case ESC_KEY: {           /* literal 0x1D */
        uint8_t c = ESC_KEY;
        if (a->is_ctrl && a->st == ST_LIVE && a->ilen < sizeof a->ibuf)
            a->ibuf[a->ilen++] = c;
        break;
    }
    default:
        ui_flash(a, FL_WARN, "unknown key after Ctrl-] — Ctrl-] ? for help");
    }
}

static void cmdline_key(struct app *a, uint8_t k)
{
    if (k == '\r' || k == '\n') {
        a->cmd_mode = 0;
        a->cmd[a->cmd_len] = 0;
        ui_status(a);
        cmd_exec(a, a->cmd);
    } else if (k == 0x1b || k == 0x03) {
        a->cmd_mode = 0;
    } else if (k == 0x7f || k == 0x08) {
        if (a->cmd_len)
            a->cmd_len--;
        else
            a->cmd_mode = 0;
    } else if (k == 0x15) {         /* Ctrl-U */
        a->cmd_len = 0;
    } else if (k >= 0x20 && k < 0x7f && a->cmd_len < sizeof a->cmd - 1) {
        a->cmd[a->cmd_len++] = (char)k;
    }
    ui_status(a);
}

void cmd_keys(struct app *a, const uint8_t *k, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a->cmd_mode) {
            cmdline_key(a, k[i]);
            continue;
        }
        if (a->esc_pending) {
            a->esc_pending = 0;
            escape_key(a, k[i]);
            continue;
        }
        if (k[i] == ESC_KEY) {
            a->esc_pending = 1;
            continue;
        }
        if (a->st != ST_LIVE) {
            ui_flash(a, FL_WARN, "not connected — key not sent");
            continue;
        }
        if (!a->is_ctrl) {
            ui_flash(a, FL_WARN, "read-only — Ctrl-] t 取得控制");
            continue;
        }
        if (a->ilen == 0)
            a->iflush_at = mono_ms() + 5;
        a->ibuf[a->ilen++] = k[i];
        if (a->ilen >= sizeof a->ibuf)
            app_flush_input(a);
    }
}
