
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <threads.h>
#include <ccan/likely/likely.h>
#include <ccan/htable/htable.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/ipc.h>
#include <l4/kip.h>
#include <l4/bootinfo.h>
#include <l4/sigma0.h>

#include <sneks/mm.h>

#include "elf.h"
#include "sysmem-defs.h"
#include "defs.h"


#define THREAD_STACK_SIZE 4096
#define SYSMEM_SEED_MEGS 2


struct sysmem_page {
	_Atomic L4_Word_t address;
};


/* TODO: push this into lib/hash.c or some such */
/* hash32shiftmult(); presumed to have been in the public domain. */
static uint32_t int_hash(uint32_t key)
{
	uint32_t c2=0x27d4eb2du; // a prime or an odd constant
	key = (key ^ 61) ^ (key >> 16);
	key = key + (key << 3);
	key = key ^ (key >> 4);
	key = key * c2;
	key = key ^ (key >> 15);
	return key;
}

#define word_hash(x) int_hash((uint32_t)(x))


static size_t hash_sysmem_page(const void *key, void *priv) {
	const struct sysmem_page *p = key;
	return word_hash(p->address);
}

static bool sysmem_page_cmp(const void *cand, void *key) {
	const struct sysmem_page *p = cand;
	return p->address == *(L4_Word_t *)key;
}


/* roottask's threading is implemented in terms of C11 threads, or a subset
 * thereof anyway.
 */
typedef int (*thrd_start_t)(void *);


static void thread_wrapper(L4_ThreadId_t parent)
{
	L4_Set_UserDefinedHandle(0);
	L4_Set_ExceptionHandler(L4_nilthread);

	L4_Accept(L4_UntypedWordsAcceptor);
	L4_MsgTag_t tag = L4_Receive(parent);
	if(L4_IpcFailed(tag)) {
		printf("%s: init failed, ec=%#lx\n", __func__, L4_ErrorCode());
		abort();
	}
	L4_Word_t fn, param;
	L4_StoreMR(1, &fn);
	L4_StoreMR(2, &param);
	int retval = (*(thrd_start_t)fn)((void *)param);
	printf("%s: thread exiting, retval=%d\n", __func__, retval);
	/* FIXME: actually exit */
	for(;;) L4_Sleep(L4_Never);
}


int thrd_create(thrd_t *t, thrd_start_t fn, void *param_ptr)
{
	static L4_Word_t utcb_base;
	static int next_tid;

	static bool first = true;
	if(unlikely(first)) {
		utcb_base = L4_MyLocalId().raw & ~511ul;
		next_tid = L4_ThreadNo(L4_Myself()) + 1;
		first = false;
	}

	L4_ThreadId_t tid = L4_GlobalId(next_tid++, 1);
	*t = tid.raw;

	void *stack = malloc(THREAD_STACK_SIZE);
	L4_Word_t stk_top = ((L4_Word_t)stack + THREAD_STACK_SIZE - 16) & ~0xfu;
#ifdef __SSE__
	/* FIXME: see comment in mung testbench start_thread_long() */
	stk_top += 4;
#endif
	L4_Word_t *sp = (L4_Word_t *)stk_top;
	*(--sp) = L4_Myself().raw;
	*(--sp) = 0xdeadb007;
	stk_top = (L4_Word_t)sp;

	static int next_utcb_slot = 1;
	L4_Word_t r = L4_ThreadControl(tid, L4_Myself(), L4_Myself(),
		L4_Pager(), (void *)(utcb_base + next_utcb_slot++ * 512));
	if(r == 0) {
		printf("%s: threadctl failed, ec=%#lx\n", __func__, L4_ErrorCode());
		free(stack);
		return thrd_error;
	}

	L4_Start_SpIp(tid, stk_top, (L4_Word_t)&thread_wrapper);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2 }.raw);
	L4_LoadMR(1, (L4_Word_t)fn);
	L4_LoadMR(2, (L4_Word_t)param_ptr);
	L4_MsgTag_t tag = L4_Send(tid);
	if(L4_IpcFailed(tag)) {
		printf("%s: init send failed, ec=%#lx\n", __func__, L4_ErrorCode());
		/* FIXME: do a real error exit */
		abort();
	}

	return thrd_success;
}


L4_ThreadId_t thrd_tidof_NP(thrd_t t) {
	return (L4_ThreadId_t){ .raw = t };
}


static int sysmem_pager_fn(void *param_ptr)
{
	struct htable *pages = param_ptr;
	for(;;) {
		L4_ThreadId_t sender;
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_MsgTag_t tag = L4_Wait(&sender);
		for(;;) {
			if(L4_IpcFailed(tag)) {
				printf("%s: IPC failed, ec=%#lx\n", __func__, L4_ErrorCode());
				break;
			}

			sender = L4_GlobalIdOf(sender);
			/* manual handling of the pager protocol. */
			if(tag.X.label >> 4 == 0xffe && tag.X.u == 2 && tag.X.t == 0) {
				L4_Word_t faddr, fip;
				L4_StoreMR(1, &faddr);
				L4_StoreMR(2, &fip);
				L4_Word_t raw_addr = faddr;
				faddr &= ~PAGE_MASK;
				struct sysmem_page *p = htable_get(pages, word_hash(faddr),
					&sysmem_page_cmp, &faddr);
				if(p != NULL) {
					L4_Word_t access = tag.X.label & L4_FullyAccessible;
#if 0
					printf("%s: pf addr=%#lx, ip=%#lx, %c%c%c\n",
						__func__, raw_addr, fip,
						(access & L4_Readable) ? 'r' : '-',
						(access & L4_Writable) ? 'w' : '-',
						(access & L4_eXecutable) ? 'x' : '-');
#endif
					L4_Fpage_t map = L4_FpageLog2(p->address, PAGE_BITS);
					L4_Set_Rights(&map, L4_FullyAccessible);
					L4_Word_t old = atomic_exchange(&p->address, 0);
					if(old != 0) {
						htable_del(pages, word_hash(faddr), p);
						free(p);
						assert(htable_get(pages, word_hash(faddr),
							&sysmem_page_cmp, &faddr) == NULL);
					}
					L4_GrantItem_t gi = L4_GrantItem(map, faddr);
					L4_LoadMR(0, (L4_MsgTag_t){ .X.t = 2 }.raw);
					L4_LoadMRs(1, 2, gi.raw);
				} else if(faddr == 0) {
					printf("%s: sysmem nullptr at ip=%#lx\n", __func__, fip);
					break;
				} else {
					printf("%s: sysmem SEGV at ip=%#lx, addr=%#lx\n",
						__func__, fip, raw_addr);
					break;
				}
			} else {
				printf("%s: unrecognized tag=%#lx, sender=%lu:%lu\n", __func__,
					tag.raw, L4_ThreadNo(sender), L4_Version(sender));
				break;
			}

			L4_Accept(L4_UntypedWordsAcceptor);
			tag = L4_ReplyWait(sender, &sender);
		}
	}

	return 0;
}


static void add_sysmem_pages(struct htable *ht, L4_Word_t start, L4_Word_t end)
{
	for(L4_Word_t addr = start; addr < end; addr += PAGE_SIZE) {
		struct sysmem_page *p = malloc(sizeof *p);
		atomic_store(&p->address, addr);
		htable_add(ht, word_hash(p->address), p);
	}
}


/* adds the page to root's space if @self is true; or to either sysmem's free
 * memory pool or its set of reserved pages otherwise.
 */
void send_phys_to_sysmem(L4_ThreadId_t sysmem_tid, bool self, L4_Word_t addr)
{
	int n = __sysmem_send_phys(sysmem_tid,
		self ? L4_Myself().raw : L4_nilthread.raw,
		addr >> PAGE_BITS, PAGE_BITS);
	if(n != 0) {
		printf("%s: ipc fail, n=%d\n", __func__, n);
		abort();
	}
	L4_Fpage_t pg = L4_FpageLog2(addr, PAGE_BITS);
	L4_Set_Rights(&pg, L4_FullyAccessible);
	L4_GrantItem_t gi = L4_GrantItem(pg, 0);
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.t = 2 }.raw);
	L4_LoadMRs(1, 2, gi.raw);
	L4_MsgTag_t tag = L4_Send_Timeout(sysmem_tid, L4_TimePeriod(10 * 1000));
	if(L4_IpcFailed(tag)) {
		printf("%s: failed to send grant, ec=%lu\n", __func__,
			L4_ErrorCode());
		abort();
	}
}


static L4_ThreadId_t start_sysmem(L4_ThreadId_t *pager_p)
{
	L4_KernelInterfacePage_t *kip = L4_GetKernelInterface();
	L4_BootInfo_t *bootinfo = (L4_BootInfo_t *)L4_BootInfo(kip);

	L4_BootRec_t *rec = L4_BootInfo_FirstEntry(bootinfo);
	bool found = false;
	const char *cmdline_rest = NULL;
	for(L4_Word_t i = 0;
		i < L4_BootInfo_Entries(bootinfo);
		i++, rec = L4_BootRec_Next(rec))
	{
		if(rec->type != L4_BootInfo_Module) {
			printf("rec at %p is not module\n", rec);
			continue;
		}

		char *cmdline = L4_Module_Cmdline(rec);
		const char *slash = strrchr(cmdline, '/');
		if(slash != NULL && memcmp(slash + 1, "sysmem", 6) == 0) {
			found = true;
			cmdline_rest = strchr(slash, ' ');
			break;
		}
	}
	if(!found) {
		printf("can't find sysmem's module! was it loaded?\n");
		abort();
	}
	printf("rest of sysmem cmdline: `%s'\n", cmdline_rest);

	/* `pages' is accessed from within sysmem_pager_fn() and this function.
	 * however, since the former only does so in response to pagefault, and
	 * pagefaults only occur after we've started the pager's client process,
	 * we can without risk add things to `pages' while setting the client
	 * process up. sysmem_pager_fn() takes ownership of `pages' eventually.
	 */
	struct htable *pages = malloc(sizeof *pages);
	htable_init(pages, &hash_sysmem_page, NULL);
	L4_Word_t start_addr = L4_Module_Start(rec),
		end_addr = start_addr + L4_Module_Size(rec) - 1;
	thrd_t pg;
	int n = thrd_create(&pg, &sysmem_pager_fn, pages);
	if(n != thrd_success) {
		fprintf(stderr, "%s: can't create thread: n=%d\n", __func__, n);
		abort();
	}
	*pager_p = thrd_tidof_NP(pg);

	/* parse and load the ELF32 binary. */
	const Elf32_Ehdr *ee = (void *)start_addr;
	if(memcmp(ee->e_ident, ELFMAG, SELFMAG) != 0) {
		printf("incorrect sysmem ELF magic\n");
		abort();
	}
	uintptr_t phoff = ee->e_phoff;
	for(int i=0; i < ee->e_phnum; i++, phoff += ee->e_phentsize) {
		const Elf32_Phdr *ep = (void *)(start_addr + phoff);
		if(ep->p_type != PT_LOAD) continue;	/* skip the GNU stack thing */

		/* map it to physical memory 1:1 */
		memcpy((void *)ep->p_vaddr, (void *)(start_addr + ep->p_offset),
			ep->p_filesz);
		if(ep->p_filesz < ep->p_memsz) {
			memset((void *)ep->p_vaddr + ep->p_filesz, 0,
				ep->p_memsz - ep->p_filesz);
		}
		add_sysmem_pages(pages, ep->p_vaddr, ep->p_vaddr + ep->p_memsz - 1);
	}

	/* set up the address space & start the main thread. */
	L4_ThreadId_t sysmem_tid = L4_GlobalId(123, 457);
	L4_Word_t res = L4_ThreadControl(sysmem_tid, sysmem_tid,
		*pager_p, *pager_p, (void *)-1);
	if(res != 1) {
		fprintf(stderr, "%s: ThreadControl failed, ec %lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	L4_Fpage_t utcb_area = L4_FpageLog2(0x100000, 14);
	L4_Word_t old_ctl;
	res = L4_SpaceControl(sysmem_tid, 0, L4_FpageLog2(0xff000, 12),
		utcb_area, L4_anythread, &old_ctl);
	if(res != 1) {
		fprintf(stderr, "%s: SpaceControl failed, ec %lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	res = L4_ThreadControl(sysmem_tid, sysmem_tid, *pager_p,
		*pager_p, (void *)L4_Address(utcb_area));
	if(res != 1) {
		fprintf(stderr, "%s: ThreadControl failed, ec %lu\n",
			__func__, L4_ErrorCode());
		abort();
	}

	/* propagated breath of life. */
	L4_Set_VirtualSender(*pager_p);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 2, .X.flags = 1 }.raw);
	L4_LoadMR(1, ee->e_entry);
	L4_LoadMR(2, 0xdeadbeef);
	L4_MsgTag_t tag = L4_Send_Timeout(sysmem_tid, L4_TimePeriod(50 * 1000));
	if(L4_IpcFailed(tag)) {
		fprintf(stderr, "%s: breath-of-life to forkserv failed: ec=%#lx\n",
			__func__, L4_ErrorCode());
		abort();
	}

	/* basic introductions.
	 * TODO: probe root UTCB area size with ThreadControl. 
	 */
	L4_Fpage_t root_kip = L4_FpageLog2(
			(L4_Word_t)L4_GetKernelInterface(), PAGE_BITS),
		root_utcb = L4_Fpage(L4_MyLocalId().raw & ~511u, 64 * 1024);
	n = __sysmem_new_task(sysmem_tid, root_kip, root_utcb, L4_Myself().raw);
	if(n != 0) goto sysmem_fail;
	n = __sysmem_add_thread(sysmem_tid, L4_Myself().raw, pager_p->raw);
	if(n != 0) goto sysmem_fail;

	/* bequeath memoization consignment upon yonder auxiliary paginator. */
	struct htable_iter it;
	for(struct sysmem_page *p = htable_first(pages, &it);
		p != NULL;
		p = htable_next(pages, &it))
	{
		L4_Word_t addr = atomic_load(&p->address);
		if(addr == 0) continue;

		send_phys_to_sysmem(sysmem_tid, false, addr);
		if(atomic_exchange(&p->address, 0) != 0) {
			htable_delval(pages, &it);
			free(p);
		}
	}
	assert(pages->elems == 0);
	/* from now, sysmem_pager_fn will only report segfaults for sysmem. */

	return sysmem_tid;

sysmem_fail:
	printf("%s: can't configure sysmem: n=%d\n", __func__, n);
	abort();
}


/* fault in memory between _start and _end, switch pagers, and grant it page
 * by page to sysmem. then trigger mm.c heap transition and pump a few megs
 * from sigma0 for sysmem.
 */
static void move_to_sysmem(L4_ThreadId_t sysmem_tid, L4_ThreadId_t sm_pager)
{
	L4_ThreadId_t s0 = L4_Pager();

	extern char _start, _end;
	for(L4_Word_t addr = (L4_Word_t)&_start & ~PAGE_MASK;
		addr < (L4_Word_t)&_end;
		addr += PAGE_SIZE)
	{
		volatile L4_Word_t *ptr = (void *)addr;
		*ptr = *ptr;
	}
	L4_Set_Pager(sysmem_tid);
	L4_Set_PagerOf(sm_pager, sysmem_tid);
	printf("moving early memory to sysmem\n");
	for(L4_Word_t addr = (L4_Word_t)&_start & ~PAGE_MASK;
		addr < (L4_Word_t)&_end;
		addr += PAGE_SIZE)
	{
		send_phys_to_sysmem(sysmem_tid, true, addr);
	}
	mm_enable_sysmem(sysmem_tid);

	L4_Word_t remain = SYSMEM_SEED_MEGS * 1024 * 1024;
	int scale = MSB(remain);
	while(remain > 0) {
		L4_Fpage_t fp = L4_Sigma0_GetAny(s0, scale, L4_CompleteAddressSpace);
		if(L4_IsNilFpage(fp)) {
			if(L4_ErrorCode() == 0 && scale > PAGE_BITS) {
				scale--;
				continue;
			} else {
				printf("can't pump %lu bytes for sysmem, ec=%lu\n",
					1lu << scale, L4_ErrorCode());
				abort();
			}
		}
		printf("adding %#lx:%#lx to sysmem\n", L4_Address(fp), L4_Size(fp));
		for(L4_Word_t addr = L4_Address(fp);
			addr < L4_Address(fp) + L4_Size(fp);
			addr += PAGE_SIZE)
		{
			send_phys_to_sysmem(sysmem_tid, false, addr);
		}
		remain -= L4_Size(fp);
	}
}


int main(void)
{
	printf("hello, world!\n");

	L4_ThreadId_t sm_pager = L4_nilthread, sysmem = start_sysmem(&sm_pager);
	printf("sysmem started at %lu:%lu\n",
		L4_ThreadNo(sysmem), L4_Version(sysmem));
	move_to_sysmem(sysmem, sm_pager);

	/* test sysmem out by allocating hugely. */
	uint8_t *ptr = malloc(300 * 1024);
	printf("ptr=%p\n", ptr);
	memset(ptr, 0xba, 300 * 1024);
	free(ptr);

	return 0;
}
