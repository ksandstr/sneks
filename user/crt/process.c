
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ccan/darray/darray.h>
#include <ccan/intmap/intmap.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/process.h>
#include <sneks/sysinfo.h>
#include <sneks/api/proc-defs.h>

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


pid_t getpid(void) {
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


/* app shitcode ahoy! */
struct spawn_bufs {
	darray(int) fds;
	darray(L4_Word_t) handles, servers;
};


static bool collect_spawn_fds(sintmap_index_t fd, struct fd_bits *bits,
	struct spawn_bufs *bufs)
{
	/* all of the realloc */
	darray_push(bufs->fds, fd);
	darray_push(bufs->handles, (L4_Word_t)bits->handle);
	darray_push(bufs->servers, bits->server.raw);
	return true;
}


int spawn_NP(const char *filename, char *const argv[], char *const envp[])
{
	char *args = p_to_argbuf(argv), *envs = p_to_argbuf(envp);
	uint16_t new_pid = 0;
	/* TODO: don't propagate all the file descriptors, that's silly. stdout,
	 * stdin, stderr should suffice.
	 */
	struct spawn_bufs bufs = {
		.fds = darray_new(), .handles = darray_new(), .servers = darray_new(),
	};
	sintmap_iterate(&fd_map, &collect_spawn_fds, &bufs);
	int n = __proc_spawn(__the_sysinfo->api.proc, &new_pid, filename, args, envs,
		bufs.servers.item, bufs.servers.size, bufs.handles.item, bufs.handles.size,
		bufs.fds.item, bufs.fds.size);
	free(args); free(envs);
	darray_free(bufs.fds); darray_free(bufs.handles); darray_free(bufs.servers);

	return NTOERR(n, new_pid);
}


pid_t wait(int *status_p) {
	return waitpid(-1, status_p, 0);
}


pid_t waitpid(pid_t pid, int *wstatus, int options)
{
	siginfo_t si;
	int n = waitid(pid == -1 ? P_ALL : P_PID, pid, &si, options);
	if(n != 0) return n;
	if(wstatus != NULL) {
		switch(si.si_code) {
			case CLD_EXITED:
				*wstatus = (si.si_status & 0xff) << 1 | 1;
				assert(WIFEXITED(*wstatus));
				assert(WEXITSTATUS(*wstatus) == (si.si_status & 0xff));
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
	__permit_recv_interrupt();
	int n = __proc_wait(__the_sysinfo->api.proc,
		&si->si_pid, &si->si_uid, &si->si_signo,
		&si->si_status, &si->si_code,
		idtype, id, options);
	__forbid_recv_interrupt();
	return NTOERR(n);
}
