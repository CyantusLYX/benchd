/*
 * main.c — `bench` client entry point: options, then interactive TUI or a subcommand.
 */
#include "client.h"
#include "bench_log.h"
#include "bench_util.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f)
{
    fprintf(f,
        "usage: bench [options] <host[:port]>                  interactive console (default port 9000)\n"
        "       bench info  <host[:port]> [--raw]              print system info and exit\n"
        "       bench tail  <host[:port]> [-n 16K]             print history and exit\n"
        "       bench watch <host[:port]> [--name N] [--duration SEC] [--out FILE] [-n SIZE]\n"
        "       bench send  <host[:port]> \"reset\\n\" [--wait MS]  take, send, release (exit 2 if refused)\n"
        "options:\n"
        "      --name NAME      name shown to others (default $USER@hostname)\n"
        "  -n SIZE              history to replay on join (K/M suffix; 0 = none), default 8K\n"
        "      --no-reconnect   exit instead of reconnecting when the server goes away\n"
        "  -v                   more logging (interactive mode logs to /tmp/bench-<pid>.log)\n"
        "      --unsafe LIST    disable client robustness: term-restore, reconnect, readn, validate, all\n"
        "  -h, --help\n"
        "keys: Ctrl-] then t take, r release, i info, s timestamps, h hex, l log, : command, ? help, q quit\n");
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        { "name", required_argument, 0, 1 }, { "no-reconnect", no_argument, 0, 2 },
        { "unsafe", required_argument, 0, 3 }, { "duration", required_argument, 0, 4 },
        { "out", required_argument, 0, 5 }, { "wait", required_argument, 0, 6 },
        { "raw", no_argument, 0, 7 }, { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
    struct app *a = calloc(1, sizeof *a);
    struct client_opts *o = &a->o;
    o->reconnect = 1;
    o->replay = 8 * 1024;
    uint32_t unsafe = 0;
    int c;

    while ((c = getopt_long(argc, argv, "n:vh", lo, NULL)) != -1) {
        switch (c) {
        case 1: bench_strlcpy(o->name, optarg, sizeof o->name); break;
        case 2: o->reconnect = 0; break;
        case 3:
            if (bench_unsafe_parse(optarg, &unsafe) < 0) {
                fprintf(stderr, "bench: unknown name in --unsafe '%s'\n", optarg);
                return 1;
            }
            break;
        case 4: o->duration = atof(optarg); break;
        case 5: o->out = optarg; break;
        case 6: o->wait_ms = (unsigned)strtoul(optarg, NULL, 10); break;
        case 7: o->raw = 1; break;
        case 'n':
            if (parse_size(optarg, &o->replay) < 0 || o->replay > 0xFFFFFFFFull) {
                fprintf(stderr, "bench: bad size '%s'\n", optarg);
                return 1;
            }
            o->replay_set = 1;
            break;
        case 'v': o->verbose++; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 1;
        }
    }
    bench_unsafe_mask = unsafe;
    if (!UNSAFE(R_SIGPIPE))
        signal(SIGPIPE, SIG_IGN);      /* R2: a write to a dead peer must return EPIPE, not kill us */
    else
        signal(SIGPIPE, SIG_DFL);      /* unsafe: default action even if the launcher ignored SIGPIPE */

    const char *sub = NULL;
    int ai = optind;
    if (ai < argc && (!strcmp(argv[ai], "info") || !strcmp(argv[ai], "tail") ||
                      !strcmp(argv[ai], "watch") || !strcmp(argv[ai], "send")))
        sub = argv[ai++];
    if (ai >= argc) {
        usage(stderr);
        return 1;
    }
    if (parse_hostport(argv[ai], o->host, sizeof o->host, &o->port, BN_DEFAULT_PORT) < 0) {
        fprintf(stderr, "bench: bad address '%s'\n", argv[ai]);
        return 1;
    }
    ai++;
    if (!o->name[0]) {
        char host[64] = "host";
        gethostname(host, sizeof host);
        host[sizeof host - 1] = 0;
        const char *u = getenv("USER");
        snprintf(o->name, sizeof o->name, "%.15s@%.15s", u ? u : "user", host);
    }
    sanitize_name(o->name, sizeof o->name);

    bench_log_set_role("bench");
    bench_log_init_level(o->verbose);

    if (sub) {
        if (!strcmp(sub, "info")) return sub_info(o);
        if (!strcmp(sub, "tail")) return sub_tail(o);
        if (!strcmp(sub, "watch")) return sub_watch(o);
        if (ai >= argc) {
            fprintf(stderr, "bench send: missing text\n");
            return 1;
        }
        return sub_send(o, argv[ai]);
    }

    /* interactive: keep the log away from the TUI */
    char lp[64];
    snprintf(lp, sizeof lp, "/tmp/bench-%d.log", (int)getpid());
    bench_log_open_file(lp);
    bench_log_set_stderr(0);
    LOGI("bench %s starting, server %s:%u as %s", BENCH_VERSION, o->host, o->port, o->name);
    return app_run(a);
}
