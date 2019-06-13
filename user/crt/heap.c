
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <l4/thread.h>
#include <sneks/mm.h>

#include "vm-defs.h"


static uintptr_t current_brk = 0, heap_bottom = 0;


int brk(void *addr)
{
	uintptr_t old_brk = current_brk;
	current_brk = (uintptr_t)addr;
	if(heap_bottom == 0 || heap_bottom > current_brk) {
		heap_bottom = current_brk;
	}

	if(current_brk != old_brk) {
		/* FIXME: our pager might not be vm. get vm's thread ID from the SIP
		 * instead.
		 */
		int n = __vm_brk(L4_Pager(), current_brk);
		if(n != 0) {
			if(n > 0 || n != -ENOMEM) {
				/* FIXME: recover POSIX error */
				printf("VM::brk failed, n=%d\n", n);
				abort();
			}
			errno = ENOMEM;
			return -1;
		}
	}

	return 0;
}


void *sbrk(intptr_t increment)
{
	if(increment == 0) return (void *)current_brk;
	assert(current_brk > 0);

	void *ret = (void *)current_brk;
	uintptr_t new_brk;
	if(increment > 0) {
		new_brk = (current_brk + increment + PAGE_MASK) & ~PAGE_MASK;
	} else {
		assert(increment < 0);
		increment = (-increment + PAGE_MASK) & ~PAGE_MASK;
		new_brk = current_brk - increment;
	}
	if(brk((void *)new_brk) != 0) ret = (void *)-1;
	return ret;
}
