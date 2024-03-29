#define SNEKS_KMSG_IMPL_SOURCE
#define ROOTSERV_IMPL_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdnoreturn.h>
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

#include <sneks/elf.h>
#include <sneks/ipc.h>
#include <sneks/mm.h>
#include <sneks/hash.h>
#include <sneks/thrd.h>
#include <sneks/bitops.h>
#include <sneks/process.h>
#include <sneks/systask.h>
#include <sneks/sysinfo.h>
#include <sneks/rootserv.h>
#include <sneks/console.h>
#include <sneks/api/io-defs.h>
#include <sneks/api/proc-defs.h>
#include <sneks/api/namespace-defs.h>
#include <sneks/sys/sysmem-defs.h>
#include <sneks/sys/kmsg-defs.h>

#include "muidl.h"
#include "root-impl-defs.h"
#include "defs.h"


struct sysmem_page {
	L4_Word_t address;
};


/* string pair given as argument to the root task module. hashed by ->key. */
struct root_arg {
	const char *key, *value;
};


static size_t hash_raw_word(const void *ptr, void *priv);

L4_KernelInterfacePage_t *__the_kip;
L4_ThreadId_t vm_tid = { .raw = 0 }, sysmsg_tid = { .raw = 0 }, initrd_tid = { .raw = 0 };

static L4_ThreadId_t sigma0_tid, rootpath_tid;
static int sysmem_pages = 0, sysmem_self_pages = 0;

struct cookie_key device_cookie_key;

/* memory set aside for sysmem, e.g. boot modules that were copied into the
 * dlmalloc heap before start_sysmem(). emptied and cleared in start_sysmem();
 * members L4_Fpage_t cast to a pointer.
 */
static struct htable pre_sysmem_resv = HTABLE_INITIALIZER(
	pre_sysmem_resv, &hash_raw_word, NULL);


extern noreturn void panic(const char *msg);


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
void send_phys_to_sysmem(L4_ThreadId_t sm, bool self, L4_Fpage_t pg)
{
	int n = __sysmem_send_phys(sm,
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
	L4_MsgTag_t tag = L4_Send_Timeout(sm, L4_TimePeriod(10 * 1000));
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
		/* not in kansas anymore. */
		goto end;
	}

	/* grab boot modules from sigma0 so they don't get passed to vm. changes
	 * bootmodule start addresses in the copied records to lie within the sbrk
	 * heap.
	 */
	bootinfo = (L4_BootInfo_t *)&bootinfo_copy[0];
	L4_BootRec_t *rec = L4_BootInfo_FirstEntry(bootinfo);
	for(int i = 0, l = L4_BootInfo_Entries(bootinfo); i < l; i++, rec = L4_BootRec_Next(rec)) {
		if(rec->type != L4_BootInfo_Module) {
			printf("%s: rec=%p is not a module\n", __func__, rec);
			continue;
		}

		L4_Word_t base = L4_Module_Start(rec), len = L4_Module_Size(rec);
		size_t copylen = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		void *copy = aligned_alloc(PAGE_SIZE, copylen);
		printf("copy boot module [%#lx, %#lx) to %p:%#lx\n",
			base, base + len, copy, (L4_Word_t)copylen);
		memcpy(copy, (void *)base, len);
		memset(copy + L4_Module_Size(rec), '\0', copylen - len);
		/* record the module load range as pre-sysmem reservation. */
		L4_Word_t addr, sz;
		for_page_range(base, base + copylen, addr, sz) {
			L4_Fpage_t p = L4_FpageLog2(addr, sz);
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
		if(cmdline[0] == '.' && cmdline[1] == '/') cmdline += 2;
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
	*pager_p = tidof_NP(pg);
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

		/* configure pager to map sysmem at 1:1 physical addresses so it can
		 * track physical memory independently.
		 */
		memcpy((void *)ep->p_vaddr, (void *)(start_addr + ep->p_offset),
			ep->p_filesz);
		if(ep->p_filesz < ep->p_memsz) {
			memset((void *)ep->p_vaddr + ep->p_filesz, 0,
				ep->p_memsz - ep->p_filesz);
		}
		add_sysmem_pages(pages, ep->p_vaddr, ep->p_vaddr + ep->p_memsz - 1);
	}

	/* set up the address space & start the main thread. */
	L4_ThreadId_t sm_tid = L4_GlobalId(500, (1 << 2) | 2);
	L4_Word_t res = L4_ThreadControl(sm_tid, sm_tid,
		*pager_p, *pager_p, (void *)-1);
	if(res != 1) {
		fprintf(stderr, "%s: ThreadControl failed, ec %lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	*utcb_area = L4_FpageLog2(0x100000, 14);
	*kip_area = L4_FpageLog2(0xff000, 12);
	L4_Word_t old_ctl;
	res = L4_SpaceControl(sm_tid, 0, *kip_area, *utcb_area,
		L4_anythread, &old_ctl);
	if(res != 1) {
		fprintf(stderr, "%s: SpaceControl failed, ec %lu\n",
			__func__, L4_ErrorCode());
		abort();
	}
	res = L4_ThreadControl(sm_tid, sm_tid, *pager_p,
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
	L4_MsgTag_t tag = L4_Send_Timeout(sm_tid, L4_TimePeriod(50 * 1000));
	if(L4_IpcFailed(tag)) {
		fprintf(stderr, "%s: breath-of-life to forkserv failed: ec=%#lx\n",
			__func__, L4_ErrorCode());
		abort();
	}
	free((void *)start_addr);	/* see get_boot_info() */

	/* send the memory where boot modules were loaded. */
	struct htable_iter it;
	for(void *ptr = htable_first(&pre_sysmem_resv, &it);
		ptr != NULL; ptr = htable_next(&pre_sysmem_resv, &it))
	{
		L4_Fpage_t fp = { .raw = (L4_Word_t)ptr };
		send_phys_to_sysmem(sm_tid, false, fp);
	}
	htable_clear(&pre_sysmem_resv);
	/* and the memory where sysmem's own binary was loaded. */
	for(struct sysmem_page *p = htable_first(pages, &it);
		p != NULL; p = htable_next(pages, &it))
	{
		assert(p->address != 0);
		send_phys_to_sysmem(sm_tid, false, L4_FpageLog2(p->address, PAGE_BITS));
		htable_delval(pages, &it);
		free(p);
	}
	assert(pages->elems == 0);
	htable_clear(pages);
	/* from now, sysmem_pager_fn will only report segfaults for sysmem. */

	return sm_tid;
}


/* fault in memory between _start and _end, switch pagers, and grant it page
 * by page to sysmem. then trigger mm.c heap transition and toss a few pages
 * at sysmem to tide us over until start_vm().
 */
static void move_to_sysmem(L4_ThreadId_t sm_pager, L4_ThreadId_t sm)
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
	L4_Set_Pager(sm);
	L4_Set_PagerOf(sm_pager, sm);
	for(L4_Word_t addr = (L4_Word_t)&_start & ~PAGE_MASK;
		addr < (L4_Word_t)&_end;
		addr += PAGE_SIZE)
	{
		send_phys_to_sysmem(sm, true, L4_FpageLog2(addr, PAGE_BITS));
	}
	mm_enable_sysmem(sm);
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
	pop_interrupt_to(tidof_NP(helper));

	L4_LoadMR(0, (L4_MsgTag_t){ .X.label = 0xf00d }.raw);
	L4_MsgTag_t tag = L4_Send(tidof_NP(helper));
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


/* launches a systask from a boot module accessed by @fh, constructing a
 * parameter page from @name, @args and @rest. returns nilthread on failure.
 */
static L4_ThreadId_t spawn_systask_fv(FILE *fh, const char *name, char **args, va_list rest)
{
	assert(!L4_SameThreads(sigma0_tid, L4_Pager()));

	/* TODO: increase root thread stack size and allocate this on it, rather
	 * than statically. not that it makes a lot of difference here.
	 */
	static uint8_t buf[PAGE_SIZE];
	int n = fread(buf, 1, sizeof buf, fh);
	if(n == 0) {
		fprintf(stderr, "%s: can't read ELF magic?\n", __func__);
		abort();
	}
	const Elf32_Ehdr *ee = (void *)buf;
	if(memcmp(ee->e_ident, ELFMAG, SELFMAG) != 0) {
		fprintf(stderr, "%s: incorrect ELF magic\n", __func__);
		return L4_nilthread;
	}

	/* read and copy all the program headers. also get the lowest address of
	 * any segment.
	 */
	int bufoff = 0, n_phdrs = ee->e_phnum, phentsize = ee->e_phentsize;
	uintptr_t phoff = ee->e_phoff, lowest = ~0ul;
	Elf32_Phdr phdrs[n_phdrs];	/* FIXME: stack oof */
	for(int i=0; i < ee->e_phnum; i++, phoff += phentsize) {
		if(phoff + phentsize > (bufoff + 1) * sizeof buf
			|| phoff < bufoff * sizeof buf)
		{
#if 0
			fprintf(stderr, "%s: page now for %u\n",
				__func__, (unsigned)phoff);
#endif
			n = fseek(fh, (phoff / sizeof buf) * sizeof buf, SEEK_SET);
			if(n != 0) {
				printf("%s: can't seek to page of %u?\n",
					__func__, (unsigned)phoff);
				abort();
			}
			bufoff = phoff / sizeof buf;
			n = fread(buf, 1, sizeof buf, fh);
			if(n != sizeof buf) {
				printf("%s: short read? n=%d\n", __func__, n);
				abort();
			}
		}
		const Elf32_Phdr *ep = (void *)(buf + (phoff - bufoff * sizeof buf));
		memcpy(&phdrs[i], ep, sizeof phdrs[i]);
		if(ep->p_type != PT_LOAD) continue;	/* ignore the GNU stack thing */
		lowest = min_t(uintptr_t, ep->p_vaddr, lowest);
	}
	lowest &= ~PAGE_MASK;
	/* place UTCB and KIP areas below the process image. (this helps with the
	 * "smallspace" ASID emulation technique, which will matter one day.)
	 */
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

	assert(!L4_IsNilThread(uapi_tid));
	L4_ThreadId_t new_tid = create_systask(kip_area, utcb_area);

	/* copy each page to a vmem buffer, then send_virt it over to the new
	 * process at the correct address.
	 */
	void *copybuf = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	for(int i=0; i < n_phdrs; i++) {
		const Elf32_Phdr *ep = &phdrs[i];
		if(ep->p_type != PT_LOAD) continue;	/* skip the GNU stack thing */

		for(size_t off = 0; off < ep->p_memsz; off += PAGE_SIZE) {
			if(off < ep->p_filesz) {
				n = fseek(fh, ep->p_offset + off, SEEK_SET);
				if(n != 0) {
					fprintf(stderr, "%s: can't seek to %u? errno=%d\n", __func__,
						(unsigned)ep->p_offset, errno);
					abort();
				}
				int sz = min_t(int, PAGE_SIZE, ep->p_filesz - off);
				n = fread(copybuf, 1, sz, fh);
				if(n != sz) {
					fprintf(stderr, "%s: EOF? wanted sz=%d, got n=%d\n",
						__func__, sz, n);
					abort();
				}
				if(sz < PAGE_SIZE) {
					/* zero fill */
					memset(copybuf + sz, '\0', PAGE_SIZE - sz);
				}
			} else {
				/* the page should be written even when entirely zero since it
				 * was previously either freshly allocated or snatched away by
				 * Sysmem::send_virt.
				 */
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
	void *argpage = make_argpage(name, args, rest);
	uint16_t ret;
	n = __sysmem_send_virt(sysmem_tid, &ret, (L4_Word_t)argpage,
		new_tid.raw, L4_Address(kip_area) - PAGE_SIZE);
	if(n != 0 || ret != 0) {
		printf("sysmem::send_virt failed on argpage, n=%d, ret=%u\n", n, ret);
		abort();
	}
	free(argpage);

	/* enable get_shape(). */
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

	return new_tid;
}

L4_ThreadId_t spawn_systask(int flags, const char *name, ...)
{
	assert((flags & ~SPAWN_BOOTMOD) == 0);
	FILE *elf;
	char **args, *noargs = NULL, *rest = NULL, *start;
	if(flags & SPAWN_BOOTMOD) {
		L4_BootRec_t *mod = find_boot_module(the_kip, name, &rest);
		if(mod == NULL) return L4_nilthread;
		args = break_argument_list(rest, NULL);
		start = (char *)L4_Module_Start(mod);
		if(elf = fmemopen(start, L4_Module_Size(mod), "r"), elf == NULL) panic("out of memory in spawn_systask()");
	} else {
		if(elf = fopen(name, "rb"), elf == NULL) {
			fprintf(stderr, "%s: fopen `%s': errno=%d\n", __func__, name, errno);
			return L4_nilthread;
		}
		const char *tmp = strrchr(name, '/');
		if(tmp != NULL) name = tmp + 1;
		args = &noargs; start = NULL;
	}
	va_list al; va_start(al, name);
	L4_ThreadId_t tid = spawn_systask_fv(elf, name, args, al);
	va_end(al);
	fclose(elf);
	if(!L4_IsNilThread(tid)) free(start); /* see get_boot_info() */
	free(rest);
	if(args != &noargs) free(args);
	return tid;
}


/* TODO: move this into root/test.c, or to a module where the
 * rootserv-exported spawn_systask_from_initrd() would go.
 */
#ifdef BUILD_SELFTEST

#include <sneks/test.h>
#include <sneks/systask.h>

START_TEST(spawn_systask_from_initrd)
{
	plan(5);

	L4_ThreadId_t task = spawn_systask(0, "/initrd/systest/sys/test/initrd_systask_partner", NULL);
	skip_start(!ok(!L4_IsNilThread(task), "task created"), 4, "no task") {
		L4_Accept(L4_UntypedWordsAcceptor);
		L4_LoadMR(0, 0);
		L4_MsgTag_t tag = L4_Call_Timeouts(task,
			L4_TimePeriod(20000), L4_TimePeriod(20000));
		L4_Word_t mr1; L4_StoreMR(1, &mr1);
		ok(L4_IpcSucceeded(tag), "partner answered");
		ok1(mr1 == 0x87654321);

		tag = L4_Receive_Timeout(task, L4_TimePeriod(20000));
		L4_Word_t ec = L4_ErrorCode();
		ok(L4_IpcFailed(tag), "partner didn't send");
		if(ec == 3) todo_start("something should clean up dead systasks...");
		if(!ok(ec == 5, "error is `non-existing partner in receive phase'")) {
			diag("ec=%lu", ec);
		}
		if(ec == 3) todo_end();
	} skip_end;
}
END_TEST

SYSTASK_SELFTEST("root:elf", spawn_systask_from_initrd);

#endif


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
	printf("got %lu KiB further s0 memory\n", total / 1024);

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
	/* TODO: reduce the static 32M minimum allocation to something much
	 * tinier, like max(total/80, 1M), once sysmem and vm start passing
	 * physical RAM in response to pressure.
	 */
	size_t assn = max_t(size_t, total / 40, 32 * 1024 * 1024);
	int rem = assn - (sysmem_pages + sysmem_self_pages) * PAGE_SIZE;
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

	L4_ThreadId_t mem_tid = spawn_systask(SPAWN_BOOTMOD, "vm", NULL);

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


static int boot_initrd_fn(void *param)
{
	/* three seconds is fine. we're not riding rabbits here. (also QEMU on a
	 * P-M needs a bit of time for vm inits and such.)
	 */
	const L4_Time_t timeout = L4_TimePeriod(3 * 1000 * 1000);
	L4_BootRec_t *img = param;

	/* get introduction, reply with image size.
	 * TODO: should this authenticate? or be more robust?
	 */
	L4_ThreadId_t client;
	L4_MsgTag_t tag = L4_Wait_Timeout(timeout, &client);
	if(L4_IpcFailed(tag) || pidof_NP(client) < SNEKS_MIN_SYSID) {
		printf("%s: introduction message failed, ec=%lu\n", __func__, L4_ErrorCode());
		abort();
	}
	L4_LoadMR(0, (L4_MsgTag_t){ .X.u = 1 }.raw);
	L4_LoadMR(1, L4_Module_Size(img));
	tag = L4_Reply(client);
	if(L4_IpcFailed(tag)) goto ipcfail;

	/* get start address, prepare memory, reply to sync. */
	tag = L4_Receive_Timeout(client, timeout);
	if(L4_IpcFailed(tag) || L4_UntypedWords(tag) < 1) goto ipcfail;
	L4_Word_t start; L4_StoreMR(1, &start);
	uint8_t *copybuf = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	for(size_t off = 0; off < L4_Module_Size(img); off += PAGE_SIZE) {
		size_t left = L4_Module_Size(img) - off;
		memcpy(copybuf, (void *)L4_Module_Start(img) + off, min_t(size_t, left, PAGE_SIZE));
		if(left < PAGE_SIZE) memset(copybuf + left, 0, PAGE_SIZE - left);
		uint16_t ret = 0;
		int n = __sysmem_send_virt(sysmem_tid, &ret, (L4_Word_t)copybuf, client.raw, start + off);
		if(n != 0 || ret != 0) {
			printf("%s: send_virt failed, n=%d, ret=%u\n", __func__, n, ret);
			abort();
		}
	}
	free(copybuf);
	free((void *)L4_Module_Start(img));	/* see get_boot_info() */
	L4_LoadMR(0, 0);
	tag = L4_Reply(client);
	if(L4_IpcFailed(tag)) goto ipcfail;
	return 0;

ipcfail:
	printf("%s: IPC fail; tag=%#lx, ec=%lu\n", __func__, tag.raw, L4_ErrorCode());
	return 1;
}

static void mount_initrd(void)
{
	L4_BootRec_t *img = find_boot_module(L4_GetKernelInterface(), "initrd.img", &(char *){ NULL });
	if(img == NULL || img->type != L4_BootInfo_Module) {
		printf("no initrd.img found (type=%d), skipping\n", img == NULL ? -1 : (int)img->type);
		return;
	}
	thrd_t bit; if(thrd_create(&bit, &boot_initrd_fn, img) != thrd_success) abort();
	assert(L4_IsGlobalId(tidof(bit)));
	char mntdata[40]; snprintf(mntdata, sizeof mntdata, "boot-initrd=%lu:%lu", L4_ThreadNo(tidof(bit)), L4_Version(tidof(bit)));
	int n, status;
	if(n = __ns_mount(rootpath_tid, "", "/", "squashfs!bootmod", 0, mntdata), n != 0) { perror_ipc("failed to mount initrd", n); abort(); }
	if(n = thrd_join(bit, &status), n != thrd_success) abort();
	if(status != 0) abort();
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


/* NOTE: that's a fucky, i.e. error-prone, parameter set right there. consider
 * replacing this with one that takes an array and generates it, and the
 * length value, thru __VA_ARGS__.
 */
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
	struct htable *root_args, L4_ThreadId_t console, int con_fd)
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
		servs[i] = console.raw;
		int tmp, n = __io_dup_to(console, &tmp, con_fd, 1);
		if(n != 0) panic("IO::dup_to of bootcon failed!");
		cookies[i] = tmp;
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
	/* root */
	int n = add_systask(SNEKS_MIN_SYSID,
		L4_FpageLog2((L4_Word_t)the_kip, L4_KipAreaSizeLog2(the_kip)),
		probe_root_utcb_area());
	assert(n < 0 || n == SNEKS_MIN_SYSID);
	if(n < 0) {
		printf("add_systask() for root failed, n=%d\n", n);
		abort();
	}
	/* sysmem */
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

	L4_ThreadId_t wm_tid = spawn_systask(SPAWN_BOOTMOD, waitmod, NULL);
	if(!L4_IsNilThread(wm_tid)) {
		while(wait_until_gone(wm_tid, L4_Never) != 0) { /* spin */ }
	}
}

static noreturn int rootserv_loop(void *parameter)
{
	printf("*** root entering service mode in tid=%lu:%lu\n",
		L4_ThreadNo(L4_Myself()), L4_Version(L4_Myself()));
	static const struct root_serv_vtable vtab = {
		.panic = &rs_panic,
		.long_panic = &rs_long_panic,
	};
	for(;;) {
		L4_Word_t status = _muidl_root_serv_dispatch(&vtab);
		if(status != 0 && !MUIDL_IS_L4_ERROR(status) && selftest_handling(status)) {
			/* test yoself */
		} else if(status == MUIDL_UNKNOWN_LABEL) {
			/* do nothing. */
			L4_MsgTag_t tag = muidl_get_tag();
			printf("rootserv: unknown message label=%#lx, u=%lu, t=%lu\n",
				L4_Label(tag), L4_UntypedWords(tag), L4_TypedWords(tag));
		} else if(status != 0 && !MUIDL_IS_L4_ERROR(status)) {
			printf("rootserv: dispatch status %#lx (last tag %#lx)\n",
				status, muidl_get_tag().raw);
		}
	}
}

static void stupid_sync(thrd_t t) {
	L4_LoadMR(0, 0);
	L4_Send(tidof_NP(t));
}


int main(void)
{
	int n = sneks_setup_console_stdio();
	if(n < 0) panic("console setup failed!");

	random_init(rdtsc());
	printf("hello, world!\n");
	the_kip = L4_GetKernelInterface();
	sigma0_tid = L4_Pager();
	__thrd_init();
	rename_first_threads();
	random_init(rdtsc());

	random_init((uintptr_t)the_kip);
	random_init(sigma0_tid.raw);

	L4_Fpage_t sm_utcb = L4_Nilpage, sm_kip = L4_Nilpage;
	L4_ThreadId_t sm_pager = L4_nilthread,
		sm_tid = start_sysmem(&sm_pager, &sm_utcb, &sm_kip);
	move_to_sysmem(sm_pager, sm_tid);
	struct htable root_args = HTABLE_INITIALIZER(root_args, &hash_arg, NULL);
	parse_initrd_args(&root_args);

	uapi_init();
	int confd;
	L4_ThreadId_t con_tid = start_bootcon(&confd, &root_args);
	random_init(rdtsc());

	/* configure sysinfo. */
	thrd_t kmsg;
	n = thrd_create(&kmsg, &kmsg_impl_fn, NULL);
	if(n != thrd_success) {
		printf("can't start kmsg!\n");
		abort();
	}
	put_sysinfo("kmsg:tid", 1, tidof_NP(kmsg).raw);
	random_init(rdtsc());

	/* launch the userspace API server. */
	configure_uapi(sm_kip, sm_utcb);
	thrd_t uapi;
	n = thrd_create(&uapi, &uapi_loop, NULL);
	if(n != thrd_success) {
		printf("can't start uapi!\n");
		abort();
	}
	/* stupid-sync with uapi so that it's ready to handle sysmem's requests.
	 * without this, sysmem and uapi wind up in a send-send deadlock.
	 */
	stupid_sync(uapi);
	random_init(rdtsc());

	put_sysinfo("uapi:tid", 1, tidof_NP(uapi).raw);
	uapi_tid = tidof_NP(uapi);

	/* start the rootserv thread, for handling of panics and the like. */
	thrd_t rootserv;
	n = thrd_create(&rootserv, &rootserv_loop, NULL);
	if(n != thrd_success) {
		printf("can't start rootserv!\n");
		abort();
	}
	put_sysinfo("rootserv:tid", 1, tidof_NP(rootserv).raw);

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

	printf("sysmem was given %d pages (%d KiB) total\n",
		sysmem_pages + sysmem_self_pages, (sysmem_pages + sysmem_self_pages) * 4);
	printf("... of which %d pages (%d KiB) were root's\n",
		sysmem_self_pages, sysmem_self_pages * 4);
	random_init(rdtsc());

	devices_init();
	thrd_t devices;
	n = thrd_create(&devices, &devices_loop, NULL);
	if(n != thrd_success) {
		printf("can't start device resolver!\n");
		abort();
	}
	stupid_sync(devices);
	generate_u(device_cookie_key.key, sizeof device_cookie_key.key);

	thrd_t rootpath;
	n = thrd_create(&rootpath, &root_path_thread, NULL);
	if(n != thrd_success) {
		printf("can't start RootPath thread!\n");
		abort();
	}
	rootpath_tid = L4_GlobalIdOf(tidof_NP(rootpath));
	put_sysinfo("rootfs:tid", 1, rootpath_tid.raw);
	mount_initrd();

	sysmsg_tid = spawn_systask(0, "/initrd/lib/sneks-0.0p0/sysmsg", NULL);
	put_sysinfo("sys:sysmsg:tid", 1, sysmsg_tid.raw);
	L4_ThreadId_t pipeserv = spawn_systask(0, "/initrd/lib/sneks-0.0p0/pipeserv", NULL);
	put_sysinfo("posix:pipe:tid", 1, pipeserv.raw);

	struct __sysinfo *sip = sip_mem;
	*sip = (struct __sysinfo){
		.magic = SNEKS_SYSINFO_MAGIC,
		.sysinfo_size_log2 = PAGE_BITS,
		.api.proc = uapi_tid,
		.api.vm = vm_tid,
		.api.rootfs = rootpath_tid,
		.memory.page_size_log2 = PAGE_BITS,
		.memory.biggest_page_log2 = PAGE_BITS,
		.posix.pipe = pipeserv,
	};

	/* launch init.
	 * TODO: when init exits, reboot or shutdown according to its exit code
	 * (or some such).
	 */
	uint16_t init_pid;
	n = launch_init(&init_pid, &root_args, con_tid, confd);
	if(n != 0) {
		printf("FAIL: can't launch init, n=%d\n", n);
		panic("this means your system is heavily broken!");
	} else if(init_pid != 1) {
		panic("init's pid isn't 1? what in tarnation");
	}

	run_waitmods(&root_args, "late");

	for(;;) {	/* inhibit the silly english kaniggot */
		L4_Sleep(L4_Never);
	}
	assert(false);
}
