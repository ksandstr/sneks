
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/darray/darray.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/process.h>

#include "proc-defs.h"
#include "private.h"


/* copypasta'd from sys/crt/misc.c .
 * TODO: deduplicate them somewhere.
 */
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


int getpid(void) {
	/* well isn't that just fucking twee */
	return pidof_NP(L4_MyGlobalId());
}


void exit(int status)
{
	int n = __proc_exit(__the_sysinfo->api.proc, status);
	fprintf(stderr, "Proc::exit returned n=%d\n", n);
	/* the alternative. */
	for(;;) {
		asm volatile ("int $69");	/* the sex number */
		L4_Set_ExceptionHandler(L4_nilthread);
		asm volatile ("int $96");	/* the weird sex number */
		L4_Sleep(L4_Never);
	}
}


int atexit(void (*fn)(void))
{
	/* failure: implementation missing (oopsie) */
	return -1;
}


/* copypasta'd from sys/crt/misc.c , caveats apply */
int spawn_NP(const char *filename, char *const argv[], char *const envp[])
{
	char *args = p_to_argbuf(argv), *envs = p_to_argbuf(envp);
	uint16_t pid = 0;
	/* app shitcode ahoy!
	 *
	 * FIXME: also, don't propagate all the file descriptors. that's silly.
	 * stdout, stdin, stderr should suffice. however since sneks doesn't
	 * really have any file descriptors besides those three, this'll do for
	 * now.
	 */
	darray(int32_t) fds = darray_new();
	darray(L4_Word_t) cookies = darray_new(), servs = darray_new();
	for(int i=0; i <= __max_valid_fd; i++) {
		if(!IS_FD_VALID(i)) continue;
		darray_push(fds, i);
		darray_push(cookies, FD_COOKIE(i));
		darray_push(servs, FD_SERVICE(i).raw);
	}
	int n = __proc_spawn(__the_sysinfo->api.proc, &pid, filename, args, envs,
		servs.item, servs.size, cookies.item, cookies.size,
		fds.item, fds.size);
	free(args); free(envs);
	darray_free(fds); darray_free(cookies); darray_free(servs);
	if(n != 0) {
		errno = n > 0 ? -EIO : -n;
		return -1;
	}

	return pid;
}


pid_t wait(int *status_p) {
	return waitpid(-1, status_p, 0);
}


pid_t waitpid(pid_t pid, int *wstatus, int options)
{
	siginfo_t si;
	int n = waitid(pid == -1 ? P_ANY : P_PID, pid, &si, options);
	if(n != 0) return n;
	if(wstatus != NULL) {
		switch(si.si_code) {
			case CLD_EXITED:
				*wstatus = si.si_status << 1 | 1;
				assert(WIFEXITED(*wstatus));
				assert(WEXITSTATUS(*wstatus) == si.si_status);
				assert(!WIFSIGNALED(*wstatus));
				assert(!WCOREDUMP(*wstatus));
				break;
			case CLD_KILLED:
				*wstatus = si.si_signo << 2;
				assert(WIFSIGNALED(*wstatus));
				assert(WTERMSIG(*wstatus) == si.si_signo);
				assert(!WIFEXITED(*wstatus));
				assert(!WCOREDUMP(*wstatus));
				break;
			case CLD_DUMPED:
				*wstatus = si.si_status << 2 | 2;
				assert(WCOREDUMP(*wstatus));
				assert(!WIFSIGNALED(*wstatus));
				break;
			case CLD_STOPPED:
			case CLD_TRAPPED:
			case CLD_CONTINUED:
				fprintf(stderr, "%s: no support for stopped/trapped/continued\n",
					__func__);
				abort();
			default:
				*wstatus = 0;
				break;	/* what what */
		}
	}

	return si.si_pid;
}


int waitid(idtype_t idtype, id_t id, siginfo_t *si, int options)
{
	siginfo_t dummy;
	if(si == NULL) si = &dummy;
	int n = __proc_wait(__the_sysinfo->api.proc,
		&si->si_pid, &si->si_uid, &si->si_signo,
		&si->si_status, &si->si_code,
		idtype, id, options);
	if(n != 0) {
		/* FIXME: respond to interrupted IPC etc. by retrying or some such */
		errno = n < 0 ? -n : EIO;
		return -1;
	}
	return 0;
}
