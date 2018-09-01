
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

#include <l4/thread.h>
#include <sneks/mm.h>

#include "vm-defs.h"


static uintptr_t current_brk = 0, heap_bottom = 0;


void *sbrk(intptr_t increment)
{
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
		/* FIXME: our pager might not be vm. get vm's thread ID from the
		 * SIP instead.
		 */
		int n = __vm_brk(L4_Pager(), current_brk);
		if(n != 0) {
			/* FIXME: recover POSIX error, return (void *)-1 */
			printf("VM::brk failed, n=%d\n", n);
			abort();
		}
	}

	return ret;
}
