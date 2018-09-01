
#include <stdlib.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>

#include <sneks/process.h>

#include "proc-defs.h"


int getpid(void) {
	/* well isn't that just fucking twee */
	return pidof_NP(L4_MyGlobalId());
}


void exit(int status)
{
	/* TODO: call Proc::exit instead, once the service is known (per
	 * sysinfopage)
	 */
	for(;;) {
		asm volatile ("int $69");	/* the sex number */
		L4_Sleep(L4_Never);
	}
}


int atexit(void (*fn)(void))
{
	/* failure: implementation missing (oopsie) */
	return -1;
}
