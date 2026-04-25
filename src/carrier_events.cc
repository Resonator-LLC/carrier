/*  carrier_events.cc
 *
 *  Internal event-emission helpers. Thin wrappers over carrier_push_event
 *  that format a body into a std::string, attach it to the QueuedEvent's
 *  backing storage, and hand off. Thread-safe.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier_events.h"
#include "carrier_internal.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace {

std::string vformat(const char *fmt, std::va_list args)
{
    std::va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);

    if (n <= 0) return {};

    std::string out(static_cast<std::size_t>(n), '\0');
    std::vsnprintf(out.data(), out.size() + 1, fmt, args);
    return out;
}

void copy_fixed(char *dst, std::size_t cap, const char *src)
{
    if (cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    const std::size_t len = std::strlen(src);
    const std::size_t n = (len < cap - 1) ? len : cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

} /* anonymous namespace */

extern "C" void carrier_emit_system(Carrier *c, const char *fmt, ...)
{
    if (!c) return;

    std::va_list args;
    va_start(args, fmt);
    std::string text = vformat(fmt, args);
    va_end(args);

    QueuedEvent qe;
    qe.ev.type = CARRIER_EVENT_SYSTEM;
    qe.ev.timestamp = carrier_now_ms();
    qe.message_text = std::move(text);
    qe.ev.system.text = qe.message_text.c_str();

    carrier_push_event(c, std::move(qe));
}

extern "C" void carrier_emit_error(Carrier *c, const char *command, const char *klass,
                                   const char *fmt, ...)
{
    if (!c) return;

    std::va_list args;
    va_start(args, fmt);
    std::string text = vformat(fmt, args);
    va_end(args);

    QueuedEvent qe;
    qe.ev.type = CARRIER_EVENT_ERROR;
    qe.ev.timestamp = carrier_now_ms();
    copy_fixed(qe.ev.error.command, sizeof(qe.ev.error.command), command);
    copy_fixed(qe.ev.error.class_, sizeof(qe.ev.error.class_), klass);
    qe.message_text = std::move(text);
    qe.ev.error.text = qe.message_text.c_str();

    carrier_push_event(c, std::move(qe));
}
