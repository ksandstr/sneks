
/* runtime stuff for the root task. things such as compat for epoch handling
 * and so forth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <ccan/compiler/compiler.h>

#include <sneks/console.h>

#include <l4/ipc.h>
#include <l4/kip.h>
#include <l4/thread.h>
#include <l4/kdebug.h>

/* FIXME: remove this (used for prototype of panic()) */
#include <ukernel/misc.h>

#include "defs.h"


void __assert_failure(
	const char *cond, const char *file, unsigned int line, const char *fn)
{
	printf("!!! assert(%s) failed in `%s' [%s:%u]\n", cond, fn, file, line);
	abort();
}


noreturn void panic(const char *msg)
{
	printf("!!! PANIC: %s\n", msg);
	for(;;) { L4_Sleep(L4_Never); }
}


void malloc_panic(void) {
	panic("malloc_panic() called!");
}


void abort(void) {
	panic("aborted");
}


noreturn void exit(int rc)
{
	printf("root: called exit(%d)??\n", rc);
	abort();
}


/* for <sneks/console.h> */
void con_putstr(const char *string) {
	L4_KDB_PrintString((char *)string);
}


/* TODO: this is better than the one in the two C runtimes. copy it over, or
 * move into lib/ .
 */
long sysconf(int name)
{
	switch(name) {
		case _SC_PAGESIZE:
			return 1 << (ffsll(L4_PageSizeMask(the_kip)) - 1);
		case _SC_NPROCESSORS_ONLN:
			return the_kip->ProcessorInfo.X.processors + 1;
		case _SC_OPEN_MAX: return 0;	/* root has no fds */
		default:
			errno = EINVAL;
			return -1;
	}
}


int sched_getcpu(void) {
	return L4_ProcessorNo();
}
