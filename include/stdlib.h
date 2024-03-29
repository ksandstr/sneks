#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include <limits.h>
#include <math.h>
#include <sys/wait.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

extern void *malloc(size_t size) __attribute__((malloc));
extern void free(void *ptr);
extern void *calloc(size_t nmemb, size_t size) __attribute__((malloc));
extern void *realloc(void *ptr, size_t size);
extern void *valloc(size_t size);
extern int posix_memalign(void **memptr, size_t alignment, size_t size);

static inline void *aligned_alloc(size_t alignment, size_t size) {
	void *ptr;
	int n = posix_memalign(&ptr, alignment, size);
	return n == 0 ? ptr : NULL;
}

extern _Noreturn void abort(void);
extern _Noreturn void exit(int status);
extern _Noreturn void _Exit(int status);

extern int abs(int j);
extern long int labs(long int j);
extern long long int llabs(long long int j);

#if defined(__GNUC__) && !defined(__IN_ABS_IMPL)
#define abs(x) __builtin_abs((x))
#define labs(x) __builtin_labs((x))
#define llabs(x) __builtin_llabs((x))
#endif

extern int atexit(void (*function)(void));

extern char *getenv(const char *name);
extern int setenv(const char *name, const char *value, int overwrite);
extern int unsetenv(const char *name);
extern int putenv(char *string);
extern int clearenv(void);

extern int atoi(const char *nptr);
extern long strtol(const char *restrict str, char **restrict endptr, int base);
extern long long strtoll(const char *restrict str, char **restrict endptr, int base);
extern unsigned long strtoul(const char *restrict str, char **restrict endptr, int base);
extern unsigned long long strtoull(const char *restrict str, char **restrict endptr, int base);
extern double strtod(const char *restrict str, char **restrict endptr);

extern long a64l(const char *str64);
extern char *l64a(long value);

extern void qsort(void *data, size_t count, size_t size, int (*compare_fn)(const void *, const void *));
extern void *bsearch(const void *needle, const void *haystack, size_t count, size_t size, int (*compare_fn)(const void *, const void *));

#endif
