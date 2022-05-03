#ifndef __SNEKS_BITOPS_H__
#define __SNEKS_BITOPS_H__

#include <limits.h>
#include <stddef.h>

/* returns LONG_BIT - 1 .. 0 */
#define MSBL(x) (LONG_BIT - __builtin_clzl((unsigned long)(x)) - 1)
/* returns ULLONG_BIT - 1 .. 0 */
#define MSBLL(x) (sizeof(unsigned long long) * 8 - __builtin_clzll((unsigned long long)(x)) - 1)
/* returns WORD_BIT - 1 .. 0 */
#define MSB(x) MSBL((int)(x))

static inline int size_to_shift(size_t sz) {
	int msb = MSBL(sz);
	return (1 << msb) < sz ? msb + 1 : msb;
}

/* zero -> 0, nonzero -> ~0. */
#define SIGNED_MASK(b) ((long)((b) | -(b)) >> (LONG_BIT - 1))
#define MASK32(b) SIGNED_MASK((int)(b))

#endif
