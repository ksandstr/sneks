
/* string.c from mung; relicensed under GPLv2+ for sneks. */

#define IN_LIB_IMPL

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <ccan/endian/endian.h>
#include <ccan/minmax/minmax.h>

/* TODO: get rid of this */
#include <ukernel/util.h>


/* TODO: make this conditional on those versions to which GCC bug #56888
 * applies.
 */
#pragma GCC optimize ("no-tree-loop-distribute-patterns")

static void *memcpy_forward(void *dst, const void *src, size_t len);


/* returns nonzero if there are zero bytes in @x. see caveat in zero_mask()
 * comment.
 *
 * via https://graphics.stanford.edu/~seander/bithacks.html#ZeroInWord and
 * others.
 */
static inline unsigned long haszero(unsigned long x) {
	return (x - 0x01010101ul) & ~x & 0x80808080ul;
}


/* sets the high bit for every byte in @x that's zero. generally haszero() is
 * used to _detect_ a zero byte in @x, however, we're interested not only in
 * that but its/their _location_, so there's a few more cycles of processing
 * afterward to exclude the 0x0100 -> 0x8080 case.
 */
static inline unsigned long zero_mask(unsigned long x) {
	return haszero(x) & (~(x & 0x01010101ul) << 7);
}


/* same, but for a given byte. */
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


/* compute bytes until end of page from @p. */
static inline int until_page(const void *ptr) {
	/* TODO: make prettier w/ PAGE_SIZE and so forth */
	return 0x1000 - ((uintptr_t)ptr & 0xfff);
}


#ifdef __i386__
/* via uClibc */
static void *memcpy_forward(void *dst, const void *src, size_t len)
{
	int32_t d0, d1, d2;
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

/* generic memcpy_forward() for one-word transfers in a simple loop. */
static void *memcpy_forward(void *dst, const void *src, size_t len)
{
	uintptr_t *d = dst;
	const uintptr_t *s = src;
	size_t i = 0;
	while(len >= sizeof(uintptr_t)) {
		d[i] = s[i];
		i++;
		len -= sizeof(uintptr_t);
	}

	uint8_t *d8 = (uint8_t *)d;
	const uint8_t *s8 = (const uint8_t *)s;
	i *= sizeof(uintptr_t);
	while(len > 0) {
		d8[i] = s8[i];
		i++;
		len--;
	}

	return dst;
}
#endif


void *memcpy(void *dst, const void *src, size_t len) {
	return memcpy_forward(dst, src, len);
}


void *memmove(void *dst, const void *src, size_t len)
{
	if(dst + min_t(size_t, len, 32) < src || dst >= src + len
		|| dst + len < src)
	{
		/* strictly forward OK, or no overlap */
		return memcpy_forward(dst, src, len);
	} else {
		/* the brute force solution */
		void *buf = malloc(len);
		if(buf == NULL) abort();	/* FIXME, somehow */
		memcpy(buf, src, len);
		memcpy(dst, buf, len);
		free(buf);
		return dst;
	}
}


void *memset(void *start, int c, size_t len)
{
	size_t i = 0;

	uint32_t *dst = start;
	const uint8_t t = c;
	uint32_t c32 = 0x01010101u * (uint32_t)t;
	while(len >= 4) {
		dst[i++] = c32;
		len -= 4;
	}
	i *= 4;

	uint8_t *dst8 = start;
	while(len > 0) {
		dst8[i++] = t;
		len--;
	}

	return start;
}


int memcmp(const void *s1, const void *s2, size_t n)
{
	size_t major = n & ~3u;
	for(size_t i = 0, w = major / 4; i < w; i++) {
		unsigned long a = BE32_TO_CPU(*(unsigned long *)(s1 + i * 4)),
			b = BE32_TO_CPU(*(unsigned long *)(s2 + i * 4));
		long c = a - b;
		if(c != 0) return c;
	}

	const uint8_t *a = s1, *b = s2;
	for(size_t i = major; i < n; i++) {
		int c = (int)a[i] - b[i];
		if(c != 0) return c;
	}
	return 0;
}


void *memswap(void *a, void *b, size_t n)
{
	if(a == b || n == 0) return a;
	void *retval = a;

	if(n <= 16) {
		char *aa = a, *bb = b;
		for(int i=0; i < n; i++) {
			char t = aa[i];
			aa[i] = bb[i];
			bb[i] = t;
		}
		goto end;
	}

	char buf[256];
	size_t head = n % sizeof(buf);
	if(head > 0) {
		memcpy(buf, a, head);
		memcpy(a, b, head);
		memcpy(b, buf, head);
		a += head; b += head;
		if(n <= head) goto end;
	}

	/* i like the part where he said "sizeof(buf)" */
	for(size_t i = 0, l = n / sizeof(buf); i < l; i++) {
		memcpy(buf, a, sizeof(buf));
		memcpy(a, b, sizeof(buf));
		memcpy(b, buf, sizeof(buf));
		a += sizeof(buf); b += sizeof(buf);
	}

end:
	return retval;
}


int strncmp(const char *a, const char *b, size_t max)
{
	if(a == b) return 0;

	/* the word-at-a-time optimization requires that long loads are valid,
	 * i.e. that they don't cross a 4k boundary in memory. so the loop
	 * consists of two parts: the one that runs up to such a boundary in
	 * either operand, and one that runs over it.
	 */
	size_t pos = 0;
	while(pos < max) {
		/* NOTE: could add a second byte-at-a-time segment here to bump @pos
		 * such that at least one of a[pos] and b[pos] is aligned to 4. but
		 * that's a can of worms, so let's not.
		 */
		int bytes = min_t(int, max - pos,
				min(until_page(&a[pos]), until_page(&b[pos]))),
			words = bytes / sizeof(unsigned long);
		for(; words > 0; words--, pos += sizeof(unsigned long)) {
			unsigned long la = BE32_TO_CPU(*(const unsigned long *)&a[pos]),
				lb = BE32_TO_CPU(*(const unsigned long *)&b[pos]);
			if((haszero(la) | haszero(lb)) == 0) {
				long c = la - lb;
				if(c != 0) return c;		/* by content */
			} else {
				unsigned long za = zero_mask(la), zb = zero_mask(lb);
				assert((za | zb) != 0);
				unsigned long m = ((1ul << MSB(za | zb)) >> 7) * 0xff;
				assert(m != 0);
				m |= m << 8;
				m |= m << 16;
				long c = (la & m) - (lb & m);
				if(c != 0) return c;		/* by content */
				return (za & m) - (zb & m);	/* by length */
			}
		}

		/* then byte at a time. */
		for(int tail = bytes % sizeof(unsigned long); tail > 0; pos++, tail--) {
			int c = (int)a[pos] - b[pos];
			if(c != 0) return c;
			if(a[pos] == '\0') {
				assert(b[pos] == '\0');
				return 0;
			}
		}
	}

	return 0;
}


int strcmp(const char *a, const char *b) {
	return strncmp(a, b, INT_MAX);
}


static inline char ascii_tolower(char c) {
	if(c >= 'A' && c <= 'Z') return c - 32; else return c;
}


int strcasecmp(const char *pa, const char *pb)
{
	char *a = strdup(pa), *b = strdup(pb);
	for(int i=0; a[i] != '\0'; i++) a[i] = ascii_tolower(a[i]);
	for(int i=0; b[i] != '\0'; i++) b[i] = ascii_tolower(b[i]);
	int n = strncmp(a, b, INT_MAX);
	free(a);
	free(b);
	return n;
}


char *strdup(const char *str) {
	return strndup(str, INT_MAX);
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


size_t strnlen(const char *str, size_t max)
{
	/* fancy-pants word-at-a-time algorithm w/ byte-at-a-time fallback for
	 * crossing 4k page boundaries safely under POSIX.
	 * NB: this, too, could pre-align @str to word boundary.
	 */
	size_t pos = 0;
	while(pos < max) {
		int bytes = min_t(int, max - pos, until_page(&str[pos])),
			words = bytes / sizeof(uintptr_t);
		for(; words > 0; words--, pos += sizeof(uintptr_t)) {
			uintptr_t w = LE32_TO_CPU(*(const uintptr_t *)&str[pos]);
			if(haszero(w) != 0) {
				uintptr_t z = zero_mask(w);
				size_t len = pos + ffsl(z) / 8 - 1;
				assert(str[len] == '\0');
				return len;
			}
		}

		for(int tail = bytes % sizeof(uintptr_t); tail > 0; pos++, tail--) {
			if(str[pos] == '\0') return pos;
		}
	}

	return pos;
}


size_t strlen(const char *str) {
	return strnlen(str, INT_MAX);
}


char *strncpy(char *dest, const char *src, size_t n)
{
	int used = strscpy(dest, src, n);
	if(used >= 0 && used < n) memset(&dest[used], '\0', n - used);
	return dest;
}


char *strchr(const char *s, int c) {
	char *ret = strchrnul(s, c);
	return *ret == '\0' ? NULL : ret;
}


char *strchrnul(const char *s, int c)
{
	if(c == '\0') return (char *)s + strlen(s);

	/* WAAT w/ page border safety */
	size_t pos = 0;
	for(;;) {
		int bytes = until_page(&s[pos]), words = bytes / sizeof(long);
		for(; words > 0; words--, pos += sizeof(long)) {
			unsigned long x = LE32_TO_CPU(*(unsigned long *)&s[pos]),
				found = byte_mask(x, c), zero = zero_mask(x);
			assert((found | zero) == 0 || found != zero);
			if(found | zero) {
				size_t len = pos + ffsl(found | zero) / 8 - 1;
				assert(s[len] == c || s[len] == '\0');
				return (char *)&s[len];
			}
		}

		for(int tail = bytes % sizeof(long); tail > 0; tail--, pos++) {
			if(s[pos] == c || s[pos] == '\0') return (char *)s + pos;
		}
	}
}


char *strrchr(const char *s, int c)
{
	const char *t = s + strlen(s);
	while(*t != c && t != s) t--;
	return t != s ? (char *)t : NULL;
}


char *strstr(const char *haystack, const char *needle)
{
	if(*needle == '\0') goto end;	/* degenerate case */

	size_t n_len = 0;
	char diff = 0;
	while(needle[n_len] != '\0') {
		if(haystack[n_len] == '\0') return NULL;	/* #haystack < #needle */
		diff |= needle[n_len] ^ haystack[n_len];
		n_len++;
	}
	if(diff == 0) goto end;			/* prefix case */

	if(haystack[0] != needle[0]) goto forward;
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
forward:
		haystack = strchr(haystack + 1, needle[0]);
	} while(haystack != NULL);

end:
	return (char *)haystack;
}


/* fancy! and O(1) space! */
char *strpbrk(const char *s, const char *needles)
{
	const int long_bits = sizeof(unsigned long) * 8,
		present_len = (256 + long_bits - 1) / long_bits;
	unsigned long present[present_len];

	for(int i=0; i < present_len; i++) present[i] = 0;
	for(int i=0; needles[i] != '\0'; i++) {
		int c = (unsigned char)needles[i], o = c / long_bits,
			b = c & (long_bits - 1);
		present[o] |= 1ul << b;
	}

	while(*s != '\0') {
		int c = (unsigned char)(*s), o = c / long_bits,
			b = c & (long_bits - 1);
		if(present[o] & (1ul << b)) return (char *)s;
		s++;
	}

	return NULL;
}


size_t strcspn(const char *s, const char *reject) {
	char *p = strpbrk(s, reject);
	return p != NULL ? p - s : strlen(s);
}


/* fairly near copypasta'd from strpbrk() above. */
size_t strspn(const char *s, const char *accept)
{
	const int long_bits = sizeof(unsigned long) * 8,
		present_len = (256 + long_bits - 1) / long_bits;
	unsigned long present[present_len];

	for(int i=0; i < present_len; i++) present[i] = 0;
	for(int i=0; accept[i] != '\0'; i++) {
		int c = (unsigned char)accept[i], o = c / long_bits,
			b = c & (long_bits - 1);
		present[o] |= 1ul << b;
	}

	const char *start = s;
	while(*s != '\0') {
		int c = (unsigned char)(*s), o = c / long_bits,
			b = c & (long_bits - 1);
		if(present[o] & (1ul << b)) s++; else break;
	}

	return s - start;
}


#ifdef ffsl
#undef ffsl
#endif
int ffsl(long int __l)
{
	/* TODO: add bit magic version as well */
	return __builtin_ffsl(__l);
}
