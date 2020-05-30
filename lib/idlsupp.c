/* support bits for µIDL.
 *
 * TODO: this interface isn't obviously defined anywhere, so its future
 * consistency is questionable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <threads.h>


static tss_t key;
static once_flag init_once = ONCE_FLAG_INIT;


static void create_key(void)
{
	int n = tss_create(&key, &free);
	if(n != thrd_success) {
		fprintf(stderr, "%s: can't create muidl support tss_t, n=%d\n",
			__func__, n);
		abort();
	}
}


void muidl_supp_alloc_context(unsigned int length)
{
	call_once(&init_once, &create_key);

	void *ctx = tss_get(key);
	if(ctx != NULL) free(ctx);

	if(length < 64) length = 64;
	ctx = malloc(length);
	if(ctx == NULL) {
		printf("%s: malloc(%u) failed\n", __func__, length);
		abort();
	}
	memset(ctx, '\0', length);
	tss_set(key, ctx);
}


void *muidl_supp_get_context(void)
{
	call_once(&init_once, &create_key);
	return tss_get(key);
}
