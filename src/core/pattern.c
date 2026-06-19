// SPDX-License-Identifier: GPL-3.0-or-later
#include "pattern.h"

/* splitmix64 — fast, well-distributed 64-bit mixing function. */
static inline uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/*
 * The pattern value for the aligned 8-byte word that starts at absolute byte
 * position `word_base` (a multiple of 8). It depends on both the seed and the
 * absolute offset, so identical content at two different offsets cannot match.
 */
static inline uint64_t word_value(uint64_t seed, uint64_t word_base)
{
    return splitmix64(seed ^ (word_base * 0x100000001B3ULL));
}

void pattern_fill(void *buf, size_t len, uint64_t seed, uint64_t offset)
{
    unsigned char *out = (unsigned char *)buf;
    uint64_t pos = offset;
    size_t i = 0;

    while (i < len) {
        uint64_t word_base = pos & ~(uint64_t)7;
        uint64_t v = word_value(seed, word_base);
        unsigned shift = (unsigned)(pos & 7);

        /* Emit bytes from this word until we cross into the next word or run out. */
        while (shift < 8 && i < len) {
            out[i] = (unsigned char)((v >> (shift * 8)) & 0xff);
            i++;
            pos++;
            shift++;
        }
    }
}

long pattern_verify(const void *buf, size_t len, uint64_t seed, uint64_t offset)
{
    const unsigned char *in = (const unsigned char *)buf;
    uint64_t pos = offset;
    size_t i = 0;

    while (i < len) {
        uint64_t word_base = pos & ~(uint64_t)7;
        uint64_t v = word_value(seed, word_base);
        unsigned shift = (unsigned)(pos & 7);

        while (shift < 8 && i < len) {
            unsigned char expect = (unsigned char)((v >> (shift * 8)) & 0xff);
            if (in[i] != expect)
                return (long)i;
            i++;
            pos++;
            shift++;
        }
    }
    return -1;
}
