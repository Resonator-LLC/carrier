/*  turtle_parse.h
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the GNU General Public License 3.0.
 */

#ifndef TURTLE_PARSE_H
#define TURTLE_PARSE_H

#include "carrier.h"

/*
 * Parse a single-line Turtle command and execute the corresponding
 * carrier_* function.
 *
 * Expected format: [] a carrier:TypeName ; carrier:pred "value" ; ... .
 *
 * Returns 0 on success, -1 on parse error, -2 on unknown command.
 */
int turtle_parse_and_execute(Carrier *c, const char *line);

#endif /* TURTLE_PARSE_H */
