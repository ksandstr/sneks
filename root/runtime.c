
/* runtime stuff for the root task. */

#include <stdio.h>
#include <stdlib.h>
#include <ccan/compiler/compiler.h>

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


void abort(void) {
	panic("aborted");
}


/* special non-portable prototype in the fake <stdio.h> */
void con_putstr(const char *string) {
	L4_KDB_PrintString((char *)string);
}


/* useless malloc and free. */
void *malloc(size_t size) {
	return NULL;
}


void free(void *ptr) {
	/* jack shit! */
}
