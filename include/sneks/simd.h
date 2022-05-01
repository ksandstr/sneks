#ifndef _SNEKS_SIMD_H
#define _SNEKS_SIMD_H

#include <limits.h>
#include <ccan/endian/endian.h>

#if LONG_BIT == 32
static inline long load_lel(const void *ptr) { return le32_to_cpu(*(const long *)ptr); }
static inline long load_bel(const void *ptr) { return be32_to_cpu(*(const long *)ptr); }
#elif LONG_BIT == 64
static inline long load_lel(const void *ptr) { return le64_to_cpu(*(const long *)ptr); }
static inline long load_bel(const void *ptr) { return be64_to_cpu(*(const long *)ptr); }
#else
#error what accursed evil is this
#endif

static inline unsigned long broadcast_l(long x) { return ~0ul / 255 * x; }

/* returns nonzero if there are zero bytes in @x. see caveat below.
 * via https://graphics.stanford.edu/~seander/bithacks.html#ZeroInWord &c.
 */
static inline unsigned long haszero(unsigned long x) {
	return (x - broadcast_l(0x01)) & ~x & broadcast_l(0x80);
}

/* sets the high bit for every byte in @x that's zero. generally haszero() is
 * used to _detect_ a zero byte in @x, however, we're interested not only in
 * that but its/their _location_, so there's a few more cycles of processing
 * afterward to exclude the 0x0100 -> 0x8080 case.
 */
static inline unsigned long zero_mask(unsigned long x) {
	return haszero(x) & (~(x & broadcast_l(0x01)) << 7);
}

/* same, but for a given byte. */
static inline unsigned long byte_mask(unsigned long x, int c) {
	return zero_mask(x ^ broadcast_l(c));
}

#endif
