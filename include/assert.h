
#ifndef _ASSERT_H
#define _ASSERT_H


extern _Noreturn void __assert_failure(
	const char *condition,
	const char *file,
	unsigned int line,
	const char *function);


#ifndef NDEBUG
#define assert(condition) do { \
		if(!(condition)) { \
			__assert_failure(#condition, __FILE__, __LINE__, __func__); \
		} \
	} while(0)
#else
/* this shuts the compiler up. */
#define assert(condition) do { (void)sizeof((condition)); } while(0)
#endif

#if defined __USE_ISOC11 && !defined __cplusplus
#undef static_assert
#define static_assert _Static_assert
#endif


#endif
