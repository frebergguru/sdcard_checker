// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Deterministic pattern generation and verification.
 *
 * The pattern for any byte is a pure function of (seed, absolute_offset), so
 * a card's contents can be regenerated and checked without storing them. Each
 * 8-byte word also encodes its own absolute offset, which makes address
 * aliasing (a fake card folding high addresses onto low ones) detectable: a
 * block read at offset X that contains the word stamped for offset Y != X
 * proves the address space wrapped.
 */
#ifndef SDCHECK_PATTERN_H
#define SDCHECK_PATTERN_H

#include <stdint.h>
#include <stddef.h>

/* Fill buf[0..len) with the pattern for the byte range [offset, offset+len). */
void pattern_fill(void *buf, size_t len, uint64_t seed, uint64_t offset);

/*
 * Verify that buf[0..len) matches the expected pattern for [offset, offset+len).
 * Returns -1 if the whole buffer matches, otherwise the index (0..len) of the
 * first mismatching byte.
 */
long pattern_verify(const void *buf, size_t len, uint64_t seed, uint64_t offset);

#endif /* SDCHECK_PATTERN_H */
