/* internals of threads implemented in sys/crt/{threads,mutex}.c etc. */

#ifndef __SNEKS_THREAD_H__
#define __SNEKS_THREAD_H__

#include <ccan/list/list.h>
#include <l4/types.h>
#include <l4/thread.h>


#define STKSIZE (PAGE_SIZE * 2)


/* entry in mtx_info.waitlist */
struct mtx_wait {
	struct list_node wait_link;
	L4_ThreadId_t ltid;
};


struct thrd
{
	L4_ThreadId_t tid;	/* global */
	void *stkbase;
	struct mtx_wait mw;

	/* exit/join syncing. */
	_Atomic int res;
	_Atomic bool alive;
	struct thrd *_Atomic joiner;
};


/* convenience extras */
extern struct thrd *thrd_from_tid(L4_ThreadId_t tid);
extern L4_ThreadId_t thrd_to_tid(thrd_t t);


#endif
