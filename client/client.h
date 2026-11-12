/*
 * client.h — internal definitions for the `bench` client.
 *
 *   main.c     argument parsing, dispatch to interactive mode or a subcommand
 *   session.c  connection, HELLO/resume, packet handling and reconnect loop (interactive)
 *   term.c     raw mode, scroll region, terminal restore on exit/crash (R11)
 *   ui.c       rendering: UART output, separators, status bar, info/help boxes
 *   cmd.c      key handling (Ctrl-] escapes) and the ':' command line
 *   subcmd.c   non-interactive: info / tail / watch / send
 */
#ifndef BENCH_CLIENT_H
#define BENCH_CLIENT_H

#include <stdint.h>
#include <stdio.h>

#include "bench_proto.h"

#define ESC_KEY 0x1D   /* Ctrl-] */

struct client_opts {
    char     host[256];
    uint16_t port;
    char     name[BN_NAME_LEN];
    uint64_t replay;
    int      replay_set;
    int      reconnect;
    int      verbose;
    /* subcommands */
    double   duration;
    const char *out;
    unsigned wait_ms;
    int      raw;
};

/* Connection state shared by interactive mode and subcommands. */
struct conn {
    int      fd;
    uint64_t pos;                 /* stream offset after the last record we received */
    int      have_pos;
    uint64_t server_start_ns;     /* from the last WELCOME */
    int      have_start;
    uint64_t last_rx_ms;
    uint64_t last_ping_ms;
};

int  conn_connect(struct conn *c, const struct client_opts *o, uint32_t replay, int timeout_ms);
int  conn_recv(struct conn *c, struct bn_hdr *h, uint8_t *buf, size_t cap, int *perr);
int  conn_send(struct conn *c, uint8_t type, const void *p, uint32_t len);
void conn_close(struct conn *c);
void conn_bye(struct conn *c);         /* BYE + half-close + drain: FIN, not RST */
int  conn_wait_welcome(struct conn *c, struct bn_welcome *w, int timeout_ms);  /* 0 ok */

/* ---- interactive app ---- */
enum { ESC_NONE, ESC_GOT_ESC, ESC_CSI, ESC_STRING, ESC_STRING_ESC, ESC_ONE_MORE };
enum { ST_DISCONNECTED, ST_HANDSHAKE, ST_LIVE };

struct app {
    struct client_opts o;
    struct conn c;
    int      st;
    int      quit;
    int      ever_welcomed;
    int      in_replay;
    int      info_pending;
    /* what the server told us */
    int      dev_state;
    int      is_ctrl;
    uint32_t baud;
    uint32_t viewers;
    uint32_t ctrl_idle;
    uint64_t rx_bytes;
    char     ctrl_name[BN_NAME_LEN];
    char     dev_path[128];
    /* terminal / ui */
    int      rows, cols;
    int      ts_mode, hex_mode;
    int      at_bol;
    FILE    *logf;
    char     logname[256];
    int      esc_pending;         /* Ctrl-] pressed, waiting for the command key */
    int      esc_state;           /* ANSI filter state for device output (ui.c) */
    char     esc_buf[32];
    size_t   esc_len;
    int      cmd_mode;
    char     cmd[128];
    size_t   cmd_len;
    char     flash[200];
    int      flash_color;
    uint64_t flash_until;
    uint64_t last_status_draw;
    /* input batching (controller keystrokes, §5.4) */
    uint8_t  ibuf[64];
    size_t   ilen;
    uint64_t iflush_at;
    /* reconnect */
    int      backoff_idx;
    uint64_t retry_at;
};

/* session.c */
int  app_run(struct app *a);
int  app_send(struct app *a, uint8_t type, const void *p, uint32_t len);
void app_flush_input(struct app *a);

/* term.c */
int  term_init(void);
void term_restore(void);
void term_size(int *rows, int *cols);
void term_set_region(int rows);
void ob_put(const void *p, size_t n);
void ob_str(const char *s);
void ob_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ob_flush(void);

/* ui.c */
enum { FL_INFO, FL_OK, FL_WARN, FL_ERR };
void ui_init(struct app *a);
void ui_resize(struct app *a);
void ui_data(struct app *a, const uint8_t *d, size_t n, uint64_t ts_ns);
void ui_separator(struct app *a, const char *text);
void ui_note(struct app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_info_box(struct app *a, const char *kv, size_t len);
void ui_help(struct app *a);
void ui_status(struct app *a);
void ui_flash(struct app *a, int color, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void ui_event(struct app *a, int code, const char *text, uint64_t ts_ns);
int  str_width(const char *s);          /* display columns of UTF-8 text (CJK = 2) */
int  info_box_lines(const char *kv, size_t len, int width, char lines[][256], int max);

/* cmd.c */
void cmd_keys(struct app *a, const uint8_t *k, size_t n);
void cmd_exec(struct app *a, const char *line);
void log_toggle(struct app *a, const char *file);

/* subcmd.c */
int sub_info(struct client_opts *o);
int sub_tail(struct client_opts *o);
int sub_watch(struct client_opts *o);
int sub_send(struct client_opts *o, const char *text);

#endif
