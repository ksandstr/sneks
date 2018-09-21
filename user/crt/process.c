
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/process.h>

#include "proc-defs.h"
#include "private.h"


int getpid(void) {
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


pid_t wait(int *status_p)
{
	return (pid_t)-1;
}
