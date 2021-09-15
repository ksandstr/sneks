
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ccan/minmax/minmax.h>
#include <ccan/intmap/intmap.h>
#include <ccan/darray/darray.h>

#include <l4/types.h>

#include <sneks/sysinfo.h>
#include <sneks/process.h>
#include <sneks/api/io-defs.h>
#include <sneks/api/proc-defs.h>

#include "private.h"


#define realloc_array(p, ns) do { \
		void *_tmp = realloc((p), sizeof (p)[0] * (ns)); \
		if(_tmp != NULL) (p) = _tmp; \
	} while(0)


struct exec_fds {
	size_t size, alloc;
	L4_Word_t *servers, *handles;
	int *fds;
};


typedef darray(char) chrbuf;


extern char **environ;


/* TODO: duplicates p_to_argbuf() in sys/crt/misc.c . this one is better. */
static char *pack_argbuf(char *const strp[])
{
	assert(strp != NULL);
	size_t alloc = 512, count = 0;
	char *buf = malloc(alloc);
	if(buf == NULL) return NULL;
	for(int i=0; strp[i] != NULL; i++) {
		int n;
		while(n = strscpy(buf + count, strp[i], alloc - count), n < 0) {
			alloc *= 2;
			char *nb = realloc(buf, alloc);
			if(nb == NULL) { free(buf); return NULL; }
			buf = nb;
		}
		count += n;
		buf[count++] = 0x1e;	/* ASCII Record Separator */
	}
	buf[count - 1] = '\0';
	return buf;
}


static bool collect_exec_fds(sintmap_index_t fd,
	struct fd_bits *bits, struct exec_fds *bufs)
{
	assert(bufs->alloc >= bufs->size);

	if(bits->flags & FD_CLOEXEC) return true;
	if(bufs->alloc == bufs->size) {
		size_t na = max(64 / sizeof(L4_ThreadId_t), bufs->alloc * 2);
		assert(na > bufs->size);
		errno = 0;
		realloc_array(bufs->servers, na);
		realloc_array(bufs->handles, na);
		realloc_array(bufs->fds, na);
		if(errno == ENOMEM) return false;
		bufs->alloc = na;
	}

	size_t i = bufs->size++;
	bufs->servers[i] = bits->server.raw;
	bufs->handles[i] = bits->handle;
	bufs->fds[i] = fd | (fd == __cwd_fd ? FF_CWD : 0);

	return true;
}


/* the meat & potatoes */
int fexecve(int fd, char *const argv[], char *const envp[])
{
	int n;

	struct fd_bits *bits = __fdbits(fd);
	if(bits == NULL) {
		errno = EBADF;
		return -1;
	}

	char *argbuf = NULL, *envbuf = NULL;
	argbuf = pack_argbuf(argv);
	if(argbuf == NULL) goto Enomem;
	envbuf = pack_argbuf(envp);
	if(envbuf == NULL) goto Enomem;
	struct exec_fds efs = { /* blanks */ };
	if(!sintmap_iterate(&fd_map, &collect_exec_fds, &efs)) goto Enomem;

	int proch;
	n = __io_dup_to(bits->server, &proch, bits->handle, pidof_NP(__the_sysinfo->api.proc));
	if(n == 0) {
		n = __proc_exec(__the_sysinfo->api.proc, bits->server.raw, proch, argbuf, envbuf,
			efs.servers, efs.size, efs.handles, efs.size, efs.fds, efs.size);
		__io_close(bits->server, proch);
	}

end:
	free(argbuf); free(envbuf);
	free(efs.servers); free(efs.handles); free(efs.fds);
	return NTOERR(n);

Enomem: n = -ENOMEM; goto end;
}


/* argument-vector variants, where "e" versions pass the given environment and
 * "p" versions examine the search path.
 */
int execvpe(const char *file, char *const argv[], char *const envp[])
{
	if(file == NULL) {
		errno = EFAULT;
		return -1;
	}
	if(file[0] == '/') return execve(file, argv, envp);

	const char *var = getenv("PATH");
	if(var == NULL) return execve(file, argv, envp);
	int var_len = strlen(var), file_len = strlen(file);
	char *copy = malloc(var_len * 2 + file_len + 3);
	if(copy == NULL) return -1;
	memcpy(copy, var, var_len + 1);
	char *tmp = copy + var_len + 1;

	/* execve() for each part. */
	bool go = true, eacces = false;
	for(char *path = copy; go && path[0] != '\0'; path += strlen(path) + 1) {
		char *colon = strchr(path, ':');
		if(colon != NULL) *colon = '\0';
		snprintf(tmp, var_len + file_len + 2, "%s/%s", path, file);
		execve(tmp, argv, envp);
		switch(errno) {
			case EACCES: eacces = true;	/* remember this one, and FALL THRU */
			case ENOENT: break;
			case ENOEXEC: /* TODO: run /bin/sh on the file, and FALL THRU */
			default: go = false;
		}
	}
	free(copy);

	errno = eacces ? EACCES : ENOENT;
	return -1;
}


int execvp(const char *file, char *const argv[]) {
	return execvpe(file, argv, environ);
}


int execve(const char *pathname, char *const argv[], char *const envp[])
{
	int fd = openat(AT_FDCWD, pathname, O_RDONLY | O_CLOEXEC);
	if(fd >= 0) {
		fexecve(fd, argv, envp);
		close(fd);
	}
	return -1;
}


int execv(const char *pathname, char *const argv[]) {
	return execve(pathname, argv, environ);
}


/* varargs porcelain. */

static char **get_argv(const char *arg0, va_list al)
{
	va_list copy; va_copy(copy, al);
	int argc = 1;
	while(va_arg(copy, char *) != NULL) argc++;
	va_end(copy); va_copy(copy, al);
	char **argv = calloc(argc + 2, sizeof *argv);
	if(argv != NULL) {
		argv[0] = (char *)arg0;
		for(int i=1; i <= argc; i++) argv[i] = va_arg(copy, char *);
		argv[argc + 1] = NULL;
	}
	va_end(copy);
	return argv;
}


int execl(const char *pathname, const char *arg0, ... /* (char *)NULL */)
{
	va_list al;
	va_start(al, arg0);
	char **argv = get_argv(arg0, al);
	va_end(al);
	if(argv == NULL) {
		errno = ENOMEM;
		return -1;
	}

	int n = execv(pathname, argv);
	assert(n < 0);
	free(argv);
	return n;
}


int execlp(const char *file, const char *arg0, ... /* (char *)NULL */)
{
	va_list al;
	va_start(al, arg0);
	char **argv = get_argv(arg0, al);
	va_end(al);
	if(argv == NULL) {
		errno = ENOMEM;
		return -1;
	}

	int n = execvp(file, argv);
	assert(n < 0);
	free(argv);
	return n;
}


int execle(const char *pathname, const char *arg0,
	... /* (char *)NULL, char *const envp[] */)
{
	va_list al;
	va_start(al, arg0);
	char **argv = get_argv(arg0, al);
	if(argv == NULL) {
		va_end(al);
		errno = ENOMEM;
		return -1;
	}
	while(va_arg(al, char *) != NULL) { /* scan */ }
	char *const *envp = va_arg(al, char *const *);
	va_end(al);

	int n = execve(pathname, argv, envp);
	assert(n < 0);
	free(argv);
	return n;
}


/* ... and then we have this silly thing per Linux. */
int execveat(int dirfd, const char *pathname,
	char *const argv[], char *const envp[], int flags)
{
	const int pass_flags = AT_SYMLINK_NOFOLLOW,
		allow_flags = AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW;

	if(flags & ~allow_flags) {
		errno = EINVAL;
		return -1;
	}
	if(pathname[0] == '\0' && (flags & AT_EMPTY_PATH)) {
		return fexecve(dirfd, argv, envp);
	}

	/* TODO: use O_CLOEXEC, of course */
	int fd = openat(dirfd, pathname, O_RDONLY, (flags & pass_flags) | O_CLOEXEC);
	if(fd >= 0) {
		fexecve(fd, argv, envp);
		close(fd);
	}
	return -1;
}
