/* these routines do cool word-at-a-time string processing and as such may
 * access extra bytes after end of string or end of buffer, if those bytes do
 * not straddle a page boundary. that's POSIX cromulent, but don't locate
 * volatiles there.
 */
#define IN_LIB_IMPL
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>
#include <sneks/bitops.h>
#include <sneks/simd.h>

#if defined(__i386__)
/* via uClibc */
static void *memcpy_forward(void *restrict dst, const void *restrict src, size_t len)
{
	long d0, d1, d2;
	if(__builtin_constant_p(len) && (len & 3) == 0) {
		asm volatile ("rep; movsl\n"
			: "=&c" (d0), "=&D" (d1), "=&S" (d2)
			: "0" (len / 4), "g" (len), "1" (dst), "2" (src)
			: "memory");
	} else {
		asm volatile (
			"rep; movsl\n"
			"movl %4, %%ecx\n"
			"andl $3, %%ecx\n"
			/* avoids rep;movsb with ecx==0, faster on post-2008 iron */
			"jz 1f\n"
			"rep; movsb\n"
			"1:\n"
			: "=&c" (d0), "=&D" (d1), "=&S" (d2)
			: "0" (len / 4), "g" (len), "1" (dst), "2" (src)
			: "memory");
	}
	return dst;
}
#else
static void *memcpy_forward(void *restrict dst, const void *restrict src, size_t len) {
	void *const start = dst;
	for(; dst - start < (len & ~(sizeof(long) - 1)); dst += sizeof(long), src += sizeof(long)) *(long *restrict)dst = *(const long *restrict)src;
	for(; dst - start < len; dst++, src++) *(uint8_t *restrict)dst = *(const uint8_t *restrict)src;
	return start;
}
#endif

void *memcpy(void *dst, const void *src, size_t len) {
	return memcpy_forward(dst, src, len);
}

static void *memcpy_backward(uint8_t *restrict d, const uint8_t *restrict s, size_t len) {
	uint8_t *start = d;
	for(d += len - 1, s += len - 1; d >= start && ((uintptr_t)d & (sizeof(long) - 1)); *d-- = *s--) /* >:3 rawr i'm a lion */ ;
	for(; d - start >= 0; d -= sizeof(long), s -= sizeof(long)) *(long *restrict)d = *(const long *restrict)s;
	return start;
}

void *memmove(void *dst, const void *src, size_t len) {
	return dst + sizeof(long) <= src || src + len < dst ? memcpy_forward(dst, src, len) : memcpy_backward(dst, src, len);
}

void *memset(void *p, int c, size_t len) {
	void *const start = p;
	for(; p - start < len && ((uintptr_t)p & (sizeof(long) - 1)); p++) *(uint8_t *)p = c;
	for(long w = broadcast_l(c); p - start < len; p += sizeof(long)) *(long *)p = w;
	return start;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
	size_t major = n & ~(sizeof(long) - 1);
	for(size_t i = 0, w = major / sizeof(long); i < w; i++) {
		long a = load_bel(s1 + i * sizeof(long)), b = load_bel(s2 + i * sizeof(long)), c = a - b;
		if(c != 0) return c;
	}
	const uint8_t *a = s1, *b = s2;
	for(size_t i = major; i < n; i++) {
		int c = (int)a[i] - b[i];
		if(c != 0) return c;
	}
	return 0;
}

void *memswap(void *left, void *right, size_t n)
{
	if(left == right || n == 0) return left;
	uint8_t tmp[256], *a = left, *b = right;
	size_t head = n % sizeof tmp;
	if(head > 0) {
		memcpy(tmp, a, head); memcpy(a, b, head); memcpy(b, tmp, head);
		a += head; b += head;
	}
	for(size_t sz = sizeof tmp; a < (uint8_t *)left + n; a += sz, b += sz) {
		memcpy(tmp, a, sz); memcpy(a, b, sz); memcpy(b, tmp, sz);
	}
	return left;
}

void *memchr(const void *ptr, int c, size_t n)
{
	const void *p = ptr;
	for(; p - ptr < n && ((uintptr_t)p & (sizeof(long) - 1)); p++) {
		if(*(const unsigned char *)p == c) return (void *)p;
	}
	for(; p - ptr < n; p += sizeof(long)) {
		long found = byte_mask(load_lel(p), c);
		if(found != 0) {
			void *spot = (void *)p + ffsl(found) / 8 - 1;
			return spot - ptr < n ? spot : NULL;
		}
	}
	return NULL;
}

static inline int until_page(const void *ptr) {
	return 0x1000 - ((uintptr_t)ptr & 0xfff);
}

int strncmp(const char *a, const char *b, size_t max)
{
	if(a == b) return 0;
	/* the word-at-a-time optimization requires that long loads are valid,
	 * i.e. that they don't cross a 4k boundary in memory. so the loop
	 * proceeds in segments that lead up to such a boundary in either operand,
	 * first doing words and then bytes and not caring about fetch alignment.
	 */
	for(size_t pos = 0; pos < max;) {
		int bytes = min_t(int, max - pos, min(until_page(&a[pos]), until_page(&b[pos]))), words = bytes / sizeof(long);
		for(; words > 0; words--, pos += sizeof(long)) {
			long la = load_bel(a + pos), lb = load_bel(b + pos);
			if((haszero(la) | haszero(lb)) == 0) {
				if(la - lb != 0) return la - lb; /* by content */
			} else {
				long za = zero_mask(la), zb = zero_mask(lb), m = ((1lu << MSBL(za | zb)) >> 7) * 0xff;
				m |= m << 8; m |= m << 16;
				long c = (la & m) - (lb & m);
				if(c != 0) return c;		/* by content */
				return (za & m) - (zb & m);	/* by length */
			}
		}
		/* then byte at a time. */
		for(int tail = bytes % sizeof(long); tail > 0; pos++, tail--) {
			int c = (int)a[pos] - b[pos];
			if(c != 0) return c;
			if(a[pos] == '\0') return 0;
		}
	}
	return 0;
}

int strcmp(const char *a, const char *b) {
	return strncmp(a, b, SIZE_MAX);
}

static inline int ascii_tolower(int c) {
	if(c >= 'A' && c <= 'Z') return c - 32; else return c;
}

int strcasecmp(const char *a, const char *b)
{
	for(int i=0; a[i] != '\0' && b[i] != '\0'; i++) {
		int c = ascii_tolower(a[i]) - ascii_tolower(b[i]);
		if(c != 0) return c;
	}
	return 0;
}

char *strndup(const char *str, size_t n)
{
	size_t len = strnlen(str, n);
	char *buf = malloc(len + 1);
	if(buf != NULL) {
		memcpy(buf, str, len);
		buf[len] = '\0';
	}
	return buf;
}

char *strdup(const char *str) {
	return strndup(str, SIZE_MAX);
}

size_t strnlen(const char *str, size_t max)
{
	const char *s = str;
	for(; s - str < max && ((uintptr_t)s & (sizeof(long) - 1)); s++) {
		if(*s == '\0') return s - str;
	}
	for(; s - str < max; s += sizeof(long)) {
		long x = load_lel(s);
		if(haszero(x)) return min_t(size_t, max, s - str + ffsl(zero_mask(x)) / 8 - 1);
	}
	return max;
}

size_t strlen(const char *str) {
	return strnlen(str, SIZE_MAX);
}

char *strncpy(char *dest, const char *src, size_t n) {
	int used = strscpy(dest, src, n);
	if(used >= 0 && used < n) memset(&dest[used], '\0', n - used);
	return dest;
}

char *strcpy(char *dest, const char *src) {
	return memmove(dest, src, strlen(src) + 1);
}

char *strchrnul(const char *s, int c)
{
	for(; (uintptr_t)s & (sizeof(long) - 1); s++) {
		if(*s == c || *s == '\0') return (char *)s;
	}
	for(;; s += sizeof(long)) {
		long x = load_lel(s), found = byte_mask(x, c), zero = zero_mask(x);
		if(found | zero) return (char *)s + ffsl(found | zero) / 8 - 1;
	}
}

char *strchr(const char *s, int c) {
	char *ret = strchrnul(s, c);
	return *ret == '\0' ? NULL : ret;
}

char *strrchr(const char *s, int c) {
	const char *t = s + strlen(s);
	while(*t != c && t != s) t--;
	return t != s ? (char *)t : NULL;
}

char *strstr(const char *haystack, const char *needle)
{
	if(*needle == '\0') return (char *)haystack; /* degenerate case */
	int n_len = 0, diff = 0;
	for(; needle[n_len] != '\0'; n_len++) {
		if(haystack[n_len] == '\0') return NULL; /* #haystack < #needle */
		diff |= needle[n_len] ^ haystack[n_len];
	}
	if(diff == 0) return (char *)haystack; /* full prefix */
	do {
		/* compare up to n_len bytes. return NULL if haystack is shorter than
		 * that. return haystack if needle is found. otherwise, advance until
		 * next instance of first letter in needle.
		 */
		bool found = true;
		for(size_t i=0; i < n_len; i++) {
			if(haystack[i] != needle[i]) {
				if(haystack[i] == '\0') return NULL;
				found = false;
				break;
			}
		}
		if(found) break;
		haystack = strchr(haystack + 1, needle[0]);
	} while(haystack != NULL);
	return (char *)haystack;
}

static char *strscan(const char *s, const char *set, bool accept)
{
	unsigned long present[(UCHAR_MAX + LONG_BIT) / LONG_BIT] = { 0 };
	static_assert(sizeof present * CHAR_BIT == UCHAR_MAX + 1);
	for(int i=0; i < ARRAY_SIZE(present); i++) assert(present[i] == 0);
	for(int i=0; set[i] != '\0'; i++) {
		int c = (unsigned char)set[i];
		present[c / LONG_BIT] |= 1ull << c % LONG_BIT;
	}
	for(; *s != '\0'; s++) {
		int c = (unsigned char)(*s);
		if(accept ^ !!(present[c / LONG_BIT] & (1ull << c % LONG_BIT))) break;
	}
	return (char *)s;
}

char *strpbrk(const char *s, const char *needles) {
	char *r = strscan(s, needles, false);
	return *r == '\0' ? NULL : r;
}

size_t strcspn(const char *s, const char *reject) {
	return strscan(s, reject, false) - s;
}

size_t strspn(const char *s, const char *accept) {
	return strscan(s, accept, true) - s;
}

#undef ffsl
int ffsl(long l) { return __builtin_ffsl(l); }

#undef ffsll
int ffsll(long long ll) { return __builtin_ffsll(ll); }
