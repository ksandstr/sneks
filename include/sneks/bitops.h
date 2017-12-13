
#ifndef __SNEKS_BITOPS_H__
#define __SNEKS_BITOPS_H__

#include <stdlib.h>


#define MSB(x) (sizeof((x)) * 8 - __builtin_clzl((x)) - 1)

static inline int size_to_shift(size_t sz) {
	int msb = MSB(sz);
	return (1 << msb) < sz ? msb + 1 : msb;
}


#endif
