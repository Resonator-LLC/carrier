/*  main_cli.c
 *
 *  carrier-cli: Streaming CLI for the Carrier library.
 *  Reads RDF Turtle commands from stdin, emits events to stdout.
 *
 *  Scope follows carrier.h. Pipe mode was removed in the v0.2
 *  vocabulary break; see D12 in arch/jami-migration.md.
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
 * Canonical output format:
 *     <ISO-8601-ms> <LEVEL> [TAG] message
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

static bool parse_log_level(const char *s, CarrierLogLevel *out)
{
    if (s == NULL) return false;
    if (strcasecmp(s, "error") == 0) { *out = CARRIER_LOG_ERROR; return true; }
    if (strcasecmp(s, "warn")  == 0) { *out = CARRIER_LOG_WARN;  return true; }
    if (strcasecmp(s, "info")  == 0) { *out = CARRIER_LOG_INFO;  return true; }
    if (strcasecmp(s, "debug") == 0) { *out = CARRIER_LOG_DEBUG; return true; }
    return false;
}

struct cli_log_ctx {
    FILE            *out;
    CarrierLogLevel  min_level;
};

static void cli_log_sink(const CarrierLogRecord *rec, void *userdata)
{
    struct cli_log_ctx *ctx = (struct cli_log_ctx *)userdata;
    if (ctx == NULL || ctx->out == NULL || rec == NULL) return;

    if ((int)rec->level > (int)ctx->min_level) return;

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
 * Turtle mode main loop
 *
 * Event loop:
 *   poll(stdin, carrier_clock_fd) →
 *     - stdin readable: fgets a line, accumulate, when `.` seen → parse + execute
 *     - clock fd readable: carrier_iterate drains the queue
 *   On timeout (iteration_interval): carrier_iterate anyway (failsafe).
 * ---------------------------------------------------------------------------*/

static int run_turtle_mode(Carrier *c, FILE *in_stream, FILE *out_stream,
                           const char *fifo_in_path)
{
    carrier_set_event_callback(c, turtle_emit_event, out_stream);
    turtle_emit_prefixes(out_stream);

    const int in_fd_initial = fileno(in_stream);
    int in_fd = in_fd_initial;
    const int clock_fd = carrier_clock_fd(c);

    char line_buf[8192];
    char stmt_buf[32768];
    size_t stmt_len = 0;
    stmt_buf[0] = '\0';

    while (g_running) {
        struct pollfd pfds[2];
        int npfds = 0;

        pfds[npfds].fd = in_fd;
        pfds[npfds].events = POLLIN;
        npfds++;

        if (clock_fd >= 0) {
            pfds[npfds].fd = clock_fd;
            pfds[npfds].events = POLLIN;
            npfds++;
        }

        int interval_ms = carrier_iteration_interval(c);
        int poll_ret = poll(pfds, (nfds_t)npfds, interval_ms);

        if (poll_ret < 0 && errno != EINTR) {
            break;
        }

        /* Always iterate on each wake — drains the queue regardless of source. */
        carrier_iterate(c);

        if (poll_ret <= 0) continue;

        /* Input stream. */
        if (pfds[0].revents & POLLIN) {
            if (fgets(line_buf, sizeof(line_buf), in_stream) != NULL) {
                size_t len = strlen(line_buf);

                if (stmt_len + len < sizeof(stmt_buf) - 1) {
                    memcpy(stmt_buf + stmt_len, line_buf, len);
                    stmt_len += len;
                    stmt_buf[stmt_len] = '\0';
                }

                size_t end = stmt_len;
                while (end > 0 && isspace((unsigned char)stmt_buf[end - 1])) end--;

                bool is_complete = (end > 0 && stmt_buf[end - 1] == '.');
                bool is_prefix = (strncmp(stmt_buf, "@prefix", 7) == 0 ||
                                  strncmp(stmt_buf, "@base", 5) == 0);

                if (!is_complete && !is_prefix) {
                    continue;  /* keep accumulating */
                }

                int ret = turtle_parse_and_execute(c, stmt_buf);

                stmt_len = 0;
                stmt_buf[0] = '\0';

                if (ret == 1) {
                    break;  /* Quit */
                }
            } else {
                if (fifo_in_path != NULL) {
                    fclose(in_stream);
                    in_stream = fopen(fifo_in_path, "r");
                    if (in_stream == NULL) break;
                    in_fd = fileno(in_stream);
                } else {
                    break;
                }
            }
        }

        /* Clock fd: nothing further needed — carrier_iterate above drained it. */
        (void) pfds;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------------*/

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d, --data-dir PATH        Jami data dir (default: platform default)\n");
    fprintf(stderr, "  -a, --account ID           Load an existing account by libjami ID\n");
    fprintf(stderr, "      --create-account N     Create a fresh account with display name N\n");
    fprintf(stderr, "      --import-account PATH  Import account from a .gz archive blob\n");
    fprintf(stderr, "      --archive-password PIN Password for the archive (encrypted exports/imports)\n");
    fprintf(stderr, "      --export-account PATH  After loading the account, export its archive to PATH and exit\n");
    fprintf(stderr, "      --remove-account ID    Remove the named account from the data dir and exit\n");
    fprintf(stderr, "      --link-account         Create a new account in linking mode and\n");
    fprintf(stderr, "                             print the import URI (DeviceLinkPin event)\n");
    fprintf(stderr, "      --fifo-in PATH         Read from named pipe instead of stdin\n");
    fprintf(stderr, "      --fifo-out PATH        Write to named pipe instead of stdout\n");
    fprintf(stderr, "      --log LEVEL            error|warn|info|debug (default: error;\n");
    fprintf(stderr, "                             falls back to CARRIER_LOG env var)\n");
    fprintf(stderr, "  -h, --help                 Show this help\n");
    fprintf(stderr, "\nExactly one of --account, --create-account, --import-account, --link-account,\n");
    fprintf(stderr, "or --remove-account is required. --export-account stacks on top of any of the\n");
    fprintf(stderr, "first three to dump the archive after the account is ready.\n");
}

int main(int argc, char **argv)
{
    const char *data_dir       = NULL;
    const char *account_id     = NULL;
    const char *create_display = NULL;
    const char *import_archive = NULL;
    const char *archive_pw     = NULL;
    const char *export_path    = NULL;
    const char *remove_id      = NULL;
    bool        link_account   = false;
    const char *fifo_in_path   = NULL;
    const char *fifo_out_path  = NULL;
    const char *log_level_arg  = NULL;

    static struct option long_opts[] = {
        {"data-dir",         required_argument, 0, 'd'},
        {"account",          required_argument, 0, 'a'},
        {"create-account",   required_argument, 0, 'C'},
        {"import-account",   required_argument, 0, 'M'},
        {"archive-password", required_argument, 0, 'P'},
        {"export-account",   required_argument, 0, 'X'},
        {"remove-account",   required_argument, 0, 'R'},
        {"link-account",     no_argument,       0, 'K'},
        {"fifo-in",          required_argument, 0, 'I'},
        {"fifo-out",         required_argument, 0, 'O'},
        {"log",              required_argument, 0, 'L'},
        {"help",             no_argument,       0, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:a:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'd': data_dir = optarg; break;
            case 'a': account_id = optarg; break;
            case 'C': create_display = optarg; break;
            case 'M': import_archive = optarg; break;
            case 'P': archive_pw = optarg; break;
            case 'X': export_path = optarg; break;
            case 'R': remove_id = optarg; break;
            case 'K': link_account = true; break;
            case 'I': fifo_in_path = optarg; break;
            case 'O': fifo_out_path = optarg; break;
            case 'L': log_level_arg = optarg; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    int mode_count = (account_id ? 1 : 0) +
                     (create_display ? 1 : 0) +
                     (import_archive ? 1 : 0) +
                     (link_account ? 1 : 0) +
                     (remove_id ? 1 : 0);
    if (mode_count != 1) {
        fprintf(stderr, "carrier: exactly one of --account, --create-account, --import-account, --link-account, or --remove-account required\n");
        return 1;
    }

    CarrierLogLevel log_level = CARRIER_LOG_ERROR;
    const char *level_src = log_level_arg;
    if (level_src == NULL) level_src = getenv("CARRIER_LOG");
    if (level_src != NULL && !parse_log_level(level_src, &log_level)) {
        fprintf(stderr, "carrier: invalid log level: %s\n", level_src);
        return 1;
    }

    FILE *in_stream  = stdin;
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

    struct cli_log_ctx log_ctx = { .out = stderr, .min_level = log_level };

    Carrier *c = carrier_new(data_dir, cli_log_sink, &log_ctx);
    if (c == NULL) {
        fprintf(stderr, "Failed to initialize Carrier\n");
        return 1;
    }
    carrier_set_log_level(c, log_level);

    /* Install the Turtle emitter up-front so AccountError / AccountReady /
     * AccountArchiveReady events fired during the synchronous create / import
     * / export paths still reach stdout — otherwise a failed --import-account
     * would exit before carrier_iterate ever runs and the AccountError that
     * carries the cause token would be dropped on the floor. */
    carrier_set_event_callback(c, turtle_emit_event, out_stream);
    turtle_emit_prefixes(out_stream);

    /* --remove-account is a one-shot maintenance op — tear down and exit. */
    if (remove_id) {
        int rrc = carrier_remove_account(c, remove_id);
        if (rrc != 0) {
            fprintf(stderr, "carrier: carrier_remove_account(%s) failed\n", remove_id);
        } else {
            fprintf(stderr, "carrier: removed account %s\n", remove_id);
        }
        carrier_iterate(c);
        carrier_free(c);
        return rrc == 0 ? 0 : 1;
    }

    char loaded_id[CARRIER_ACCOUNT_ID_LEN];
    loaded_id[0] = '\0';

    if (create_display) {
        /* Pass archive_pw through to create_account so the freshly-minted
         * keystore is encrypted with the same password the caller intends
         * to use for --export-account / future imports. libjami's
         * exportToFile needs the keystore password to decrypt the source
         * before re-encrypting the destination. */
        if (carrier_create_account(c, create_display, NULL, archive_pw, loaded_id) != 0) {
            fprintf(stderr, "carrier: carrier_create_account failed\n");
            carrier_iterate(c);
            carrier_free(c);
            return 1;
        }
        fprintf(stderr, "carrier: created account %s\n", loaded_id);
    } else if (import_archive) {
        if (carrier_create_account(c, NULL, import_archive, archive_pw, loaded_id) != 0) {
            fprintf(stderr, "carrier: carrier_create_account(import) failed\n");
            carrier_iterate(c);
            carrier_free(c);
            return 1;
        }
        fprintf(stderr, "carrier: imported account %s from %s\n", loaded_id, import_archive);
    } else if (link_account) {
        if (carrier_create_linking_account(c, loaded_id) != 0) {
            fprintf(stderr, "carrier: carrier_create_linking_account failed\n");
            carrier_iterate(c);
            carrier_free(c);
            return 1;
        }
        fprintf(stderr, "carrier: created linking-mode account %s\n", loaded_id);
    } else {
        if (carrier_load_account(c, account_id) != 0) {
            fprintf(stderr, "carrier: carrier_load_account(%s) failed\n", account_id);
            carrier_iterate(c);
            carrier_free(c);
            return 1;
        }
        snprintf(loaded_id, sizeof(loaded_id), "%s", account_id);
        fprintf(stderr, "carrier: loaded account %s\n", loaded_id);
    }

    /* --export-account stacks on the above: dump the archive, drain the
     * resulting AccountArchiveReady through the turtle emitter, then exit
     * without entering the Turtle-stdin loop. */
    if (export_path) {
        int xrc = carrier_export_account(c, loaded_id, export_path, archive_pw);
        carrier_iterate(c);  /* drain AccountArchiveReady or AccountError */
        carrier_free(c);
        if (fifo_in_path  && in_stream)  fclose(in_stream);
        if (fifo_out_path && out_stream) fclose(out_stream);
        return xrc == 0 ? 0 : 1;
    }

    int rc = run_turtle_mode(c, in_stream, out_stream, fifo_in_path);

    carrier_free(c);

    if (fifo_in_path  && in_stream)  fclose(in_stream);
    if (fifo_out_path && out_stream) fclose(out_stream);

    return rc;
}
