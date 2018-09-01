
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/ipc.h>
#include <l4/kdebug.h>

#include <sneks/mm.h>
#include <sneks/sysinfo.h>

#include "private.h"


/* i don't see where these should go, so let's put them here. -ks */
L4_KernelInterfacePage_t *__the_kip = NULL;
struct __sysinfo *__the_sysinfo = NULL;


void __return_from_main(int status)
{
	/* this truley just exits the main thread. other threads may remain if any
	 * were spawnt. (NOTE: this might not agree with the STDs. verify whether
	 * it does.)
	 */
	exit(status);
}


void abort(void)
{
	L4_KDB_PrintString("aborted!");
	for(;;) {
		asm volatile ("int $70");
		L4_Sleep(L4_TimePeriod(10000));
	}
}


void __assert_failure(
	const char *cond,
	const char *file, unsigned int line, const char *fn)
{
	printf("!!! assert(%s) failed in `%s' [%s:%u]\n", cond, fn, file, line);
	abort();
}


void malloc_panic(void) {
	L4_KDB_PrintString("malloc_panic() called!");
	abort();
}


void con_putstr(const char *str) {
	L4_KDB_PrintString((char *)str);
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


int __crt1_entry(uintptr_t fdlistptr)
{
	__the_kip = L4_GetKernelInterface();
	__the_sysinfo = __get_sysinfo(__the_kip);

	extern char _end;
	L4_Word_t argpos = ((L4_Word_t)&_end + PAGE_SIZE - 1) & ~PAGE_MASK;
	/* TODO: gee, it'd sure be nice to have realloc and such here for programs
	 * that're given a gopping enormous number of arguments. such as those
	 * non-primordial-goo ones, over there just past the horizon.
	 */
	char *argv[12], *arg = (char *)argpos;
	int n_args = 0;
	while(*arg != '\0' && n_args < ARRAY_SIZE(argv)) {
		argv[n_args++] = arg;
		arg += strlen(arg) + 1;
	}
	/* we don't care about envbuf for now, as there are no tests for that
	 * anyway.
	 */

	__file_init((void *)fdlistptr);
	/* printf() works after this line only. otherwise, use
	 * L4_KDB_PrintString() w/ fingers crossed.
	 */

	extern int main(int argc, char *argv[], char *envp[]);
	char *envs = NULL;
	return main(n_args, argv, &envs);
}
