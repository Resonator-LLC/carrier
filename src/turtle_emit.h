/*  turtle_emit.h
 *
 * *  This file is part of Carrier.
 */

#ifndef TURTLE_EMIT_H
#define TURTLE_EMIT_H

#include "carrier.h"
#include <stdio.h>

/* Print Turtle @prefix declarations to output stream */
void turtle_emit_prefixes(FILE *out);

/*
 * Event callback suitable for carrier_set_event_callback().
 * userdata must be a FILE* (stdout, or a FIFO fd).
 */
void turtle_emit_event(const CarrierEvent *ev, void *userdata);

#endif /* TURTLE_EMIT_H */
