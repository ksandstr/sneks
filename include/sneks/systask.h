
/* things from sys/crt that're available to systasks, and not to root, sysmem,
 * or userspace.
 */

#ifndef __SNEKS_SYSTASK_H__
#define __SNEKS_SYSTASK_H__

#include <stdbool.h>
#include <l4/types.h>


/* from threads.c */
extern L4_ThreadId_t __uapi_tid;


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
