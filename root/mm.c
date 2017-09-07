
/* broad roottask memory management. initial (sigma0) memory through sbrk(),
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


#define PAGE_BITS 12	/* FIXME: move somewhere else, choose properly */
#define PAGE_MASK ((1u << PAGE_BITS) - 1)
#define PAGE_SIZE (PAGE_MASK + 1)

#define N_PRE_HEAP 12	/* fpages recorded in early sbrk() */


static L4_Word_t heap_top = 0, heap_pos = 0;

static L4_Fpage_t pre_heap_pages[N_PRE_HEAP];
static int pre_heap_pos = 0;


static L4_Word_t find_phys_mem_top(void)
{
	L4_KernelInterfacePage_t *kip = L4_GetKernelInterface();
	L4_MemoryDesc_t *mds = (void *)kip + kip->MemoryInfo.MemDescPtr;
	int n_descs = kip->MemoryInfo.n;
	L4_Word_t high = 0;
	for(int i=0; i < n_descs; i++) {
		if(L4_IsMemoryDescVirtual(&mds[i])) continue;
		if(L4_MemoryDescType(&mds[i]) != L4_ConventionalMemoryType) continue;
		L4_Word_t size = L4_MemoryDescHigh(&mds[i])
			- L4_MemoryDescLow(&mds[i]) + 1;
		if(size < (1 << PAGE_BITS)) continue;
		high = max_t(L4_Word_t, L4_MemoryDescHigh(&mds[i]), high);
	}

	return high;
}


void *sbrk(intptr_t increment)
{
	if(unlikely(heap_top == 0)) {
		heap_top = find_phys_mem_top() + 1;
		printf("top of physical memory is %#lx\n", heap_top - 1);
		heap_top = ((heap_top + PAGE_MASK) & ~PAGE_MASK) - PAGE_SIZE;
		heap_pos = heap_top;
	}

	if(increment > 0) {
		increment = (increment + PAGE_MASK) & ~PAGE_MASK;
		L4_Fpage_t p = L4_Sigma0_GetPage(L4_nilthread,
			L4_Fpage(heap_pos - increment, increment));
		if(L4_IsNilFpage(p)) return NULL;
		assert(pre_heap_pos + 1 < ARRAY_SIZE(pre_heap_pages));
		pre_heap_pages[pre_heap_pos++] = p;
		printf("%s: p=%#lx:%#lx\n", __func__, L4_Address(p), L4_Size(p));
		heap_pos = L4_Address(p);
	} else if(increment < 0) {
		printf("%s: can't rewind yet (increment=%d)\n",
			__func__, (int)increment);
	}

	return (void *)heap_pos;
}
