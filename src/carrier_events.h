/*  carrier_events.h
 *
 *  Internal event-emission helpers. C-compatible so Turtle-layer TUs
 *  (turtle_parse.c) can call into them. Implementation lives in
 *  carrier_events.cc and does need C++ internals.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef CARRIER_EVENTS_H
#define CARRIER_EVENTS_H

#include "carrier.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enqueue a formatted SYSTEM event. Thread-safe. */
void carrier_emit_system(Carrier *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Enqueue a formatted ERROR event. `command` and `klass` may be NULL.
 * Thread-safe. */
void carrier_emit_error(Carrier *c, const char *command, const char *klass,
                        const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CARRIER_EVENTS_H */
