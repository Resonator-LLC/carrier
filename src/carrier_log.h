/*  carrier_log.h
 *
 *  Internal logging helpers for Carrier. All log output is delivered to the
 *  caller-registered callback (see carrier_set_log_callback). The library
 *  itself never writes to stdout or stderr.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef CARRIER_LOG_H
#define CARRIER_LOG_H

#include "carrier.h"
#include "carrier_internal.h"

/* Emit a log record to the registered callback.
 * Gated by level — records above c->log_level are discarded without formatting.
 * No-op when c->log_cb is NULL. Single-threaded (caller's responsibility). */
void carrier_log(Carrier *c, CarrierLogLevel level, const char *tag,
                 const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Convenience macros. Argument order mirrors carrier_log. */
#define CLOG_ERROR(c, tag, ...) carrier_log((c), CARRIER_LOG_ERROR, (tag), __VA_ARGS__)
#define CLOG_WARN(c, tag, ...)  carrier_log((c), CARRIER_LOG_WARN,  (tag), __VA_ARGS__)
#define CLOG_INFO(c, tag, ...)  carrier_log((c), CARRIER_LOG_INFO,  (tag), __VA_ARGS__)
#define CLOG_DEBUG(c, tag, ...) carrier_log((c), CARRIER_LOG_DEBUG, (tag), __VA_ARGS__)

#endif /* CARRIER_LOG_H */
