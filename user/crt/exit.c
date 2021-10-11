#include <stdlib.h>
#include <stdnoreturn.h>
#include <sneks/sysinfo.h>
#include <sneks/api/proc-defs.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include "private.h"


noreturn void _exit(int) __attribute__((weak, alias("_Exit")));


int atexit(void (*fn)(void))
{
	/* failure: implementation missing (oopsie) */
	return -1;
}


noreturn void exit(int status)
{
	/* TODO: call atexit sequence */
	_Exit(status);
}


noreturn void _Exit(int status)
{
	__proc_exit(__the_sysinfo->api.proc, status);
	/* the alternative. */
	for(;;) {
		asm volatile ("int $69");	/* the sex number */
		L4_Set_ExceptionHandler(L4_nilthread);
		asm volatile ("int $96");	/* the weird sex number */
		L4_Sleep(L4_Never);
	}
}
