/*  carrier_events.c
 *
 *  Event emission for Carrier.
 *
 * *  This file is part of Carrier.
 */

#include "carrier_events.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void carrier_emit(Carrier *c, const CarrierEvent *event)
{
    if (c == NULL || c->event_cb == NULL) {
        return;
    }

    c->event_cb(event, c->event_userdata);
}

void carrier_emit_system(Carrier *c, const char *fmt, ...)
{
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_SYSTEM;
    ev.timestamp = now_ms();

    va_list args;
    va_start(args, fmt);
    vsnprintf(ev.system.text, sizeof(ev.system.text), fmt, args);
    va_end(args);

    carrier_emit(c, &ev);
}

void carrier_emit_error(Carrier *c, const char *cmd, const char *fmt, ...)
{
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_ERROR;
    ev.timestamp = now_ms();

    if (cmd != NULL) {
        snprintf(ev.error.cmd, sizeof(ev.error.cmd), "%s", cmd);
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(ev.error.text, sizeof(ev.error.text), fmt, args);
    va_end(args);

    carrier_emit(c, &ev);
}

void carrier_emit_connected(Carrier *c, int transport)
{
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_CONNECTED;
    ev.timestamp = now_ms();
    ev.connected.transport = transport;

    carrier_emit(c, &ev);
}

void carrier_emit_disconnected(Carrier *c)
{
    CarrierEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = CARRIER_EVENT_DISCONNECTED;
    ev.timestamp = now_ms();

    carrier_emit(c, &ev);
}
