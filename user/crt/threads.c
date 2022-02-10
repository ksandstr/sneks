
/* various thread-like things for the singly-threaded runtime.
 *
 * to clarify: the user program is not given access to any thrd_create() etc.
 * stuff, but the runtime does use some threads internally. for example, epoll
 * requires a thread for async reception of I/O notifications to avoid
 * checking every source of interest all the time (recentralization).
 *
 * so there's some runtime-private threading things here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdnoreturn.h>
#include <errno.h>
#include <assert.h>
#include <threads.h>
#include <ccan/htable/htable.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/syscall.h>

#include <sneks/hash.h>
#include <sneks/sysinfo.h>
#include <sneks/api/proc-defs.h>

#include "private.h"


#define CRT_STACK_SIZE (16 * 1024)


static size_t rehash_stkbase(const void *, void *);


static struct htable crt_threads = HTABLE_INITIALIZER(
	crt_threads, &rehash_stkbase, NULL);


static size_t rehash_stkbase(const void *stkbase, void *unused) {
	return int_hash(*(L4_Word_t *)(stkbase + CRT_STACK_SIZE
		- sizeof(L4_Word_t)));
}


static bool cmp_tid_to_stkbase(const void *stkbase, void *key) {
	L4_Word_t ref = *(L4_Word_t *)(stkbase + CRT_STACK_SIZE
		- sizeof(L4_Word_t));
	return ref == *(L4_Word_t *)key;
}


int *__errno_location(void)
{
	static int teh_errno = 0;
	return &teh_errno;
}


noreturn void thrd_exit(int res) {
	/* pedantic but correct. */
	exit(res);
}


static noreturn void crt_thread_wrapper(void (*fn)(void *), void *param_ptr)
{
	(*fn)(param_ptr);
	__proc_remove_thread(__the_sysinfo->api.proc,
		L4_MyGlobalId().raw, L4_MyLocalId().raw);

	for(;;) L4_Sleep(L4_Never);	/* fallback? */
}


/* internal ("CRT") threads are like POSIX threads, but they can't receive
 * signals and can't properly interact with portable subprograms (since those
 * facilities neither apply to internal threads, nor are available to them due
 * to thread unsafeness).
 *
 * thread-unsafeness means things like malloc; CRT threads must consume data
 * produced elsewhere typically by means of atomic operations, or have Call
 * synchronization with the main program before calling malloc. most POSIXy
 * routines should be presumed unsafe even where malloc would be safe.
 */
int __crt_thread_create(
	L4_ThreadId_t *tid_p,
	void (*fn)(void *), void *param_ptr)
{
	assert(tid_p != NULL);
	*tid_p = L4_nilthread;

	void *stkbase = aligned_alloc(4096, CRT_STACK_SIZE);
	if(stkbase == NULL) return -ENOMEM;

	int n = __proc_create_thread(__the_sysinfo->api.proc, &tid_p->raw);
	if(n != 0) {
		free(stkbase);
		return n;
	}

	*(L4_Word_t *)(stkbase + CRT_STACK_SIZE - sizeof(L4_Word_t)) = tid_p->raw;
	assert(rehash_stkbase(stkbase, NULL) == int_hash(tid_p->raw));
	uintptr_t stktop = ((uintptr_t)stkbase + CRT_STACK_SIZE - 32) & ~0xfu;
#ifdef __SSE__
	stktop += 4;	/* for arcane reasons */
#endif
	L4_Word_t *stk = (L4_Word_t *)stktop;
	*--stk = (L4_Word_t)param_ptr;
	*--stk = (L4_Word_t)fn;
	*--stk = 0xbeef7007;	/* flatus of a carnivore */

	bool ok = htable_add(&crt_threads, rehash_stkbase(stkbase, NULL), stkbase);
	if(!ok) {
		free(stkbase);
		__proc_remove_thread(__the_sysinfo->api.proc, tid_p->raw,
			L4_LocalIdOf(*tid_p).raw);
		return -ENOMEM;
	}

	L4_Set_PagerOf(*tid_p, L4_Pager());
	L4_Start_SpIp(*tid_p, (L4_Word_t)stk, (L4_Word_t)&crt_thread_wrapper);

	return 0;
}


void __crt_thread_join(L4_ThreadId_t tid)
{
	assert(L4_IsGlobalId(tid));
	size_t hash = int_hash(tid.raw);
	void *stkbase = htable_get(&crt_threads, hash, &cmp_tid_to_stkbase, &tid);
	if(stkbase == NULL) return;

	/* listen until it goes away, having been removed. reply everything with
	 * muidl EINVAL and discard @tid and its stack only when the partner has
	 * gone away.
	 */
	L4_MsgTag_t tag;
	do {
		L4_Accept(L4_UntypedWordsAcceptor);
		tag = L4_Receive(tid);
		if(L4_IpcSucceeded(tag)) {
			L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1, .X.label = 1 }.raw);
			L4_LoadMR(1, EINVAL);
			tag = L4_Reply(tid);
		}
	} while(L4_IpcSucceeded(tag) || L4_ErrorCode() == 2);
	if(L4_ErrorCode() == 3) {
		bool ok = htable_del(&crt_threads, hash, stkbase);
		if(!ok) abort();
		free(stkbase);
	}
}
