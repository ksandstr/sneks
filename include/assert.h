#ifndef _ASSERT_H
#define _ASSERT_H

extern _Noreturn void __assert_failure(const char *condition, const char *file, unsigned int line, const char *function);

#ifndef NDEBUG
#define assert(condition) do { \
		if(!(condition)) { __assert_failure(#condition, __FILE__, __LINE__, __func__); } \
	} while(0)
#else
/* quiet warnings about @condition not referenced */
#define assert(condition) do { (void)sizeof((condition)); } while(0)
#endif

/* always C11, never C++. >:3 */
#undef static_assert
#define static_assert _Static_assert

#endif
