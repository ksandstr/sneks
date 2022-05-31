/* support bits for µIDL. such as they are. */
#include <stdlib.h>
#include <threads.h>

static tss_t key;
static once_flag init_once = ONCE_FLAG_INIT;

static void create_key(void) {
	if(tss_create(&key, &free) != thrd_success) abort();
}

void muidl_supp_alloc_context(unsigned int length)
{
	call_once(&init_once, &create_key);
	void *ctx = tss_get(key);
	if(ctx != NULL) free(ctx);
	if(length < 64) length = 64;
	if(ctx = calloc(1, length), ctx == NULL) abort();
	tss_set(key, ctx);
}

void *muidl_supp_get_context(void) {
	call_once(&init_once, &create_key);
	return tss_get(key);
}
