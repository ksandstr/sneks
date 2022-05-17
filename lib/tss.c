/* thread-specific storage for all runtimes. undertested for corner cases. */
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <threads.h>
#include <assert.h>
#include <l4/types.h>
#include <l4/thread.h>
#include <sneks/spin.h>

#define MAX_TSS 15

struct tss {
	unsigned max;
	void *ptrs[];
};

static _Atomic tss_dtor_t dtors[MAX_TSS + 1];

int tss_create(tss_t *key, tss_dtor_t dtor_fn) {
	for(int i = 1; i <= MAX_TSS; i++) {
		if(atomic_compare_exchange_strong(&dtors[i], &(tss_dtor_t){ NULL }, dtor_fn)) { *key = i; return thrd_success; }
	}
	return thrd_error;
}

void tss_delete(tss_t key) {
	atomic_store_explicit(&dtors[key], NULL, memory_order_release);
}

void *tss_get(tss_t key) {
	struct tss *tss = (void *)L4_UserDefinedHandle();
	return tss == NULL || key > tss->max ? NULL : tss->ptrs[key];
}

void tss_set(tss_t key, void *value)
{
	if(key <= 0 || key > MAX_TSS) return;
	struct tss *tss = (void *)L4_UserDefinedHandle();
	if(tss == NULL || key > tss->max) {
		struct tss *re = realloc(tss, sizeof *tss + sizeof(void *) * (key + 1));
		if(re == NULL) abort();
		for(unsigned i = tss == NULL ? 0 : re->max + 1; i < key; i++) re->ptrs[i] = NULL;
		tss = re; tss->max = key;
		L4_Set_UserDefinedHandle((L4_Word_t)tss);
	}
	tss->ptrs[key] = value;
}

void call_once(once_flag *flag, void (*func)(void))
{
	int old = atomic_load_explicit(flag, memory_order_acquire);
again:
	if(old > 1) return; /* done */
	if(old == 0) { /* try to run @func. */
		if(!atomic_compare_exchange_strong(flag, &old, 1)) goto again;
		(*func)();
		atomic_store(flag, 2);
	} else {
		assert(old == 1);
		/* wait until concurrent @func completes. */
		spinner_t s = { };
		while(atomic_load_explicit(flag, memory_order_acquire) <= 1) spin(&s);
	}
}

void __tss_on_exit(void)
{
	struct tss *tss = (void *)L4_UserDefinedHandle();
	if(tss == NULL) return;
	assert(tss->ptrs[0] == NULL);
	bool repeat;
	do {
		repeat = false;
		for(unsigned i = 1; i <= tss->max; i++) {
			void *ptr = tss->ptrs[i];
			if(ptr != NULL) {
				repeat = true;
				tss->ptrs[i] = NULL;
				if(dtors[i] != NULL) (*dtors[i])(ptr);
			}
		}
	} while(repeat);
	L4_Set_UserDefinedHandle(0);
	free(tss);
}
