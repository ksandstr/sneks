
/* thread-specific storage, compatible with root, user, and sys alike. (but
 * not sysmem, there's no malloc.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>
#include <assert.h>

#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/schedule.h>

#include "epoch.h"


typedef void (*tss_dtor_t)(void *);


struct tss_block {
	unsigned max_tss;
	void *ptrs[];
};


struct tss_dtors {
	unsigned length;
	tss_dtor_t dtors[];
};


/* NOTE: because of a recursion hazard from e_begin(), write access to
 * current_dtors is restricted with a makeshift spinlock. this is used from
 * tss_create() and tss_delete(), and nowhere else.
 */
static struct tss_dtors *_Atomic current_dtors;
static _Atomic int current_dtors_lock = 0;


static inline void lock_current_dtors(void)
{
	int old = 0;
	while(!atomic_compare_exchange_weak_explicit(&current_dtors_lock, &old, 1,
		memory_order_acquire, memory_order_relaxed))
	{
		/* please wait warmly */
		L4_ThreadSwitch(L4_nilthread);
		old = 0;
	}
}


static inline void unlock_current_dtors(void) {
	int old = atomic_exchange_explicit(&current_dtors_lock, 0,
		memory_order_release);
	assert(old == 1);
}


int tss_create(tss_t *key, tss_dtor_t dtor)
{
	lock_current_dtors();
	struct tss_dtors *old = atomic_load(&current_dtors), *new = NULL;
	do {
		free(new);
		if(unlikely(old == NULL)) {
			*key = 1;
			new = malloc(sizeof *new + sizeof(tss_dtor_t) * 2);
			if(new == NULL) goto nomem;
			new->length = 2;
			new->dtors[0] = NULL;
		} else {
			*key = old->length;
			new = malloc(sizeof *new + sizeof(tss_dtor_t) * (*key + 1));
			if(new == NULL) goto nomem;
			new->length = *key + 1;
			memcpy(new->dtors, old->dtors, old->length * sizeof(tss_dtor_t));
		}
		new->dtors[*key] = dtor;
	} while(!atomic_compare_exchange_strong(&current_dtors, &old, new));
	if(old != NULL) e_free(old);
	unlock_current_dtors();
	return thrd_success;

nomem:
	unlock_current_dtors();
	return thrd_nomem;
}


/* stop the dtor associated with @key from being called from any thread
 * exiting after tss_delete() returns.
 */
void tss_delete(tss_t key)
{
	lock_current_dtors();
	int eck = e_begin();
	struct tss_dtors *old = atomic_load(&current_dtors), *new = NULL;
	do {
		free(new);
		int newsize = old->length;
		if(key == newsize - 1) newsize--;
		new = malloc(sizeof *new + sizeof(tss_dtor_t) * newsize);
		if(new == NULL) {
			fprintf(stderr, "%s: out of memory!\n", __func__);
			abort();
		}
		new->length = newsize;
		memcpy(new->dtors, old->dtors, newsize * sizeof(tss_dtor_t));
		if(key < newsize) new->dtors[key] = NULL;
	} while(!atomic_compare_exchange_strong(&current_dtors, &old, new));
	e_free(old);
	e_end(eck);
	unlock_current_dtors();
}


void *tss_get(tss_t key)
{
	struct tss_block *blk = (void *)L4_UserDefinedHandle();
	return blk == NULL || key > blk->max_tss ? NULL : blk->ptrs[key];
}


void tss_set(tss_t key, void *value)
{
	if(unlikely(key <= 0)) return;
	struct tss_block *blk = (void *)L4_UserDefinedHandle();

	if(blk == NULL || key > blk->max_tss) {
		struct tss_block *oth = realloc(blk,
			sizeof *blk + sizeof(void *) * (key + 1));
		if(oth == NULL) {
			fprintf(stderr, "tss.c: realloc in tss_set() failed\n");
			abort();
		}
		for(unsigned i = blk == NULL ? 0 : oth->max_tss + 1; i < key; i++) {
			oth->ptrs[i] = NULL;
		}
		blk = oth;
		blk->max_tss = key;
		L4_Set_UserDefinedHandle((L4_Word_t)blk);
	}

	blk->ptrs[key] = value;
}


/* this is basically a spinlock around @func, ensuring that concurrent callers
 * return only once the function has completed.
 */
void call_once(once_flag *flag, void (*func)(void))
{
	int old = atomic_load_explicit(flag, memory_order_relaxed);

again:
	if(likely(old > 1)) {
		/* early out. */
		return;
	} else if(old == 0) {
		/* try to run @func. */
		bool run = atomic_compare_exchange_strong(flag, &old, 1);
		if(!run) goto again;	/* nope! */
		(*func)();
		atomic_store(flag, 2);
	} else if(unlikely(old == 1)) {
		/* wait until concurrent @func completes. */
		while(atomic_load(flag) <= 1) {
			asm volatile ("pause");
		}
	}
}


void __tss_on_exit(void)
{
	struct tss_block *blk = (void *)L4_UserDefinedHandle();
	if(blk == NULL) return;

	assert(blk->ptrs[0] == NULL);
	e_begin();
	struct tss_dtors *dtors = atomic_load_explicit(&current_dtors,
		memory_order_relaxed);
	assert(blk->max_tss < dtors->length);
	assert(dtors->dtors[0] == NULL);
	/* NOTE: this repeat algorithm is wrong. it should keep repeat set until
	 * the loop finds no non-NULL pointers.
	 */
	bool repeat;
	do {
		repeat = false;
		for(unsigned i=1; i <= blk->max_tss; i++) {
			tss_dtor_t fn = dtors->dtors[i];
			void *ptr = blk->ptrs[i];
			if(fn != NULL && ptr != NULL) {
				blk->ptrs[i] = NULL;
				(*fn)(ptr);
				if(blk->ptrs[i] != NULL) repeat = true;
			}
		}
	} while(repeat);
	/* NOTE: no e_end() because the client struct was just dtor'd */
	free(blk);
	L4_Set_UserDefinedHandle(0);
}
