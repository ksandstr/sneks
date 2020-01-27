
/* odds and ends without a great enough theme to warrant a module. */

#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <setjmp.h>
#include <threads.h>
#include <errno.h>
#include <assert.h>
#include <sneks/process.h>
#include <sneks/systask.h>

#include "proc-defs.h"
#include "private.h"


#define RECSEP 0x1e	/* ASCII record separator control character. whee! */


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


int spawn_NP(const char *filename, char *const argv[], char *const envp[])
{
	char *args = p_to_argbuf(argv), *envs = p_to_argbuf(envp);
	uint16_t pid = 0;
	/* FIXME: pass stdfoo fds from somewhere... */
	int n = __proc_spawn(__uapi_tid, &pid, filename, args, envs,
		NULL, 0, NULL, 0, NULL, 0);
	free(args);
	free(envs);
	if(n != 0) {
		errno = n > 0 ? -EIO : -n;
		return -1;
	}

	return pid;
}


noreturn void longjmp(jmp_buf env, int val)
{
	extern noreturn void __longjmp_actual(jmp_buf, int);
	__longjmp_actual(env, val);
}
