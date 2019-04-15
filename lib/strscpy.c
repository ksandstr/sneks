
/* strscpy(), split off into a file for hostsuite's benefit. */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ccan/endian/endian.h>
#include <ccan/minmax/minmax.h>


/* FIXME: move these into a header for string.c and strscpy.c both */

static inline unsigned long haszero(unsigned long x) {
	return (x - 0x01010101ul) & ~x & 0x80808080ul;
}

static inline unsigned long zero_mask(unsigned long x) {
	return haszero(x) & (~(x & 0x01010101ul) << 7);
}

static inline unsigned long byte_mask(unsigned long x, int c) {
	unsigned long ret = zero_mask(x ^ (~0ul / 255 * c));
#ifdef DEBUG_ME_HARDER
	unsigned long t = ret;
	while(t != 0) {
		int p = ffsl(t) / 8 - 1;
		assert(((x >> (p * 8)) & 0xff) == c);
		t &= ~(0xfful << (p * 8));
	}
#endif
	return ret;
}

static inline int until_page(const void *ptr) {
	return 0x1000 - ((uintptr_t)ptr & 0xfff);
}


int strscpy(char *dest, const char *src, size_t n)
{
	size_t pos = 0;

	if(n == 0) return -E2BIG;
	if(labs(dest - src) < sizeof(uintptr_t)) goto rest;	/* , you pervert */

	/* again with the usual old WAAT: one loop per page boundary, inner loops
	 * for words and trailing bytes.
	 *
	 * NOTE: could try to align @src for the first segment, if it already
	 * isn't. unaligned loads are typically slower than unaligned stores (when
	 * there's any difference).
	 */
	while(pos < n) {
		int bytes = min_t(int, n - pos,
				min(until_page(&dest[pos]), until_page(&src[pos]))),
			words = bytes / sizeof(uintptr_t);
		assert(bytes > 0);
		for(; words > 0; words--, pos += sizeof(uintptr_t)) {
			uintptr_t x = LE32_TO_CPU(*(const uintptr_t *)&src[pos]);
			if(haszero(x) == 0) {
				*(uintptr_t *)&dest[pos] = CPU_TO_LE32(x);
			} else {
				uintptr_t z = zero_mask(x);
				*(uintptr_t *)&dest[pos] = CPU_TO_LE32(x & ~((z >> 7) * 0xff));
				pos += ffsl(z) / 8 - 1;
				assert(src[pos] == '\0');
				goto end;
			}
		}
		assert(pos <= n);

		for(int tail = bytes % sizeof(uintptr_t); tail > 0; pos++, tail--) {
			assert(pos < n);
			if(src[pos] == '\0') goto end;
			dest[pos] = src[pos];
		}
		assert(pos <= n);
	}

rest:
	for(; pos < n && src[pos] != '\0'; pos++) dest[pos] = src[pos];
	if(pos >= n) {
		dest[n - 1] = '\0';
		return -E2BIG;
	} else {
end:
		dest[pos] = '\0';
		assert(strcmp(src, dest) == 0 || labs(src - dest) < pos + 1);
		return pos;
	}
}
