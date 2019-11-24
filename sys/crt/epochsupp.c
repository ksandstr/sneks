
/* support routine for lfht-epoch.o . */

#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/schedule.h>

#include "epoch.h"


static tss_t epoch_key;
static void (*epoch_dtor_fn)(void *ptr) = NULL;


static void call_dtor(void *ptr) {
	(*epoch_dtor_fn)(ptr);
}


static void init_epoch_client_c11(void) {
	int n = tss_create(&epoch_key, &call_dtor);
	if(n != thrd_success) {
		printf("%s: tss_create() failed: n=%d\n", __func__, n);
		abort();
	}
}


void *e_ext_get(size_t size, void (*dtor_fn)(void *ptr))
{
	static once_flag init_once = ONCE_FLAG_INIT;
	call_once(&init_once, &init_epoch_client_c11);

	void *ptr = tss_get(epoch_key);
	if(unlikely(ptr == NULL)) {
		if(epoch_dtor_fn == NULL) epoch_dtor_fn = dtor_fn;
		ptr = calloc(1, size);
		tss_set(epoch_key, ptr);
	}

	return ptr;
}


int sched_yield(void)
{
	/* in sys/crt sched_yield() should drop thread priority to the slopsucker
	 * level, ThreadSwitch to let other threads at that level have a go, then
	 * reëlevate priority. since priorities aren't done, this'll do.
	 */
	L4_ThreadSwitch(L4_nilthread);
	return 0;
}
