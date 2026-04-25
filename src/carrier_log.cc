/*  carrier_log.cc
 *
 *  Log record formatting and callback dispatch.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier_internal.hpp"
#include "carrier_log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

std::int64_t log_now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} /* anonymous namespace */

extern "C" void carrier_log(Carrier *c, CarrierLogLevel level, const char *tag,
                            const char *fmt, ...)
{
    if (!c || !c->log_cb) return;

    /* Enum values: ERROR=0, WARN=1, INFO=2, DEBUG=3.
     * log_level is the highest level the sink accepts — records whose
     * level exceeds it are dropped without formatting. */
    if (static_cast<int>(level) > static_cast<int>(c->log_level)) {
        return;
    }

    CarrierLogRecord rec{};
    rec.level = level;
    rec.timestamp_ms = log_now_ms();

    if (tag) {
        std::snprintf(rec.tag, sizeof(rec.tag), "%s", tag);
    }

    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(rec.message, sizeof(rec.message), fmt, args);
    va_end(args);

    c->log_cb(&rec, c->log_userdata);
}

extern "C" void carrier_set_log_callback(Carrier *c, carrier_log_cb cb, void *userdata)
{
    if (!c) return;
    c->log_cb = cb;
    c->log_userdata = userdata;
}

extern "C" void carrier_set_log_level(Carrier *c, CarrierLogLevel level)
{
    if (!c) return;
    c->log_level = level;
}
