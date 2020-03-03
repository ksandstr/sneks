
#ifndef __SNEKS_BITOPS_H__
#define __SNEKS_BITOPS_H__

#include <stdlib.h>
#include <stdint.h>


/* ia32: returns 31..0 */
#define MSB(x) (sizeof(unsigned long) * 8 - __builtin_clzl((unsigned long)(x)) - 1)

/* ia32: returns 63..0 */
#define MSBLL(x) (sizeof(unsigned long long) * 8 \
	- __builtin_clzll((unsigned long long)(x)) - 1)

static inline int size_to_shift(size_t sz) {
	int msb = MSB(sz);
	return (1 << msb) < sz ? msb + 1 : msb;
}

/* 32-bit comparison-to-mask conversion. zero -> 0, nonzero -> ~0. */
#define MASK32(b) ((int32_t)((b) | -(b)) >> 31)


#endif
