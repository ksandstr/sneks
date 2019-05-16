
/* runtime stuff for the root task. */

#include <stdio.h>
#include <stdlib.h>
#include <ccan/compiler/compiler.h>

#include <sneks/console.h>

#include <l4/ipc.h>
#include <l4/kdebug.h>

/* FIXME: remove this (used for prototype of panic()) */
#include <ukernel/misc.h>


void __assert_failure(
	const char *cond, const char *file, unsigned int line, const char *fn)
{
	printf("!!! assert(%s) failed in `%s' [%s:%u]\n", cond, fn, file, line);
	abort();
}


void NORETURN panic(const char *msg)
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


/* for <sneks/console.h> */
void con_putstr(const char *string) {
	L4_KDB_PrintString((char *)string);
}


/* FIXME: this is stupid and dangerous, however, errno doesn't actually get
 * used for much inside roottask so it's probably fine. this should properly
 * be allocated per thread, see sys/crt/crt1.c for example.
 */
int *__errno_location(void)
{
	static int the_errno = 0;
	return &the_errno;
}
