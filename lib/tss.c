
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


static struct tss_dtors *_Atomic current_dtors;


/* NOTE: this function is the sole thing in this module that's allowed to
 * touch current_dtors without an open epoch bracket. that's because it is the
 * only routine that can free an old version of the array, so it may instead
 * exclude concurrent instances of itself. we do this to prevent indefinite
 * mutual recursion between this and e_begin() (via e_ext_get()).
 */
int tss_create(tss_t *key, tss_dtor_t dtor)
{
	static atomic_flag lock = ATOMIC_FLAG_INIT;
	while(!atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {
		/* spin */
		L4_ThreadSwitch(L4_nilthread);
	}

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
	atomic_flag_clear(&lock);
	return thrd_success;

nomem:
	atomic_flag_clear(&lock);
	return thrd_nomem;
}


/* stop the dtor associated with @key from being called from any thread
 * exiting after tss_delete() returns.
 */
void tss_delete(tss_t key)
{
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


void __tss_on_exit(void)
{
	struct tss_block *blk = (void *)L4_UserDefinedHandle();
	if(blk == NULL) return;

	assert(blk->ptrs[0] == NULL);
	int eck = e_begin();
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
