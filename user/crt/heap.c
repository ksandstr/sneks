
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>

#ifndef __SNEKS__
#include <limits.h>
#else
#include <sneks/mm.h>
#endif

#include <l4/thread.h>
#include <sneks/api/vm-defs.h>

#include "private.h"


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
		new_brk = (current_brk + increment + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	} else {
		assert(increment < 0);
		increment = (-increment + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		new_brk = current_brk - increment;
	}
	if(brk((void *)new_brk) != 0) ret = (void *)-1;
	return ret;
}


void *mmap(
	void *_addr, size_t length, int prot, int flags,
	int fd, off_t offset)
{
	int n;
	if(~flags & MAP_ANONYMOUS) {
		/* TODO: file support */
		n = -ENOSYS;
		goto err;
	}

	L4_Word_t addr = (L4_Word_t)_addr;
	n = __vm_mmap(L4_Pager(), 0, &addr, length, prot, flags,
		L4_nilthread.raw, 0, offset);
	if(n != 0) goto err;

	return (void *)addr;

err:
	return NTOERR(n), MAP_FAILED;
}


int munmap(void *addr, size_t length)
{
	int n = __vm_munmap(L4_Pager(), (L4_Word_t)addr, length);
	return NTOERR(n);
}
