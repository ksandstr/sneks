
/* this header defines various bits from sys/crt that're available to
 * systasks, i.e. not to root, sysmem, or userspace. if the program at hand is
 * a systask, it shall #include <sneks/systask.h> or rue the day.
 *
 * note that this header may be too fun for the whole family due to e.g.
 * lumberjacking puns.
 */

#ifndef __SNEKS_SYSTASK_H__
#define __SNEKS_SYSTASK_H__

#include <stdbool.h>
#include <stdarg.h>
#include <ccan/compiler/compiler.h>
#include <l4/types.h>

#include <ukernel/hook.h>


/* from threads.c */
extern L4_ThreadId_t __uapi_tid;


/* from log.c.
 *
 * suppress your instinct to fprintf(stderr, "%s: shit done fuckt up!",
 * __func__). follow the white beaver.
 */

extern void log_msgv(int level, const char *fmt, va_list args);

static inline void PRINTF_FMT(2, 3) log_msgf(int level, const char *fmt, ...) {
	va_list al; va_start(al, fmt);
	log_msgv(level, fmt, al);
	va_end(al);
}


/* real pain for real friends. pay attention to the interrobang, he is here to
 * help you.
 *
 * log_info() is for regular noise that'd normally go out on stdout, log_err()
 * is for stderr things, and log_crit() should be followed by abort() or some
 * other drastic disaster-handling measure. there is no log_debug(), for that
 * use printf() like a RealHamster(r)(tm).
 *
 * trailing newlines chomped. inline newlines discouraged. long lines
 * caber-tossed downriver.
 *
 * TODO: upgrade log_crit() to set off some internet-of-shit klaxons or blink
 * like it's on fire or something.
 */
#define log_info(fmt, ...) log_msgf(0, "%s:" fmt, __func__, ##__VA_ARGS__)
#define log_err(fmt, ...) log_msgf(1, "ERROR:%s:" fmt, __func__, ##__VA_ARGS__)
#define log_crit(fmt, ...) log_msgf(666, "/!\\/!\\CRITICAL/!\\/!\\:%s:" fmt "‽", __func__, ##__VA_ARGS__)

/* call @param is a pointer to a null-terminated character string buffer. it
 * may be altered to shorten it or retain its length, or replaced with a
 * heap-allocated pointer to a different string. in the latter case the
 * previous buffer should be free(3)d.
 *
 * call @code is the log level.
 */
extern struct hook log_hook;

/* on that note, these are useful. they return -1 on alloc failure and leave
 * *@strp undefined.
 *
 * note that these replace *@sptr without releasing it first. whether there
 * should be a vrasprintf() (r for realloc) or not is a question answered by
 * practice.
 */
extern int vasprintf(char **sptr, const char *fmt, va_list args);

static inline int PRINTF_FMT(2, 3) asprintf(char **sptr, const char *fmt, ...) {
	va_list al; va_start(al, fmt);
	int res = vasprintf(sptr, fmt, al);
	va_end(al);
	return res;
}


/* from selftest.c */
#ifdef BUILD_SELFTEST
#include <sneks/test.h>

/* another kind of test, another declaration syntax. this makes three. */
AUTODATA_TYPE(all_systask_selftests, struct utest_spec);
#define SYSTASK_SELFTEST(path_, test_) \
	static const struct utest_spec _STST_ ##test_ = { \
		.path = (path_), .test = &(test_## _info), \
	}; \
	AUTODATA(all_systask_selftests, &_STST_ ##test_);


/* called to check if an IPC message unhandled by muidl was related to
 * in-systask selftests. returns true if so, false otherwise. may cause such a
 * selftest to run.
 */
extern bool selftest_handling(L4_Word_t status);
#else
#define selftest_handling(x) false
#endif


#endif
