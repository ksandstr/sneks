
#define SNEKS_KMSG_IMPL_SOURCE
#define ROOTSERV_IMPL_SOURCE
#define BOOTCON_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ccan/compiler/compiler.h>
#include <ccan/likely/likely.h>
#include <ccan/htable/htable.h>
#include <ccan/hash/hash.h>
#include <ccan/array_size/array_size.h>
#include <ccan/str/str.h>

#include <l4/types.h>
#include <l4/thread.h>
#include <l4/schedule.h>
#include <l4/space.h>
#include <l4/ipc.h>
#include <l4/kip.h>
#include <l4/bootinfo.h>
#include <l4/sigma0.h>
#include <l4/kdebug.h>

#include <sneks/mm.h>
#include <sneks/hash.h>
#include <sneks/bitops.h>
#include <sneks/process.h>
#include <sneks/sysinfo.h>
#include <sneks/rootserv.h>

#include "elf.h"
#include "muidl.h"
#include "sysmem-defs.h"
#include "kmsg-defs.h"
#include "proc-defs.h"
#include "rootserv-defs.h"
#include "bootcon-defs.h"
#include "defs.h"


#define THREAD_STACK_SIZE 4096


struct sysmem_page {
	L4_Word_t address;
};


/* string pair given as argument to the root task module. hashed by ->key. */
struct root_arg {
	const char *key, *value;
};


static size_t hash_raw_word(const void *ptr, void *priv);

L4_KernelInterfacePage_t *the_kip;
L4_ThreadId_t vm_tid = { .raw = 0 };

static L4_ThreadId_t sigma0_tid, sysmem_tid;
static int sysmem_pages = 0, sysmem_self_pages = 0;

/* memory set aside for sysmem, e.g. boot modules that were copied into the
 * dlmalloc heap before start_sysmem(). emptied and cleared in start_sysmem();
 * members L4_Fpage_t cast to a pointer.
 */
static struct htable pre_sysmem_resv = HTABLE_INITIALIZER(
	pre_sysmem_resv, &hash_raw_word, NULL);


extern NORETURN void panic(const char *msg);


static size_t hash_raw_word(const void *ptr, void *priv) {
	return word_hash((L4_Word_t)ptr);
}


static size_t hash_sysmem_page(const void *key, void *priv) {
	const struct sysmem_page *p = key;
	return word_hash(p->address);
}

static bool sysmem_page_cmp(const void *cand, void *key) {
	const struct sysmem_page *p = cand;
	return p->address == *(L4_Word_t *)key;
}


static size_t hash_arg(const void *key, void *priv) {
	const struct root_arg *p = key;
	return hash_string(p->key);
}

static bool arg_cmp(const void *cand, void *key) {
	const struct root_arg *p = cand;
	return streq(p->key, key);
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
					printf("sysmem pf addr=%#lx, ip=%#lx, %c%c%c\n",
						raw_addr, fip,
						(access & L4_Readable) ? 'r' : '-',
						(access & L4_Writable) ? 'w' : '-',
						(access & L4_eXecutable) ? 'x' : '-');
#endif
					L4_Fpage_t map = L4_FpageLog2(p->address, PAGE_BITS);
					L4_Set_Rights(&map, L4_FullyAccessible);
					L4_MapItem_t gi = L4_MapItem(map, faddr);
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
		p->address = addr;
		htable_add(ht, word_hash(p->address), p);
	}
}


/* adds the page to root's space if @self is true; or to either sysmem's free
 * memory pool or its set of reserved pages otherwise.
 */
void send_phys_to_sysmem(L4_ThreadId_t sysmem_tid, bool self, L4_Fpage_t pg)
{
	int n = __sysmem_send_phys(sysmem_tid,
		self ? L4_Myself().raw : L4_nilthread.raw,
		L4_Address(pg) >> PAGE_BITS, L4_SizeLog2(pg));
	if(n != 0) {
		printf("%s: ipc fail, n=%d\n", __func__, n);
		abort();
	}
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

	int n_pages = L4_Size(pg) >> PAGE_BITS;
	if(self) sysmem_self_pages += n_pages; else sysmem_pages += n_pages;
}


/* build a systask with the given shape, and construct an address space etc.
 * for it. returns the first thread's ID, which will be awaiting a
 * breath-of-life from its pager.
 */
static L4_ThreadId_t create_systask(L4_Fpage_t kip_area, L4_Fpage_t utcb_area)
{
	/* first is root, then sysmem. others after that. */
	static int task_offset = SNEKS_MIN_SYSID + 2;

	lock_uapi();
	int pid = task_offset++, n = add_systask(pid, kip_area, utcb_area);
	assert(n < 0 || n == pid);
	if(n < 0) {
		printf("%s: add_task failed, n=%d\n", __func__, n);
		abort();
	}
	void *utcb_loc;
	L4_ThreadId_t new_tid = allocate_thread(pid, &utcb_loc);
	if(L4_IsNilThread(new_tid)) {
		printf("%s: allocate_thread failed\n", __func__);
		abort();
	}
	unlock_uapi();

	L4_ThreadId_t pager = L4_Pager();
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
	res = L4_ThreadControl(new_tid, new_tid, pager, pager, utcb_loc);
	if(res != 1) {
		fprintf(stderr, "%s: second ThreadControl failed, ec=%lu\n",
			__func__, L4_ErrorCode());
		abort();
	}

	return new_tid;
}


static L4_BootInfo_t *get_boot_info(L4_KernelInterfacePage_t *kip)
{
	static bool copied = false;
	static char bootinfo_copy[1024]
		__attribute__((aligned(sizeof(L4_BootInfo_t))));

	if(copied) goto end;

	L4_BootInfo_t *bootinfo = (L4_BootInfo_t *)L4_BootInfo(kip);
	L4_Word_t sz = L4_BootInfo_Size(bootinfo);
	if(sz > sizeof bootinfo_copy) {
		printf("%s: bootinfo size=%lu too large!\n", __func__, sz);
		abort();
	}
	memcpy(bootinfo_copy, bootinfo, sz);
	copied = true;

	if(L4_ThreadNo(L4_Pager()) > L4_ThreadNo(L4_Myself())) {
		/* not under sigma0 anymore. */
		goto end;
	}

	/* grab boot modules from sigma0 so they don't get passed to vm, stepped
	 * over by sbrk(), or some other untoward outcome. changes bootmodule
	 * start addresses to lie within the heap.
	 */
	bootinfo = (L4_BootInfo_t *)&bootinfo_copy[0];
	L4_BootRec_t *rec = L4_BootInfo_FirstEntry(bootinfo);
	for(L4_Word_t i = 0;
		i < L4_BootInfo_Entries(bootinfo);
		i++, rec = L4_BootRec_Next(rec))
	{
		if(rec->type != L4_BootInfo_Module) {
			printf("%s: rec=%p is not a module\n", __func__, rec);
			continue;
		}

		L4_Word_t base = L4_Module_Start(rec), len = L4_Module_Size(rec);
		size_t copylen = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		void *copy = aligned_alloc(PAGE_SIZE, copylen);
		memset(copy, '\0', copylen);
		printf("copy boot module [%#lx, %#lx) to %p:%#lx\n",
			base, base + len, copy, (L4_Word_t)copylen);
		L4_Word_t addr, sz;
		for_page_range(base, base + copylen, addr, sz) {
			L4_Fpage_t p = L4_Sigma0_GetPage(sigma0_tid, L4_FpageLog2(addr, sz));
			if(L4_IsNilFpage(p)) {
				printf("%s: couldn't get page %#lx:%#lx from s0!\n",
					__func__, addr, 1ul << sz);
				abort();
			}
			assert(copy + L4_Address(p) - base >= copy);
			assert(copy + L4_Address(p) - base + L4_Size(p) <= copy + copylen);
			memcpy(copy + L4_Address(p) - base, (void *)L4_Address(p), L4_Size(p));
			bool ok = htable_add(&pre_sysmem_resv, word_hash(p.raw), (void *)p.raw);
			if(!ok) {
				printf("%s: failed to add to pre_sysmem_resv!\n", __func__);
				abort();
			}
		}
		((L4_Boot_Module_t *)rec)->start = (L4_Word_t)copy;
		assert(L4_Module_Start(rec) == (L4_Word_t)copy);
	}

end:
	return (L4_BootInfo_t *)&bootinfo_copy[0];
}


static L4_BootRec_t *find_boot_module(
	L4_KernelInterfacePage_t *kip, const char *name, char **cmd_rest_p)
{
	L4_BootInfo_t *bootinfo = get_boot_info(kip);

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
		const char *slash = strrchr(cmdline, '/'),
			*space = strchr(cmdline, ' ');
		if(slash != NULL && memcmp(slash + 1, name, name_len) == 0) {
			found = true;
			cmdline_rest = space;
			break;
		} else if(slash == NULL && streq(cmdline, name)) {
			found = true;
			cmdline_rest = space;
			break;
		} else if(space != NULL
			&& space - cmdline >= name_len
			&& memcmp(cmdline, name,
				min_t(int, name_len, space - cmdline)) == 0)
		{
			found = true;
			cmdline_rest = space;
			break;
		}
	}
	if(cmd_rest_p != NULL) {
		*cmd_rest_p = cmdline_rest == NULL || !found
			? NULL : strdup(cmdline_rest);
	}
	return found ? rec : NULL;
}


static L4_ThreadId_t start_sysmem(
	L4_ThreadId_t *pager_p, L4_Fpage_t *utcb_area, L4_Fpage_t *kip_area)
{
	L4_KernelInterfacePage_t *kip = the_kip;
	L4_BootRec_t *rec = find_boot_module(kip, "sysmem", NULL);
	if(rec == NULL) {
		printf("can't find sysmem's module! was it loaded?\n");
		abort();
	}

	/* `pages' is accessed from within sysmem_pager_fn() and this function. by
	 * setting the pager to run at a lower priority, we ensure that it never
	 * preempts the launching thread, making the hash table mostly safe.
	 * (knocks on wood.)
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
	int rc = L4_Set_Priority(*pager_p, 1);	/* very very low indeed. */
	if(rc == 0) panic("couldn't set sysmem pager priority!");

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
	/* add a little something something just to get sysmem off the ground. */
	int rem = 256 * 1024;
	while(rem > 0) {
		L4_Fpage_t pg;
		for(int s = size_to_shift(rem); s >= PAGE_BITS; s--) {
			pg = L4_Sigma0_GetAny(L4_Pager(), s, L4_CompleteAddressSpace);
			if(!L4_IsNilFpage(pg)) break;
		}
		if(L4_IsNilFpage(pg)) {
			printf("%s: can't get seed memory from s0!\n", __func__);
			abort();
		}
		printf("%s: sending pg=%#lx:%#lx for sysmem init\n", __func__,
			L4_Address(pg), L4_Size(pg));
		add_sysmem_pages(pages, L4_Address(pg),
			L4_Address(pg) + L4_Size(pg) - 1);
		rem -= L4_Size(pg);
	}

	/* set up the address space & start the main thread. */
	L4_ThreadId_t sysmem_tid = L4_GlobalId(500, (1 << 2) | 2);
	L4_Word_t res = L4_ThreadControl(sysmem_tid, sysmem_tid,
		*pager_p, *pager_p, (void *)-1);
	if(res != 1) {
		fprintf(stderr, "%s: ThreadControl failed, ec %lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	*utcb_area = L4_FpageLog2(0x100000, 14);
	*kip_area = L4_FpageLog2(0xff000, 12);
	L4_Word_t old_ctl;
	res = L4_SpaceControl(sysmem_tid, 0, *kip_area, *utcb_area,
		L4_anythread, &old_ctl);
	if(res != 1) {
		fprintf(stderr, "%s: SpaceControl failed, ec %lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	res = L4_ThreadControl(sysmem_tid, sysmem_tid, *pager_p,
		*pager_p, (void *)L4_Address(*utcb_area));
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
	free((void *)start_addr);	/* see get_boot_info() */

	/* send the memory where boot modules were loaded. */
	struct htable_iter it;
	for(void *ptr = htable_first(&pre_sysmem_resv, &it);
		ptr != NULL;
		ptr = htable_next(&pre_sysmem_resv, &it))
	{
		L4_Fpage_t fp = { .raw = (L4_Word_t)ptr };
		send_phys_to_sysmem(sysmem_tid, false, fp);
	}
	htable_clear(&pre_sysmem_resv);
	/* and the memory where sysmem's own binary was loaded. */
	for(struct sysmem_page *p = htable_first(pages, &it);
		p != NULL;
		p = htable_next(pages, &it))
	{
		assert(p->address != 0);
		send_phys_to_sysmem(sysmem_tid, false,
			L4_FpageLog2(p->address, PAGE_BITS));
		htable_delval(pages, &it);
		free(p);
	}
	assert(pages->elems == 0);
	htable_clear(pages);
	/* from now, sysmem_pager_fn will only report segfaults for sysmem. */

	return sysmem_tid;
}


/* fault in memory between _start and _end, switch pagers, and grant it page
 * by page to sysmem. then trigger mm.c heap transition and toss a few pages
 * at sysmem to tide us over until start_vm().
 */
static void move_to_sysmem(L4_ThreadId_t sm_pager)
{
	assert(L4_SameThreads(sigma0_tid, L4_Pager()));

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
	for(L4_Word_t addr = (L4_Word_t)&_start & ~PAGE_MASK;
		addr < (L4_Word_t)&_end;
		addr += PAGE_SIZE)
	{
		send_phys_to_sysmem(sysmem_tid, true, L4_FpageLog2(addr, PAGE_BITS));
	}
	mm_enable_sysmem(sysmem_tid);
}


static void pop_interrupt_to(L4_ThreadId_t dest)
{
	L4_ThreadId_t old_exh = L4_ExceptionHandler();
	L4_Set_ExceptionHandler(dest);
	asm volatile ("int $99" ::: "memory");
	L4_Set_ExceptionHandler(old_exh);
}


static void rename_forbidden_thread(
	L4_ThreadId_t sender, L4_MsgTag_t frame_tag, L4_Word_t *frame)
{
	sender = L4_GlobalIdOf(sender);
	L4_ThreadId_t new_tid = L4_GlobalId(L4_ThreadNo(sender), 2);
	L4_Word_t res = L4_ThreadControl(new_tid, L4_Myself(), L4_nilthread,
		L4_nilthread, (void *)-1);
	if(res != 1) {
		printf("renaming threadctl failed, ec=%lu\n", L4_ErrorCode());
		abort();
	}
	void *stk = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	L4_Word_t *sp = (L4_Word_t *)(stk + PAGE_SIZE - 16 + 4);
	*(--sp) = L4_Myself().raw;
	*(--sp) = 0xabadc0de;
	L4_Start_SpIp(new_tid, (L4_Word_t)sp, (L4_Word_t)&pop_interrupt_to);
	L4_MsgTag_t tag = L4_Receive(new_tid);
	if(L4_IpcFailed(tag)) {
		printf("%s: can't get exception message, ec=%lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	/* reply w/ old frame to resuscitate old context. */
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = frame_tag.X.u }.raw);
	L4_LoadMRs(1, frame_tag.X.u, frame);
	tag = L4_Reply(new_tid);
	if(L4_IpcFailed(tag)) {
		printf("%s: frame reply failed, ec=%lu\n", __func__, L4_ErrorCode());
		abort();
	}

	free(stk);
}


static int rename_helper_fn(void *param_ptr)
{
	for(;;) {
		L4_ThreadId_t sender;
		L4_MsgTag_t tag = L4_Wait(&sender);
		if(L4_IpcFailed(tag)) {
			printf("%s: ipc failed, ec=%#lx\n", __func__, L4_ErrorCode());
			continue;
		}

		if((L4_Label(tag) & 0xfff0) == 0xffb0) {
			L4_Word_t frame[64];
			L4_StoreMRs(1, tag.X.u + tag.X.t, frame);
			frame[0] += 2;	/* skip past int $nn */
			rename_forbidden_thread(sender, tag, frame);
		} else if(L4_Label(tag) == 0xf00d && L4_IsLocalId(sender)) {
			/* call it quits. */
			break;
		} else {
			printf("%s: sender=%lu:%lu, tag=%#lx unrecognized\n", __func__,
				L4_ThreadNo(sender), L4_Version(sender), tag.raw);
		}
	}
	return 0;
}


/* move the boot thread out of the forbidden range. this requires use of a
 * helper thread, which we'll spawn first.
 */
static COLD void rename_first_threads(void)
{
	thrd_t helper;
	int n = thrd_create(&helper, &rename_helper_fn, NULL);
	if(n != thrd_success) {
		printf("%s: thrd_create failed, n=%d\n", __func__, n);
		abort();
	}
	pop_interrupt_to(thrd_tidof_NP(helper));

	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xf00d }.raw);
	L4_MsgTag_t tag = L4_Send(thrd_tidof_NP(helper));
	if(L4_IpcFailed(tag)) {
		printf("%s: helper quit msg failed, ec=%lu\n", __func__, L4_ErrorCode());
		abort();
	}
	n = thrd_join(helper, NULL);
	if(n != thrd_success) {
		printf("%s: helper join failed, n=%d\n", __func__, n);
		abort();
	}
}


static void *make_argpage(const char *name, char **argv, va_list more_args)
{
	void *mem = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	memset(mem, 0, PAGE_SIZE);
	int32_t *argc_p = mem;
	char *argbase = (char *)&argc_p[1], *argmem = argbase;
	argmem += strscpy(argmem, name, PAGE_SIZE - sizeof *argc_p) + 1;

	int argc;
	for(argc = 0; argv[argc] != NULL; argc++) {
		argmem += strscpy(argmem, argv[argc],
			PAGE_SIZE - (argmem - argbase)) + 1;
	}
	for(;;) {
		char *str = va_arg(more_args, char *);
		if(str == NULL) break;
		assert(argmem < argbase + PAGE_SIZE);
		int n = strscpy(argmem, str, PAGE_SIZE - (argmem - argbase));
		assert(n >= 0);
		argmem += n + 1;
		argc++;
	}
	*argc_p = ++argc;
	assert(argmem < argbase + PAGE_SIZE);
	*argmem = '\0';

	return mem;
}


/* return value is a malloc'd NULL-terminated array of pointers into @str,
 * which will have nul bytes dropped into the correct places.
 */
static char **break_argument_list(char *str, const char *delims)
{
	if(str == NULL) {
		char **end = malloc(sizeof(char *));
		*end = NULL;
		return end;
	}

	if(delims == NULL) delims = " \t\n\r";
	int len = strlen(str), nargs = 0;
	char *args[len / 2 + 1], *cur = str;
	while(cur < str + len) {
		char *brk = strpbrk(cur, delims);
		if(brk == cur) {
			/* skip doubles, triples, etc. */
			*(cur++) = '\0';
		} else {
			assert(nargs < len / 2 + 1);
			args[nargs++] = cur;
			if(brk == NULL) break;
			*brk = '\0';
			cur = brk + 1;
		}
	}

	char **copy = malloc(sizeof(char *) * (nargs + 1));
	for(int i=0; i < nargs; i++) copy[i] = args[i];
	copy[nargs] = NULL;
	return copy;
}


/* launches a systask from a boot module recognized with @name, appending the
 * given NULL-terminated parameter list to that specified for the module.
 */
static L4_ThreadId_t spawn_systask(const char *name, ...)
{
	assert(!L4_SameThreads(sigma0_tid, L4_Pager()));

	L4_KernelInterfacePage_t *kip = the_kip;
	char *rest = NULL;
	L4_BootRec_t *mod = find_boot_module(kip, name, &rest);
	printf("name=`%s', mod=%p, rest=`%s'\n", name, mod, rest);
	char **args = break_argument_list(rest, NULL);

	L4_Word_t start_addr = L4_Module_Start(mod);
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
#if 0
	printf("utcb_area=%#lx:%#lx, kip_area=%#lx:%#lx, lowest=%#x\n",
		L4_Address(utcb_area), L4_Size(utcb_area),
		L4_Address(kip_area), L4_Size(kip_area), lowest);
#endif

	assert(!L4_IsNilThread(uapi_tid));
	/* create the task w/ all of that shit & what-not. */
	L4_ThreadId_t new_tid = create_systask(kip_area, utcb_area);

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
			int n = __sysmem_send_virt(sysmem_tid, &ret,
				(L4_Word_t)copybuf, new_tid.raw, ep->p_vaddr + off);
			if(n != 0 || ret != 0) {
				printf("sysmem::send_virt failed, n=%d, ret=%u\n", n, ret);
				abort();
			}
		}
	}
	free(copybuf);

	/* make us an argument page as well & map it in. */
	va_list al;
	va_start(al, name);
	void *argpage = make_argpage(name, args, al);
	va_end(al);
	uint16_t ret;
	int n = __sysmem_send_virt(sysmem_tid, &ret, (L4_Word_t)argpage,
		new_tid.raw, L4_Address(kip_area) - PAGE_SIZE);
	if(n != 0 || ret != 0) {
		printf("sysmem::send_virt failed on argpage, n=%d, ret=%u\n", n, ret);
		abort();
	}
	free(argpage);

	/* allow get_shape(). */
	n = __sysmem_set_kernel_areas(sysmem_tid, new_tid.raw,
		utcb_area, kip_area);
	if(n != 0) {
		printf("sysmem::set_kernel_areas failed, n=%d\n", n);
		abort();
	}

	/* start 'er up. */
	n = __sysmem_breath_of_life(sysmem_tid, &ret, new_tid.raw,
		ee->e_entry, 0xdeadbeef);
	if(n != 0 || ret != 0) {
		printf("sysmem::breath_of_life failed, n=%d, ret=%u\n", n, ret);
		abort();
	}

	free((void *)start_addr);	/* see get_boot_info() */
	free(args);
	free(rest);
	return new_tid;
}


static int pump_sigma0(size_t *total_p, L4_Fpage_t *phys, int phys_len)
{
	L4_Word_t total = 0, n_phys = 0;
	for(int siz = sizeof(L4_Word_t) * 8 - 1;
		siz >= 12 && n_phys < phys_len;
		siz--)
	{
		int n = 0;
		for(;;) {
			L4_Fpage_t page = L4_Sigma0_GetAny(sigma0_tid,
				siz, L4_CompleteAddressSpace);
			if(L4_IsNilFpage(page)) break;

			n++;
			phys[n_phys++] = page;
		}
		total += n * (1ul << siz);
	}
	printf("root: got %lu KiB (%lu MiB) of memory from sigma0\n",
		total / 1024, total / (1024 * 1024));

	*total_p = total;
	return n_phys;
}


static int cmp_fpage(const void *a, const void *b) {
	return L4_Address(*(L4_Fpage_t *)a) - L4_Address(*(L4_Fpage_t *)b);
}


static void send_phys_to_vm(L4_ThreadId_t vm_tid, L4_Fpage_t page)
{
	L4_Accept(L4_UntypedWordsAcceptor);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xb0a7, .X.u = 2 }.raw);
	L4_LoadMR(1, page.raw);
	L4_LoadMR(2, 0);		/* upper 32 bits of physical address */
	L4_MsgTag_t tag = L4_Call(vm_tid);
	if(L4_IpcSucceeded(tag) && !L4_IsNilFpage(page)) {
		L4_Set_Rights(&page, L4_FullyAccessible);
		L4_GrantItem_t gi = L4_GrantItem(page, 0);
		L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xb0a8, .X.t = 2 }.raw);
		L4_LoadMRs(1, 2, gi.raw);
		tag = L4_Reply(vm_tid);
	}
	if(L4_IpcFailed(tag)) {
		printf("%s: ipc failed, ec=%#lx\n", __func__, L4_ErrorCode());
		abort();
	}
}


/* give sysmem its due; pass rest to vm under @vm_tid.
 *
 * NOTE: this doesn't enforce an upper limit on how much memory sysmem can end
 * up with, however, it's generally the case that vm's virtual memory isn't so
 * much to push sysmem over its basic allocation.
 */
static void portion_phys(size_t total, L4_ThreadId_t vm_tid, L4_Fpage_t page)
{
	/* FIXME: reduce the static 24M minimum allocation to something much
	 * tinier, like max(total/80, 1M), once sysmem and vm start passing
	 * physical RAM in response to pressure.
	 */
	size_t assn = max_t(size_t, total / 40, 24 * 1024 * 1024);
	int rem = assn - sysmem_pages * PAGE_SIZE;
	if(rem <= 0) send_phys_to_vm(vm_tid, page);
	else if(rem >= L4_Size(page)) send_phys_to_sysmem(sysmem_tid, false, page);
	else {
		size_t mid = L4_Address(page) + rem, sz;
		uintptr_t addr;
		for_page_range(L4_Address(page), mid, addr, sz) {
			send_phys_to_sysmem(sysmem_tid, false, L4_FpageLog2(addr, sz));
		}
		for_page_range(mid, L4_Address(page) + L4_Size(page), addr, sz) {
			send_phys_to_vm(vm_tid, L4_FpageLog2(addr, sz));
		}
	}
#if 0
	if(rem >= PAGE_SIZE) {
		printf("%s: sysmem_pages'=%d (%u KiB out of %u assigned)\n", __func__,
			sysmem_pages, sysmem_pages * PAGE_SIZE / 1024,
			(unsigned)assn / 1024);
	}
#endif
}


#define FP_HIGH(p) (L4_Address((p)) + L4_Size((p)) - 1)

/* start memory server, hand the physical memory that'd overlap its virtual
 * addresses to sysmem, and do initialization protocol. this also sets up
 * read-only sharing of @sip_mem with vm so that it can be mapped into
 * userspace processes.
 */
static L4_ThreadId_t start_vm(uintptr_t sip_mem)
{
	assert((sip_mem & PAGE_MASK) == 0);

	L4_ThreadId_t mem_tid = spawn_systask("vm", NULL);

	/* portion memory out to sysmem, vm, and consumers of various special
	 * ranges.
	 */
	L4_Fpage_t phys[1000];
	size_t total = 0;
	int n_phys = pump_sigma0(&total, phys, ARRAY_SIZE(phys));
	qsort(phys, n_phys, sizeof phys[0], &cmp_fpage);

	/* introduction to vm. this lets vm determine its memory model and
	 * initialize its heap etc. so that it can receive physical memory.
	 */
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xefff, .X.u = 2 }.raw);
	L4_LoadMR(1, total / PAGE_SIZE);
	L4_LoadMR(2, FP_HIGH(phys[n_phys - 1]));
	L4_MsgTag_t tag = L4_Call(mem_tid);
	if(L4_IpcFailed(tag)) goto initfail;

	/* second phase: map the SIP. */
	L4_Fpage_t sip_page = L4_FpageLog2(sip_mem, PAGE_BITS);
	L4_Set_Rights(&sip_page, L4_Readable);
	L4_MapItem_t mi = L4_MapItem(sip_page, 0);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xeffe, .X.t = 2 }.raw);
	L4_LoadMRs(1, 2, mi.raw);
	tag = L4_Call(mem_tid);
	if(L4_IpcFailed(tag)) goto initfail;

	/* capture special ranges (i.e. below 1M) and pass every other page to
	 * either sysmem or vm, depending whether it falls in vm's sysmem-paged
	 * range and whether sysmem has already received its proper due.
	 */
	L4_Word_t vm_low, vm_high;
	bool stale = true;
	for(int i=0; i < n_phys; i++) {
		L4_Fpage_t page = phys[i];
		// printf("page=%#lx:%#lx\n", L4_Address(page), L4_Size(page));
		L4_Word_t high = L4_Address(page) + L4_Size(page) - 1;
		if(L4_Address(page) < 1024 * 1024 || high < 1024 * 1024) {
			// printf("  kept as low memory\n");
			continue;
		}

		if(stale) {
			int n = __sysmem_get_shape(sysmem_tid, &vm_low, &vm_high, mem_tid.raw);
			if(n != 0) {
				printf("Sysmem::get_shape failed, n=%d\n", n);
				abort();
			}
			stale = false;
		}

		if(vm_high < L4_Address(page) || vm_low > high) {
			// printf("  disjoint from vm [%#lx..%#lx]\n", vm_low, vm_high);
			portion_phys(total, mem_tid, page);
			continue;
		}

		/* overlaps vm; divvy it up. */
		L4_Word_t points[] = {
			vm_low > L4_Address(page) ? L4_Address(page) : ~0ul,
			max(L4_Address(page), vm_low),
			min(high, vm_high) + 1,
			vm_high < high ? high + 1 : ~0ul,
		};
		bool to_vm = true;
		for(int j=1; j < ARRAY_SIZE(points); j++, to_vm = !to_vm) {
			if(points[j] == ~0ul) continue;
			L4_Word_t base;
			int size;
			for_page_range(points[j - 1], points[j], base, size) {
				L4_Fpage_t p = L4_FpageLog2(base, size);
				if(!to_vm) send_phys_to_sysmem(sysmem_tid, false, p);
				else {
					portion_phys(total, mem_tid, p);
					stale = true;	/* mild overkill. */
				}
			}
		}
	}
	send_phys_to_vm(mem_tid, L4_Nilpage);
	return mem_tid;

initfail:
	printf("%s: initialization failed, ec=%#lx\n", __func__,
		L4_ErrorCode());
	abort();
}


static L4_ThreadId_t mount_initrd(void)
{
	/* three seconds is fine. we're not riding rabbits here. (also QEMU on a
	 * P-M needs a bit of time for vm inits and such.)
	 */
	const L4_Time_t timeout = L4_TimePeriod(3 * 1000 * 1000);

	L4_KernelInterfacePage_t *kip = L4_GetKernelInterface();
	char *img_rest = NULL;
	L4_BootRec_t *img = find_boot_module(kip, "initrd.img", &img_rest);
	if(img == NULL || img->type != L4_BootInfo_Module) {
		printf("no initrd.img found (type=%d), skipping\n",
			img == NULL ? -1 : (int)img->type);
		return L4_nilthread;
	}

	L4_ThreadId_t self = L4_MyGlobalId();
	char tid[32];
	snprintf(tid, sizeof tid, "%lu:%lu",
		L4_ThreadNo(self), L4_Version(self));
	L4_ThreadId_t initrd_tid = spawn_systask("fs.squashfs",
		"--boot-initrd", tid, NULL);
	if(L4_IsNilThread(initrd_tid)) {
		panic("can't start fs.squashfs to mount initrd!");
	}

	/* first message communicates length of image, reply carries start
	 * address.
	 */
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
	L4_LoadMR(1, L4_Module_Size(img));
	L4_MsgTag_t tag = L4_Call_Timeouts(initrd_tid, timeout, timeout);
	if(L4_IpcFailed(tag)) goto ipcfail;
	L4_Word_t start; L4_StoreMR(1, &start);

	uint8_t *copybuf = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	for(size_t off = 0; off < L4_Module_Size(img); off += PAGE_SIZE) {
		size_t left = L4_Module_Size(img) - off;
		memcpy(copybuf, (void *)L4_Module_Start(img) + off,
			min_t(size_t, left, PAGE_SIZE));
		if(left < PAGE_SIZE) memset(copybuf + left, 0, PAGE_SIZE - left);
		uint16_t ret = 0;
		int n = __sysmem_send_virt(sysmem_tid, &ret, (L4_Word_t)copybuf,
			initrd_tid.raw, start + off);
		if(n != 0 || ret != 0) {
			printf("sysmem::send_virt failed, n=%d, ret=%u\n", n, ret);
			abort();
		}
	}
	free(copybuf);
	free((void *)L4_Module_Start(img));	/* see get_boot_info() */

	/* sync & get mount status. */
	L4_LoadMR(0, 0);
	tag = L4_Call_Timeouts(initrd_tid, timeout, timeout);
	if(L4_IpcFailed(tag)) goto ipcfail;
	L4_Word_t status; L4_StoreMR(1, &status);

	if(status != 0) {
		printf("initrd mount failure, status=%lu\n", status);
		panic("can't mount initrd!");
	}

	/* TODO: release physical memory of initrd image? */

	return initrd_tid;

ipcfail:
	printf("IPC fail; tag=%#lx, ec=%lu\n", tag.raw, L4_ErrorCode());
	panic("couldn't do init protocol with fs.squashfs!");
}


static int impl_kmsg_putstr(const char *str)
{
	extern void ser_putstr(const char *str);
	ser_putstr(str);
	return 0;
}


static int kmsg_impl_fn(void *param UNUSED)
{
	static struct sneks_kmsg_vtable vtab = {
		.putstr = &impl_kmsg_putstr,
	};
	for(;;) {
		L4_Word_t status = _muidl_sneks_kmsg_dispatch(&vtab);
		if(status == MUIDL_UNKNOWN_LABEL) {
			/* do nothing. */
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("%s: dispatch status %#lx (last tag %#lx)\n",
				__func__, status, muidl_get_tag().raw);
		}
	}

	return 0;
}


static void put_sysinfo(const char *name, int nvals, ...)
{
	int namelen = strlen(name), pos = 0;
	/* NOTE: this may read bytes past end of string, which may land on end of
	 * page. thankfully it's all init-time, so in practice problems should
	 * make themselves apparent in test builds.
	 */
	for(int i=0; i < namelen; i += sizeof(L4_Word_t)) {
		L4_Word_t w = *(const L4_Word_t *)&name[i];
		L4_LoadMR(++pos, w);
	}
	if(namelen % sizeof(L4_Word_t) == 0) L4_LoadMR(++pos, 0); /* terminator */
	va_list al;
	va_start(al, nvals);
	for(int i=0; i < nvals; i++) {
		L4_Word_t w = va_arg(al, L4_Word_t);
		L4_LoadMR(++pos, w);
	}
	va_end(al);
	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xbaaf, .X.u = pos }.raw);
	L4_MsgTag_t tag = L4_Send(sysmem_tid);
	if(L4_IpcFailed(tag)) {
		printf("%s: send failed, ec=%#lx\n", __func__, L4_ErrorCode());
		abort();
	}
}


static void halt_all_threads(void)
{
	/* TODO: halt the process service thread to lock its data, then crawl over
	 * said data to delete all existing threads w/ ThreadControl.
	 */
}


static void guru_meditation(const char *str)
{
	int max_line = 0, line = 0;
	for(int i=0; str[i] != '\0'; i++) {
		if(str[i] != '\n') line++;
		else {
			max_line = max(max_line, line);
			line = 0;
		}
	}
	max_line = max(max_line, line);

	char header[max_line + 6];
	memset(header, '*', max_line + 4);
	header[max_line + 4] = '\n';
	header[max_line + 5] = '\0';
	impl_kmsg_putstr(header);
	const char *last = str;
	char tmp[max_line + 1];
	for(int i=0; str[i] != '\0'; i++) {
		if(str[i] != '\n') continue;
		impl_kmsg_putstr("* ");
		size_t len = &str[i] - last;
		memcpy(tmp, last, len);
		tmp[len] = '\0';
		impl_kmsg_putstr(tmp);
		for(int j=max_line - len; j > 0; j--) impl_kmsg_putstr(" ");
		impl_kmsg_putstr(" *\n");
		last = &str[i + 1];
	}
	/* the final line. */
	impl_kmsg_putstr("* ");
	impl_kmsg_putstr(last);
	for(int j = max_line - strlen(last); j > 0; j--) impl_kmsg_putstr(" ");
	impl_kmsg_putstr(" *\n");
	/* final bar of asterisks. */
	impl_kmsg_putstr(header);
}


static void rs_long_panic(int32_t class, const char *message)
{
	printf("PANIC: %s\n", message);
	halt_all_threads();
	switch(class & 0xff) {
		case PANIC_EXIT:
			guru_meditation(
				"With this character's death, the thread of\n"
				"prophecy is severed.  Restore a saved game\n"
				"to restore the weave of fate, or persist in\n"
				"    the doomed world you have created.");
			break;

		case PANIC_BENIGN:
			if(class & PANICF_SEGV) {
				guru_meditation("Press F to pay respects");
			} else {
				guru_meditation("sit still to let the time go by");
			}
			break;

		default:
		case PANIC_UNKNOWN: {
			char str[200];
			snprintf(str, sizeof str,
				"Software failure.  Press left mouse button to continue.\n"
				"           Guru Meditation #%08X.%08X",
				class, 0xb0a7face);	/* respects to Boaty */
			guru_meditation(str);
			break;
		}
	}

	for(;;) L4_Sleep(L4_Never);	/* fuck it */
}


static void rs_panic(const char *str) {
	rs_long_panic(PANIC_UNKNOWN, str);
}


static void parse_initrd_args(struct htable *dest)
{
	/* find the initrd module. (the root module is loaded by the microkernel
	 * and so doesn't carry any parameters, so we use initrd instead.)
	 */
	char *rest = NULL;
	L4_BootRec_t *initrd = find_boot_module(the_kip, "initrd.img", &rest);
	if(initrd == NULL) {
		printf("WARNING: %s: can't find initrd.img!\n", __func__);
		return;
	}

	char **args = break_argument_list(rest, NULL);
	for(int i=0; args[i] != NULL; i++) {
		struct root_arg *arg = malloc(sizeof *arg + strlen(args[i]) + 1);
		char *eq = strchr(args[i], '=');
		if(eq == NULL) arg->value = NULL;
		else {
			*eq = '\0';
			arg->value = strdup(&eq[1]);
		}
		arg->key = strdup(args[i]);
		printf("KERNEL ARG: `%s' -> `%s'\n", arg->key, arg->value);
		bool ok = htable_add(dest, hash_string(arg->key), arg);
		if(!ok) panic("htable_add failed in parse_initrd_args");
	}

	free(args);
	free(rest);
}


static int bootcon_write(
	int32_t cookie, const uint8_t *buf, unsigned buf_len)
{
	extern void computchar(unsigned char ch);
	for(unsigned i=0; i < buf_len; i++) computchar(buf[i]);
	return buf_len;
}


static int bootcon_thread_fn(void *param_ptr)
{
	// struct htable *root_args = param_ptr;
	/* FIXME: make malloc threadsafe and remove this sleep so the dispatch
	 * function can safely enter malloc.
	 */
	L4_Sleep(L4_TimePeriod(20 * 1000));

	static const struct boot_con_vtable vtab = {
		.write = &bootcon_write,
	};
	for(;;) {
		L4_Word_t status = _muidl_boot_con_dispatch(&vtab);
		if(status == MUIDL_UNKNOWN_LABEL) {
			L4_MsgTag_t tag = muidl_get_tag();
			printf("bootcon: unknown message label=%#lx, u=%lu, t=%lu\n",
				L4_Label(tag), L4_UntypedWords(tag), L4_TypedWords(tag));
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("bootcon: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;
}


static L4_ThreadId_t console_init(struct htable *root_args)
{
	thrd_t con_thrd;
	int n = thrd_create(&con_thrd, &bootcon_thread_fn, root_args);
	return n != thrd_success ? L4_nilthread
		: L4_GlobalIdOf(thrd_tidof_NP(con_thrd));
}


/* gets a zero-or-one argument and returns it, or NULL if there wasn't one.
 * (zero-or-more will wait until the structures accommodate it.)
 */
static const char *get_root_arg(struct htable *root_args, const char *key)
{
	struct root_arg *arg = htable_get(root_args,
		hash_string(key), &arg_cmp, key);
	return arg != NULL ? arg->value : NULL;
}


static int launch_init(
	uint16_t *init_pid_p,
	struct htable *root_args, L4_ThreadId_t console)
{
	const char *init = get_root_arg(root_args, "init");
	if(init == NULL || init[0] == '\0') {
		panic("no init specified! can't boot like this.");
	}

	printf("running init=`%s'\n", init);
	char *semi = strchr(init, ';');
	int proglen = semi == NULL ? strlen(init) : semi - init;
	char prog[proglen + 1];
	memcpy(prog, init, proglen);
	prog[proglen] = '\0';

	char *copy = strdup(init);
	for(int i=0; copy[i] != '\0'; i++) {
		if(copy[i] == ';') copy[i] = 0x1e;	/* see RECSEP */
	}

	L4_Word_t servs[3], cookies[3];
	int32_t fds[3];
	for(int i=0; i < 3; i++) {
		fds[i] = i;
		if(i == 0) {
			servs[i] = L4_nilthread.raw;
			cookies[i] = 0;
		} else {
			servs[i] = console.raw;
			cookies[i] = 0xbadcafe0;
		}
	}
	/* init starts out with an empty environment. so sad. */
	int n = __proc_spawn(uapi_tid, init_pid_p, prog,
		copy, "", servs, 3, cookies, 3, fds, 3);
	free(copy);
	return n;
}


static COLD bool is_good_utcb(void *ptr)
{
	const L4_ThreadId_t dump_tid = L4_GlobalId(1000, 7);
	L4_Word_t res = L4_ThreadControl(dump_tid, L4_Myself(),
		L4_Pager(), L4_Myself(), ptr);
	if(res == 1) {
		res = L4_ThreadControl(dump_tid, L4_nilthread, L4_nilthread,
			L4_nilthread, (void *)-1);
		if(res != 1) {
			printf("can't delete dump_tid=%lu:%lu, ec=%lu\n",
				L4_ThreadNo(dump_tid), L4_Version(dump_tid), L4_ErrorCode());
			abort();
		}
		return true;
	} else if(L4_ErrorCode() != 6) {
		printf("%s: can't probe ptr=%p: ec=%lu?\n", __func__,
			ptr, L4_ErrorCode());
	}
	return false;
}


static COLD L4_Fpage_t probe_root_utcb_area(void)
{
	int u_align = 1 << L4_UtcbAlignmentLog2(the_kip),
		u_size = L4_UtcbSize(the_kip);

	L4_Word_t base = L4_MyLocalId().raw & ~(u_align - 1),
		bad = base + PAGE_SIZE, good = base + u_size;
	while(is_good_utcb((void *)bad)) {
		good = bad;
		bad += bad - base;
	}
	if(is_good_utcb((void *)(bad - u_size))) good = bad;

	return L4_Fpage(base, good - base);
}


static COLD void configure_uapi(L4_Fpage_t sm_kip, L4_Fpage_t sm_utcb)
{
	int n = add_systask(SNEKS_MIN_SYSID,
		L4_FpageLog2((L4_Word_t)the_kip, L4_KipAreaSizeLog2(the_kip)),
		probe_root_utcb_area());
	assert(n < 0 || n == SNEKS_MIN_SYSID);
	if(n < 0) {
		printf("add_systask() for root failed, n=%d\n", n);
		abort();
	}
	n = add_systask(pidof_NP(sysmem_tid), sm_kip, sm_utcb);
	assert(n < 0 || n == pidof_NP(sysmem_tid));
	if(n < 0) {
		printf("add_systask() for sysmem failed, n=%d\n", n);
		abort();
	}
}


/* run specified boot module as a systask and wait for it to complete, as
 * determined by its main thread becoming unavailable. @stem is prefixed to
 * "waitmod" to fetch the list of modules from @root_args.
 *
 * TODO: run multiway modules as well; one after another.
 */
static COLD void run_waitmods(struct htable *root_args, const char *stem)
{
	if(stem == NULL) stem = "";
	char arg[16 + strlen(stem)];
	snprintf(arg, sizeof arg, "%swaitmod", stem);
	const char *waitmod = get_root_arg(root_args, arg);
	if(waitmod == NULL) return;

	L4_ThreadId_t wm_tid = spawn_systask(waitmod, NULL);
	if(!L4_IsNilThread(wm_tid)) {
		/* wait until it's been removed, indicating completion. */
		L4_MsgTag_t tag;
		do {
			L4_Accept(L4_UntypedWordsAcceptor);
			tag = L4_Receive(wm_tid);
		} while(L4_IpcSucceeded(tag));
		if(L4_ErrorCode() != 5) {
			printf("%swaitmod exit check failed, ec=%lu\n", stem, L4_ErrorCode());
			abort();
		}
	}
}


int main(void)
{
	printf("hello, world!\n");
	the_kip = L4_GetKernelInterface();
	sigma0_tid = L4_Pager();
	rt_thrd_init();
	rename_first_threads();

	L4_Fpage_t sm_utcb = L4_Nilpage, sm_kip = L4_Nilpage;
	L4_ThreadId_t sm_pager = L4_nilthread;
	sysmem_tid = start_sysmem(&sm_pager, &sm_utcb, &sm_kip);
	move_to_sysmem(sm_pager);
	struct htable root_args = HTABLE_INITIALIZER(root_args, &hash_arg, NULL);
	parse_initrd_args(&root_args);

	uapi_init();
	configure_uapi(sm_kip, sm_utcb);
	rt_thrd_tests();
	L4_ThreadId_t con_tid = console_init(&root_args);

	/* configure sysinfo. */
	thrd_t kmsg;
	int n = thrd_create(&kmsg, &kmsg_impl_fn, NULL);
	if(n != thrd_success) {
		printf("can't start kmsg!\n");
		abort();
	}
	put_sysinfo("kmsg:tid", 1, thrd_tidof_NP(kmsg).raw);
	put_sysinfo("rootserv:tid", 1, L4_Myself().raw);
	printf("sysmem was initialized w/ %d pages and %d own pages\n",
		sysmem_pages, sysmem_self_pages);

	/* launch the userspace API server. */
	thrd_t uapi;
	n = thrd_create(&uapi, &uapi_loop, NULL);
	if(n != thrd_success) {
		printf("can't start uapi!\n");
		abort();
	}
	/* stupid-sync with uapi so that it's ready to handle sysmem's requests.
	 * without this, sysmem and uapi wind up in a send-send deadlock.
	 */
	L4_LoadMR(0, 0);
	L4_Send(thrd_tidof_NP(uapi));

	put_sysinfo("uapi:tid", 1, thrd_tidof_NP(uapi).raw);
	uapi_tid = thrd_tidof_NP(uapi);
	/* TODO: move these into a root internal test setup, like mung has with
	 * "ktest".
	 */
	rt_thrd_tests();

	void *sip_mem = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	L4_Fpage_t sip_page = L4_FpageLog2((uintptr_t)sip_mem, PAGE_BITS);
	L4_Set_Rights(&sip_page, L4_FullyAccessible);
	L4_FlushFpage(sip_page);
	n = __sysmem_alter_flags(L4_Pager(),
		L4_nilthread.raw, sip_page, SMATTR_PIN, ~0ul);
	if(n != 0) {
		printf("can't pin sip_mem=%p: n=%d\n", sip_mem, n);
		abort();
	}
	memset(sip_mem, '\0', PAGE_SIZE);
	vm_tid = start_vm((uintptr_t)sip_mem);
	assert(!L4_IsNilThread(vm_tid));
	put_sysinfo("uapi:vm:tid", 1, L4_GlobalIdOf(vm_tid).raw);

	/* FIXME: ensure that sysmem got at least a couple of megs' worth of
	 * memory to start with. generally vm and its UTCB segment take care of
	 * that, but still. (this'll be a stopgap until sigma1, sysmem, and vm
	 * trade pages as memory pressure changes between microkernel, system
	 * space, and user space.)
	 */
	printf("sysmem has been given %d pages and %d own pages\n",
		sysmem_pages, sysmem_self_pages);

	L4_ThreadId_t initrd_tid = mount_initrd();
	put_sysinfo("rootfs:tid", 1, initrd_tid.raw);

	struct __sysinfo *sip = sip_mem;
	*sip = (struct __sysinfo){
		.magic = SNEKS_SYSINFO_MAGIC,
		.sysinfo_size_log2 = PAGE_BITS,
		.api.proc = uapi_tid,
		.api.vm = vm_tid,
		.memory.page_size_log2 = PAGE_BITS,
		.memory.biggest_page_log2 = PAGE_BITS,
	};

	/* launch init.
	 * TODO: when init exits, reboot or shutdown according to its exit code
	 * (or some such).
	 */
	uint16_t init_pid;
	n = launch_init(&init_pid, &root_args, con_tid);
	if(n != 0) {
		printf("FAIL: can't launch init, n=%d\n", n);
		panic("this means your system is heavily broken!");
	} else if(init_pid != 1) {
		panic("init's pid isn't 1? what in tarnation");
	}

	run_waitmods(&root_args, "late");

	printf("*** root entering service mode\n");
	static const struct root_serv_vtable vtab = {
		.panic = &rs_panic,
		.long_panic = &rs_long_panic,
	};
	for(;;) {
		L4_Word_t status = _muidl_root_serv_dispatch(&vtab);
		if(status == MUIDL_UNKNOWN_LABEL) {
			/* do nothing. */
			L4_MsgTag_t tag = muidl_get_tag();
			printf("rootserv: unknown message label=%#lx, u=%lu, t=%lu\n",
				L4_Label(tag), L4_UntypedWords(tag), L4_TypedWords(tag));
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("rootserv: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}

	return 0;	/* but to whom? */
}
