/* strscpy(), in a file for hostsuite's benefit. */
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <ccan/endian/endian.h>
#include <ccan/minmax/minmax.h>
#include <sneks/simd.h>

ssize_t strscpy(char *dest, const char *src, size_t sz)
{
	long pos = 0, x;
	for(; pos < sz && ((uintptr_t)(src + pos) & (sizeof(long) - 1)) && src[pos] != '\0'; pos++) dest[pos] = src[pos];
	for(; pos < sz; pos += sizeof(long)) {
		if(!haszero(x = load_lel(src + pos))) *(unsigned long *)(dest + pos) = *(const unsigned long *)(src + pos);
		else {
			int tail = min_t(int, sz - pos, ffsl(zero_mask(x)) / 8 - 1);
			memcpy(dest + pos, src + pos, tail + 1);
			pos += tail;
			break;
		}
	}
	if(pos >= sz && sz > 0) dest[sz - 1] = '\0';
	return pos < sz ? pos : -E2BIG;
}
