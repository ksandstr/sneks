
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <ucontext.h>
#include <sys/auxv.h>
#include <ccan/array_size/array_size.h>
#include <ccan/likely/likely.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/kdebug.h>

#ifdef __SNEKS__
#include <sneks/mm.h>
#include <sneks/elf.h>
#include <asm/ucontext-offsets.h>
#else
#include <limits.h>
#endif

#include <sneks/process.h>
#include <sneks/sysinfo.h>

#include "private.h"


/* i don't see where these should go, so let's put them here. -ks */
L4_KernelInterfacePage_t *__the_kip = NULL;
struct __sysinfo *__the_sysinfo = NULL;

L4_ThreadId_t __main_tid;

char *program_invocation_name = NULL, *program_invocation_short_name = NULL;

static size_t flat_auxv[64] = { 0 };
static uint64_t auxv_present = 0;


extern char **environ;


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


#ifdef __SNEKS__
/* copypasta'd from sys/crt/crt1.c */
long sysconf(int name)
{
	switch(name) {
		case _SC_PAGESIZE: return PAGE_SIZE;
		case _SC_NPROCESSORS_ONLN: return 1;	/* FIXME: get from KIP! */
		case _SC_OPEN_MAX:
			/* return a conservative number, because CCAN pipecmd will iterate
			 * the range in the face of close(2) which would explode real hard
			 * if the entire range of 2**31 descriptors that sneks allows were
			 * crawled through. we won't be using many FDs in the native
			 * runtime anyway.
			 */
			return 256;
		default: errno = EINVAL; return -1;
	}
}
#endif


unsigned long getauxval(unsigned long name)
{
	if(name < ARRAY_SIZE(flat_auxv) && (auxv_present & (1ull << name))) {
		return flat_auxv[name];
	} else {
		errno = ENOENT;
		return 0;
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


void __attribute__((noinline)) __init_crt(char **, char *);

void __init_crt(char **envp, char *progname)
{
#ifdef __SNEKS__
	/* verify <asm/ucontext-offsets.h> */
	static_assert(offsetof(ucontext_t, uc_flags) == o_uc_flags);
	static_assert(offsetof(ucontext_t, uc_link) == o_uc_link);
	static_assert(offsetof(ucontext_t, uc_stack) == o_uc_stack);
	static_assert(offsetof(ucontext_t, uc_mcontext) == o_uc_mcontext);
	static_assert(offsetof(ucontext_t, uc_sigmask) == o_uc_sigmask);
	static_assert(offsetof(ucontext_t, __fpregs_mem) == o___fpregs_mem);
	static_assert(sizeof(ucontext_t) == ucontext_t_size);
	static_assert(offsetof(mcontext_t, gregs) == o_gregs);
	static_assert(offsetof(mcontext_t, fpregs) == o_fpregs);
	static_assert(offsetof(mcontext_t, oldmask) == o_oldmask);
	static_assert(offsetof(mcontext_t, cr2) == o_cr2);
#endif

	environ = envp;
	int envc = 0;
	while(envp[envc] != NULL) envc++;
	auxv_t *auxv = (auxv_t *)(envp + envc + 1);

	for(int i=0; auxv[i].a_type != AT_NULL; i++) {
		size_t name = auxv[i].a_type;
		if(name == AT_IGNORE) continue;
		flat_auxv[name] = auxv[i].a_val;
		auxv_present |= 1ull << name;
	}
	__init_crt_cached_creds(flat_auxv);

	program_invocation_name = progname;
	if((auxv_present & (1ull << AT_EXECFN)) && flat_auxv[AT_EXECFN] != 0) {
		program_invocation_short_name = (char *)flat_auxv[AT_EXECFN];
	} else {
		char *slash = strrchr(progname, '/');
		program_invocation_short_name = slash != NULL ? slash + 1 : progname;
	}

	for(int i=0; i <= 2; i++) {
		if(__fdbits(i) != NULL) continue;
		int fd = open("/dev/null", O_RDWR);
		if(fd < 0) abort();
		assert(fd == i);
	}
	stdin = fdopen(0, "r");
	stdout = fdopen(1, "w"); setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
	stderr = fdopen(2, "w"); setvbuf(stderr, NULL, _IONBF, 0);
	/* printf() works now. */

	if(__cwd_fd < 0 && chdir("/") < 0) abort();
}


int __libc_start_main(int (*mainfn)(int, char **, char **), int argc, char *argv[])
{
	char **envp = argv + argc + 1;
	__init_crt(envp, argv[0]);
	atomic_thread_fence(memory_order_seq_cst);
	/* TODO: _init, etc. */
	return (*mainfn)(argc, argv, envp);
}


/* entrypoint from crt0 */
void _start_c(long *p)
{
	int argc = *p;
	char **argv = (char **)(p + 1);

	extern int main(int, char **, char **);
	exit(__libc_start_main(&main, argc, argv));
}
