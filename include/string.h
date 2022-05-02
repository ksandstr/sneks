#ifndef _STRING_H
#define _STRING_H 1

#include <stddef.h>

#define __pure __attribute__((__pure__))

extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memmove(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern int memcmp(const void *s1, const void *s2, size_t n);
extern void *memchr(const void *ptr, int c, size_t n);

extern char *strncpy(char *dest, const char *src, size_t n);
extern char *strcpy(char *dest, const char *src);

extern __pure int strcmp(const char *a, const char *b);
extern __pure int strncmp(const char *a, const char *b, size_t n);
extern __pure int strcasecmp(const char *a, const char *b);

extern __pure size_t strlen(const char *str);
extern __pure size_t strnlen(const char *str, size_t n);
extern __pure char *strchr(const char *s, int c);
extern __pure char *strchrnul(const char *s, int c);
extern __pure char *strrchr(const char *s, int c);
extern __pure char *strstr(const char *haystack, const char *needle);
extern __pure char *strpbrk(const char *s, const char *a);
extern __pure size_t strspn(const char *s, const char *accept);
extern __pure size_t strcspn(const char *s, const char *reject);

extern char *strdup(const char *str);
extern char *strndup(const char *str, size_t n);
#ifdef _GNU_SOURCE
#define strdupa(__str) ({ char *__s = (__str); int __l = strlen(__s); \
	__t = __builtin_alloca(__l + 1); memcpy(__t, __s, __l + 1); __t; })
#define strndupa(__str, __n) ({ char *__s = (__str); int __l = strnlen(__s, (__n)); \
	__t = __builtin_alloca(__l + 1); memcpy(__t, __s, __l + 1); __t; })
extern int ffsl(long);
extern int ffsll(long long);
#define ffsl(__l) __builtin_ffsl((__l))
#define ffsll(__ll) __builtin_ffsll((__ll))
#endif

#if !defined(IN_LIB_IMPL) && defined(__GNUC__) && defined(__OPTIMIZE__)
#define memcpy(a, b, c) __builtin_memcpy((a), (b), (c))
#define memset(a, b, c) __builtin_memset((a), (b), (c))
#define memchr(s, c, n) __builtin_memchr((s), (c), (n))
#define memmove(a, b, c) __builtin_memmove((a), (b), (c))
#define strcpy(a, b) __builtin_strcpy((a), (b))
#define strncpy(a, b, c) __builtin_strncpy((a), (b), (c))
#define strcmp(a, b) __builtin_strcmp((a), (b))
#define strncmp(a, b, n) __builtin_strncmp((a), (b), (n))
#define strlen(a) __builtin_strlen((a))
#define strchr(s, c) __builtin_strchr((s), (c))
#define strrchr(s, c) __builtin_strrchr((s), (c))
#define strpbrk(s, a) __builtin_strpbrk((s), (a))
#define strstr(h, n) __builtin_strstr((h), (n))
#endif

/* such nonstandard, very clever, wow. */
/* returns @l. undefined results when @l[0..n-1] and @r[0..n-1] overlap. */
extern void *memswap(void *l, void *r, size_t n);

/* copies @src into @dest up to @n bytes, setting exactly one of @dest[0..@n)
 * to '\0' so that @dest is always a valid C string. returns -E2BIG if '\0'
 * didn't occur in src[0..n), or the index into @src where the null byte was
 * found (it'll be at the same place in @dest). [interface via Linux, except
 * that we don't have ssize_t.]
 */
extern int strscpy(char *dest, const char *src, size_t n);

#undef __pure
#endif
