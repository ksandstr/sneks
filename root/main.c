
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
#include <l4/space.h>
#include <l4/ipc.h>
#include <l4/kip.h>
#include <l4/bootinfo.h>
#include <l4/sigma0.h>

#include <sneks/mm.h>
#include <sneks/hash.h>

#include "elf.h"
#include "sysmem-defs.h"
#include "defs.h"


#define THREAD_STACK_SIZE 4096
#define SYSMEM_SEED_MEGS 2


struct sysmem_page {
	_Atomic L4_Word_t address;
};


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
#if 0
					L4_Word_t access = tag.X.label & L4_FullyAccessible;
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


/* create a new system task in a waiting condition: the returned thread exists
 * in a space configured to @kip_area, @utcb_area, its utcb location is set to
 * the very start of @utcb_area, it'll be paged by the calling thread's pager
 * (which should be sysmem), and it can be started by the pager (or a
 * propagator for it) with a L4.X2 breath-of-life message.
 */
static L4_ThreadId_t new_task(L4_Fpage_t kip_area, L4_Fpage_t utcb_area)
{
	static int task_offset = 1;
	/* FIXME: this is a fucked up silly way to allocate thread IDs for system
	 * tasks. manage these better.
	 */
	L4_ThreadId_t new_tid = L4_GlobalId(
			L4_ThreadNo(L4_Myself()) + task_offset * 100, 1),
		pager = L4_Pager();
	task_offset++;
	L4_Word_t res = L4_ThreadControl(new_tid, new_tid,
		pager, pager, (void *)-1);
	if(res != 1) {
		fprintf(stderr, "%s: first ThreadControl failed, ec=%lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	L4_Word_t old_ctl;
	res = L4_SpaceControl(new_tid, 0, kip_area, utcb_area,
		L4_anythread, &old_ctl);
	if(res != 1) {
		fprintf(stderr, "%s: SpaceControl failed, ec=%lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	res = L4_ThreadControl(new_tid, new_tid, pager,
		pager, (void *)L4_Address(utcb_area));
	if(res != 1) {
		fprintf(stderr, "%s: second ThreadControl failed, ec=%lu\n",
			__func__, L4_ErrorCode());
		abort();
	}

	return new_tid;
}


static L4_BootRec_t *find_boot_module(
	L4_KernelInterfacePage_t *kip, const char *name, char **cmd_rest_p)
{
	L4_BootInfo_t *bootinfo = (L4_BootInfo_t *)L4_BootInfo(kip);

	L4_BootRec_t *rec = L4_BootInfo_FirstEntry(bootinfo);
	bool found = false;
	const char *cmdline_rest = NULL;
	int name_len = strlen(name);
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
		if(slash != NULL && memcmp(slash + 1, name, name_len) == 0) {
			found = true;
			cmdline_rest = strchr(slash, ' ');
			break;
		}
	}
	if(found && cmdline_rest != NULL && cmd_rest_p != NULL) {
		*cmd_rest_p = strdup(cmdline_rest);
	}
	return found ? rec : NULL;
}


static L4_ThreadId_t start_sysmem(L4_ThreadId_t *pager_p)
{
	L4_KernelInterfacePage_t *kip = L4_GetKernelInterface();
	L4_BootRec_t *rec = find_boot_module(kip, "sysmem", NULL);
	if(rec == NULL) {
		printf("can't find sysmem's module! was it loaded?\n");
		abort();
	}

	/* `pages' is accessed from within sysmem_pager_fn() and this function.
	 * however, since the former only does so in response to pagefault, and
	 * pagefaults only occur after we've started the pager's client process,
	 * we can without risk add things to `pages' while setting the client
	 * process up. sysmem_pager_fn() takes ownership of `pages' eventually.
	 */
	struct htable *pages = malloc(sizeof *pages);
	htable_init(pages, &hash_sysmem_page, NULL);
	L4_Word_t start_addr = L4_Module_Start(rec);
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
	htable_clear(pages);
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


static L4_ThreadId_t start_boot_module(L4_ThreadId_t s0, const char *name)
{
	L4_ThreadId_t sysmem_tid = L4_Pager();
	assert(!L4_SameThreads(sysmem_tid, s0));

	L4_KernelInterfacePage_t *kip = L4_GetKernelInterface();
	char *rest = NULL;
	L4_BootRec_t *mod = find_boot_module(kip, name, &rest);
	printf("name=`%s', mod=%p, rest=`%s'\n", name, mod, rest);
	free(rest);

	/* get the boot module's pages from sigma0. */
	L4_Word_t start_addr = L4_Module_Start(mod);
	int num_mod_pages = (L4_Module_Size(mod) + PAGE_MASK) >> PAGE_BITS;
	L4_Fpage_t mod_pages[num_mod_pages];
	for(int i=0; i < num_mod_pages; i++) {
		mod_pages[i] = L4_Sigma0_GetPage(s0,
			L4_FpageLog2(start_addr + i * PAGE_SIZE, PAGE_BITS));
	}

	const Elf32_Ehdr *ee = (void *)start_addr;
	if(memcmp(ee->e_ident, ELFMAG, SELFMAG) != 0) {
		printf("incorrect ELF magic in boot module `%s'\n", name);
		abort();
	}

	/* place UTCB and KIP areas below the process image. (this helps with the
	 * "smallspace" ASID emulation technique, which will matter one day.)
	 */
	uintptr_t phoff = ee->e_phoff, lowest = ~0ul;
	for(int i=0; i < ee->e_phnum; i++, phoff += ee->e_phentsize) {
		const Elf32_Phdr *ep = (void *)(start_addr + phoff);
		if(ep->p_type != PT_LOAD) continue;	/* skip the GNU stack thing */
		lowest = min_t(uintptr_t, ep->p_vaddr, lowest);
	}
	lowest &= ~PAGE_MASK;
	L4_Fpage_t kip_area, utcb_area;
	int utcb_scale = min_t(int, MSB(lowest) - 1, 21);
	L4_Word_t utcb_size = 1u << utcb_scale;
	if(ffsl(lowest) < 20) {
		/* KIP area up front. */
		kip_area = L4_FpageLog2(lowest - PAGE_SIZE, PAGE_BITS);
		utcb_area = L4_FpageLog2(
			(lowest - utcb_size) & ~(utcb_size - 1), utcb_scale);
	} else {
		/* UTCB area up front. */
		utcb_area = L4_FpageLog2(lowest - utcb_size, utcb_scale);
		kip_area = L4_FpageLog2(L4_Address(utcb_area) - PAGE_SIZE,
			PAGE_BITS);
	}
	printf("utcb_area=%#lx:%#lx, kip_area=%#lx:%#lx, lowest=%#x\n",
		L4_Address(utcb_area), L4_Size(utcb_area),
		L4_Address(kip_area), L4_Size(kip_area), lowest);

	/* create the task w/ all of that shit & what-not. */
	L4_ThreadId_t new_tid = new_task(kip_area, utcb_area);
	int n = __sysmem_new_task(sysmem_tid, kip_area, utcb_area, new_tid.raw);
	if(n != 0) {
		printf("sysmem::new_task failed, n=%d\n", n);
		abort();
	}

	/* copy each page to a vmem buffer, then send_virt it over to the new
	 * process at the correct address.
	 */
	void *copybuf = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	phoff = ee->e_phoff;
	for(int i=0; i < ee->e_phnum; i++, phoff += ee->e_phentsize) {
		const Elf32_Phdr *ep = (void *)(start_addr + phoff);
		/* (`ep' is s0'd in the previous loop.) */
		if(ep->p_type != PT_LOAD) continue;	/* skip the GNU stack thing */

		void *src = (void *)start_addr + ep->p_offset;
		for(size_t off = 0; off < ep->p_memsz; off += PAGE_SIZE) {
			if(off < ep->p_filesz) {
				memcpy(copybuf, src + off,
					min_t(int, PAGE_SIZE, ep->p_filesz - off));
				if(ep->p_filesz - off < PAGE_SIZE) {
					memset(copybuf + ep->p_filesz - off, '\0',
						PAGE_SIZE - (ep->p_filesz - off));
				}
			} else {
				memset(copybuf, '\0', PAGE_SIZE);
			}
			uint16_t ret = 0;
			n = __sysmem_send_virt(sysmem_tid, &ret, (L4_Word_t)copybuf,
				new_tid.raw, ep->p_vaddr + off);
			if(n != 0 || ret != 0) {
				printf("sysmem::send_virt failed, n=%d, ret=%u\n", n, ret);
				abort();
			}

			/* FIXME: this is an ugly bodge for mung's failure to recognize a
			 * smaller subpage at a non-zero offset during recursive Unmap,
			 * i.e. the one that sysmem does. flushing explicitly works fine.
			 *
			 * remove it once mung becomes slightly less fuckered.
			 */
			L4_Fpage_t foo = L4_FpageLog2((L4_Word_t)copybuf, PAGE_BITS);
			L4_Set_Rights(&foo, L4_FullyAccessible);
			L4_FlushFpage(foo);
		}
	}
	free(copybuf);

	/* start 'er up. */
	uint16_t ret = 0;
	n = __sysmem_breath_of_life(sysmem_tid, &ret, new_tid.raw,
		ee->e_entry, 0xdeadbeef);
	if(n != 0 || ret != 0) {
		printf("sysmem::breath_of_life failed, n=%d, ret=%u\n", n, ret);
		abort();
	}

	/* toss the module pages into sysmem as spare RAM. no need for 'em
	 * anymore.
	 */
	for(int i=0; i < num_mod_pages; i++) {
		if(L4_IsNilFpage(mod_pages[i])) continue;
		send_phys_to_sysmem(sysmem_tid, false, L4_Address(mod_pages[i]));
	}

	return new_tid;
}


int main(void)
{
	printf("hello, world!\n");

	L4_ThreadId_t s0 = L4_Pager();
	L4_ThreadId_t sm_pager = L4_nilthread, sysmem = start_sysmem(&sm_pager);
	printf("sysmem started at %lu:%lu\n",
		L4_ThreadNo(sysmem), L4_Version(sysmem));
	move_to_sysmem(sysmem, sm_pager);

	/* test sysmem out by allocating hugely. */
	uint8_t *ptr = malloc(300 * 1024);
	printf("ptr=%p\n", ptr);
	memset(ptr, 0xba, 300 * 1024);
	free(ptr);

	/* TODO: launch mem, fs.cramfs here */

	/* run systest if present. */
	L4_ThreadId_t systest_tid = start_boot_module(s0, "systest");
	if(!L4_IsNilThread(systest_tid)) {
		/* wait until it's been removed. */
		L4_MsgTag_t tag;
		do {
			L4_Accept(L4_UntypedWordsAcceptor);
			tag = L4_Receive(systest_tid);
		} while(L4_IpcSucceeded(tag));
		if(L4_ErrorCode() != 5) {
			printf("systest exit ipc failed, ec=%lu\n", L4_ErrorCode());
			abort();
		}
	}

	printf("*** root would enter service mode\n");
	for(;;) L4_Sleep(L4_Never);		/* but fuck it. */

	return 0;
}
