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
#include <sneks/api/io-defs.h>
#include <sneks/api/vm-defs.h>
#include "private.h"

static uintptr_t current_brk = 0, heap_bottom = 0;

int brk(void *addr)
{
	uintptr_t old_brk = current_brk;
	current_brk = (uintptr_t)addr;
	if(heap_bottom == 0 || heap_bottom > current_brk) heap_bottom = current_brk;
	if(current_brk != old_brk) {
		int n = __vm_brk(L4_Pager(), current_brk);
		if(n != 0) {
			if(n != -ENOMEM) { perror_ipc_NP("VM::brk", n); abort(); }
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

void *mmap(void *_addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	L4_ThreadId_t server = L4_nilthread;
	int copy = 0, n;
	if(~flags & MAP_ANONYMOUS) {
		struct fd_bits *f = __fdbits(fd);
		if(f == NULL) { errno = EBADF; return MAP_FAILED; }
		if(n = __io_dup_to(f->server, &copy, f->handle, pidof_NP(L4_Pager())), n != 0) return NTOERR(n), MAP_FAILED;
		server = f->server;
	}
	L4_Word_t addr = (L4_Word_t)_addr;
	n = __vm_mmap(L4_Pager(), 0, &addr, length, prot, flags, server.raw, copy, offset);
	if(n != 0 && (~flags & MAP_ANONYMOUS)) __io_close(server, copy);
	return n == 0 ? (void *)addr : (NTOERR(n), MAP_FAILED);
}

int munmap(void *addr, size_t length) {
	int n = __vm_munmap(L4_Pager(), (L4_Word_t)addr, length);
	return NTOERR(n);
}
