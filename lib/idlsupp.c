
/* very basic support interface for µIDL. works in a non-threaded runtime
 * only.
 *
 * TODO: reimplement for C11 TSD stuff so that this can be used to deploy
 * services from root and other multithreaded tasks.
 *
 * TODO: this interface isn't obviously defined anywhere, so its future
 * consistency is questionable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <ccan/likely/likely.h>


static void *the_context = NULL;


void muidl_supp_alloc_context(unsigned int length)
{
	if(unlikely(the_context == NULL)) {
		if(length < 64) length = 64;
		the_context = malloc(length);
		if(the_context == NULL) {
			printf("%s: malloc(%u) failed\n", __func__, length);
			abort();
		}
		memset(the_context, '\0', length);
	}
}


void *muidl_supp_get_context(void) {
	return the_context;
}
