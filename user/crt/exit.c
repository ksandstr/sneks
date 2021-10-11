#include <stdlib.h>
#include <stdnoreturn.h>
#include <sneks/sysinfo.h>
#include <sneks/api/proc-defs.h>
#include <ccan/minmax/minmax.h>

#include <l4/types.h>
#include <l4/ipc.h>

#include "private.h"


typedef void (*atexit_fn)(void);

noreturn void _exit(int) __attribute__((weak, alias("_Exit")));


static atexit_fn *exitfns = NULL;
static int exitfns_size = 0, exitfns_alloc = 0;


int atexit(atexit_fn fn)
{
	if(fn == NULL) return 0;
	if(exitfns_size + 1 >= exitfns_alloc) {
		exitfns_alloc = max(exitfns_alloc * 2, 16);
		atexit_fn *new = realloc(exitfns, exitfns_alloc * sizeof *exitfns);
		if(new == NULL) {
			exitfns_alloc = exitfns_size;
			return -1;
		}
		exitfns = new;
	}

	exitfns[exitfns_size++] = fn;
	return 0;
}


noreturn void exit(int status)
{
	for(int i = exitfns_size - 1; i >= 0; i--) {
		(*exitfns[i])();
	}
	/* as for exitfns, we release its memory like a cruise missile. */
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
