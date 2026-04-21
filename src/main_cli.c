/*  main_cli.c
 *
 *  carrier-cli: Streaming CLI for the Carrier library.
 *  Reads RDF Turtle commands from stdin, emits events to stdout.
 *  Supports --pipe mode for raw bidirectional data transfer.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier.h"
#include "turtle_emit.h"
#include "turtle_parse.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---------------------------------------------------------------------------
 * Log level parsing + default stderr sink
 *
 * Canonical output format (see arch/log.md and the DEBUG logging proposal):
 *     <ISO-8601-ms> <LEVEL> [TAG] message
 *
 * Example:
 *     2026-04-21T17:42:03.412Z DEBUG [DHT] bootstrap host=tox.plastiras.org ...
 * ---------------------------------------------------------------------------*/

static const char *log_level_str(CarrierLogLevel level)
{
    switch (level) {
        case CARRIER_LOG_ERROR: return "ERROR";
        case CARRIER_LOG_WARN:  return "WARN";
        case CARRIER_LOG_INFO:  return "INFO";
        case CARRIER_LOG_DEBUG: return "DEBUG";
        default:                return "?";
    }
}

/* Parse a level name. Returns true on success; *out untouched on failure. */
static bool parse_log_level(const char *s, CarrierLogLevel *out)
{
    if (s == NULL) return false;
    if (strcasecmp(s, "error") == 0) { *out = CARRIER_LOG_ERROR; return true; }
    if (strcasecmp(s, "warn")  == 0) { *out = CARRIER_LOG_WARN;  return true; }
    if (strcasecmp(s, "info")  == 0) { *out = CARRIER_LOG_INFO;  return true; }
    if (strcasecmp(s, "debug") == 0) { *out = CARRIER_LOG_DEBUG; return true; }
    return false;
}

/* Userdata handed to cli_log_sink by main_cli so the sink can filter
 * records above the user-requested level. The library delivers everything
 * at DEBUG; this sink is the gate for carrier-cli's stderr. */
struct cli_log_ctx {
    FILE            *out;
    CarrierLogLevel  min_level;  /* records with level > min_level are dropped */
};

static void cli_log_sink(const CarrierLogRecord *rec, void *userdata)
{
    struct cli_log_ctx *ctx = (struct cli_log_ctx *)userdata;
    if (ctx == NULL || ctx->out == NULL || rec == NULL) return;

    if ((int)rec->level > (int)ctx->min_level) return;

    /* Format timestamp as ISO-8601 UTC with millisecond precision. */
    time_t secs = (time_t)(rec->timestamp_ms / 1000);
    int ms = (int)(rec->timestamp_ms % 1000);
    struct tm tm_buf;
    gmtime_r(&secs, &tm_buf);

    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    fprintf(ctx->out, "%s.%03dZ %-5s [%s] %s\n",
            tsbuf, ms, log_level_str(rec->level),
            rec->tag[0] ? rec->tag : "?", rec->message);
    fflush(ctx->out);
}

/* ---------------------------------------------------------------------------
 * Pipe mode event handler
 * ---------------------------------------------------------------------------*/

static void pipe_event_handler(const CarrierEvent *ev, void *userdata)
{
    FILE *out = (FILE *)userdata;

    switch (ev->type) {
        case CARRIER_EVENT_PIPE_DATA:
            fwrite(ev->pipe_data.data, 1, ev->pipe_data.len, out);
            fflush(out);
            break;

        case CARRIER_EVENT_PIPE_EOF:
        case CARRIER_EVENT_FRIEND_OFFLINE:
            g_running = 0;
            break;

        case CARRIER_EVENT_PIPE:
            /* Remote opened pipe to us — ready */
            break;

        case CARRIER_EVENT_CONNECTED:
        case CARRIER_EVENT_DISCONNECTED:
            /* Silently handle connection changes in pipe mode */
            break;

        case CARRIER_EVENT_ERROR:
            fprintf(stderr, "carrier: error: %s: %s\n",
                    ev->error.cmd, ev->error.text);
            break;

        case CARRIER_EVENT_SYSTEM:
            fprintf(stderr, "carrier: %s\n", ev->system.text);
            break;

        default:
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Pipe mode main loop
 * ---------------------------------------------------------------------------*/

static int run_pipe_mode(Carrier *c, uint32_t friend_id,
                         FILE *in_stream, FILE *out_stream)
{
    carrier_set_event_callback(c, pipe_event_handler, out_stream);

    /* Wait for friend to come online */
    fprintf(stderr, "carrier: waiting for friend #%u to come online...\n", friend_id);

    bool friend_online = false;

    while (g_running && !friend_online) {
        carrier_iterate(c);

        /* Check if pipe_open succeeds as a connectivity test */
        int ret = carrier_pipe_open(c, friend_id);

        if (ret == 0) {
            friend_online = true;
            fprintf(stderr, "carrier: friend #%u is online, pipe open\n", friend_id);
        } else {
            usleep((useconds_t)(carrier_iteration_interval(c) * 1000));
        }
    }

    if (!g_running) {
        return 1;
    }

    int in_fd = fileno(in_stream);
    bool stdin_eof = false;

    /* Pending buffer: data read from stdin but not yet fully sent */
    uint8_t buf[1372];  /* Match max lossless payload (1373 - 1 tag byte) */
    size_t buf_len = 0;
    size_t buf_off = 0;

    while (g_running) {
        carrier_iterate(c);

        /* Try to send pending data first */
        while (buf_off < buf_len && g_running) {
            int written = carrier_pipe_write(c, friend_id,
                                             buf + buf_off,
                                             buf_len - buf_off);

            if (written > 0) {
                buf_off += (size_t)written;
            } else {
                /* Queue full — iterate to flush, then retry */
                carrier_iterate(c);
                usleep(5000);  /* 5ms */
                break;  /* Try again next loop iteration */
            }
        }

        /* If all pending data sent, reset buffer and read more */
        if (buf_off >= buf_len) {
            buf_off = 0;
            buf_len = 0;

            if (!stdin_eof) {
                struct pollfd pfd = { .fd = in_fd, .events = POLLIN };
                int ret = poll(&pfd, 1, 1);  /* 1ms poll — don't block long */

                if (ret > 0 && (pfd.revents & POLLIN)) {
                    ssize_t n = read(in_fd, buf, sizeof(buf));

                    if (n > 0) {
                        buf_len = (size_t)n;
                    } else {
                        /* EOF — flush then send close */
                        carrier_pipe_close(c, friend_id);
                        stdin_eof = true;
                        fprintf(stderr, "carrier: stdin EOF, sent pipe close\n");
                    }
                }
            }
        }

        /* If stdin is done and buffer drained, just iterate until remote EOF */
        if (stdin_eof && buf_off >= buf_len) {
            usleep((useconds_t)(carrier_iteration_interval(c) * 1000));
        }
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Turtle mode main loop
 * ---------------------------------------------------------------------------*/

static int run_turtle_mode(Carrier *c, FILE *in_stream, FILE *out_stream,
                           const char *fifo_in_path)
{
    carrier_set_event_callback(c, turtle_emit_event, out_stream);
    turtle_emit_prefixes(out_stream);
    carrier_get_id(c);
    carrier_get_dht_info(c);

    int in_fd = fileno(in_stream);
    char line_buf[8192];

    /* Accumulation buffer for multi-line Turtle statements */
    char stmt_buf[32768];
    size_t stmt_len = 0;
    stmt_buf[0] = '\0';

    while (g_running) {
        struct pollfd pfd = {
            .fd = in_fd,
            .events = POLLIN,
        };

        int interval_ms = carrier_iteration_interval(c);
        int poll_ret = poll(&pfd, 1, interval_ms);

        if (poll_ret > 0 && (pfd.revents & POLLIN)) {
            if (fgets(line_buf, sizeof(line_buf), in_stream) != NULL) {
                size_t len = strlen(line_buf);

                /* Accumulate lines into stmt_buf */
                if (stmt_len + len < sizeof(stmt_buf) - 1) {
                    memcpy(stmt_buf + stmt_len, line_buf, len);
                    stmt_len += len;
                    stmt_buf[stmt_len] = '\0';
                }

                /* Check if we have a complete statement (ends with '.') */
                /* Trim trailing whitespace to find the last real char */
                size_t end = stmt_len;
                while (end > 0 && isspace((unsigned char)stmt_buf[end - 1])) end--;

                bool is_complete = (end > 0 && stmt_buf[end - 1] == '.');
                bool is_prefix = (strncmp(stmt_buf, "@prefix", 7) == 0 ||
                                  strncmp(stmt_buf, "@base", 5) == 0);

                if (!is_complete && !is_prefix) {
                    /* Incomplete statement — keep accumulating */
                    carrier_iterate(c);
                    continue;
                }

                int ret = turtle_parse_and_execute(c, stmt_buf);

                /* Reset accumulation buffer */
                stmt_len = 0;
                stmt_buf[0] = '\0';

                if (ret == 1) {
                    break;  /* Quit */
                }

                if (ret == 2) {
                    /* Pipe command — extract friendId and switch to pipe mode */
                    /* Re-parse to get friendId (simple extraction) */
                    const char *fid_str = strstr(line_buf, "friendId");

                    if (fid_str != NULL) {
                        uint32_t fid = 0;
                        const char *p = fid_str + 8; /* skip "friendId" */

                        while (*p && (*p < '0' || *p > '9')) p++;

                        fid = (uint32_t)strtoul(p, NULL, 10);
                        return run_pipe_mode(c, fid, in_stream, out_stream);
                    }
                }
            } else {
                if (fifo_in_path != NULL) {
                    fclose(in_stream);
                    in_stream = fopen(fifo_in_path, "r");

                    if (in_stream == NULL) {
                        break;
                    }

                    in_fd = fileno(in_stream);
                } else {
                    break;
                }
            }
        } else if (poll_ret < 0 && errno != EINTR) {
            break;
        }

        carrier_iterate(c);
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------------*/

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n\n", prog);
    fprintf(stderr, "Modes:\n");
    fprintf(stderr, "  (default)             Turtle protocol on stdin/stdout\n");
    fprintf(stderr, "  --pipe FRIEND_ID      Raw bidirectional data pipe to friend\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -p, --profile PATH    Tox profile file (default: carrier_profile.tox)\n");
    fprintf(stderr, "  -c, --config PATH     Config file\n");
    fprintf(stderr, "  -n, --nodes PATH      DHT nodes JSON file\n");
    fprintf(stderr, "  --fifo-in PATH        Read from named pipe instead of stdin\n");
    fprintf(stderr, "  --fifo-out PATH       Write to named pipe instead of stdout\n");
    fprintf(stderr, "  --log LEVEL           Log level: error|warn|info|debug (default: error).\n"
                    "                        Falls back to CARRIER_LOG env var if unset.\n");
    fprintf(stderr, "  -h, --help            Show this help\n");
}

int main(int argc, char **argv)
{
    const char *profile_path = NULL;
    const char *config_path = NULL;
    const char *nodes_path = NULL;
    const char *fifo_in_path = NULL;
    const char *fifo_out_path = NULL;
    const char *log_level_arg = NULL;
    int pipe_friend = -1;

    static struct option long_opts[] = {
        {"profile",  required_argument, 0, 'p'},
        {"config",   required_argument, 0, 'c'},
        {"nodes",    required_argument, 0, 'n'},
        {"pipe",     required_argument, 0, 'P'},
        {"fifo-in",  required_argument, 0, 'I'},
        {"fifo-out", required_argument, 0, 'O'},
        {"log",      required_argument, 0, 'L'},
        {"help",     no_argument,       0, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;

    while ((opt = getopt_long(argc, argv, "p:c:n:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p': profile_path = optarg; break;
            case 'c': config_path = optarg; break;
            case 'n': nodes_path = optarg; break;
            case 'P': pipe_friend = atoi(optarg); break;
            case 'I': fifo_in_path = optarg; break;
            case 'O': fifo_out_path = optarg; break;
            case 'L': log_level_arg = optarg; break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Resolve log level: --log flag wins; CARRIER_LOG env var is fallback;
     * default is ERROR. */
    CarrierLogLevel log_level = CARRIER_LOG_ERROR;
    const char *level_src = log_level_arg;
    if (level_src == NULL) {
        level_src = getenv("CARRIER_LOG");
    }
    if (level_src != NULL && !parse_log_level(level_src, &log_level)) {
        fprintf(stderr, "carrier: invalid log level: %s\n", level_src);
        return 1;
    }

    /* Set up I/O */
    FILE *in_stream = stdin;
    FILE *out_stream = stdout;

    if (fifo_in_path != NULL) {
        in_stream = fopen(fifo_in_path, "r");

        if (in_stream == NULL) {
            fprintf(stderr, "Cannot open FIFO input: %s: %s\n",
                    fifo_in_path, strerror(errno));
            return 1;
        }
    }

    if (fifo_out_path != NULL) {
        out_stream = fopen(fifo_out_path, "w");

        if (out_stream == NULL) {
            fprintf(stderr, "Cannot open FIFO output: %s: %s\n",
                    fifo_out_path, strerror(errno));
            if (fifo_in_path) fclose(in_stream);
            return 1;
        }
    }

    setlinebuf(out_stream);

    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    /* Sink userdata. Must outlive carrier_free(). Stack in main() is fine. */
    struct cli_log_ctx log_ctx = { .out = stderr, .min_level = log_level };

    Carrier *c = carrier_new(profile_path, config_path, nodes_path,
                             cli_log_sink, &log_ctx);

    if (c == NULL) {
        fprintf(stderr, "Failed to initialize Carrier\n");
        return 1;
    }

    int rc;

    if (pipe_friend >= 0) {
        rc = run_pipe_mode(c, (uint32_t)pipe_friend, in_stream, out_stream);
    } else {
        rc = run_turtle_mode(c, in_stream, out_stream, fifo_in_path);
    }

    carrier_free(c);

    if (fifo_in_path && in_stream) {
        fclose(in_stream);
    }

    if (fifo_out_path && out_stream) {
        fclose(out_stream);
    }

    return rc;
}
