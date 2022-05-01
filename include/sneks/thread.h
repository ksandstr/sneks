/* internals of threads implemented in sys/crt/{threads,mutex}.c etc. */
#ifndef _SNEKS_THREAD_H
#define _SNEKS_THREAD_H

#include <threads.h>
#include <ccan/list/list.h>
#include <l4/types.h>

#define STKSIZE (PAGE_SIZE * 2)

struct mtx_wait {
	struct list_node wait_link; /* in mtx_info.waitlist */
	L4_ThreadId_t ltid;
};

/* stkbase is address of thrd & ~(STKSIZE - 1). */
struct thrd
{
	int magic;
	L4_ThreadId_t tid;	/* global */
	struct mtx_wait mw;

	/* exit/join syncing. */
	_Atomic int res;
	_Atomic bool alive;
	struct thrd *_Atomic joiner;
};

extern struct thrd *thrd_from_tid(L4_ThreadId_t tid);
extern L4_ThreadId_t thrd_to_tid(thrd_t t);
#define tidof_NP(t) thrd_to_tid((t))

#endif
