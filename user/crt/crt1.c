
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <ccan/array_size/array_size.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/kdebug.h>

#include <sneks/mm.h>
#include <sneks/process.h>
#include <sneks/sysinfo.h>

#include "private.h"


/* i don't see where these should go, so let's put them here. -ks */
L4_KernelInterfacePage_t *__the_kip = NULL;
struct __sysinfo *__the_sysinfo = NULL;

L4_ThreadId_t __main_tid;


void __assert_failure(
	const char *cond,
	const char *file, unsigned int line, const char *fn)
{
	fprintf(stderr, "!!! assert(%s) failed in `%s' [%s:%u]\n",
		cond, fn, file, line);
	abort();
}


void malloc_panic(void) {
	L4_KDB_PrintString("user/crt: malloc_panic() called!");
	abort();
}


/* copypasta'd from sys/crt/crt1.c */
long sysconf(int name)
{
	switch(name) {
		case _SC_PAGESIZE: return PAGE_SIZE;
		case _SC_NPROCESSORS_ONLN: return 1;	/* FIXME: get from KIP! */
		default: errno = EINVAL; return -1;
	}
}


/* TODO: move this wherever */
int sched_yield(void) {
	L4_ThreadSwitch(L4_nilthread);
	return 0;
}


int sched_getcpu(void) {
	return L4_ProcessorNo();
}


static size_t nstrlen(int *count_p, const char *base)
{
	int count = 0;
	const char *s = base;
	while(*s != '\0') {
		count++;
		s += strlen(s) + 1;
	}
	if(count_p != NULL) *count_p = count;
	return s - base + 1;
}


static uintptr_t fdlist_last(uintptr_t fdlistptr)
{
	if(fdlistptr == 0) return 0;
	struct sneks_fdlist *list = (void *)fdlistptr;
	while(list->next != 0) list = sneks_fdlist_next(list);
	return (uintptr_t)&list[1];
}


static char **unpack_argpage(
	L4_Word_t base, int buflen, int n_strs, bool terminate)
{
	char **strv;
	int n_alloc = terminate ? n_strs + 1 : n_strs;
	if(PAGE_SIZE - (buflen & PAGE_MASK) > n_alloc * sizeof(char *) + 8) {
		strv = (char **)((base + buflen + 7) & ~7);
	} else {
		strv = malloc(n_alloc * sizeof *strv);
		if(strv == NULL) abort();
	}
	char *str = (char *)base;
	for(int i=0; *str != '\0'; str += strlen(str) + 1, i++) {
		assert(i <= n_strs);
		strv[i] = str;
	}
	if(terminate) strv[n_strs] = NULL;
	return strv;
}


static char *consume_env(char **envp, const char *key)
{
	int key_len = strlen(key), found = -1;
	char *eq = NULL;
	for(int i=0; envp[i] != NULL; i++) {
		eq = strchr(envp[i], '=');
		if(eq == NULL) continue;
		if(eq - envp[i] == key_len && memcmp(key, envp[i], key_len) == 0) {
			found = i;
			break;
		}
	}
	if(found < 0) return NULL;

	for(int i = found; envp[i] != NULL; i++) envp[i] = envp[i + 1];

	*eq = '\0';
	return &eq[1];
}


static bool use_cwd_handle(char **envp)
{
	char *var = consume_env(envp, "__CWD_HANDLE");
	if(var == NULL) return false;

	/* TODO: replace with sscanf() */
	char *colon = strchr(var, ':'),
		*comma = strchr(colon != NULL ? colon + 1 : var, ',');
	if(colon != NULL) *colon = '\0';
	if(comma != NULL) *comma = '\0';
	unsigned long tno = strtoul(var, NULL, 0),
		version = colon != NULL ? strtoul(colon + 1, NULL, 0) : 0,
		handle = comma != NULL ? strtoul(comma + 1, NULL, 0) : 0;
	L4_ThreadId_t server = L4_GlobalId(tno, version);
	if(L4_IsLocalId(server)) fprintf(stderr, "crt1: UwU what's this?\n");
	int fd = __create_fd(-1, server, handle, 0);
	if(unlikely(fd < 0)) {
		fprintf(stderr, "crt1:%s: __create_fd() failed, n=%d\n", __func__, fd);
		return false;
	}
	if(unlikely(fchdir(fd) < 0)) {
		fprintf(stderr, "crt1:%s: fchdir() failed, errno=%d\n", __func__, errno);
		return false;
	}
	close(fd);

	return true;
}


static void init_cwd(char **envp)
{
	char *curdir = consume_env(envp, "__CWD_PATH");
	if(curdir != NULL) {
		int n = chdir(curdir);
		if(n < 0) {
			fprintf(stderr, "crt1: chdir to `%s' failed, errno=%d\n", curdir, errno);
			curdir = NULL;
		}
	}
	if(curdir == NULL && !use_cwd_handle(envp)) {
		/* if all else fails, silently default to "/". */
		if(chdir("/") < 0) {
			fprintf(stderr, "crt1: chdir to '/' failed, errno=%d\n", errno);
			abort();	/* screw you guys, i'm going home */
		}
	}
}


int __crt1_entry(uintptr_t fdlistptr)
{
	__the_kip = L4_GetKernelInterface();
	__the_sysinfo = __get_sysinfo(__the_kip);
	__main_tid = L4_MyLocalId();

	extern char _end;
	L4_Word_t argpos = ((L4_Word_t)&_end + PAGE_SIZE - 1) & ~PAGE_MASK;

	/* the way that argument passing works at the moment is silly: UAPI puts a
	 * flattened array of argv[] on the page after &_end, and then the same
	 * for envp[] on the page after that array ends. fdlists go after that.
	 *
	 * to parse this, all userspace programs must start by parsing through all
	 * three buffers and then choosing the page after the last of these ends
	 * for the very first program break. this is fucktarded, and UAPI should
	 * be changed to pass a pointer to a <struct sneks_arghdr> or some such
	 * where fdlistptr goes right now; uapi_spawn() crawls over its parameters
	 * to produce the silly version already, so it's also a performance insult.
	 *
	 * TODO: replace all of this with something reasonable, i.e. doing all of
	 * this work in UAPI already.
	 */

	L4_Word_t args_base = argpos;
	int n_args, argbuflen = nstrlen(&n_args, (char *)args_base);
	L4_Word_t envs_base = (argpos + argbuflen + PAGE_SIZE - 1) & ~PAGE_MASK;
	int n_envs, envbuflen = nstrlen(&n_envs, (char *)envs_base);
	L4_Word_t fdlist_end = fdlist_last(fdlistptr);
	brk((void *)max((envs_base + PAGE_SIZE - 1) & ~PAGE_MASK,
		(fdlist_end + PAGE_SIZE - 1) & ~PAGE_MASK));
	/* right. malloc is now good. */

	__file_init((void *)fdlistptr);
	/* printf() works after this line only. otherwise, use
	 * L4_KDB_PrintString() w/ fingers crossed.
	 */
	__init_crt_cached_creds();

	char **envp = unpack_argpage(envs_base, envbuflen, n_envs, true);
	init_cwd(envp);

	extern int main(int argc, char *argv[], char *envp[]);
	/* FIXME: stick the unpacked argpage into putenv() one at a time, once
	 * either lib/env.c drops the htable requirement or lib/ starts including
	 * htable. for now, spawn environment doesn't show up in getenv().
	 */
	return main(n_args, unpack_argpage(args_base, argbuflen, n_args, false), envp);
}
