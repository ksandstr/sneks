
/* odds and ends without a great enough theme to warrant a module. */

#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <errno.h>
#include <assert.h>
#include <sneks/process.h>

#include "proc-defs.h"


#define RECSEP 0x1e	/* ASCII record separator control character. whee! */


/* FIXME: __uapi_tid isn't stable before first call to thrd_create(). so all
 * of this may well fail badly. to correct, use a lazy-init macro for
 * __uapi_tid instead of the silly thrd_create() in spawn_NP().
 */
extern L4_ThreadId_t __uapi_tid;


static char *p_to_argbuf(char *const strp[])
{
	size_t sz = 128;
	char *buf = malloc(sz), *pos = buf;
	for(int i=0; strp[i] != NULL; i++) {
		int len = strlen(strp[i]);
		if(len + 1 > sz - (pos - buf)) {
			sz *= 2;
			int off = pos - buf;
			buf = realloc(buf, sz);
			pos = buf + off;
		}
		memcpy(pos, strp[i], len);
		pos[len] = strp[i + 1] == NULL ? '\0' : RECSEP;
		pos += len + 1;
	}
	if(strp[0] == NULL) *(pos++) = '\0';
	return realloc(buf, pos - buf + 1);
}


static int nothing_fn(void *ptr) {
	/* not strictly what it says on the tin, man */
	return 0;
}


/* TODO: do away with this extremely silly thing once __uapi_tid gets
 * civilized (see comment earlier).
 */
static L4_ThreadId_t get_uapi_tid(void)
{
	if(L4_IsNilThread(__uapi_tid)) {
		thrd_t foo;
		int n = thrd_create(&foo, &nothing_fn, NULL);
		if(n == thrd_success) {
			int res;
			thrd_join(foo, &res);
		}
	}
	return __uapi_tid;
}


int spawn_NP(const char *filename, char *const argv[], char *const envp[])
{
	char *args = p_to_argbuf(argv), *envs = p_to_argbuf(envp);
	uint16_t pid = 0;
	int n = __proc_spawn(get_uapi_tid(), &pid, filename, args, envs);
	free(args);
	free(envs);
	if(n != 0) {
		errno = n > 0 ? -EIO : -n;
		return -1;
	}

	return pid;
}
