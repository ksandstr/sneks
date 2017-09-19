
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/kdebug.h>

#include <sneks/mm.h>

#include "sysmem-defs.h"


void __return_from_main(int main_rc)
{
	int n = __sysmem_rm_thread(L4_Pager(), L4_Myself().raw, L4_Myself().raw);
	assert(n != 0);
	L4_KDB_PrintString("systask __return_from_main failed!");
	for(;;) { L4_Sleep(L4_Never); }
}


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


/* for fake_stdio.c. this should change over to writing into a system dmesg,
 * or some such buffer, over IPC.
 */
void con_putstr(const char *string) {
	L4_KDB_PrintString((char *)string);
}


void *sbrk(intptr_t increment)
{
	static uintptr_t current_brk = 0, heap_bottom = 0;
	if(current_brk == 0) {
		extern char _end;
		current_brk = ((L4_Word_t)&_end + PAGE_MASK) & ~PAGE_MASK;
		heap_bottom = current_brk;
		assert(current_brk != 0);
	}

	void *ret = (void *)current_brk;
	if(increment > 0) {
		current_brk = (current_brk + increment + PAGE_MASK) & ~PAGE_MASK;
	} else if(increment < 0) {
		increment = (-increment + PAGE_MASK) & ~PAGE_MASK;
		current_brk -= increment;
	}
	if(increment != 0) {
		if(current_brk < heap_bottom) current_brk = heap_bottom;
		int n = __sysmem_brk(L4_Pager(), current_brk);
		if(n != 0) {
			printf("sbrk failed, n=%d\n", n);
			abort();
		}
	}

	return ret;
}
