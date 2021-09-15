
/* broad roottask memory management. boot-time (sigma0) memory through sbrk(),
 * integration with sysmem.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <ccan/likely/likely.h>
#include <ccan/minmax/minmax.h>
#include <ccan/array_size/array_size.h>
#include <ccan/compiler/compiler.h>

#include <l4/types.h>
#include <l4/sigma0.h>
#include <l4/kip.h>
#include <l4/bootinfo.h>

#include <sneks/elf.h>
#include <sneks/mm.h>
#include <sneks/bitops.h>
#include <sneks/sys/sysmem-defs.h>

#include "defs.h"


#define N_PRE_HEAP 48	/* max # of fpages recorded in early sbrk() */


static uintptr_t current_brk = 0, heap_bottom;

static L4_Fpage_t pre_heap_pages[N_PRE_HEAP];
static int pre_heap_pos = 0;	/* < 0 indicates sysmem paging */

L4_ThreadId_t sysmem_tid;


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
			p = L4_Sigma0_GetPage(L4_nilthread, L4_FpageLog2(address, size_log2));
			if(L4_IsNilFpage(p)) {
				if(have_sysmem) break;
				printf("s0 GetPage failed for %#lx:%#lx! ec=%lu\n", address,
					1ul << size_log2, L4_ErrorCode());
				abort();
			}
			memset((void *)L4_Address(p), '\0', L4_Size(p));
			if(have_sysmem) {
				/* transfer immediately. */
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


static uintptr_t last_load_address(L4_BootRec_t *rec)
{
	const Elf32_Ehdr *ee = (void *)L4_Module_Start(rec);
	if(memcmp(ee->e_ident, ELFMAG, SELFMAG) != 0) {
		printf("%s: incorrect ELF magic\n", __func__);
		abort();
	}
	uintptr_t phoff = ee->e_phoff, last = 0;
	for(int i=0; i < ee->e_phnum; i++, phoff += ee->e_phentsize) {
		const Elf32_Phdr *ep = (void *)(L4_Module_Start(rec) + phoff);
		if(ep->p_type == PT_LOAD) {
			last = max(last, (uintptr_t)ep->p_vaddr + ep->p_filesz - 1);
		}
	}
	return last;
}


/* crawl over the boot modules and determine the last address out of those
 * where modules were loaded at boot and those where modules requiring
 * idempotent mappings will be loaded.
 */
static uintptr_t last_module_address(void)
{
	L4_BootInfo_t *binf = (void *)L4_BootInfo(L4_GetKernelInterface());
	uintptr_t ret = (L4_Word_t)binf + sizeof *binf - 1;

	L4_BootRec_t *rec = L4_BootInfo_FirstEntry(binf);
	for(int i = 0, l = L4_BootInfo_Entries(binf); i < l; i++, rec = L4_BootRec_Next(rec)) {
		/* dodge bootinfo itself */
		ret = max_t(uintptr_t, ret, (L4_Word_t)rec + sizeof *rec - 1);
		if(rec->type != L4_BootInfo_Module) continue;

		/* dodge all modules */
		ret = max_t(uintptr_t, ret, L4_Module_Start(rec) + L4_Module_Size(rec) - 1);

		char *cmdline = L4_Module_Cmdline(rec);
		const char *slash = strrchr(cmdline, '/'), *space = strchr(cmdline, ' ');
		if(slash == NULL) slash = cmdline; else slash++;
		if(space == NULL) {
			space = strchr(slash, ' ');
			if(space == NULL) space = slash + strlen(slash);
		} else if(space < slash) {
			slash = space + 1;
		}
		char modname[32];
		int len = min_t(size_t, space - slash, sizeof modname - 1);
		memcpy(modname, slash, len);
		modname[len] = '\0';
		if(streq(modname, "sysmem")) {
			/* and sysmem's ELF load area */
			ret = max(ret, last_load_address(rec));
		}
	}

	ret = (ret + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	return ret - 1;
}


void *sbrk(intptr_t increment)
{
	if(unlikely(current_brk == 0)) {
		extern char _end;
		current_brk = ((L4_Word_t)&_end + PAGE_MASK) & ~PAGE_MASK;
		current_brk = max(current_brk, last_module_address() + 1);
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
