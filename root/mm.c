
/* broad roottask memory management. boot-time (sigma0) memory through sbrk(),
 * integration with sysmem.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>

#include <l4/types.h>
#include <l4/sigma0.h>
#include <l4/kip.h>

#include <sneks/mm.h>
#include <sneks/bitops.h>

#include "sysmem-defs.h"
#include "defs.h"


#define N_PRE_HEAP 30	/* max # of fpages recorded in early sbrk() */


static uintptr_t current_brk = 0, heap_bottom;

static L4_Fpage_t pre_heap_pages[N_PRE_HEAP];
static int pre_heap_pos = 0;	/* < 0 indicates sysmem paging */


void *sbrk(intptr_t increment)
{
	if(unlikely(current_brk == 0)) {
		extern char _end;
		current_brk = ((L4_Word_t)&_end + PAGE_MASK) & ~PAGE_MASK;
		heap_bottom = current_brk - 1;
		printf("first current_brk=%#x\n", current_brk);
	}

	void *ret = (void *)current_brk;
	if(increment > 0) {
		current_brk = (current_brk + increment + PAGE_MASK) & ~PAGE_MASK;
		if(unlikely(pre_heap_pos >= 0)) {
			/* pre-sysmem heap handling. */
			int size_log2;
			L4_Word_t address, start = (L4_Word_t)ret;
			for_page_range(start, current_brk, address, size_log2) {
				L4_Fpage_t p = L4_Sigma0_GetPage(L4_nilthread,
					L4_FpageLog2(address, size_log2));
				if(L4_IsNilFpage(p)) {
					printf("s0 GetPage failed! ec=%lu\n", L4_ErrorCode());
					abort();
				}
				assert(L4_Address(p) == address);
				assert(L4_SizeLog2(p) == size_log2);
				pre_heap_pages[pre_heap_pos++] = p;
			}
		}
	} else if(increment < 0) {
		increment = (-increment + PAGE_MASK) & ~PAGE_MASK;
		current_brk -= increment;
		if(current_brk < heap_bottom) current_brk = heap_bottom;
	}

	if(increment != 0 && likely(pre_heap_pos < 0)) {
		int n = __sysmem_brk(L4_Pager(), current_brk);
		if(n != 0) {
			printf("__sysmem_brk() failed, n=%d\n", n);
			abort();
		}
	}

	return ret;
}


void mm_enable_sysmem(L4_ThreadId_t sysmem_tid)
{
	/* send me your money! */
	for(int i=0; i < pre_heap_pos; i++) {
		/* for now, sysmem only does single pages at a time. this should be
		 * changed once it gains leeter skillz.
		 */
		L4_Fpage_t fp = pre_heap_pages[i];
		for(uintptr_t addr = L4_Address(fp);
			addr < L4_Address(fp) + L4_Size(fp);
			addr += PAGE_SIZE)
		{
			send_phys_to_sysmem(sysmem_tid, true, addr);
		}
	}
	pre_heap_pos = -1;

	int n = __sysmem_brk(sysmem_tid, current_brk);
	if(n != 0) {
		printf("%s: can't into __sysmem_brk(): n=%d\n", __func__, n);
		abort();
	}
}
