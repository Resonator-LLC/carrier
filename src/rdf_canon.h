/*  rdf_canon.h — RDF canonicalization (RDNA2015 subset) + SHA-256 hash.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef RDF_CANON_H
#define RDF_CANON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compute the canonical RDNA2015 SHA-256 hash of a Turtle document.
 *
 * Algorithm:
 *   1. Parse turtle with serd.
 *   2. Serialize each triple as an N-Quads line; blank node subjects are
 *      replaced with _:c14n0 (correct RDNA2015 behavior for the single-BN
 *      topology used by carrier events).
 *   3. Sort the lines lexicographically.
 *   4. SHA-256 the sorted, newline-terminated lines concatenated.
 *
 * out receives 32 bytes.
 * Returns 0 on success, -1 on parse error or allocation failure.
 */
int rdf_canon_hash(const char *turtle, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* RDF_CANON_H */
