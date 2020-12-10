
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
#include <ccan/compiler/compiler.h>

#include <l4/types.h>
#include <l4/sigma0.h>
#include <l4/kip.h>

#include <sneks/mm.h>
#include <sneks/bitops.h>
#include <sneks/sys/sysmem-defs.h>

#include "defs.h"


#define N_PRE_HEAP 48	/* max # of fpages recorded in early sbrk() */


static uintptr_t current_brk = 0, heap_bottom;

static L4_Fpage_t pre_heap_pages[N_PRE_HEAP];
static int pre_heap_pos = 0;	/* < 0 indicates sysmem paging */

static L4_ThreadId_t sysmem_tid;


static void get_more_memory(L4_Word_t start)
{
	/* get physical memory from sigma0 in case it's not been pumped yet. */
	const bool have_sysmem = pre_heap_pos < 0;
	int size_log2;
	L4_Word_t address;
	for_page_range(start, current_brk, address, size_log2) {
		/* sigma0 returns a page within the range, which is the page we're
		 * asking for iff there's no applicable shrapnel yet. so we should
		 * keep asking until we either get the exact request, or nilpage
		 * once the range has emptied.
		 */
		L4_Fpage_t p;
		do {
			p = L4_Sigma0_GetPage(L4_nilthread,
				L4_FpageLog2(address, size_log2));
			if(L4_IsNilFpage(p)) {
				if(have_sysmem) break;
				printf("s0 GetPage failed! ec=%lu\n", L4_ErrorCode());
				abort();
			}
			memset((void *)L4_Address(p), '\0', L4_Size(p));
			if(have_sysmem) {
				/* transfer immediately. */
				extern L4_ThreadId_t sysmem_tid;	/* FIXME: terrible hack */
				send_phys_to_sysmem(sysmem_tid, true, p);
			} else {
				/* defer until mm_enable_sysmem(). */
				if(pre_heap_pos == N_PRE_HEAP) {
					printf("%s: ran out of pre-heap pages!\n", __func__);
					abort();
				}
				assert(pre_heap_pos < N_PRE_HEAP);
				pre_heap_pages[pre_heap_pos++] = p;
				/* we can require this in pristine pre-sysmem land. */
				assert(L4_Address(p) == address && L4_SizeLog2(p) == size_log2);
			}
		} while(L4_Address(p) != address || L4_SizeLog2(p) != size_log2);
	}
}


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
		get_more_memory((L4_Word_t)ret);
	} else if(increment < 0) {
		/* FIXME: before sysmem, get_more_memory() will break on pages it has
		 * already taken but which it expects sigma0 to hand out again.
		 */
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


COLD void mm_enable_sysmem(L4_ThreadId_t sm_tid)
{
	sysmem_tid = sm_tid;
	/* send me your money! */
	for(int i=0; i < pre_heap_pos; i++) {
		send_phys_to_sysmem(sysmem_tid, true, pre_heap_pages[i]);
	}
	pre_heap_pos = -1;

	int n = __sysmem_brk(sysmem_tid, current_brk);
	if(n != 0) {
		printf("%s: can't into __sysmem_brk(): n=%d\n", __func__, n);
		abort();
	}
}
