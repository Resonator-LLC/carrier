/*  carrier_log.c
 *
 *  Log record formatting and callback dispatch.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int64_t log_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void carrier_log(Carrier *c, CarrierLogLevel level, const char *tag,
                 const char *fmt, ...)
{
    if (c == NULL || c->log_cb == NULL) {
        return;
    }

    /* Enum values: ERROR=0, WARN=1, INFO=2, DEBUG=3.
     * log_level is the highest level the sink accepts — records whose
     * level exceeds it are dropped without formatting. */
    if ((int)level > (int)c->log_level) {
        return;
    }

    CarrierLogRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.level = level;
    rec.timestamp_ms = log_now_ms();

    if (tag != NULL) {
        snprintf(rec.tag, sizeof(rec.tag), "%s", tag);
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(rec.message, sizeof(rec.message), fmt, args);
    va_end(args);

    c->log_cb(&rec, c->log_userdata);
}

void carrier_set_log_callback(Carrier *c, carrier_log_cb cb, void *userdata)
{
    if (c == NULL) {
        return;
    }

    c->log_cb = cb;
    c->log_userdata = userdata;
}

void carrier_set_log_level(Carrier *c, CarrierLogLevel level)
{
    if (c == NULL) {
        return;
    }

    c->log_level = level;
}
