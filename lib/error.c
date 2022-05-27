#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ccan/array_size/array_size.h>

const char *const sys_errlist[] = {
	[EPERM] = "operation not permitted",
	[ENOENT] = "no such file or directory",
	[ESRCH] = "no such process",
	[EINTR] = "interrupted system call",
	[EIO] = "i/o error",
	[E2BIG] = "argument list too long",
	[ENOEXEC] = "executable format error",
	[EBADF] = "bad file number",
	[ECHILD] = "no child processes",
	[EAGAIN] = "try again",
	[ENOMEM] = "out of memory",
	[EACCES] = "access check failed",
	[EFAULT] = "bad address",
	[EBUSY] = "device or resource busy",
	[EEXIST] = "file exists",
	[ENODEV] = "no such device",
	[ENOTDIR] = "not a directory",
	[EISDIR] = "is a directory",
	[EINVAL] = "invalid value",
	[ENFILE] = "file table overflow (system)",
	[EMFILE] = "too many open files (process)",
	[ENOTTY] = "not a typewriter",
	[ENOSPC] = "no space left on device",
	[ESPIPE] = "illegal seek",
	[EROFS] = "read-only file system",
	[EPIPE] = "broken pipe",
	[ERANGE] = "out of range",
	[EDEADLK] = "deadlock",
	[ENAMETOOLONG] = "name too long",
	[ENOSYS] = "function not implemented",
	[ELOOP] = "loop",
	[EWOULDBLOCK] = "would block",
	[EOVERFLOW] = "overflow",
	[ETIMEDOUT] = "timed out",
};
int sys_nerr = ARRAY_SIZE(sys_errlist);

char *strerror(int err) {
	if(err < 0 || err >= ARRAY_SIZE(sys_errlist)) { errno = EINVAL; return "(unknown error)"; }
	return sys_errlist[err] == NULL ? "(unknown error)" : (char *)sys_errlist[err];
}

void perror(const char *where) {
	if(where != NULL && where[0] != '\0') {
		fprintf(stderr, "%s: %s\n", where, strerror(errno));
	} else {
		fprintf(stderr, "%s\n", strerror(errno));
	}
}

char *stripcerr_NP(int n)
{
	if(n < 0) return strerror(-n);
	static char *const ipcerrs[] = {
		[0] = "ok", [1] = "receive-phase success (?)",
		[2] = "send timeout", [3] = "recv timeout",
		[4] = "dest not exist", [5] = "src not exist",
		[6] = "send cancel", [7] = "recv cancel",
		[8] = "overflow in send", [9] = "overflow in recv",
		[10] = "own xfer timeout in send", [11] = "own xfer timeout in recv",
		[12] = "partner xfer timeout in send", [13] = "partner xfer timeout in recv",
		[14] = "send aborted", [15] = "recv aborted",
	};
	return n < ARRAY_SIZE(ipcerrs) ? ipcerrs[n] : "(unknown error)";
}

void perror_ipc_NP(const char *where, int n) {
	if(where != NULL && where[0] != '\0') {
		fprintf(stderr, "%s: %s\n", where, stripcerr_NP(n));
	} else {
		fprintf(stderr, "%s\n", stripcerr_NP(n));
	}
}
