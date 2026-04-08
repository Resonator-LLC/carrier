/*  carrier_events.h
 *
 *  Internal event helpers for Carrier.
 *
 * *  This file is part of Carrier.
 */

#ifndef CARRIER_EVENTS_H
#define CARRIER_EVENTS_H

#include "carrier.h"
#include "carrier_internal.h"

/* Emit an event to the registered callback. Thread-safe. */
void carrier_emit(Carrier *c, const CarrierEvent *event);

/* Convenience emitters for common events */
void carrier_emit_system(Carrier *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void carrier_emit_error(Carrier *c, const char *cmd, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void carrier_emit_connected(Carrier *c, int transport);
void carrier_emit_disconnected(Carrier *c);

#endif /* CARRIER_EVENTS_H */
